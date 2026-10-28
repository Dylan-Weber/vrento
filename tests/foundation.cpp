#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.test;
import vrento;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vrento;
using namespace vrento::vulkan;

TEST(ShaderCode, FreezesWithoutCopyAndKeepsStorageAlive) {
    ShaderCode            survivor;
    const rstd::uint32_t* pointer {};
    {
        auto words =
            Vec<rstd::uint32_t>::from(array<rstd::uint32_t, 3> { 0x07230203u, 2u, 3u }.as_slice());
        pointer   = words.data();
        auto code = ShaderCode::from(rstd::move(words));
        survivor  = code.clone();
        EXPECT_EQ(code.data(), pointer);
        EXPECT_EQ(survivor.data(), pointer);
    }
    EXPECT_EQ(survivor.data(), pointer);
    EXPECT_EQ(survivor.len(), usize(3));
    EXPECT_EQ(survivor[usize(2)], 3u);
}

TEST(ShaderCode, ComparesContentAndReplacesOnlyOneOwner) {
    const auto words = array<rstd::uint32_t, 2> { 1u, 2u };
    auto       first = ShaderCode::from(words.as_slice());
    auto       equal = ShaderCode::from(words.as_slice());
    EXPECT_NE(first.data(), equal.data());
    EXPECT_EQ(first, equal);
    auto retained = first.clone();
    first         = ShaderCode::from(array<rstd::uint32_t, 2> { 1u, 3u }.as_slice());
    EXPECT_NE(first, equal);
    EXPECT_EQ(retained, equal);
    EXPECT_EQ(retained[usize(1)], 2u);
    ShaderCode empty;
    EXPECT_TRUE(empty.is_empty());
    EXPECT_EQ(empty, empty.clone());
    EXPECT_EQ(empty, ShaderCode::from(Vec<rstd::uint32_t> {}));
    EXPECT_EQ(empty.data(), nullptr);
}

namespace vrento_test::foundation
{
struct Counts {
    int  preprocess {};
    int  compile {};
    int  reflect {};
    int  destroyed {};
    bool fail {};
};

struct FakeTextureLoader {
    rstd::sync::Arc<Image> image;

    auto LoadTexture(ref<str> key) const
        -> Result<rstd::sync::Arc<Image>, resource::ResourceError> {
        if (key != "pixels"_str) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = String::make("missing pixels"_str),
            });
        }
        return Ok(image.clone());
    }
};

} // namespace vrento_test::foundation

namespace rstd
{
template<>
struct Impl<resource::TextureLoader, vrento_test::foundation::FakeTextureLoader>
    : ImplBase<vrento_test::foundation::FakeTextureLoader> {
    auto LoadTexture(ref<str> key) const -> Result<sync::Arc<Image>, resource::ResourceError> {
        return this->self().LoadTexture(key);
    }
};
} // namespace rstd

namespace vrento_test::foundation
{

struct FakeTextureProvider {
    rstd::sync::Arc<Image> image;
    u64                    revision { 1 };

    auto ResolveTextureContent(const resource::TextureRequest& request) const
        -> Result<resource::ImportedTextureContentIdentity, resource::ResourceError> {
        return Ok(resource::ImportedTextureContentIdentity {
            .key      = request.name.clone(),
            .revision = revision,
        });
    }
    auto OpenTextureLoader() const
        -> Result<rstd::sync::Arc<dyn<resource::TextureLoader>>, resource::ResourceError> {
        return Ok(rstd::sync::Arc<dyn<resource::TextureLoader>>::make(
            FakeTextureLoader { image.clone() }));
    }
    auto ResolveVideoPlayback(const resource::TextureRequest&) const
        -> Option<rstd::sync::Arc<dyn<VideoPlayback>>> {
        return None();
    }
};

} // namespace vrento_test::foundation

namespace rstd
{
template<>
struct Impl<resource::TextureContentProvider, vrento_test::foundation::FakeTextureProvider>
    : ImplBase<vrento_test::foundation::FakeTextureProvider> {
    auto ResolveTextureContent(const resource::TextureRequest& request) const
        -> Result<resource::ImportedTextureContentIdentity, resource::ResourceError> {
        return this->self().ResolveTextureContent(request);
    }
    auto OpenTextureLoader() const
        -> Result<sync::Arc<dyn<resource::TextureLoader>>, resource::ResourceError> {
        return this->self().OpenTextureLoader();
    }
    auto ResolveVideoPlayback(const resource::TextureRequest& request) const
        -> Option<sync::Arc<dyn<VideoPlayback>>> {
        return this->self().ResolveVideoPlayback(request);
    }
};
} // namespace rstd

namespace vrento_test::foundation
{

struct PlaybackData {
    VideoPlaybackSnapshot snapshot;
    f64                   current {};
    Option<f64>           duration;
};

struct FakePlayback {
    PlaybackData* data;
    auto          Snapshot() const -> VideoPlaybackSnapshot { return data->snapshot; }
    void          PublishTime(f64 current, Option<f64> duration) const {
        data->current  = current;
        data->duration = duration;
    }
};

struct TraceData {
    u64                  next {};
    rstd::array<bool, 4> ended {};
};

struct FakeObserver {
    TraceData* data;
    auto       Begin(RenderEvent) const -> u64 {
        auto token = data->next;
        data->next += u64(1);
        return token;
    }
    void End(u64 token) const {
        auto index = rstd::as_cast<usize>(token);
        rstd_assert(! data->ended[index]);
        data->ended[index] = true;
    }
};

struct FakeShaderBackend {
    explicit FakeShaderBackend(Counts& value): counts(&value) {}
    FakeShaderBackend(const FakeShaderBackend&) = delete;
    FakeShaderBackend(FakeShaderBackend&& other) noexcept
        : counts(rstd::exchange(other.counts, nullptr)) {}
    ~FakeShaderBackend() {
        if (counts != nullptr) ++counts->destroyed;
    }

    bool Preprocess(ref<str> source, ShaderType stage, SourceLang lang, String& output) const {
        ++counts->preprocess;
        rstd_assert(stage == ShaderType::FRAGMENT && lang == SourceLang::Hlsl);
        if (counts->fail) return false;
        output = rstd::into(source);
        return true;
    }

    auto CompileAndLinkShaderUnits(slice<ShaderCompUnit> units, const ShaderCompOpt& options,
                                   Vec<Uni_ShaderSpv>& output) const
        -> Result<empty, vrento::resource::ResourceError> {
        ++counts->compile;
        if (counts->fail)
            return Err(vrento::resource::ResourceError {
                .message = String::make("test shader failure"_str) });
        rstd_assert(units.len() == usize(1) && options.target == VulkanTarget::Vulkan_1_1);
        auto stage         = Box<ShaderSpv>::make();
        stage->stage       = units[usize()].stage;
        stage->entry_point = units[usize()].entry_point.clone();
        stage->spirv       = ShaderCode::from(array<rstd::uint32_t, 1> { 0x07230203u }.as_slice());
        output.push(rstd::move(stage));
        return Ok(empty {});
    }

    bool GenReflect(slice<ShaderCode> codes, Vec<Uni_ShaderSpv>&, ShaderReflected& output) const {
        ++counts->reflect;
        rstd_assert(codes.len() == usize(1) && codes[usize()][usize()] == 0x07230203u);
        if (counts->fail) return false;
        (void)output.input_location_map.insert("position"_Str, ShaderReflected::Input {});
        return true;
    }

    Counts* counts;
};

} // namespace vrento_test::foundation

namespace rstd
{
template<>
struct Impl<VideoPlayback, vrento_test::foundation::FakePlayback>
    : ImplBase<vrento_test::foundation::FakePlayback> {
    auto Snapshot() const -> VideoPlaybackSnapshot { return this->self().Snapshot(); }
    void PublishTime(f64 current, Option<f64> duration) const {
        this->self().PublishTime(current, duration);
    }
};

template<>
struct Impl<RenderObserver, vrento_test::foundation::FakeObserver>
    : ImplBase<vrento_test::foundation::FakeObserver> {
    auto Begin(RenderEvent event) const -> u64 { return this->self().Begin(event); }
    void End(u64 token) const { this->self().End(token); }
};

template<>
struct Impl<ShaderBackend, vrento_test::foundation::FakeShaderBackend>
    : ImplBase<vrento_test::foundation::FakeShaderBackend> {
    bool Preprocess(ref<str> source, ShaderType stage, SourceLang lang, String& output) const {
        return this->self().Preprocess(source, stage, lang, output);
    }
    auto CompileAndLinkShaderUnits(slice<ShaderCompUnit> units, const ShaderCompOpt& options,
                                   Vec<Uni_ShaderSpv>& output) const
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().CompileAndLinkShaderUnits(units, options, output);
    }
    bool GenReflect(slice<ShaderCode> codes, Vec<Uni_ShaderSpv>& stages,
                    ShaderReflected& output) const {
        return this->self().GenReflect(codes, stages, output);
    }
};
} // namespace rstd

namespace vrento_test::foundation
{

static_assert(static_cast<int>(TextureFormat::BC1) == 0);
static_assert(static_cast<int>(TextureFormat::D32F) == 7);
static_assert(static_cast<int>(ShaderType::GEOMETRY) == 1);
static_assert(static_cast<int>(ShaderMatrixMajor::Row) == 1);
static_assert(static_cast<int>(ShaderMatrixAbi::Hlsl) == 1);
static_assert(static_cast<int>(CompareOp::Always) == 7);
static_assert(static_cast<int>(BlendMode::AlphaToCoverage) == 3);
static_assert(static_cast<int>(CullMode::Back) == 2);
static_assert(static_cast<int>(MeshPrimitive::TRIANGLE) == 1);
static_assert(static_cast<int>(FillMode::ASPECTCROP) == 2);
static_assert(TextureSample {}.wrapS == TextureWrap::REPEAT);
static_assert(TextureSample {}.minFilter == TextureFilter::NEAREST);
static_assert(! TextureSample {}.compare_enable);
static_assert(TextureSample {}.border_color == TextureBorderColor::OpaqueBlack);

void CheckImageDataLifetime() {
    int  released = 0;
    auto make     = [&] {
        auto token = Box<int>::make(1);
        return ImageDataPtr(new rstd::uint8_t[4],
                            [token = rstd::move(token), &released](rstd::uint8_t* data) {
                                released += *token;
                                delete[] data;
                            });
    };
    {
        auto  first  = make();
        auto  second = make();
        auto* pixels = first.get();
        auto  moved  = rstd::move(first);
        rstd_assert(! first && moved.get() == pixels && released == 0);
        second = rstd::move(moved);
        rstd_assert(! moved && second.get() == pixels && released == 1);
        second.reset();
        second.reset();
        rstd_assert(! second && released == 2);
    }
    rstd_assert(released == 2);
    {
        ImageDataPtr empty(nullptr, [&](rstd::uint8_t*) {
            ++released;
        });
        auto         moved = rstd::move(empty);
        moved.reset();
    }
    rstd_assert(released == 2);
    {
        Vec<ImageData> mipmaps;
        for (usize i {}; i < usize(20); ++i) {
            mipmaps.push(ImageData { .data = make() });
        }
        rstd_assert(released == 2);
        mipmaps.truncate(usize(3));
        rstd_assert(released == 19);
    }
    rstd_assert(released == 22);
}

void CheckPendingImageLifetime() {
    RecordedBufferUploads  buffers;
    RecordedImageUploads   images;
    BufferUploadBatchLease buffer_lease;
    ImageUploadBatchLease  image_lease;
    rstd_assert(! buffers.Valid() && ! images.Valid());
    rstd_assert(! buffer_lease.Valid() && buffer_lease.Tickets().is_empty());
    rstd_assert(! image_lease.Valid() && image_lease.Tickets().is_empty());

    BufferAllocation empty;
    auto             moved = rstd::move(empty);
    rstd_assert(! empty && ! moved);
    rstd_assert(moved.buffer() == VK_NULL_HANDLE && moved.offset() == 0 && moved.size() == 0);
}

TEST(Foundation, Resources) {
    CheckImageDataLifetime();
    CheckPendingImageLifetime();
    rstd_assert(ToVkType(TextureFormat::D32F) == VK_FORMAT_D32_SFLOAT);
    rstd_assert(ToVkType(TextureFormat::RGBA8_SRGB) == VK_FORMAT_R8G8B8A8_SRGB);
    rstd_assert(ToVkType(TextureFormat::RGBA32F) == VK_FORMAT_R32G32B32A32_SFLOAT);
    resource::TextureDefinition original, mip_filtered;
    mip_filtered.sample.mipFilter = TextureMipmapFilter::None;
    rstd_assert(! (original == mip_filtered));
    rstd_assert(ToVkType(TextureWrap::MIRRORED_REPEAT) == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
    rstd_assert(ToVkType(TextureFormat::BC1) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    rstd_assert(ToVkType(TextureWrap::CLAMP_TO_BORDER) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    rstd_assert(ToVkType(TextureFilter::NEAREST) == VK_FILTER_NEAREST);
    rstd_assert(ToVkType(CompareOp::GreaterEqual) == VK_COMPARE_OP_GREATER_OR_EQUAL);

    int released = 0;
    {
        auto image = rstd::sync::Arc<Image>::make();
        rstd_assert(image->header.kind == ImageKind::Pixels);
        image->slots.push(Image::Slot {});
        image->slots[usize()].mipmaps.push(ImageData {});
        image->slots[usize()].mipmaps[usize()].data =
            ImageDataPtr(new rstd::uint8_t[4], [&released](rstd::uint8_t* data) {
                delete[] data;
                ++released;
            });
        {
            auto retained = image.clone();
            image         = rstd::sync::Arc<Image>::make();
            rstd_assert(released == 0);
        }
        rstd_assert(released == 1);
    }
    rstd_assert(released == 1);

    {
        Option<rstd::sync::Arc<dyn<resource::TextureLoader>>> loader;
        {
            auto image = rstd::sync::Arc<Image>::make();
            image->slots.push(Image::Slot {});
            image->slots[usize()].mipmaps.push(ImageData {});
            image->slots[usize()].mipmaps[usize()].data =
                ImageDataPtr(new rstd::uint8_t[4], [&released](rstd::uint8_t* data) {
                    delete[] data;
                    ++released;
                });
            FakeTextureProvider provider { rstd::move(image) };
            auto                view = dyn<resource::TextureContentProvider>::from_ref(provider);
            resource::TextureRequest request { .name = String::make("pixels"_str) };
            rstd_assert(view->ResolveTextureContent(request).unwrap().revision == u64(1));
            provider.revision = u64(2);
            rstd_assert(view->ResolveTextureContent(request).unwrap().revision == u64(2));
            loader = Some(view->OpenTextureLoader().unwrap());
        }
        rstd_assert(released == 1);
        rstd_assert((*loader)->LoadTexture("missing"_str).is_err());
        auto first  = (*loader)->LoadTexture("pixels"_str).unwrap();
        auto second = (*loader)->LoadTexture("pixels"_str).unwrap();
        rstd_assert(rstd::addressof(*first) == rstd::addressof(*second));
        loader = None();
        rstd_assert(released == 1);
    }
    rstd_assert(released == 2);

    PlaybackData playback_data;
    playback_data.snapshot.playing       = false;
    playback_data.snapshot.seek_sequence = u64(7);
    auto playback = rstd::sync::Arc<dyn<VideoPlayback>>::make(FakePlayback { &playback_data });
    rstd_assert(! playback->Snapshot().playing);
    rstd_assert(playback->Snapshot().seek_sequence == u64(7));
    playback->PublishTime(f64(2.5), Some(f64(10)));
    rstd_assert(playback_data.current == f64(2.5) && playback_data.duration == Some(f64(10)));

    TraceData trace_data;
    {
        RenderTrace trace(rstd::sync::Arc<dyn<RenderObserver>>::make(FakeObserver { &trace_data }));
        auto        first  = RenderScope(trace, RenderEvent::TexturePlan);
        auto        moved  = rstd::move(first);
        auto        second = RenderScope(trace, RenderEvent::TextureDecode);
        second             = rstd::move(moved);
        rstd_assert(trace_data.ended[usize(1)] && ! trace_data.ended[usize(0)]);
        trace = {};
    }
    rstd_assert(trace_data.next == u64(2) && trace_data.ended[usize(0)]);
    {
        auto disabled = RenderScope({}, RenderEvent::Instance);
    }

    Counts counts;
    {
        auto   backend = Box<dyn<ShaderBackend>>::make(FakeShaderBackend(counts));
        auto   view    = backend.as_ref();
        String preprocessed;
        rstd_assert(
            view->Preprocess("source"_str, ShaderType::FRAGMENT, SourceLang::Hlsl, preprocessed));
        rstd_assert(preprocessed.as_str() == "source"_str);
        rstd::array<ShaderCompUnit, 1> units {
            ShaderCompUnit { ShaderType::FRAGMENT, "source"_Str, "entry"_Str, SourceLang::Hlsl },
        };
        Vec<Uni_ShaderSpv> stages;
        rstd_assert(
            view->CompileAndLinkShaderUnits(units.as_slice(), ShaderCompOpt {}, stages).is_ok());
        rstd_assert(stages.len() == usize(1) &&
                    stages[usize()]->entry_point.as_str() == "entry"_str);
        Vec<ShaderCode> codes;
        codes.push(stages[usize()]->spirv.clone());
        ShaderReflected reflected;
        rstd_assert(view->GenReflect(codes.as_slice(), stages, reflected));
        rstd_assert(reflected.input_location_map.contains_key("position"_str));
        counts.fail = true;
        rstd_assert(
            ! view->Preprocess("source"_str, ShaderType::FRAGMENT, SourceLang::Hlsl, preprocessed));
        rstd_assert(
            ! view->CompileAndLinkShaderUnits(units.as_slice(), ShaderCompOpt {}, stages).is_ok());
        rstd_assert(! view->GenReflect(codes.as_slice(), stages, reflected));
        rstd_assert(counts.destroyed == 0);
    }
    rstd_assert(counts.preprocess == 2 && counts.compile == 2 && counts.reflect == 2);
    rstd_assert(counts.destroyed == 1);
}
TEST(RenderFoundation, OutputRecipeRetainsShaderFailure) {
    Counts counts;
    counts.fail  = true;
    auto backend = Box<dyn<ShaderBackend>>::make(FakeShaderBackend(counts));
    auto result =
        texture_output_recipe(backend.as_ref(), {}, {}, {}, VK_FORMAT_R8G8B8A8_UNORM, true);
    ASSERT_TRUE(result.is_err());
    auto error = rstd::move(result).unwrap_err();
    EXPECT_TRUE(error.message.as_str() == "test shader failure"_str);
}
} // namespace vrento_test::foundation

TEST(BackendErrors, DispatchSnapshotOwnsCommandAndMemoryDetails) {
    using Cause = vrento::vulkan::BackendCause;
    auto make   = [] {
        auto               command = rstd::ffi::CString::from_raw_parts("vkCreateDevice");
        vvk::DispatchError failure {};
        failure.kind       = vvk::DispatchErrorKind::MissingCommand;
        failure.command    = command.as_ptr();
        failure.api_result = VK_ERROR_INITIALIZATION_FAILED;
        return vrento::vulkan::BackendError {
            "create device"_Str, Cause::Dispatch(vrento::vulkan::DispatchFailure::capture(failure))
        };
    };
    auto error    = make();
    auto snapshot = vrento::vulkan::BackendErrorSnapshot(error.clone());
    EXPECT_EQ(snapshot.error().cause.as_Dispatch().error.command.as_str(), "vkCreateDevice"_str);
    EXPECT_EQ(snapshot.error().cause.native(), VK_ERROR_INITIALIZATION_FAILED);
    EXPECT_TRUE(as<rstd::error::Error>(snapshot.error()).source().is_some());
    vvk::MemoryError memory { .kind = vvk::MemoryErrorKind::PoolCapacity, .allocation_size = 4096 };
    auto             cause = Cause::Memory(memory);
    EXPECT_EQ(cause.clone().as_Memory().error.allocation_size, 4096u);
}
TEST(BackendErrors, MissingExtensionSurvivesSessionFailure) {
    vrento::vulkan::Extension            extension { true, "VK_VRENTO_missing_extension"_str };
    vrento::vulkan::DeviceSessionOptions options;
    options.instance_extensions =
        slice<vrento::vulkan::Extension>::from_raw_parts(&extension, usize(1));
    auto result = vrento::vulkan::DeviceSession::create(options);
    ASSERT_TRUE(result.is_err());
    auto error = result.unwrap_err();
    ASSERT_TRUE(error.cause);
    ASSERT_TRUE(error.cause.error().cause.is_Unsupported());
    EXPECT_EQ(error.cause.error().cause.as_Unsupported().capability.as_str(), extension.name);
}

TEST(BackendErrors, BufferRangeBusyAndCancellation) {
    auto          session = DeviceSession::create({ .validation = true }).unwrap();
    BufferManager buffers(session.device());
    EXPECT_TRUE(buffers.TryAllocate({ .size = 16 }).unwrap_err().cause.is_Closed());
    buffers.init();
    EXPECT_TRUE(buffers.TryAllocate({ .size = 0 }).unwrap_err().cause.is_Invalid());
    auto          allocation = buffers.TryAllocate({ .size = 16 }).unwrap();
    rstd::uint8_t bytes[4] {};
    auto          data  = slice<rstd::uint8_t>::from_raw_parts(bytes, usize(4));
    auto          range = buffers.TryQueueWrite(allocation, data, 15).unwrap_err();
    EXPECT_TRUE(range.cause.is_Range());
    EXPECT_EQ(range.cause.as_Range().capacity, 16u);
    buffers.TryQueueWrite(allocation, data).unwrap();
    RecordedBufferUploads batch;
    buffers.TryPreparePendingUploads(batch).unwrap();
    EXPECT_TRUE(buffers.TryQueueWrite(allocation, data).unwrap_err().cause.is_Busy());
    batch = vrento::vulkan::RecordedBufferUploads {};
    buffers.TryQueueWrite(allocation, data).unwrap();
    EXPECT_EQ(session.validation_errors(), 0u);
}
TEST(BufferUploadCache, RetiresOnlyIdleBlocksAndReusesWorkingSet) {
    auto          session = DeviceSession::create({ .validation = true }).unwrap();
    auto          queue   = TextureContext::create(session).unwrap();
    BufferManager buffers(session.device());
    ASSERT_TRUE(buffers.init());
    constexpr VkDeviceSize block_size = 2 * 1024 * 1024;
    auto                   allocation = buffers.TryAllocate({ .size = 3 * block_size }).unwrap();
    Vec<rstd::uint8_t>     bytes;
    bytes.resize(usize(block_size), rstd::uint8_t());
    auto              now = rstd::time::Instant::now();
    UploadCachePolicy policy { .max_idle_bytes = 2 * block_size,
                               .idle_timeout   = rstd::time::Duration::from_secs(u64(2)) };
    auto              upload = [&](unsigned count) {
        for (unsigned i {}; i < count; ++i)
            buffers.TryQueueWrite(allocation, bytes.as_slice(), i * block_size).unwrap();
        EXPECT_FALSE(buffers.CollectUploadCache(policy, now));
        EXPECT_EQ(buffers.uploadCacheStatistics().active_bytes, count * bytes.len().to_primitive());
        auto                  work   = queue.Begin().unwrap();
        auto                  native = native_recording(work, session).unwrap();
        vvk::CommandBuffer    command(native, session.device().handle().Dispatch());
        RecordedBufferUploads recorded;
        buffers.TryPreparePendingUploads(recorded).unwrap();
        buffers.TryRecordUploads(command, recorded).unwrap();
        auto lease = buffers.CommitRecordedUploads(rstd::move(recorded)).unwrap();
        work.retain(rstd::move(lease));
        EXPECT_FALSE(buffers.CollectUploadCache(policy, now));
        auto token = queue.Submit(rstd::move(work)).unwrap();
        queue.wait(token).unwrap();
    };
    upload(3);
    EXPECT_EQ(buffers.uploadCacheStatistics().allocations, u64(3));
    EXPECT_TRUE(buffers.CollectUploadCache(policy, now));
    EXPECT_EQ(buffers.uploadCacheStatistics().idle_bytes, 2 * block_size);
    upload(2);
    EXPECT_EQ(buffers.uploadCacheStatistics().allocations, u64(3));
    EXPECT_TRUE(buffers.CollectUploadCache(policy, now));
    EXPECT_TRUE(buffers.CollectUploadCache(policy, now + rstd::time::Duration::from_secs(u64(1))));
    EXPECT_FALSE(buffers.CollectUploadCache(policy, now + policy.idle_timeout));
    EXPECT_EQ(buffers.uploadCacheStatistics().blocks, u64());

    bytes.resize(usize(3 * block_size), rstd::uint8_t());
    upload(1);
    EXPECT_FALSE(buffers.CollectUploadCache(policy, now));
    EXPECT_EQ(buffers.uploadCacheStatistics().blocks, u64());
    EXPECT_EQ(session.validation_errors(), 0u);
}

TEST(BackendErrors, TextureSnapshotPreservesAllocationFailure) {
    auto make = [] {
        vvk::MemoryError memory { .kind            = vvk::MemoryErrorKind::PoolCapacity,
                                  .allocation_size = 8192 };
        return vrento::texture::Error::from_backend(
            { "flush image upload"_Str, vrento::vulkan::BackendCause::Memory(memory) });
    };
    auto error    = make();
    auto wrapped  = vrento::resource::ResourceError::texture(error, "record image upload"_Str);
    auto snapshot = vrento::resource::ResourceErrorSnapshot(wrapped.clone());
    wrapped       = {};
    const auto& retained = snapshot.error().source->as_Texture().source;
    ASSERT_TRUE(retained.backend);
    EXPECT_EQ(retained.backend.error().cause.as_Memory().error.allocation_size, 8192u);
    EXPECT_TRUE(as<rstd::error::Error>(retained).source().is_some());
}
TEST(BackendErrors, DriverCacheOwnsIoPath) {
    vrento::vulkan::Device              device;
    vrento::vulkan::DriverPipelineCache cache;
    auto path  = rstd::path::PathBuf::from("/vrento-missing-directory/errors-cache-test"_str);
    auto error = cache.load(device, path.as_path()).unwrap_err();
    path       = rstd::path::PathBuf {};
    ASSERT_TRUE(error.cause);
    const auto& cause = error.cause.error().cause;
    ASSERT_TRUE(cause.is_Io());
    EXPECT_EQ(cause.as_Io().path.as_str(), "/vrento-missing-directory/errors-cache-test"_str);
    EXPECT_TRUE(as<rstd::error::Error>(cause).source().is_some());
}
