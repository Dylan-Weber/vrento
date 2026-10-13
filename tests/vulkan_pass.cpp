module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
module vrento;
import :vulkan.renderer;
import :vulkan.passes.graphics;
import rstd.test;

import vrento.tests.pass_set;

using namespace rstd::prelude;

using namespace vrento::vulkan;

namespace vrento_test::vulkan_pass
{
TEST(VulkanPassExecution, AttachmentLoadsAndStages) {
    VulkanPassData data { .scope = 1 };
    data.resources.push({ .id = { 1, 1 } });
    PassExecutionInfo pass { .id = { 1 }, .kind = 1 };
    pass.uses.push(
        { { 1, 1 }, 1, GraphAccess::Write, PassUsage::ColorAttachment, PassStages::Color });
    pass.graphics = Some(GraphicsDescription {
        .color = Some(GraphAttachment { { 1, 1 }, 1, AttachmentLoad::Load }) });
    data.passes.push(rstd::move(pass));
    auto invalid = data.passes[usize()].clone();
    rstd_assert(validate_pass_execution(invalid, data.resources.as_slice()).unwrap_err().kind ==
                GraphErrorKind::Conflict);
    data.passes[usize()].graphics->color->load = AttachmentLoad::Clear;
    auto valid                   = vrento_test::make_pass_set(rstd::move(data)).unwrap();
    invalid                      = valid.passes()[usize()].clone();
    invalid.uses[usize()].stages = PassStages::Vertex;
    rstd_assert(validate_pass_execution(invalid, valid.resources()).unwrap_err().kind ==
                GraphErrorKind::Conflict);
}
TEST(VulkanPassExecution, LogicalQueueRequirements) {
    VulkanPassData data { .scope = 1 };
    data.passes.push({ .id = { 1 }, .kind = 1, .queue = { 2, QueueClass::Transfer } });
    data.passes.push({ .id = { 2 }, .kind = 1, .queue = { 2, QueueClass::Graphics } });
    rstd_assert(vrento_test::make_pass_set(rstd::move(data)).unwrap_err().kind ==
                GraphErrorKind::Conflict);
    VulkanPassData buffer { .scope = 2 };
    buffer.resources.push(
        { .id = { 1, 1 }, .kind = GraphResourceKind::Buffer, .bytes = 64, .imported = true });
    PassExecutionInfo read { .id = { 1 }, .kind = 1, .queue = { 1, QueueClass::Transfer } };
    read.uses.push(
        { { 1, 1 }, 0, GraphAccess::Read, PassUsage::Uniform, PassStages::Vertex, { .size = 64 } });
    buffer.passes.push(rstd::move(read));
    rstd_assert(vrento_test::make_pass_set(rstd::move(buffer)).unwrap_err().kind ==
                GraphErrorKind::Conflict);
}
TEST(VulkanPassExecution, DiscardedOutputIsUnavailable) {
    VulkanPassData data { .scope = 1 };
    data.resources.push({ .id = { 1, 1 } });
    PassExecutionInfo clear { .id = { 1 }, .kind = 1 };
    clear.uses.push(
        { { 1, 1 }, 1, GraphAccess::Write, PassUsage::ColorAttachment, PassStages::Color });
    clear.graphics = Some(GraphicsDescription {
        .color = Some(
            GraphAttachment { { 1, 1 }, 1, AttachmentLoad::Clear, AttachmentStore::Discard }) });
    data.passes.push(rstd::move(clear));
    data.passes.push(texture_sampled_pass({ 2 }, { 1, 1 }, 1));
    rstd_assert(vrento_test::make_pass_set(rstd::move(data)).unwrap_err().kind ==
                GraphErrorKind::Undefined);
}
TEST(VulkanPassExecution, AssociationPreservesOriginalDependencies) {
    vrento::fg::FrameGraphBuilder builder(91);
    auto a = builder.add_pass({ 1 }).unwrap(), b = builder.add_pass({ 2 }).unwrap(),
         c = builder.add_pass({ 3 }).unwrap();
    builder.depends_on(b, a).unwrap();
    builder.depends_on(c, b).unwrap();
    auto                   graph = rstd::move(builder).finish().unwrap();
    Vec<PassExecutionInfo> descriptions;
    for (auto id : array<PassId, 3> { c, a, b }) descriptions.push({ .id = id, .kind = 1 });
    auto result = VulkanPassSet::associate(graph, rstd::move(descriptions)).unwrap();
    EXPECT_EQ(vrento::fg::encode_frame_graph(graph).unwrap(),
              vrento::fg::encode_frame_graph(result.frame_graph()).unwrap());
}
TEST(VulkanPassExecution, AssociationRejectsIncorrectUseIds) {
    vrento::fg::FrameGraphBuilder builder(92);
    auto                          source = builder.import_resource({ .id = { 1, 92 } }).unwrap();
    auto                          target = builder.create_resource({ .id = { 2, 92 } }).unwrap();
    auto                          pass   = builder.add_pass({ 1 }, { 1 }).unwrap();
    auto                          output = builder.reserve_write(pass, target).unwrap();
    auto declaration = texture_copy_pass(pass, source.resource, 0, output.resource, output.version);
    vrento_test::declare_pass(builder, declaration).unwrap();
    auto graph                            = rstd::move(builder).finish().unwrap();
    declaration.uses[usize()].logical_use = 999;
    Vec<PassExecutionInfo> descriptions;
    descriptions.push(rstd::move(declaration));
    EXPECT_EQ(VulkanPassSet::associate(graph, rstd::move(descriptions)).unwrap_err().kind,
              GraphErrorKind::Conflict);
}
TEST(VulkanPassExecution, VersionStorageConflicts) {
    for (unsigned mode {}; mode < 3; ++mode) {
        vrento::fg::FrameGraphBuilder builder(94);
        auto                          source =
            builder.import_resource({ .id = { 1, 94 }, .storage = mode == 2 ? 1u : 0u }).unwrap();
        auto              pass   = builder.add_pass({ 1 }).unwrap();
        auto              output = builder.reserve_write(pass, source, mode == 1).unwrap();
        PassExecutionInfo declaration { .id = pass, .kind = 1 };
        declaration.uses.push({ source.resource,
                                source.version,
                                GraphAccess::Read,
                                PassUsage::Sampled,
                                PassStages::Fragment });
        declaration.uses.push({ output.resource,
                                output.version,
                                GraphAccess::Write,
                                PassUsage::ColorAttachment,
                                PassStages::Color });
        declaration.graphics = Some(GraphicsDescription {
            .color = Some(
                GraphAttachment { output.resource, output.version, AttachmentLoad::Discard }) });
        vrento_test::declare_pass(builder, declaration).unwrap();
        auto        graph       = rstd::move(builder).finish().unwrap();
        const auto& allocations = graph.inspect().allocations;
        ASSERT_EQ(allocations.len(), usize(2));
        EXPECT_EQ(allocations[usize()].slot == allocations[usize(1)].slot, mode != 0);
        Vec<PassExecutionInfo> descriptions;
        descriptions.push(rstd::move(declaration));
        auto associated = VulkanPassSet::associate(graph, rstd::move(descriptions));
        if (mode == 0) {
            EXPECT_TRUE(associated.is_ok());
        } else {
            ASSERT_TRUE(associated.is_err());
            EXPECT_EQ(associated.unwrap_err().kind, GraphErrorKind::Conflict);
            EXPECT_EQ(associated.unwrap_err().pass, pass);
            EXPECT_EQ(associated.unwrap_err().resource, source.resource);
        }
    }
}
TEST(VulkanPassExecution, ReusesOriginalGraphForRepeatedAssociation) {
    constexpr unsigned            nodes = 96, frames = 32;
    vrento::fg::FrameGraphBuilder builder(93);
    for (unsigned i {}; i < nodes; ++i) {
        auto id = builder.add_pass({ i + 1 }).unwrap();
        if (i) builder.depends_on(id, { i }).unwrap();
    }
    auto graph   = rstd::move(builder).finish().unwrap();
    auto started = rstd::time::Instant::now();
    for (unsigned frame {}; frame < frames; ++frame) {
        Vec<PassExecutionInfo> descriptions;
        for (unsigned i {}; i < nodes; ++i) descriptions.push({ .id = { i + 1 }, .kind = 1 });
        auto result = VulkanPassSet::associate(graph, rstd::move(descriptions)).unwrap();
        EXPECT_EQ(result.frame_graph().inspect().order.len(), usize(nodes));
    }
    rstd::io::println("Vulkan association: {} frames, {} passes, {} ms",
                      frames,
                      nodes,
                      started.elapsed().as_secs_f64() * 1000);
}
TEST(VulkanPassExecution, GraphicsRecipeOwnsLayoutWithoutDevice) {
    GraphicsDrawDesc          recipe;
    PipelineLayoutRequirement layout { .pipeline = { rstd::u64(1), rstd::u64(9) } };
    layout.descriptor_sets.push({ rstd::u32(1), false, {} });
    layout.descriptor_sets[usize()].bindings.push({ rstd::u32(3),
                                                    rstd::u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                    rstd::u32(1),
                                                    rstd::u32(VK_SHADER_STAGE_VERTEX_BIT) });
    recipe.layout = Some(rstd::move(layout));
    auto copied   = recipe.clone();
    recipe.layout->descriptor_sets.clear();
    ASSERT_TRUE(copied.layout);
    ASSERT_EQ(copied.layout->descriptor_sets.len(), usize(1));
    EXPECT_EQ(copied.layout->descriptor_sets[usize()].bindings[usize()].binding, rstd::u32(3));
}
} // namespace vrento_test::vulkan_pass
