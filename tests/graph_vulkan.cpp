#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#include <cstdio>
import rstd;
import rstd.test;
import vrento;
import vrento.tests.shaders;

import vrento.tests.pass_set;

using namespace rstd::prelude;

namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
namespace rg  = vrento::vulkan;
namespace vrento_test::texture_vulkan
{
auto read(vk::TextureContext&, tex::TextureViewRef, unsigned, unsigned) -> Vec<u8>;
}
namespace vrento_test::graph_vulkan
{
struct Assets {
    Option<tex::SubmissionToken>                            gate;
    vk::DeviceSession                                       session;
    vrento::resource_registry::DescriptorLayoutRegistry     descriptors;
    vrento::resource_registry::PipelineLayoutRegistry       layouts;
    vrento::resource_registry::RenderPassRegistry           render_passes;
    vrento::resource_registry::PipelineRegistry             pipelines;
    tex::SamplerRef                                         sampler;
    Option<vrento::resource_registry::PipelineLayoutResult> layout;
    Option<vrento::resource::PipelineRecipeHandle>          recipe;
    unsigned                                                live {}, prepares {};
    bool                  fail_prepare {}, fail_record {}, pending {}, green {};
    VkColorComponentFlags write_mask { 15 };
    bool                  freeze_descriptor {};
    rg::GraphSerial       fail_pass {};
    Option<vrento::resource_registry::PreparedDescriptorBinding> descriptor_source;
    Assets(vk::DeviceSession device, tex::SamplerRef sample)
        : session(rstd::move(device)), sampler(rstd::move(sample)) {
        vrento::resource_registry::PipelineLayoutRequest request;
        vk::DescriptorSetInfo                            set;
        set.bindings.push(
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT });
        request.descriptor_sets.push(rstd::move(set));
        layout = Some(layouts.Ensure(session.device(), request, descriptors).unwrap());
    }
};
struct Prepared {
    rstd::sync::Arc<Assets>                                      assets;
    rg::GraphSerial                                              kind;
    Option<vrento::resource_registry::PipelineResourceResult>    pipeline;
    vvk::Framebuffer                                             framebuffer;
    Option<vrento::resource_registry::PreparedDescriptorBinding> descriptor;
    Prepared(rstd::sync::Arc<Assets> owner, rg::GraphSerial type)
        : assets(rstd::move(owner)), kind(type) {
        ++assets->live;
    }
    Prepared(const Prepared&) = delete;
    Prepared(Prepared&& other)
        : assets(other.assets.clone()),
          kind(other.kind),
          pipeline(rstd::move(other.pipeline)),
          framebuffer(rstd::move(other.framebuffer)),
          descriptor(rstd::move(other.descriptor)) {
        ++assets->live;
    }
    ~Prepared() { --assets->live; }
    auto Record(vk::GraphRecordContext&) const -> Result<empty, vk::GraphExecutionError>;
};
struct SurfaceImage {
    vk::DeviceSession   session;
    vvk::AllocatedImage image;
    vvk::ImageView      view;
    explicit SurfaceImage(vk::DeviceSession device): session(rstd::move(device)) {}
};
struct SurfaceClearDraw {
    auto Record(vk::GraphRecordContext&) const -> Result<empty, vk::GraphExecutionError> {
        return Ok(empty {});
    }
};
struct SurfaceClearBackend {
    auto Prepare(const rg::PassExecutionInfo&, const vk::GraphPrepareContext&)
        -> Result<rstd::sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError>;
};
struct UncertainSubmit {
    unsigned calls {}, fail_at { 1 };
    auto     Submit(const vvk::Queue& queue, const VkSubmitInfo& info, VkFence fence) -> VkResult {
        auto result = queue.Submit(info, fence);
        ++calls;
        return result == VK_SUCCESS && calls == fail_at ? VK_ERROR_UNKNOWN : result;
    }
};
struct Backend {
    rstd::sync::Arc<Assets> assets;
    auto                    Prepare(const rg::PassExecutionInfo&, const vk::GraphPrepareContext&)
        -> Result<rstd::sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError>;
};
} // namespace vrento_test::graph_vulkan
namespace rstd
{

template<>
struct Impl<vk::VulkanPassBackend, vrento_test::graph_vulkan::SurfaceClearBackend>
    : ImplBase<vrento_test::graph_vulkan::SurfaceClearBackend> {
    auto Prepare(const rg::PassExecutionInfo& pass, const vk::GraphPrepareContext& context)
        -> Result<sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};
template<>
struct Impl<vk::NativeQueueSubmit, vrento_test::graph_vulkan::UncertainSubmit>
    : ImplBase<vrento_test::graph_vulkan::UncertainSubmit> {
    auto Submit(const vvk::Queue& queue, const VkSubmitInfo& info, VkFence fence) -> VkResult {
        return self().Submit(queue, info, fence);
    }
};

template<>
struct Impl<vk::VulkanPassBackend, vrento_test::graph_vulkan::Backend>
    : ImplBase<vrento_test::graph_vulkan::Backend> {
    auto Prepare(const rg::PassExecutionInfo& pass, const vk::GraphPrepareContext& context)
        -> Result<rstd::sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};
template<>
struct Impl<vk::VulkanPass, rstd::sync::Arc<vrento_test::graph_vulkan::Prepared>>
    : VulkanPassImplementation<rstd::sync::Arc<vrento_test::graph_vulkan::Prepared>> {
    auto Record(vk::GraphRecordContext& context) const -> Result<empty, vk::GraphExecutionError> {
        return self()->Record(context);
    }
};
} // namespace rstd
namespace vrento_test::graph_vulkan
{
auto SurfaceClearBackend::Prepare(const rg::PassExecutionInfo&, const vk::GraphPrepareContext&)
    -> Result<rstd::sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError> {
    return Ok(rstd::sync::Arc<dyn<vk::VulkanPass>>::make(SurfaceClearDraw {}));
}
template<rstd::size_t N>
auto shader(const unsigned (&words)[N], vrento::ShaderType stage) -> vk::Uni_ShaderSpv {
    auto value   = Box<vk::ShaderSpv>::make();
    value->stage = stage;
    for (auto word : words) value->spirv.push(unsigned(word));
    return value;
}
auto Backend::Prepare(const rg::PassExecutionInfo& pass, const vk::GraphPrepareContext& context)
    -> Result<rstd::sync::Arc<dyn<vk::VulkanPass>>, vk::GraphExecutionError> {
    ++assets->prepares;
    if (assets->pending && pass.kind == 2)
        return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Pending });
    if (assets->fail_prepare && pass.kind == 2)
        return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Prepare });
    auto prepared = rstd::sync::Arc<Prepared>::make(assets.clone(), pass.kind);
    if (pass.kind == 2) {
        vk::GraphicsPipeline defaults;
        defaults.toDefault();
        vk::PipelineResourceRequest request;
        request.pipeline_layout                  = assets->layout->handle;
        request.topology                         = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        request.depth                            = defaults.depth;
        request.raster                           = defaults.raster;
        request.multisample                      = defaults.multisample;
        request.color_blend.colorWriteMask       = assets->write_mask;
        auto scope                               = (*context.scope)->Info();
        request.multisample.rasterizationSamples = scope.signature.samples;
        request.color_load_op                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
        request.color_initial_layout             = request.color_final_layout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        request.shader_stages.push(shader(shaders::vertex.words, vrento::ShaderType::VERTEX));
        request.shader_stages.push(
            assets->green ? shader(shaders::fragment.words, vrento::ShaderType::FRAGMENT)
                          : shader(shaders::graph_fragment.words, vrento::ShaderType::FRAGMENT));
        if (assets->recipe.is_none())
            assets->recipe = assets->pipelines.RegisterRecipe(rstd::move(request), assets->layouts);
        prepared->pipeline = Some(assets->pipelines
                                      .EnsureRecipe(context.session.device(),
                                                    *assets->recipe,
                                                    scope.signature,
                                                    assets->render_passes,
                                                    assets->layouts)
                                      .unwrap());
        auto source =
            context.bindings.resolve(pass.uses[usize()].resource).unwrap()->Texture().unwrap();
        auto target =
            context.bindings.resolve(pass.uses[usize(1)].resource).unwrap()->Texture().unwrap();
        auto  view   = vk::native_view(target).unwrap();
        auto& device = context.session.device().handle();
        rstd_assert(view && scope.extent.width == 4 && scope.extent.height == 4);
        const auto layout =
            assets->descriptors.Resolve(assets->layout->physical->descriptor_layouts[usize()])
                .unwrap();
        auto sampled = vk::sampled_image(source, assets->sampler).unwrap();
        vrento::resource_registry::DescriptorImageBinding binding { 0,
                                                                    sampled.image,
                                                                    sampled.layout };
        auto& owner = assets->session.graphics_resources().descriptor_snapshots;
        auto  original =
            owner
                .Prepare(context.session.device(),
                         0,
                         *layout,
                         slice<vrento::resource_registry::DescriptorImageBinding>::from_raw_parts(
                             &binding, usize(1)),
                         {},
                         vrento::resource_registry::DescriptorBindingReuse::Exclusive)
                .unwrap();
        assets->descriptor_source = Some(original.clone());
        prepared->descriptor =
            assets->freeze_descriptor
                ? Some(owner.Snapshot(context.session.device(), original, assets->descriptors)
                           .unwrap())
                : Some(rstd::move(original));
    }
    return Ok(rstd::sync::Arc<dyn<vk::VulkanPass>>::make(rstd::move(prepared)));
}
auto Prepared::Record(vk::GraphRecordContext& context) const
    -> Result<empty, vk::GraphExecutionError> {
    if ((assets->fail_record && kind == 2) || context.pass.id.value == assets->fail_pass)
        return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Record });
    auto        command = vk::native_recording(context.submission, context.session).unwrap();
    const auto& d       = context.session.device().handle().Dispatch();
    if (kind == 5) {
        auto waited = context.submission.commands()->Wait(*assets->gate, tex::Stages::All);
        if (waited.is_err())
            return Err(vk::GraphExecutionError {
                vk::GraphExecutionStage::Access, {}, {}, VK_SUCCESS, Some(waited.unwrap_err()) });
        return Ok(empty {});
    }
    if (kind == 1) {
        tex::TextureUpload upload;
        upload.target = context.texture(context.pass.uses[usize()].resource).unwrap();
        upload.width = upload.height = 4;
        for (unsigned i {}; i < 64; ++i) upload.bytes.push(u8(i % 4 == 3 ? 255 : 41));
        auto result = context.submission.commands()->Upload(rstd::move(upload));
        if (result.is_err())
            return Err(vk::GraphExecutionError {
                vk::GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(result.unwrap_err()) });
    } else if (kind == 2) {
        d.vkCmdBindPipeline(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline->pipeline->pipeline.handle);
        vvk::CommandBuffer                                      cmd(command, d);
        vrento::resource_registry::DescriptorBindingRecordState state;
        descriptor->Record(cmd, pipeline->pipeline->pipeline.layout, state);
        d.vkCmdDraw(command, 3, 1, 0, 0);
    } else {
        auto        source = context.texture(context.pass.uses[usize()].resource).unwrap();
        auto        target = context.texture(context.pass.uses[usize(1)].resource).unwrap();
        VkImageCopy region {};
        region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent                                 = { 4, 4, 1 };
        d.vkCmdCopyImage(command,
                         vk::native_texture(source.texture()).unwrap(),
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         vk::native_texture(target.texture()).unwrap(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1,
                         &region);
    }
    return Ok(empty {});
}
auto graph() -> rg::VulkanPassSet {
    rg::VulkanPassData data { .scope = 77 };
    for (unsigned i = 1; i <= 3; ++i) data.resources.push({ .id = { i, 1 } });
    rg::PassExecutionInfo upload { .id    = { 10 },
                                   .kind  = 1,
                                   .queue = { 1, rg::QueueClass::Transfer } };
    upload.uses.push(
        { { 1, 1 }, 1, rg::GraphAccess::Write, rg::PassUsage::Transfer, rg::PassStages::Transfer });
    rg::PassExecutionInfo draw { .id = { 20 }, .kind = 2 };
    draw.uses.push({ { 1, 1 }, 1 });
    draw.uses.push({ { 2, 1 },
                     1,
                     rg::GraphAccess::Write,
                     rg::PassUsage::ColorAttachment,
                     rg::PassStages::Color });
    draw.graphics =
        Some(rg::GraphicsDescription { .color = Some(rg::GraphAttachment { { 2, 1 } }) });
    rg::PassExecutionInfo copy { .id    = { 30 },
                                 .kind  = 3,
                                 .queue = { 1, rg::QueueClass::Transfer } };
    copy.uses.push(
        { { 2, 1 }, 1, rg::GraphAccess::Read, rg::PassUsage::Transfer, rg::PassStages::Transfer });
    copy.uses.push(
        { { 3, 1 }, 1, rg::GraphAccess::Write, rg::PassUsage::Transfer, rg::PassStages::Transfer });
    data.passes.push(rstd::move(copy));
    data.passes.push(rstd::move(draw));
    data.passes.push(rstd::move(upload));
    return vrento_test::make_pass_set(rstd::move(data)).unwrap();
}
auto queue_graph() -> rg::VulkanPassSet {
    auto               original = graph();
    rg::VulkanPassData data { .scope = original.scope() };
    for (const auto& resource : original.resources())
        data.resources.push(rg::GraphBindingRequirement(resource));
    for (const auto& pass : original.passes()) data.passes.push(pass.clone());
    rg::PassExecutionInfo consumer { .id = { 40 }, .kind = 4 };
    consumer.uses.push(
        { { 3, 1 }, 1, rg::GraphAccess::Read, rg::PassUsage::Transfer, rg::PassStages::Transfer });
    data.passes.push(rstd::move(consumer));
    return vrento_test::make_pass_set(rstd::move(data)).unwrap();
}
void queue_output(vk::DeviceSession session, vk::QueueParameters transfer, bool fail = false,
                  bool uncertain = false) {
    auto queue          = vk::TextureContext::create(session).unwrap();
    auto assets         = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    assets->fail_record = fail;
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned i = 1; i <= 3; ++i)
        backends.bind(i, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    backends.bind(4, 1, Box<dyn<vk::VulkanPassBackend>>::make(SurfaceClearBackend {})).unwrap();
    auto                     description = queue_graph();
    vk::GraphBindings        bindings(description.scope());
    Vec<tex::TextureViewRef> views;
    for (unsigned i = 1; i <= 3; ++i) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::Sampled | tex::Usage::ColorAttachment |
                      tex::Usage::TransferDestination | tex::Usage::TransferSource;
        auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        bindings
            .bind({ i, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
        views.push(rstd::move(view));
    }
    vk::GraphQueueAssignment assignments[] { { { 0, rg::QueueClass::Graphics },
                                               session.device().graphics_queue() },
                                             { { 1, rg::QueueClass::Transfer }, transfer } };
    Option<rstd::sync::Arc<dyn<vk::NativeQueueSubmit>>> submitter;
    if (uncertain)
        submitter =
            Some(rstd::sync::Arc<dyn<vk::NativeQueueSubmit>>::make(UncertainSubmit { 0, 2 }));
    auto program   = vk::GraphQueueProgram::prepare(
                         session,
                         description,
                         bindings,
                         backends,
                         slice<vk::GraphQueueAssignment>::from_raw_parts(assignments, usize(2)),
                         rstd::move(submitter))
                         .unwrap();
    auto submitted = program.execute();
    if (submitted.error() && ! fail && ! uncertain) {
        const auto& error = *submitted.error();
        fprintf(stderr,
                "queue graph stage=%u resource=%llu native=%d\n",
                unsigned(error.stage),
                static_cast<unsigned long long>(error.resource.value),
                error.texture ? int(error.texture->kind) : int(error.native));
    }
    if (uncertain) {
        rstd_assert(submitted.status() == vk::GraphSubmissionStatus::Uncertain);
        rstd_assert(submitted.accepted().len() == usize(1));
        rstd_assert(submitted.poll().is_err() && ! submitted.ready().unwrap());
        rstd_assert(views[usize(1)].texture().content_revision() == 0);
        rstd_assert(views[usize(2)].texture().content_revision() == 0);
        rstd_assert(session.wait_idle() == VK_SUCCESS);
        rstd_assert(session.validation_errors() == 0);
        return;
    }
    rstd_assert(submitted.status() ==
                (fail ? vk::GraphSubmissionStatus::Partial : vk::GraphSubmissionStatus::Submitted));
    rstd_assert(submitted.accepted().len() == usize(fail ? 1 : 4));
    if (fail) rstd_assert(! submitted.ready().unwrap());
    submitted.wait().unwrap();
    if (fail) {
        rstd_assert(views[usize(2)].texture().content_revision() == 0);
        auto recovery = queue.Begin().unwrap();
        recovery.commands()
            ->UseTexture({ views[usize()],
                           tex::Access::Sampled,
                           tex::Stages::Fragment,
                           false,
                           None(),
                           true })
            .unwrap();
        queue.wait(queue.Submit(rstd::move(recovery)).unwrap()).unwrap();
    } else
        rstd_assert(submitted.ready().unwrap());
    auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(fail ? 0 : 2)], 0, 0);
    for (usize i {}; i < pixels.len(); ++i)
        rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphQueue, SameQueue) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    queue_output(session, session.device().graphics_queue());
}
TEST(GraphQueue, UncertainSubmission) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    queue_output(session, session.device().graphics_queue(), false, true);
}
TEST(GraphQueue, CompletionSet) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    Option<vk::QueueParameters> other;
    for (auto queue : session.device().queues())
        if ((queue.flags & VK_QUEUE_COMPUTE_BIT) &&
            *queue.handle != *session.device().graphics_queue().handle) {
            other = Some(vk::QueueParameters(queue));
            break;
        }
    if (! other) GTEST_SKIP();
    auto queue  = vk::TextureContext::create(session).unwrap();
    auto assets = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &type;
    vvk::Semaphore semaphore;
    rstd_assert(session.device().handle().CreateSemaphore(info, semaphore) == VK_SUCCESS);
    auto handle = *semaphore;
    assets->gate =
        Some(queue
                 .ImportDependency(
                     handle, 1, rstd::sync::Arc<dyn<rstd::any::Any>>::make(rstd::move(semaphore)))
                 .unwrap());
    auto               original = queue_graph();
    rg::VulkanPassData data { .scope = original.scope() };
    for (const auto& resource : original.resources())
        data.resources.push(rg::GraphBindingRequirement(resource));
    for (const auto& pass : original.passes()) data.passes.push(pass.clone());
    data.passes.push({ .id = { 50 }, .kind = 5, .queue = { 3, rg::QueueClass::Compute } });
    vk::GraphBindings bindings(data.scope);
    for (unsigned i = 1; i <= 3; ++i) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::Sampled | tex::Usage::ColorAttachment |
                      tex::Usage::TransferSource | tex::Usage::TransferDestination;
        auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        bindings
            .bind({ i, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
    }
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 5; ++kind)
        if (kind == 4)
            backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(SurfaceClearBackend {}))
                .unwrap();
        else
            backends
                .bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
                .unwrap();
    vk::GraphQueueAssignment assignments[] {
        { { 0, rg::QueueClass::Graphics }, session.device().graphics_queue() },
        { { 1, rg::QueueClass::Transfer }, session.device().graphics_queue() },
        { { 3, rg::QueueClass::Compute }, *other }
    };
    auto described = vrento_test::make_pass_set(rstd::move(data)).unwrap();
    auto schedule  = described.compile().unwrap();
    auto program   = vk::GraphQueueProgram::prepare(
                         session,
                         rstd::move(described),
                         bindings,
                         backends,
                         slice<vk::GraphQueueAssignment>::from_raw_parts(assignments, usize(3)))
                         .unwrap();
    auto submitted = program.execute();
    rstd_assert(submitted.status() == vk::GraphSubmissionStatus::Submitted);
    for (usize batch {}; batch < schedule.batches.len(); ++batch)
        if (schedule.batches[batch].queue.value != 3)
            queue.wait(submitted.accepted()[batch]).unwrap();
    rstd_assert(! submitted.poll().unwrap() && ! submitted.ready().unwrap());
    auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkSignalSemaphoreKHR"));
    VkSemaphoreSignalInfo released { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    released.semaphore = handle;
    released.value     = 1;
    rstd_assert(signal(*session.device().handle(), &released) == VK_SUCCESS);
    submitted.wait().unwrap();
    rstd_assert(submitted.ready().unwrap() && session.validation_errors() == 0);
}
TEST(GraphQueue, SharedFamily) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    for (auto queue : session.device().queues())
        if (queue.family_index == session.device().graphics_queue().family_index &&
            queue.queue_index != 0) {
            queue_output(session, queue);
            return;
        }
    GTEST_SKIP();
}
TEST(GraphQueue, CrossFamily) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    for (auto queue : session.device().queues())
        if (queue.family_index != session.device().graphics_queue().family_index &&
            (queue.flags &
             (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))) {
            fprintf(stderr,
                    "graphics family=%u transfer family=%u index=%u flags=%u\n",
                    session.device().graphics_queue().family_index,
                    queue.family_index,
                    queue.queue_index,
                    queue.flags);
            queue_output(session, queue);
            queue_output(session, queue, true);
            return;
        }
    GTEST_SKIP();
}
TEST(GraphVulkan, BindPrepareRecordAndRetire) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto assets  = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned i = 1; i <= 3; ++i)
        backends.bind(i, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    auto                       original = graph();
    Vec<vk::PassExecutionInfo> declarations;
    for (const auto& pass : original.passes()) declarations.push(pass.clone());
    auto description =
        vk::VulkanPassSet::associate(
            vrento::fg::decode_frame_graph(
                vrento::fg::encode_frame_graph(original.frame_graph()).unwrap().as_slice())
                .unwrap(),
            rstd::move(declarations))
            .unwrap();
    for (unsigned frame {}; frame < 2; ++frame) {
        vk::GraphBindings        bindings(description.scope());
        Vec<tex::TextureViewRef> views;
        for (unsigned i = 1; i <= 3; ++i) {
            tex::TextureDesc desc;
            desc.extent = { 4, 4 };
            desc.usage  = tex::Usage::Sampled | tex::Usage::ColorAttachment |
                          tex::Usage::TransferDestination | tex::Usage::TransferSource;
            auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
            bindings
                .bind({ i, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
                .unwrap();
            views.push(rstd::move(view));
        }
        if (! frame) {
            vk::GraphBindings missing(description.scope());
            rstd_assert(vk::PreparedGraphProgram::prepare(session, description, missing, backends)
                            .unwrap_err()
                            .stage == vk::GraphExecutionStage::Binding);
            vk::GraphBindings scope(description.scope() + 1), generation(description.scope());
            for (unsigned i = 1; i <= 3; ++i) {
                scope.bind({ i, 1 }, bindings.resolve({ i, 1 }).unwrap()).unwrap();
                generation.bind({ i, i == 1 ? 2u : 1u }, bindings.resolve({ i, 1 }).unwrap())
                    .unwrap();
            }
            rstd_assert(vk::PreparedGraphProgram::prepare(session, description, scope, backends)
                            .unwrap_err()
                            .stage == vk::GraphExecutionStage::Binding);
            rstd_assert(
                vk::PreparedGraphProgram::prepare(session, description, generation, backends)
                    .unwrap_err()
                    .stage == vk::GraphExecutionStage::Binding);
            auto foreign_session = vk::DeviceSession::create({ .validation = true }).unwrap();
            auto foreign_queue   = vk::TextureContext::create(foreign_session).unwrap();
            auto foreign_view =
                foreign_queue
                    .CreateView(foreign_queue.CreateTexture(tex::TextureDesc { .extent = { 4, 4 } })
                                    .unwrap(),
                                {})
                    .unwrap();
            vk::GraphBindings foreign(description.scope());
            foreign
                .bind({ 1, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(
                          vk::GraphTextureBinding(foreign_view)))
                .unwrap();
            for (unsigned i = 2; i <= 3; ++i)
                foreign.bind({ i, 1 }, bindings.resolve({ i, 1 }).unwrap()).unwrap();
            rstd_assert(vk::PreparedGraphProgram::prepare(session, description, foreign, backends)
                            .unwrap_err()
                            .stage == vk::GraphExecutionStage::Device);
            rg::VulkanPassData unknown;
            unknown.scope = description.scope();
            for (const auto& resource : description.resources())
                unknown.resources.push(rg::GraphBindingRequirement(resource));
            for (const auto& pass : description.passes()) unknown.passes.push(pass.clone());
            unknown.passes[usize()].schema_version = 2;
            rstd_assert(vk::PreparedGraphProgram::prepare(
                            session,
                            vrento_test::make_pass_set(rstd::move(unknown)).unwrap(),
                            bindings,
                            backends)
                            .unwrap_err()
                            .stage == vk::GraphExecutionStage::Schema);
            rstd_assert(assets->live == 0);
            assets->fail_prepare = true;
            rstd_assert(vk::PreparedGraphProgram::prepare(session, description, bindings, backends)
                            .is_err());
            rstd_assert(assets->live == 0);
            assets->fail_prepare = false;
            assets->pending      = true;
            rstd_assert(vk::PreparedGraphProgram::prepare(session, description, bindings, backends)
                            .unwrap_err()
                            .stage == vk::GraphExecutionStage::Pending);
            rstd_assert(assets->live == 0 && views[usize(1)].texture().content_revision() == 0);
            assets->pending = false;
        }
        auto prepared = vk::PreparedGraphProgram::prepare(session, description, bindings, backends);
        if (prepared.is_err()) {
            auto error = prepared.unwrap_err();
            fprintf(stderr,
                    "graph prepare stage=%u pass=%llu resource=%llu\n",
                    unsigned(error.stage),
                    static_cast<unsigned long long>(error.pass.value),
                    static_cast<unsigned long long>(error.resource.value));
        }
        auto program = Some(rstd::move(prepared).unwrap());
        if (! frame) {
            assets->fail_record = true;
            {
                auto cancelled = queue.Begin().unwrap();
                auto receipt   = cancelled.receipt();
                rstd_assert(program->record(cancelled).is_err());
                rstd_assert(receipt.cancelled() && ! cancelled.valid());
                assets->fail_record = false;
                auto retry          = queue.Begin().unwrap();
                rstd_assert(program->record(retry).is_ok());
                retry.discard();
            }
            assets->fail_record = false;
        }
        auto work = queue.Begin().unwrap();
        program->record(work).unwrap();
        auto token = queue.Submit(rstd::move(work)).unwrap();
        program    = None();
        rstd_assert(assets->live == 3);
        queue.wait(token).unwrap();
        queue.Poll().unwrap();
        rstd_assert(assets->live == 0);
        auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(2)], 0, 0);
        rstd_assert(pixels.len() == usize(64));
        for (usize i {}; i < pixels.len(); ++i)
            rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    }
    rstd_assert(assets->pipelines.stats().creations == u64(1));
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, VersionedBindings) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue   = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc texture_description;
    texture_description.extent = { 4, 4 };
    texture_description.usage =
        tex::Usage::Sampled | tex::Usage::TransferSource | tex::Usage::TransferDestination;
    auto first  = queue.CreateView(queue.CreateTexture(texture_description).unwrap(), {}).unwrap();
    auto second = queue.CreateView(queue.CreateTexture(texture_description).unwrap(), {}).unwrap();
    vk::GraphBindings bindings(77);
    bindings
        .bind_version({ { 1, 1 }, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(first)))
        .unwrap();
    bindings
        .bind_version({ { 1, 1 }, 2 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(second)))
        .unwrap();
    vk::VulkanPassData    data { .scope = 77, .resources = bindings.descriptions() };
    vk::PassExecutionInfo upload { .id    = { 10 },
                                   .kind  = 1,
                                   .queue = { 1, vk::QueueClass::Transfer } };
    upload.uses.push(
        { { 1, 1 }, 1, vk::GraphAccess::Write, vk::PassUsage::Transfer, vk::PassStages::Transfer });
    data.passes.push(rstd::move(upload));
    data.passes.push(vrento_test::texture_copy_pass({ 20 }, { 1, 1 }, 1, { 1, 1 }, 2));
    auto described = vrento_test::make_pass_set(rstd::move(data)).unwrap();
    auto assets    = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.bind(1, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() })).unwrap();
    vk::register_texture_passes(backends).unwrap();
    auto prepared =
        vk::PreparedGraphProgram::prepare(session, described, bindings, backends).unwrap();
    auto cancelled = queue.Begin().unwrap();
    prepared.record(cancelled).unwrap();
    cancelled.discard();
    auto work = queue.Begin().unwrap();
    prepared.record(work).unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    rstd::array<tex::TextureViewRef, 2> versions { first, second };
    for (auto view : versions) {
        auto pixels = vrento_test::texture_vulkan::read(queue, view, 0, 0);
        for (usize i {}; i < pixels.len(); ++i)
            rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    }
    vk::GraphBindings aliased(77);
    aliased
        .bind_version({ { 1, 1 }, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(first)))
        .unwrap();
    aliased
        .bind_version({ { 1, 1 }, 2 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(first)))
        .unwrap();
    rstd_assert(vk::PreparedGraphProgram::prepare(session, described, aliased, backends)
                    .unwrap_err()
                    .stage == vk::GraphExecutionStage::Binding);
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, PlannedTextureAliases) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue   = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc desc;
    desc.extent = { 4, 4 };
    desc.usage  = tex::Usage::Sampled | tex::Usage::TransferDestination;
    auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    for (bool overlap : array<bool, 2> { false, true }) {
        vrento::fg::FrameGraphBuilder builder(78);
        auto first  = builder.create_resource({ .id = { 1, 1 }, .allocation_class = 1 }).unwrap();
        auto second = builder.create_resource({ .id = { 2, 1 }, .allocation_class = 1 }).unwrap();
        auto a = builder.add_pass({ 1 }).unwrap(), b = builder.add_pass({ 2 }).unwrap();
        builder.depends_on(b, a).unwrap();
        first  = builder.reserve_write(a, first).unwrap();
        second = builder.reserve_write(b, second).unwrap();
        Vec<vk::PassExecutionInfo> declarations;
        for (auto version : array<vrento::fg::ResourceVersion, 2> { first, second }) {
            vk::PassExecutionInfo pass { .id = version == first ? a : b, .kind = 1 };
            pass.uses.push({ version.resource,
                             version.version,
                             vk::GraphAccess::Write,
                             vk::PassUsage::Transfer,
                             vk::PassStages::Transfer });
            vrento_test::declare_pass(builder, pass).unwrap();
            declarations.push(rstd::move(pass));
        }
        if (overlap) {
            auto read = builder.add_pass({ 3 }).unwrap();
            auto pass = vrento_test::texture_sampled_pass(read, first.resource, first.version);
            pass.uses.push({ second.resource,
                             second.version,
                             vk::GraphAccess::Read,
                             vk::PassUsage::Sampled,
                             vk::PassStages::Fragment });
            vrento_test::declare_pass(builder, pass).unwrap();
            declarations.push(rstd::move(pass));
        }
        auto graph = vk::VulkanPassSet::associate(rstd::move(builder).finish().unwrap(),
                                                  rstd::move(declarations))
                         .unwrap();
        vk::GraphBindings bindings(78);
        for (auto version : array<vrento::fg::ResourceVersion, 2> { first, second })
            bindings
                .bind_version(
                    version,
                    rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
                .unwrap();
        auto result = bindings.validate(graph, session);
        if (overlap) {
            ASSERT_TRUE(result.is_err());
            EXPECT_EQ(result.unwrap_err().stage, vk::GraphExecutionStage::Binding);
        } else {
            EXPECT_TRUE(result.is_ok());
        }
        EXPECT_TRUE(bindings.validate_resources(graph.resources(), session).is_err());
    }
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(GraphVulkan, StandalonePasses) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto assets  = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 3; ++kind)
        backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    vk::GraphBindings        bindings(1);
    Vec<tex::TextureViewRef> views;
    for (unsigned id = 1; id <= 3; ++id) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::Sampled | tex::Usage::ColorAttachment |
                      tex::Usage::TransferDestination | tex::Usage::TransferSource;
        auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        bindings
            .bind({ id, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
        views.push(rstd::move(view));
    }
    auto description = graph();
    auto undefined   = queue.Begin().unwrap();
    vk::record_vulkan_pass(
        session, description.passes()[usize(2)].clone(), bindings, backends, undefined)
        .unwrap();
    auto load                  = description.passes()[usize(1)].clone();
    load.graphics->color->load = rg::AttachmentLoad::Load;
    load.uses.push({ { 2, 1 },
                     0,
                     rg::GraphAccess::Read,
                     rg::PassUsage::ColorAttachment,
                     rg::PassStages::Color });
    auto error = vk::record_vulkan_pass(session, rstd::move(load), bindings, backends, undefined)
                     .unwrap_err();
    rstd_assert(error.stage == vk::GraphExecutionStage::Access &&
                error.resource == rg::ResourceId({ 2, 1 }));
    rstd_assert(undefined.receipt().cancelled() &&
                views[usize(1)].texture().content_revision() == 0);
    auto cancelled = queue.Begin().unwrap();
    vk::record_vulkan_pass(
        session, description.passes()[usize(2)].clone(), bindings, backends, cancelled)
        .unwrap();
    auto unknown           = description.passes()[usize(1)].clone();
    unknown.schema_version = 2;
    rstd_assert(vk::record_vulkan_pass(session, rstd::move(unknown), bindings, backends, cancelled)
                    .unwrap_err()
                    .stage == vk::GraphExecutionStage::Schema);
    rstd_assert(cancelled.receipt().cancelled() && ! cancelled.valid());
    rstd_assert(assets->live == 0);
    auto work = queue.Begin().unwrap();
    for (unsigned index = 3; index; --index)
        vk::record_vulkan_pass(
            session, description.passes()[usize(index - 1)].clone(), bindings, backends, work)
            .unwrap();
    auto token = queue.Submit(rstd::move(work)).unwrap();
    rstd_assert(assets->live == 3);
    queue.wait(token).unwrap();
    queue.Poll().unwrap();
    rstd_assert(assets->live == 0);
    auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(2)], 0, 0);
    for (usize i {}; i < pixels.len(); ++i)
        rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    rg::VulkanPassData pinned { .scope = 1 };
    auto               resources = bindings.descriptions();
    auto               resource  = resources[usize(1)];
    resource.content_revision += 1;
    pinned.resources.push(rstd::move(resource));
    auto pinned_graph = vrento_test::make_pass_set(rstd::move(pinned)).unwrap();
    rstd_assert(bindings.validate(pinned_graph, session).unwrap_err().stage ==
                vk::GraphExecutionStage::Binding);
    auto discard                   = description.passes()[usize(1)].clone();
    discard.graphics->color->store = rg::AttachmentStore::Discard;
    auto discarded                 = queue.Begin().unwrap();
    vk::record_vulkan_pass(session, rstd::move(discard), bindings, backends, discarded).unwrap();
    auto complete = queue.Submit(rstd::move(discarded)).unwrap();
    queue.wait(complete).unwrap();
    queue.Poll().unwrap();
    auto after = queue.Begin().unwrap();
    auto lost_contents =
        vk::record_vulkan_pass(
            session, description.passes()[usize()].clone(), bindings, backends, after)
            .unwrap_err();
    rstd_assert(lost_contents.stage == vk::GraphExecutionStage::Access &&
                lost_contents.resource == rg::ResourceId({ 2, 1 }));
    rstd_assert(after.receipt().cancelled());
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, SurfaceOutput) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue   = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc source_desc;
    source_desc.extent = { 4, 4 };
    source_desc.usage =
        tex::Usage::TransferDestination | tex::Usage::TransferSource | tex::Usage::Sampled;
    auto source = queue.CreateView(queue.CreateTexture(source_desc).unwrap(), {}).unwrap();
    auto setup  = queue.Begin().unwrap();
    tex::TextureUpload upload;
    upload.target = source;
    upload.width = upload.height = 4;
    for (unsigned i {}; i < 64; ++i) upload.bytes.push(u8(i % 4 == 3 ? 255 : 41));
    setup.commands()->Upload(rstd::move(upload)).unwrap();
    queue.wait(queue.Submit(rstd::move(setup)).unwrap()).unwrap();
    queue.Poll().unwrap();
    for (unsigned mode {}; mode < 3; ++mode) {
        unsigned          size  = mode == 1 ? 6 : 4;
        auto              owner = rstd::sync::Arc<SurfaceImage>::make(session);
        VkImageCreateInfo image { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format    = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent    = { size, size, 1 };
        image.mipLevels = image.arrayLayers = 1;
        image.samples                       = VK_SAMPLE_COUNT_1_BIT;
        image.tiling                        = VK_IMAGE_TILING_OPTIMAL;
        image.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        owner->image = session.device().memory_allocator().create_image(image).unwrap();
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = owner->image.handle();
        view.format           = image.format;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        rstd_assert(session.device().handle().CreateImageView(view, owner->view) == VK_SUCCESS);
        vrento::FrameSurfaceLease lease;
        lease.identity             = { u64(7), u32(mode), u64(1) };
        lease.image.handle         = owner->image.handle();
        lease.image.view           = *owner->view;
        lease.image.extent         = image.extent;
        lease.format               = image.format;
        lease.initial_queue_family = lease.final_queue_family =
            session.device().graphics_queue().family_index;
        lease.final_layout = VK_IMAGE_LAYOUT_GENERAL;
        rstd_assert(lease.valid());
        vk::GraphBindings bindings(1);
        bindings
            .bind({ 1, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(source)))
            .unwrap();
        auto pin = rstd::sync::Arc<dyn<rstd::any::Any>>::make(owner.clone());
        bindings
            .bind({ 2, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(
                      vk::SurfaceGraphBinding(session, lease, pin.clone())))
            .unwrap();
        rg::VulkanPassData data { .scope = 1, .resources = bindings.descriptions() };
        data.resources[usize(1)].imported = false;
        rg::PassExecutionInfo output { .id   = { 1 },
                                       .kind = mode == 2 ? 9u : vk::surface_transfer_kind };
        if (mode != 2)
            output.uses.push({ { 1, 1 },
                               0,
                               rg::GraphAccess::Read,
                               rg::PassUsage::Transfer,
                               rg::PassStages::Transfer });
        output.uses.push({ { 2, 1 },
                           1,
                           rg::GraphAccess::Write,
                           mode == 2 ? rg::PassUsage::ColorAttachment : rg::PassUsage::Transfer,
                           mode == 2 ? rg::PassStages::Color : rg::PassStages::Transfer });
        if (mode == 2) {
            rg::GraphAttachment color { { 2, 1 } };
            color.clear     = { 0.25f, 0.5f, 0.75f, 1.0f };
            output.graphics = Some(rg::GraphicsDescription { .color = Some(color) });
        }
        data.passes.push(rstd::move(output));
        rg::PassExecutionInfo release { .id = { 2 }, .kind = vk::surface_release_kind };
        release.uses.push({ { 2, 1 },
                            1,
                            rg::GraphAccess::Read,
                            rg::PassUsage::Release,
                            mode == 2 ? rg::PassStages::Color : rg::PassStages::Transfer });
        data.passes.push(rstd::move(release));
        auto                   graph = vrento_test::make_pass_set(rstd::move(data)).unwrap();
        vk::VulkanPassBackends backends;
        backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(
            vk::SurfaceScopeBackend({ 2, 1 }, lease, pin.clone())));
        backends
            .bind(vk::surface_transfer_kind,
                  1,
                  Box<dyn<vk::VulkanPassBackend>>::make(vk::SurfaceTransferBackend { lease }))
            .unwrap();
        backends
            .bind(vk::surface_release_kind,
                  1,
                  Box<dyn<vk::VulkanPassBackend>>::make(vk::SurfaceReleaseBackend { lease }))
            .unwrap();
        backends.bind(9, 1, Box<dyn<vk::VulkanPassBackend>>::make(SurfaceClearBackend {})).unwrap();
        auto program =
            vk::PreparedGraphProgram::prepare(session, graph, bindings, backends).unwrap();
        auto cancelled = queue.Begin().unwrap();
        program.record(cancelled).unwrap();
        cancelled.discard();
        auto work = queue.Begin().unwrap();
        program.record(work).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        queue.Poll().unwrap();
        VkBufferCreateInfo buffer_info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size  = size * size * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto buffer       = session.device()
                                .memory_allocator()
                                .create_buffer(buffer_info, vvk::MemoryRequest::Readback())
                                .unwrap();
        auto read         = queue.Begin().unwrap();
        auto command      = vk::native_recording(read, session).unwrap();
        VkImageMemoryBarrier transition { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        transition.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        transition.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        transition.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        transition.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image                                                = lease.image.handle;
        transition.subresourceRange                                     = view.subresourceRange;
        const auto& d = session.device().handle().Dispatch();
        d.vkCmdPipelineBarrier(command,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               0,
                               nullptr,
                               0,
                               nullptr,
                               1,
                               &transition);
        VkBufferImageCopy copy {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent      = image.extent;
        d.vkCmdCopyImageToBuffer(command,
                                 lease.image.handle,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 buffer.handle(),
                                 1,
                                 &copy);
        VkBufferMemoryBarrier host { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        host.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer                                         = buffer.handle();
        host.size                                           = VK_WHOLE_SIZE;
        d.vkCmdPipelineBarrier(command,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT,
                               0,
                               0,
                               nullptr,
                               1,
                               &host,
                               0,
                               nullptr);
        read.retain(owner.clone());
        queue.wait(queue.Submit(rstd::move(read)).unwrap()).unwrap();
        queue.Poll().unwrap();
        auto memory = buffer.allocation();
        auto mapped = memory.map().unwrap();
        memory.invalidate().unwrap();
        auto     bytes = static_cast<const unsigned char*>(mapped.data());
        unsigned clear[] { 64, 128, 191, 255 };
        for (unsigned i {}; i < size * size * 4; ++i) {
            auto expected = mode == 2 ? clear[i % 4] : i % 4 == 3 ? 255u : 41u;
            rstd_assert(bytes[i] + 1 >= expected && bytes[i] <= expected + 1);
        }
    }
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, MergedScopes) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto assets  = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 3; ++kind)
        backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    vk::GraphBindings        bindings(77);
    Vec<tex::TextureViewRef> views;
    for (unsigned id = 1; id <= 3; ++id) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::ColorAttachment | tex::Usage::TransferSource |
                      tex::Usage::TransferDestination | tex::Usage::Sampled;
        auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        bindings
            .bind({ id, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
        views.push(rstd::move(view));
    }
    auto description = [&](rg::AttachmentLoad load) {
        auto               original = graph();
        rg::VulkanPassData data { .scope = 77 };
        for (auto resource : original.resources())
            data.resources.push(rg::GraphBindingRequirement(resource));
        auto first                      = original.passes()[usize(1)].clone();
        auto second                     = first.clone();
        second.id                       = { 21 };
        second.uses[usize(1)].version   = 2;
        second.graphics->color->version = 2;
        second.graphics->color->load    = load;
        if (load == rg::AttachmentLoad::Load)
            second.uses.push({ { 2, 1 },
                               1,
                               rg::GraphAccess::Read,
                               rg::PassUsage::ColorAttachment,
                               rg::PassStages::Color });
        auto copy                  = original.passes()[usize()].clone();
        copy.uses[usize()].version = 2;
        data.passes.push(original.passes()[usize(2)].clone());
        data.passes.push(rstd::move(first));
        data.passes.push(rstd::move(second));
        data.passes.push(rstd::move(copy));
        return vrento_test::make_pass_set(rstd::move(data)).unwrap();
    };
    for (unsigned variant {}; variant < 2; ++variant) {
        auto program =
            vk::PreparedGraphProgram::prepare(
                session,
                description(variant ? rg::AttachmentLoad::Clear : rg::AttachmentLoad::Load),
                bindings,
                backends)
                .unwrap();
        rstd_assert(program.scope_count() == usize(variant ? 2 : 1));
        if (! variant) {
            assets->fail_pass = 21;
            auto cancelled    = queue.Begin().unwrap();
            rstd_assert(program.record(cancelled).is_err() && cancelled.receipt().cancelled());
            rstd_assert(views[usize(1)].texture().content_revision() == 0);
            assets->fail_pass = 0;
        }
        auto work = queue.Begin().unwrap();
        program.record(work).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        queue.Poll().unwrap();
        auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(2)], 0, 0);
        for (usize i {}; i < pixels.len(); ++i)
            rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    }
    rstd_assert(assets->pipelines.stats().creations == u64(1));
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, DescriptorSnapshot) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto assets  = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    assets->freeze_descriptor = true;
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 3; ++kind)
        backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    vk::GraphBindings        bindings(77);
    Vec<tex::TextureViewRef> views;
    for (unsigned id = 1; id <= 4; ++id) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::ColorAttachment | tex::Usage::TransferSource |
                      tex::Usage::TransferDestination | tex::Usage::Sampled;
        auto view   = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        if (id < 4)
            bindings
                .bind({ id, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
                .unwrap();
        views.push(rstd::move(view));
    }
    auto               setup = queue.Begin().unwrap();
    tex::TextureUpload alternate;
    alternate.target = views[usize(3)];
    alternate.width = alternate.height = 4;
    for (unsigned i {}; i < 64; ++i) alternate.bytes.push(u8(i % 4 == 3 ? 255 : 87));
    setup.commands()->Upload(rstd::move(alternate)).unwrap();
    setup.commands()
        ->UseSampledView(views[usize(3)], assets->sampler, tex::Stages::Fragment)
        .unwrap();
    queue.wait(queue.Submit(rstd::move(setup)).unwrap()).unwrap();
    queue.Poll().unwrap();
    auto program = vk::PreparedGraphProgram::prepare(session, graph(), bindings, backends).unwrap();
    auto changed = vk::sampled_image(views[usize(3)], assets->sampler).unwrap();
    vrento::resource_registry::DescriptorImageBinding image { 0, changed.image, changed.layout };
    assets->descriptor_source
        ->UpdateImages(slice<vrento::resource_registry::DescriptorImageBinding>::from_raw_parts(
            &image, usize(1)))
        .unwrap();
    auto work = queue.Begin().unwrap();
    program.record(work).unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    queue.Poll().unwrap();
    auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(2)], 0, 0);
    for (usize i {}; i < pixels.len(); ++i)
        rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, UncertainSubmissionRetirement) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue  = Some(vk::TextureContext::create(
                           session, Some(Box<dyn<vk::NativeQueueSubmit>>::make(UncertainSubmit {})))
                           .unwrap());
    auto assets = rstd::sync::Arc<Assets>::make(session, queue->CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 3; ++kind)
        backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    vk::GraphBindings        bindings(77);
    Vec<tex::TextureViewRef> views;
    for (unsigned id = 1; id <= 3; ++id) {
        tex::TextureDesc desc;
        desc.extent = { 4, 4 };
        desc.usage  = tex::Usage::ColorAttachment | tex::Usage::TransferSource |
                      tex::Usage::TransferDestination | tex::Usage::Sampled;
        auto view   = queue->CreateView(queue->CreateTexture(desc).unwrap(), {}).unwrap();
        bindings
            .bind({ id, 1 },
                  rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
            .unwrap();
        views.push(rstd::move(view));
    }
    auto program =
        Some(vk::PreparedGraphProgram::prepare(session, graph(), bindings, backends).unwrap());
    auto work    = queue->Begin().unwrap();
    auto receipt = work.receipt();
    program->record(work).unwrap();
    auto error = queue->Submit(rstd::move(work)).unwrap_err();
    program    = None();
    rstd_assert(error.native == VK_ERROR_UNKNOWN && receipt.uncertain() && ! receipt.cancelled());
    rstd_assert(assets->live == 3 && views[usize(2)].texture().content_revision() == 0);
    rstd_assert(queue->Poll().unwrap_err().kind == tex::ErrorKind::Closed);
    rstd_assert(queue->Begin().unwrap_err().kind == tex::ErrorKind::Closed);
    queue = None();
    rstd_assert(assets->live == 0 && receipt.uncertain());
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, AttachmentVariantsAndRetention) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto assets  = rstd::sync::Arc<Assets>::make(session, queue.CreateSampler({}).unwrap());
    vk::VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}));
    for (unsigned kind = 1; kind <= 3; ++kind)
        backends.bind(kind, 1, Box<dyn<vk::VulkanPassBackend>>::make(Backend { assets.clone() }))
            .unwrap();
    struct Variant {
        vrento::TextureFormat format;
        unsigned              samples;
        bool                  green;
        unsigned              mask;
    };
    Variant variants[] {
        { vrento::TextureFormat::RGBA8, 1, false, 15 },
        { vrento::TextureFormat::RGBA8, 1, false, 15 },
        { vrento::TextureFormat::RGBA8_SRGB, 1, false, 15 },
        { vrento::TextureFormat::RGBA8, 4, false, 15 },
        { vrento::TextureFormat::RGBA8, 1, true, 15 },
        { vrento::TextureFormat::RGBA8, 1, true, 0 },
    };
    for (unsigned index {}; index < 6; ++index) {
        auto variant = variants[index];
        if (index >= 3) assets->recipe = None();
        assets->green      = variant.green;
        assets->write_mask = variant.mask;
        vk::GraphBindings        bindings(77);
        Vec<tex::TextureViewRef> views;
        for (unsigned id = 1; id <= 5; ++id) {
            tex::TextureDesc desc;
            desc.extent  = { 4, 4 };
            desc.format  = id == 1 ? vrento::TextureFormat::RGBA8 : variant.format;
            desc.samples = id == 2 || id == 5 ? variant.samples : 1;
            if (index == 2 && id == 2) desc.mip_levels = 3;
            desc.usage = id == 5 ? tex::Usage::DepthAttachment
                                 : tex::Usage::ColorAttachment | tex::Usage::TransferSource |
                                       tex::Usage::TransferDestination | tex::Usage::Sampled;
            if (id == 5) desc.format = vrento::TextureFormat::D32F;
            rstd_assert(queue.QuerySupport(desc).is_ok());
            tex::ViewDesc view_desc;
            if (id == 5) view_desc.range.aspect = tex::Aspect::Depth;
            auto view = queue.CreateView(queue.CreateTexture(desc).unwrap(), view_desc).unwrap();
            bindings
                .bind({ id, 1 },
                      rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
                .unwrap();
            views.push(rstd::move(view));
        }
        rg::VulkanPassData data { .scope = 77 };
        for (auto resource : bindings.descriptions()) data.resources.push(rstd::move(resource));
        auto original = graph();
        auto upload   = original.passes()[usize(2)].clone();
        auto draw     = original.passes()[usize(1)].clone();
        auto copy     = original.passes()[usize()].clone();
        draw.uses.push({ { 5, 1 },
                         1,
                         rg::GraphAccess::Write,
                         rg::PassUsage::DepthAttachment,
                         rg::PassStages::Depth,
                         { .aspect = 2 } });
        draw.graphics->depth                 = Some(rg::GraphAttachment { { 5, 1 } });
        draw.graphics->depth->clear[usize()] = 1;
        if (variant.samples > 1) {
            draw.uses.push({ { 4, 1 },
                             1,
                             rg::GraphAccess::Write,
                             rg::PassUsage::ColorAttachment,
                             rg::PassStages::Color });
            draw.graphics->resolve =
                Some(rg::GraphAttachment { { 4, 1 }, 1, rg::AttachmentLoad::Discard });
            draw.graphics->color->store = rg::AttachmentStore::Discard;
            copy.uses[usize()].resource = { 4, 1 };
        }
        data.passes.push(rstd::move(upload));
        data.passes.push(rstd::move(draw));
        data.passes.push(rstd::move(copy));
        auto described = vrento_test::make_pass_set(rstd::move(data));
        if (described.is_err()) {
            auto error = described.unwrap_err();
            std::fprintf(stderr,
                         "variant=%u graph error=%u pass=%llu resource=%llu\n",
                         index,
                         unsigned(error.kind),
                         static_cast<unsigned long long>(error.pass.value),
                         static_cast<unsigned long long>(error.resource.value));
        }
        auto description = rstd::move(described).unwrap();
        auto program     = Some(
            vk::PreparedGraphProgram::prepare(session, description, bindings, backends).unwrap());
        if (index == 3) {
            auto cancelled = queue.Begin().unwrap();
            program->record(cancelled).unwrap();
            cancelled.discard();
            rstd_assert(views[usize(3)].texture().content_revision() == 0);
        }
        auto        work = queue.Begin().unwrap();
        VkSemaphore held {};
        if (index == 5) {
            VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            info.pNext = &type;
            vvk::Semaphore semaphore;
            rstd_assert(session.device().handle().CreateSemaphore(info, semaphore) == VK_SUCCESS);
            held = *semaphore;
            auto gate =
                queue
                    .ImportDependency(
                        held, 1, rstd::sync::Arc<dyn<rstd::any::Any>>::make(rstd::move(semaphore)))
                    .unwrap();
            work.commands()->Wait(gate, tex::Stages::All).unwrap();
        }
        program->record(work).unwrap();
        auto token = queue.Submit(rstd::move(work)).unwrap();
        program    = None();
        rstd_assert(assets->live == 3);
        if (held) {
            rstd_assert(! token.poll().unwrap());
            assets->pipelines.SetLimits({ .max_entries = usize(), .unused_collections = u64() });
            rstd_assert(assets->pipelines.entryCount() == usize(1));
            auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
                session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                         "vkSignalSemaphoreKHR"));
            VkSemaphoreSignalInfo release { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
            release.semaphore = held;
            release.value     = 1;
            rstd_assert(signal && signal(*session.device().handle(), &release) == VK_SUCCESS);
        }
        auto creations = assets->pipelines.stats().creations;
        // The active pipeline and its layout must survive a zero cache budget.
        rstd_assert(assets->layouts.Resolve(assets->layout->handle).is_some());
        queue.wait(token).unwrap();
        queue.Poll().unwrap();
        assets->pipelines.PruneExpired();
        rstd_assert(assets->live == 0);
        if (held) rstd_assert(assets->pipelines.entryCount() == usize());
        auto pixels = vrento_test::texture_vulkan::read(queue, views[usize(2)], 0, 0);
        for (usize pixel {}; pixel < pixels.len(); ++pixel) {
            auto channel  = pixel.to_primitive() % 4;
            auto expected = ! variant.mask  ? 0u
                            : channel == 3  ? 255u
                            : variant.green ? (channel == 1 ? 255u : 0u)
                            : variant.format == vrento::TextureFormat::RGBA8_SRGB ? 112u
                                                                                  : 41u;
            auto actual   = u8(pixels[pixel]).to_primitive();
            rstd_assert(actual + 1 >= expected && actual <= expected + 1);
        }
        rstd_assert(creations == u64(index == 0 ? 1 : index));
        assets->pipelines.SetLimits({ .max_entries = usize(16), .unused_collections = u64(10) });
    }
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphVulkan, TextureTransfers) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             context = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc source_desc;
    source_desc.extent = { 4, 4 };
    source_desc.usage =
        tex::Usage::Sampled | tex::Usage::TransferSource | tex::Usage::TransferDestination;
    auto source      = context.CreateView(context.CreateTexture(source_desc).unwrap(), {}).unwrap();
    auto target_desc = source_desc;
    target_desc.mip_levels = 3;
    auto    target = context.CreateView(context.CreateTexture(target_desc).unwrap(), {}).unwrap();
    Vec<u8> bytes;
    for (unsigned i {}; i < 64; ++i) bytes.push(u8(i % 4 == 3 ? 255 : 41));
    auto upload = context.Begin().unwrap();
    upload.commands()
        ->Upload({ .target = source, .width = 4, .height = 4, .bytes = rstd::move(bytes) })
        .unwrap();
    auto ready = context.Submit(rstd::move(upload)).unwrap();
    context.wait(ready).unwrap();
    context.Poll().unwrap();
    vk::GraphBindings bindings(1);
    bindings
        .bind({ 1, 1 },
              rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(source)))
        .unwrap();
    bindings
        .bind({ 2, 1 },
              rstd::sync::Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(target)))
        .unwrap();
    rg::VulkanPassData data { .scope = 1, .resources = bindings.descriptions() };
    data.resources[usize(1)].imported       = false;
    data.resources[usize(1)].check_revision = false;
    data.passes.push(vrento_test::texture_copy_pass({ 1 }, { 1, 1 }, 0, { 2, 1 }, 1));
    data.passes.push(vrento_test::texture_mips_pass({ 2 }, { 2, 1 }, 1, 2, 3));
    data.passes.push(vrento_test::texture_sampled_pass({ 3 }, { 2, 1 }, 2, { .mip_count = 3 }));
    data.passes.push(vrento_test::texture_sampled_pass({ 4 }, { 1, 1 }, 0));
    data.dependencies.push({ { 1 }, { 4 } });
    auto                   graph = vrento_test::make_pass_set(rstd::move(data)).unwrap();
    vk::VulkanPassBackends backends;
    vk::register_texture_passes(backends).unwrap();
    auto program   = vk::PreparedGraphProgram::prepare(session, graph, bindings, backends).unwrap();
    auto cancelled = context.Begin().unwrap();
    program.record(cancelled).unwrap();
    cancelled.discard();
    rstd_assert(target.texture().content_revision() == 0);
    auto work = context.Begin().unwrap();
    program.record(work).unwrap();
    auto complete = context.Submit(rstd::move(work)).unwrap();
    context.wait(complete).unwrap();
    context.Poll().unwrap();
    for (unsigned mip {}; mip < 3; ++mip) {
        auto pixels = vrento_test::texture_vulkan::read(context, target, mip, 0);
        for (usize i {}; i < pixels.len(); ++i)
            rstd_assert(pixels[i] == u8(i.to_primitive() % 4 == 3 ? 255 : 41));
    }
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::graph_vulkan
