module;
#include <vulkan/vk_platform.h>
export module vrento.device_session;
export import vrento.vulkan;
import rstd;

export namespace vrento::vulkan {
using namespace rstd::prelude;
using namespace rstd::literals;

enum class DeviceSessionStage { Invalid, Instance, Surface, Device, Lost };
struct DeviceSessionError {
    DeviceSessionStage stage;
    VkResult result{VK_ERROR_INITIALIZATION_FAILED};
};
class SurfaceLifetime {
  public:
    virtual ~SurfaceLifetime() = default;
};
struct SessionSurface {
    VkSurfaceKHR handle;
    Box<SurfaceLifetime> lifetime;
};
class SessionSurfaceFactory {
  public:
    virtual ~SessionSurfaceFactory() = default;
    virtual auto create(VkInstance) -> Result<SessionSurface, DeviceSessionError> = 0;
};
struct DeviceSessionOptions {
    slice<Extension> instance_extensions;
    slice<Extension> device_extensions;
    VkExtent2D extent{1, 1};
    SwapchainOptions swapchain{};
    bool validation{};
};

class DeviceSession {
    struct State {
        rstd::uint64_t epoch{};
        unsigned errors{};
        Option<Box<SurfaceLifetime>> native_surface;
        Instance instance;
        vvk::DebugUtilsMessenger debug;
        Device device;
        ~State() {
            if (device.handle())
                (void)device.handle().WaitIdle();
        }
        static VKAPI_ATTR VkBool32 VKAPI_CALL
        validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                   const VkDebugUtilsMessengerCallbackDataEXT *, void *user) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                __atomic_fetch_add(&static_cast<State *>(user)->errors, 1u, __ATOMIC_RELAXED);
            return VK_FALSE;
        }
    };
    rstd::sync::Arc<State> state_;
    explicit DeviceSession(rstd::sync::Arc<State> state) : state_(rstd::move(state)) {}

  public:
    DeviceSession(const DeviceSession &other) : state_(other.state_.clone()) {}
    auto operator=(const DeviceSession &other) -> DeviceSession & {
        if (this != &other)
            state_ = other.state_.clone();
        return *this;
    }
    DeviceSession(DeviceSession &&) = default;
    auto operator=(DeviceSession &&) -> DeviceSession & = default;

    static auto create(DeviceSessionOptions options = {}, SessionSurfaceFactory *surface = nullptr)
        -> Result<DeviceSession, DeviceSessionError> {
        if (!options.extent.width || !options.extent.height)
            return Err(DeviceSessionError{DeviceSessionStage::Invalid});
        auto state = rstd::sync::Arc<State>::make();
        InstanceLayer layer{true, VALIDATION_LAYER_NAME};
        auto layers = options.validation ? slice<InstanceLayer>::from_raw_parts(&layer, usize(1))
                                         : slice<InstanceLayer>{};
        if (!Instance::Create(state->instance, options.instance_extensions, layers))
            return Err(DeviceSessionError{DeviceSessionStage::Instance});
        if (options.validation) {
            VkDebugUtilsMessengerCreateInfoEXT info{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            info.pfnUserCallback = State::validation;
            info.pUserData = &*state;
            state->debug = state->instance.inst().CreateDebugUtilsMessenger(info);
            if (!state->debug)
                return Err(DeviceSessionError{DeviceSessionStage::Instance});
        }
        if (surface) {
            auto created = surface->create(*state->instance.inst());
            if (created.is_err())
                return Err(created.unwrap_err());
            auto native = created.unwrap();
            if (!native.handle)
                return Err(DeviceSessionError{DeviceSessionStage::Surface});
            state->native_surface = Some(rstd::move(native.lifetime));
            state->instance.setSurface(native.handle);
        }
        Vec<Extension> extensions;
        extensions.push({true, "VK_KHR_timeline_semaphore"_str});
        for (const auto &extension : options.device_extensions)
            extensions.push(Extension(extension));
        auto check = [&](const vvk::PhysicalDevice &gpu) {
            return Device::CheckGPU(gpu, extensions.as_slice(), *state->instance.surface());
        };
        auto predicate = CheckGpuOp::from_ref(check);
        if (!state->instance.ChoosePhysicalDevice(predicate.as_mut_ref()) ||
            !Device::Create(state->instance, extensions.as_slice(), options.extent, state->device,
                            options.swapchain))
            return Err(DeviceSessionError{DeviceSessionStage::Device});
        static rstd::uint64_t next_epoch = 1;
        state->epoch = __atomic_fetch_add(&next_epoch, 1, __ATOMIC_RELAXED);
        if (!state->epoch)
            rstd::panic("device session identity exhausted");
        return Ok(DeviceSession(rstd::move(state)));
    }
    auto valid() const -> bool { return bool(state_); }
    auto lost() const -> bool { return valid() && state_->device.lost(); }
    // Pass only results observed from operations on this logical device.
    auto observe_result(VkResult result) const -> VkResult {
        return valid() ? state_->device.observe_result(result) : result;
    }
    auto wait_idle() const -> VkResult {
        if (!valid())
            return VK_ERROR_INITIALIZATION_FAILED;
        return observe_result(device().handle().WaitIdle());
    }
    auto device() const -> const Device & { return state_->device; }
    auto instance() const -> VkInstance { return *state_->instance.inst(); }
    auto epoch() const -> rstd::uint64_t { return state_->epoch; }
    auto has_surface() const -> bool { return valid() && bool(state_->instance.surface()); }
    // The surface owner must first retire every swapchain and presentation user.
    void release_surface() {
        if (!valid())
            return;
        state_->device.releaseSwapchain();
        state_->instance.setSurface(VK_NULL_HANDLE);
    }
    auto validation_errors() const -> unsigned {
        return __atomic_load_n(&state_->errors, __ATOMIC_RELAXED);
    }
    // The surface owner must retire all swapchain users before calling this method.
    auto resize_surface(VkExtent2D extent, SwapchainOptions options)
        -> Result<empty, DeviceSessionError> {
        if (lost())
            return Err(DeviceSessionError{DeviceSessionStage::Lost, VK_ERROR_DEVICE_LOST});
        if (!valid() || !has_surface() || !extent.width || !extent.height)
            return Err(DeviceSessionError{DeviceSessionStage::Invalid});
        auto result = observe_result(
            state_->device.recreateSwapchainResult(*state_->instance.surface(), extent, options));
        if (result != VK_SUCCESS)
            return Err(DeviceSessionError{result == VK_ERROR_DEVICE_LOST
                                              ? DeviceSessionStage::Lost
                                              : DeviceSessionStage::Surface,
                                          result});
        return Ok(empty{});
    }
};
} // namespace vrento::vulkan
