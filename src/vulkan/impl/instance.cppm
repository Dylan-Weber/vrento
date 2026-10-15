module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

#if __is_target_os(macos)
#    include <vulkan/vulkan.h>
#    include <vulkan/vulkan_metal.h>
#endif

module vrento:vulkan.impl.instance;

import :vulkan.api;

import rstd;
import rstd.log;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::ffi::CStr;
using rstd::ffi::CString;
using namespace vrento::vulkan;

#include "ExtensionNames.hpp"

constexpr rstd::array<InstanceLayer, 0> base_inst_layers {};

// VK_EXT_debug_utils is required everywhere. MoltenVK additionally needs
// portability enumeration; keep that extension Apple-only so Linux keeps its
// original instance extension set and create flags.
#if __is_target_os(macos)
const rstd::array<Extension, 2> base_inst_exts {
    Extension { true, CStr::from_ptr(VK_EXT_DEBUG_UTILS_EXTENSION_NAME).to_str().unwrap() },
    Extension { false, "VK_KHR_portability_enumeration"_str }
};
#else
const rstd::array<Extension, 1> base_inst_exts { Extension {
    true, CStr::from_ptr(VK_EXT_DEBUG_UTILS_EXTENSION_NAME).to_str().unwrap() } };
#endif

namespace
{

VkBool32 DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                     VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                     void* /*pUserData*/) {
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        rstd_error("validation layer: {}", pCallbackData->pMessage);
    } else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rstd_warn("validation layer: {}", pCallbackData->pMessage);
    }
    return VK_FALSE;
}

auto SetupDebugCallback(vvk::Instance& instance) -> Result<vvk::DebugUtilsMessenger, BackendError> {
    VkDebugUtilsMessengerCreateInfoEXT info {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext           = nullptr,
        .flags           = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugUtilsMessengerCallback,
        .pUserData       = nullptr,
    };
    VkDebugUtilsMessengerEXT handle {};
    auto                     result =
        instance.Dispatch().vkCreateDebugUtilsMessengerEXT(*instance, &info, nullptr, &handle);
    if (result != VK_SUCCESS)
        return Err(BackendError { "create debug messenger"_Str, BackendCause::Native(result) });
    return Ok(vvk::DebugUtilsMessenger(handle, *instance, instance.Dispatch()));
}

auto CreateInstance(vvk::Instance* inst, slice<CString> exts, slice<CString> layers,
                    vvk::InstanceDispatch& dld, rstd::uint32_t api_version,
                    const vvk::GlobalDispatch& global) -> Result<empty, BackendError> {
    VkApplicationInfo app_info {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = WP_APPLICATION_NAME,
        .applicationVersion = api_version,
        .pEngineName        = "vulkan",
        .apiVersion         = api_version,
    };

    auto extension_names_c = Vec<const char*>::with_capacity(exts.len());
    for (const auto& ext : exts) extension_names_c.push(ext.as_ptr());
    auto layer_names_c = Vec<const char*>::with_capacity(layers.len());
    for (const auto& layer : layers) layer_names_c.push(layer.as_ptr());

    // VK_EXT_metal_objects requires the instance to opt in to the Metal
    // object type before vkExportMetalObjectsEXT can return the MTLDevice.
    // Keep this pNext optional so the same instance path remains valid on
    // non-Apple Vulkan implementations.
#if __is_target_os(macos)
    VkExportMetalObjectCreateInfoEXT metal_export_info {
        .sType            = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .pNext            = nullptr,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT,
    };
    const void* instance_next = &metal_export_info;
#else
    const void* instance_next = nullptr;
#endif

    VkInstanceCreateInfo info {
        .sType               = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext               = instance_next,
        .pApplicationInfo    = &app_info,
        .enabledLayerCount   = static_cast<rstd::uint32_t>(layer_names_c.len().to_primitive()),
        .ppEnabledLayerNames = layer_names_c.data(),
        .enabledExtensionCount =
            static_cast<rstd::uint32_t>(extension_names_c.len().to_primitive()),
        .ppEnabledExtensionNames = extension_names_c.data(),
    };
#if __is_target_os(macos)
    for (const auto& ext : exts)
        if (ext.as_ref().to_str().unwrap() == "VK_KHR_portability_enumeration"_str)
            info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    auto result = vvk::Instance::Create(*inst, global, info, dld);
    if (result.is_err())
        return Err(
            BackendError { "create instance"_Str,
                           BackendCause::Dispatch(DispatchFailure::capture(result.unwrap_err())) });
    return Ok(empty {});
}
template<typename Property, typename Enumerate>
auto EnumerateProperties(BTreeSet<String>& set, Enumerate enumerate, ref<str> operation)
    -> Result<empty, BackendError> {
    Vec<Property>  properties;
    rstd::uint32_t count {};
    VkResult       result;
    do {
        result = enumerate(&count, nullptr);
        if (result != VK_SUCCESS)
            return Err(BackendError { String::make(operation), BackendCause::Native(result) });
        properties.resize(usize(count), Property {});
        result = enumerate(&count, properties.data());
    } while (result == VK_INCOMPLETE);
    if (result != VK_SUCCESS)
        return Err(BackendError { String::make(operation), BackendCause::Native(result) });
    for (rstd::uint32_t i {}; i < count; ++i) {
        if constexpr (requires(Property p) { p.extensionName; })
            set.insert(
                String::make(CStr::from_ptr(properties[usize(i)].extensionName).to_str().unwrap()));
        else
            set.insert(
                String::make(CStr::from_ptr(properties[usize(i)].layerName).to_str().unwrap()));
    }
    return Ok(empty {});
}
} // namespace

auto Instance::TryChoosePhysicalDevice(mut_ref<CheckGpuOp> checkgpu, slice<rstd::uint8_t> uuid)
    -> Result<empty, BackendError> {
    Vec<VkPhysicalDevice> handles;
    rstd::uint32_t        count {};
    VkResult              result;
    do {
        result = m_vinst.Dispatch().vkEnumeratePhysicalDevices(*m_vinst, &count, nullptr);
        if (result != VK_SUCCESS)
            return Err(
                BackendError { "enumerate physical devices"_Str, BackendCause::Native(result) });
        handles.resize(usize(count), VK_NULL_HANDLE);
        result = m_vinst.Dispatch().vkEnumeratePhysicalDevices(*m_vinst, &count, handles.data());
    } while (result == VK_INCOMPLETE);
    if (result != VK_SUCCESS)
        return Err(BackendError { "enumerate physical devices"_Str, BackendCause::Native(result) });
    Vec<vvk::PhysicalDevice> deviceList;
    for (rstd::uint32_t i {}; i < count; ++i)
        deviceList.push(vvk::PhysicalDevice(handles[usize(i)], m_vinst.Dispatch()));

    auto logGpu = [](const VkPhysicalDeviceProperties& props) {
        rstd_info("vulkan device: {}", props.deviceName);
    };

    vvk::PhysicalDevice        final_gpu;
    VkPhysicalDeviceProperties final_props;

    for (const auto& d : deviceList) {
        VkPhysicalDeviceIDProperties device_id_props {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = nullptr
        };
        VkPhysicalDeviceProperties2 props2 { .sType =
                                                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &device_id_props };
        d.GetProperties2KHR(props2);
        auto& props = props2.properties;
        if (! uuid.is_empty()) {
            auto device_uuid = slice<rstd::uint8_t>::from_raw_parts(device_id_props.deviceUUID,
                                                                    usize(VK_UUID_SIZE));
            if (uuid == device_uuid) {
                final_props = props;
                final_gpu   = d;
                break;
            }
        } else {
            if (checkgpu->operator()(d)) {
                final_props = props;
                final_gpu   = d;
                break;
            }
        }
    }
    if (final_gpu) {
        logGpu(final_props);
        m_gpu = final_gpu;
        return Ok(empty {});
    } else {
        return Err(BackendError { "select physical device"_Str,
                                  BackendCause::Unsupported("no matching physical device"_Str) });
    }
}

const vvk::Instance&       Instance::inst() const { return m_vinst; }
const vvk::PhysicalDevice& Instance::gpu() const { return m_gpu; }
const vvk::SurfaceKHR&     Instance::surface() const { return m_surface; }

bool Instance::offscreen() const { return ! m_surface; }

void Instance::setSurface(VkSurfaceKHR sf) {
    m_surface = vvk::SurfaceKHR(sf, *m_vinst, m_vinst.Dispatch());
}

bool Instance::supportExt(ref<str> name) const { return m_extensions.contains(name); }
bool Instance::supportLayer(ref<str> name) const { return m_layers.contains(name); }

void Instance::Destroy() {}

auto Instance::TryCreate(Instance& inst, slice<Extension> instExts, slice<InstanceLayer> instLayers,
                         rstd::uint32_t api_version) -> Result<empty, BackendError> {
    auto loader = vvk::VulkanLoader::Open();
    if (loader.is_err()) {
        auto error = loader.unwrap_err();
        return Err(BackendError { "open Vulkan loader"_Str,
                                  BackendCause::Loader(error.kind,
                                                       rstd::move(error.message),
                                                       DispatchFailure::capture(error.dispatch)) });
    }
    inst.m_loader      = Some(loader.unwrap_unchecked());
    const auto& global = inst.m_loader->global();

    auto enumerated = EnumerateProperties<VkExtensionProperties>(
        inst.m_extensions,
        [&](rstd::uint32_t* count, VkExtensionProperties* data) {
            return global.vkEnumerateInstanceExtensionProperties(nullptr, count, data);
        },
        "enumerate instance extensions"_str);
    if (enumerated.is_err()) return enumerated;
    auto requested_exts = Vec<Extension>::from(base_inst_exts.as_slice());
    requested_exts.extend_from_slice(instExts);
    auto exts = detail::SelectExtensionNames(inst.m_extensions, requested_exts.as_slice());
    if (exts.is_err())
        return Err(BackendError { "select instance extensions"_Str,
                                  BackendCause::Unsupported(exts.unwrap_err()) });

    auto enumerated_layers = EnumerateProperties<VkLayerProperties>(
        inst.m_layers,
        [&](rstd::uint32_t* count, VkLayerProperties* data) {
            return global.vkEnumerateInstanceLayerProperties(count, data);
        },
        "enumerate instance layers"_str);
    if (enumerated_layers.is_err()) return enumerated_layers;
    auto requested_layers = Vec<InstanceLayer>::from(base_inst_layers.as_slice());
    requested_layers.extend_from_slice(instLayers);
    auto layers = detail::SelectExtensionNames(inst.m_layers, requested_layers.as_slice());
    if (layers.is_err())
        return Err(BackendError { "select instance layers"_Str,
                                  BackendCause::Unsupported(layers.unwrap_err()) });
    auto created = CreateInstance(
        &inst.m_vinst, exts->as_slice(), layers->as_slice(), inst.m_dld, api_version, global);
    if (created.is_err()) return created;
    inst.m_api_version        = api_version;
    inst.m_enabled_extensions = rstd::move(exts).unwrap();

    auto debug = SetupDebugCallback(inst.m_vinst);
    if (debug.is_err()) return Err(debug.unwrap_err());
    inst.m_debug_utils = debug.unwrap();
    return Ok(empty {});
}
