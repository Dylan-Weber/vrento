module;
#include <rstd/enum.hpp>
export module vrento:vulkan.error;
import rstd;
import vvk;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::vulkan
{
struct DispatchFailure {
    vvk::DispatchErrorKind kind;
    vvk::DispatchStage     stage;
    String                 command;
    VkResult               result;
    bool                   cleanup_unavailable {};
    static auto            capture(const vvk::DispatchError& error) -> DispatchFailure {
        return { error.kind,
                 error.stage,
                 error.command
                     ? String::make(rstd::ffi::CStr::from_ptr(error.command).to_str().unwrap())
                     : String {},
                 error.api_result,
                 bool(error.unowned_instance || error.unowned_device) };
    }
    auto clone() const -> DispatchFailure {
        return { kind, stage, command.clone(), result, cleanup_unavailable };
    }
};
class BackendCause {
    RSTD_ENUM(BackendCause, (Invalid, (String detail;)), (Unsupported, (String capability;)),
              (State, (String detail;)), (Closed), (Busy),
              (Range, (rstd::uint64_t offset; rstd::uint64_t size; rstd::uint64_t capacity;)),
              (Capacity, (rstd::uint64_t size;)),
              (Io, (rstd::io::error::Error error; String path;)), (Native, (VkResult result;)),
              (Dispatch, (DispatchFailure error;)), (Memory, (vvk::MemoryError error;)),
              (Loader, (vvk::LoaderErrorKind kind; String message; DispatchFailure dispatch;)))
public:
    auto lost() const -> bool { return native() == VK_ERROR_DEVICE_LOST; }
    auto native() const -> VkResult {
        if (is_Native()) return as_Native().result;
        if (is_Dispatch()) return as_Dispatch().error.result;
        if (is_Memory()) return as_Memory().error.api_result;
        if (is_Loader()) return as_Loader().dispatch.result;
        return VK_SUCCESS;
    }
};
struct BackendError {
    String       operation;
    BackendCause cause;
};
} // namespace vrento::vulkan
export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::vulkan::BackendCause> : ImplBase<vrento::vulkan::BackendCause> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& v = this->self();
        if (v.is_Io())
            return f.write_fmt(
                fmt::Arguments::make("I/O path={}: {}", v.as_Io().path, v.as_Io().error));
        if (v.is_Closed()) return f.write_str("closed"_str);
        if (v.is_Busy()) return f.write_str("resource is reserved"_str);
        if (v.is_Range()) {
            const auto& r = v.as_Range();
            return f.write_fmt(fmt::Arguments::make(
                "range offset={} size={} capacity={}", r.offset, r.size, r.capacity));
        }
        if (v.is_Capacity())
            return f.write_fmt(
                fmt::Arguments::make("allocation capacity exceeded size={}", v.as_Capacity().size));
        if (v.is_Invalid())
            return f.write_fmt(fmt::Arguments::make("invalid: {}", v.as_Invalid().detail));
        if (v.is_Unsupported())
            return f.write_fmt(
                fmt::Arguments::make("unsupported: {}", v.as_Unsupported().capability));
        if (v.is_State())
            return f.write_fmt(fmt::Arguments::make("invalid state: {}", v.as_State().detail));
        if (v.is_Native())
            return f.write_fmt(fmt::Arguments::make("Vulkan result={}", int(v.as_Native().result)));
        if (v.is_Dispatch()) {
            const auto& e = v.as_Dispatch().error;
            return f.write_fmt(fmt::Arguments::make(
                "dispatch kind={} stage={} command={} result={} cleanup-unavailable={}",
                int(e.kind),
                int(e.stage),
                e.command,
                int(e.result),
                e.cleanup_unavailable));
        }
        if (v.is_Memory()) {
            const auto& e = v.as_Memory().error;
            return f.write_fmt(
                fmt::Arguments::make("memory kind={} result={} type={} size={} attempts={}",
                                     int(e.kind),
                                     int(e.api_result),
                                     e.memory_type,
                                     e.allocation_size,
                                     e.device_allocation_attempts));
        }
        const auto& e = v.as_Loader();
        return f.write_fmt(fmt::Arguments::make("loader kind={} message={} command={} result={}",
                                                int(e.kind),
                                                e.message,
                                                e.dispatch.command,
                                                int(e.dispatch.result)));
    }
};
template<>
struct Impl<fmt::Debug, vrento::vulkan::BackendCause> : ImplBase<vrento::vulkan::BackendCause> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::vulkan::BackendCause> : ImplBase<vrento::vulkan::BackendCause> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        if (this->self().is_Io())
            return Some(dyn<error::Error>::from_ref(this->self().as_Io().error));
        return None();
    }
};
template<>
struct Impl<fmt::Display, vrento::vulkan::BackendError> : ImplBase<vrento::vulkan::BackendError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(fmt::Arguments::make("{} failed", this->self().operation));
    }
};
template<>
struct Impl<fmt::Debug, vrento::vulkan::BackendError> : ImplBase<vrento::vulkan::BackendError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(
            fmt::Arguments::make("{}: {}", this->self().operation, this->self().cause));
    }
};
template<>
struct Impl<error::Error, vrento::vulkan::BackendError> : ImplBase<vrento::vulkan::BackendError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        return Some(dyn<error::Error>::from_ref(this->self().cause));
    }
};
} // namespace rstd
