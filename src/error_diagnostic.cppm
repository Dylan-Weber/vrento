export module vrento:error_diagnostic;
import rstd;
using namespace rstd::prelude;

export namespace vrento
{
struct ErrorDiagnostic {
    String      message;
    String      debug;
    Vec<String> sources;
    ErrorDiagnostic() = default;
    ErrorDiagnostic(const ErrorDiagnostic& other)
        : message(other.message.clone()),
          debug(other.debug.clone()),
          sources(other.sources.clone()) {}
    auto operator=(const ErrorDiagnostic& other) -> ErrorDiagnostic& {
        if (this != &other) *this = ErrorDiagnostic(other);
        return *this;
    }
    ErrorDiagnostic(ErrorDiagnostic&&)                    = default;
    auto operator=(ErrorDiagnostic&&) -> ErrorDiagnostic& = default;
    template<class E>
    static auto capture(const E& error) -> ErrorDiagnostic {
        ErrorDiagnostic result;
        result.message = rstd::format("{}", error);
        result.debug   = rstd::format("{:?}", error);
        auto source    = rstd::as<rstd::error::Error>(error).source();
        while (source.is_some()) {
            result.sources.push(rstd::format("{}", *source));
            source = (*source)->source();
        }
        return result;
    }
};
} // namespace vrento
