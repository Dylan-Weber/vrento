module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
module vrento;
import :frame_graph;
import :pass.resources;
import rstd.test;

using namespace rstd::prelude;
using namespace rstd::literals;

using namespace vrento::fg;

namespace vrento_test::frame_graph
{
auto texture(Serial id) -> ResourceDescription {
    return { .id = { id, 1 }, .allocation_class = 1 };
}
auto slot(const FrameGraph& graph, ResourceVersion version) -> Index {
    for (auto allocation : graph.inspect().allocations)
        if (allocation.version == version) return allocation.slot;
    return invalid_index;
}
TEST(FrameGraph, VersionsAndOwnership) {
    FrameGraphBuilder graph(11);
    auto              resource = graph.create_resource(texture(1)).unwrap();
    auto              first    = graph.add_pass({ 1 }).unwrap();
    auto              written  = graph.write(first, resource).unwrap();
    auto              reader   = graph.add_pass({ 2 }, { 1 }).unwrap();
    graph.read(reader, written).unwrap();
    for (unsigned i = 0; i < 128; ++i) graph.add_pass({ i + 3 }).unwrap();
    rstd_assert(graph.read(reader, { { 1, 2 }, written.version }).unwrap_err().kind ==
                ErrorKind::MissingResource);
    rstd_assert(graph.add_pass({ 1 }).unwrap_err().kind == ErrorKind::Duplicate);
    auto frozen = rstd::move(graph).finish().unwrap();
    rstd_assert(frozen.inspect().order[usize()] == 0 && frozen.inspect().order[usize(1)] == 1);
    rstd_assert(frozen.inspect().batches[usize(1)].waits[usize()] == 0);
    auto retained = FrameGraph(frozen);
    auto changed  = frozen.clone_data();
    changed.passes.clear();
    rstd_assert(retained.passes().len() == usize(130));
    rstd_assert(frozen.compile({ .uses = 1 }).unwrap_err().kind == ErrorKind::Limit);
}
TEST(FrameGraph, SlotsAndLastWrites) {
    FrameGraphBuilder graph(1);
    auto              value  = graph.create_resource(texture(1)).unwrap();
    auto              first  = graph.add_pass({ 1 }).unwrap();
    auto              a      = graph.write(first, value).unwrap();
    auto              second = graph.add_pass({ 2 }).unwrap();
    graph.read(second, a).unwrap();
    auto b     = graph.write(second, a).unwrap();
    auto third = graph.add_pass({ 3 }).unwrap();
    graph.read(third, b).unwrap();
    auto c      = graph.write(third, b).unwrap();
    auto frozen = rstd::move(graph).finish().unwrap();
    rstd_assert(slot(frozen, a) != slot(frozen, b) && slot(frozen, a) == slot(frozen, c));
    for (const auto& life : frozen.inspect().lifetimes)
        if (life.version == c)
            rstd_assert(life.first == 2 && life.last == 2 && life.uses.len() == usize(1));
}
TEST(FrameGraph, OverwriteAndConcurrentVersions) {
    FrameGraphBuilder graph(1);
    auto              value       = graph.create_resource(texture(1)).unwrap();
    auto              a           = graph.write(graph.add_pass({ 1 }).unwrap(), value).unwrap();
    auto              b           = graph.write(graph.add_pass({ 2 }).unwrap(), a).unwrap();
    auto              c           = graph.write(graph.add_pass({ 3 }).unwrap(), b).unwrap();
    auto              overwritten = rstd::move(graph).finish().unwrap();
    rstd_assert(slot(overwritten, a) == slot(overwritten, b) &&
                slot(overwritten, b) == slot(overwritten, c));
    FrameGraphBuilder concurrent(2);
    value     = concurrent.create_resource(texture(1)).unwrap();
    a         = concurrent.write(concurrent.add_pass({ 1 }).unwrap(), value).unwrap();
    auto next = concurrent.add_pass({ 2 }).unwrap();
    concurrent.read(next, a).unwrap();
    b    = concurrent.write(next, a).unwrap();
    next = concurrent.add_pass({ 3 }).unwrap();
    concurrent.read(next, a).unwrap();
    concurrent.read(next, b).unwrap();
    c         = concurrent.write(next, b).unwrap();
    auto live = rstd::move(concurrent).finish().unwrap();
    rstd_assert(slot(live, a) != slot(live, b) && slot(live, a) != slot(live, c) &&
                slot(live, b) != slot(live, c));
}
TEST(FrameGraph, InPlaceAndHistory) {
    FrameGraphBuilder graph(1);
    auto              history = graph.import_resource(texture(1)).unwrap();
    auto              consume = graph.add_pass({ 1 }, { 1 }).unwrap();
    graph.read(consume, history).unwrap();
    auto update = graph.add_pass({ 2 }, { 2 }).unwrap();
    auto output = graph.write(update, history, None(), true).unwrap();
    graph.depends_on(update, consume).unwrap();
    graph.export_resource(output).unwrap();
    auto frozen = rstd::move(graph).finish().unwrap();
    rstd_assert(slot(frozen, history) == slot(frozen, output));
    rstd_assert(frozen.inspect().precedes(0, 1));
    rstd_assert(frozen.inspect().dependencies.len() == usize(1));
    rstd_assert(frozen.inspect().dependencies[usize()].sources ==
                (Index(DependencySource::Explicit) | Index(DependencySource::Storage)));
    rstd_assert(frozen.inspect().lifetimes[usize()].retained);
    auto retry = FrameGraphBuilder(frozen.clone_data());
    retry.depends_on(consume, update).unwrap();
    rstd_assert(rstd::move(retry).finish().unwrap_err().kind == ErrorKind::Cycle);
}
TEST(FrameGraph, ParallelQueuesCannotAlias) {
    FrameGraphBuilder graph(1);
    auto              a      = graph.create_resource(texture(1)).unwrap();
    auto              b      = graph.create_resource(texture(2)).unwrap();
    auto              first  = graph.add_pass({ 1 }, { 1 }).unwrap(),
                      second = graph.add_pass({ 2 }, { 2 }).unwrap();
    a                        = graph.write(first, a).unwrap();
    b                        = graph.write(second, b).unwrap();
    auto parallel            = rstd::move(graph).finish().unwrap();
    rstd_assert(slot(parallel, a) != slot(parallel, b));
    auto serial = FrameGraphBuilder(parallel.clone_data());
    serial.depends_on(second, first).unwrap();
    auto ordered = rstd::move(serial).finish().unwrap();
    rstd_assert(slot(ordered, a) == slot(ordered, b));
}
TEST(FrameGraph, PartialContentsAndRanges) {
    FrameGraphBuilder graph(1);
    auto              resource =
        graph.create_resource({ .id = { 1, 1 }, .kind = ResourceKind::Buffer, .bytes = 64 })
            .unwrap();
    auto first  = graph.add_pass({ 1 }).unwrap();
    auto left   = graph.write(first, resource, Some(ResourceRange { .size = 32 })).unwrap();
    auto second = graph.add_pass({ 2 }).unwrap();
    auto complete =
        graph.write(second, left, Some(ResourceRange { .offset = 32, .size = 32 }), true, true)
            .unwrap();
    auto read = graph.add_pass({ 3 }).unwrap();
    graph.read(read, complete).unwrap();
    auto frozen = rstd::move(graph).finish().unwrap();
    rstd_assert(frozen.inspect().precedes(0, 1) && frozen.inspect().precedes(1, 2));
    auto missing                        = frozen.clone_data();
    missing.versions[usize(2)].inherits = false;
    rstd_assert(FrameGraph::create(rstd::move(missing)).unwrap_err().kind == ErrorKind::Undefined);
    auto invalid                                     = frozen.clone_data();
    invalid.passes[usize()].uses[usize()].range.size = 65;
    rstd_assert(FrameGraph::create(rstd::move(invalid)).unwrap_err().kind == ErrorKind::Range);
}
TEST(FrameGraph, DiscardAndUndefinedExports) {
    FrameGraphBuilder graph(1);
    auto              resource = graph.create_resource(texture(1)).unwrap();
    auto              first    = graph.add_pass({ 1 }).unwrap();
    auto discarded             = graph.write(first, resource, None(), false, false, false).unwrap();
    graph.export_resource(discarded).unwrap();
    rstd_assert(rstd::move(graph).finish().unwrap_err().kind == ErrorKind::Undefined);
    FrameGraphBuilder empty(2);
    resource = empty.create_resource(texture(1)).unwrap();
    empty.read(empty.add_pass({ 1 }).unwrap(), resource).unwrap();
    rstd_assert(rstd::move(empty).finish().unwrap_err().kind == ErrorKind::Undefined);
}
TEST(FrameGraph, Serialization) {
    FrameGraphBuilder graph(17);
    auto              resource = graph.import_resource(texture(1)).unwrap();
    auto              pass     = graph.add_pass({ 8 }, { 3 }).unwrap();
    graph.read(pass, resource).unwrap();
    auto result = graph.write(pass, resource, None(), true).unwrap();
    graph.export_resource(result).unwrap();
    auto original  = rstd::move(graph).finish().unwrap();
    auto encoded   = encode_frame_graph(original).unwrap();
    auto decoded   = decode_frame_graph(encoded.as_slice()).unwrap();
    auto reencoded = encode_frame_graph(decoded).unwrap();
    rstd_assert(encoded == reencoded && decoded.scope() == 17);
    rstd_assert(decoded.inspect().order == original.inspect().order);
    for (usize length {}; length < encoded.len(); ++length)
        rstd_assert(decode_frame_graph(slice<rstd::uint8_t>::from_raw_parts(encoded.data(), length))
                        .is_err());
    encoded[usize(4)] = 2;
    rstd_assert(decode_frame_graph(encoded.as_slice()).unwrap_err().kind == ErrorKind::Schema);
    rstd_assert(encode_frame_graph(original, { .encoded_bytes = 1 }).unwrap_err().kind ==
                ErrorKind::Limit);
}
TEST(FrameGraph, ExplicitAliasesRequireOrder) {
    FrameGraphBuilder graph(1);
    auto              first_description = texture(1);
    first_description.storage           = 77;
    auto second_description             = texture(2);
    second_description.storage          = 77;
    auto first  = graph.import_resource(rstd::move(first_description)).unwrap();
    auto second = graph.import_resource(rstd::move(second_description)).unwrap();
    auto reader = graph.add_pass({ 1 }, { 1 }).unwrap();
    graph.read(reader, first).unwrap();
    auto writer = graph.add_pass({ 2 }, { 2 }).unwrap();
    graph.write(writer, second, None(), true).unwrap();
    auto unordered = FrameGraph::create(graph.data().clone());
    rstd_assert(unordered.unwrap_err().kind == ErrorKind::Conflict);
    graph.depends_on(writer, reader).unwrap();
    rstd_assert(rstd::move(graph).finish().is_ok());
}
TEST(FrameGraph, TextureRangeCoverage) {
    FrameGraphBuilder graph(1);
    auto              description = texture(1);
    description.mip_levels        = 2;
    description.layers            = 2;
    auto source                   = graph.create_resource(rstd::move(description)).unwrap();
    auto first                    = graph.add_pass({ 1 }).unwrap();
    auto lower  = graph.write(first, source, Some(ResourceRange { .layer_count = 2 })).unwrap();
    auto second = graph.add_pass({ 2 }).unwrap();
    auto full =
        graph
            .write(
                second, lower, Some(ResourceRange { .first_mip = 1, .layer_count = 2 }), true, true)
            .unwrap();
    graph.read(graph.add_pass({ 3 }).unwrap(), full).unwrap();
    auto completed                                          = rstd::move(graph).finish().unwrap();
    auto broken                                             = completed.clone_data();
    broken.passes[usize(1)].uses[usize()].range.layer_count = 1;
    rstd_assert(FrameGraph::create(rstd::move(broken)).unwrap_err().kind == ErrorKind::Undefined);
}
TEST(FrameGraph, CachedScheduleAndDiagnostics) {
    constexpr unsigned samples = 32, nodes = 96;
    auto               started = rstd::time::Instant::now();
    for (unsigned run {}; run < samples; ++run) {
        FrameGraphBuilder       graph(run + 1);
        Option<ResourceVersion> previous;
        for (unsigned i {}; i < nodes; ++i) {
            auto resource = graph.create_resource({ .id = { i + 1, 1 } }).unwrap();
            auto pass     = graph.add_pass({ i + 1 }).unwrap();
            if (previous) graph.read(pass, *previous).unwrap();
            previous = Some(graph.write(pass, resource).unwrap());
        }
        auto compiled = rstd::move(graph).finish().unwrap();
        auto first = compiled.compile().unwrap(), second = compiled.compile().unwrap();
        rstd_assert(first.order == second.order && first.order.len() == usize(nodes));
        rstd_assert(first.dependencies.len() == usize(nodes - 1));
        for (auto dependency : first.dependencies)
            rstd_assert(dependency.sources ==
                        (Index(DependencySource::Contents) | Index(DependencySource::Queue)));
        rstd_assert(compiled.compile({ .dependencies = nodes - 2 }).unwrap_err().kind ==
                    ErrorKind::Limit);
        if (! run) {
            auto dot = graphviz(compiled);
            rstd_assert(dot->contains("slot="_str) && dot->contains("order"_str) &&
                        dot->contains("first="_str));
        }
    }
    rstd::io::println("frame graph: {} samples, {} passes, build + two cached schedules: {:.3} ms",
                      samples,
                      nodes,
                      started.elapsed().as_secs_f64() * 1000);
}
TEST(FrameGraph, UnicodeDiagnostics) {
    FrameGraphBuilder graph(1);
    graph.add_pass({ 1 }, {}, String::make("文字\"\n"_str)).unwrap();
    auto value = rstd::move(graph).finish().unwrap();
    auto dot   = graphviz(value);
    rstd_assert(dot->contains("文字\\\"\\n"_str));
    auto restored = decode_frame_graph(encode_frame_graph(value).unwrap().as_slice()).unwrap();
    rstd_assert(restored.passes()[usize()].name == "文字\"\n"_str);
}

} // namespace vrento_test::frame_graph

namespace vrento_test::frame_graph
{
TEST(FrameGraph, AnalysisCountersExcludeCachedSchedules) {
    AnalysisStatistics statistics;
    FrameGraphBuilder  builder(411);
    builder.add_pass({ 1 }).unwrap();
    auto graph = rstd::move(builder).finish({}, &statistics).unwrap();
    EXPECT_EQ(statistics.attempts, 1u);
    EXPECT_EQ(statistics.failures, 0u);
    graph.compile().unwrap();
    graph.compile().unwrap();
    EXPECT_EQ(statistics.attempts, 1u);
    auto data = graph.clone_data();
    data.passes.push(PassNode { .id = { 1 } });
    EXPECT_TRUE(FrameGraph::create(rstd::move(data), {}, &statistics).is_err());
    EXPECT_EQ(statistics.attempts, 2u);
    EXPECT_EQ(statistics.failures, 1u);
}
} // namespace vrento_test::frame_graph

namespace vrento_test::frame_graph
{
TEST(FrameGraphResources, BuildsTextureAndBufferMappingWithoutDevice) {
    namespace resource = vrento::resource;
    vrento::pass::GraphResources resources(u64(91));
    FrameGraphBuilder            builder(91);
    resource::TextureRequest     request { .kind       = resource::TextureRequestKind::RenderTarget,
                                           .name       = rstd::format("target"),
                                           .definition = Some(resource::TextureDefinition {
                                               .width = i32(8), .height = i32(8) }) };
    auto                         initial =
        resources
            .declare(builder, { .key = rstd::format("target"), .definition = rstd::move(request) })
            .unwrap();
    auto                      writer  = builder.add_pass({ 1 }).unwrap();
    auto                      written = builder.write(writer, initial).unwrap();
    auto                      use     = resources.handle(written).unwrap();
    resource::ResourcePlan    plan { .generation = u64(91) };
    resource::BufferUseHandle buffer { u64(), u64(91) };
    plan.buffers.push(
        { .handle     = buffer,
          .definition = vrento::resource::BufferRequest { .name       = rstd::format("vertex"),
                                                          .definition = { .size = usize(64) } } });
    resources.declare_buffers(builder, plan.buffers.as_slice()).unwrap();
    auto buffer_version = resources.version(buffer).unwrap();
    builder.read(writer, buffer_version).unwrap();
    auto graph            = rstd::move(builder).finish().unwrap();
    auto prepared_plan    = resources.resourcePlan(graph).unwrap();
    prepared_plan.buffers = rstd::move(plan.buffers);
    auto mapping          = resources.bindings(graph, prepared_plan).unwrap();
    EXPECT_EQ(mapping.scope, 91u);
    ASSERT_EQ(mapping.textures.len(), usize(1));
    EXPECT_EQ(mapping.textures[usize()].resource.handle, use);
    EXPECT_EQ(mapping.textures[usize()].resource.version, written);
    ASSERT_EQ(mapping.buffers.len(), usize(1));
    EXPECT_EQ(mapping.buffers[usize()].resource.use, buffer);
    EXPECT_EQ(mapping.buffers[usize()].range.size, 64u);
    EXPECT_NE(buffer_version.resource, initial.resource);
    ASSERT_EQ(prepared_plan.textures.len(), usize(1));
    EXPECT_EQ(prepared_plan.textures[usize()].handle, use);
    FrameGraphBuilder other(92);
    EXPECT_TRUE(resources.bindings(rstd::move(other).finish().unwrap(), prepared_plan).is_err());
}
} // namespace vrento_test::frame_graph

namespace vrento_test::frame_graph
{
TEST(FrameGraphResources, StableIdentityKeepsGraphPolicyOutsideDefinition) {
    vrento::resource::TextureResourceRef definition(vrento::resource::TextureRequest {
        .kind = vrento::resource::TextureRequestKind::RenderTarget,
        .name = "history"_Str,
        .definition =
            Some(vrento::resource::TextureDefinition { .width = i32(8), .height = i32(8) }),
        .lifetime = vrento::resource::TextureLifetimeClass::FrameLocal });
    Option<vrento::pass::ContentRef>     previous;
    for (Serial scope : array<Serial, 2> { Serial(101), Serial(102) }) {
        FrameGraphBuilder            graph(scope);
        vrento::pass::GraphResources resources { u64(scope) };
        auto                         input =
            resources.initial(graph, { .key = "history"_Str, .definition = definition.clone() })
                .unwrap();
        resources.import_history(graph, input).unwrap();
        auto pass   = graph.add_pass({ 1 }).unwrap();
        auto output = resources.output(graph, pass, input).unwrap();
        graph.write_range(pass, output.version()).unwrap();
        resources.handle(output).unwrap();
        if (previous) EXPECT_TRUE(previous->resolve(graph).is_err());
        previous      = Some(output.clone());
        auto compiled = rstd::move(graph).finish().unwrap();
        auto plan     = resources.resourcePlan(compiled).unwrap();
        for (const auto& entry : plan.textures) {
            EXPECT_EQ(entry.definition.id(), definition.id());
            EXPECT_EQ(entry.definition.revision(), u64(1));
            EXPECT_EQ(entry.request().lifetime, vrento::resource::TextureLifetimeClass::FrameLocal);
            EXPECT_TRUE(entry.preserve_contents);
        }
    }
}

TEST(FrameGraphResources, TransparentBootstrapRequiresExplicitOwnedColorFeedback) {
    namespace resource = vrento::resource;
    struct Case {
        bool enabled, read_initial, write, imported, depth;
        bool expected;
    };
    for (auto test : array<Case, 6> { Case { false, true, true, false, false, false },
                                      Case { true, true, true, false, false, true },
                                      Case { true, true, false, false, false, false },
                                      Case { true, false, true, false, false, false },
                                      Case { true, true, true, true, false, false },
                                      Case { true, true, true, false, true, false } }) {
        FrameGraphBuilder            graph(103);
        vrento::pass::GraphResources resources(u64(103));
        resource::TextureResourceRef definition(resource::TextureRequest {
            .kind       = test.imported ? resource::TextureRequestKind::Imported
                                        : resource::TextureRequestKind::RenderTarget,
            .name       = "feedback"_Str,
            .definition = Some(resource::TextureDefinition {
                .width  = i32(8),
                .height = i32(8),
                .usage  = test.depth ? resource::TextureUsage::DepthAttachment
                                     : resource::TextureUsage::Color }),
            .lifetime   = resource::TextureLifetimeClass::FrameLocal });
        vrento::pass::TextureSpec    spec { .key                            = "feedback"_Str,
                                            .imported                       = test.imported,
                                            .definition                     = definition.clone(),
                                            .allocation_family              = Some("temporary"_Str),
                                            .initialize_history_transparent = test.enabled };
        auto                         initial = resources.initial(graph, spec.clone()).unwrap();
        if (test.read_initial) {
            resources.import_history(graph, initial).unwrap();
            auto reader = graph.add_pass({ 1 }).unwrap();
            graph.read(reader, initial.version()).unwrap();
            resources.handle(initial).unwrap();
        }
        if (test.write) {
            auto writer  = graph.add_pass({ 2 }).unwrap();
            auto written = graph.write(writer, initial.version()).unwrap();
            resources.handle(written).unwrap();
            auto reader = graph.add_pass({ 3 }).unwrap();
            graph.read(reader, written).unwrap();
        }
        auto built  = rstd::move(graph).finish().unwrap();
        auto plan   = resources.resourcePlan(built).unwrap();
        auto copied = plan.clone();
        ASSERT_FALSE(plan.textures.is_empty());
        for (usize index {}; index < plan.textures.len(); ++index) {
            const auto& entry = plan.textures[index];
            EXPECT_EQ(entry.initialize_transparent, test.expected);
            EXPECT_EQ(copied.textures[index].initialize_transparent, test.expected);
            EXPECT_EQ(entry.definition.id(), definition.id());
            EXPECT_EQ(entry.definition.revision(), definition.revision());
            EXPECT_TRUE(resource::SameTextureRequest(entry.request(), definition.definition()));
            if (test.expected) {
                EXPECT_TRUE(entry.preserve_contents);
                EXPECT_TRUE(entry.allocation_key.is_empty());
            }
        }
        auto bindings = resources.bindings(built, plan).unwrap();
        for (const auto& binding : bindings.textures)
            EXPECT_EQ(binding.content.texture()->revision(), definition.revision());
    }
}
} // namespace vrento_test::frame_graph
