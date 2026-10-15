export module vrento:vulkan.pass.pass;
export import :vulkan.pass.error;
import rstd;
export namespace vrento::vulkan
{
struct GraphPrepareContext;
struct GraphRecordContext;
struct VulkanPass {
    using Trait                  = VulkanPass;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = VulkanPass;
        auto Prepare(const GraphPrepareContext& context) const
            -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>,
                      GraphExecutionError> {
            return rstd::trait_call<0>(this, context);
        }
        auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
            return rstd::trait_call<1>(this, context);
        }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Prepare, &T::Record>;
};
} // namespace vrento::vulkan
export namespace rstd
{
template<class T>
struct VulkanPassImplementation : ImplBase<T> {
    auto Prepare(const vrento::vulkan::GraphPrepareContext& context) const
        -> Result<vrento::vulkan::Preparation<sync::Arc<dyn<vrento::vulkan::VulkanPass>>>,
                  vrento::vulkan::GraphExecutionError> {
        if constexpr (requires { this->self().Prepare(context); })
            return this->self().Prepare(context);
        else if constexpr (requires { this->self().Instantiate(context); })
            return this->self().Instantiate(context);
        else
            return Err(vrento::vulkan::GraphExecutionError {
                vrento::vulkan::GraphExecutionStage::Prepare });
    }
    auto Record(vrento::vulkan::GraphRecordContext& context) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        if constexpr (requires { this->self().Record(context); })
            return this->self().Record(context);
        else
            return Err(vrento::vulkan::GraphExecutionError {
                vrento::vulkan::GraphExecutionStage::Record });
    }
};
template<class T>
    requires(
        requires { &T::Record; } || requires { &T::Instantiate; } || requires { &T::Prepare; } ||
        requires { typename T::FrameData; })
struct Impl<vrento::vulkan::VulkanPass, T> : VulkanPassImplementation<T> {};
} // namespace rstd
