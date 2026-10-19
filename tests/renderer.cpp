#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
import vrento.tests.shaders;
import vrento.tests.pass_set;

using namespace rstd::prelude;
using rstd::sync::Arc;
using namespace rstd::literals;

namespace
{
template<class T>
auto make_test_pass(rstd::uint64_t scope, rstd::uint64_t revision,
                    vrento::vulkan::PassExecutionInfo execution, T desc)
    -> vrento::vulkan::PassInput {
    auto id = execution.id;
    if constexpr (requires { desc.resources; }) {
        namespace vk = vrento::vulkan;
        if (execution.graphics) {
            auto convert = [scope](const auto& value) -> rstd::Option<vk::GraphicsAttachment> {
                if (! value) return rstd::None();
                return rstd::Some(
                    vk::GraphicsAttachment { .target = vrento::pass::ContentRef::from_version(
                                                 scope, { value->resource, value->version }),
                                             .load    = value->load,
                                             .store   = value->store,
                                             .clear   = value->clear,
                                             .stencil = value->stencil });
            };
            desc.resources.attachments.color   = convert(execution.graphics->color);
            desc.resources.attachments.depth   = convert(execution.graphics->depth);
            desc.resources.attachments.resolve = convert(execution.graphics->resolve);
        }
        for (const auto& use : execution.uses) {
            if (use.access != vk::GraphAccess::Read) continue;
            if (use.usage == vk::PassUsage::Sampled)
                desc.resources.textures.push(
                    { vrento::pass::ContentRef::from_version(scope, { use.resource, use.version }),
                      use.range,
                      use.stages });
            else if (use.usage != vk::PassUsage::ColorAttachment &&
                     use.usage != vk::PassUsage::DepthAttachment)
                desc.resources.buffers.push(
                    { vrento::pass::ContentRef::from_version(scope, { use.resource, use.version }),
                      use.range,
                      use.usage,
                      use.stages });
        }
        if (! execution.graphics)
            desc.resources.attachments.color = rstd::Some(vk::GraphicsAttachment {
                .target = vrento::pass::ContentRef::from_version(scope, { { 1, scope }, 1 }) });
    } else
        desc.io = vrento_test::test_io(scope, rstd::move(execution));
    return vrento::vulkan::PassInput::publish(scope, revision, id, rstd::move(desc)).unwrap();
}
} // namespace

namespace vrento_test::texture_vulkan
{
auto read(vrento::vulkan::TextureContext&, vrento::texture::TextureViewRef, unsigned, unsigned)
    -> rstd::vec::Vec<rstd::u8>;
}

namespace vrento_test::renderer
{
namespace vk = vrento::vulkan;
namespace fg = vrento::fg;
struct State {
    unsigned compiled {}, instantiated {}, recorded {}, pending { 1 };
    bool     fail {}, frame_fail {};
    unsigned frame_pending {}, observed {};
};
struct Configuration {
    using PassDescriptor = vrento::pass::PassDesc;
    vk::VulkanPassIO                  io;
    Arc<State>                        state;
    Arc<array<VkViewport, 1>>         viewports;
    VkPipelineViewportStateCreateInfo viewport;
    explicit Configuration(Arc<State> owner)
        : state(rstd::move(owner)),
          viewports(Arc<array<VkViewport, 1>>::make()),
          viewport { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO } {
        viewport.viewportCount = 1;
        viewport.pViewports    = viewports->data();
    }
};
struct UnregisteredDesc {
    using PassDescriptor = vrento::pass::PassDesc;
    vk::VulkanPassIO io;
};
struct Commands {
    Arc<State> state;
    auto       Record(vk::GraphRecordContext&) const -> Result<empty, vk::GraphExecutionError> {
        ++state->recorded;
        return Ok(empty {});
    }
};
struct FrameInput {
    unsigned              value {};
    Option<Arc<unsigned>> lifetime;
    FrameInput(unsigned input): value(input) {}
};
struct Native {
    using FrameData = FrameInput;
    Arc<State> state;
    auto       Instantiate(const vk::GraphPrepareContext&, Arc<FrameInput>) const
        -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError>;
    auto Instantiate(const vk::GraphPrepareContext& context) const
        -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        return Instantiate(context, Arc<FrameInput>::make(0));
    }
};
struct Compiler {
    using Pass = Native;
    auto Compile(const Configuration&, const vk::PassExecutionInfo&, const vk::PassCompileContext&)
        -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError>;
};
} // namespace vrento_test::renderer
namespace rstd
{

} // namespace rstd
namespace vrento_test::renderer
{
auto Native::Instantiate(const vk::GraphPrepareContext&, Arc<FrameInput> input) const
    -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
    ++state->instantiated;
    if (input) state->observed = input->value;
    if (state->frame_pending) {
        --state->frame_pending;
        return Ok(vrento::vulkan::PendingPreparation {});
    }
    if (state->frame_fail) return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Prepare });
    return Ok(Arc<dyn<vk::VulkanPass>>::make(Commands { state.clone() }));
}
auto Compiler::Compile(const Configuration& desc, const vk::PassExecutionInfo&,
                       const vk::PassCompileContext&)
    -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
    rstd_assert(desc.viewport.pViewports == desc.viewports->data());
    ++desc.state->compiled;
    if (desc.state->pending) {
        --desc.state->pending;
        return Ok(vrento::vulkan::PendingPreparation {});
    }
    if (desc.state->fail) {
        vk::GraphExecutionError error { vk::GraphExecutionStage::Prepare };
        error.cause = vrento::vulkan::GraphExecutionCause::Resource(
            vrento::resource::ResourceErrorSnapshot(vrento::resource::ResourceError {
                .message = String::make("native factory failure"_str),
                .source  = Some(vrento::resource::ResourceErrorSource::Native(-3)) }));
        return Err(rstd::move(error));
    }
    return Ok(Arc<dyn<vk::VulkanPass>>::make(Native { desc.state.clone() }));
}
TEST(VulkanRenderer, LostDeviceRemainsTheCompileFailureCause) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                  session = created.unwrap();
    vk::Renderer          renderer(session);
    fg::FrameGraphBuilder builder(48);
    builder.add_pass({ 1 }).unwrap();
    auto graph       = rstd::move(builder).finish().unwrap();
    auto description = [] {
        return make_test_pass(48,
                              1,
                              vk::PassExecutionInfo { .id = { 1 }, .kind = 7 },
                              Configuration(Arc<State>::make()));
    };
    session.observe_result(VK_ERROR_DEVICE_LOST);
    Vec<vk::PassInput> descriptions;
    descriptions.push(description());
    auto compiled = renderer.compile(graph, rstd::move(descriptions));
    ASSERT_TRUE(compiled.is_err());
    EXPECT_EQ(compiled.as_ref().unwrap_err().native_result(), VK_ERROR_DEVICE_LOST);
    auto standalone = renderer.compile_pass(description());
    ASSERT_TRUE(standalone.is_err());
    EXPECT_EQ(standalone.as_ref().unwrap_err().native_result(), VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(renderer.status().error->native_result(), VK_ERROR_DEVICE_LOST);
    EXPECT_EQ(renderer.statistics().compile_failures, 2u);
}
TEST(VulkanRenderer, TypedDescriptionPendingAndFrameRevision) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto         session = created.unwrap();
    vk::Renderer renderer(session);
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto                  state = Arc<State>::make();
    fg::FrameGraphBuilder builder(44);
    builder.add_pass({ 4 }).unwrap();
    auto               graph = rstd::move(builder).finish().unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(
        44, 8, vk::PassExecutionInfo { .id = { 4 }, .kind = 7 }, Configuration(state.clone())));
    EXPECT_FALSE(descriptions[usize()].get<unsigned>().is_some());
    EXPECT_EQ(renderer.compile(graph, rstd::move(descriptions)).unwrap(),
              vk::CompileProgress::Pending);
    EXPECT_TRUE(renderer.pending());
    EXPECT_FALSE(renderer.compiled().is_some());
    renderer.continue_compile(renderer.status().id).unwrap();
    EXPECT_EQ(state->compiled, 2u);
    auto compiled = renderer.compiled().unwrap();
    renderer.clear();
    ASSERT_EQ(compiled.descriptions().len(), usize(1));
    EXPECT_EQ(compiled.descriptions()[usize()].revision(), 8u);
    EXPECT_TRUE(compiled.descriptions()[usize()].get<Configuration>());
    vk::GraphBindings         bindings(44);
    Vec<vk::PassFrameBinding> inputs;
    inputs.push(compiled.frame<Native>(vk::PassId { 4 }, 8, unsigned(0)).unwrap());
    EXPECT_TRUE(compiled.frame<Native>({ 4 }, 7, unsigned(0)).is_err());
    EXPECT_EQ(inputs[usize()].id(), vk::PassId { 4 });
    EXPECT_EQ(inputs[usize()].revision(), 8u);
    auto frame =
        compiled
            .instantiate(bindings,
                         inputs.as_slice(),
                         Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
            .unwrap()
            .take_ready();
    auto context = vk::TextureContext::create(session).unwrap();
    auto work    = context.Begin().unwrap();
    frame.record(work).unwrap();
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    EXPECT_EQ(state->instantiated, 1u);
    EXPECT_EQ(state->recorded, 1u);
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(VulkanRenderer, ReusesImmutableDescriptionsAndKeepsPreviousOnFailure) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto         session = created.unwrap();
    vk::Renderer renderer(session);
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto state     = Arc<State>::make();
    state->pending = 0;
    fg::FrameGraphBuilder builder(46);
    builder.add_pass({ 1 }).unwrap();
    auto graph     = rstd::move(builder).finish().unwrap();
    auto prototype = make_test_pass(
        46, 1, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, Configuration(state.clone()));
    auto descriptions = [&] {
        Vec<vk::PassInput> values;
        values.push(prototype.clone());
        return values;
    };
    renderer.compile(graph, descriptions()).unwrap();
    for (unsigned frame {}; frame < 3; ++frame) {
        renderer.compile(graph, descriptions()).unwrap();
        vk::GraphBindings         bindings(46);
        Vec<vk::PassFrameBinding> inputs;
        inputs.push(
            renderer.compiled()->frame<Native>(vk::PassId { 1 }, 1, unsigned(frame)).unwrap());
        auto prepared =
            renderer.compiled()
                ->instantiate(bindings,
                              inputs.as_slice(),
                              Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
                .unwrap()
                .take_ready();
        auto queue = vk::TextureContext::create(session).unwrap();
        auto work  = queue.Begin().unwrap();
        prepared.record(work).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    }
    EXPECT_EQ(state->compiled, 1u);
    EXPECT_EQ(state->recorded, 3u);
    EXPECT_EQ(renderer.statistics().graph_compiles, 1u);
    EXPECT_EQ(renderer.statistics().native_compiles, 1u);
    EXPECT_EQ(renderer.statistics().compile_requests, 4u);
    EXPECT_EQ(renderer.statistics().graph_reuses, 3u);
    EXPECT_EQ(renderer.statistics().native_completed, 1u);
    EXPECT_EQ(renderer.statistics().execution.frame_preparations, 3u);
    EXPECT_EQ(renderer.statistics().execution.native_instantiations, 3u);
    EXPECT_EQ(renderer.statistics().execution.native_records, 3u);
    EXPECT_EQ(renderer.status().scope, 46u);
    EXPECT_EQ(renderer.status().reason, vk::CompileReason::Reused);
    Vec<vk::PassInput> unknown;
    unknown.push(make_test_pass(
        46, 2, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, UnregisteredDesc {}));
    EXPECT_EQ(renderer.compile(graph, rstd::move(unknown)).unwrap_err().stage,
              vk::GraphExecutionStage::Schema);
    EXPECT_TRUE(renderer.compiled());
    EXPECT_FALSE(renderer.pending());
    auto next = make_test_pass(
        46, 2, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, Configuration(state.clone()));
    Vec<vk::PassInput> changed;
    changed.push(rstd::move(next));
    renderer.compile(graph, rstd::move(changed)).unwrap();
    EXPECT_EQ(state->compiled, 2u);
    EXPECT_EQ(renderer.status().reason, vk::CompileReason::Description);
    EXPECT_EQ(renderer.statistics().compile_failures, 1u);
    auto               waiting = Arc<State>::make();
    Vec<vk::PassInput> pending;
    pending.push(make_test_pass(
        46, 3, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, Configuration(waiting.clone())));
    EXPECT_EQ(renderer.compile(graph, rstd::move(pending)).unwrap(), vk::CompileProgress::Pending);
    EXPECT_TRUE(renderer.compiled());
    renderer.cancel_compile();
    EXPECT_EQ(renderer.statistics().compile_cancellations, 1u);
    EXPECT_FALSE(renderer.pending());
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(VulkanRenderer, CompileTransactionsRejectStaleContinuationAndKeepCompletedPasses) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    vk::Renderer renderer(created.unwrap());
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    fg::FrameGraphBuilder builder(47);
    builder.add_pass({ 1 }).unwrap();
    builder.add_pass({ 2 }).unwrap();
    auto graph     = rstd::move(builder).finish().unwrap();
    auto first     = Arc<State>::make();
    auto second    = Arc<State>::make();
    first->pending = second->pending = 0;
    auto descriptions                = [&](unsigned revision) {
        Vec<vk::PassInput> values;
        values.push(make_test_pass(47,
                                   revision,
                                   vk::PassExecutionInfo { .id = { 1 }, .kind = 7 },
                                   Configuration(first.clone())));
        values.push(make_test_pass(47,
                                   revision,
                                   vk::PassExecutionInfo { .id = { 2 }, .kind = 7 },
                                   Configuration(second.clone())));
        return values;
    };
    renderer.compile(graph, descriptions(1)).unwrap();
    auto previous     = renderer.compiled().unwrap();
    auto completed_id = renderer.status().id;
    EXPECT_TRUE(renderer.ready());
    second->pending = 2;
    EXPECT_EQ(renderer.compile(graph, descriptions(2)).unwrap(), vk::CompileProgress::Pending);
    auto pending_id = renderer.status().id;
    EXPECT_NE(pending_id, completed_id);
    EXPECT_FALSE(renderer.ready());
    EXPECT_EQ(first->compiled, 2u);
    EXPECT_TRUE(renderer.continue_compile(completed_id).is_err());
    EXPECT_FALSE(renderer.cancel_compile(completed_id));
    EXPECT_EQ(renderer.status().id, pending_id);
    EXPECT_EQ(renderer.continue_compile(pending_id).unwrap(), vk::CompileProgress::Pending);
    EXPECT_EQ(first->compiled, 2u);
    renderer.continue_compile(pending_id).unwrap();
    EXPECT_EQ(first->compiled, 2u);
    EXPECT_EQ(second->compiled, 4u);
    EXPECT_TRUE(renderer.ready());
    EXPECT_TRUE(renderer.continue_compile(pending_id).is_err());

    second->pending = 1;
    EXPECT_EQ(renderer.compile(graph, descriptions(3)).unwrap(), vk::CompileProgress::Pending);
    auto replaced_id    = renderer.status().id;
    auto malformed      = descriptions(4);
    malformed[usize(1)] = make_test_pass(
        47, 4, vk::PassExecutionInfo { .id = { 2 }, .kind = 7 }, UnregisteredDesc {});
    EXPECT_EQ(renderer.compile(graph, rstd::move(malformed)).unwrap_err().stage,
              vk::GraphExecutionStage::Schema);
    EXPECT_EQ(renderer.status().state, vk::CompileState::Failed);
    EXPECT_EQ(renderer.status().error->stage, vk::GraphExecutionStage::Schema);
    EXPECT_FALSE(renderer.pending());
    EXPECT_FALSE(renderer.ready());
    EXPECT_TRUE(renderer.compiled());
    EXPECT_TRUE(renderer.continue_compile(replaced_id).is_err());
    EXPECT_EQ(previous.description().scope(), 47u);

    second->pending = 1;
    EXPECT_EQ(renderer.compile(graph, descriptions(5)).unwrap(), vk::CompileProgress::Pending);
    auto cancelled_id = renderer.status().id;
    EXPECT_TRUE(renderer.cancel_compile(cancelled_id));
    EXPECT_EQ(renderer.status().state, vk::CompileState::Cancelled);
    EXPECT_TRUE(renderer.compiled());
    EXPECT_TRUE(renderer.continue_compile(cancelled_id).is_err());
    renderer.compile(graph, descriptions(6)).unwrap();
    EXPECT_TRUE(renderer.ready());
    second->fail = true;
    EXPECT_EQ(renderer.compile(graph, descriptions(7)).unwrap_err().stage,
              vk::GraphExecutionStage::Prepare);
    EXPECT_EQ(renderer.status().state, vk::CompileState::Failed);
    auto failure = renderer.status();
    EXPECT_EQ(failure.error->pass.value, 2u);
    EXPECT_TRUE(failure.error->cause.is_Resource());
    EXPECT_EQ(failure.error->cause.as_Resource().error.error().source->as_Native().code, -3);
    EXPECT_TRUE(renderer.compiled());
    EXPECT_FALSE(renderer.ready());
    renderer.clear();
    EXPECT_EQ(renderer.status().state, vk::CompileState::Idle);
    EXPECT_FALSE(renderer.compiled());
    EXPECT_EQ(failure.error->cause.as_Resource().error.error().source->as_Native().code, -3);
    auto before = renderer.status().id;
    EXPECT_TRUE(renderer.continue_compile(failure.id).is_err());
    EXPECT_EQ(renderer.status().id, before);
    EXPECT_EQ(renderer.status().state, vk::CompileState::Idle);
}

TEST(VulkanRenderer, StandalonePassRetainsPendingDescription) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto         session = created.unwrap();
    vk::Renderer renderer(session);
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto state       = Arc<State>::make();
    state->pending   = 2;
    auto description = Configuration(state.clone());
    auto weak        = description.viewports.downgrade();
    auto result      = renderer.compile_pass(make_test_pass(
        48, 1, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, rstd::move(description)));
    EXPECT_FALSE(result.unwrap().ready());
    EXPECT_EQ(renderer.status().kind, vk::CompileKind::Pass);
    auto id = renderer.status().id;
    EXPECT_TRUE(weak.upgrade());
    EXPECT_EQ(renderer.continue_compile(id).unwrap(), vk::CompileProgress::Pending);
    renderer.continue_compile(id).unwrap();
    EXPECT_TRUE(renderer.ready());
    EXPECT_FALSE(renderer.compiled());
    auto pass = renderer.compiled_pass();
    renderer.clear();
    EXPECT_TRUE(weak.upgrade());
    EXPECT_EQ(state->compiled, 3u);
    {
        vk::GraphBindings bindings(48);
        auto              context = vk::TextureContext::create(session).unwrap();
        auto              work    = context.Begin().unwrap();
        pass->record(bindings,
                     None(),
                     Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}),
                     work)
            .unwrap();
        EXPECT_EQ(state->recorded, 1u);
        pass = None();
        EXPECT_TRUE(weak.upgrade());
    }
    EXPECT_FALSE(weak.upgrade());
}

TEST(VulkanRenderer, AssociationUsesOriginalGraphAndRejectsExtraAccess) {
    fg::FrameGraphBuilder builder(45);
    auto                  source   = builder.import_resource({ .id = { 1, 45 } }).unwrap();
    auto                  target   = builder.create_resource({ .id = { 2, 45 } }).unwrap();
    auto                  pass     = builder.add_pass({ 1 }, { 1 }).unwrap();
    auto                  output   = builder.reserve_write(pass, target).unwrap();
    auto                  declared = vrento_test::texture_copy_pass(
        pass, source.resource, source.version, output.resource, output.version);
    vrento_test::declare_pass(builder, declared).unwrap();
    auto                       graph       = rstd::move(builder).finish().unwrap();
    auto                       declaration = rstd::move(declared);
    Vec<vk::PassExecutionInfo> passes;
    passes.push(declaration.clone());
    auto set = vk::VulkanPassSet::associate(graph, rstd::move(passes)).unwrap();
    EXPECT_EQ(fg::encode_frame_graph(set.frame_graph()).unwrap(),
              fg::encode_frame_graph(graph).unwrap());
    for (unsigned variant {}; variant < 3; ++variant) {
        auto invalid = declaration.clone();
        if (variant == 0)
            invalid.uses.pop();
        else if (variant == 1)
            invalid.uses[usize()].range.first_mip = 1;
        else
            invalid.uses[usize()].access = vk::GraphAccess::Write;
        Vec<vk::PassExecutionInfo> mismatched;
        mismatched.push(rstd::move(invalid));
        EXPECT_TRUE(vk::VulkanPassSet::associate(graph, rstd::move(mismatched)).is_err());
    }
    declaration.uses.push(vk::PassResourceUse(declaration.uses[usize()]));
    passes.push(rstd::move(declaration));
    EXPECT_EQ(vk::VulkanPassSet::associate(graph, rstd::move(passes)).unwrap_err().kind,
              fg::ErrorKind::Conflict);
}
TEST(VulkanRenderer, CopyDescriptionReusesCompiledGraphAcrossFrames) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                         session = created.unwrap();
    auto                         queue   = vk::TextureContext::create(session).unwrap();
    vrento::texture::TextureDesc texture;
    texture.extent = { 4, 4 };
    texture.usage  = vrento::texture::Usage::Sampled | vrento::texture::Usage::TransferSource |
                     vrento::texture::Usage::TransferDestination;
    auto    source = queue.CreateView(queue.CreateTexture(texture).unwrap(), {}).unwrap();
    auto    target = queue.CreateView(queue.CreateTexture(texture).unwrap(), {}).unwrap();
    Vec<u8> bytes;
    bytes.resize(usize(64), u8(0));
    for (usize i {}; i < bytes.len(); i += usize(4)) {
        bytes[i]            = u8(37);
        bytes[i + usize(3)] = u8(255);
    }
    auto upload = queue.Begin().unwrap();
    upload.commands()
        ->Upload({ .target = source, .width = 4, .height = 4, .bytes = rstd::move(bytes) })
        .unwrap();
    queue.wait(queue.Submit(rstd::move(upload)).unwrap()).unwrap();
    fg::FrameGraphBuilder builder(46);
    auto input  = builder.import_resource({ .id = { 1, 46 }, .storage = 1 }).unwrap();
    auto output = builder.create_resource({ .id = { 2, 46 }, .storage = 2 }).unwrap();
    auto copy   = vk::declare_texture_copy(builder, { 1 }, input, output).unwrap();
    builder.export_resource(copy.output).unwrap();
    auto               graph = rstd::move(builder).finish().unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(rstd::move(copy.description));
    vk::Renderer renderer(session);
    vk::register_texture_factories(renderer).unwrap();
    renderer.compile(graph, rstd::move(descriptions)).unwrap();
    auto              compiled = renderer.compiled().unwrap();
    vk::GraphBindings bindings(46);
    bindings.bind(input.resource, Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(source)))
        .unwrap();
    bindings
        .bind(output.resource, Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(target)))
        .unwrap();
    for (unsigned frame = 0; frame != 2; ++frame) {
        auto execution =
            compiled
                .instantiate(bindings,
                             {},
                             Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
                .unwrap()
                .take_ready();
        auto work = queue.Begin().unwrap();
        execution.record(work).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        auto pixels = vrento_test::texture_vulkan::read(queue, target, 0, 0);
        EXPECT_EQ(pixels[usize()], u8(37));
        EXPECT_EQ(pixels[usize(3)], u8(255));
    }
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(VulkanRenderer, ClearGraphicsDescriptionOwnsExecution) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                         session = created.unwrap();
    auto                         queue   = vk::TextureContext::create(session).unwrap();
    vrento::texture::TextureDesc texture;
    texture.extent = { 4, 4 };
    texture.usage =
        vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::TransferSource;
    auto view = queue.CreateView(queue.CreateTexture(texture).unwrap(), {}).unwrap();
    fg::FrameGraphBuilder builder(47);
    auto target = builder.create_resource({ .id = { 1, 47 }, .storage = 1 }).unwrap();
    auto id     = builder.add_pass({ 1 }).unwrap();
    auto output = builder.reserve_write(id, target).unwrap();
    vk::PassExecutionInfo pass { .id = id, .kind = vk::graphics_clear_kind };
    pass.uses.push({ output.resource,
                     output.version,
                     vk::GraphAccess::Write,
                     vk::PassUsage::ColorAttachment,
                     vk::PassStages::Color });
    vk::GraphAttachment attachment { output.resource, output.version };
    attachment.clear = { 0.0f, 1.0f, 0.0f, 1.0f };
    pass.graphics    = Some(vk::GraphicsDescription { .color = Some(attachment) });
    vrento_test::declare_pass(builder, pass).unwrap();
    auto         graph = rstd::move(builder).finish().unwrap();
    vk::Renderer renderer(session);
    vk::register_graphics_factories(renderer).unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(47, 1, rstd::move(pass), vk::GraphicsClearDesc {}));
    renderer.compile(graph, rstd::move(descriptions)).unwrap();
    vk::GraphBindings bindings(47);
    bindings.bind(target.resource, Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
        .unwrap();
    auto frame =
        renderer.compiled()
            ->instantiate(
                bindings, {}, Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
            .unwrap()
            .take_ready();
    renderer.clear();
    auto work = queue.Begin().unwrap();
    frame.record(work).unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto pixels = vrento_test::texture_vulkan::read(queue, view, 0, 0);
    EXPECT_EQ(pixels[usize()], u8(0));
    EXPECT_EQ(pixels[usize(1)], u8(255));
    EXPECT_EQ(pixels[usize(3)], u8(255));
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(VulkanRenderer, GeometrySnapshotSurvivesSourceMutation) {
    Vec<u8> bytes;
    bytes.resize(usize(4), u8(7));
    vrento::GeometryView source;
    source.vertices.push({ .bytes              = bytes.as_slice(),
                           .capacity           = usize(16),
                           .stride             = usize(4),
                           .count              = usize(1),
                           .data_generation    = u64(3),
                           .storage_generation = u64(4) });
    auto snapshot  = vrento::GeometrySnapshot::capture(source);
    bytes[usize()] = u8(9);
    bytes.clear();
    auto moved = rstd::move(snapshot);
    auto view  = moved.view();
    EXPECT_EQ(view.vertices[usize()].bytes[usize()], u8(7));
    EXPECT_EQ(view.vertices[usize()].capacity, usize(16));
    EXPECT_EQ(view.vertices[usize()].data_generation, u64(3));
    EXPECT_EQ(view.vertices[usize()].storage_generation, u64(4));
}
} // namespace vrento_test::renderer

namespace vrento_test::renderer
{
TEST(VulkanRenderer, FramePendingKeepsInputsAndCompletedNativePasses) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto         session = created.unwrap();
    vk::Renderer renderer(session);
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto first     = Arc<State>::make();
    auto second    = Arc<State>::make();
    first->pending = second->pending = 0;
    second->frame_pending            = 2;
    fg::FrameGraphBuilder builder(72);
    builder.add_pass({ 1 }).unwrap();
    builder.add_pass({ 2 }).unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(
        72, 1, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, Configuration(first.clone())));
    descriptions.push(make_test_pass(
        72, 1, vk::PassExecutionInfo { .id = { 2 }, .kind = 7 }, Configuration(second.clone())));
    renderer.compile(rstd::move(builder).finish().unwrap(), rstd::move(descriptions)).unwrap();
    auto compiled  = renderer.compiled().unwrap();
    auto resources = Arc<vrento::resource_registry::RenderResourceSystem>::make();
    ASSERT_TRUE(resources->Initialize(session));
    auto request = [&] {
        vk::GraphFrameRequest value { .bindings = vk::GraphBindings(72),
                                      .scopes   = Box<dyn<vk::GraphicsScopeBackend>>::make(
                                          vk::GraphicsScopeCache {}) };
        value.parameters.push(compiled.frame<Native>({ 1 }, 1, unsigned(17)).unwrap());
        value.parameters.push(compiled.frame<Native>({ 2 }, 1, unsigned(29)).unwrap());
        return value;
    };
    auto frame = vk::GraphFrame::begin(
                     resources.clone(), compiled, request(), resources->BeginFrame().unwrap())
                     .unwrap();
    renderer.clear();
    EXPECT_EQ(frame.record().unwrap_err().stage, vk::GraphExecutionStage::Record);
    EXPECT_EQ(frame.advance().unwrap(), vk::FrameProgress::Pending);
    EXPECT_EQ(frame.advance().unwrap(), vk::FrameProgress::Pending);
    frame.advance().unwrap();
    EXPECT_EQ(first->instantiated, 1u);
    EXPECT_EQ(second->instantiated, 3u);
    EXPECT_EQ(renderer.statistics().execution.frame_preparations, 1u);
    EXPECT_EQ(renderer.statistics().execution.native_instantiations, 4u);
    EXPECT_EQ(renderer.statistics().execution.native_pending, 2u);
    EXPECT_EQ(first->observed, 17u);
    EXPECT_EQ(second->observed, 29u);
    frame.record().unwrap();
    EXPECT_TRUE(frame.record().is_err());
    frame.submit().unwrap();
    EXPECT_EQ(frame.state(), vk::FrameState::Submitted);
    EXPECT_TRUE(frame.submit().is_err());
    EXPECT_TRUE(frame.wait(10000000000ull).is_ok());
    EXPECT_EQ(frame.state(), vk::FrameState::Completed);
    EXPECT_TRUE(frame.wait().is_ok());
    EXPECT_EQ(first->compiled, 1u);
    EXPECT_EQ(first->recorded, 1u);
    EXPECT_EQ(second->recorded, 1u);
    auto       cancelled_request = request();
    auto       owner             = Arc<unsigned>::make(1);
    auto       input_lifetime    = owner.downgrade();
    FrameInput input(17);
    input.lifetime = Some(rstd::move(owner));
    cancelled_request.parameters[usize()] =
        compiled.frame<Native>({ 1 }, 1, rstd::move(input)).unwrap();
    auto cancelled = vk::GraphFrame::begin(resources.clone(),
                                           compiled,
                                           rstd::move(cancelled_request),
                                           resources->BeginFrame().unwrap())
                         .unwrap();
    cancelled.advance().unwrap();
    EXPECT_TRUE(input_lifetime.upgrade());
    cancelled.cancel();
    EXPECT_FALSE(input_lifetime.upgrade());
    EXPECT_EQ(cancelled.state(), vk::FrameState::Cancelled);
    EXPECT_TRUE(cancelled.receipt().cancelled());
    EXPECT_TRUE(cancelled.advance().is_err());
    EXPECT_TRUE(cancelled.submit().is_err());
    auto retry = vk::GraphFrame::begin(
                     resources.clone(), compiled, request(), resources->BeginFrame().unwrap())
                     .unwrap();
    retry.advance().unwrap();
    retry.record().unwrap();
    retry.cancel();
    second->frame_fail = true;
    auto failed        = vk::GraphFrame::begin(
                             resources.clone(), compiled, request(), resources->BeginFrame().unwrap())
                             .unwrap();
    EXPECT_EQ(failed.advance().unwrap_err().stage, vk::GraphExecutionStage::Prepare);
    auto attempts = second->instantiated;
    EXPECT_EQ(failed.state(), vk::FrameState::Failed);
    EXPECT_EQ(failed.error()->pass.value, 2u);
    EXPECT_EQ(renderer.statistics().execution.native_failures, 1u);
    EXPECT_TRUE(failed.receipt().cancelled());
    EXPECT_TRUE(failed.advance().is_err());
    EXPECT_EQ(second->instantiated, attempts);
    EXPECT_TRUE(failed.record().is_err());
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::renderer

namespace vrento_test::renderer
{
struct OutputGate {
    vk::DeviceSession session;
    vvk::Semaphore    semaphore;
    explicit OutputGate(vk::DeviceSession device): session(rstd::move(device)) {}
};
TEST(VulkanRenderer, SubmittedFrameTimeoutRetainsOwnersUntilCompletion) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto session = created.unwrap();
    auto signal  = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkSignalSemaphoreKHR"));
    ASSERT_TRUE(signal);
    auto resources = Arc<vrento::resource_registry::RenderResourceSystem>::make();
    ASSERT_TRUE(resources->Initialize(session));
    auto                      gate = Arc<OutputGate>::make(session);
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
    ASSERT_EQ(session.device().handle().CreateSemaphore(info, gate->semaphore), VK_SUCCESS);
    vk::Renderer          renderer(session);
    fg::FrameGraphBuilder builder(75);
    renderer.compile(rstd::move(builder).finish().unwrap(), {}).unwrap();
    auto work = resources->BeginFrame().unwrap();
    resources->TextureContext()
        .AttachWait(
            work,
            vk::NativeSemaphoreWait { .device    = *session.device().handle(),
                                      .semaphore = *gate->semaphore,
                                      .kind      = vk::NativeSemaphoreKind::Timeline,
                                      .value     = 1,
                                      .owner     = Arc<dyn<rstd::any::Any>>::make(gate.clone()) })
        .unwrap();
    auto retained = Arc<dyn<rstd::any::Any>>::make(unsigned(31));
    auto lifetime = retained.downgrade();
    work.retain(rstd::move(retained));
    vk::GraphFrameRequest request { .bindings = vk::GraphBindings(75),
                                    .scopes   = Box<dyn<vk::GraphicsScopeBackend>>::make(
                                        vk::GraphicsScopeCache {}) };
    auto                  frame =
        vk::GraphFrame::begin(
            resources.clone(), renderer.compiled().unwrap(), rstd::move(request), rstd::move(work))
            .unwrap();
    EXPECT_TRUE(frame.wait().is_err());
    frame.advance().unwrap();
    frame.record().unwrap();
    frame.submit().unwrap();
    EXPECT_EQ(frame.wait(0).unwrap_err().kind, vrento::texture::ErrorKind::Timeout);
    frame.cancel();
    EXPECT_EQ(frame.state(), vk::FrameState::Submitted);
    EXPECT_TRUE(lifetime.upgrade());
    VkSemaphoreSignalInfo release { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    release.semaphore = *gate->semaphore;
    release.value     = 1;
    ASSERT_EQ(signal(*session.device().handle(), &release), VK_SUCCESS);
    frame.wait().unwrap();
    EXPECT_EQ(frame.state(), vk::FrameState::Completed);
    EXPECT_FALSE(lifetime.upgrade());
    frame.wait().unwrap();
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(VulkanRenderer, SurfaceTargetAndScopeOwnTheAcquiredImage) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                         session = created.unwrap();
    auto                         context = vk::TextureContext::create(session).unwrap();
    vrento::texture::TextureDesc description;
    description.extent = { 4, 4 };
    description.usage  = vrento::texture::Usage::ColorAttachment;
    auto view = context.CreateView(context.CreateTexture(description).unwrap(), {}).unwrap();
    vrento::FrameSurfaceLease lease;
    lease.identity             = { u64(1), u32(), u64(1) };
    lease.image                = vk::attachment_image(view).unwrap().image;
    lease.format               = VK_FORMAT_R8G8B8A8_UNORM;
    lease.initial_queue_family = lease.final_queue_family =
        session.device().graphics_queue().family_index;
    lease.final_layout = VK_IMAGE_LAYOUT_GENERAL;
    auto owner         = Arc<dyn<rstd::any::Any>>::make(view);
    auto lifetime      = owner.downgrade();
    auto target = Some(vk::SurfaceTarget::create(session, lease, rstd::move(owner)).unwrap());
    auto cache  = Arc<vk::GraphicsScopeCache>::make();
    auto cache_lifetime = cache.downgrade();
    auto scope          = Some(Box<dyn<vk::GraphicsScopeBackend>>::make(
        vk::SurfaceScopeBackend({ 1, 75 }, target->lease(), target->owner(), rstd::move(cache))));
    {
        auto binding = Arc<dyn<vk::GraphBinding>>::make(
            vk::SurfaceGraphBinding(session, target->lease(), target->owner()));
        binding->Freeze(false).unwrap();
    }
    EXPECT_TRUE(lifetime.upgrade());
    EXPECT_TRUE(cache_lifetime.upgrade());
    auto invalid                    = lease;
    invalid.identity.acquire_serial = u64();
    EXPECT_TRUE(vk::SurfaceTarget::create(session, invalid, target->owner()).is_err());
    target = None();
    EXPECT_TRUE(lifetime.upgrade());
    scope = None();
    EXPECT_FALSE(lifetime.upgrade());
    EXPECT_FALSE(cache_lifetime.upgrade());
}
} // namespace vrento_test::renderer

namespace vrento_test::renderer
{
TEST(VulkanRenderer, GraphicsDrawRebindsAndRecompilesRecipeWithDeviceCache) {
    namespace registry = vrento::resource_registry;
    auto created       = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto session   = created.unwrap();
    auto resources = Arc<registry::RenderResourceSystem>::make();
    ASSERT_TRUE(resources->Initialize(session));
    auto transaction = resources->BeginPreparePlan({ .generation = u64(1) }, {}).unwrap();
    resources->ContinuePreparePlan(transaction).unwrap();
    resources->CommitPreparePlan(transaction).unwrap();
    auto         queue = vk::TextureContext::create(session).unwrap();
    vk::Renderer renderer(session);
    vk::register_graphics_factories(renderer).unwrap();
    fg::FrameGraphBuilder builder(101);
    auto target = builder.create_resource({ .id = { 1, 101 }, .storage = 1 }).unwrap();
    auto id     = builder.add_pass({ 1 }).unwrap();
    auto output = builder.reserve_write(id, target).unwrap();
    vk::GraphicsPassResources accesses;
    accesses.attachments.color = Some(vk::GraphicsAttachment {
        .target = vrento::pass::ContentRef::bind(builder, output).unwrap() });
    auto declaration = vk::resolve_graphics_inputs(builder, id, rstd::move(accesses)).unwrap();
    vk::GraphicsPipeline defaults;
    defaults.toDefault();
    vk::GraphicsDrawDesc recipe;
    recipe.pipeline         = { u64(1), u64(1) };
    recipe.render_pass      = { u64(1), u64(1) };
    recipe.layout           = Some(vk::PipelineLayoutRequirement { .pipeline = recipe.pipeline });
    recipe.request.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    recipe.request.depth    = defaults.depth;
    recipe.request.raster   = defaults.raster;
    recipe.request.raster.cullMode            = VK_CULL_MODE_NONE;
    recipe.request.multisample                = defaults.multisample;
    recipe.request.color_blend.colorWriteMask = 15;
    auto stage                                = [&](const auto& words, vrento::ShaderType type) {
        auto value   = Box<vk::ShaderSpv>::make();
        value->stage = type;
        value->spirv = vrento::ShaderCode::from(
            slice<rstd::uint32_t>::from_raw_parts(words, usize(sizeof(words) / sizeof(words[0]))));
        recipe.request.shader_stages.push(rstd::move(value));
    };
    stage(vrento_test::shaders::vertex.words, vrento::ShaderType::VERTEX);
    stage(vrento_test::shaders::fragment.words, vrento::ShaderType::FRAGMENT);
    recipe.resources = declaration.clone();
    auto desc        = vk::PassInput::publish(101, 1, id, recipe.clone()).unwrap();
    vk::declare_input(builder, desc).unwrap();
    auto               graph = rstd::move(builder).finish().unwrap();
    Vec<vk::PassInput> initial;
    initial.push(desc.clone());
    auto prepared =
        vk::PreparedGraphicsResources::prepare(resources.clone(), initial.as_slice(), {}).unwrap();
    auto compile = [&](vk::PassInput value) {
        Vec<vk::PassInput> descriptions;
        descriptions.push(rstd::move(value));
        renderer.compile(graph, rstd::move(descriptions), Some(prepared->compilation())).unwrap();
    };
    compile(desc.clone());
    auto creations = session.graphics_resources().pipelines.stats().creations;
    EXPECT_EQ(creations, u64(1));
    vrento::texture::TextureDesc texture;
    texture.extent = { 4, 4 };
    texture.usage =
        vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::TransferSource;
    for (unsigned frame {}; frame < 2; ++frame) {
        auto view = queue.CreateView(queue.CreateTexture(texture).unwrap(), {}).unwrap();
        vk::GraphBindings bindings(101);
        bindings
            .bind(target.resource, Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
        vk::GraphicsFrameData input;
        input.viewports.push({ 0, 0, 4, 4, 0, 1 });
        input.scissors.push({ { 0, 0 }, { 4, 4 } });
        input.draws.push({ 3, 0 });
        Vec<vk::PassFrameBinding> parameters;
        parameters.push(
            renderer.compiled()->frame<vk::GraphicsDrawPass>(id, 1, rstd::move(input)).unwrap());
        auto instance =
            renderer.compiled()
                ->instantiate(bindings,
                              parameters.as_slice(),
                              Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
                .unwrap()
                .take_ready();
        auto work = queue.Begin().unwrap();
        instance.record(work).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        auto pixels = vrento_test::texture_vulkan::read(queue, view, 0, 0);
        EXPECT_EQ(pixels[usize(1)], u8(255));
        EXPECT_EQ(pixels[usize(3)], u8(255));
    }
    EXPECT_EQ(renderer.statistics().graph_compiles, 1u);
    EXPECT_EQ(renderer.statistics().execution.native_records, 2u);
    EXPECT_EQ(session.graphics_resources().pipelines.stats().creations, creations);
    compile(desc.clone());
    EXPECT_EQ(renderer.status().reason, vk::CompileReason::Reused);
    recipe.request.color_blend.colorWriteMask = 7;
    recipe.resources                          = declaration.clone();
    compile(vk::PassInput::publish(101, 2, id, rstd::move(recipe)).unwrap());
    EXPECT_EQ(renderer.status().reason, vk::CompileReason::Description);
    EXPECT_EQ(session.graphics_resources().pipelines.stats().creations, creations + u64(1));
    compile(desc.clone());
    EXPECT_EQ(session.graphics_resources().pipelines.stats().creations, creations + u64(1));
    EXPECT_TRUE(prepared->pipeline({ u64(1), u64(1) }).unwrap()->cache_hit);
    EXPECT_EQ(session.validation_errors(), 0u);
}

TEST(VulkanRenderer, TextureRegionSurvivesPendingFailureAndCancellationWithoutCompile) {
    namespace registry = vrento::resource_registry;
    namespace tex      = vrento::texture;
    auto created       = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto session   = created.unwrap();
    auto resources = Arc<registry::RenderResourceSystem>::make();
    ASSERT_TRUE(resources->Initialize(session));
    registry::RegistryTextureContext    textures(vk::TextureContext::create(session).unwrap());
    vrento::resource::TextureDefinition definition;
    definition.width = definition.height = i32(2);
    definition.usage                     = vrento::resource::TextureUsage::TransferDestination |
                                           vrento::resource::TextureUsage::Sampled;
    auto allocation      = textures.AllocateTexture(definition, false).unwrap().allocation;
    auto state           = Arc<State>::make();
    state->pending       = 0;
    state->frame_pending = 1;
    vk::Renderer renderer(session);
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    fg::FrameGraphBuilder builder(102);
    builder.add_pass({ 1 }).unwrap();
    auto               graph = rstd::move(builder).finish().unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(
        102, 1, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, Configuration(state.clone())));
    renderer.compile(graph, rstd::move(descriptions)).unwrap();
    auto compiled = renderer.compiled().unwrap();
    auto region   = [&](unsigned size) {
        Vec<u8> bytes;
        bytes.resize(usize(size * size * 4), u8(127));
        tex::TextureUpload upload;
        upload.target = allocation->Slots()[usize()].view;
        upload.width = upload.height = size;
        upload.bytes                 = rstd::move(bytes);
        Vec<tex::TextureUpload> uploads;
        uploads.push(rstd::move(upload));
        return registry::PreparedTextureUpdate { allocation.clone(), rstd::move(uploads) };
    };
    auto begin = [&](const registry::PreparedTextureUpdate& update) {
        Vec<registry::PreparedTextureUpdate> regions;
        regions.push(update.clone());
        return vk::GraphFrame::begin(resources.clone(),
                                     compiled,
                                     { .bindings = vk::GraphBindings(102),
                                       .scopes   = Box<dyn<vk::GraphicsScopeBackend>>::make(
                                           vk::GraphicsScopeCache {}) },
                                     resources->BeginFrame().unwrap(),
                                     rstd::move(regions))
            .unwrap();
    };
    auto update  = region(2);
    auto pending = begin(update);
    EXPECT_EQ(pending.advance().unwrap(), vk::FrameProgress::Pending);
    EXPECT_EQ(update.Readiness(Some(pending.receipt())).unwrap(),
              registry::ImageReadiness::Recorded);
    pending.cancel();
    EXPECT_EQ(update.Readiness().unwrap(), registry::ImageReadiness::Pending);
    state->frame_fail = true;
    auto failed       = begin(update);
    EXPECT_EQ(failed.advance().unwrap_err().pass.value, 1u);
    EXPECT_EQ(update.Readiness().unwrap(), registry::ImageReadiness::Pending);
    state->frame_fail = false;
    auto retry        = begin(update);
    retry.advance().unwrap();
    retry.record().unwrap();
    retry.submit().unwrap();
    retry.wait().unwrap();
    EXPECT_EQ(update.Readiness().unwrap(), registry::ImageReadiness::Complete);
    auto smaller = region(1);
    auto next    = begin(smaller);
    next.advance().unwrap();
    next.record().unwrap();
    next.submit().unwrap();
    next.wait().unwrap();
    EXPECT_EQ(renderer.statistics().graph_compiles, 1u);
    EXPECT_EQ(renderer.statistics().native_compiles, 1u);
    EXPECT_EQ(compiled.descriptions()[usize()].revision(), 1u);
    EXPECT_EQ(graph.passes().len(), usize(1));
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::renderer

namespace vrento_test::pass_inputs
{
namespace vk       = vrento::vulkan;
namespace fg       = vrento::fg;
namespace resource = vrento::resource;
namespace pass     = vrento::pass;

auto texture_definition(ref<str> name) -> resource::TextureResourceRef {
    return resource::TextureRequest { .kind       = resource::TextureRequestKind::RenderTarget,
                                      .name       = String::make(name),
                                      .definition = Some(resource::TextureDefinition {
                                          .width = i32(8), .height = i32(8) }) };
}
TEST(PassInputs, LogicalDefinitionRevisionDoesNotReplaceIdentityOrOldSnapshot) {
    auto first                = texture_definition("first"_str);
    auto retained             = first.clone();
    auto request              = first.definition().clone();
    request.definition->width = i32(16);
    auto next                 = first.with_definition(rstd::move(request));
    EXPECT_EQ(first.id(), next.id());
    EXPECT_EQ(next.revision(), first.revision() + u64(1));
    EXPECT_EQ(retained.definition().definition->width, i32(8));
    EXPECT_EQ(next.definition().definition->width, i32(16));
}
TEST(PassInputs, OutputReferencesKeepResourceOwnersAndAllowDescriptionReordering) {
    fg::FrameGraphBuilder graph(701);
    pass::GraphResources  resources(u64(701));
    auto                  definition = texture_definition("source"_str);
    auto                  source =
        resources.initial(graph, { .key = "source"_Str, .definition = definition.clone() })
            .unwrap();
    auto target =
        resources
            .initial(graph, { .key = "target"_Str, .definition = texture_definition("target"_str) })
            .unwrap();
    auto clear = vk::declare_graphics_clear(graph, { 1 }, source, {}).unwrap();
    auto copy  = vk::declare_texture_copy(graph, { 2 }, clear.output.clone(), target).unwrap();
    EXPECT_EQ(clear.output.output()->producer.value, 1u);
    EXPECT_EQ(copy.output.output()->producer.value, 2u);
    EXPECT_EQ(clear.output.texture()->id(), definition.id());
    resources.handle(clear.output).unwrap();
    resources.handle(copy.output).unwrap();
    auto frame_graph = rstd::move(graph).finish().unwrap();
    auto plan        = resources.resourcePlan(frame_graph).unwrap();
    auto mapping     = resources.bindings(frame_graph, plan).unwrap();
    ASSERT_FALSE(mapping.textures.is_empty());
    EXPECT_TRUE(mapping.textures[usize()].content.output());
    auto               clear_identity = clear.description.description().as_raw_ptr();
    Vec<vk::PassInput> descriptions;
    descriptions.push(rstd::move(copy.description));
    descriptions.push(clear.description.clone());
    auto input =
        vk::GraphInput::create(frame_graph, rstd::move(descriptions), rstd::move(mapping)).unwrap();
    EXPECT_EQ(input.descriptions()[usize(1)].description().as_raw_ptr(), clear_identity);
    EXPECT_EQ(input.descriptions()[usize(1)].io().outputs[usize()].target.texture()->id(),
              definition.id());
    Vec<vk::PassInput> duplicate;
    duplicate.push(clear.description.clone());
    duplicate.push(clear.description.clone());
    EXPECT_TRUE(input.with_descriptions(rstd::move(duplicate)).is_err());
}
TEST(PassInputs, RejectsIncompleteDescriptionsWrongProducerAndScopeWithoutDevice) {
    EXPECT_TRUE(vk::PassInput::publish(702, 1, { 1 }, vk::TextureCopyDesc {}).is_err());
    EXPECT_TRUE(vk::PassInput::publish(702, 1, { 1 }, vk::GraphicsDrawDesc {}).is_err());
    fg::FrameGraphBuilder graph(702);
    pass::GraphResources  resources(u64(702));
    auto                  source = resources
                                       .initial(graph,
                                                { .key        = "source"_Str,
                                                  .imported   = true,
                                                  .definition = texture_definition("source"_str) })
                                       .unwrap();
    auto                  target =
        resources
            .initial(graph, { .key = "target"_Str, .definition = texture_definition("target"_str) })
            .unwrap();
    graph.add_pass({ 1 }).unwrap();
    auto output = resources.output(graph, { 1 }, target).unwrap();
    EXPECT_TRUE(
        vk::PassInput::publish(702, 1, { 2 }, vk::TextureCopyDesc { source, output }).is_err());
    EXPECT_TRUE(
        vk::PassInput::publish(703, 1, { 1 }, vk::TextureCopyDesc { source, output }).is_err());
    EXPECT_FALSE(output.resolve(703).is_ok());
}
TEST(PassInputs, AttachmentLoadReadsPreviousOutputAndReusesItsStorage) {
    fg::FrameGraphBuilder graph(704);
    pass::GraphResources  resources(u64(704));
    auto                  initial =
        resources
            .initial(graph, { .key = "target"_Str, .definition = texture_definition("target"_str) })
            .unwrap();
    auto clear = vk::declare_graphics_clear(graph, { 1 }, initial, {}).unwrap();
    graph.add_pass({ 2 }).unwrap();
    auto                 output = resources.output(graph, { 2 }, clear.output).unwrap();
    vk::GraphicsDrawDesc draw;
    draw.resources.attachments.color =
        Some(vk::GraphicsAttachment { .target   = output.clone(),
                                      .previous = Some(clear.output.clone()),
                                      .load     = vk::AttachmentLoad::Load });
    auto        published = vk::declare_input(graph, { 2 }, rstd::move(draw)).unwrap();
    const auto& io        = published.io();
    ASSERT_EQ(io.inputs.len(), usize(1));
    EXPECT_EQ(io.inputs[usize()].source.output()->producer.value, 1u);
    EXPECT_EQ(io.outputs[usize()].target.output()->producer.value, 2u);
    auto built = rstd::move(graph).finish().unwrap();
    EXPECT_TRUE(built.find_version(output.version())->same_storage);
    EXPECT_TRUE(built.inspect().precedes(0, 1));
}
TEST(PassInputs, DiscardedOutputCannotSupplyLaterContents) {
    fg::FrameGraphBuilder graph(705);
    pass::GraphResources  resources(u64(705));
    auto                  first =
        resources
            .initial(graph, { .key = "first"_Str, .definition = texture_definition("first"_str) })
            .unwrap();
    auto target =
        resources
            .initial(graph, { .key = "second"_Str, .definition = texture_definition("second"_str) })
            .unwrap();
    graph.add_pass({ 1 }).unwrap();
    auto                  output = resources.output(graph, { 1 }, first).unwrap();
    vk::GraphicsClearDesc clear;
    clear.resources.attachments.color = Some(
        vk::GraphicsAttachment { .target = output.clone(), .store = vk::AttachmentStore::Discard });
    vk::declare_input(graph, { 1 }, rstd::move(clear)).unwrap();
    vk::declare_texture_copy(graph, { 2 }, output, target).unwrap();
    EXPECT_EQ(rstd::move(graph).finish().unwrap_err().kind, fg::ErrorKind::Undefined);
}
} // namespace vrento_test::pass_inputs

TEST(PassInputs, GraphScopeIsIndependentOfResourceGeneration) {
    vrento::fg::FrameGraphBuilder graph(706);
    auto                          version = graph.import_resource({ .id = { 9, 23 } }).unwrap();
    auto                          content = vrento::pass::ContentRef::bind(graph, version).unwrap();
    EXPECT_TRUE(content.resolve(706).is_ok());
    EXPECT_FALSE(content.resolve(23).is_ok());
    EXPECT_EQ(content.version().resource.generation, 23u);
}

namespace vrento_test::renderer
{
struct SharedCompiler {
    using Pass = Native;
    Arc<dyn<vk::VulkanPass>> native;
    auto Compile(const Configuration&, const vk::PassExecutionInfo&, const vk::PassCompileContext&)
        -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        return Ok(native.clone());
    }
};
TEST(VulkanRenderer, IndexedFramesResolveSparseReorderedPasses) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    vk::Renderer renderer(created.unwrap());
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto first     = Arc<State>::make();
    auto second    = Arc<State>::make();
    first->pending = second->pending = 0;
    fg::PassId            large { 9000000000000ULL }, small { 17 };
    fg::FrameGraphBuilder builder(801);
    builder.add_pass(large).unwrap();
    builder.add_pass(small).unwrap();
    builder.depends_on(large, small).unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(
        801, 4, vk::PassExecutionInfo { .id = small, .kind = 7 }, Configuration(second.clone())));
    descriptions.push(make_test_pass(
        801, 9, vk::PassExecutionInfo { .id = large, .kind = 7 }, Configuration(first.clone())));
    renderer.compile(rstd::move(builder).finish().unwrap(), rstd::move(descriptions)).unwrap();
    auto compiled = renderer.compiled().unwrap();
    EXPECT_EQ((*compiled.description(large))->revision(), 9u);
    EXPECT_EQ((*compiled.description(small))->revision(), 4u);
    EXPECT_FALSE(compiled.description({ 18 }));
    EXPECT_TRUE(compiled.frame<Native>({ 18 }, 4, unsigned(0)).is_err());
    EXPECT_TRUE(compiled.frame<vk::GraphicsDrawPass>(large, 9, vk::GraphicsFrameData {}).is_err());
    Vec<vk::PassFrameBinding> inputs;
    inputs.push(compiled.frame<Native>(large, 9, unsigned(31)).unwrap());
    inputs.push(compiled.frame<Native>(small, 4, unsigned(47)).unwrap());
    auto scopes = [] {
        return Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {});
    };
    vk::GraphBindings bindings(801);
    auto frame = compiled.prepare_frame(bindings, inputs.as_slice(), scopes()).unwrap();
    inputs.push(inputs[usize()].clone());
    EXPECT_TRUE(compiled.prepare_frame(bindings, inputs.as_slice(), scopes()).is_err());
    renderer.clear();
    frame.advance().unwrap();
    EXPECT_EQ(first->observed, 31u);
    EXPECT_EQ(second->observed, 47u);
    EXPECT_EQ(first->instantiated, 1u);
    EXPECT_EQ(second->instantiated, 1u);
    auto defaults = compiled.prepare_frame(bindings, {}, scopes()).unwrap();
    defaults.advance().unwrap();
    EXPECT_EQ(first->observed, 0u);
    EXPECT_EQ(second->observed, 0u);
}
TEST(VulkanRenderer, FrameBindingRejectsAnotherCompilationSharingTheSameNativePass) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto         session = created.unwrap();
    vk::Renderer first(session), second(session);
    auto         state   = Arc<State>::make();
    auto         native  = Arc<dyn<vk::VulkanPass>>::make(Native { state.clone() });
    auto         factory = Arc<dyn<vk::PassFactory>>::make(
        vk::TypedPassFactory<Configuration, SharedCompiler> { { native.clone() } });
    first.bind(factory.clone()).unwrap();
    second.bind(factory.clone()).unwrap();
    fg::FrameGraphBuilder builder(802);
    builder.add_pass({ 42 }).unwrap();
    auto graph     = rstd::move(builder).finish().unwrap();
    auto prototype = make_test_pass(
        802, 3, vk::PassExecutionInfo { .id = { 42 }, .kind = 7 }, Configuration(state.clone()));
    auto descriptions = [&] {
        Vec<vk::PassInput> values;
        values.push(prototype.clone());
        return values;
    };
    first.compile(graph, descriptions()).unwrap();
    auto                      original = first.compiled().unwrap();
    Vec<vk::PassFrameBinding> inputs;
    inputs.push(original.frame<Native>({ 42 }, 3, unsigned(19)).unwrap());
    second.compile(graph, descriptions()).unwrap();
    auto other  = second.compiled().unwrap();
    auto scopes = [] {
        return Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {});
    };
    vk::GraphBindings bindings(802);
    EXPECT_TRUE(other.prepare_frame(bindings, inputs.as_slice(), scopes()).is_err());
    first.compile(graph, descriptions()).unwrap();
    EXPECT_EQ(first.status().reason, vk::CompileReason::Reused);
    auto reused   = first.compiled().unwrap();
    auto accepted = reused.prepare_frame(bindings, inputs.as_slice(), scopes()).unwrap();
    accepted.advance().unwrap();
    EXPECT_EQ(state->observed, 19u);
    auto a     = first.compile_pass(prototype.clone()).unwrap().take_ready();
    auto b     = second.compile_pass(prototype.clone()).unwrap().take_ready();
    auto queue = vk::TextureContext::create(session).unwrap();
    auto work  = queue.Begin().unwrap();
    EXPECT_TRUE(
        b.record(bindings, Some(a.frame<Native>({ 42 }, 3, unsigned(1)).unwrap()), scopes(), work)
            .is_err());
    work.discard();
}
TEST(VulkanRenderer, IndexedPendingFramesKeepSeparateInputs) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    vk::Renderer renderer(created.unwrap());
    renderer.bind(Arc<dyn<vk::PassFactory>>::make(vk::TypedPassFactory<Configuration, Compiler> {}))
        .unwrap();
    auto state           = Arc<State>::make();
    state->pending       = 0;
    state->frame_pending = 1;
    fg::FrameGraphBuilder builder(803);
    builder.add_pass({ 33 }).unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(make_test_pass(
        803, 1, vk::PassExecutionInfo { .id = { 33 }, .kind = 7 }, Configuration(state.clone())));
    renderer.compile(rstd::move(builder).finish().unwrap(), rstd::move(descriptions)).unwrap();
    auto compiled = renderer.compiled().unwrap();
    auto capture  = [&](unsigned value) {
        Vec<vk::PassFrameBinding> inputs;
        inputs.push(compiled.frame<Native>({ 33 }, 1, value).unwrap());
        return compiled
            .prepare_frame(vk::GraphBindings(803),
                           inputs.as_slice(),
                           Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}))
            .unwrap();
    };
    auto first  = capture(11);
    auto second = capture(22);
    renderer.clear();
    EXPECT_EQ(first.advance().unwrap(), vk::FrameProgress::Pending);
    second.advance().unwrap();
    EXPECT_EQ(state->observed, 22u);
    first.advance().unwrap();
    EXPECT_EQ(state->observed, 11u);
    EXPECT_TRUE(first.ready());
    EXPECT_TRUE(second.ready());
    EXPECT_EQ(state->instantiated, 3u);
}
} // namespace vrento_test::renderer
