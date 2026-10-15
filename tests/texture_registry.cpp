#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;

import vrento.tests.pass_set;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace tex      = vrento::texture;
namespace vk       = vrento::vulkan;
namespace registry = vrento::resource_registry;

namespace vrento_test::texture_registry
{
struct Playback {
    struct State {
        vrento::VideoPlaybackSnapshot snapshot;
        f64                           current;
        Option<f64>                   duration;
    };
    mutable rstd::sync::Mutex<State> state;
    auto                             Snapshot() const -> vrento::VideoPlaybackSnapshot {
        return state.lock().unwrap()->snapshot;
    }
    void PublishTime(f64 time, Option<f64> length) const {
        auto locked      = state.lock().unwrap();
        locked->current  = time;
        locked->duration = length;
    }
};
struct SemaphoreOwner {
    vk::DeviceSession session;
    vvk::Semaphore    semaphore;
    explicit SemaphoreOwner(vk::DeviceSession value): session(rstd::move(value)) {}
};
} // namespace vrento_test::texture_registry

namespace rstd
{
template<>
struct Impl<vrento::VideoPlayback, Arc<vrento_test::texture_registry::Playback>>
    : ImplBase<Arc<vrento_test::texture_registry::Playback>> {
    auto Snapshot() const -> vrento::VideoPlaybackSnapshot { return self()->Snapshot(); }
    void PublishTime(f64 time, Option<f64> duration) const { self()->PublishTime(time, duration); }
};
} // namespace rstd

namespace vrento_test::texture_registry
{
TEST(RenderResourceSystem, OwnsPreparationAndRejectsStaleCommitOrCancel) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    registry::RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(created.unwrap()));
    auto begin = [&](u64 generation) {
        vrento::resource::ResourcePlan temporary { .generation = generation };
        return resources.BeginPreparePlan(temporary, {});
    };
    auto first = begin(u64(31)).unwrap();
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Preparing);
    EXPECT_TRUE(begin(u64(32)).is_err());
    EXPECT_EQ(resources.PrepareStatus().id, first);
    EXPECT_TRUE(resources.CommitPreparePlan(first).is_err());
    EXPECT_EQ(resources.ContinuePreparePlan(first).unwrap(),
              registry::ResourcePrepareProgress::Complete);
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Prepared);
    EXPECT_TRUE(resources.ContinuePreparePlan(first).is_err());
    resources.CommitPreparePlan(first).unwrap();
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Committed);
    EXPECT_EQ(resources.Prepared().Generation(), u64(31));

    auto second = begin(u64(32)).unwrap();
    EXPECT_NE(second, first);
    EXPECT_FALSE(resources.CancelPreparePlan(first));
    EXPECT_TRUE(resources.ContinuePreparePlan(first).is_err());
    EXPECT_TRUE(resources.CommitPreparePlan(first).is_err());
    EXPECT_EQ(resources.PrepareStatus().id, second);
    resources.ContinuePreparePlan(second).unwrap();
    EXPECT_EQ(resources.Prepared().Generation(), u64(32));
    EXPECT_TRUE(resources.CancelPreparePlan(second));
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Cancelled);
    EXPECT_EQ(resources.Prepared().Generation(), u64(31));
    EXPECT_TRUE(resources.CommitPreparePlan(second).is_err());

    vrento::resource::ResourcePlan invalid { .generation = u64(33) };
    invalid.DeclareTexture({ .kind = vrento::resource::TextureRequestKind::RenderTarget,
                             .name = "missing-definition"_Str },
                           vrento::resource::ResourceAccess::Write);
    auto rejected = resources.BeginPreparePlan(invalid, {});
    ASSERT_TRUE(rejected.is_err());
    auto reason = rstd::move(rejected).unwrap_err();
    ASSERT_EQ(reason.textures.len(), usize(1));
    EXPECT_TRUE(reason.textures[usize()].name.as_str() == "missing-definition"_str);
    auto logical = reason.textures[usize()].resource;
    EXPECT_TRUE(logical.Valid());
    auto saved = resources.PrepareStatus().clone();
    ASSERT_TRUE(saved.error);
    EXPECT_EQ(saved.error->textures[usize()].resource, logical);
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Failed);
    EXPECT_TRUE(resources.PrepareStatus().error);
    EXPECT_EQ(resources.Prepared().Generation(), u64(31));
    resources.AbortPreparePlan();
    EXPECT_EQ(saved.error->textures[usize()].resource, logical);
    EXPECT_TRUE(reason.textures[usize()].name.as_str() == "missing-definition"_str);
    auto retry = begin(u64(34)).unwrap();
    resources.ContinuePreparePlan(retry).unwrap();
    resources.CommitPreparePlan(retry).unwrap();
    EXPECT_EQ(resources.Prepared().Generation(), u64(34));
    resources.Reset();
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Idle);
}

TEST(RegistryTextureContext, SlowGpuBoundsPendingUploads) {
    auto                      session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto                      context = vk::TextureContext::create(session).unwrap();
    auto                      gate    = Arc<SemaphoreOwner>::make(session);
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
    rstd_assert(session.device().handle().CreateSemaphore(info, gate->semaphore) == VK_SUCCESS);
    auto work = context.Begin().unwrap();
    dyn<vk::NativeSubmission>::from_ref(context)
        ->AttachWait(work,
                     { *session.device().handle(),
                       *gate->semaphore,
                       vk::NativeSemaphoreKind::Timeline,
                       1,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       Arc<dyn<rstd::any::Any>>::make(gate.clone()) })
        .unwrap();
    auto                                blocked = context.Submit(rstd::move(work)).unwrap();
    registry::RegistryTextureContext    textures(rstd::move(context));
    vrento::resource::TextureDefinition definition;
    definition.width = definition.height = i32(16);
    definition.usage                     = vrento::resource::TextureUsage::Sampled |
                                           vrento::resource::TextureUsage::TransferDestination;
    for (usize batch {}; batch < textures.MaxUploadBatches; ++batch) {
        for (usize item {}; item < textures.MaxPendingContents; ++item)
            textures.AllocateTexture(definition, true).unwrap();
        rstd_assert(textures.PendingCount() == textures.MaxPendingContents);
        textures.Flush().unwrap();
    }
    rstd_assert(textures.InFlightCount() == textures.MaxUploadBatches);
    rstd_assert(! blocked.poll().unwrap());
    auto release =
        rstd::thread::spawn([gate = gate.clone()] {
            rstd::thread::sleep(rstd::time::Duration::from_millis(u64(30)));
            auto& device = gate->session.device();
            auto  signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
                device.handle().Dispatch().vkGetDeviceProcAddr(*device.handle(),
                                                               "vkSignalSemaphoreKHR"));
            rstd_assert(signal);
            VkSemaphoreSignalInfo signal_info { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
            signal_info.semaphore = *gate->semaphore;
            signal_info.value     = 1;
            rstd_assert(signal(*device.handle(), &signal_info) == VK_SUCCESS);
        }).unwrap();
    textures.AllocateTexture(definition, true).unwrap();
    rstd::move(release).join().unwrap();
    rstd_assert(blocked.poll().unwrap());
    rstd_assert(textures.InFlightCount() < textures.MaxUploadBatches);
    rstd_assert(textures.PendingCount() == usize(1));
    {
        auto cancelled = textures.Context().Begin().unwrap();
        rstd_assert(textures.RecordPendingUploads(cancelled));
    }
    rstd_assert(textures.PendingCount() == usize(1));
    textures.CancelRecording();
    textures.Flush().unwrap();
    textures.WaitForUploads(5'000'000'000).unwrap();
    rstd_assert(textures.InFlightCount() == usize());
    rstd_assert(session.validation_errors() == 0);
}

auto PixelImage() -> Arc<vrento::Image> {
    auto image           = Arc<vrento::Image>::make();
    image->header.format = vrento::TextureFormat::RGBA8;
    vrento::Image::Slot slot;
    slot.width = slot.height = 2;
    slot.mipmaps.push(vrento::ImageData {
        2, 2, isize(16), vrento::ImageDataPtr(new rstd::uint8_t[16] {}, [](rstd::uint8_t* data) {
            delete[] data;
        }) });
    image->slots.push(rstd::move(slot));
    return image;
}

TEST(RegistryTextureContext, ImageIdentityReusesPreparedAllocationAndCancelledUpload) {
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                             session = created.unwrap();
    registry::RegistryTextureContext textures(vk::TextureContext::create(session).unwrap());
    auto                             preload = PixelImage();
    ASSERT_TRUE(textures.TryPrepareImage(preload.clone()).unwrap());
    auto adopted = textures.CreateImportedTexture(preload.clone(), None()).unwrap();
    EXPECT_FALSE(adopted.upload);
    EXPECT_EQ(textures.PendingCount(), usize());
    auto image  = PixelImage();
    auto first  = textures.CreateImportedTexture(image.clone(), None()).unwrap();
    auto reused = textures.CreateImportedTexture(image.clone(), None()).unwrap();
    EXPECT_TRUE(Arc<registry::TextureAllocation>::ptr_eq(first.allocation, reused.allocation));
    EXPECT_EQ(textures.PendingCount(), usize(1));
    EXPECT_EQ(first.allocation->InitialReadiness().unwrap(), registry::ImageReadiness::Pending);
    {
        auto cancelled = textures.Context().Begin().unwrap();
        auto updates   = textures.PreparePendingUploads(cancelled).unwrap();
        ASSERT_EQ(updates.len(), usize(1));
        EXPECT_EQ(updates[usize()].Readiness().unwrap(), registry::ImageReadiness::Pending);
        updates[usize()].Record(cancelled, session).unwrap();
        EXPECT_EQ(updates[usize()].Readiness(Some(cancelled.receipt())).unwrap(),
                  registry::ImageReadiness::Recorded);
        EXPECT_TRUE(updates[usize()].clone().Record(cancelled, session).is_err());
        auto foreign         = textures.Context().Begin().unwrap();
        auto foreign_receipt = foreign.receipt();
        auto token           = textures.Context().Submit(rstd::move(foreign)).unwrap();
        EXPECT_FALSE(textures.ConfirmSubmitted(foreign_receipt, token));
        EXPECT_EQ(textures.PendingCount(), usize(1));
        EXPECT_EQ(first.allocation->InitialReadiness(Some(cancelled.receipt())).unwrap(),
                  registry::ImageReadiness::Recorded);
    }
    EXPECT_EQ(first.allocation->InitialReadiness().unwrap(), registry::ImageReadiness::Pending);
    textures.CancelRecording();
    textures.Flush().unwrap();
    auto again = textures.CreateImportedTexture(image.clone(), None()).unwrap();
    EXPECT_TRUE(Arc<registry::TextureAllocation>::ptr_eq(first.allocation, again.allocation));
    EXPECT_FALSE(again.upload);
    auto revision = textures.CreateImportedTexture(PixelImage(), None()).unwrap();
    EXPECT_FALSE(Arc<registry::TextureAllocation>::ptr_eq(first.allocation, revision.allocation));
    textures.Flush().unwrap();
    textures.WaitForUploads(5'000'000'000).unwrap();
    EXPECT_EQ(first.allocation->InitialReadiness().unwrap(), registry::ImageReadiness::Complete);
    EXPECT_EQ(session.validation_errors(), 0u);
}

TEST(RegistryTextureContext, UploadBeforeGraphKeepsDescriptionsAndRequiresSameSubmission) {
    namespace fg = vrento::fg;
    auto created = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                                session = created.unwrap();
    registry::RegistryTextureContext    textures(vk::TextureContext::create(session).unwrap());
    vrento::resource::TextureDefinition source_desc;
    source_desc.width = source_desc.height = i32(2);
    source_desc.usage                      = vrento::resource::TextureUsage::Sampled |
                                             vrento::resource::TextureUsage::TransferSource |
                                             vrento::resource::TextureUsage::TransferDestination;
    auto source                            = textures.AllocateTexture(source_desc, true).unwrap();
    vrento::resource::TextureDefinition target_desc;
    target_desc.width = target_desc.height = i32(2);
    target_desc.usage                      = vrento::resource::TextureUsage::Sampled |
                                             vrento::resource::TextureUsage::TransferDestination;
    auto                  target           = textures.AllocateTexture(target_desc, false).unwrap();
    fg::FrameGraphBuilder builder(91);
    auto input    = builder.import_resource({ .id = { 1, 91 }, .spec = 1, .storage = 1 }).unwrap();
    auto output   = builder.import_resource({ .id = { 2, 91 }, .spec = 2, .storage = 2 }).unwrap();
    auto copy     = vk::declare_texture_copy(builder, { 1 }, input, output).unwrap();
    auto revision = copy.description.revision();
    auto graph    = rstd::move(builder).finish().unwrap();
    vk::Renderer renderer(session);
    vk::register_texture_factories(renderer).unwrap();
    Vec<vk::PassInput> descriptions;
    descriptions.push(copy.description.clone());
    renderer.compile(graph, rstd::move(descriptions)).unwrap();
    vk::GraphBindings bindings(graph.scope());
    bindings
        .bind_version(input,
                      Arc<dyn<vk::GraphBinding>>::make(
                          vk::GraphTextureBinding(source.allocation->Slots()[usize()].view)))
        .unwrap();
    bindings
        .bind_version(copy.output,
                      Arc<dyn<vk::GraphBinding>>::make(
                          vk::GraphTextureBinding(target.allocation->Slots()[usize()].view)))
        .unwrap();
    auto work = textures.Context().Begin().unwrap();
    ASSERT_TRUE(textures.RecordPendingUploads(work));
    auto frame =
        renderer.compiled()
            ->instantiate(bindings,
                          {},
                          Box<dyn<vk::GraphicsScopeBackend>>::make(vk::GraphicsScopeCache {}),
                          true,
                          &work)
            .unwrap()
            .take_ready();
    {
        auto unrelated = textures.Context().Begin().unwrap();
        EXPECT_EQ(frame.record(unrelated).unwrap_err().stage, vk::GraphExecutionStage::Binding);
    }
    frame.record(work).unwrap();
    auto receipt = work.receipt();
    auto token   = textures.Context().Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(textures.ConfirmSubmitted(receipt, token));
    EXPECT_EQ(graph.passes().len(), usize(1));
    EXPECT_EQ(copy.description.revision(), revision);
    EXPECT_EQ(renderer.statistics().graph_compiles, 1u);
    textures.Context().wait(token).unwrap();
    EXPECT_EQ(session.validation_errors(), 0u);
}

TEST(RenderResourceSystem, PreparedUploadsRequireTheirFrame) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    registry::RenderResourceSystem resources;
    rstd_assert(resources.Initialize(session));
    auto foreign   = vk::TextureContext::create(session).unwrap();
    auto first     = resources.BeginFrame().unwrap();
    auto unrelated = foreign.Begin().unwrap();
    rstd_assert(resources.PreparePendingUploads(unrelated).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Invalid);
    auto cancelled = resources.PreparePendingUploads(first).unwrap();
    rstd_assert(cancelled.Record(unrelated).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Invalid);
    first.discard();
    auto retry = resources.BeginFrame().unwrap();
    rstd_assert(cancelled.Record(retry).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Invalid);
    auto omitted = resources.PreparePendingUploads(retry).unwrap();
    rstd_assert(resources.SubmitFrame(rstd::move(retry)).unwrap_err().kind ==
                tex::ErrorKind::Invalid);
    auto final = resources.BeginFrame().unwrap();
    auto ready = resources.PreparePendingUploads(final).unwrap();
    ready.Record(final).unwrap();
    rstd_assert(ready.Record(final).unwrap_err().kind ==
                vrento::resource::ResourceErrorKind::Invalid);
    resources.TextureContext().wait(resources.SubmitFrame(rstd::move(final)).unwrap()).unwrap();
    resources.Reset();
    rstd_assert(session.validation_errors() == 0);
}

TEST(RenderResourceSystem, BufferUploadSnapshotMergesWritesAndCancelsReservation) {
    auto              session = vk::DeviceSession::create({ .validation = true }).unwrap();
    vk::BufferManager buffers(session.device());
    rstd_assert(buffers.init());
    auto allocation = buffers.Allocate({ 32, 4, vk::BufferUploadClass::Vertex }).unwrap();
    array<rstd::uint8_t, 16> bytes {};
    buffers.QueueWrite(allocation, bytes.as_slice()).unwrap();
    buffers.QueueWrite(allocation, bytes.as_slice(), 8).unwrap();
    {
        vk::RecordedBufferUploads prepared;
        rstd_assert(buffers.PreparePendingUploads(prepared));
        auto writes = prepared.Writes();
        ASSERT_EQ(writes.len(), usize(1));
        EXPECT_EQ(writes[usize()].offset, 0u);
        EXPECT_EQ(writes[usize()].size, 24u);
        EXPECT_EQ(buffers.statistics().recorded_copies[unsigned(vk::BufferUploadClass::Vertex)],
                  0u);
        EXPECT_FALSE(buffers.QueueWrite(allocation, bytes.as_slice()));
    }
    rstd_assert(buffers.QueueWrite(allocation, bytes.as_slice(), 16));
    auto                      queue   = vk::TextureContext::create(session).unwrap();
    auto                      work    = queue.Begin().unwrap();
    auto                      command = vk::native_command_recording(work, session).unwrap();
    vk::RecordedBufferUploads prepared;
    rstd_assert(buffers.PreparePendingUploads(prepared));
    auto writes = prepared.Writes();
    ASSERT_EQ(writes.len(), usize(1));
    EXPECT_EQ(writes[usize()].size, 32u);
    rstd_assert(buffers.RecordUploads(command, prepared));
    work.retain(buffers.CommitRecordedUploads(rstd::move(prepared)).unwrap());
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    rstd_assert(session.validation_errors() == 0);
}

TEST(RenderResourceSystem, CancelledBufferUploadCanBeReplayed) {
    auto              session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto              context = vk::TextureContext::create(session).unwrap();
    vk::BufferManager buffers(session.device());
    rstd_assert(buffers.init());
    auto allocation = buffers.Allocate({ 16, 4, vk::BufferUploadClass::Vertex }).unwrap();
    array<rstd::uint8_t, 16> bytes {};
    auto                     ticket = buffers.QueueWrite(allocation, bytes.as_slice()).unwrap();
    Option<vk::BufferUploadBatchLease> held;
    {
        auto                      work    = context.Begin().unwrap();
        auto                      command = vk::native_command_recording(work, session).unwrap();
        vk::RecordedBufferUploads recorded;
        rstd_assert(buffers.RecordPendingUploads(command, recorded));
        held = buffers.CommitRecordedUploads(rstd::move(recorded));
        rstd_assert(held.is_some());
    }
    rstd_assert(buffers.RestoreUploads(rstd::move(*held)));
    rstd_assert(buffers.HasPendingUploads());
    auto                      retry   = context.Begin().unwrap();
    auto                      command = vk::native_command_recording(retry, session).unwrap();
    vk::RecordedBufferUploads recorded;
    rstd_assert(buffers.RecordPendingUploads(command, recorded));
    auto lease = buffers.CommitRecordedUploads(rstd::move(recorded)).unwrap();
    rstd_assert(lease.Tickets().len() == usize(1));
    rstd_assert(lease.Tickets()[usize()] == ticket);
    retry.retain(rstd::move(lease));
    context.wait(context.Submit(rstd::move(retry)).unwrap()).unwrap();
    context.Poll().unwrap();
    rstd_assert(! buffers.HasPendingUploads());
    rstd_assert(session.validation_errors() == 0);
}

TEST(RenderResourceSystem, LocalOutputExportsAndReusesSlots) {
    array<vk::Extension, 3> extensions {
        vk::Extension { true, "VK_KHR_external_memory_fd"_str },
        vk::Extension { true, "VK_KHR_external_semaphore_fd"_str },
        vk::Extension { true, "VK_EXT_external_memory_dma_buf"_str },
    };
    auto                           session = vk::DeviceSession::create(
                                                 { .device_extensions = extensions.as_slice(), .validation = true })
                                                 .unwrap();
    registry::RenderResourceSystem resources;
    rstd_assert(resources.Initialize(session));
    auto& context = resources.TextureContext();
    auto  result = resources.CreateLocalSwapchain(session.device(), 32, 32, VK_IMAGE_TILING_LINEAR);
    if (result.is_none()) {
        rstd::io::eprintln("linear DMA-BUF output unavailable on this device");
        GTEST_SKIP();
    }
    auto                        output = rstd::move(*result);
    auto                        signal = Arc<SemaphoreOwner>::make(session);
    VkExportSemaphoreCreateInfo export_info { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo semaphore { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &export_info };
    rstd_assert(session.device().handle().CreateSemaphore(semaphore, signal->semaphore) ==
                VK_SUCCESS);
    for (unsigned frame {}; frame < 4; ++frame) {
        auto acquired = output->AsSwapchain().acquireRenderTarget();
        rstd_assert(acquired.status == vrento::FrameSurfaceAcquireStatus::Acquired);
        rstd_assert(output->AsSwapchain().acquireRenderTarget().status ==
                    vrento::FrameSurfaceAcquireStatus::Busy);
        auto work  = context.Begin().unwrap();
        auto owner = Arc<dyn<rstd::any::Any>>::make(output.clone());
        vk::recordAcquire(
            work,
            session,
            acquired.lease,
            rstd::move(owner),
            vk::native_access({ tex::Access::TransferDestination, tex::Stages::Transfer }))
            .unwrap();
        auto              command = vk::native_command_recording(work, session).unwrap();
        VkClearColorValue clear {};
        clear.float32[0] = float(frame) / 4.0f;
        clear.float32[3] = 1.0f;
        command.ClearColorImage(acquired.lease.image.handle,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &clear,
                                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
        vk::recordRelease(work, session, acquired.lease).unwrap();
        context
            .AttachSignal(work,
                          { *session.device().handle(),
                            *signal->semaphore,
                            vk::NativeSemaphoreKind::Binary,
                            0,
                            Arc<dyn<rstd::any::Any>>::make(signal.clone()) })
            .unwrap();
        context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
        context.Poll().unwrap();
        VkSemaphoreGetFdInfoKHR query { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
        query.semaphore  = *signal->semaphore;
        query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd           = -1;
        rstd_assert(session.device().handle().GetSemaphoreFdKHR(query, &fd) == VK_SUCCESS);
        rstd_assert(acquired.completion.Submit(fd).status ==
                    vrento::FrameSurfaceCompletionStatus::Submitted);
        auto consumed = output->AsSwapchain().eatFrame();
        rstd_assert(consumed && consumed->fd >= 0);
        rstd_assert(consumed->id() == i32(acquired.lease.identity.image_index.to_primitive()));
        auto owned_fd =
            rstd::os::fd::OwnedFd::from_raw_fd(output->AsSwapchain().takeLastFrameSyncFd());
    }
    auto abandoned = output->AsSwapchain().acquireRenderTarget();
    rstd_assert(abandoned.completion.Abort().status ==
                vrento::FrameSurfaceCompletionStatus::Aborted);
    rstd_assert(session.validation_errors() == 0);
}

TEST(MediaRegistry, ContinuousPlaybackKeepsStableTexture) {
    auto path = rstd::env::var("VR_TEST_VIDEO"_str);
    if (path.is_err()) GTEST_SKIP();
    auto                     width  = rstd::env::var("VR_TEST_VIDEO_WIDTH"_str).unwrap();
    auto                     height = rstd::env::var("VR_TEST_VIDEO_HEIGHT"_str).unwrap();
    auto                     hwdec  = rstd::env::var("VR_TEST_VIDEO_HWDEC"_str).unwrap();
    array<vk::Extension, 13> extensions {
        vk::Extension { true, "VK_KHR_external_memory_fd"_str },
        vk::Extension { true, "VK_KHR_external_semaphore_fd"_str },
        vk::Extension { false, "VK_KHR_synchronization2"_str },
        vk::Extension { false, "VK_EXT_external_memory_dma_buf"_str },
        vk::Extension { false, "VK_KHR_image_format_list"_str },
        vk::Extension { false, "VK_EXT_image_drm_format_modifier"_str },
        vk::Extension { false, "VK_EXT_queue_family_foreign"_str },
        vk::Extension { false, "VK_KHR_video_queue"_str },
        vk::Extension { false, "VK_KHR_video_decode_queue"_str },
        vk::Extension { false, "VK_KHR_video_decode_h264"_str },
        vk::Extension { false, "VK_KHR_video_decode_h265"_str },
        vk::Extension { false, "VK_EXT_descriptor_buffer"_str },
        vk::Extension { false, "VK_EXT_shader_object"_str },
    };
    auto session = vk::DeviceSession::create({ .device_extensions = extensions.as_slice(),
                                               .api_version       = VK_API_VERSION_1_3,
                                               .validation        = true })
                       .unwrap();
    auto context = vk::TextureContext::create(session).unwrap();
    vk::MediaRegistry media(session, { hwdec.clone(), {} });
    auto              physical_path = rstd::path::PathBuf::from(path->as_str());
    auto              file          = rstd::fs::File::open(physical_path.as_path()).unwrap();
    auto              size          = file.metadata().unwrap().len();
    auto              source        = rstd::io::SharedReadAt::make(rstd::move(file));
    vrento::Image     image;
    image.header.kind = vrento::ImageKind::Video;
    image.key         = "playback"_Str;
    vrento::Image::Slot slot;
    slot.width  = rstd::from_str<i32>(width.as_str()).unwrap().to_primitive();
    slot.height = rstd::from_str<i32>(height.as_str()).unwrap().to_primitive();
    vrento::ImageData data;
    data.width        = slot.width;
    data.height       = slot.height;
    data.video_source = Some(rstd::io::ReadRange::make(rstd::move(source), u64(), size).unwrap());
    slot.mipmaps.push(rstd::move(data));
    image.slots.push(rstd::move(slot));
    auto control     = Arc<Playback>::make();
    auto playback    = Arc<dyn<vrento::VideoPlayback>>::make(control.clone());
    auto preparation = media.Prepare(context, image, Some(playback.clone()));
    if (preparation.is_err())
        rstd::io::eprintln("media preparation failed: {}", preparation.as_ref().unwrap_err());
    auto prepared = rstd::move(preparation).unwrap();
    auto identity = prepared.view.texture().identity();
    auto initial  = context.Begin().unwrap();
    for (auto& upload : prepared.uploads) initial.commands()->Upload(rstd::move(upload)).unwrap();
    initial.commands()
        ->UseSampledView(prepared.view, prepared.sampler, tex::Stages::Fragment)
        .unwrap();
    context.wait(context.Submit(rstd::move(initial)).unwrap()).unwrap();
    auto expected = hwdec.as_str() == "none"_str     ? vk::MediaSourceKind::Software
                    : hwdec.as_str() == "vulkan"_str ? vk::MediaSourceKind::VulkanShared
                                                     : vk::MediaSourceKind::VaapiDrm;
    rstd_assert(prepared.producer->SourceKind() == expected);
    auto tick = [&] {
        prepared.producer->Pump(context, 1.0 / 30.0).unwrap();
        vrento::vulkan::VulkanPassData data { .scope = 1 };
        data.resources.push({ .id = { 1, 1 }, .imported = true });
        data.passes.push(vrento_test::texture_sampled_pass({ 1 }, { 1, 1 }, 0));
        vk::GraphBindings bindings(1);
        bindings
            .bind({ 1, 1 },
                  Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(prepared.view)))
            .unwrap();
        vk::VulkanPassBackends backends;
        vk::register_texture_passes(backends).unwrap();
        auto graph =
            vk::PreparedGraphProgram::prepare(
                session, vrento_test::make_pass_set(rstd::move(data)).unwrap(), bindings, backends)
                .unwrap();
        auto sampled = context.Begin().unwrap();
        graph.record(sampled).unwrap();
        context.wait(context.Submit(rstd::move(sampled)).unwrap()).unwrap();
        context.Poll().unwrap();
        rstd_assert(prepared.view.texture().identity() == identity);
    };
    for (unsigned frame {}; frame < 12; ++frame) tick();
    rstd_assert(prepared.producer->ContentRevision() != u64());
    control->state.lock().unwrap()->snapshot.playing = false;
    auto paused                                      = prepared.producer->ContentRevision();
    for (unsigned frame {}; frame < 4; ++frame) tick();
    rstd_assert(prepared.producer->ContentRevision() == paused);
    {
        auto state              = control->state.lock().unwrap();
        state->snapshot.playing = true;
        ++state->snapshot.seek_sequence;
        state->snapshot.seek_seconds = *state->duration - f64(0.2);
    }
    bool looped {};
    for (unsigned frame {}; frame < 30; ++frame) {
        tick();
        if (control->state.lock().unwrap()->current < f64(1.0)) looped = true;
    }
    rstd_assert(looped);
    media.Drain(u64(5'000'000'000)).unwrap();
    media.Clear();
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::texture_registry

namespace vrento_test::texture_registry
{
TEST(PreparedResourceTable, DescriptorTextureDependenciesSurviveCloneAndRemoveOldEdges) {
    namespace resource = vrento::resource;
    registry::DescriptorSystem       descriptors;
    registry::PreparedResourceTable  table;
    registry::DescriptorImageBinding image { .binding = 3 };
    auto                             packet = [&] {
        return descriptors.PreparePush(
            0, slice<registry::DescriptorImageBinding>::from_raw_parts(&image, usize(1)), {});
    };
    auto first = packet(), second = packet(), unrelated = packet();
    auto a = first.handle, b = second.handle, c = unrelated.handle;
    table.Insert(rstd::move(first));
    table.Insert(rstd::move(second));
    table.Insert(rstd::move(unrelated));
    resource::TextureUseHandle         one { u64(1), u64(9) }, two { u64(2), u64(9) };
    registry::DescriptorTextureBinding source { 3, one, usize(2) };
    ASSERT_TRUE(table.TrackDescriptorTextures(
        a, slice<registry::DescriptorTextureBinding>::from_raw_parts(&source, usize(1))));
    ASSERT_TRUE(table.TrackDescriptorTextures(
        b, slice<registry::DescriptorTextureBinding>::from_raw_parts(&source, usize(1))));
    source.texture = two;
    ASSERT_TRUE(table.TrackDescriptorTextures(
        c, slice<registry::DescriptorTextureBinding>::from_raw_parts(&source, usize(1))));
    const resource::TextureUseHandle changed[] { one, one };
    EXPECT_EQ(table
                  .AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(changed, usize(2)))
                  .len(),
              usize(2));
    auto previous = table.clone();
    ASSERT_TRUE(table.TrackDescriptorTextures(
        a, slice<registry::DescriptorTextureBinding>::from_raw_parts(&source, usize(1))));
    EXPECT_EQ(table
                  .AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(&one, usize(1)))
                  .len(),
              usize(1));
    EXPECT_EQ(previous
                  .AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(&one, usize(1)))
                  .len(),
              usize(2));
    table.Remove(b);
    EXPECT_TRUE(table
                    .AffectedTextureDescriptors(
                        slice<resource::TextureUseHandle>::from_raw_parts(&one, usize(1)))
                    .is_empty());
    registry::PreparedResourceTable next;
    next.CarryForward(rstd::move(table), resource::ResourcePlanTextures);
    EXPECT_EQ(next.AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(&two, usize(1)))
                  .len(),
              usize(2));
    EXPECT_EQ(next.DescriptorTextures(a)[usize()].texture, two);
    next.ClearPreparedState();
    EXPECT_TRUE(next.AffectedTextureDescriptors(
                        slice<resource::TextureUseHandle>::from_raw_parts(&two, usize(1)))
                    .is_empty());
}

TEST(RenderResourceSystem, RefreshesOnlyDependentDescriptorsAndRollsBackReplacementSets) {
    namespace resource = vrento::resource;
    auto created       = vk::DeviceSession::create({ .validation = true });
    if (created.is_err()) GTEST_SKIP() << "Vulkan unavailable";
    auto                           session = created.unwrap();
    registry::RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(session));
    resource::ResourcePlan plan { .generation = u64(91) };
    auto                   declare = [&](ref<str> name) {
        return plan.DeclareTexture(
            { .kind       = resource::TextureRequestKind::RenderTarget,
              .name       = String::make(name),
              .definition = Some(resource::TextureDefinition {
                  .width  = i32(8),
                  .height = i32(8),
                  .usage  = resource::TextureUsage::Color | resource::TextureUsage::Sampled }),
              .lifetime   = resource::TextureLifetimeClass::Retained },
            resource::ResourceAccess::ReadWrite);
    };
    auto first = declare("first"_str), second = declare("second"_str);
    resources.PreparePlan(plan, {}).unwrap();
    registry::PipelineLayoutRequest layout_request;
    vk::DescriptorSetInfo           set;
    set.bindings.push(
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    layout_request.descriptor_sets.push(rstd::move(set));
    auto layout  = resources.PreparePipelineLayout(session.device(), layout_request).unwrap();
    auto prepare = [&](resource::TextureUseHandle use) {
        registry::DescriptorTextureBinding source { 0, use, usize() };
        return resources
            .PrepareTextureDescriptor(
                session.device(),
                layout,
                u32(),
                slice<registry::DescriptorTextureBinding>::from_raw_parts(&source, usize(1)),
                {})
            .unwrap();
    };
    auto a = prepare(first), shared = prepare(first), b = prepare(second);
    auto retained        = resources.Prepared().clone();
    auto original        = (**retained.Resolve(a)).clone();
    auto original_shared = (**retained.Resolve(shared)).clone();
    auto unrelated       = (**retained.Resolve(b)).clone();
    ASSERT_EQ(original.backend, registry::DescriptorBindingBackend::Set);
    resources.PreparePlan(plan, {}, resource::ResourcePlanTextures).unwrap();
    EXPECT_EQ((*resources.Prepared().Resolve(a))->set->handle, original.set->handle);
    auto next_request              = plan.textures[usize()].definition.definition().clone();
    next_request.definition->width = i32(16);
    plan.textures[usize()].definition =
        plan.textures[usize()].definition.with_definition(rstd::move(next_request));
    auto transaction =
        resources.BeginPreparePlan(plan, {}, resource::ResourcePlanTextures).unwrap();
    while (resources.ContinuePreparePlan(transaction).unwrap() !=
           registry::ResourcePrepareProgress::Complete) {
    }
    auto replaced = (**resources.Prepared().Resolve(a)).clone();
    EXPECT_NE(replaced.set->handle, original.set->handle);
    EXPECT_NE((*resources.Prepared().Resolve(shared))->set->handle, original_shared.set->handle);
    EXPECT_EQ((*resources.Prepared().Resolve(b))->set->handle, unrelated.set->handle);
    EXPECT_NE(replaced.images[usize()].image.view, original.images[usize()].image.view);
    EXPECT_EQ((*retained.Resolve(a))->set->handle, original.set->handle);
    EXPECT_TRUE(resources.CancelPreparePlan(transaction));
    EXPECT_EQ((*resources.Prepared().Resolve(a))->set->handle, original.set->handle);
    EXPECT_EQ(resources.Prepared()
                  .AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(&first, usize(1)))
                  .len(),
              usize(2));
    resources.PreparePlan(plan, {}, resource::ResourcePlanTextures).unwrap();
    auto committed = (**resources.Prepared().Resolve(a)).clone();
    EXPECT_NE(committed.set->handle, original.set->handle);
    EXPECT_EQ((*resources.Prepared().Resolve(b))->set->handle, unrelated.set->handle);
    registry::DescriptorTextureBinding invalid { 0, first, usize(99) };
    EXPECT_TRUE(
        resources
            .UpdateDescriptorTextures(
                a, slice<registry::DescriptorTextureBinding>::from_raw_parts(&invalid, usize(1)))
            .is_err());
    EXPECT_EQ((*resources.Prepared().Resolve(a))->set->handle, committed.set->handle);
    auto missing = plan.clone();
    missing.textures.remove(usize());
    EXPECT_TRUE(resources.PreparePlan(missing, {}, resource::ResourcePlanTextures).is_err());
    EXPECT_EQ(resources.PrepareStatus().state, registry::ResourcePrepareState::Failed);
    EXPECT_EQ((*resources.Prepared().Resolve(a))->set->handle, committed.set->handle);
    resources.UpdateDescriptorImages(a, committed.images.as_slice()).unwrap();
    EXPECT_TRUE(resources.Prepared().DescriptorTextures(a).is_empty());
    EXPECT_EQ(resources.Prepared()
                  .AffectedTextureDescriptors(
                      slice<resource::TextureUseHandle>::from_raw_parts(&first, usize(1)))
                  .len(),
              usize(1));
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::texture_registry

namespace vrento_test::texture_registry
{
TEST(TextureRegistry, StableReferencesSeparateNamesStorageAndRevisions) {
    vrento::resource::TextureRegistry textures;
    vrento::resource::TextureRequest  request {
        .kind = vrento::resource::TextureRequestKind::RenderTarget,
        .name = "same"_Str,
        .definition =
            Some(vrento::resource::TextureDefinition { .width = i32(8), .height = i32(8) })
    };
    vrento::resource::TextureResourceRef first(request.clone()), other(request.clone());
    auto                                 a = textures.RegisterResource(first.clone()).unwrap();
    auto                                 b = textures.RegisterResource(other.clone()).unwrap();
    EXPECT_NE(a, b);
    EXPECT_EQ(textures.RegisterResource(first.clone()).unwrap(), a);
    EXPECT_TRUE(textures.Find(request.kind, request.name.as_str()).is_none());
    auto separate =
        textures.RegisterResource(first.clone(), None(), Some("other-storage"_str)).unwrap();
    EXPECT_NE(separate, a);
    auto old                  = first.clone();
    request.definition->width = i32(16);
    auto updated              = first.with_definition(request.clone());
    EXPECT_EQ(textures.RegisterResource(updated.clone()).unwrap(), a);
    EXPECT_EQ(old.definition().definition->width, i32(8));
    request.definition->width = i32(32);
    auto conflict             = first.with_definition(request.clone());
    EXPECT_TRUE(textures.RegisterResource(conflict).is_err());
    EXPECT_TRUE(textures.RegisterResource(old).is_err());
    ASSERT_TRUE(textures.BeginPrepareTransaction());
    auto next = updated.with_definition(request.clone());
    textures.RegisterResource(next).unwrap();
    auto cancelled =
        textures.RegisterResource(vrento::resource::TextureResourceRef(request.clone())).unwrap();
    textures.AbortPrepareTransaction();
    EXPECT_FALSE(textures.ResolveTexture(cancelled));
    auto fresh =
        textures.RegisterResource(vrento::resource::TextureResourceRef(request.clone())).unwrap();
    EXPECT_NE(fresh, cancelled);
    EXPECT_EQ((*textures.ResolveTexture(a))->definition.revision(), updated.revision());
    array<vrento::resource::TextureHandle, 1> active { a };
    textures.RetainActive(active.as_slice());
    EXPECT_EQ(textures.Size(), usize(1));
    textures.Reset();
    EXPECT_FALSE(textures.ResolveTexture(a));
    auto restored = textures.RegisterResource(updated).unwrap();
    EXPECT_NE(restored, a);
    for (unsigned iteration {}; iteration < 100; ++iteration) {
        auto temporary =
            textures.RegisterResource(vrento::resource::TextureResourceRef(request.clone()))
                .unwrap();
        array<vrento::resource::TextureHandle, 2> live { restored, temporary };
        textures.RetainActive(live.as_slice());
        EXPECT_EQ(textures.Size(), usize(2));
        EXPECT_TRUE(textures.ResolveTexture(restored));
    }
}
} // namespace vrento_test::texture_registry

namespace vrento_test::texture_registry
{
TEST(TextureRegistry, SharesCompatibleContentButSeparatesSourceScopes) {
    namespace resource = vrento::resource;
    auto session       = vk::DeviceSession::create({ .validation = true }).unwrap();
    registry::RegistryTextureContext context(vk::TextureContext::create(session).unwrap());
    auto                             image = PixelImage();
    context.TryPrepareImage(image.clone()).unwrap();
    auto allocation = context.CreateImportedTexture(rstd::move(image), None()).unwrap().allocation;
    resource::TextureRegistry textures;
    resource::TextureRequest  request { .kind = resource::TextureRequestKind::Imported,
                                        .name = "shared"_Str };
    resource::ImportedTextureContentIdentity content { .key      = "pixels"_Str,
                                                       .revision = u64(1),
                                                       .scope    = u64(1) };
    auto                                     a =
        textures
            .RegisterResource(resource::TextureResourceRef(request.clone()), Some(content.clone()))
            .unwrap();
    textures.Publish(a, allocation.clone()).unwrap();
    auto b =
        textures
            .RegisterResource(resource::TextureResourceRef(request.clone()), Some(content.clone()))
            .unwrap();
    EXPECT_TRUE(textures.ReusePhysical(b));
    EXPECT_TRUE(Arc<registry::TextureAllocation>::ptr_eq((*textures.ResolveCurrent(b))->allocation,
                                                         allocation));
    content.scope = u64(2);
    auto c =
        textures
            .RegisterResource(resource::TextureResourceRef(request.clone()), Some(content.clone()))
            .unwrap();
    EXPECT_FALSE(textures.ReusePhysical(c));
    textures.RetainActive(array<resource::TextureHandle, 1> { b }.as_slice());
    EXPECT_EQ(textures.Size(), usize(1));
    EXPECT_TRUE(textures.ResolveCurrent(b));
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(RenderResourceSystem, StableReferencesKeepStoragePolicyAndOldPreparedAllocations) {
    namespace resource = vrento::resource;
    auto session       = vk::DeviceSession::create({ .validation = true }).unwrap();
    registry::RenderResourceSystem resources;
    ASSERT_TRUE(resources.Initialize(session));
    resource::TextureRequest     request { .kind       = resource::TextureRequestKind::RenderTarget,
                                           .name       = "target"_Str,
                                           .definition = Some(resource::TextureDefinition {
                                               .width = i32(8), .height = i32(8) }),
                                           .lifetime = resource::TextureLifetimeClass::FrameLocal };
    resource::TextureResourceRef first(request.clone()), second(request.clone());
    auto                         plan = [&](u64 generation, ref<str> slot) {
        resource::ResourcePlan result { .generation = generation };
        result.textures.push({ .handle         = { u64(0), generation },
                               .definition     = first.clone(),
                               .allocation_key = String::make(slot) });
        result.textures.push({ .handle         = { u64(1), generation },
                               .definition     = second.clone(),
                               .allocation_key = String::make(slot) });
        result.textures.push({ .handle         = { u64(2), generation },
                               .definition     = first.clone(),
                               .allocation_key = "separate"_Str });
        return result;
    };
    auto before = plan(u64(1), "graph1:slot"_str);
    resources.PreparePlan(before, {}).unwrap();
    auto retained = resources.Prepared().clone();
    auto a        = (*retained.Resolve(before.textures[usize(0)].handle))->physical.clone();
    auto b        = (*retained.Resolve(before.textures[usize(1)].handle))->physical.clone();
    auto c        = (*retained.Resolve(before.textures[usize(2)].handle))->physical.clone();
    EXPECT_TRUE(Arc<registry::TextureAllocation>::ptr_eq(a, b));
    EXPECT_FALSE(Arc<registry::TextureAllocation>::ptr_eq(a, c));
    auto after = plan(u64(2), "graph2:slot"_str);
    resources.PreparePlan(after, {}).unwrap();
    auto d = (*resources.Prepared().Resolve(after.textures[usize(0)].handle))->physical.clone();
    EXPECT_FALSE(Arc<registry::TextureAllocation>::ptr_eq(a, d));
    EXPECT_TRUE(a->Slots()[usize()].view.texture().check().is_ok());
    EXPECT_EQ((*retained.Resolve(before.textures[usize(0)].handle))->definition.id(), first.id());
    EXPECT_EQ(session.validation_errors(), 0u);
}
} // namespace vrento_test::texture_registry

TEST(MediaRegistry, OpenFailureOwnsDecoderDiagnostic) {
    array<vk::Extension, 2> extensions { vk::Extension { true, "VK_KHR_external_memory_fd"_str },
                                         vk::Extension { true,
                                                         "VK_KHR_external_semaphore_fd"_str } };
    auto session = vk::DeviceSession::create({ .device_extensions = extensions.as_slice(),
                                               .api_version       = VK_API_VERSION_1_3,
                                               .validation        = true })
                       .unwrap();
    auto context = vk::TextureContext::create(session).unwrap();
    vk::MediaRegistry media(session, { "none"_Str, {} });
    auto              dir = rstd::fs::TempDir::make("vrento-invalid-video"_str).unwrap();
    auto              path =
        rstd::path::PathBuf::from(dir.path().as_os_str().to_os_string()).join("invalid.bin"_str);
    rstd::fs::write(path.as_path(), "not a video"_str.as_bytes()).unwrap();
    auto          file = rstd::fs::File::open(path.as_path()).unwrap();
    auto          size = file.metadata().unwrap().len();
    vrento::Image input;
    input.header.kind = vrento::ImageKind::Video;
    input.key         = "invalid-video"_Str;
    vrento::Image::Slot slot;
    slot.width  = 16;
    slot.height = 16;
    vrento::ImageData data;
    data.width  = 16;
    data.height = 16;
    data.video_source =
        Some(rstd::io::ReadRange::make(rstd::io::SharedReadAt::make(rstd::move(file)), u64(), size)
                 .unwrap());
    slot.mipmaps.push(rstd::move(data));
    input.slots.push(rstd::move(slot));
    auto failed = media.Prepare(context, input);
    ASSERT_TRUE(failed.is_err());
    auto error = failed.unwrap_err();
    EXPECT_EQ(error.operation, tex::MediaOperation::Open);
    ASSERT_TRUE(error.cause.is_Decoder());
    EXPECT_FALSE(error.cause.as_Decoder().message.is_empty());
    auto snapshot = vrento::resource::ResourceErrorSnapshot(
        vrento::resource::ResourceError::media(rstd::move(error)));
    media.Clear();
    EXPECT_FALSE(snapshot.error().source->as_Media().source.cause.as_Decoder().message.is_empty());
    EXPECT_EQ(session.validation_errors(), 0u);
}
