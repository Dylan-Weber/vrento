export module vrento:resource.buffer;
import rstd;
import :resource.handle;

using namespace rstd::prelude;
using rstd::bytes::Bytes;

export namespace vrento::resource
{

class BufferContent {
    Bytes bytes_;
    usize size_ {};
    bool  zeroed_ {};

public:
    BufferContent() = default;
    BufferContent(BufferContent&& other) noexcept { *this = rstd::move(other); }
    auto operator=(BufferContent&& other) noexcept -> BufferContent& {
        if (this != &other) {
            bytes_  = rstd::move(other.bytes_);
            size_   = rstd::exchange(other.size_, usize());
            zeroed_ = rstd::exchange(other.zeroed_, false);
        }
        return *this;
    }
    static auto from(Bytes bytes) -> BufferContent {
        BufferContent result;
        result.size_  = bytes.len();
        result.bytes_ = rstd::move(bytes);
        return result;
    }
    template<typename T, typename A>
        requires requires(Vec<T, A> bytes) { Bytes::from_vec(rstd::move(bytes)); }
    static auto from(Vec<T, A> bytes) -> BufferContent {
        return from(Bytes::from_vec(rstd::move(bytes)));
    }
    static auto from(slice<u8> bytes) -> BufferContent {
        return from(Bytes::copy_from_slice(bytes));
    }
    static auto zeroed(usize size) -> BufferContent {
        BufferContent result;
        result.size_   = size;
        result.zeroed_ = true;
        return result;
    }
    auto as_slice() const -> slice<u8> { return bytes_.as_slice(); }
    auto len() const -> usize { return size_; }
    auto is_zeroed() const -> bool { return zeroed_; }
    auto clone() const -> BufferContent {
        BufferContent result;
        result.bytes_  = bytes_.clone();
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
