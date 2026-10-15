export module vrento:vulkan.pass.error;
export import :vulkan.pass.execution;
import :texture.core;
import :resource.error;
import :vulkan.api;
import rstd;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
enum class GraphExecutionStage
{
    Graph,
    Binding,
    Device,
    Schema,
    Prepare,
    Pending,
    Access,
    Record,
    Unsupported
};
struct GraphExecutionError {
    GraphExecutionStage             stage;
    PassId                          pass {};
    ResourceId                      resource {};
    VkResult                        native { VK_SUCCESS };
    Option<texture::Error>          texture {};
    Option<GraphError>              graph {};
    resource::ResourceErrorSnapshot preparation;
};

} // namespace vrento::vulkan

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::vulkan::GraphExecutionError>
    : ImplBase<vrento::vulkan::GraphExecutionError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& value = this->self();
        return formatter.write_str(
            (rstd::format("graph execution error stage={} pass={} resource={} native={}",
                          int(value.stage),
                          value.pass.value,
                          value.resource.value,
                          int(value.native)))
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
        if (value.preparation) return Some(dyn<error::Error>::from_ref(value.preparation.error()));
        if (value.texture) return Some(dyn<error::Error>::from_ref(*value.texture));
        if (value.graph) return Some(dyn<error::Error>::from_ref(*value.graph));
        return None();
    }
};
} // namespace rstd
