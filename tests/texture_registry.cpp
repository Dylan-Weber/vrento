#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;

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
        rstd::io::eprintln("media preparation failed: kind={} native={}",
                           int(preparation.unwrap_err().kind),
                           preparation.unwrap_err().native);
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
        auto sampled = context.Begin().unwrap();
        sampled.commands()
            ->UseSampledView(prepared.view, prepared.sampler, tex::Stages::Fragment)
            .unwrap();
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
