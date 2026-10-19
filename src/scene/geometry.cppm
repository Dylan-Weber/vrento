export module vrento:scene.geometry;
export import :types.vertex;
export import :resource.buffer;
import rstd;
using namespace rstd::prelude;

namespace vrento::detail
{
template<typename T>
class GeometryArrayStorage {
    static_assert(rstd::mtp::same_as<T, float> || rstd::mtp::same_as<T, rstd::uint32_t>);
    rstd::bytes::BytesMut      writable_;
    Option<rstd::bytes::Bytes> published_;
    usize                      count_ {};

    template<typename A>
    static auto allocate(usize count, A allocator) -> rstd::bytes::BytesMut {
        auto values = Vec<T, A>::with_capacity_in(count, rstd::move(allocator));
        values.resize(count, T {});
        return rstd::bytes::BytesMut::from_vec(rstd::move(values));
    }

public:
    GeometryArrayStorage() = default;
    explicit GeometryArrayStorage(usize count)
        : writable_(allocate(count, rstd::alloc::Global {})), count_(count) {}
    explicit GeometryArrayStorage(slice<T> values)
        : writable_(rstd::bytes::BytesMut::from_vec(Vec<T>::from(values))), count_(values.len()) {}
    GeometryArrayStorage(GeometryArrayStorage&& other) noexcept
        : writable_(rstd::move(other.writable_)),
          published_(other.published_.take()),
          count_(rstd::exchange(other.count_, usize())) {}
    auto operator=(GeometryArrayStorage&& other) noexcept -> GeometryArrayStorage& {
        if (this != &other) {
            writable_  = rstd::move(other.writable_);
            published_ = other.published_.take();
            count_     = rstd::exchange(other.count_, usize());
        }
        return *this;
    }
    auto len() const -> usize { return count_; }
    auto data() const -> const T* {
        auto bytes = published_ ? published_->as_slice() : writable_.as_slice();
        return bytes.is_empty() ? nullptr : reinterpret_cast<const T*>(bytes.as_raw_ptr());
    }
    auto write(bool preserve = true) -> mut_ref<T[]> {
        if (published_) {
            auto recovered = rstd::move(published_.take().unwrap_unchecked()).try_into_mut();
            if (recovered.is_ok()) {
                writable_ = rstd::move(recovered).unwrap_unchecked();
            } else {
                auto source = rstd::move(recovered).unwrap_err_unchecked();
                writable_   = allocate(count_, source.allocator());
                if (preserve && ! source.is_empty())
                    rstd::mem::memcpy(writable_.data(), source.data(), source.len());
            }
        }
        // Every allocation contains initialized T objects before byte ownership is transferred.
        return mut_ref<T[]>::from_raw_parts(reinterpret_cast<T*>(writable_.data()), count_);
    }
    auto publish(usize count) -> resource::BufferContent {
        if (count > count_) rstd::panic { "geometry publication exceeds initialized storage" };
        if (! published_) published_ = Some(writable_.freeze());
        auto bytes = published_->clone();
        bytes.truncate(count * usize(sizeof(T)));
        return resource::BufferContent::from(rstd::move(bytes));
    }
};
} // namespace vrento::detail

export namespace vrento
{
struct GeometryBufferLayout {
    usize       byte_length {}, capacity {}, stride {}, count {};
    u64         data_generation {}, storage_generation {};
    friend bool operator==(const GeometryBufferLayout&, const GeometryBufferLayout&) = default;
};
} // namespace vrento

export namespace rstd
{
template<>
struct Impl<Copy, vrento::GeometryBufferLayout> {};
} // namespace rstd

export namespace vrento
{

struct GeometryLayout {
    Vec<GeometryBufferLayout>    vertices;
    Option<GeometryBufferLayout> index;
    bool                         dynamic {};
    friend bool                  operator==(const GeometryLayout&, const GeometryLayout&) = default;
};

// Bytes are the initialized prefix of the stream identified by storage_generation.
// Views borrow their array until mutation, move, or destruction; never retain across frames.
struct GeometryBufferView {
    slice<u8> bytes;
    usize     capacity {};
    usize     stride {};
    usize     count {};
    u64       data_generation {};
    u64       storage_generation {};
    auto      layout() const -> GeometryBufferLayout {
        return { bytes.len(), capacity, stride, count, data_generation, storage_generation };
    }
};

struct GeometryView {
    Vec<GeometryBufferView>    vertices;
    Option<GeometryBufferView> index;
    bool                       dynamic { false };
    auto                       layout() const -> GeometryLayout {
        GeometryLayout result { .dynamic = dynamic };
        result.vertices.reserve(vertices.len());
        for (const auto& buffer : vertices) result.vertices.push(buffer.layout());
        if (index) result.index = Some(index->layout());
        return result;
    }
};

class GeometrySnapshot {
    struct Buffer {
        resource::BufferContent bytes;
        usize                   capacity {}, stride {}, count {};
        u64                     data_generation {}, storage_generation {};
        auto                    view() const -> GeometryBufferView {
            return {
                bytes.as_slice(), capacity, stride, count, data_generation, storage_generation
            };
        }
        auto clone() const -> Buffer {
            return { bytes.clone(), capacity, stride, count, data_generation, storage_generation };
        }
    };
    Vec<Buffer>    vertices_;
    Option<Buffer> index_;
    bool           dynamic_ {};

public:
    static auto capture(const GeometryView& source, Option<ref<GeometrySnapshot>> previous = None())
        -> GeometrySnapshot {
        GeometrySnapshot result;
        result.dynamic_ = source.dynamic;
        auto buffer     = [](const GeometryBufferView& view, const Buffer* old) {
            const bool reuse = old && view.storage_generation != u64() &&
                               old->storage_generation == view.storage_generation &&
                               old->data_generation == view.data_generation &&
                               old->bytes.len() == view.bytes.len();
            return Buffer { reuse ? old->bytes.clone() : resource::BufferContent::from(view.bytes),
                            view.capacity,
                            view.stride,
                            view.count,
                            view.data_generation,
                            view.storage_generation };
        };
        result.vertices_.reserve(source.vertices.len());
        for (usize i {}; i < source.vertices.len(); ++i) {
            const auto* old =
                previous && i < (*previous)->vertices_.len() ? &(*previous)->vertices_[i] : nullptr;
            result.vertices_.push(buffer(source.vertices[i], old));
        }
        if (source.index)
            result.index_ = Some(buffer(
                *source.index, previous && (*previous)->index_ ? &*(*previous)->index_ : nullptr));
        return result;
    }
    auto view() const -> GeometryView {
        GeometryView result { .dynamic = dynamic_ };
        for (const auto& buffer : vertices_) result.vertices.push(buffer.view());
        if (index_) result.index = Some(index_->view());
        return result;
    }
    auto layout() const -> GeometryLayout {
        GeometryLayout result { .dynamic = dynamic_ };
        result.vertices.reserve(vertices_.len());
        for (const auto& buffer : vertices_) result.vertices.push(buffer.view().layout());
        if (index_) result.index = Some(index_->view().layout());
        return result;
    }
    static auto from(GeometryLayout layout, Vec<resource::BufferContent> vertices,
                     Option<resource::BufferContent> index = None()) -> Option<GeometrySnapshot> {
        if (layout.vertices.len() != vertices.len() || layout.index.is_some() != index.is_some())
            return None();
        GeometrySnapshot result;
        result.dynamic_ = layout.dynamic;
        auto buffer     = [](const GeometryBufferLayout& meta, resource::BufferContent bytes) {
            return Buffer { rstd::move(bytes), meta.capacity,        meta.stride,
                            meta.count,        meta.data_generation, meta.storage_generation };
        };
        for (usize i {}; i < vertices.len(); ++i) {
            if (vertices[i].is_zeroed() || vertices[i].len() != layout.vertices[i].byte_length)
                return None();
            result.vertices_.push(buffer(layout.vertices[i], rstd::move(vertices[i])));
        }
        if (index) {
            if (index->is_zeroed() || index->len() != layout.index->byte_length) return None();
            result.index_ = Some(buffer(*layout.index, index.take().unwrap()));
        }
        return Some(rstd::move(result));
    }
    auto vertexContent(usize index) const -> resource::BufferContent {
        return vertices_[index].bytes.clone();
    }
    auto indexContent() const -> Option<resource::BufferContent> {
        return index_ ? Some(index_->bytes.clone()) : None();
    }
    auto clone() const -> GeometrySnapshot {
        GeometrySnapshot result;
        result.dynamic_ = dynamic_;
        for (const auto& buffer : vertices_) result.vertices_.push(buffer.clone());
        if (index_) result.index_ = Some(index_->clone());
        return result;
    }
};

class IndexArray {
    constexpr static usize Unit_Byte_Size { sizeof(uint32_t) };

public:
    IndexArray(const IndexArray&)                    = delete;
    auto operator=(const IndexArray&) -> IndexArray& = delete;
    IndexArray(usize indexCount);
    IndexArray(slice<uint32_t> data);

    IndexArray(IndexArray&&) noexcept;
    ~IndexArray() = default;

    void Assign(usize index, slice<uint32_t> data) {
        if (index > m_data.len() || data.len() > m_data.len() - index) return;
        if (! IncreaseCheckSet((index + data.len()) * Unit_Byte_Size)) return;
        auto target = m_data.write();
        for (usize source_index {}; source_index < data.len(); ++source_index) {
            target[index + source_index] = data[source_index];
        }
        BumpDataGeneration();
    }

    const uint32_t* Data() const { return m_data.data(); }
    usize           DataCount() const { return m_size; }
    usize           DataSizeOf() const { return m_size * Unit_Byte_Size; }

    usize RenderDataCount() const noexcept {
        return m_render_size > m_size ? m_size : m_render_size;
    }
    void SetRenderDataCount(usize val) noexcept { m_render_size = val; }

    usize CapacityCount() const { return m_data.len(); }
    usize CapacitySizeof() const { return m_data.len() * Unit_Byte_Size; }
    u64   DataGeneration() const { return m_generation; }
    auto  BufferView() const -> GeometryBufferView;
    auto  PublishContent() -> resource::BufferContent { return m_data.publish(m_size); }

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

private:
    bool IncreaseCheckSet(usize size);
    void BumpDataGeneration() noexcept { ++m_generation; }

    detail::GeometryArrayStorage<uint32_t> m_data;
    usize                                  m_size { 0 };

    usize m_render_size { usize::MAX };

    u32 m_id { u32::MAX };
    u64 m_generation { 1 };
    u64 m_storage_generation;
};

struct VertexWriteResult {
    usize vertex_count {};
    usize capacity {};
    bool  overflowed { false };
};

class VertexWriter {
public:
    VertexWriter(const VertexWriter&) = delete;
    VertexWriter(VertexWriter&&)      = delete;
    auto AppendZeroedVertex() noexcept -> Option<mut_ref<float[]>>;

    usize Stride() const noexcept { return m_stride; }
    usize Capacity() const noexcept { return m_capacity; }
    usize Written() const noexcept { return m_written; }
    bool  Overflowed() const noexcept { return m_overflowed; }

private:
    friend class VertexArray;

    VertexWriter(mut_ref<float[]> data, usize stride) noexcept
        : m_data(data),
          m_stride(stride),
          m_capacity(stride == usize() ? usize() : data.len() / stride) {}

    mut_ref<float[]> m_data;
    usize            m_stride {};
    usize            m_capacity {};
    usize            m_written {};
    bool             m_overflowed { false };
};

class VertexArray {
public:
    struct VertexAttribute {
        String     name;
        VertexType type;
        bool       padding { true };

        auto clone() const -> VertexAttribute { return { name.clone(), type, padding }; }
        void clone_from(const VertexAttribute& source) { *this = source.clone(); }
    };

    VertexArray(const VertexArray&)                    = delete;
    auto operator=(const VertexArray&) -> VertexArray& = delete;
    VertexArray(Vec<VertexAttribute> attrs, usize count);
    VertexArray(initializer_list<VertexAttribute> attrs, usize count)
        : VertexArray(Vec<VertexAttribute>::from(slice<VertexAttribute>::from_raw_parts(
                          attrs.begin(), usize(attrs.size()))),
                      count) {}
    ~VertexArray() = default;

    VertexArray(VertexArray&&) noexcept;
    VertexArray& operator=(VertexArray&&) noexcept;

    bool AddVertex(const float*);
    bool SetVertex(ref<str> name, slice<float> data) noexcept;
    bool SetVertexs(usize index, slice<float> data) noexcept;

    template<typename Fill>
    [[nodiscard]] auto RewriteVertices(Fill&& fill) -> VertexWriteResult {
        if (m_rewriting) rstd::panic { "nested geometry rewrite" };
        VertexWriter writer(m_data.write(false), m_oneSize);
        m_rewriting = true;
        try {
            fill(writer);
        } catch (...) {
            (void)FinishVertexRewrite(writer);
            throw;
        }
        return FinishVertexRewrite(writer);
    }

    // Drops the active size to zero without releasing capacity. Subsequent
    // SetVertexs calls regrow it.
    void ResetSize() noexcept;

    const float* Data() const { return m_data.data(); }
    usize        DataSize() const { return m_size; }
    usize        DataSizeOf() const { return m_size * usize(sizeof(float)); }
    usize        VertexCount() const { return m_oneSize == usize() ? usize() : m_size / m_oneSize; }
    usize        CapacitySize() const { return m_data.len(); }
    usize        CapacitySizeOf() const { return m_data.len() * usize(sizeof(float)); }
    usize        OneSize() const { return m_oneSize; }
    usize        OneSizeOf() const { return m_oneSize * usize(sizeof(float)); }
    u64          DataGeneration() const { return m_generation; }
    auto         BufferView() const -> GeometryBufferView;
    auto         PublishContent() -> resource::BufferContent {
        if (m_rewriting) rstd::panic { "geometry publication during rewrite" };
        return m_data.publish(m_size);
    }

    auto Attributes() const -> slice<VertexAttribute> { return m_attributes.as_slice(); }
    auto AttributeOffset(ref<str> name) const -> Option<usize>;

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

    static usize TypeCount(VertexType);
    static usize RealAttributeSize(const VertexAttribute&);

private:
    bool TrySetSize(usize) noexcept;
    auto FinishVertexRewrite(const VertexWriter&) noexcept -> VertexWriteResult;
    void BumpDataGeneration() noexcept { ++m_generation; }

    Vec<VertexAttribute> m_attributes;

    detail::GeometryArrayStorage<float> m_data;
    usize                               m_oneSize { 0 };
    usize                               m_size { 0 };
    bool                                m_rewriting {};

    u32 m_id { u32::MAX };
    u64 m_generation { 1 };
    u64 m_storage_generation;
};

} // namespace vrento
