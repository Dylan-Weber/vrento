module;
#include <rstd/enum.hpp>

export module vrento:resource.error;
import rstd;
import :texture.core;
import :resource.handle;
using namespace rstd::prelude;

export namespace vrento::resource
{

enum class ResourceErrorKind
{
    MissingDefinition,
    MissingContent,
    BackendFailure,
    Pending,
    Invalid,
    Closed,
    Device,
    Worker,
};

class ResourceErrorSource {
    RSTD_ENUM(ResourceErrorSource, (Texture, (texture::Error source;)),
              (Native, (rstd::int32_t code;)), (Io, (rstd::io::error::Error source;)))
};

struct TextureErrorContext {
    ResourceHandle<TextureResourceTag> resource;
    u64                                revision;
    TextureUseHandle                   use;
    String                             name;
    auto clone() const -> TextureErrorContext { return { resource, revision, use, name.clone() }; }
};
struct ResourceError {
    ResourceErrorKind           kind { ResourceErrorKind::BackendFailure };
    String                      message;
    Option<ResourceErrorSource> source;
    Vec<TextureErrorContext>    textures;
    auto                        clone() const -> ResourceError {
        Option<ResourceErrorSource> copied;
        if (source) {
            if (source->is_Texture())
                copied = Some(ResourceErrorSource::Texture(source->as_Texture().source));
            else if (source->is_Native())
                copied = Some(ResourceErrorSource::Native(source->as_Native().code));
            else
                copied = Some(ResourceErrorSource::Io(source->as_Io().source));
        }
        Vec<TextureErrorContext> contexts;
        for (const auto& texture : textures) contexts.push(texture.clone());
        return { kind, message.clone(), rstd::move(copied), rstd::move(contexts) };
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
    : DefaultInImpl<error::Error, vrento::resource::ResourceErrorSource> {};

template<>
struct Impl<fmt::Display, vrento::resource::ResourceError>
    : ImplBase<vrento::resource::ResourceError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        if (! formatter.write_fmt(fmt::Arguments::make("{}", this->self().message))) return false;
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
        return formatter.write_fmt(fmt::Arguments::make("ResourceError(kind={}, message={})",
                                                        static_cast<int>(this->self().kind),
                                                        this->self().message));
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
