export module vrento:vulkan.pass.factory;
export import :vulkan.pass.input;
export import :vulkan.pass.resources;
import rstd;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
struct PassCompileContext {
    const DeviceSession&                  session;
    Option<Arc<GraphicsCompileResources>> graphics;
};

template<typename T, typename Backend>
struct TypedPassFactory {
    Backend backend;
    auto    Type() const -> rstd::any::TypeId { return rstd::any::TypeId::of<T>(); }
    auto    PassType() const -> rstd::any::TypeId {
        return rstd::any::TypeId::of<typename Backend::Pass>();
    }
    auto Compile(const PassInput& desc, const PassCompileContext& context)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
        auto value = desc.get<T>();
        if (! value)
            return Err(GraphExecutionError { GraphExecutionStage::Schema, desc.execution().id });
        return backend.Compile(**value, desc.execution(), context);
    }
};

struct PassFactory {
    using Trait                  = PassFactory;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = PassFactory;
        auto Type() const -> rstd::any::TypeId { return rstd::trait_call<0>(this); }
        auto PassType() const -> rstd::any::TypeId { return rstd::trait_call<2>(this); }
        auto Compile(const PassInput& desc, const PassCompileContext& context)
            -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
            return rstd::trait_call<1>(this, desc, context);
        }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Type, &T::Compile, &T::PassType>;
};

} // namespace vrento::vulkan
export namespace rstd
{
template<typename T, typename Backend>
struct Impl<vrento::vulkan::PassFactory, vrento::vulkan::TypedPassFactory<T, Backend>>
    : ImplBase<vrento::vulkan::TypedPassFactory<T, Backend>> {
    auto Type() const -> any::TypeId { return this->self().Type(); }
    auto PassType() const -> any::TypeId { return this->self().PassType(); }
    auto Compile(const vrento::vulkan::PassInput&          desc,
                 const vrento::vulkan::PassCompileContext& context)
        -> Result<sync::Arc<dyn<vrento::vulkan::VulkanPass>>, vrento::vulkan::GraphExecutionError> {
        return this->self().Compile(desc, context);
    }
};
} // namespace rstd
