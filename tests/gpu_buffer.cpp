#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
import vrento.tests.shaders;
import vrento.tests.pass_set;

using namespace rstd::prelude;
using namespace vrento::vulkan;

namespace vrento_test::texture_vulkan
{
auto read(vrento::vulkan::TextureContext&, vrento::texture::TextureViewRef, unsigned, unsigned)
    -> rstd::vec::Vec<rstd::u8>;
}

namespace vrento_test::gpu_buffer
{
namespace tex      = vrento::texture;
namespace rg       = vrento::vulkan;
namespace registry = vrento::resource_registry;
struct ComputeAssets {
    DeviceSession                               session;
    GpuBuffer                                   first, second;
    registry::DescriptorSystem                  descriptors;
    Option<registry::PipelineLayoutResult>      layout;
    Option<registry::PreparedDescriptorBinding> compute_binding, draw_binding;
    vvk::Pipeline                               compute;
    bool                                        direct {};
    explicit ComputeAssets(DeviceSession owner)
        : session(owner),
          first(GpuBuffer::create(owner, 16,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                    .unwrap()),
          second(GpuBuffer::create(owner, 16,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                     .unwrap()) {
        auto&                           graphics = session.graphics_resources();
        registry::PipelineLayoutRequest request;
        DescriptorSetInfo               set;
        set.bindings.push({ 0,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            1,
                            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr });
        request.descriptor_sets.push(rstd::move(set));
        layout =
            Some(graphics.layouts.Ensure(session.device(), request, graphics.descriptors).unwrap());
        auto set_layout =
            graphics.descriptors.Resolve(layout->physical->descriptor_layouts[usize()]).unwrap();
        registry::DescriptorBufferBinding first_buffer {
            0, first.native(), 0, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        };
        registry::DescriptorBufferBinding second_buffer {
            0, second.native(), 0, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        };
        compute_binding =
            Some(descriptors
                     .Prepare(session.device(),
                              0,
                              *set_layout,
                              {},
                              slice<registry::DescriptorBufferBinding>::from_raw_parts(
                                  &first_buffer, usize(1)),
                              registry::DescriptorBindingReuse::Exclusive)
                     .unwrap());
        draw_binding = Some(descriptors
                                .Prepare(session.device(),
                                         0,
                                         *set_layout,
                                         {},
                                         slice<registry::DescriptorBufferBinding>::from_raw_parts(
                                             &second_buffer, usize(1)),
                                         registry::DescriptorBindingReuse::Exclusive)
                                .unwrap());
        VkShaderModuleCreateInfo shader { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader.codeSize = sizeof(shaders::buffer_compute.words);
        shader.pCode    = shaders::buffer_compute.words;
        vvk::ShaderModule module;
        rstd_assert(session.device().handle().CreateShaderModule(shader, module) == VK_SUCCESS);
        VkComputePipelineCreateInfo pipeline { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline.stage  = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            nullptr,
                            0,
                            VK_SHADER_STAGE_COMPUTE_BIT,
                            *module,
                            "main",
                            nullptr };
        pipeline.layout = *layout->physical->layout;
        rstd_assert(session.device().handle().CreateComputePipeline(pipeline, compute) ==
                    VK_SUCCESS);
    }
};
struct BufferPrepared {
    rstd::sync::Arc<ComputeAssets>           assets;
    rg::GraphSerial                          kind;
    Option<registry::PipelineResourceResult> pipeline;
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        auto               command = native_recording(context.submission, context.session).unwrap();
        const auto&        d       = context.session.device().handle().Dispatch();
        vvk::CommandBuffer commands(command, d);
        registry::DescriptorBindingRecordState descriptors;
        if (kind == 101) {
            d.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, *assets->compute);
            assets->compute_binding->Record(commands,
                                            *assets->layout->physical->layout,
                                            descriptors,
                                            VK_PIPELINE_BIND_POINT_COMPUTE);
            d.vkCmdDispatch(command, 1, 1, 1);
        } else if (kind == 102) {
            VkBufferCopy copy { 0, 0, 16 };
            d.vkCmdCopyBuffer(command, assets->first.native(), assets->second.native(), 1, &copy);
        } else if (kind == 103) {
            d.vkCmdBindPipeline(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline->pipeline->pipeline.handle);
            const auto& binding = assets->direct ? *assets->compute_binding : *assets->draw_binding;
            binding.Record(commands, *assets->layout->physical->layout, descriptors);
            d.vkCmdDraw(command, 3, 1, 0, 0);
        }
        return Ok(empty {});
    }
};
struct BufferBackend {
    rstd::sync::Arc<ComputeAssets> assets;
    auto                           Prepare(const rg::PassExecutionInfo&, const GraphPrepareContext&)
        -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>,
                  GraphExecutionError>;
};
} // namespace vrento_test::gpu_buffer
namespace rstd
{

template<>
struct Impl<vrento::vulkan::VulkanPassBackend, vrento_test::gpu_buffer::BufferBackend>
    : ImplBase<vrento_test::gpu_buffer::BufferBackend> {
    auto Prepare(const vrento::vulkan::PassExecutionInfo&   pass,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<vrento::vulkan::Preparation<sync::Arc<dyn<vrento::vulkan::VulkanPass>>>,
                  vrento::vulkan::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};
} // namespace rstd
namespace vrento_test::gpu_buffer
{
auto BufferBackend::Prepare(const rg::PassExecutionInfo& pass, const GraphPrepareContext& context)
    -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>, GraphExecutionError> {
    BufferPrepared result { assets.clone(), pass.kind };
    if (pass.kind == 103) {
        GraphicsPipeline defaults;
        defaults.toDefault();
        PipelineResourceRequest request;
        request.pipeline_layout            = assets->layout->handle;
        request.topology                   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        request.depth                      = defaults.depth;
        request.raster                     = defaults.raster;
        request.multisample                = defaults.multisample;
        request.color_blend.colorWriteMask = 15;
        auto add                           = [&](const auto& words, vrento::ShaderType type) {
            auto shader   = Box<ShaderSpv>::make();
            shader->stage = type;
            shader->spirv = vrento::ShaderCode::from(slice<rstd::uint32_t>::from_raw_parts(
                words, usize(sizeof(words) / sizeof(words[0]))));
            request.shader_stages.push(rstd::move(shader));
        };
        add(shaders::vertex.words, vrento::ShaderType::VERTEX);
        add(shaders::buffer_fragment.words, vrento::ShaderType::FRAGMENT);
        auto& graphics = context.session.graphics_resources();
        auto  recipe =
            graphics.pipelines.RegisterRecipe(rstd::move(request), graphics.layouts).unwrap();
        result.pipeline = graphics.pipelines.EnsureRecipe(context.session.device(),
                                                          recipe,
                                                          (*context.scope)->Info().signature,
                                                          graphics.passes,
                                                          graphics.layouts);
        graphics.pipelines.ReleaseRecipe(recipe);
        if (! result.pipeline) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    }
    return Ok(rstd::sync::Arc<dyn<VulkanPass>>::make(rstd::move(result)));
}
void compute_output(DeviceSession session, QueueParameters compute, QueueParameters transfer,
                    bool direct = false) {
    auto queue     = TextureContext::create(session).unwrap();
    auto assets    = rstd::sync::Arc<ComputeAssets>::make(session);
    assets->direct = direct;
    tex::TextureDesc desc;
    desc.extent = { 4, 4 };
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource;
    auto          output = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    GraphBindings bindings(1);
    bindings
        .bind({ 1, 1 },
              rstd::sync::Arc<dyn<GraphBinding>>::make(GraphGpuBufferBinding(assets->first)))
        .unwrap();
    bindings
        .bind({ 2, 1 },
              rstd::sync::Arc<dyn<GraphBinding>>::make(GraphGpuBufferBinding(assets->second)))
        .unwrap();
    bindings.bind({ 3, 1 }, rstd::sync::Arc<dyn<GraphBinding>>::make(GraphTextureBinding(output)))
        .unwrap();
    rg::VulkanPassData    data { .scope = 1, .resources = bindings.descriptions() };
    rg::PassExecutionInfo write { .id    = { 1 },
                                  .kind  = 101,
                                  .queue = { 1, rg::QueueClass::Compute } };
    rg::PassResourceUse   storage {
        { 1, 1 }, 1, rg::GraphAccess::Write, rg::PassUsage::Storage, rg::PassStages::Compute
    };
    storage.range.size = 16;
    write.uses.push(rg::PassResourceUse(storage));
    data.passes.push(rstd::move(write));
    rg::PassExecutionInfo copy { .id    = { 2 },
                                 .kind  = 102,
                                 .queue = { 2, rg::QueueClass::Transfer } };
    storage.access = rg::GraphAccess::Read;
    storage.usage  = rg::PassUsage::Transfer;
    storage.stages = rg::PassStages::Transfer;
    copy.uses.push(rg::PassResourceUse(storage));
    storage.resource = { 2, 1 };
    storage.access   = rg::GraphAccess::Write;
    copy.uses.push(rg::PassResourceUse(storage));
    if (! direct) data.passes.push(rstd::move(copy));
    rg::PassExecutionInfo draw { .id = { 3 }, .kind = 103 };
    storage.access = rg::GraphAccess::Read;
    storage.usage  = rg::PassUsage::Storage;
    storage.stages = rg::PassStages::Fragment;
    if (direct) storage.resource = { 1, 1 };
    draw.uses.push(rg::PassResourceUse(storage));
    draw.uses.push({ { 3, 1 },
                     1,
                     rg::GraphAccess::Write,
                     rg::PassUsage::ColorAttachment,
                     rg::PassStages::Color });
    draw.graphics =
        Some(rg::GraphicsDescription { .color = Some(rg::GraphAttachment { { 3, 1 } }) });
    data.passes.push(rstd::move(draw));
    rg::PassExecutionInfo consumer { .id = { 4 }, .kind = 104 };
    consumer.uses.push(
        { { 3, 1 }, 1, rg::GraphAccess::Read, rg::PassUsage::Transfer, rg::PassStages::Transfer });
    data.passes.push(rstd::move(consumer));
    VulkanPassBackends backends;
    backends.set_scopes(Box<dyn<GraphicsScopeBackend>>::make(GraphicsScopeCache {}));
    for (unsigned kind = 101; kind <= 104; ++kind)
        backends.bind(kind, 1, Box<dyn<VulkanPassBackend>>::make(BufferBackend { assets.clone() }))
            .unwrap();
    GraphQueueAssignment assignments[] { { { 0, rg::QueueClass::Graphics },
                                           session.device().graphics_queue() },
                                         { { 1, rg::QueueClass::Compute }, compute },
                                         { { 2, rg::QueueClass::Transfer }, transfer } };
    auto                 prepared = GraphQueueProgram::prepare(
        session,
        vrento_test::make_pass_set(rstd::move(data)).unwrap(),
        bindings,
        backends,
        slice<GraphQueueAssignment>::from_raw_parts(assignments, usize(3)));
    if (prepared.is_err()) {
        auto error = prepared.unwrap_err();
        rstd::io::eprintln("buffer prepare failed stage={} pass={} resource={} native={}",
                           unsigned(error.stage),
                           error.pass.value,
                           error.resource.value,
                           error.texture_error() ? int(error.texture_error()->kind)
                                                 : int(error.native_result()));
    }
    auto program   = prepared.unwrap();
    auto submitted = program.execute();
    if (submitted.error())
        rstd::io::eprintln("buffer graph failed stage={} resource={} error={}",
                           unsigned(submitted.error()->stage),
                           submitted.error()->resource.value,
                           submitted.error()->texture_error()
                               ? unsigned(submitted.error()->texture_error()->kind)
                               : 0);
    rstd_assert(submitted.status() == GraphSubmissionStatus::Submitted);
    submitted.wait().unwrap();
    auto pixels = vrento_test::texture_vulkan::read(queue, output, 0, 0);
    for (usize i {}; i < pixels.len(); ++i)
        rstd_assert(pixels[i] ==
                    u8(i.to_primitive() % 4 == 0 || i.to_primitive() % 4 == 3 ? 255 : 0));
    rstd_assert(assets->first.content_revision() == 1 &&
                assets->second.content_revision() == (direct ? 0 : 1));
    rstd_assert(session.validation_errors() == 0);
}
TEST(GraphQueue, ComputeBufferSameQueue) {
    auto session = DeviceSession::create({ .validation = true }).unwrap();
    if (! (session.device().graphics_queue().flags & VK_QUEUE_COMPUTE_BIT)) GTEST_SKIP();
    compute_output(session, session.device().graphics_queue(), session.device().graphics_queue());
}
TEST(GraphQueue, ComputeBufferSharedFamily) {
    auto session = DeviceSession::create({ .validation = true }).unwrap();
    for (auto compute : session.device().queues())
        for (auto transfer : session.device().queues())
            if ((compute.flags & VK_QUEUE_COMPUTE_BIT) &&
                compute.family_index == transfer.family_index &&
                compute.queue_index != transfer.queue_index) {
                rstd::io::eprintln("shared family={} queues={},{}",
                                   compute.family_index,
                                   compute.queue_index,
                                   transfer.queue_index);
                compute_output(session, compute, transfer);
                return;
            }
    GTEST_SKIP();
}
TEST(GraphQueue, ComputeBufferCrossFamily) {
    auto session = DeviceSession::create({ .validation = true }).unwrap();
    for (auto compute : session.device().queues())
        for (auto transfer : session.device().queues())
            if ((compute.flags & VK_QUEUE_COMPUTE_BIT) &&
                compute.family_index != transfer.family_index &&
                (transfer.flags &
                 (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) &&
                compute.family_index != session.device().graphics_queue().family_index) {
                rstd::io::eprintln("compute family={} transfer family={} graphics family={}",
                                   compute.family_index,
                                   transfer.family_index,
                                   session.device().graphics_queue().family_index);
                compute_output(session, compute, transfer, true);
                return;
            }
    GTEST_SKIP();
}
void fill(const DeviceSession& session, tex::Submission& work, const GpuBuffer& buffer,
          VkDeviceSize offset, VkDeviceSize size, unsigned value) {
    buffer
        .use(work,
             { offset,
               size,
               VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               true,
               false })
        .unwrap();
    session.device().handle().Dispatch().vkCmdFillBuffer(
        native_recording(work, session).unwrap(), buffer.native(), offset, size, value);
}
void host(tex::Submission& work, const GpuBuffer& buffer) {
    buffer.use(work, { 0, buffer.size(), VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT })
        .unwrap();
}
void check(const GpuBuffer& buffer, unsigned first, unsigned second) {
    auto bytes = buffer.readback().unwrap();
    for (unsigned i {}; i < 16; ++i) {
        auto expected = i < 8 ? first : second;
        rstd_assert(u8(bytes[usize(i)]).to_primitive() == ((expected >> ((i % 4) * 8)) & 255));
    }
}
TEST(GpuBuffer, OwnershipRanges) {
    auto                    session = DeviceSession::create({ .validation = true }).unwrap();
    auto                    source  = session.device().graphics_queue();
    Option<QueueParameters> destination;
    for (auto queue : session.device().queues())
        if (queue.family_index != source.family_index &&
            (queue.flags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) {
            destination = Some(queue);
            break;
        }
    if (! destination) GTEST_SKIP();
    auto first  = TextureContext::create_on_queue(session, source).unwrap();
    auto second = TextureContext::create_on_queue(session, *destination).unwrap();
    auto buffer =
        GpuBuffer::create(session,
                          16,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          vvk::MemoryRequest::Readback())
            .unwrap();
    auto work = first.Begin().unwrap();
    fill(session, work, buffer, 0, 16, 0x01010101);
    buffer.release(work, 0, 16, destination->family_index).unwrap();
    rstd_assert(
        buffer.use(work, { 0, 16, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT })
            .unwrap_err()
            .kind == tex::ErrorKind::Invalid);
    first.wait(first.Submit(rstd::move(work)).unwrap()).unwrap();
    auto acquired = second.Begin().unwrap();
    rstd_assert(
        buffer.use(acquired, { 0, 8, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT })
            .unwrap_err()
            .kind == tex::ErrorKind::Unsupported);
    buffer.use(acquired, { 0, 16, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT })
        .unwrap();
    host(acquired, buffer);
    second.wait(second.Submit(rstd::move(acquired)).unwrap()).unwrap();
    check(buffer, 0x01010101, 0x01010101);
    rstd_assert(buffer.content_revision() == 1 && session.validation_errors() == 0);
}
TEST(GpuBuffer, RangesCancellationAndPublication) {
    auto session = DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = TextureContext::create(session).unwrap();
    auto buffer =
        GpuBuffer::create(session,
                          16,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          vvk::MemoryRequest::Readback())
            .unwrap();
    auto cancelled = queue.Begin().unwrap();
    fill(session, cancelled, buffer, 0, 8, 0x11223344);
    rstd_assert(buffer.use(cancelled, { 8, 8, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT })
                    .is_err());
    rstd_assert(buffer.content_revision() == 0 && buffer.readback().is_err());
    cancelled.discard();
    rstd_assert(buffer.content_revision() == 0);
    auto work = queue.Begin().unwrap();
    fill(session, work, buffer, 0, 8, 0x11223344);
    fill(session, work, buffer, 8, 8, 0xaabbccdd);
    host(work, buffer);
    rstd_assert(buffer.content_revision() == 0);
    auto token = queue.Submit(rstd::move(work)).unwrap();
    rstd_assert(buffer.content_revision() == 1);
    queue.wait(token).unwrap();
    check(buffer, 0x11223344, 0xaabbccdd);
    auto read   = buffer.reserve_read().unwrap();
    auto frozen = queue.Begin().unwrap();
    rstd_assert(buffer
                    .use(frozen,
                         { 0,
                           16,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           true,
                           false })
                    .unwrap_err()
                    .kind == tex::ErrorKind::Busy);
    buffer
        .use(frozen,
             { 0, 16, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT },
             Some(read.clone()))
        .unwrap();
    queue.wait(queue.Submit(rstd::move(frozen)).unwrap()).unwrap();
    auto rewritten = queue.Begin().unwrap();
    fill(session, rewritten, buffer, 0, 16, 0x55667788);
    host(rewritten, buffer);
    queue.wait(queue.Submit(rstd::move(rewritten)).unwrap()).unwrap();
    check(buffer, 0x55667788, 0x55667788);
    rstd_assert(buffer.content_revision() == 2 && session.validation_errors() == 0);
}
TEST(GpuBuffer, UploadedRangeOverwrite) {
    auto          session = DeviceSession::create({ .validation = true }).unwrap();
    auto          queue   = TextureContext::create(session).unwrap();
    BufferManager buffers(session.device());
    rstd_assert(buffers.init());
    auto source = buffers.Allocate({ 16, 4, BufferUploadClass::Transfer }).unwrap();
    auto upload = [&](tex::Submission& work, unsigned value) {
        rstd::uint8_t bytes[16];
        for (auto& byte : bytes) byte = rstd::uint8_t(value);
        buffers.QueueWrite(source, slice<rstd::uint8_t>::from_raw_parts(bytes, usize(16))).unwrap();
        vvk::CommandBuffer    command(native_recording(work, session).unwrap(),
                                      session.device().handle().Dispatch());
        RecordedBufferUploads recorded;
        rstd_assert(buffers.RecordPendingUploads(command, recorded));
        work.retain(buffers.CommitRecordedUploads(rstd::move(recorded)).unwrap());
    };
    auto initial = queue.Begin().unwrap();
    upload(initial, 1);
    queue.wait(queue.Submit(rstd::move(initial)).unwrap()).unwrap();
    auto first  = GpuBuffer::create(
                      session, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto second = GpuBuffer::create(
                      session, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto copy   = [&](tex::Submission& work, const GpuBuffer& destination) {
        rg::PassResourceUse use {
            { 1, 1 }, 0, rg::GraphAccess::Read, rg::PassUsage::Transfer, rg::PassStages::Transfer
        };
        use.range.size = 16;
        GraphUploadedBufferBinding(session, source.clone()).Use(work, use).unwrap();
        destination
            .use(work,
                 { 0,
                   16,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   true,
                   false })
            .unwrap();
        VkBufferCopy region { source.offset(), 0, 16 };
        session.device().handle().Dispatch().vkCmdCopyBuffer(
            native_recording(work, session).unwrap(),
            source.buffer(),
            destination.native(),
            1,
            &region);
        host(work, destination);
    };
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &type;
    vvk::Semaphore semaphore;
    rstd_assert(session.device().handle().CreateSemaphore(info, semaphore) == VK_SUCCESS);
    auto handle = *semaphore;
    auto gate =
        queue
            .ImportDependency(
                handle, 1, rstd::sync::Arc<dyn<rstd::any::Any>>::make(rstd::move(semaphore)))
            .unwrap();
    auto old = queue.Begin().unwrap();
    old.commands()->Wait(gate, tex::Stages::All).unwrap();
    copy(old, first);
    auto old_token = queue.Submit(rstd::move(old)).unwrap();
    rstd_assert(! old_token.poll().unwrap());
    auto next = queue.Begin().unwrap();
    upload(next, 2);
    copy(next, second);
    auto token = queue.Submit(rstd::move(next)).unwrap();
    rstd_assert(! token.poll().unwrap());
    auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkSignalSemaphoreKHR"));
    VkSemaphoreSignalInfo released { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    released.semaphore = handle;
    released.value     = 1;
    rstd_assert(signal(*session.device().handle(), &released) == VK_SUCCESS);
    queue.wait(token).unwrap();
    check(first, 0x01010101, 0x01010101);
    check(second, 0x02020202, 0x02020202);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::gpu_buffer
