#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
using namespace rstd::prelude;
using namespace vrento;
using namespace vrento::texture;

namespace vrento_test::texture
{
struct FakeDevice {
    Serial next {};
    auto   QuerySupport(const TextureDesc& desc) const -> Result<empty, Error> {
        return validate(desc);
    }
    auto CreateTexture(const TextureDesc& desc) -> Result<TextureRef, Error> {
        auto valid = validate(desc);
        if (valid.is_err()) return Err(valid.unwrap_err());
        return Ok(
            TextureRef({ 1, 1, ++next }, desc, rstd::sync::Arc<dyn<rstd::any::Any>>::make(0)));
    }
    auto CreateView(const TextureRef& texture, ViewDesc desc) -> Result<TextureViewRef, Error> {
        auto normalized = normalize(texture.description(), desc);
        if (normalized.is_err()) return Err(normalized.unwrap_err());
        return Ok(TextureViewRef(
            texture, normalized.unwrap(), rstd::sync::Arc<dyn<rstd::any::Any>>::make(0)));
    }
    auto CreateSampler(const SamplerDesc& desc) -> Result<SamplerRef, Error> {
        return Ok(
            SamplerRef({ 1, 1, ++next }, desc, rstd::sync::Arc<dyn<rstd::any::Any>>::make(0)));
    }
};
struct Progress {
    Serial submitted {}, completed {};
    bool   failed {};
    bool   uncertain {};
};
struct FakeDependency {
    rstd::sync::Arc<Progress> progress;
    Serial                    value;
};
struct DropProbe {
    int* count;
    explicit DropProbe(int& value): count(&value) {}
    DropProbe(const DropProbe&) = delete;
    DropProbe(DropProbe&& other): count(rstd::exchange(other.count, nullptr)) {}
    ~DropProbe() {
        if (count) ++*count;
    }
};
struct FakeCommands {
    Vec<TextureUpload>            uploads;
    Vec<TextureViewRef>           views;
    Vec<SamplerRef>               samplers;
    Vec<SubmissionToken>          dependencies;
    Vec<Box<dyn<rstd::any::Any>>> retained;
    auto                          Upload(TextureUpload upload) -> Result<empty, Error> {
        auto checked = validate_upload(upload);
        if (checked.is_err()) return Err(checked.unwrap_err());
        uploads.push(rstd::move(upload));
        return Ok(empty {});
    }
    auto UseTexture(TextureAccess use) -> Result<empty, Error> {
        if (! use.view.valid() || ! use.view.permits(writes(use.access)))
            return Err(Error { ErrorKind::Invalid });
        views.push(rstd::move(use.view));
        return Ok(empty {});
    }
    auto UseSampledView(TextureViewRef view, SamplerRef sampler, Stages stages)
        -> Result<empty, Error> {
        auto checked = UseTexture({ rstd::move(view), Access::Sampled, stages });
        if (checked.is_ok()) samplers.push(rstd::move(sampler));
        return checked;
    }
    void Retain(Box<dyn<rstd::any::Any>> owner) { retained.push(rstd::move(owner)); }
    auto Backend() -> mut_ref<dyn<rstd::any::Any>> {
        return dyn<rstd::any::Any>::from_ref(*this).as_mut_ref();
    }
    auto Wait(SubmissionToken token, Stages) -> Result<empty, Error> {
        dependencies.push(rstd::move(token));
        return Ok(empty {});
    }
};
struct FakeQueue {
    rstd::sync::Arc<Progress> progress = rstd::sync::Arc<Progress>::make();
    Vec<Submission>           pending;
    auto                      Begin() -> Result<Submission, Error>;
    auto                      Submit(Submission work) -> Result<SubmissionToken, Error>;
    auto                      Poll() -> Result<empty, Error> {
        if (progress->failed) return Err(Error { ErrorKind::Backend });
        if (progress->completed == progress->submitted) pending.clear();
        return Ok(empty {});
    }
};
} // namespace vrento_test::texture

namespace rstd
{
template<>
struct Impl<TextureDevice, vrento_test::texture::FakeDevice>
    : ImplBase<vrento_test::texture::FakeDevice> {
    auto QuerySupport(const TextureDesc& d) const -> Result<empty, Error> {
        return self().QuerySupport(d);
    }
    auto CreateTexture(const TextureDesc& d) -> Result<TextureRef, Error> {
        return self().CreateTexture(d);
    }
    auto CreateView(const TextureRef& t, ViewDesc d) -> Result<TextureViewRef, Error> {
        return self().CreateView(t, d);
    }
    auto CreateSampler(const SamplerDesc& d) -> Result<SamplerRef, Error> {
        return self().CreateSampler(d);
    }
};
template<>
struct Impl<Dependency, vrento_test::texture::FakeDependency>
    : ImplBase<vrento_test::texture::FakeDependency> {
    auto IdentityValue() const -> Identity { return { 1, 1, 1 }; }
    auto Value() const -> Serial { return self().value; }
    auto Poll() const -> Result<bool, Error> {
        if (self().progress->failed) return Err(Error { ErrorKind::Backend });
        return Ok(self().progress->completed >= self().value);
    }
    auto Backend() const -> ref<dyn<any::Any>> {
        return dyn<any::Any>::from_ref(*self().progress).as_ref();
    }
};
template<>
struct Impl<TextureCommands, vrento_test::texture::FakeCommands>
    : ImplBase<vrento_test::texture::FakeCommands> {
    auto Upload(TextureUpload u) -> Result<empty, Error> { return self().Upload(rstd::move(u)); }
    auto UseTexture(TextureAccess a) -> Result<empty, Error> {
        return self().UseTexture(rstd::move(a));
    }
    auto UseSampledView(TextureViewRef v, SamplerRef s, Stages stages) -> Result<empty, Error> {
        return self().UseSampledView(rstd::move(v), rstd::move(s), stages);
    }
    void Retain(Box<dyn<any::Any>> owner) { self().Retain(rstd::move(owner)); }
    auto Backend() -> mut_ref<dyn<any::Any>> { return self().Backend(); }
    auto Wait(SubmissionToken t, Stages stages) -> Result<empty, Error> {
        return self().Wait(rstd::move(t), stages);
    }
};
template<>
struct Impl<SubmissionQueue, vrento_test::texture::FakeQueue>
    : ImplBase<vrento_test::texture::FakeQueue> {
    auto Begin() -> Result<Submission, Error> { return self().Begin(); }
    auto Submit(Submission work) -> Result<SubmissionToken, Error> {
        return self().Submit(rstd::move(work));
    }
    auto Poll() -> Result<empty, Error> { return self().Poll(); }
};
} // namespace rstd

namespace vrento_test::texture
{
auto FakeQueue::Begin() -> Result<Submission, Error> {
    return Ok(Submission(Box<dyn<TextureCommands>>::make(FakeCommands {})));
}
auto FakeQueue::Submit(Submission work) -> Result<SubmissionToken, Error> {
    if (! work.valid()) return Err(Error { ErrorKind::Invalid });
    if (progress->uncertain) {
        BackendAccess::Uncertain(work);
        pending.push(rstd::move(work));
        return Err(Error { ErrorKind::Backend });
    }
    if (progress->failed) return Err(Error { ErrorKind::Backend });
    ++progress->submitted;
    BackendAccess::Submitted(work);
    pending.push(rstd::move(work));
    return Ok(SubmissionToken(rstd::sync::Arc<dyn<Dependency>>::make(
        FakeDependency { progress.clone(), progress->submitted })));
}
TEST(Texture, PublicApi) {
    FakeDevice  receiver;
    auto        api = dyn<TextureDevice>::from_ref(receiver);
    TextureDesc desc;
    desc.extent     = { 8, 8 };
    desc.mip_levels = 4;
    rstd_assert(rstd::as<TextureDevice>(receiver).QuerySupport(desc).is_ok());
    rstd_assert(api->QuerySupport(desc).is_ok());
    auto texture = api->CreateTexture(desc).unwrap();
    auto view    = api->CreateView(texture, {}).unwrap();
    rstd_assert(view.description().range.mip_count == 4);
    rstd_assert(normalize(desc, { Dimension::D2, { Aspect::Color, 4 } }).is_err());
    TextureUpload upload;
    upload.target    = view;
    upload.width     = 3;
    upload.height    = 2;
    upload.row_pitch = 16;
    for (unsigned i = 0; i < 28; ++i) upload.bytes.push(u8(i));
    auto layout = validate_upload(upload).unwrap();
    rstd_assert(layout.row_bytes == 12 && layout.required_bytes == 28);
    upload.row_pitch = ~Serial {};
    rstd_assert(validate_upload(upload).is_err());
    auto pool   = TexturePool::create(api.as_mut_ref(), desc, 1).unwrap();
    auto write  = pool.acquire().unwrap();
    auto target = write.view();
    rstd_assert(target.permits(true));
    rstd_assert(pool.acquire().unwrap_err().kind == ErrorKind::Busy);
    write.cancel();
    rstd_assert(pool.acquire().is_err());
    target = {};
    rstd_assert(pool.stats().available == 1);
    rstd_assert(pool.acquire().is_ok());
    rstd_assert(api->CreateTexture(desc).unwrap().identity() != texture.identity());
    auto cube      = desc;
    cube.dimension = Dimension::Cube;
    cube.layers    = 6;
    rstd_assert(normalize(cube, { Dimension::Cube, { Aspect::Color, 0, 1, 0, 6 } }).is_ok());
    rstd_assert(normalize(cube, { Dimension::Cube, { Aspect::Color, 0, 1, 1, 5 } }).is_err());
    cube.layers = ~Count {};
    rstd_assert(validate(cube).is_err());
    auto compressed       = desc;
    compressed.extent     = { 6, 6 };
    compressed.mip_levels = 1;
    compressed.format     = TextureFormat::BC1;
    auto compressed_view  = api->CreateView(api->CreateTexture(compressed).unwrap(), {}).unwrap();
    TextureUpload block;
    block.target = compressed_view;
    block.x      = 4;
    block.y      = 4;
    block.width  = 2;
    block.height = 2;
    for (int i = 0; i < 8; ++i) block.bytes.push(u8(i));
    rstd_assert(validate_upload(block).unwrap().required_bytes == 8);
    block.x = 1;
    rstd_assert(validate_upload(block).is_err());
    SamplerDesc invalid_sampler;
    invalid_sampler.min_lod = -1;
    rstd_assert(validate_sampler(invalid_sampler).is_err());

    FakeQueue queue;
    auto      submission_api = dyn<SubmissionQueue>::from_ref(queue);
    int       dropped        = 0;
    {
        auto work = rstd::as<SubmissionQueue>(queue).Begin().unwrap();
        work.retain(DropProbe(dropped));
        auto moved = rstd::move(work);
        rstd_assert(! work.valid() && moved.valid());
    }
    rstd_assert(dropped == 1);
    auto work    = submission_api->Begin().unwrap();
    auto sampler = api->CreateSampler({}).unwrap();
    rstd_assert(
        work.commands()->UseSampledView(view, sampler, Stages::Vertex | Stages::Compute).is_ok());
    upload.row_pitch = 16;
    rstd_assert(work.commands()->Upload(upload.clone()).is_ok());
    rstd_assert(work.commands()->Wait({}, Stages::Fragment).is_ok());
    work.retain(DropProbe(dropped));
    auto receipt = work.receipt();
    rstd_assert(! receipt.submitted());
    auto token = submission_api->Submit(rstd::move(work)).unwrap();
    rstd_assert(! work.valid() && receipt.submitted() && ! token.poll().unwrap());
    auto copied_token      = token;
    token                  = {};
    queue.progress->failed = true;
    rstd_assert(copied_token.poll().unwrap_err().kind == ErrorKind::Backend);
    rstd_assert(submission_api->Poll().is_err() && dropped == 1);
    queue.progress->failed    = false;
    queue.progress->completed = copied_token.value();
    rstd_assert(copied_token.poll().unwrap());
    rstd_assert(submission_api->Poll().is_ok() && dropped == 2);
    int                    released = 0;
    Option<TextureViewRef> kept;
    {
        auto owned = TextureRef(
            { 1, 1, 99 }, desc, rstd::sync::Arc<dyn<rstd::any::Any>>::make(DropProbe(released)));
        kept = Some(api->CreateView(owned, {}).unwrap());
    }
    rstd_assert(released == 0);
    kept = None();
    rstd_assert(released == 1);
    vrento::Image oversized;
    oversized.header.format = TextureFormat::RGBA32F;
    vrento::Image::Slot oversized_slot;
    oversized_slot.width = oversized_slot.height = 0x7fffffff;
    rstd::uint8_t source_byte {};
    oversized_slot.mipmaps.push(vrento::ImageData {
        0x7fffffff, 0x7fffffff, isize(1), vrento::ImageDataPtr(&source_byte, [](rstd::uint8_t*) {
        }) });
    oversized.slots.push(rstd::move(oversized_slot));
    vrento::texture::ImagePrepareContext preparer(api.as_mut_ref());
    rstd_assert(
        rstd::as<TextureContentPreparer>(preparer).PrepareImage(oversized).unwrap_err().kind ==
        ErrorKind::Invalid);
}
TEST(Texture, MetadataAndContentDescriptionsAgree) {
    for (auto kind : array<ImageKind, 2> { ImageKind::Pixels, ImageKind::Video }) {
        Image image;
        image.header = { .kind = kind, .format = TextureFormat::BC3 };
        Vec<ImageSlotDescription> slots;
        for (int index {}; index < 2; ++index) {
            Image::Slot slot;
            slot.width  = 64 >> index;
            slot.height = 32 >> index;
            for (int mip {}; mip < 3; ++mip)
                slot.mipmaps.push(
                    ImageData { .width = slot.width >> mip, .height = slot.height >> mip });
            slots.push({ slot.width, slot.height, slot.mipmaps.len() });
            image.slots.push(rstd::move(slot));
        }
        auto metadata = describe_image(image.header, slots.as_slice()).unwrap();
        auto content  = describe_image(image).unwrap();
        ASSERT_EQ(metadata.len(), content.len());
        for (usize index {}; index < content.len(); ++index)
            EXPECT_EQ(metadata[index], content[index]);
        EXPECT_EQ(metadata[usize()].mip_levels, kind == ImageKind::Video ? 1u : 3u);
        EXPECT_EQ(metadata[usize()].format,
                  kind == ImageKind::Video ? TextureFormat::RGBA8 : TextureFormat::BC3);
        for (auto count : array<usize, 2> { usize(), usize(33) }) {
            slots[usize()].mip_count = count;
            EXPECT_TRUE(describe_image(image.header, slots.as_slice()).is_err());
        }
        slots[usize()].mip_count = usize(3);
        slots[usize()].width     = 0;
        EXPECT_TRUE(describe_image(image.header, slots.as_slice()).is_err());
        EXPECT_TRUE(describe_image(image.header, {}).is_err());
    }
}

TEST(Texture, ImageUploadsOwnEveryMipPayload) {
    FakeDevice          device;
    auto                api = dyn<TextureDevice>::from_ref(device);
    ImagePrepareContext preparer(api.as_mut_ref());
    for (auto format : array<TextureFormat, 2> { TextureFormat::RGBA8, TextureFormat::BC1 }) {
        auto  layout = format_layout(format);
        Image image;
        image.header.format = format;
        unsigned released {};
        for (Count slot_index {}; slot_index < 2; ++slot_index) {
            Image::Slot slot;
            slot.width  = 64 >> slot_index;
            slot.height = 32 >> slot_index;
            for (Count mip {}; mip < 3; ++mip) {
                auto  width  = Count(slot.width) >> mip;
                auto  height = Count(slot.height) >> mip;
                auto  size   = ((width + layout.block_width - 1) / layout.block_width) *
                               ((height + layout.block_height - 1) / layout.block_height) *
                               layout.block_bytes;
                auto* bytes  = new rstd::uint8_t[size];
                for (Serial index {}; index < size; ++index)
                    bytes[index] = rstd::uint8_t((index + slot_index + mip) % 251);
                slot.mipmaps.push(ImageData { rstd::int32_t(width),
                                              rstd::int32_t(height),
                                              isize(size),
                                              ImageDataPtr(bytes, [&released](rstd::uint8_t* data) {
                                                  ++released;
                                                  delete[] data;
                                              }) });
            }
            image.slots.push(rstd::move(slot));
        }
        auto prepared = preparer.PrepareImage(image).unwrap();
        image.slots.clear();
        EXPECT_EQ(released, 6u);
        ASSERT_EQ(prepared.len(), usize(2));
        for (usize slot {}; slot < prepared.len(); ++slot) {
            ASSERT_EQ(prepared[slot].uploads.len(), usize(3));
            EXPECT_EQ(prepared[slot].view.texture().description().format, format);
            for (usize mip {}; mip < prepared[slot].uploads.len(); ++mip) {
                const auto& upload = prepared[slot].uploads[mip];
                EXPECT_EQ(upload.mip, mip.to_primitive());
                EXPECT_EQ(upload.width, 64u >> (slot.to_primitive() + mip.to_primitive()));
                EXPECT_EQ(upload.height, 32u >> (slot.to_primitive() + mip.to_primitive()));
                EXPECT_EQ(validate_upload(upload).unwrap().required_bytes,
                          upload.bytes.len().to_primitive());
                for (usize index {}; index < upload.bytes.len(); ++index)
                    ASSERT_EQ(upload.bytes[index], u8((index + slot + mip).to_primitive() % 251));
            }
        }
    }
}
TEST(Texture, ImageUploadsRejectMismatchedPayloads) {
    FakeDevice          device;
    auto                api = dyn<TextureDevice>::from_ref(device);
    ImagePrepareContext preparer(api.as_mut_ref());
    Image               image;
    Image::Slot         slot;
    slot.width = slot.height = 2;
    array<rstd::uint8_t, 16> bytes {};
    slot.mipmaps.push(ImageData { 2, 2, isize(16), ImageDataPtr(bytes.data(), [](rstd::uint8_t*) {
                                  }) });
    image.slots.push(rstd::move(slot));
    for (auto size : array<int, 4> { -1, 0, 15, 17 }) {
        image.slots[usize()].mipmaps[usize()].size = isize(size);
        EXPECT_EQ(preparer.PrepareImage(image).unwrap_err().kind, ErrorKind::Invalid);
    }
}
TEST(Texture, ShaderAccessStages) {
    FakeDevice  device;
    TextureDesc desc;
    desc.extent = { 8, 8 };
    desc.usage  = Usage::Sampled | Usage::Storage;
    auto   view = device.CreateView(device.CreateTexture(desc).unwrap(), {}).unwrap();
    Access accesses[] { Access::Sampled, Access::StorageRead, Access::StorageWrite };
    for (auto access : accesses) {
        rstd_assert(validate_access({ view, access, Stages::Geometry }).is_ok());
        rstd_assert(validate_access({ view, access, Stages::Shaders }).is_ok());
        rstd_assert(
            validate_access({ view, access, Stages::Geometry | Stages::Transfer }).is_err());
        rstd_assert(validate_access({ view, access, Stages::None }).is_err());
    }
}
TEST(Texture, MultisampleValidation) {
    TextureDesc desc;
    desc.extent  = { 8, 8 };
    desc.samples = 4;
    desc.usage   = Usage::ColorAttachment | Usage::Sampled | Usage::TransferSource;
    rstd_assert(validate(desc).is_ok());
    Count invalid_samples[] { 0, 3, 128 };
    for (Count invalid : invalid_samples) {
        desc.samples = invalid;
        rstd_assert(validate(desc).unwrap_err().kind == ErrorKind::Invalid);
    }
    desc.samples    = 4;
    desc.mip_levels = 2;
    rstd_assert(validate(desc).is_err());
    desc.mip_levels = 1;
    desc.format     = TextureFormat::BC1;
    rstd_assert(validate(desc).is_err());
    desc.format = TextureFormat::RGBA8;
    desc.usage  = Usage::ColorAttachment | Usage::TransferDestination;
    FakeDevice    factory;
    auto          texture = factory.CreateTexture(desc).unwrap();
    auto          view    = factory.CreateView(texture, {}).unwrap();
    TextureUpload upload;
    upload.target = view;
    upload.width = upload.height = 1;
    for (int i = 0; i < 4; ++i) upload.bytes.push(u8(0));
    rstd_assert(validate_upload(upload).is_err());
}
TEST(Texture, SubmissionOutcomes) {
    FakeQueue queue;
    auto      cancelled    = queue.Begin().unwrap();
    auto      cancellation = cancelled.receipt();
    auto      replacement  = queue.Begin().unwrap();
    cancelled              = rstd::move(replacement);
    rstd_assert(cancellation.cancelled());
    auto rejected          = queue.Begin().unwrap();
    auto rejection         = rejected.receipt();
    queue.progress->failed = true;
    rstd_assert(queue.Submit(rstd::move(rejected)).is_err());
    rstd_assert(rejection.cancelled() && ! rejection.submitted());
    queue.progress->failed    = false;
    queue.progress->uncertain = true;
    int  dropped              = 0;
    auto possible             = queue.Begin().unwrap();
    possible.retain(DropProbe(dropped));
    auto uncertainty = possible.receipt();
    rstd_assert(queue.Submit(rstd::move(possible)).is_err());
    rstd_assert(uncertainty.uncertain() && ! uncertainty.cancelled() && dropped == 0);
    queue.pending.clear();
    rstd_assert(uncertainty.uncertain() && dropped == 1);
}
} // namespace vrento_test::texture
