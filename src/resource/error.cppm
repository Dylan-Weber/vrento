module;
#include <rstd/enum.hpp>

export module vrento:resource.error;
import rstd;
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
              (External, (rstd::sync::Arc<dyn<rstd::error::Error>> source;)),
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
    static auto external(String operation, rstd::sync::Arc<dyn<rstd::error::Error>> error)
        -> ResourceError {
        return { .message = rstd::move(operation),
                 .source  = Some(ResourceErrorSource::External(rstd::move(error))) };
    }
    static auto backend(vulkan::BackendError error) -> ResourceError {
        return { .message = error.operation.clone(),
                 .source  = Some(ResourceErrorSource::Backend(rstd::move(error))) };
    }
    static auto texture(texture::Error error, String operation) -> ResourceError {
        return { .message = rstd::move(operation),
                 .source  = Some(ResourceErrorSource::Texture(error)) };
    }
    auto clone() const -> ResourceError {
        Option<ResourceErrorSource> copied;
        if (source) {
            if (source->is_Texture())
                copied = Some(ResourceErrorSource::Texture(source->as_Texture().source));
            else if (source->is_Native())
                copied = Some(ResourceErrorSource::Native(source->as_Native().code));
            else if (source->is_External())
                copied = Some(ResourceErrorSource::External(source->as_External().source.clone()));
            else if (source->is_Backend())
                copied = Some(ResourceErrorSource::Backend(source->as_Backend().source.clone()));
            else
                copied = Some(ResourceErrorSource::Io(source->as_Io().source));
        }
        Vec<TextureErrorContext> contexts;
        for (const auto& texture : textures) contexts.push(texture.clone());
        Vec<ResourceErrorContext> details;
        for (const auto& context : this->contexts) details.push(context.clone());
        return {
            kind, message.clone(), rstd::move(copied), rstd::move(contexts), rstd::move(details)
        };
    }
};

class ResourceErrorSnapshot {
    Option<rstd::sync::Arc<ResourceError>> error_;

public:
    ResourceErrorSnapshot() = default;
    explicit ResourceErrorSnapshot(ResourceError error)
        : error_(Some(rstd::sync::Arc<ResourceError>::make(rstd::move(error)))) {}
    ResourceErrorSnapshot(const ResourceErrorSnapshot& other): error_(other.error_.clone()) {}
    auto operator=(const ResourceErrorSnapshot& other) -> ResourceErrorSnapshot& {
        error_ = other.error_.clone();
        return *this;
    }
    ResourceErrorSnapshot(ResourceErrorSnapshot&&)                        = default;
    auto     operator=(ResourceErrorSnapshot&&) -> ResourceErrorSnapshot& = default;
    explicit operator bool() const { return error_.is_some(); }
    auto     error() const -> const ResourceError& { return **error_; }
};

} // namespace vrento::resource

export namespace rstd
{

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
