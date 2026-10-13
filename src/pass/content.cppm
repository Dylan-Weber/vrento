export module vrento:pass.content;
import rstd;
import :frame_graph.graph;
import :resource;
using namespace rstd::prelude;

export namespace vrento::pass
{
struct OutputRef {
    fg::Serial  scope {};
    fg::PassId  producer;
    fg::Index   index {};
    friend bool operator==(const OutputRef&, const OutputRef&) = default;
};
class ContentRef {
    fg::Serial                           scope_ {};
    fg::ResourceVersion                  version_;
    Option<OutputRef>                    output_;
    Option<resource::TextureResourceRef> texture_;
    Option<resource::BufferResourceRef>  buffer_;

public:
    ContentRef() = default;
    template<class Graph>
        requires(rstd::mtp::same_as<Graph, fg::FrameGraphBuilder> ||
                 rstd::mtp::same_as<Graph, fg::FrameGraph>)
    static auto bind(const Graph& graph, fg::ResourceVersion version,
                     Option<resource::TextureResourceRef> texture = None(),
                     Option<resource::BufferResourceRef>  buffer  = None())
        -> Result<ContentRef, fg::Error> {
        auto scope = [&] {
            if constexpr (rstd::mtp::same_as<Graph, fg::FrameGraphBuilder>)
                return graph.data().scope;
            else
                return graph.scope();
        }();
        auto versions = [&] {
            if constexpr (rstd::mtp::same_as<Graph, fg::FrameGraphBuilder>)
                return graph.data().versions.as_slice();
            else
                return graph.versions();
        }();
        auto node = graph.find_version(version);
        if (! node || (texture && buffer) || (texture && ! texture->valid()) ||
            (buffer && ! buffer->valid()))
            return Err(fg::Error { fg::ErrorKind::MissingResource, {}, version.resource });
        ContentRef result;
        result.scope_   = scope;
        result.version_ = version;
        result.texture_ = rstd::move(texture);
        result.buffer_  = rstd::move(buffer);
        if (node->writer) {
            fg::Index index {};
            for (const auto& candidate : versions) {
                if (candidate.id == version) break;
                if (candidate.writer == node->writer) ++index;
            }
            result.output_ = Some(OutputRef { scope, *node->writer, index });
        }
        return Ok(rstd::move(result));
    }
    static auto write(fg::FrameGraphBuilder& graph, fg::PassId pass, const ContentRef& previous,
                      bool reuse = false, bool inherits = false) -> Result<ContentRef, fg::Error> {
        auto base = previous.resolve(graph);
        if (base.is_err()) return Err(base.unwrap_err());
        auto version = graph.reserve_write(pass, *base, reuse, inherits);
        if (version.is_err()) return Err(version.unwrap_err());
        return bind(graph, *version, previous.texture(), previous.buffer());
    }
    auto        first_output() const -> bool { return version_.version == 1; }
    static auto from_version(fg::Serial scope, fg::ResourceVersion version) -> ContentRef {
        ContentRef result;
        result.scope_   = scope;
        result.version_ = version;
        return result;
    }
    auto version() const -> fg::ResourceVersion { return version_; }
    auto valid() const -> bool { return scope_ && version_.resource.valid(); }
    auto initial() const -> bool { return version_.version == 0; }
    auto output() const -> Option<OutputRef> { return output_; }
    auto texture() const -> Option<resource::TextureResourceRef> { return texture_.clone(); }
    auto buffer() const -> Option<resource::BufferResourceRef> { return buffer_.clone(); }
    auto resolve(fg::Serial scope) const -> Result<fg::ResourceVersion, fg::Error> {
        if (! valid() || scope_ != scope)
            return Err(fg::Error { fg::ErrorKind::Conflict, {}, version_.resource });
        return Ok(fg::ResourceVersion(version_));
    }
    template<class Graph>
        requires(rstd::mtp::same_as<Graph, fg::FrameGraphBuilder> ||
                 rstd::mtp::same_as<Graph, fg::FrameGraph>)
    auto resolve(const Graph& graph) const -> Result<fg::ResourceVersion, fg::Error> {
        auto mapped = bind(graph, version_, texture(), buffer());
        if (mapped.is_err()) return Err(mapped.unwrap_err());
        if (mapped->scope_ != scope_ || (output_ && mapped->output() != output_))
            return Err(fg::Error { fg::ErrorKind::Conflict,
                                   output_ ? output_->producer : fg::PassId {},
                                   version_.resource });
        return Ok(fg::ResourceVersion(version_));
    }
    auto clone() const -> ContentRef {
        return { scope_, version_, output_, texture_.clone(), buffer_.clone() };
    }
    ContentRef(const ContentRef& other)
        : scope_(other.scope_),
          version_(other.version_),
          output_(other.output_),
          texture_(other.texture_.clone()),
          buffer_(other.buffer_.clone()) {}
    ContentRef(ContentRef&&)                    = default;
    auto operator=(ContentRef&&) -> ContentRef& = default;
    auto operator=(const ContentRef& other) -> ContentRef& {
        scope_   = other.scope_;
        version_ = other.version_;
        output_  = other.output_;
        texture_ = other.texture_.clone();
        buffer_  = other.buffer_.clone();
        return *this;
    }

private:
    ContentRef(fg::Serial scope, fg::ResourceVersion version, Option<OutputRef> output,
               Option<resource::TextureResourceRef> texture,
               Option<resource::BufferResourceRef>  buffer)
        : scope_(scope),
          version_(version),
          output_(output),
          texture_(rstd::move(texture)),
          buffer_(rstd::move(buffer)) {}
};
} // namespace vrento::pass
