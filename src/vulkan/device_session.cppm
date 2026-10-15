module;
#include <vulkan/vk_platform.h>
export module vrento:vulkan.device_session;
export import :vulkan.api;
export import :resource_registry.graphics;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::vulkan
{

enum class DeviceSessionStage
{
    Invalid,
    Instance,
    Surface,
    Device,
    Lost
};
struct DeviceSessionError {
    DeviceSessionStage   stage;
    VkResult             result { VK_SUCCESS };
    BackendErrorSnapshot cause;
    static auto          from(DeviceSessionStage stage, BackendError error) -> DeviceSessionError {
        auto native = error.cause.native();
        return { stage, native, BackendErrorSnapshot(rstd::move(error)) };
    }
};
using SurfaceLifetime = rstd::any::Any;
struct GraphicsResources {
    resource_registry::DescriptorLayoutRegistry descriptors;
    resource_registry::DescriptorSystem         descriptor_snapshots;
    resource_registry::PipelineLayoutRegistry   layouts;
    resource_registry::RenderPassRegistry       passes;
    resource_registry::PipelineRegistry         pipelines;
};
struct SessionSurface {
    VkSurfaceKHR handle { VK_NULL_HANDLE };
    // Instance owns the VkSurfaceKHR; this owner retains its native window.
    Box<dyn<SurfaceLifetime>> lifetime;
};
struct SessionSurfaceFactory {
    using Trait                  = SessionSurfaceFactory;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SessionSurfaceFactory;

        auto create(VkInstance instance) -> Result<SessionSurface, DeviceSessionError> {
            return rstd::trait_call<0>(this, instance);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::create>;
};
struct DeviceSessionOptions {
    slice<Extension>     instance_extensions;
    slice<Extension>     device_extensions;
    slice<rstd::uint8_t> physical_device_uuid;
    rstd::uint32_t       api_version { WP_VULKAN_VERSION };
    VkExtent2D           extent { 1, 1 };
    SwapchainOptions     swapchain {};
    bool                 validation {};
    slice<u8>            pipeline_cache_data {};
};

class DeviceSession {
    struct State {
        rstd::uint64_t                    epoch {};
        unsigned                          errors {};
        Option<Box<dyn<SurfaceLifetime>>> native_surface;
        Instance                          instance;
        vvk::DebugUtilsMessenger          debug;
        Device                            device;
        GraphicsResources                 graphics;
        ~State() {
            if (device.handle()) (void)device.handle().WaitIdle();
        }
        static VKAPI_ATTR VkBool32 VKAPI_CALL
        validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                   const VkDebugUtilsMessengerCallbackDataEXT*, void* user) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                __atomic_fetch_add(&static_cast<State*>(user)->errors, 1u, __ATOMIC_RELAXED);
            return VK_FALSE;
        }
    };
    rstd::sync::Arc<State> state_;
    explicit DeviceSession(rstd::sync::Arc<State> state): state_(rstd::move(state)) {}

public:
    DeviceSession(const DeviceSession& other): state_(other.state_.clone()) {}
    auto operator=(const DeviceSession& other) -> DeviceSession& {
        if (this != &other) state_ = other.state_.clone();
        return *this;
    }
    DeviceSession(DeviceSession&&)                    = default;
    auto operator=(DeviceSession&&) -> DeviceSession& = default;

    static auto create(DeviceSessionOptions                        options = {},
                       Option<mut_ref<dyn<SessionSurfaceFactory>>> surface = None())
        -> Result<DeviceSession, DeviceSessionError> {
        if (! options.extent.width || ! options.extent.height ||
            (! options.physical_device_uuid.is_empty() &&
             options.physical_device_uuid.len() != usize(VK_UUID_SIZE)))
            return Err(DeviceSessionError { DeviceSessionStage::Invalid });
        auto          state = rstd::sync::Arc<State>::make();
        InstanceLayer layer { true, VALIDATION_LAYER_NAME };
        auto layers   = options.validation ? slice<InstanceLayer>::from_raw_parts(&layer, usize(1))
                                           : slice<InstanceLayer> {};
        auto instance = Instance::TryCreate(
            state->instance, options.instance_extensions, layers, options.api_version);
        if (instance.is_err())
            return Err(
                DeviceSessionError::from(DeviceSessionStage::Instance, instance.unwrap_err()));
        if (options.validation) {
            VkDebugUtilsMessengerCreateInfoEXT info {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
            };
            info.messageSeverity              = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType                  = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            info.pfnUserCallback              = State::validation;
            info.pUserData                    = &*state;
            const auto&              instance = state->instance.inst();
            VkDebugUtilsMessengerEXT handle {};
            auto                     result = instance.Dispatch().vkCreateDebugUtilsMessengerEXT(
                *instance, &info, nullptr, &handle);
            if (result != VK_SUCCESS)
                return Err(DeviceSessionError::from(
                    DeviceSessionStage::Instance,
                    { "create validation messenger"_Str, BackendCause::Native(result) }));
            state->debug = vvk::DebugUtilsMessenger(handle, *instance, instance.Dispatch());
        }
        if (surface) {
            auto created = (*surface)->create(*state->instance.inst());
            if (created.is_err()) return Err(created.unwrap_err());
            auto native = created.unwrap();
            if (! native.handle) return Err(DeviceSessionError { DeviceSessionStage::Surface });
            state->native_surface = Some(rstd::move(native.lifetime));
            state->instance.setSurface(native.handle);
        }
        Vec<Extension> extensions;
        extensions.push({ true, "VK_KHR_timeline_semaphore"_str });
        for (const auto& extension : options.device_extensions)
            extensions.push(Extension(extension));
        Option<BackendError> rejected;
        auto                 check = [&](const vvk::PhysicalDevice& gpu) {
            auto result =
                Device::ValidateGPU(gpu, extensions.as_slice(), *state->instance.surface());
            if (result.is_err()) {
                rejected = Some(result.unwrap_err());
                return false;
            }
            return true;
        };
        auto predicate = CheckGpuOp::from_ref(check);
        auto selected  = state->instance.TryChoosePhysicalDevice(predicate.as_mut_ref(),
                                                                 options.physical_device_uuid);
        if (selected.is_err()) {
            if (rejected)
                return Err(
                    DeviceSessionError::from(DeviceSessionStage::Device, rejected.take().unwrap()));
            return Err(DeviceSessionError::from(DeviceSessionStage::Device, selected.unwrap_err()));
        }
        auto created = Device::TryCreate(state->instance,
                                         extensions.as_slice(),
                                         options.extent,
                                         state->device,
                                         options.swapchain);
        if (created.is_err())
            return Err(DeviceSessionError::from(DeviceSessionStage::Device, created.unwrap_err()));
        static rstd::uint64_t next_epoch = 1;
        state->epoch                     = __atomic_fetch_add(&next_epoch, 1, __ATOMIC_RELAXED);
        if (! state->epoch) rstd::panic("device session identity exhausted");
        auto cache = state->graphics.pipelines.driver_cache().initialize(
            state->device, options.pipeline_cache_data);
        if (cache.is_err())
            return Err(DeviceSessionError { DeviceSessionStage::Device,
                                            cache.as_ref().unwrap_err().native,
                                            cache.unwrap_err().cause });
        return Ok(DeviceSession(rstd::move(state)));
    }
    auto valid() const -> bool { return bool(state_); }
    auto lost() const -> bool { return valid() && state_->device.lost(); }
    // Pass only results observed from operations on this logical device.
    auto observe_result(VkResult result) const -> VkResult {
        return valid() ? state_->device.observe_result(result) : result;
    }
    auto wait_idle() const -> VkResult {
        if (! valid()) return VK_ERROR_INITIALIZATION_FAILED;
        return observe_result(device().handle().WaitIdle());
    }
    auto device() const -> const Device& { return state_->device; }
    auto graphics_resources() const -> GraphicsResources& { return state_->graphics; }
    auto instance_owner() const -> const Instance& { return state_->instance; }
    auto instance() const -> VkInstance { return *state_->instance.inst(); }
    auto epoch() const -> rstd::uint64_t { return state_->epoch; }
    auto has_surface() const -> bool { return valid() && bool(state_->instance.surface()); }
    // The surface owner must first retire every swapchain and presentation user.
    void release_surface() {
        if (! valid()) return;
        state_->device.releaseSwapchain();
        state_->instance.setSurface(VK_NULL_HANDLE);
        // Shared sessions retain the native window after swapchain retirement.
    }
    auto validation_errors() const -> unsigned {
        return __atomic_load_n(&state_->errors, __ATOMIC_RELAXED);
    }
    // The surface owner must retire all swapchain users before calling this method.
    auto resize_surface(VkExtent2D extent, SwapchainOptions options)
        -> Result<empty, DeviceSessionError> {
        if (lost())
            return Err(DeviceSessionError { DeviceSessionStage::Lost, VK_ERROR_DEVICE_LOST });
        if (! valid() || ! has_surface() || ! extent.width || ! extent.height)
            return Err(DeviceSessionError { DeviceSessionStage::Invalid });
        auto result = observe_result(
            state_->device.recreateSwapchainResult(*state_->instance.surface(), extent, options));
        if (result != VK_SUCCESS)
            return Err(DeviceSessionError { result == VK_ERROR_DEVICE_LOST
                                                ? DeviceSessionStage::Lost
                                                : DeviceSessionStage::Surface,
                                            result });
        return Ok(empty {});
    }
};
} // namespace vrento::vulkan

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::vulkan::DeviceSessionError>
    : ImplBase<vrento::vulkan::DeviceSessionError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(fmt::Arguments::make("device session stage={} result={}",
                                                int(this->self().stage),
                                                int(this->self().result)));
    }
};
template<>
struct Impl<fmt::Debug, vrento::vulkan::DeviceSessionError>
    : ImplBase<vrento::vulkan::DeviceSessionError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, vrento::vulkan::DeviceSessionError>
    : ImplBase<vrento::vulkan::DeviceSessionError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& e = this->self();
        return e.cause ? Some(dyn<error::Error>::from_ref(e.cause.error()))
                       : None<error::ErrorRef>();
    }
};
} // namespace rstd
