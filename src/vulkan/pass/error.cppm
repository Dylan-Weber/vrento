export module vrento:vulkan.pass.error;
export import :vulkan.pass.execution;
import :texture.core;
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
    GraphExecutionStage    stage;
    PassId                 pass {};
    ResourceId             resource {};
    VkResult               native { VK_SUCCESS };
    Option<texture::Error> texture {};
    Option<GraphError>     graph {};
};

} // namespace vrento::vulkan
