module;
#include <rstd/enum.hpp>

export module vrento:resource.error;
import rstd;
export import :error_diagnostic;
export import :preparation;
import :texture.core;
import :resource.handle;
import :vulkan.error;

using namespace rstd::prelude;

export namespace vrento::resource
{

enum class ResourceErrorKind
{
    MissingDefinition,
    MissingContent,
    BackendFailure,
    Invalid,
    InvalidUse,
    DuplicateUse,
    RevisionMismatch,
    IncompatibleLayout,
    Busy,
    StaleTransaction,
    Capacity,
    Unsupported,
    Cancelled,
    Closed,
    Device,
    Worker,
};

class ResourceErrorSource {
    RSTD_ENUM(ResourceErrorSource, (Texture, (texture::Error source;)),
              (Native, (rstd::int32_t code;)), (Backend, (vulkan::BackendError source;)),
              (External, (Box<dyn<rstd::error::Error>> source;)),
              (Io, (rstd::io::error::Error source;)))
};

struct TextureErrorContext {
    ResourceHandle<TextureResourceTag> resource;
    u64                                revision;
    TextureUseHandle                   use;
    String                             name;
    auto clone() const -> TextureErrorContext { return { resource, revision, use, name.clone() }; }
};
class ResourceErrorContext {
    RSTD_ENUM(ResourceErrorContext,
              (Buffer,
               (ResourceHandle<BufferResourceTag> resource; u64 revision; BufferUseHandle use;)),
              (Shader, (ShaderUseHandle use; String name;)),
              (Pipeline, (PipelineUseHandle use; RenderPassUseHandle render_pass;)),
              (Transaction, (u64 requested; u64 current;)))
public:
    auto clone() const -> ResourceErrorContext {
        if (is_Buffer()) {
            const auto& c = as_Buffer();
            return Buffer(c.resource, c.revision, c.use);
        }
        if (is_Shader()) {
            const auto& c = as_Shader();
            return Shader(c.use, c.name.clone());
        }
        if (is_Pipeline()) {
            const auto& c = as_Pipeline();
            return Pipeline(c.use, c.render_pass);
        }
        const auto& c = as_Transaction();
        return Transaction(c.requested, c.current);
    }
};
struct ResourceError {
    ResourceErrorKind           kind { ResourceErrorKind::BackendFailure };
    String                      message;
    Option<ResourceErrorSource> source;
    Vec<TextureErrorContext>    textures;
    Vec<ResourceErrorContext>   contexts;
    static auto external(String operation, Box<dyn<rstd::error::Error>> error) -> ResourceError {
        return { .message = rstd::move(operation),
                 .source  = Some(ResourceErrorSource::External(rstd::move(error))) };
    }
    static auto backend(vulkan::BackendError error) -> ResourceError {
        return { .message = error.operation.clone(),
                 .source  = Some(ResourceErrorSource::Backend(rstd::move(error))) };
    }
    static auto texture(texture::Error error, String operation) -> ResourceError {
        return { .message = rstd::move(operation),
                 .source  = Some(ResourceErrorSource::Texture(rstd::move(error))) };
    }
};

struct ResourceErrorSnapshot {
    ResourceErrorKind         kind { ResourceErrorKind::BackendFailure };
    ErrorDiagnostic           diagnostic;
    Vec<TextureErrorContext>  textures;
    Vec<ResourceErrorContext> contexts;
    auto                      clone() const -> ResourceErrorSnapshot {
        ResourceErrorSnapshot result { kind, diagnostic };
        for (const auto& texture : textures) result.textures.push(texture.clone());
        for (const auto& context : contexts) result.contexts.push(context.clone());
        return result;
    }
    static auto capture(const ResourceError&) -> ResourceErrorSnapshot;
};

} // namespace vrento::resource

export namespace rstd
{

template<>
struct Impl<clone::Clone, vrento::resource::ResourceErrorSnapshot>
    : DefaultInImpl<clone::Clone, vrento::resource::ResourceErrorSnapshot> {
    auto clone() const -> vrento::resource::ResourceErrorSnapshot { return self().clone(); }
};

template<>
struct Impl<fmt::Display, vrento::resource::ResourceErrorSource>
    : ImplBase<vrento::resource::ResourceErrorSource> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& value = this->self();
        if (value.is_Texture()) return as<fmt::Display>(value.as_Texture().source).fmt(formatter);
        if (value.is_Io()) return as<fmt::Display>(value.as_Io().source).fmt(formatter);
        if (value.is_External())
            return formatter.write_fmt(
                fmt::Arguments::make("{}", value.as_External().source.deref()));
        if (value.is_Backend()) return as<fmt::Display>(value.as_Backend().source).fmt(formatter);
        return formatter.write_str(
            rstd::format("backend error code={}", value.as_Native().code).as_str());
    }
};
template<>
struct Impl<fmt::Debug, vrento::resource::ResourceErrorSource>
    : ImplBase<vrento::resource::ResourceErrorSource> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, vrento::resource::ResourceErrorSource>
    : ImplBase<vrento::resource::ResourceErrorSource> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& e = this->self();
        if (e.is_Texture()) return Some(dyn<error::Error>::from_ref(e.as_Texture().source));
        if (e.is_Io()) return Some(dyn<error::Error>::from_ref(e.as_Io().source));
        if (e.is_External()) return Some(e.as_External().source.deref());
        if (e.is_Backend()) return Some(dyn<error::Error>::from_ref(e.as_Backend().source));
        return None();
    }
};

template<>
struct Impl<fmt::Display, vrento::resource::ResourceErrorContext>
    : ImplBase<vrento::resource::ResourceErrorContext> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& c = this->self();
        if (c.is_Buffer()) {
            const auto& v = c.as_Buffer();
            return f.write_fmt(fmt::Arguments::make("buffer={}:{} revision={} use={}:{}",
                                                    v.resource.index,
                                                    v.resource.generation,
                                                    v.revision,
                                                    v.use.index,
                                                    v.use.generation));
        }
        if (c.is_Shader()) {
            const auto& v = c.as_Shader();
            return f.write_fmt(
                fmt::Arguments::make("shader={} use={}:{}", v.name, v.use.index, v.use.generation));
        }
        if (c.is_Pipeline()) {
            const auto& v = c.as_Pipeline();
            return f.write_fmt(fmt::Arguments::make("pipeline-use={}:{} render-pass-use={}:{}",
                                                    v.use.index,
                                                    v.use.generation,
                                                    v.render_pass.index,
                                                    v.render_pass.generation));
        }
        const auto& v = c.as_Transaction();
        return f.write_fmt(
            fmt::Arguments::make("transaction={} current={}", v.requested, v.current));
    }
};

template<>
struct Impl<fmt::Display, vrento::resource::ResourceError>
    : ImplBase<vrento::resource::ResourceError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        if (! formatter.write_fmt(fmt::Arguments::make(
                "resource kind={}: {}", int(this->self().kind), this->self().message)))
            return false;
        for (const auto& context : this->self().contexts)
            if (! formatter.write_fmt(fmt::Arguments::make(" [{}]", context))) return false;
        for (const auto& texture : this->self().textures)
            if (! formatter.write_str(rstd::format(" [texture={} id={}:{} revision={} use={}:{}]",
                                                   texture.name,
                                                   texture.resource.index,
                                                   texture.resource.generation,
                                                   texture.revision,
                                                   texture.use.index,
                                                   texture.use.generation)
                                          .as_str()))
                return false;
        return true;
    }
};

template<>
struct Impl<fmt::Debug, vrento::resource::ResourceError>
    : ImplBase<vrento::resource::ResourceError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

template<>
struct Impl<error::Error, vrento::resource::ResourceError>
    : ImplBase<vrento::resource::ResourceError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        if (this->self().source) return Some(dyn<error::Error>::from_ref(*this->self().source));
        return None();
    }
};

} // namespace rstd

static_assert(rstd::Impled<vrento::resource::ResourceError, rstd::error::Error>);

namespace vrento::resource
{
inline auto ResourceErrorSnapshot::capture(const ResourceError& error) -> ResourceErrorSnapshot {
    ResourceErrorSnapshot result { error.kind, ErrorDiagnostic::capture(error) };
    for (const auto& texture : error.textures) result.textures.push(texture.clone());
    for (const auto& context : error.contexts) result.contexts.push(context.clone());
    return result;
}
} // namespace vrento::resource
