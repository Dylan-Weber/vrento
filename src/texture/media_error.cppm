module;
#include <rstd/enum.hpp>
export module vrento:texture.media_error;
import rstd;
import :texture.core;
import :vulkan.error;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::texture
{
enum class MediaOperation
{
    Prepare,
    Open,
    Seek,
    Decode,
    Import,
    Convert
};
class MediaCause {
    RSTD_ENUM(MediaCause, (Texture, (Error error;)), (Decoder, (String message;)),
              (Invalid, (String detail;)), (Backend, (vulkan::BackendError error;)))
public:
    auto clone() const -> MediaCause {
        if (is_Texture()) return Texture(as_Texture().error);
        if (is_Decoder()) return Decoder(as_Decoder().message.clone());
        if (is_Invalid()) return Invalid(as_Invalid().detail.clone());
        return Backend(as_Backend().error.clone());
    }
};
struct MediaError {
    MediaOperation operation;
    MediaCause     cause;
    auto           clone() const -> MediaError { return { operation, cause.clone() }; }
    static auto    texture(Error error, MediaOperation operation = MediaOperation::Prepare)
        -> MediaError {
        return { operation, MediaCause::Texture(error) };
    }
    static auto decoder(MediaOperation operation, String message) -> MediaError {
        return { operation, MediaCause::Decoder(rstd::move(message)) };
    }
};
} // namespace vrento::texture
export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::texture::MediaCause> : ImplBase<vrento::texture::MediaCause> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& c = this->self();
        if (c.is_Texture()) return as<fmt::Display>(c.as_Texture().error).fmt(f);
        if (c.is_Backend()) return as<fmt::Display>(c.as_Backend().error).fmt(f);
        if (c.is_Decoder())
            return f.write_fmt(fmt::Arguments::make("decoder: {}", c.as_Decoder().message));
        return f.write_fmt(fmt::Arguments::make("invalid media: {}", c.as_Invalid().detail));
    }
};
template<>
struct Impl<fmt::Debug, vrento::texture::MediaCause> : ImplBase<vrento::texture::MediaCause> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::texture::MediaCause> : ImplBase<vrento::texture::MediaCause> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& c = this->self();
        if (c.is_Texture()) return Some(dyn<error::Error>::from_ref(c.as_Texture().error));
        if (c.is_Backend()) return Some(dyn<error::Error>::from_ref(c.as_Backend().error));
        return None();
    }
};
template<>
struct Impl<fmt::Display, vrento::texture::MediaError> : ImplBase<vrento::texture::MediaError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(
            fmt::Arguments::make("media operation={} failed", int(this->self().operation)));
    }
};
template<>
struct Impl<fmt::Debug, vrento::texture::MediaError> : ImplBase<vrento::texture::MediaError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::texture::MediaError> : ImplBase<vrento::texture::MediaError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        return Some(dyn<error::Error>::from_ref(this->self().cause));
    }
};
} // namespace rstd
