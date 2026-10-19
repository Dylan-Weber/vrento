export module vrento:draw_buffer;
export import :scene.geometry;
export import :scene.identity;
export import :resource;
import :resource_registry;
import rstd;

using namespace rstd::prelude;

export namespace vrento
{
enum class DrawBufferRole
{
    Uniform,
    Vertex,
    Index
};

struct DrawBufferKey {
    RenderItemId   render_item;
    DrawBufferRole role { DrawBufferRole::Vertex };
    u32            submesh_index {};
    u32            stream_index {};
    u64            data_generation {};
    u64            allocation_generation {};
    u64            storage_generation {};
    usize          capacity {};
    usize          stride {};
};

struct DrawBufferRefs {
    RenderItemId                      render_item;
    u64                               allocation_generation {};
    bool                              dynamic { false };
    bool                              content_confirmed { false };
    u32                               draw_count {};
    Vec<DrawBufferKey>                vertex_keys;
    Option<DrawBufferKey>             index_key;
    Vec<resource::BufferUseHandle>    vertices;
    Option<resource::BufferUseHandle> index;
    bool                              hasIndex() const { return index.is_some(); }
};

// The request and geometry bytes are borrowed only for the duration of each call.
struct DrawBufferRequest {
    RenderItemId                     render_item;
    const GeometryView&              geometry;
    u32                              submesh_index {};
    u64                              dynamic_allocation_generation {};
    slice<resource::BufferUseHandle> buffer_uses;
};

struct DrawBufferPrepareRequest {
    RenderItemId                     render_item;
    const GeometryLayout&            geometry;
    u32                              submesh_index {};
    u64                              dynamic_allocation_generation {};
    slice<resource::BufferUseHandle> buffer_uses;
};

enum class DrawBufferUpdateErrorKind
{
    InvalidGeometry,
    UploadFailed
};
enum class DrawUpdateOutcome
{
    Updated,
    NeedsReprepare
};
struct DrawBufferUpdateError {
    DrawBufferUpdateErrorKind       kind;
    String                          message;
    resource::ResourceErrorSnapshot source;
};

String BuildDrawBufferResourceName(DrawId, DrawBufferRole, u32 stream_index = u32());
auto   BuildDrawBufferKeys(const DrawBufferRequest&, u64 allocation_generation = u64())
    -> Vec<DrawBufferKey>;
auto BuildDrawBufferKeys(const DrawBufferPrepareRequest&, u64 allocation_generation = u64())
    -> Vec<DrawBufferKey>;

class RenderBufferResolver {
public:
    explicit RenderBufferResolver(const resource_registry::PreparedResourceTable& resources)
        : m_resources(resources) {}
    auto tryPrepareDrawBuffers(const DrawBufferRequest&)
        -> Result<DrawBufferRefs, resource::ResourceError>;
    auto tryPrepareDrawBuffers(const DrawBufferPrepareRequest&)
        -> Result<DrawBufferRefs, resource::ResourceError>;
    auto        prepareDrawBuffers(const DrawBufferRequest&) -> Option<DrawBufferRefs>;
    static auto updateDrawBuffers(const DrawBufferRequest&, DrawBufferRefs&,
                                  mut_ref<dyn<resource::BufferContentWriter>>)
        -> Result<DrawUpdateOutcome, DrawBufferUpdateError>;

private:
    const resource_registry::PreparedResourceTable& m_resources;
};
} // namespace vrento

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::DrawBufferUpdateError> : ImplBase<vrento::DrawBufferUpdateError> {
    auto fmt(fmt::Formatter& f) const -> bool { return f.write_str(this->self().message.as_str()); }
};
template<>
struct Impl<fmt::Debug, vrento::DrawBufferUpdateError> : ImplBase<vrento::DrawBufferUpdateError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::DrawBufferUpdateError> : ImplBase<vrento::DrawBufferUpdateError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& e = this->self();
        if (e.source) return Some(dyn<error::Error>::from_ref(e.source.error()));
        return None();
    }
};
} // namespace rstd
