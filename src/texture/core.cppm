export module vrento:texture.core;
export import :types.texture;
export import :texture.image;
import rstd;
import :vulkan.error;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento::texture
{

using Serial = rstd::uint64_t;
using Count  = rstd::uint32_t;

enum class ErrorKind
{
    Invalid,
    Unsupported,
    DeviceMismatch,
    Stale,
    Busy,
    Closed,
    Lost,
    Backend,
    Timeout
};
struct Error {
    ErrorKind                    kind;
    rstd::int32_t                native {};
    vulkan::BackendErrorSnapshot backend;
    static auto                  from_backend(vulkan::BackendError error) -> Error {
        auto native = error.cause.native();
        return { error.cause.lost() ? ErrorKind::Lost : ErrorKind::Backend,
                 static_cast<rstd::int32_t>(native),
                 vulkan::BackendErrorSnapshot(rstd::move(error)) };
    }
};
enum class Dimension
{
    D2,
    Array2D,
    Cube
};
enum class Aspect
{
    Color,
    Depth
};
enum class Usage : Count
{
    None                = 0,
    Sampled             = 1,
    ColorAttachment     = 2,
    DepthAttachment     = 4,
    TransferSource      = 8,
    TransferDestination = 16,
    Storage             = 32
};
constexpr auto operator|(Usage a, Usage b) -> Usage { return Usage(Count(a) | Count(b)); }
constexpr bool has(Usage a, Usage b) { return (Count(a) & Count(b)) == Count(b); }
enum class Stages : Count
{
    None     = 0,
    Vertex   = 1,
    Fragment = 2,
    Compute  = 4,
    Transfer = 8,
    Color    = 16,
    Depth    = 32,
    Geometry = 64,
    Shaders  = Vertex | Fragment | Compute | Geometry,
    All      = 127
};
constexpr auto operator|(Stages a, Stages b) -> Stages { return Stages(Count(a) | Count(b)); }
enum class Access
{
    Undefined,
    Sampled,
    ColorAttachment,
    DepthAttachment,
    TransferSource,
    TransferDestination,
    StorageRead,
    StorageWrite
};
struct Extent {
    Count       width {}, height {}, depth { 1 };
    friend bool operator==(const Extent&, const Extent&) = default;
};
struct Identity {
    Serial      device {}, epoch {}, resource {};
    friend bool operator==(const Identity&, const Identity&) = default;
};
struct TextureDesc {
    Dimension     dimension { Dimension::D2 };
    Extent        extent;
    Count         mip_levels { 1 }, layers { 1 }, samples { 1 };
    TextureFormat format { TextureFormat::RGBA8 };
    Usage         usage { Usage::Sampled | Usage::TransferDestination };
    friend bool   operator==(const TextureDesc&, const TextureDesc&) = default;
};
struct Range {
    Aspect      aspect { Aspect::Color };
    Count       first_mip {}, mip_count {}, first_layer {}, layer_count {};
    friend bool operator==(const Range&, const Range&) = default;
};
struct ViewDesc {
    Dimension   dimension { Dimension::D2 };
    Range       range;
    friend bool operator==(const ViewDesc&, const ViewDesc&) = default;
};
struct SamplerDesc {
    TextureSample sample;
    TextureWrap   wrap_w { TextureWrap::REPEAT };
    float         min_lod {}, max_lod { 1000 }, lod_bias {};
};
inline auto validate_sampler(const SamplerDesc& desc) -> Result<empty, Error> {
    auto& sample = desc.sample;
    if (Count(sample.wrapS) > 3 || Count(sample.wrapT) > 3 || Count(desc.wrap_w) > 3 ||
        Count(sample.magFilter) > 1 || Count(sample.minFilter) > 1 || Count(sample.mipFilter) > 2 ||
        Count(sample.compare_op) > 7 || Count(sample.border_color) > 2 ||
        ! rstd::f32(desc.min_lod).is_finite() || ! rstd::f32(desc.max_lod).is_finite() ||
        ! rstd::f32(desc.lod_bias).is_finite() || desc.min_lod < 0 || desc.max_lod < desc.min_lod ||
        (sample.mipFilter == TextureMipmapFilter::None && desc.min_lod != 0))
        return Err(Error { ErrorKind::Invalid });
    return Ok(empty {});
}
struct FormatLayout {
    Count  block_width { 1 }, block_height { 1 }, block_bytes {};
    Aspect aspect { Aspect::Color };
};
inline auto format_layout(TextureFormat format) -> FormatLayout {
    switch (format) {
    case TextureFormat::BC1: return { 4, 4, 8 };
    case TextureFormat::BC2:
    case TextureFormat::BC3: return { 4, 4, 16 };
    case TextureFormat::RGB8: return { 1, 1, 3 };
    case TextureFormat::RGBA8:
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::BGRA8:
    case TextureFormat::R11G11B10F:
    case TextureFormat::BGRA8_SRGB: return { 1, 1, 4 };
    case TextureFormat::RG8: return { 1, 1, 2 };
    case TextureFormat::R8: return { 1, 1, 1 };
    case TextureFormat::D32F: return { 1, 1, 4, Aspect::Depth };
    case TextureFormat::RGBA16F: return { 1, 1, 8 };
    case TextureFormat::RGBA32F: return { 1, 1, 16 };
    }
    return {};
}
inline auto validate(const TextureDesc& desc) -> Result<empty, Error> {
    if ((desc.dimension != Dimension::D2 && desc.dimension != Dimension::Array2D &&
         desc.dimension != Dimension::Cube) ||
        ! desc.extent.width || ! desc.extent.height || desc.extent.depth != 1 || ! desc.layers ||
        ! desc.mip_levels || desc.mip_levels > 32 || desc.layers > ~Count {} / desc.mip_levels ||
        ! format_layout(desc.format).block_bytes || ! Count(desc.usage) ||
        (Count(desc.usage) & ~Count(63)))
        return Err(Error { ErrorKind::Invalid });
    if (! desc.samples || desc.samples > 64 || (desc.samples & (desc.samples - 1)))
        return Err(Error { ErrorKind::Invalid });
    if (desc.samples != 1 && (desc.mip_levels != 1 || desc.dimension == Dimension::Cube ||
                              format_layout(desc.format).block_width != 1))
        return Err(Error { ErrorKind::Invalid });
    if ((desc.dimension == Dimension::D2 && desc.layers != 1) ||
        (desc.dimension == Dimension::Cube &&
         (desc.layers % 6 || desc.extent.width != desc.extent.height)))
        return Err(Error { ErrorKind::Invalid });
    Count limit  = desc.extent.width > desc.extent.height ? desc.extent.width : desc.extent.height;
    Count levels = 1;
    while (limit > 1) {
        limit /= 2;
        ++levels;
    }
    if (desc.mip_levels > levels) return Err(Error { ErrorKind::Invalid });
    bool depth = format_layout(desc.format).aspect == Aspect::Depth;
    if ((depth && (has(desc.usage, Usage::ColorAttachment) || has(desc.usage, Usage::Storage))) ||
        (! depth && has(desc.usage, Usage::DepthAttachment)))
        return Err(Error { ErrorKind::Invalid });
    return Ok(empty {});
}
inline auto normalize(const TextureDesc& texture, ViewDesc view) -> Result<ViewDesc, Error> {
    if (validate(texture).is_err() ||
        (view.dimension != Dimension::D2 && view.dimension != Dimension::Array2D &&
         view.dimension != Dimension::Cube))
        return Err(Error { ErrorKind::Invalid });
    auto& r = view.range;
    if (r.aspect != format_layout(texture.format).aspect || r.first_mip >= texture.mip_levels ||
        r.first_layer >= texture.layers)
        return Err(Error { ErrorKind::Invalid });
    if (! r.mip_count) r.mip_count = texture.mip_levels - r.first_mip;
    if (! r.layer_count) r.layer_count = texture.layers - r.first_layer;
    if (r.mip_count > texture.mip_levels - r.first_mip ||
        r.layer_count > texture.layers - r.first_layer ||
        (view.dimension == Dimension::D2 && r.layer_count != 1) ||
        (view.dimension == Dimension::Cube &&
         (texture.dimension != Dimension::Cube || r.first_layer % 6 || r.layer_count % 6)))
        return Err(Error { ErrorKind::Invalid });
    return Ok(view);
}
inline auto mip_extent(const TextureDesc& desc, Count mip) -> Extent {
    return { rstd::cmp::max(Count(1), desc.extent.width >> mip),
             rstd::cmp::max(Count(1), desc.extent.height >> mip),
             1 };
}
struct Dependency {
    using Trait                  = Dependency;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = Dependency;
        auto IdentityValue() const -> Identity { return rstd::trait_call<0>(this); }
        auto Value() const -> Serial { return rstd::trait_call<1>(this); }
        auto Poll() const -> Result<bool, Error> { return rstd::trait_call<2>(this); }
        auto Backend() const -> ref<dyn<rstd::any::Any>> { return rstd::trait_call<3>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::IdentityValue, &T::Value, &T::Poll, &T::Backend>;
};
class SubmissionToken {
    Option<Arc<dyn<Dependency>>> dependency_;

public:
    SubmissionToken() = default;
    SubmissionToken(const SubmissionToken& other)
        : dependency_(other.dependency_.is_some() ? Some(other.dependency_->clone()) : None()) {}
    auto operator=(const SubmissionToken& other) -> SubmissionToken& {
        if (this != &other)
            dependency_ = other.dependency_.is_some() ? Some(other.dependency_->clone()) : None();
        return *this;
    }
    SubmissionToken(SubmissionToken&& other): dependency_(other.dependency_.take()) {}
    auto operator=(SubmissionToken&& other) -> SubmissionToken& {
        if (this != &other) dependency_ = other.dependency_.take();
        return *this;
    }
    explicit SubmissionToken(Arc<dyn<Dependency>> dependency)
        : dependency_(Some(rstd::move(dependency))) {}
    bool valid() const { return dependency_.is_some() && bool(*dependency_); }
    auto identity() const -> Identity { return (*dependency_)->IdentityValue(); }
    auto value() const -> Serial { return (*dependency_)->Value(); }
    auto poll() const -> Result<bool, Error> { return valid() ? (*dependency_)->Poll() : Ok(true); }
    auto backend() const -> ref<dyn<rstd::any::Any>> { return (*dependency_)->Backend(); }
    friend bool operator==(const SubmissionToken& a, const SubmissionToken& b) {
        return a.valid() == b.valid() &&
               (! a.valid() || (a.identity() == b.identity() && a.value() == b.value()));
    }
};
struct ResourceStatus {
    using Trait                  = ResourceStatus;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = ResourceStatus;
        auto Check() const -> Result<empty, Error> { return rstd::trait_call<0>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Check>;
};
struct AccessState {
    Access access { Access::Undefined };
    Stages stages { Stages::None };
};
struct StorageState {
    Identity                         identity;
    TextureDesc                      desc;
    Arc<dyn<rstd::any::Any>>         backing;
    Vec<AccessState>                 states;
    Option<Arc<dyn<ResourceStatus>>> status;
    Serial                           recording {}, pool {}, writer {}, version {};
    Serial                           content_revision {};
    Count                            prepared_reads {};
    Count                            prepared_native_reads {};
    SubmissionToken                  last_write, last_use;
    StorageState(Identity id, TextureDesc d, Arc<dyn<rstd::any::Any>> owner)
        : identity(id), desc(d), backing(rstd::move(owner)) {
        for (Count layer = 0; layer < desc.layers; ++layer)
            for (Count mip = 0; mip < desc.mip_levels; ++mip) states.push(AccessState {});
    }
};
class TexturePool;
class TextureViewRef;
struct BackendAccess;
class TextureRef {
    Option<Arc<StorageState>> state_;
    friend class TexturePool;
    friend class TextureViewRef;
    friend struct BackendAccess;

public:
    TextureRef() = default;
    TextureRef(const TextureRef& other)
        : state_(other.state_.is_some() ? Some(other.state_->clone()) : None()) {}
    auto operator=(const TextureRef& other) -> TextureRef& {
        if (this != &other) {
            state_ = other.state_.is_some() ? Some(other.state_->clone()) : None();
        }
        return *this;
    }
    TextureRef(TextureRef&& other): state_(other.state_.take()) {}
    auto operator=(TextureRef&& other) -> TextureRef& {
        if (this != &other) state_ = other.state_.take();
        return *this;
    }
    TextureRef(Identity id, TextureDesc desc, Arc<dyn<rstd::any::Any>> owner)
        : state_(Some(Arc<StorageState>::make(id, desc, rstd::move(owner)))) {}
    bool valid() const { return bool(state_); }
    auto identity() const -> Identity { return (*state_)->identity; }
    auto description() const -> const TextureDesc& { return (*state_)->desc; }
    auto check() const -> Result<empty, Error> {
        if (! valid()) return Err(Error { ErrorKind::Invalid });
        return (*state_)->status.is_some() ? (*(*state_)->status)->Check() : Ok(empty {});
    }
    auto ready() const -> SubmissionToken { return (*state_)->last_write; }
    auto content_revision() const -> Serial { return (*state_)->content_revision; }

private:
    auto backend_state() const -> const Arc<StorageState>& { return *state_; }
};
struct ViewState {
    TextureRef               texture;
    ViewDesc                 desc;
    Arc<dyn<rstd::any::Any>> backing;
    ViewState(TextureRef t, ViewDesc d, Arc<dyn<rstd::any::Any>> b)
        : texture(rstd::move(t)), desc(d), backing(rstd::move(b)) {}
};
enum class Permission
{
    Ordinary,
    Read,
    Write
};
class TexturePool;
class TextureViewRef {
    Option<Arc<ViewState>>           state_;
    Option<Arc<dyn<rstd::any::Any>>> pin_;
    Permission                       permission_ { Permission::Ordinary };
    Serial                           version_ {};
    friend class TexturePool;
    friend struct BackendAccess;

public:
    TextureViewRef() = default;
    TextureViewRef(const TextureViewRef& other)
        : state_(other.state_.is_some() ? Some(other.state_->clone()) : None()) {
        pin_        = other.pin_.is_some() ? Some(other.pin_->clone()) : None();
        permission_ = other.permission_;
        version_    = other.version_;
    }
    auto operator=(const TextureViewRef& other) -> TextureViewRef& {
        if (this != &other) {
            state_      = other.state_.is_some() ? Some(other.state_->clone()) : None();
            pin_        = other.pin_.is_some() ? Some(other.pin_->clone()) : None();
            permission_ = other.permission_;
            version_    = other.version_;
        }
        return *this;
    }
    TextureViewRef(TextureViewRef&& other)
        : state_(other.state_.take()),
          pin_(other.pin_.take()),
          permission_(other.permission_),
          version_(other.version_) {}
    auto operator=(TextureViewRef&& other) -> TextureViewRef& {
        if (this != &other) {
            state_      = other.state_.take();
            pin_        = other.pin_.take();
            permission_ = other.permission_;
            version_    = other.version_;
        }
        return *this;
    }
    explicit TextureViewRef(Arc<ViewState> state): state_(Some(rstd::move(state))) {}
    TextureViewRef(TextureRef t, ViewDesc d, Arc<dyn<rstd::any::Any>> b)
        : state_(Some(Arc<ViewState>::make(rstd::move(t), d, rstd::move(b)))) {}
    bool valid() const { return bool(state_); }
    auto texture() const -> const TextureRef& { return (*state_)->texture; }
    auto description() const -> const ViewDesc& { return (*state_)->desc; }
    bool permits(bool write) const {
        auto& s = *texture().backend_state();
        if (! s.pool) return permission_ == Permission::Ordinary;
        if (permission_ == Permission::Write) return s.writer == version_ && version_ != 0;
        return ! write && permission_ == Permission::Read && s.version == version_ && ! s.writer;
    }

private:
    auto backend() const -> ref<dyn<rstd::any::Any>> { return (*state_)->backing.deref(); }
    auto backend_state() const -> const Arc<ViewState>& { return *state_; }
};
struct SamplerState {
    Identity                 identity;
    SamplerDesc              desc;
    Arc<dyn<rstd::any::Any>> backing;
};
class SamplerRef {
    Option<Arc<SamplerState>> state_;
    friend struct BackendAccess;

public:
    SamplerRef() = default;
    SamplerRef(const SamplerRef& other)
        : state_(other.state_.is_some() ? Some(other.state_->clone()) : None()) {}
    auto operator=(const SamplerRef& other) -> SamplerRef& {
        if (this != &other) {
            state_ = other.state_.is_some() ? Some(other.state_->clone()) : None();
        }
        return *this;
    }
    SamplerRef(SamplerRef&& other): state_(other.state_.take()) {}
    auto operator=(SamplerRef&& other) -> SamplerRef& {
        if (this != &other) state_ = other.state_.take();
        return *this;
    }
    SamplerRef(Identity id, SamplerDesc desc, Arc<dyn<rstd::any::Any>> owner)
        : state_(Some(Arc<SamplerState>::make(SamplerState { id, desc, rstd::move(owner) }))) {}
    bool valid() const { return bool(state_); }
    auto identity() const -> Identity { return (*state_)->identity; }
    auto description() const -> const SamplerDesc& { return (*state_)->desc; }

private:
    auto backend() const -> ref<dyn<rstd::any::Any>> { return (*state_)->backing.deref(); }
};
class Submission;
// Backend construction and commit hooks; consumers use references and traits.
struct BackendAccess {
    static void Submitted(Submission& work);
    static void Uncertain(Submission& work);
    static auto Storage(const TextureRef& value) -> const Arc<StorageState>& {
        return value.backend_state();
    }
    static auto View(const TextureViewRef& value) -> ref<dyn<rstd::any::Any>> {
        return value.backend();
    }
    static auto ViewStorage(const TextureViewRef& value) -> const Arc<ViewState>& {
        return value.backend_state();
    }
    static auto Sampler(const SamplerRef& value) -> ref<dyn<rstd::any::Any>> {
        return value.backend();
    }
};
class TextureUploadPayload {
    struct Storage {
        Vec<u8>            owned;
        Option<Arc<Image>> image;
        usize              slot {}, mip {};
        explicit Storage(Vec<u8> bytes): owned(rstd::move(bytes)) {}
        Storage(Arc<Image> source, usize source_slot, usize source_mip)
            : image(Some(rstd::move(source))), slot(source_slot), mip(source_mip) {}
        auto bytes() const -> slice<u8> {
            if (! image) return owned.as_slice();
            const auto& data = (*image)->slots[slot].mipmaps[mip];
            return slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(data.data.get()),
                                             usize(data.size.to_primitive()));
        }
    };
    Option<Arc<Storage>> storage_;
    explicit TextureUploadPayload(Arc<Storage> storage): storage_(Some(rstd::move(storage))) {}

public:
    TextureUploadPayload() = default;
    TextureUploadPayload(Vec<u8> bytes): storage_(Some(Arc<Storage>::make(rstd::move(bytes)))) {}
    TextureUploadPayload(const TextureUploadPayload&)                    = delete;
    auto operator=(const TextureUploadPayload&) -> TextureUploadPayload& = delete;
    TextureUploadPayload(TextureUploadPayload&&)                         = default;
    auto operator=(TextureUploadPayload&&) -> TextureUploadPayload&      = default;
    // Published image contents must remain immutable while uploads share them.
    static auto FromImage(Arc<Image> image, usize slot, usize mip)
        -> Result<TextureUploadPayload, Error> {
        if (! image || image->header.kind != ImageKind::Pixels || slot >= image->slots.len() ||
            mip >= image->slots[slot].mipmaps.len())
            return Err(Error { ErrorKind::Invalid });
        const auto& data = image->slots[slot].mipmaps[mip];
        if (! data.data || data.size <= isize()) return Err(Error { ErrorKind::Invalid });
        return Ok(TextureUploadPayload(Arc<Storage>::make(rstd::move(image), slot, mip)));
    }
    auto as_slice() const -> slice<u8> { return storage_ ? (*storage_)->bytes() : slice<u8> {}; }
    auto len() const -> usize { return as_slice().len(); }
    bool is_empty() const { return len() == usize(); }
    auto operator[](usize index) const -> u8 { return as_slice()[index]; }
    auto clone() const -> TextureUploadPayload {
        return storage_ ? TextureUploadPayload(storage_->clone()) : TextureUploadPayload {};
    }
};
struct TextureUpload {
    TextureViewRef       target;
    Count                mip {}, layer {}, x {}, y {}, width {}, height {};
    Serial               row_pitch {}, slice_pitch {};
    TextureUploadPayload bytes;
    auto                 clone() const -> TextureUpload {
        return { target, mip, layer, x, y, width, height, row_pitch, slice_pitch, bytes.clone() };
    }
};
struct UploadLayout {
    Serial row_bytes {}, rows {}, source_pitch {}, required_bytes {};
};
inline auto validate_upload(const TextureUpload& upload) -> Result<UploadLayout, Error> {
    if (! upload.target.valid() || ! upload.target.permits(true))
        return Err(Error { ErrorKind::Invalid });
    auto& desc  = upload.target.texture().description();
    auto  range = upload.target.description().range;
    if (desc.samples != 1 || ! has(desc.usage, Usage::TransferDestination) ||
        upload.mip >= desc.mip_levels || upload.layer >= desc.layers ||
        upload.mip < range.first_mip || upload.mip - range.first_mip >= range.mip_count ||
        upload.layer < range.first_layer || upload.layer - range.first_layer >= range.layer_count)
        return Err(Error { ErrorKind::Invalid });
    auto size = mip_extent(desc, upload.mip);
    if (! upload.width || ! upload.height || upload.x >= size.width || upload.y >= size.height ||
        upload.width > size.width - upload.x || upload.height > size.height - upload.y)
        return Err(Error { ErrorKind::Invalid });
    auto f = format_layout(desc.format);
    if (upload.x % f.block_width || upload.y % f.block_height ||
        (upload.width % f.block_width && upload.x + upload.width != size.width) ||
        (upload.height % f.block_height && upload.y + upload.height != size.height))
        return Err(Error { ErrorKind::Invalid });
    Serial row   = ((Serial(upload.width) + f.block_width - 1) / f.block_width) * f.block_bytes;
    Serial rows  = (Serial(upload.height) + f.block_height - 1) / f.block_height;
    Serial pitch = upload.row_pitch ? upload.row_pitch : row;
    if (pitch < row || (rows > 1 && pitch > (~Serial {} - row) / (rows - 1)))
        return Err(Error { ErrorKind::Invalid });
    Serial required = pitch * (rows - 1) + row;
    if (required > upload.bytes.len().to_primitive() ||
        (upload.slice_pitch && upload.slice_pitch < required))
        return Err(Error { ErrorKind::Invalid });
    return Ok(UploadLayout { row, rows, pitch, required });
}
struct TextureAccess {
    TextureViewRef view;
    Access         access { Access::Sampled };
    Stages         stages { Stages::Fragment };
    bool           discard {};
    Option<Range>  range;
    bool           require_contents {};
};
inline bool writes(Access access) {
    return access == Access::ColorAttachment || access == Access::DepthAttachment ||
           access == Access::TransferDestination || access == Access::StorageWrite;
}
inline auto validate_access(const TextureAccess& use) -> Result<Range, Error> {
    if (! use.view.valid() || ! use.view.permits(writes(use.access)) || ! Count(use.stages) ||
        (Count(use.stages) & ~Count(Stages::All)) || (use.discard && ! writes(use.access)))
        return Err(Error { ErrorKind::Invalid });
    Usage required;
    switch (use.access) {
    case Access::Sampled: required = Usage::Sampled; break;
    case Access::ColorAttachment: required = Usage::ColorAttachment; break;
    case Access::DepthAttachment: required = Usage::DepthAttachment; break;
    case Access::TransferSource: required = Usage::TransferSource; break;
    case Access::TransferDestination: required = Usage::TransferDestination; break;
    case Access::StorageRead:
    case Access::StorageWrite: required = Usage::Storage; break;
    default: return Err(Error { ErrorKind::Invalid });
    }
    if (! has(use.view.texture().description().usage, required) ||
        ((use.access == Access::Sampled || use.access == Access::StorageRead ||
          use.access == Access::StorageWrite) &&
         (Count(use.stages) & ~Count(Stages::Shaders))))
        return Err(Error { ErrorKind::Invalid });
    auto range = use.range.is_some() ? *use.range : use.view.description().range;
    auto normalized =
        normalize(use.view.texture().description(),
                  { range.layer_count == 1 ? Dimension::D2 : Dimension::Array2D, range });
    if (normalized.is_err()) return Err(normalized.unwrap_err());
    range        = normalized.unwrap().range;
    auto visible = use.view.description().range;
    if (range.aspect != visible.aspect || range.first_mip < visible.first_mip ||
        range.first_layer < visible.first_layer ||
        range.first_mip + range.mip_count > visible.first_mip + visible.mip_count ||
        range.first_layer + range.layer_count > visible.first_layer + visible.layer_count)
        return Err(Error { ErrorKind::Invalid });
    return Ok(range);
}
struct TextureDevice {
    using Trait                  = TextureDevice;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureDevice;
        auto QuerySupport(const TextureDesc& desc) const -> Result<empty, Error> {
            return rstd::trait_call<0>(this, desc);
        }
        auto CreateTexture(const TextureDesc& desc) -> Result<TextureRef, Error> {
            return rstd::trait_call<1>(this, desc);
        }
        auto CreateView(const TextureRef& t, ViewDesc desc) -> Result<TextureViewRef, Error> {
            return rstd::trait_call<2>(this, t, desc);
        }
        auto CreateSampler(const SamplerDesc& desc) -> Result<SamplerRef, Error> {
            return rstd::trait_call<3>(this, desc);
        }
    };
    template<typename T>
    using Funcs =
        rstd::TraitFuncs<&T::QuerySupport, &T::CreateTexture, &T::CreateView, &T::CreateSampler>;
};
struct TextureCommands {
    using Trait                  = TextureCommands;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureCommands;
        auto Upload(TextureUpload upload) -> Result<empty, Error> {
            return rstd::trait_call<0>(this, rstd::move(upload));
        }
        auto UseTexture(TextureAccess access) -> Result<empty, Error> {
            return rstd::trait_call<1>(this, rstd::move(access));
        }
        auto UseSampledView(TextureViewRef view, SamplerRef sampler, Stages stages)
            -> Result<empty, Error> {
            return rstd::trait_call<2>(this, rstd::move(view), rstd::move(sampler), stages);
        }
        void Retain(Box<dyn<rstd::any::Any>> owner) {
            rstd::trait_call<3>(this, rstd::move(owner));
        }
        auto Backend() -> mut_ref<dyn<rstd::any::Any>> { return rstd::trait_call<4>(this); }
        auto Wait(SubmissionToken token, Stages stages) -> Result<empty, Error> {
            return rstd::trait_call<5>(this, rstd::move(token), stages);
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Upload, &T::UseTexture, &T::UseSampledView, &T::Retain,
                                   &T::Backend, &T::Wait>;
};
struct SubmissionState {
    enum class Status
    {
        Recording,
        Submitted,
        Cancelled,
        Uncertain
    };
    Status status { Status::Recording };
};
using SubmissionStatus = SubmissionState::Status;
class SubmissionReceipt {
    Arc<SubmissionState> state_;
    friend class Submission;
    explicit SubmissionReceipt(Arc<SubmissionState> state): state_(rstd::move(state)) {}

public:
    SubmissionReceipt(const SubmissionReceipt& other): state_(other.state_.clone()) {}
    auto operator=(const SubmissionReceipt& other) -> SubmissionReceipt& {
        if (this != &other) state_ = other.state_.clone();
        return *this;
    }
    SubmissionReceipt(SubmissionReceipt&&)                    = default;
    auto operator=(SubmissionReceipt&&) -> SubmissionReceipt& = default;
    auto status() const -> SubmissionStatus {
        return state_ ? state_->status : SubmissionStatus::Cancelled;
    }
    bool submitted() const { return status() == SubmissionStatus::Submitted; }
    bool cancelled() const { return status() == SubmissionStatus::Cancelled; }
    bool uncertain() const { return status() == SubmissionStatus::Uncertain; }
    bool same(const SubmissionReceipt& other) const {
        return Arc<SubmissionState>::ptr_eq(state_, other.state_);
    }
};
class Submission {
    Option<Box<dyn<TextureCommands>>> commands_;
    Arc<SubmissionState>              state_;
    friend struct BackendAccess;
    bool valid_ { true };
    void cancel() {
        if (valid_ && state_ && state_->status == SubmissionStatus::Recording)
            state_->status = SubmissionStatus::Cancelled;
    }

public:
    explicit Submission(Box<dyn<TextureCommands>> commands)
        : commands_(Some(rstd::move(commands))), state_(Arc<SubmissionState>::make()) {}
    Submission(const Submission&)                    = delete;
    auto operator=(const Submission&) -> Submission& = delete;
    Submission(Submission&& other)
        : commands_(rstd::move(other.commands_)),
          state_(rstd::move(other.state_)),
          valid_(rstd::exchange(other.valid_, false)) {}
    auto operator=(Submission&& other) -> Submission& {
        if (this != &other) {
            cancel();
            commands_ = rstd::move(other.commands_);
            state_    = rstd::move(other.state_);
            valid_    = rstd::exchange(other.valid_, false);
        }
        return *this;
    }
    ~Submission() { cancel(); }
    bool valid() const { return valid_; }
    void discard() {
        if (! valid_ || state_->status != SubmissionStatus::Recording) return;
        cancel();
        valid_    = false;
        commands_ = None();
    }
    auto receipt() const -> SubmissionReceipt { return SubmissionReceipt(state_.clone()); }
    auto commands() -> mut_ref<dyn<TextureCommands>> { return (*commands_).deref_mut(); }
    auto backend() -> mut_ref<dyn<rstd::any::Any>> { return (*commands_)->Backend(); }
    template<typename T>
    void retain(T value) {
        (*commands_)->Retain(Box<dyn<rstd::any::Any>>::make(rstd::move(value)));
    }
};
inline void BackendAccess::Submitted(Submission& work) {
    work.state_->status = SubmissionStatus::Submitted;
}
inline void BackendAccess::Uncertain(Submission& work) {
    work.state_->status = SubmissionStatus::Uncertain;
}
struct SubmissionQueue {
    using Trait                  = SubmissionQueue;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = SubmissionQueue;
        auto Begin() -> Result<Submission, Error> { return rstd::trait_call<0>(this); }
        auto Submit(Submission work) -> Result<SubmissionToken, Error> {
            return rstd::trait_call<1>(this, rstd::move(work));
        }
        auto Poll() -> Result<empty, Error> { return rstd::trait_call<2>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Begin, &T::Submit, &T::Poll>;
};
} // namespace vrento::texture

export namespace rstd
{
template<>
struct Impl<hash::Hash, vrento::texture::Identity> : ImplBase<vrento::texture::Identity> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(u64(this->self().device), state);
        hash::hash_into(u64(this->self().epoch), state);
        hash::hash_into(u64(this->self().resource), state);
    }
};
template<>
struct Impl<fmt::Display, vrento::texture::Error> : ImplBase<vrento::texture::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& value = this->self();
        return formatter.write_str(
            (rstd::format("texture error kind={} native={}", int(value.kind), value.native))
                .as_str());
    }
};
template<>
struct Impl<fmt::Debug, vrento::texture::Error> : ImplBase<vrento::texture::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, vrento::texture::Error> : ImplBase<vrento::texture::Error> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& value = this->self();
        if (value.backend) return Some(dyn<error::Error>::from_ref(value.backend.error()));
        return None();
    }
};
} // namespace rstd
