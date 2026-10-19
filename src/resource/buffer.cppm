export module vrento:resource.buffer;
import rstd;
import :resource.handle;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento::resource
{

class BufferContent {
    Option<Arc<dyn<rstd::any::Any>>> owner_;
    slice<u8>                        bytes_;
    usize                            size_ {};
    bool                             zeroed_ {};

public:
    BufferContent() = default;
    BufferContent(BufferContent&& other) noexcept { *this = rstd::move(other); }
    auto operator=(BufferContent&& other) noexcept -> BufferContent& {
        if (this != &other) {
            owner_  = other.owner_.take();
            bytes_  = rstd::exchange(other.bytes_, {});
            size_   = rstd::exchange(other.size_, usize());
            zeroed_ = rstd::exchange(other.zeroed_, false);
        }
        return *this;
    }
    template<typename A>
    static auto from(Vec<u8, A> bytes) -> BufferContent {
        BufferContent result;
        if (! bytes.is_empty()) {
            result.bytes_ = bytes.as_slice();
            result.size_  = bytes.len();
            result.owner_ = Some(Arc<dyn<rstd::any::Any>>::make(rstd::move(bytes)));
        }
        return result;
    }
    static auto from(slice<u8> bytes) -> BufferContent { return from(Vec<u8>::from(bytes)); }
    static auto zeroed(usize size) -> BufferContent {
        BufferContent result;
        result.size_   = size;
        result.zeroed_ = true;
        return result;
    }
    auto as_slice() const -> slice<u8> { return bytes_; }
    auto len() const -> usize { return size_; }
    auto is_zeroed() const -> bool { return zeroed_; }
    auto clone() const -> BufferContent {
        BufferContent result;
        result.owner_  = owner_.clone();
        result.bytes_  = bytes_;
        result.size_   = size_;
        result.zeroed_ = zeroed_;
        return result;
    }
};

struct BufferDefinitionId {
    u32 index { u32::MAX };
    u64 generation {};

    bool Valid() const noexcept { return index != u32::MAX && generation != u64(); }

    friend bool operator==(const BufferDefinitionId&, const BufferDefinitionId&) = default;
};

enum class BufferUsage
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Transfer,
};

enum class BufferLifetimeClass
{
    FrameLocal,
    Retained,
    Dynamic,
};

struct BufferDefinition {
    usize       size { 0 };
    BufferUsage usage { BufferUsage::Vertex };
    usize       alignment { 1 };

    friend bool operator==(const BufferDefinition&, const BufferDefinition&) = default;
};

struct BufferRequest {
    String                     name;
    Option<BufferDefinitionId> source;
    BufferDefinition           definition;
    BufferLifetimeClass        lifetime { BufferLifetimeClass::Retained };
    u64                        content_version { 1 };
    // Versions are local to a content source; replacing it must invalidate uploads.
    u64 content_identity {};

    auto clone() const -> BufferRequest {
        return BufferRequest {
            .name             = name.clone(),
            .source           = source,
            .definition       = definition,
            .lifetime         = lifetime,
            .content_version  = content_version,
            .content_identity = content_identity,
        };
    }
};

using BufferResourceRef = ResourceReference<BufferRequest, BufferResourceTag>;

} // namespace vrento::resource
