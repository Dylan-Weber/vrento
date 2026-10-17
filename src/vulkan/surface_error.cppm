export module vrento:vulkan.surface_error;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

export namespace vrento
{
enum class FrameSurfaceErrorKind
{
    SerialExhausted,
    InvalidLease,
    NotPending,
    StaleIdentity,
    SessionLost,
    ProtocolFailure,
};

struct FrameSurfaceError {
    FrameSurfaceErrorKind                kind;
    Option<Arc<dyn<rstd::error::Error>>> cause;

    auto clone() const -> FrameSurfaceError { return { kind, cause.clone() }; }
};
} // namespace vrento

using vrento::FrameSurfaceErrorKind;

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::FrameSurfaceError> : ImplBase<vrento::FrameSurfaceError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        ref<str> message = "surface protocol failure"_str;
        switch (this->self().kind) {
        case FrameSurfaceErrorKind::SerialExhausted:
            message = "surface acquire serial exhausted"_str;
            break;
        case FrameSurfaceErrorKind::InvalidLease: message = "invalid surface lease"_str; break;
        case FrameSurfaceErrorKind::NotPending:
            message = "no pending surface acquisition"_str;
            break;
        case FrameSurfaceErrorKind::StaleIdentity: message = "stale surface identity"_str; break;
        case FrameSurfaceErrorKind::SessionLost: message = "surface session lost"_str; break;
        case FrameSurfaceErrorKind::ProtocolFailure: break;
        }
        if (! f.write_str(message)) return false;
        if (this->self().cause)
            return f.write_fmt(fmt::Arguments::make(": {}", this->self().cause->deref()));
        return true;
    }
};
template<>
struct Impl<fmt::Debug, vrento::FrameSurfaceError> : ImplBase<vrento::FrameSurfaceError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::FrameSurfaceError> : ImplBase<vrento::FrameSurfaceError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        if (this->self().cause) return Some(this->self().cause->deref());
        return None();
    }
};
} // namespace rstd
static_assert(rstd::Impled<vrento::FrameSurfaceError, rstd::error::Error>);
