#include <rstd/macro.hpp>
import rstd;
import vrento.texture_vulkan;
using namespace rstd::prelude;
namespace tex = vrento::texture;
namespace vk  = vrento::vulkan;

auto read(vk::TextureContext& device, tex::TextureViewRef view, unsigned mip = 0,
          unsigned layer = 0) -> Vec<u8> {
    auto               extent = tex::mip_extent(view.texture().description(), mip);
    auto               size   = extent.width * extent.height * 4;
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
    for (unsigned i = 0; i < width * height * 4; ++i) upload.bytes.push(u8(value));
    work.commands()->Upload(rstd::move(upload)).unwrap();
}
int main() {
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
    for (unsigned i = 0; i < 76; ++i) upload.bytes.push(u8(i));
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
    vk::TextureCache       legacy_textures(session.device());
    vk::ImageUploadManager legacy_uploads(session.device());
    rstd_assert(legacy_uploads.init());
    vk::ImagePrepareContext legacy_context(legacy_textures, legacy_uploads);
    auto                    legacy_backend = dyn<vk::ImagePrepareBackend>::from_ref(legacy_context);
    vrento::Image           atlas;
    atlas.header.format = vrento::TextureFormat::R8;
    rstd::uint8_t       atlas_bytes[16] {};
    vrento::Image::Slot atlas_slot;
    atlas_slot.width = atlas_slot.height = 4;
    atlas_slot.mipmaps.push(
        vrento::ImageData { 4, 4, isize(16), vrento::ImageDataPtr(atlas_bytes, [](rstd::uint8_t*) {
                            }) });
    atlas.slots.push(rstd::move(atlas_slot));
    auto legacy_image = legacy_backend->CreateImportedTexture(rstd::ref<vrento::Image>::from_raw_parts(&atlas), None()).unwrap();
    work              = queue->Begin().unwrap();
    auto                     legacy_command = vk::native_recording(work, session).unwrap();
    vvk::CommandBuffer       command(legacy_command, session.device().handle().Dispatch());
    vk::RecordedImageUploads legacy_recorded;
    rstd_assert(legacy_uploads.RecordPendingUploads(command, legacy_recorded));
    work.retain(legacy_uploads.CommitRecordedUploads(rstd::move(legacy_recorded)).unwrap());
    work.retain(legacy_image.allocation.clone());
    auto legacy_done = queue->Submit(rstd::move(work)).unwrap();
    device.wait(legacy_done).unwrap();
    atlas_bytes[5] = 99;
    rstd_assert(legacy_textures.UploadFontAtlasRegion(
        legacy_image.allocation.deref(), atlas_bytes, 4, 1, 1, 1, 1));
    legacy_uploads.destroy();
    rstd_assert(session.validation_errors() == 0);
}
