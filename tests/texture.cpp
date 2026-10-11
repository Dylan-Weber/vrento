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
} // namespace vrento_test::texture
