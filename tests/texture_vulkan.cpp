#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace tex = vrento::texture;
namespace vk  = vrento::vulkan;

namespace vrento_test::texture_vulkan
{
struct ReadbackDevice {
    vk::TextureContext& context;
};
} // namespace vrento_test::texture_vulkan
namespace rstd
{
template<>
struct Impl<tex::TextureDevice, vrento_test::texture_vulkan::ReadbackDevice>
    : ImplBase<vrento_test::texture_vulkan::ReadbackDevice> {
    auto QuerySupport(const tex::TextureDesc& description) const -> Result<empty, tex::Error> {
        auto desc  = description;
        desc.usage = desc.usage | tex::Usage::TransferSource;
        return self().context.QuerySupport(desc);
    }
    auto CreateTexture(const tex::TextureDesc& description) -> Result<tex::TextureRef, tex::Error> {
        auto desc  = description;
        desc.usage = desc.usage | tex::Usage::TransferSource;
        return self().context.CreateTexture(desc);
    }
    auto CreateView(const tex::TextureRef& texture, tex::ViewDesc desc)
        -> Result<tex::TextureViewRef, tex::Error> {
        return self().context.CreateView(texture, desc);
    }
    auto CreateSampler(const tex::SamplerDesc& desc) -> Result<tex::SamplerRef, tex::Error> {
        return self().context.CreateSampler(desc);
    }
};
} // namespace rstd

namespace vrento_test::texture_vulkan
{
struct SynchronizationOwner {
    vk::DeviceSession session;
    vvk::Semaphore    semaphore;
    vvk::Fence        fence;
    explicit SynchronizationOwner(vk::DeviceSession value): session(rstd::move(value)) {}
};
auto semaphore(vk::DeviceSession session, bool timeline = false)
    -> rstd::sync::Arc<SynchronizationOwner> {
    auto                      owner = rstd::sync::Arc<SynchronizationOwner>::make(session);
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (timeline) info.pNext = &type;
    rstd_assert(session.device().handle().CreateSemaphore(info, owner->semaphore) == VK_SUCCESS);
    return owner;
}
auto pin(const rstd::sync::Arc<SynchronizationOwner>& owner)
    -> rstd::sync::Arc<dyn<rstd::any::Any>> {
    return rstd::sync::Arc<dyn<rstd::any::Any>>::make(owner.clone());
}
void host_signal(vk::DeviceSession session, VkSemaphore semaphore, tex::Serial value) {
    auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkSignalSemaphoreKHR"));
    rstd_assert(signal);
    VkSemaphoreSignalInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    info.semaphore = semaphore;
    info.value     = value;
    rstd_assert(signal(*session.device().handle(), &info) == VK_SUCCESS);
}
struct NativePassOwner {
    vk::DeviceSession session;
    vvk::RenderPass   pass;
    vvk::Framebuffer  framebuffer;
    explicit NativePassOwner(vk::DeviceSession value): session(rstd::move(value)) {}
};
struct ProducerOwner {
    vk::DeviceSession session;
    unsigned*         dropped;
    ProducerOwner(vk::DeviceSession value, unsigned& count)
        : session(rstd::move(value)), dropped(&count) {}
    ProducerOwner(const ProducerOwner&) = delete;
    ProducerOwner(ProducerOwner&& other)
        : session(rstd::move(other.session)), dropped(rstd::exchange(other.dropped, nullptr)) {}
    ~ProducerOwner() {
        if (dropped) ++*dropped;
    }
};
struct SurfaceOwner {
    vk::DeviceSession                     session;
    tex::TextureViewRef                   view;
    rstd::sync::Arc<SynchronizationOwner> acquire;
};
void producer_clear(tex::Submission& work, vk::DeviceSession session,
                    const vk::ProducerTarget& target, float value) {
    auto                 command  = vk::native_recording(work, session).unwrap();
    auto&                dispatch = session.device().handle().Dispatch();
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask       = target.initial.access;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout           = target.initial.layout;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                                             = target.image.handle;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    dispatch.vkCmdPipelineBarrier(command,
                                  target.initial.stages,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &barrier);
    VkClearColorValue clear {};
    for (auto& channel : clear.float32) channel = value;
    dispatch.vkCmdClearColorImage(command,
                                  target.image.handle,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &clear,
                                  1,
                                  &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = target.final.access;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = target.final.layout;
    dispatch.vkCmdPipelineBarrier(command,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  target.final.stages,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &barrier);
}
auto read(vk::TextureContext& device, tex::TextureViewRef view, unsigned mip = 0,
          unsigned layer = 0) -> Vec<u8> {
    auto               extent = tex::mip_extent(view.texture().description(), mip);
    auto               format = tex::format_layout(view.texture().description().format);
    auto               size   = ((extent.width + format.block_width - 1) / format.block_width) *
                                ((extent.height + format.block_height - 1) / format.block_height) *
                                format.block_bytes;
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer = device.session()
                      .device()
                      .memory_allocator()
                      .create_buffer(info, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto work   = device.Begin().unwrap();
    work.commands()
        ->UseTexture({ view,
                       tex::Access::TransferSource,
                       tex::Stages::Transfer,
                       false,
                       Some(tex::Range { tex::Aspect::Color, mip, 1, layer, 1 }) })
        .unwrap();
    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 };
    region.imageExtent      = { extent.width, extent.height, 1 };
    auto command            = vk::native_recording(work, device.session()).unwrap();
    device.session().device().handle().Dispatch().vkCmdCopyImageToBuffer(
        command,
        vk::native_texture(view.texture()).unwrap(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffer.handle(),
        1,
        &region);
    work.retain(buffer.clone());
    auto submitted = device.Submit(rstd::move(work)).unwrap();
    device.wait(submitted).unwrap();
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    Vec<u8> result;
    auto    bytes = reinterpret_cast<const unsigned char*>(mapped.data());
    for (unsigned i = 0; i < size; ++i) result.push(u8(bytes[i]));
    return result;
}
void fill(tex::Submission& work, tex::TextureViewRef target, unsigned width, unsigned height,
          unsigned value, unsigned mip = 0, unsigned layer = 0, unsigned x = 0, unsigned y = 0) {
    tex::TextureUpload upload;
    upload.target = target;
    upload.width  = width;
    upload.height = height;
    upload.mip    = mip;
    upload.layer  = layer;
    upload.x      = x;
    upload.y      = y;
    Vec<u8> bytes;
    bytes.resize(usize(width * height * 4), u8(value));
    upload.bytes = rstd::move(bytes);
    work.commands()->Upload(rstd::move(upload)).unwrap();
}
TEST(TextureVulkan, UploadLayoutsPreserveMipAndLayerPayloads) {
    auto  session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto  device  = vk::TextureContext::create(session).unwrap();
    usize tested_formats {};
    for (auto format : array<vrento::TextureFormat, 2> { vrento::TextureFormat::RGBA8,
                                                         vrento::TextureFormat::BC1 }) {
        tex::TextureDesc desc;
        desc.dimension  = tex::Dimension::Array2D;
        desc.extent     = { 12, 8 };
        desc.mip_levels = desc.layers = 2;
        desc.format                   = format;
        desc.usage =
            tex::Usage::Sampled | tex::Usage::TransferDestination | tex::Usage::TransferSource;
        if (device.QuerySupport(desc).is_err()) continue;
        ++tested_formats;
        auto texture = device.CreateTexture(desc).unwrap();
        auto view    = device.CreateView(texture, { tex::Dimension::Array2D }).unwrap();
        auto blocks  = tex::format_layout(format);
        for (unsigned layout {}; layout < 4; ++layout) {
            for (unsigned layer {}; layer < desc.layers; ++layer) {
                for (unsigned mip {}; mip < desc.mip_levels; ++mip) {
                    auto extent = tex::mip_extent(desc, mip);
                    auto row_bytes =
                        ((extent.width + blocks.block_width - 1) / blocks.block_width) *
                        blocks.block_bytes;
                    auto rows  = (extent.height + blocks.block_height - 1) / blocks.block_height;
                    auto pitch = row_bytes + (layout >= 2 ? 7 : 0);
                    auto size  = pitch * (rows - 1) + row_bytes;
                    tex::TextureUpload upload;
                    upload.target    = view;
                    upload.mip       = mip;
                    upload.layer     = layer;
                    upload.width     = extent.width;
                    upload.height    = extent.height;
                    upload.row_pitch = layout == 0 ? 0 : pitch;
                    Vec<u8> bytes;
                    bytes.resize(usize(size + (layout == 3 ? 31 : 0)), u8(0xee));
                    for (unsigned row {}; row < rows; ++row)
                        for (unsigned byte {}; byte < row_bytes; ++byte)
                            bytes[usize(row * pitch + byte)] =
                                u8((row * row_bytes + byte + mip * 13 + layer * 7) % 251);
                    upload.bytes = rstd::move(bytes);
                    auto work    = device.Begin().unwrap();
                    work.commands()->Upload(rstd::move(upload)).unwrap();
                    auto submitted = device.Submit(rstd::move(work)).unwrap();
                    device.wait(submitted).unwrap();
                    auto readback = read(device, view, mip, layer);
                    ASSERT_EQ(readback.len(), usize(row_bytes * rows));
                    for (usize index {}; index < readback.len(); ++index)
                        ASSERT_EQ(readback[index],
                                  u8((index.to_primitive() + mip * 13 + layer * 7) % 251));
                }
            }
        }
    }
    EXPECT_GT(tested_formats, usize());
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(TextureVulkan, PitchedRegionUploadsPreserveOtherBlocks) {
    auto  session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto  device  = vk::TextureContext::create(session).unwrap();
    usize tested_formats {};
    for (auto format : array<vrento::TextureFormat, 3> { vrento::TextureFormat::RGBA8,
                                                         vrento::TextureFormat::BC1,
                                                         vrento::TextureFormat::BC3 }) {
        tex::TextureDesc desc;
        desc.extent = { 18, 14 };
        desc.format = format;
        desc.usage =
            tex::Usage::Sampled | tex::Usage::TransferDestination | tex::Usage::TransferSource;
        if (device.QuerySupport(desc).is_err()) continue;
        ++tested_formats;
        auto texture = device.CreateTexture(desc).unwrap();
        auto view    = device.CreateView(texture, {}).unwrap();
        auto blocks  = tex::format_layout(format);
        auto columns = (desc.extent.width + blocks.block_width - 1) / blocks.block_width;
        auto rows    = (desc.extent.height + blocks.block_height - 1) / blocks.block_height;
        auto work    = device.Begin().unwrap();
        tex::TextureUpload initial;
        initial.target = view;
        initial.width  = desc.extent.width;
        initial.height = desc.extent.height;
        Vec<u8> initial_bytes;
        initial_bytes.resize(usize(columns * rows * blocks.block_bytes), u8(0x27));
        initial.bytes = rstd::move(initial_bytes);
        work.commands()->Upload(rstd::move(initial)).unwrap();
        tex::TextureUpload patch;
        patch.target = view;
        patch.x = patch.y  = 4;
        patch.width        = desc.extent.width - patch.x;
        patch.height       = desc.extent.height - patch.y;
        auto patch_columns = (patch.width + blocks.block_width - 1) / blocks.block_width;
        auto patch_rows    = (patch.height + blocks.block_height - 1) / blocks.block_height;
        auto row_bytes     = patch_columns * blocks.block_bytes;
        auto pitch         = row_bytes + 7;
        patch.row_pitch    = pitch;
        Vec<u8> patch_bytes;
        patch_bytes.resize(usize(pitch * (patch_rows - 1) + row_bytes), u8(0xee));
        for (unsigned row {}; row < patch_rows; ++row)
            for (unsigned byte {}; byte < row_bytes; ++byte)
                patch_bytes[usize(row * pitch + byte)] = u8((row * row_bytes + byte) % 251);
        patch.bytes = rstd::move(patch_bytes);
        work.commands()->Upload(rstd::move(patch)).unwrap();
        device.wait(device.Submit(rstd::move(work)).unwrap()).unwrap();
        auto bytes = read(device, view);
        ASSERT_EQ(bytes.len(), usize(columns * rows * blocks.block_bytes));
        auto first_column = 4 / blocks.block_width;
        auto first_row    = 4 / blocks.block_height;
        for (unsigned row {}; row < rows; ++row)
            for (unsigned column {}; column < columns; ++column)
                for (unsigned byte {}; byte < blocks.block_bytes; ++byte) {
                    auto expected = row >= first_row && column >= first_column
                                        ? u8(((row - first_row) * row_bytes +
                                              (column - first_column) * blocks.block_bytes + byte) %
                                             251)
                                        : u8(0x27);
                    EXPECT_EQ(bytes[usize((row * columns + column) * blocks.block_bytes + byte)],
                              expected);
                }
    }
    EXPECT_GT(tested_formats, usize());
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(TextureVulkan, ImageStagingRowsAndLeaseLifetime) {
    auto                   session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto                   device  = vk::TextureContext::create(session).unwrap();
    vk::ImageUploadManager uploads(session.device());
    ASSERT_TRUE(uploads.init());
    EXPECT_EQ(uploads.statistics().capacity_bytes, 0u);
    tex::TextureDesc desc;
    desc.extent = { 4, 4 };
    desc.usage = tex::Usage::Sampled | tex::Usage::TransferDestination | tex::Usage::TransferSource;
    auto              texture = device.CreateTexture(desc).unwrap();
    auto              view    = device.CreateView(texture, {}).unwrap();
    auto              image   = vk::native_texture(texture).unwrap();
    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { 4, 4, 1 };
    array<rstd::uint8_t, 76> bytes {};
    for (unsigned row {}; row < 4; ++row)
        for (unsigned byte {}; byte < 16; ++byte)
            bytes[usize(row * 20 + byte)] = rstd::uint8_t(row * 16 + byte);
    for (auto invalid :
         array<vk::ImageUploadRows, 5> { vk::ImageUploadRows { 0, 4, 20 },
                                         vk::ImageUploadRows { 16, 0, 20 },
                                         vk::ImageUploadRows { 16, 4, 15 },
                                         vk::ImageUploadRows { 16, 4, 21 },
                                         vk::ImageUploadRows { 16, 4, ~VkDeviceSize {} } })
        EXPECT_TRUE(
            uploads.QueueRegion(image, region, bytes.as_slice(), 4, Some(invalid)).is_err());
    auto non_tight            = region;
    non_tight.bufferRowLength = 4;
    EXPECT_TRUE(
        uploads
            .QueueRegion(
                image, non_tight, bytes.as_slice(), 4, Some(vk::ImageUploadRows { 16, 4, 20 }))
            .is_err());
    EXPECT_FALSE(uploads.HasPendingUploads());
    EXPECT_EQ(uploads.statistics().capacity_bytes, 0u);
    auto ticket =
        uploads
            .QueueRegion(
                image, region, bytes.as_slice(), 4, Some(vk::ImageUploadRows { 16, 4, 20 }))
            .unwrap();
    EXPECT_TRUE(ticket.Valid());
    auto capacity = uploads.statistics().capacity_bytes;
    EXPECT_GE(capacity, 64u);
    EXPECT_EQ(uploads.statistics().live_bytes, 64u);
    EXPECT_EQ(uploads.statistics().free_capacity_bytes, 0u);
    for (auto& byte : bytes) byte = 0xff;
    {
        auto cancelled = device.Begin().unwrap();
        cancelled.commands()
            ->UseTexture({ view, tex::Access::TransferDestination, tex::Stages::Transfer })
            .unwrap();
        auto command = vk::native_command_recording(cancelled, session).unwrap();
        vk::RecordedImageUploads recorded;
        uploads.RecordQueuedRegions(command, recorded).unwrap();
    }
    EXPECT_TRUE(uploads.HasPendingUploads());
    EXPECT_EQ(uploads.statistics().live_bytes, 64u);
    auto work = device.Begin().unwrap();
    work.commands()
        ->UseTexture({ view, tex::Access::TransferDestination, tex::Stages::Transfer })
        .unwrap();
    auto                     command = vk::native_command_recording(work, session).unwrap();
    vk::RecordedImageUploads recorded;
    uploads.RecordQueuedRegions(command, recorded).unwrap();
    auto lease = uploads.CommitRecordedUploads(rstd::move(recorded)).unwrap();
    EXPECT_EQ(lease.Tickets().len(), usize(1));
    EXPECT_EQ(lease.Tickets()[usize()], ticket);
    work.retain(rstd::move(lease));
    auto gate = semaphore(session, true);
    auto api  = dyn<vk::NativeSubmission>::from_ref(device);
    api->AttachWait(work,
                    { *session.device().handle(),
                      *gate->semaphore,
                      vk::NativeSemaphoreKind::Timeline,
                      1,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      pin(gate) })
        .unwrap();
    auto done = device.Submit(rstd::move(work)).unwrap();
    EXPECT_FALSE(done.poll().unwrap());
    device.Poll().unwrap();
    EXPECT_EQ(uploads.statistics().live_bytes, 64u);
    uploads.QueueRegion(image, region, bytes.as_slice(), 4, Some(vk::ImageUploadRows { 16, 4 }))
        .unwrap();
    EXPECT_EQ(uploads.statistics().capacity_bytes, capacity * 2);
    EXPECT_EQ(uploads.statistics().live_bytes, 128u);
    EXPECT_EQ(uploads.statistics().free_capacity_bytes, 0u);
    uploads.DiscardPendingUploads();
    EXPECT_EQ(uploads.statistics().live_bytes, 64u);
    EXPECT_EQ(uploads.statistics().free_capacity_bytes, capacity);
    host_signal(session, *gate->semaphore, 1);
    device.wait(done).unwrap();
    EXPECT_EQ(uploads.statistics().live_bytes, 0u);
    EXPECT_EQ(uploads.statistics().free_capacity_bytes, capacity * 2);
    auto pixels = read(device, view);
    ASSERT_EQ(pixels.len(), usize(64));
    for (usize index {}; index < pixels.len(); ++index)
        EXPECT_EQ(pixels[index], u8(index.to_primitive()));
    uploads.Trim();
    EXPECT_EQ(uploads.statistics().capacity_bytes, capacity);
    EXPECT_EQ(uploads.statistics().free_capacity_bytes, capacity);
    uploads.QueueRegion(image, region, bytes.as_slice()).unwrap();
    EXPECT_EQ(uploads.statistics().capacity_bytes, capacity);
    EXPECT_EQ(uploads.statistics().live_bytes, 76u);
    uploads.DiscardPendingUploads();
    EXPECT_EQ(uploads.statistics().live_bytes, 0u);
    uploads.destroy();
    EXPECT_EQ(uploads.statistics().capacity_bytes, 0u);
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(TextureVulkan, Resources) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             device  = vk::TextureContext::create(session).unwrap();
    auto             factory = dyn<tex::TextureDevice>::from_ref(device);
    auto             queue   = dyn<tex::SubmissionQueue>::from_ref(device);
    tex::TextureDesc desc;
    desc.extent     = { 8, 8 };
    desc.mip_levels = 4;
    desc.usage = tex::Usage::Sampled | tex::Usage::TransferDestination | tex::Usage::TransferSource;
    auto storage = factory->CreateTexture(desc).unwrap();
    auto all     = factory->CreateView(storage, {}).unwrap();
    rstd_assert(vk::validate_sampled_texture(session, all, tex::Stages::Geometry).is_ok());
    rstd_assert(vk::validate_sampled_texture(session, all, tex::Stages::Shaders).is_ok());
    rstd_assert(
        vk::validate_sampled_texture(session, all, tex::Stages::Geometry | tex::Stages::Color)
            .is_err());
    auto mip =
        factory->CreateView(storage, { tex::Dimension::D2, { tex::Aspect::Color, 1, 1, 0, 1 } })
            .unwrap();
    rstd_assert(all.texture().identity() == mip.texture().identity());
    rstd_assert(vk::native_view(mip).unwrap() ==
                vk::native_view(factory->CreateView(storage, mip.description()).unwrap()).unwrap());
    auto             nearest = factory->CreateSampler({}).unwrap();
    tex::SamplerDesc linear;
    linear.sample.minFilter = linear.sample.magFilter = vrento::TextureFilter::LINEAR;
    auto smooth                                       = factory->CreateSampler(linear).unwrap();
    rstd_assert(vk::native_sampler(nearest).unwrap() != vk::native_sampler(smooth).unwrap());
    {
        auto cancelled = queue->Begin().unwrap();
        cancelled.commands()
            ->UseTexture({ mip, tex::Access::TransferDestination, tex::Stages::Transfer })
            .unwrap();
        rstd_assert(queue->Begin().unwrap().commands()->UseTexture({ all }).unwrap_err().kind ==
                    tex::ErrorKind::Busy);
    }
    auto               work = queue->Begin().unwrap();
    tex::TextureUpload upload;
    upload.target = mip;
    upload.mip    = 1;
    upload.width = upload.height = 4;
    upload.row_pitch             = 20;
    Vec<u8> upload_bytes;
    for (unsigned i = 0; i < 76; ++i) upload_bytes.push(u8(i));
    upload.bytes = rstd::move(upload_bytes);
    work.commands()->Upload(rstd::move(upload)).unwrap();
    work.commands()->UseSampledView(mip, smooth, tex::Stages::Fragment).unwrap();
    auto submitted = queue->Submit(rstd::move(work)).unwrap();
    device.wait(submitted).unwrap();
    vk::prepare_texture_read(session, mip, tex::Stages::Fragment).unwrap();
    rstd_assert(vk::prepare_texture_read(session, all, tex::Stages::Fragment).is_err());
    auto pixels = read(device, mip, 1);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 16; ++x) rstd_assert(pixels[usize(y * 16 + x)] == u8(y * 20 + x));
    work = queue->Begin().unwrap();
    fill(work, all, 2, 2, 250, 1, 0, 1, 1);
    queue->Submit(rstd::move(work)).unwrap();
    pixels = read(device, mip, 1);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
            for (unsigned c = 0; c < 4; ++c)
                rstd_assert(pixels[usize((y * 4 + x) * 4 + c)] ==
                            u8(x >= 1 && x <= 2 && y >= 1 && y <= 2 ? 250 : y * 20 + x * 4 + c));
    tex::TextureDesc cube_desc { tex::Dimension::Cube,         { 4, 4 },  3, 6, 1,
                                 vrento::TextureFormat::RGBA8, desc.usage };
    auto             cube      = factory->CreateTexture(cube_desc).unwrap();
    auto             cube_view = factory->CreateView(cube, { tex::Dimension::Cube }).unwrap();
    work                       = queue->Begin().unwrap();
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < 3; ++level)
            fill(work, cube_view, 4 >> level, 4 >> level, face * 10 + level, level, face);
    queue->Submit(rstd::move(work)).unwrap();
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < 3; ++level) {
            auto result = read(device, cube_view, level, face);
            for (auto byte : result) rstd_assert(byte == u8(face * 10 + level));
        }
    tex::TextureDesc array_desc { tex::Dimension::Array2D,      { 4, 2 },  2, 3, 1,
                                  vrento::TextureFormat::RGBA8, desc.usage };
    auto             array      = factory->CreateTexture(array_desc).unwrap();
    auto             array_view = factory->CreateView(array, { tex::Dimension::Array2D }).unwrap();
    work                        = queue->Begin().unwrap();
    for (unsigned layer = 0; layer < 3; ++layer)
        for (unsigned level = 0; level < 2; ++level)
            fill(work, array_view, 4 >> level, 2 >> level, 40 + layer * 10 + level, level, layer);
    work.commands()
        ->UseSampledView(array_view, smooth, tex::Stages::Vertex | tex::Stages::Compute)
        .unwrap();
    queue->Submit(rstd::move(work)).unwrap();
    for (unsigned layer = 0; layer < 3; ++layer)
        for (unsigned level = 0; level < 2; ++level) {
            auto result = read(device, array_view, level, layer);
            for (auto byte : result) rstd_assert(byte == u8(40 + layer * 10 + level));
        }
    auto other_context = vk::TextureContext::create(session).unwrap();
    auto other_sampler = other_context.CreateSampler({}).unwrap();
    rstd_assert(other_sampler.identity() != nearest.identity());
    auto invalid = queue->Begin().unwrap();
    rstd_assert(
        invalid.commands()->UseSampledView({}, nearest, tex::Stages::Fragment).unwrap_err().kind ==
        tex::ErrorKind::Invalid);
    tex::TextureDesc pool_desc;
    pool_desc.extent = { 4, 4 };
    pool_desc.usage  = desc.usage;
    auto pool        = tex::TexturePool::create(factory.as_mut_ref(), pool_desc, 1).unwrap();
    auto write       = pool.acquire().unwrap();
    work             = queue->Begin().unwrap();
    fill(work, write.view(), 4, 4, 100);
    work.commands()->UseSampledView(write.view(), nearest, tex::Stages::Fragment).unwrap();
    auto done      = queue->Submit(rstd::move(work)).unwrap();
    auto published = pool.publish(rstd::move(write), done).unwrap();
    device.wait(done).unwrap();
    rstd_assert(pool.acquire().unwrap_err().kind == tex::ErrorKind::Busy);
    auto copy = published;
    published = {};
    rstd_assert(pool.acquire().is_err());
    copy = {};
    rstd_assert(pool.acquire().is_ok());
    ReadbackDevice           readback_device { device };
    auto                     readback_factory = dyn<tex::TextureDevice>::from_ref(readback_device);
    tex::ImagePrepareContext atlas_context(readback_factory.as_mut_ref());
    auto atlas_preparer  = dyn<tex::TextureContentPreparer>::from_ref(atlas_context);
    auto atlas           = rstd::sync::Arc<vrento::Image>::make();
    atlas->header.format = vrento::TextureFormat::R8;
    auto* atlas_bytes    = new rstd::uint8_t[16] {};
    for (unsigned i = 0; i < 16; ++i) atlas_bytes[i] = rstd::uint8_t(i);
    vrento::Image::Slot atlas_slot;
    atlas_slot.width = atlas_slot.height = 4;
    atlas_slot.mipmaps.push(vrento::ImageData {
        4, 4, isize(16), vrento::ImageDataPtr(atlas_bytes, [](rstd::uint8_t* data) {
            delete[] data;
        }) });
    atlas->slots.push(rstd::move(atlas_slot));
    auto atlas_weak     = atlas.downgrade();
    auto prepared_atlas = atlas_preparer->PrepareImage(rstd::move(atlas)).unwrap();
    rstd_assert(prepared_atlas.len() == usize(1));
    auto& atlas_image = prepared_atlas[usize()];
    rstd_assert(atlas_image.view.texture().description().format == vrento::TextureFormat::R8);
    EXPECT_FALSE(atlas_weak.expired());
    work = queue->Begin().unwrap();
    for (auto& item : atlas_image.uploads) work.commands()->Upload(rstd::move(item)).unwrap();
    EXPECT_TRUE(atlas_weak.expired());
    work.commands()
        ->UseSampledView(atlas_image.view, atlas_image.sampler, tex::Stages::Fragment)
        .unwrap();
    device.wait(queue->Submit(rstd::move(work)).unwrap()).unwrap();
    auto atlas_pixels = read(device, atlas_image.view);
    rstd_assert(atlas_pixels.len() == usize(16));
    for (unsigned i = 0; i < 16; ++i) rstd_assert(atlas_pixels[usize(i)] == u8(i));
    tex::TextureUpload atlas_update;
    atlas_update.target = atlas_image.view;
    atlas_update.x = atlas_update.y = 1;
    atlas_update.width = atlas_update.height = 1;
    Vec<u8> atlas_update_bytes;
    atlas_update_bytes.push(u8(99));
    atlas_update.bytes = rstd::move(atlas_update_bytes);
    work               = queue->Begin().unwrap();
    work.commands()->Upload(rstd::move(atlas_update)).unwrap();
    work.commands()
        ->UseSampledView(atlas_image.view, atlas_image.sampler, tex::Stages::Fragment)
        .unwrap();
    device.wait(queue->Submit(rstd::move(work)).unwrap()).unwrap();
    atlas_pixels = read(device, atlas_image.view);
    rstd_assert(atlas_pixels.len() == usize(16));
    for (unsigned i = 0; i < 16; ++i) rstd_assert(atlas_pixels[usize(i)] == u8(i == 5 ? 99 : i));
    rstd_assert(session.validation_errors() == 0);
}
TEST(TextureVulkan, MixedSubmission) {
    auto              session      = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto              context      = vk::TextureContext::create(session).unwrap();
    auto              api          = dyn<vk::NativeSubmission>::from_ref(context);
    auto              device       = *session.device().handle();
    auto              acquire      = semaphore(session);
    auto              timeline     = semaphore(session, true);
    auto              gate         = semaphore(session, true);
    auto              release      = semaphore(session);
    auto              release_weak = release.downgrade();
    auto              fence        = rstd::sync::Arc<SynchronizationOwner>::make(session);
    VkFenceCreateInfo fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    rstd_assert(session.device().handle().CreateFence(fence_info, fence->fence) == VK_SUCCESS);
    auto setup = context.Begin().unwrap();
    api->AttachSignal(
           setup, { device, *acquire->semaphore, vk::NativeSemaphoreKind::Binary, 0, pin(acquire) })
        .unwrap();
    api->AttachSignal(
           setup,
           { device, *timeline->semaphore, vk::NativeSemaphoreKind::Timeline, 3, pin(timeline) })
        .unwrap();
    auto setup_done = context.Submit(rstd::move(setup)).unwrap();
    context.wait(setup_done).unwrap();
    auto work = context.Begin().unwrap();
    api->AttachWait(work,
                    { device,
                      *acquire->semaphore,
                      vk::NativeSemaphoreKind::Binary,
                      0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      pin(acquire) })
        .unwrap();
    api->AttachWait(work,
                    { device,
                      *timeline->semaphore,
                      vk::NativeSemaphoreKind::Timeline,
                      1,
                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                      pin(timeline) })
        .unwrap();
    api->AttachWait(work,
                    { device,
                      *timeline->semaphore,
                      vk::NativeSemaphoreKind::Timeline,
                      3,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      pin(timeline) })
        .unwrap();
    api->AttachWait(work,
                    { device,
                      *gate->semaphore,
                      vk::NativeSemaphoreKind::Timeline,
                      1,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      pin(gate) })
        .unwrap();
    api->AttachSignal(
           work, { device, *release->semaphore, vk::NativeSemaphoreKind::Binary, 0, pin(release) })
        .unwrap();
    api->AttachSignal(
           work,
           { device, *timeline->semaphore, vk::NativeSemaphoreKind::Timeline, 5, pin(timeline) })
        .unwrap();
    api->AttachFence(work, { device, *fence->fence, pin(fence) }).unwrap();
    rstd_assert(api->AttachFence(work, { device, *fence->fence, pin(fence) }).is_err());
    rstd_assert(api->AttachWait(work,
                                { device,
                                  *gate->semaphore,
                                  vk::NativeSemaphoreKind::Binary,
                                  1,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  pin(gate) })
                    .is_err());
    auto recorder = vk::native_command_recording(work, session).unwrap();
    (void)recorder;
    auto receipt = work.receipt();
    auto done    = context.Submit(rstd::move(work)).unwrap();
    release.reset();
    rstd_assert(receipt.submitted() && ! done.poll().unwrap());
    context.Poll().unwrap();
    rstd_assert(! release_weak.expired());
    host_signal(session, *gate->semaphore, 1);
    context.wait(done).unwrap();
    rstd_assert(done.poll().unwrap() && release_weak.expired());
    auto native_fence = *fence->fence;
    rstd_assert(session.device().handle().Dispatch().vkResetFences(device, 1, &native_fence) ==
                VK_SUCCESS);
    rstd_assert(done.poll().unwrap());
    auto external = context.ImportDependency(*timeline->semaphore, 5, pin(timeline)).unwrap();
    rstd_assert(external.poll().unwrap());
    auto rejected         = context.Begin().unwrap();
    auto rejected_receipt = rejected.receipt();
    auto other_context    = vk::TextureContext::create(session).unwrap();
    rstd_assert(other_context.Submit(rstd::move(rejected)).unwrap_err().kind ==
                tex::ErrorKind::DeviceMismatch);
    rstd_assert(rejected_receipt.cancelled());
    rstd_assert(session.validation_errors() == 0);
}
TEST(TextureVulkan, MultisampleResolveAndDepthLayout) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             context = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc desc;
    desc.extent = { 4, 4 };
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::TransferSource;
    VkImageFormatProperties properties {};
    rstd_assert(session.device().instance_dispatch().vkGetPhysicalDeviceImageFormatProperties(
                    *session.device().gpu(),
                    VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_TYPE_2D,
                    VK_IMAGE_TILING_OPTIMAL,
                    vk::image_usage(desc.usage),
                    0,
                    &properties) == VK_SUCCESS);
    unsigned chosen = 0;
    for (unsigned samples = 2; samples <= 64; samples *= 2) {
        desc.samples = samples;
        rstd_assert(context.QuerySupport(desc).is_ok() == bool(properties.sampleCounts & samples));
        if (! chosen && context.QuerySupport(desc).is_ok()) chosen = samples;
    }
    rstd_assert(chosen != 0);
    desc.samples     = chosen;
    auto multisample = context.CreateTexture(desc).unwrap();
    auto ms_view     = context.CreateView(multisample, {}).unwrap();
    rstd_assert(vk::attachment_image(ms_view).unwrap().samples == VkSampleCountFlagBits(chosen));
    desc.samples  = 1;
    desc.usage    = desc.usage | tex::Usage::Sampled;
    auto resolved = context.CreateView(context.CreateTexture(desc).unwrap(), {}).unwrap();
    NativePassOwner         pass(session);
    VkAttachmentDescription attachments[2] {};
    for (unsigned i = 0; i < 2; ++i) {
        attachments[i].format  = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[i].samples = i == 0 ? VkSampleCountFlagBits(chosen) : VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp =
            i == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].initialLayout = attachments[i].finalLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentReference color { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolve { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription  subpass {};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color;
    subpass.pResolveAttachments  = &resolve;
    VkRenderPassCreateInfo pass_info { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pass_info.attachmentCount = 2;
    pass_info.pAttachments    = attachments;
    pass_info.subpassCount    = 1;
    pass_info.pSubpasses      = &subpass;
    rstd_assert(session.device().handle().CreateRenderPass(pass_info, pass.pass) == VK_SUCCESS);
    VkImageView views[] { vk::native_view(ms_view).unwrap(), vk::native_view(resolved).unwrap() };
    VkFramebufferCreateInfo framebuffer { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebuffer.renderPass      = *pass.pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments    = views;
    framebuffer.width = framebuffer.height = 4;
    framebuffer.layers                     = 1;
    rstd_assert(session.device().handle().CreateFramebuffer(framebuffer, pass.framebuffer) ==
                VK_SUCCESS);
    auto                  work = context.Begin().unwrap();
    VkClearValue          clear {};
    VkRenderPassBeginInfo begin { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    auto                  command = vk::native_recording(work, session).unwrap();
    for (unsigned repeat {}; repeat < 2; ++repeat) {
        work.commands()
            ->UseTexture({ ms_view, tex::Access::ColorAttachment, tex::Stages::Color, true })
            .unwrap();
        work.commands()
            ->UseTexture({ resolved, tex::Access::ColorAttachment, tex::Stages::Color, true })
            .unwrap();
        clear                               = {};
        clear.color.float32[repeat ? 2 : 0] = 1;
        clear.color.float32[3]              = 1;
        begin.renderPass                    = *pass.pass;
        begin.framebuffer                   = *pass.framebuffer;
        begin.renderArea.extent             = { 4, 4 };
        begin.clearValueCount               = 1;
        begin.pClearValues                  = &clear;
        session.device().handle().Dispatch().vkCmdBeginRenderPass(
            command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        session.device().handle().Dispatch().vkCmdEndRenderPass(command);
        work.commands()
            ->UseTexture({ resolved, tex::Access::Sampled, tex::Stages::Fragment })
            .unwrap();
    }
    work.retain(rstd::move(pass));
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    auto pixels = read(context, resolved);
    for (unsigned i = 0; i < 16; ++i) {
        rstd_assert(pixels[usize(i * 4)] == u8(0));
        rstd_assert(pixels[usize(i * 4 + 2)] == u8(255));
        rstd_assert(pixels[usize(i * 4 + 1)] == u8(0));
        rstd_assert(pixels[usize(i * 4 + 3)] == u8(255));
    }
    tex::TextureDesc depth_desc;
    depth_desc.extent      = { 4, 4 };
    depth_desc.format      = vrento::TextureFormat::D32F;
    depth_desc.usage       = tex::Usage::DepthAttachment | tex::Usage::Sampled;
    auto             depth = context
                                 .CreateView(context.CreateTexture(depth_desc).unwrap(),
                                             { tex::Dimension::D2, { tex::Aspect::Depth } })
                                 .unwrap();
    tex::SamplerDesc compare;
    compare.sample.compare_enable = true;
    auto sampler                  = context.CreateSampler(compare).unwrap();
    rstd_assert(vk::sampled_image(depth, sampler).unwrap().layout ==
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    auto invalid_final = context.Begin().unwrap();
    rstd_assert(vk::RecordFinalState(invalid_final, session, { depth }).is_err());
    work = context.Begin().unwrap();
    work.commands()
        ->UseTexture({ depth, tex::Access::DepthAttachment, tex::Stages::Depth, true })
        .unwrap();
    NativePassOwner         depth_pass(session);
    VkAttachmentDescription attachment {};
    attachment.format        = VK_FORMAT_D32_SFLOAT;
    attachment.samples       = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachment.finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference depth_reference { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    subpass.colorAttachmentCount = 0;
    subpass.pColorAttachments = subpass.pResolveAttachments = nullptr;
    subpass.pDepthStencilAttachment                         = &depth_reference;
    pass_info.attachmentCount                               = 1;
    pass_info.pAttachments                                  = &attachment;
    rstd_assert(session.device().handle().CreateRenderPass(pass_info, depth_pass.pass) ==
                VK_SUCCESS);
    VkImageView depth_view      = vk::native_view(depth).unwrap();
    framebuffer.renderPass      = *depth_pass.pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments    = &depth_view;
    rstd_assert(session.device().handle().CreateFramebuffer(framebuffer, depth_pass.framebuffer) ==
                VK_SUCCESS);
    clear.depthStencil.depth = 0.5f;
    begin.renderPass         = *depth_pass.pass;
    begin.framebuffer        = *depth_pass.framebuffer;
    command                  = vk::native_recording(work, session).unwrap();
    session.device().handle().Dispatch().vkCmdBeginRenderPass(
        command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    session.device().handle().Dispatch().vkCmdEndRenderPass(command);
    vk::RecordFinalState(work, session, { depth, tex::Access::Sampled, tex::Stages::Fragment })
        .unwrap();
    work.retain(rstd::move(depth_pass));
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    vk::prepare_texture_read(session, depth, tex::Stages::Fragment).unwrap();
    {
        auto cancelled = context.Begin().unwrap();
        cancelled.commands()
            ->UseTexture({ depth, tex::Access::DepthAttachment, tex::Stages::Depth })
            .unwrap();
    }
    vk::prepare_texture_read(session, depth, tex::Stages::Fragment).unwrap();
    rstd_assert(session.validation_errors() == 0);
}
TEST(TextureVulkan, ContinuousProducer) {
    auto             session          = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             context          = vk::TextureContext::create(session).unwrap();
    auto             producer_context = vk::TextureContext::create(session).unwrap();
    auto             interop          = dyn<vk::TextureInterop>::from_ref(context);
    tex::TextureDesc desc;
    desc.extent = { 4, 4 };
    desc.usage = tex::Usage::Sampled | tex::Usage::TransferDestination | tex::Usage::TransferSource;
    auto     texture  = context.CreateTexture(desc).unwrap();
    auto     view     = context.CreateView(texture, {}).unwrap();
    auto     identity = texture.identity();
    unsigned dropped  = 0;
    {
        auto cancelled =
            interop
                ->BeginProducerWrite(
                    { view,
                      { tex::Access::Sampled, tex::Stages::Fragment },
                      None(),
                      rstd::sync::Arc<dyn<rstd::any::Any>>::make(ProducerOwner(session, dropped)) })
                .unwrap();
        auto conflict = context.Begin().unwrap();
        rstd_assert(conflict.commands()->UseTexture({ view }).unwrap_err().kind ==
                    tex::ErrorKind::Busy);
        cancelled.cancel();
        rstd_assert(conflict.commands()->UseTexture({ view }).is_ok());
    }
    rstd_assert(dropped == 1);
    for (unsigned frame = 1; frame <= 2; ++frame) {
        tex::SubmissionToken done;
        {
            auto write = interop
                             ->BeginProducerWrite({ view,
                                                    { tex::Access::Sampled, tex::Stages::Fragment },
                                                    None(),
                                                    rstd::sync::Arc<dyn<rstd::any::Any>>::make(
                                                        ProducerOwner(session, dropped)) })
                             .unwrap();
            context.wait(write.target().dependency).unwrap();
            auto work = producer_context.Begin().unwrap();
            producer_clear(work, session, write.target(), frame == 1 ? 0.0f : 1.0f);
            done = producer_context.Submit(rstd::move(work)).unwrap();
            interop->CommitProducerWrite(write, done).unwrap();
            rstd_assert(! write.active() && texture.identity() == identity &&
                        texture.ready() == done);
        }
        rstd_assert(dropped == frame);
        context.wait(done).unwrap();
        producer_context.Poll().unwrap();
        rstd_assert(dropped == frame + 1);
        auto pixels = read(context, view);
        for (auto pixel : pixels) rstd_assert(pixel == u8(frame == 1 ? 0 : 255));
    }
    auto poisoned_texture = context.CreateTexture(desc).unwrap();
    auto poisoned         = context.CreateView(poisoned_texture, {}).unwrap();
    {
        auto uncertain =
            context
                .BeginProducerWrite(
                    { poisoned,
                      { tex::Access::Sampled, tex::Stages::Fragment },
                      None(),
                      rstd::sync::Arc<dyn<rstd::any::Any>>::make(ProducerOwner(session, dropped)) })
                .unwrap();
        uncertain.uncertain();
        auto reader = context.Begin().unwrap();
        rstd_assert(reader.commands()->UseTexture({ poisoned }).unwrap_err().kind ==
                    tex::ErrorKind::Closed);
    }
    rstd_assert(dropped == 4);
    rstd_assert(session.validation_errors() == 0);
}
TEST(TextureVulkan, SurfaceLeaseRecording) {
    auto             session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             context = vk::TextureContext::create(session).unwrap();
    tex::TextureDesc desc;
    desc.extent  = { 4, 4 };
    desc.usage   = tex::Usage::ColorAttachment | tex::Usage::TransferDestination;
    auto texture = context.CreateTexture(desc).unwrap();
    auto view    = context.CreateView(texture, {}).unwrap();
    auto acquire = semaphore(session);
    auto owner =
        rstd::sync::Arc<dyn<rstd::any::Any>>::make(SurfaceOwner { session, view, acquire.clone() });
    auto                      family = session.device().graphics_queue().family_index;
    vrento::FrameSurfaceLease lease;
    lease.identity             = { u64(1), u32(0), u64(1) };
    lease.image                = vk::attachment_image(view).unwrap().image;
    lease.format               = VK_FORMAT_R8G8B8A8_UNORM;
    lease.initial_layout       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    lease.reuse.kind           = vrento::FrameSurfaceReuseKind::PresentationAcquired;
    lease.acquire.kind         = vrento::FrameSurfaceAcquireKind::BinarySemaphore;
    lease.acquire.semaphore    = *acquire->semaphore;
    lease.initial_queue_family = lease.final_queue_family = family;
    lease.final_layout                                    = VK_IMAGE_LAYOUT_GENERAL;
    rstd_assert(lease.valid());
    auto setup = context.Begin().unwrap();
    context
        .AttachSignal(setup,
                      { *session.device().handle(),
                        *acquire->semaphore,
                        vk::NativeSemaphoreKind::Binary,
                        0,
                        pin(acquire) })
        .unwrap();
    context.wait(context.Submit(rstd::move(setup)).unwrap()).unwrap();
    auto cancelled    = context.Begin().unwrap();
    auto cancellation = cancelled.receipt();
    rstd_assert(vk::recordRelease(cancelled, session, lease).unwrap_err().kind ==
                tex::ErrorKind::Stale);
    vk::recordAcquire(cancelled, session, lease, owner.clone()).unwrap();
    rstd_assert(context.Submit(rstd::move(cancelled)).unwrap_err().kind == tex::ErrorKind::Invalid);
    rstd_assert(cancellation.cancelled());
    auto work = context.Begin().unwrap();
    vk::recordAcquire(
        work,
        session,
        lease,
        owner.clone(),
        vk::native_access({ tex::Access::TransferDestination, tex::Stages::Transfer }))
        .unwrap();
    rstd_assert(vk::recordAcquire(work, session, lease, owner.clone()).unwrap_err().kind ==
                tex::ErrorKind::Busy);
    auto                    command = vk::native_recording(work, session).unwrap();
    VkClearColorValue       clear {};
    VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    session.device().handle().Dispatch().vkCmdClearColorImage(
        command, lease.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    vk::recordSurfaceFinalState(
        work,
        session,
        lease,
        vk::native_access({ tex::Access::TransferDestination, tex::Stages::Transfer }))
        .unwrap();
    vk::recordRelease(work, session, lease).unwrap();
    rstd_assert(vk::recordRelease(work, session, lease).unwrap_err().kind == tex::ErrorKind::Stale);
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    setup = context.Begin().unwrap();
    context
        .AttachSignal(setup,
                      { *session.device().handle(),
                        *acquire->semaphore,
                        vk::NativeSemaphoreKind::Binary,
                        0,
                        pin(acquire) })
        .unwrap();
    context.wait(context.Submit(rstd::move(setup)).unwrap()).unwrap();
    lease.identity.acquire_serial = u64(2);
    lease.initial_layout          = VK_IMAGE_LAYOUT_GENERAL;
    work                          = context.Begin().unwrap();
    vk::recordAcquire(work, session, lease, owner.clone()).unwrap();
    vk::recordRelease(work, session, lease).unwrap();
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vrento_test::texture_vulkan
