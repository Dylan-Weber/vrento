module;
#include <rstd/enum.hpp>

export module vrento:vulkan.pass.error;
export import :vulkan.pass.execution;
export import :preparation;
import :texture.core;
import :resource.error;
import :vulkan.api;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
enum class CompileProgress
{
    Pending,
    Ready
};
enum class FrameProgress
{
    Pending,
    Ready
};

enum class GraphExecutionStage
{
    Graph,
    Binding,
    Device,
    Schema,
    Prepare,
    Access,
    Record,
    Submit,
    Unsupported
};
enum class GraphFailure
{
    InvalidState,
    MissingBinding,
    SchemaMismatch,
    Unsupported,
    Closed,
    StaleTransaction
};
class GraphExecutionCause {
    RSTD_ENUM(GraphExecutionCause, (Reason, (GraphFailure value;)), (Native, (VkResult code;)),
              (Texture, (texture::Error error;)), (Graph, (GraphError error;)),
              (Resource, (resource::ResourceErrorSnapshot error;)),
              (Backend, (BackendErrorSnapshot error;)))
public:
    auto clone() const -> GraphExecutionCause {
        if (is_Reason()) return Reason(as_Reason().value);
        if (is_Native()) return Native(as_Native().code);
        if (is_Texture()) return Texture(as_Texture().error);
        if (is_Graph()) return Graph(as_Graph().error);
        if (is_Resource()) return Resource(as_Resource().error);
        return Backend(as_Backend().error);
    }
};
struct GraphExecutionError {
    GraphExecutionStage stage;
    PassId              pass {};
    ResourceId          resource {};
    GraphExecutionCause cause;
    GraphExecutionError(GraphExecutionStage operation, PassId id = {}, ResourceId input = {})
        : stage(operation),
          pass(id),
          resource(input),
          cause(GraphExecutionCause::Reason(
              operation == GraphExecutionStage::Binding       ? GraphFailure::MissingBinding
              : operation == GraphExecutionStage::Schema      ? GraphFailure::SchemaMismatch
              : operation == GraphExecutionStage::Unsupported ? GraphFailure::Unsupported
              : operation == GraphExecutionStage::Device      ? GraphFailure::Closed
                                                              : GraphFailure::InvalidState)) {}
    GraphExecutionError(GraphExecutionStage operation, PassId id, ResourceId input,
                        GraphExecutionCause reason)
        : stage(operation), pass(id), resource(input), cause(rstd::move(reason)) {}
    GraphExecutionError(const GraphExecutionError& other)
        : stage(other.stage),
          pass(other.pass),
          resource(other.resource),
          cause(other.cause.clone()) {}
    auto operator=(const GraphExecutionError& other) -> GraphExecutionError& {
        stage    = other.stage;
        pass     = other.pass;
        resource = other.resource;
        cause    = other.cause.clone();
        return *this;
    }
    GraphExecutionError(GraphExecutionError&&)                    = default;
    auto operator=(GraphExecutionError&&) -> GraphExecutionError& = default;
    auto native_result() const -> VkResult {
        if (cause.is_Native()) return cause.as_Native().code;
        if (cause.is_Texture()) return VkResult(cause.as_Texture().error.native);
        if (cause.is_Backend() && cause.as_Backend().error)
            return cause.as_Backend().error.error().cause.native();
        return VK_SUCCESS;
    }
    auto texture_error() const -> Option<texture::Error> {
        return cause.is_Texture() ? Some(texture::Error(cause.as_Texture().error))
                                  : None<texture::Error>();
    }
};

} // namespace vrento::vulkan

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::vulkan::GraphExecutionCause>
    : ImplBase<vrento::vulkan::GraphExecutionCause> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& c = this->self();
        if (c.is_Reason())
            return f.write_fmt(fmt::Arguments::make("reason={}", int(c.as_Reason().value)));
        if (c.is_Native())
            return f.write_fmt(fmt::Arguments::make("native={}", int(c.as_Native().code)));
        if (c.is_Texture()) return as<fmt::Debug>(c.as_Texture().error).fmt(f);
        if (c.is_Graph()) return as<fmt::Debug>(c.as_Graph().error).fmt(f);
        if (c.is_Resource()) return as<fmt::Debug>(c.as_Resource().error.error()).fmt(f);
        return as<fmt::Debug>(c.as_Backend().error.error()).fmt(f);
    }
};

template<>
struct Impl<fmt::Display, vrento::vulkan::GraphExecutionError>
    : ImplBase<vrento::vulkan::GraphExecutionError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& value = this->self();
        return formatter.write_str(
            (rstd::format("graph execution error stage={} pass={} resource={} cause={}",
                          int(value.stage),
                          value.pass.value,
                          value.resource.value,
                          value.cause))
                .as_str());
    }
};
template<>
struct Impl<fmt::Debug, vrento::vulkan::GraphExecutionError>
    : ImplBase<vrento::vulkan::GraphExecutionError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, vrento::vulkan::GraphExecutionError>
    : ImplBase<vrento::vulkan::GraphExecutionError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& value = this->self();
        if (value.cause.is_Resource())
            return Some(dyn<error::Error>::from_ref(value.cause.as_Resource().error.error()));
        if (value.cause.is_Backend())
            return Some(dyn<error::Error>::from_ref(value.cause.as_Backend().error.error()));
        if (value.cause.is_Texture())
            return Some(dyn<error::Error>::from_ref(value.cause.as_Texture().error));
        if (value.cause.is_Graph())
            return Some(dyn<error::Error>::from_ref(value.cause.as_Graph().error));
        return None();
    }
};
} // namespace rstd
