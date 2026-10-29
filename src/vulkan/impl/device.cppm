module;

#include <rstd/macro.hpp>

#include "vvk/macros.hpp"

module vrento:vulkan.impl.device;

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

namespace
{

auto EnumateDeviceExts(const vvk::PhysicalDevice& gpu, BTreeSet<String>& set)
    -> Result<empty, BackendError> {
    Vec<VkExtensionProperties> properties;
    auto                       result = gpu.EnumerateDeviceExtensionProperties(properties);
    if (result != VK_SUCCESS)
        return Err(
            BackendError { "enumerate device extensions"_Str, BackendCause::Native(result) });
    for (auto& ext : properties)
        set.insert(String::make(CStr::from_ptr(ext.extensionName).to_str().unwrap()));
    return Ok(empty {});
}

} // namespace

bool Device::CheckGPU(vvk::PhysicalDevice gpu, slice<Extension> exts, VkSurfaceKHR surface) {
    return ValidateGPU(gpu, exts, surface).is_ok();
}
auto Device::ValidateGPU(vvk::PhysicalDevice gpu, slice<Extension> exts, VkSurfaceKHR surface)
    -> Result<empty, BackendError> {
    auto props = gpu.GetQueueFamilyProperties();

    bool     has_graphics_queue { false };
    bool     has_present_queue { false };
    unsigned index { 0 };
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) has_graphics_queue = true;
        if (surface) {
            bool ok { false };
            auto result = gpu.GetSurfaceSupportKHR(index, surface, ok);
            if (result != VK_SUCCESS)
                return Err(
                    BackendError { "query present support"_Str, BackendCause::Native(result) });
            if (ok) has_present_queue = true;
        }
        index++;
    }
    if (! has_graphics_queue)
        return Err(BackendError { "validate device"_Str,
                                  BackendCause::Unsupported("graphics queue"_Str) });
    if (surface && ! has_present_queue)
        return Err(
            BackendError { "validate device"_Str, BackendCause::Unsupported("present queue"_Str) });

    auto extensions = BTreeSet<String>::make();
    auto enumerated = EnumateDeviceExts(gpu, extensions);
    if (enumerated.is_err()) return enumerated;
    bool requires_timeline_semaphore { false };
    bool requires_maintenance { false };
    for (auto& ext : exts) {
        if (ext.required) {
            if (! extensions.contains(ext.name))
                return Err(BackendError { "validate device extensions"_Str,
                                          BackendCause::Unsupported(String::make(ext.name)) });
            if (ext.name == "VK_EXT_swapchain_maintenance1"_str) requires_maintenance = true;
            if (ext.name ==
                CStr::from_ptr(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME).to_str().unwrap()) {
                requires_timeline_semaphore = true;
            }
        }
    }
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance_features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
    };
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext = nullptr,
    };
    VkPhysicalDeviceFeatures2KHR features2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
        .pNext = requires_timeline_semaphore
                     ? static_cast<void*>(&timeline_features)
                     : (requires_maintenance ? &maintenance_features : nullptr),
    };
    if (requires_timeline_semaphore && requires_maintenance)
        timeline_features.pNext = &maintenance_features;
    gpu.GetFeatures2KHR(features2);
    if (requires_maintenance && ! maintenance_features.swapchainMaintenance1)
        return Err(BackendError { "validate device"_Str,
                                  BackendCause::Unsupported("swapchainMaintenance1"_Str) });
#if ! __is_target_os(macos)
    if (! features2.features.geometryShader)
        return Err(BackendError { "validate device"_Str,
                                  BackendCause::Unsupported("geometryShader"_Str) });
#endif
    if (requires_timeline_semaphore && ! timeline_features.timelineSemaphore)
        return Err(BackendError { "validate device"_Str,
                                  BackendCause::Unsupported("timelineSemaphore"_Str) });
    return Ok(empty {});
}

auto Device::ChooseDeviceQueue(VkSurfaceKHR surface)
    -> Result<Vec<VkDeviceQueueCreateInfo>, BackendError> {
    Vec<VkDeviceQueueCreateInfo> queues;

    auto props = m_gpu.GetQueueFamilyProperties();

    Vec<rstd::uint32_t> graphic_indexs, present_indexs;
    rstd::uint32_t      index = 0;
    for (auto& prop : props) {
        if (prop.queueFlags & VK_QUEUE_GRAPHICS_BIT) graphic_indexs.emplace_back(index);
        index++;
    }
    if (graphic_indexs.is_empty())
        return Err(BackendError { "choose device queues"_Str,
                                  BackendCause::Unsupported("graphics queue"_Str) });
    m_graphics_queue.family_index = graphic_indexs[usize()];
    m_graphics_queue.flags        = props[usize(m_graphics_queue.family_index)].queueFlags;
    const static float defaultQueuePriorities[] = { 0.0f, 0.0f };
    m_present_queue.family_index                = graphic_indexs[usize()];
    if (surface) {
        index = 0;
        for (auto& prop : props) {
            (void)prop;
            bool ok { false };
            auto result = m_gpu.GetSurfaceSupportKHR(index, surface, ok);
            if (result != VK_SUCCESS)
                return Err(
                    BackendError { "query present support"_Str, BackendCause::Native(result) });
            if (ok) present_indexs.emplace_back(index);
            index++;
        }
        if (present_indexs.is_empty()) {
            return Err(BackendError { "choose device queues"_Str,
                                      BackendCause::Unsupported("present queue"_Str) });
        } else {
            m_present_queue.family_index = present_indexs[usize()];
        }
    }
    for (rstd::uint32_t i = 0; i < props.len().to_primitive(); ++i) {
        if (props[usize(i)].queueCount == 0) continue;
        VkDeviceQueueCreateInfo info {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = i,
            .queueCount       = rstd::cmp::min(props[usize(i)].queueCount, rstd::uint32_t(2)),
            .pQueuePriorities = defaultQueuePriorities,
        };
        queues.push(rstd::move(info));
    }
    return Ok(rstd::move(queues));
}

auto Device::TryCreate(Instance& inst, slice<Extension> exts, VkExtent2D extent, Device& device,
                       SwapchainOptions swapchain_options) -> Result<empty, BackendError> {
    auto valid = ValidateGPU(inst.gpu(), exts, *inst.surface());
    if (valid.is_err()) return valid;
    device.m_instance             = *inst.inst();
    device.m_instance_api_version = inst.api_version();
    device.m_gpu                  = inst.gpu();
    device.m_limits               = inst.gpu().GetProperties().limits;
    device.set_out_extent(extent);
    device.m_enabled_instance_extensions = Vec<CString>::from(inst.enabled_extensions());

    auto enumerated = EnumateDeviceExts(inst.gpu(), device.m_extensions);
    if (enumerated.is_err()) return enumerated;
    auto selected = detail::SelectExtensionNames(device.m_extensions, exts);
    if (selected.is_err())
        return Err(BackendError { "select device extensions"_Str,
                                  BackendCause::Unsupported(selected.unwrap_err()) });
    device.m_enabled_device_extensions = rstd::move(selected).unwrap();
    auto tested_exts_c = Vec<const char*>::with_capacity(device.m_enabled_device_extensions.len());
    auto tested_exts   = BTreeSet<String>::make();
    for (const auto& name : device.m_enabled_device_extensions) {
        tested_exts_c.push(name.as_ptr());
        tested_exts.insert(String::make(name.as_ref().to_str().unwrap()));
    }
    device.m_instance_dispatch = &inst.inst().Dispatch();
    bool rq_surface            = ! inst.offscreen();

    // The WE particle vertex ABI can use a geometry-shader or expanded-quad path
    // on Apple/Metal, where Vulkan geometry shaders are not available.
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR supported_timeline {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext = nullptr,
    };
    const bool has_depth_clip_extension = tested_exts.contains(
        CStr::from_ptr(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME).to_str().unwrap());
    VkPhysicalDeviceDepthClipEnableFeaturesEXT supported_depth_clip {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
    };
    VkPhysicalDeviceSynchronization2FeaturesKHR supported_sync2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext = has_depth_clip_extension ? &supported_depth_clip : nullptr,
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures supported_ycbcr {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .pNext = &supported_sync2,
    };
    const bool maintenance = tested_exts.contains("VK_EXT_swapchain_maintenance1"_str);
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT supported_maintenance {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
    };
    if (maintenance) supported_ycbcr.pNext = &supported_maintenance;
    supported_maintenance.pNext = &supported_sync2;
    supported_timeline.pNext    = &supported_ycbcr;
    VkPhysicalDeviceFeatures2KHR supported2 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
        .pNext = &supported_timeline,
    };
    device.m_gpu.GetFeatures2KHR(supported2);
    if (! supported_timeline.timelineSemaphore) {
        return Err(BackendError { "create device"_Str,
                                  BackendCause::Unsupported("timelineSemaphore"_Str) });
    }
#if ! __is_target_os(macos)
    if (! supported2.features.geometryShader) {
        return Err(
            BackendError { "create device"_Str, BackendCause::Unsupported("geometryShader"_Str) });
    }
#endif
    const bool enable_shader_output_viewport_index = tested_exts.contains(
        CStr::from_ptr(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME).to_str().unwrap());
    const bool enable_multi_viewport =
        enable_shader_output_viewport_index && supported2.features.multiViewport;
    const auto d32_features =
        device.m_gpu.GetFormatProperties(VK_FORMAT_D32_SFLOAT).optimalTilingFeatures;
    const bool sampled_depth_d32 =
        (d32_features &
         (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) ==
        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkPhysicalDeviceFeatures enabled {};
#if __is_target_os(macos)
    enabled.geometryShader = supported2.features.geometryShader;
#else
    enabled.geometryShader = VK_TRUE;
#endif
    enabled.sampleRateShading = supported2.features.sampleRateShading;
    enabled.samplerAnisotropy = supported2.features.samplerAnisotropy;
    device.m_sampler_anisotropy = enabled.samplerAnisotropy == VK_TRUE;
    enabled.multiViewport     = enable_multi_viewport ? VK_TRUE : VK_FALSE;
    enabled.depthClamp        = supported2.features.depthClamp;
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR enabled_timeline {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext             = nullptr,
        .timelineSemaphore = VK_TRUE,
    };
    const bool enable_sync2 =
        tested_exts.contains(
            CStr::from_ptr(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME).to_str().unwrap()) &&
        supported_sync2.synchronization2;
    const bool enable_depth_clip = has_depth_clip_extension && supported_depth_clip.depthClipEnable;
    VkPhysicalDeviceDepthClipEnableFeaturesEXT enabled_depth_clip {
        .sType           = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
        .depthClipEnable = VK_TRUE,
    };
    VkPhysicalDeviceSynchronization2FeaturesKHR enabled_sync2 {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext            = enable_depth_clip ? &enabled_depth_clip : nullptr,
        .synchronization2 = enable_sync2 ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures enabled_ycbcr {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .pNext = enable_sync2 ? static_cast<void*>(&enabled_sync2)
                              : (enable_depth_clip ? &enabled_depth_clip : nullptr),
        .samplerYcbcrConversion = supported_ycbcr.samplerYcbcrConversion,
    };
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT enabled_maintenance {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        .pNext = enabled_ycbcr.pNext,
        .swapchainMaintenance1 = VK_TRUE,
    };
    if (maintenance) {
        if (! supported_maintenance.swapchainMaintenance1)
            return Err(BackendError { "create device"_Str,
                                      BackendCause::Unsupported("swapchainMaintenance1"_Str) });
        enabled_ycbcr.pNext = &enabled_maintenance;
    }
    enabled_timeline.pNext = &enabled_ycbcr;

    auto queues = device.ChooseDeviceQueue(*inst.surface());
    if (queues.is_err()) return Err(queues.unwrap_err());
    auto               queue_create_infos = queues.unwrap();
    VkDeviceCreateInfo device_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_timeline,
        .queueCreateInfoCount =
            static_cast<rstd::uint32_t>(queue_create_infos.len().to_primitive()),
        .pQueueCreateInfos       = queue_create_infos.data(),
        .enabledExtensionCount   = static_cast<rstd::uint32_t>(tested_exts_c.len().to_primitive()),
        .ppEnabledExtensionNames = tested_exts_c.data(),
        .pEnabledFeatures        = &enabled,
    };
    auto created = vvk::Device::Create(
        device.m_device, *device.m_gpu, inst.inst().Dispatch(), device_info, device.dld);
    if (created.is_err())
        return Err(BackendError {
            "create device"_Str,
            BackendCause::Dispatch(DispatchFailure::capture(created.unwrap_err())) });

    device.m_graphics_queue.handle = device.m_device.GetQueue(device.m_graphics_queue.family_index);
    device.m_present_queue.handle  = device.m_device.GetQueue(device.m_present_queue.family_index);
    auto queue_properties          = device.m_gpu.GetQueueFamilyProperties();
    device.m_present_queue.flags =
        queue_properties[usize(device.m_present_queue.family_index)].queueFlags;
    device.m_queues.clear();
    for (const auto& info : queue_create_infos)
        for (rstd::uint32_t index {}; index < info.queueCount; ++index)
            device.m_queues.push({ device.m_device.GetQueue(info.queueFamilyIndex, index),
                                   info.queueFamilyIndex,
                                   queue_properties[usize(info.queueFamilyIndex)].queueFlags,
                                   index });
    rstd::uint32_t max_push_descriptors {};
    if (tested_exts.contains(
            CStr::from_ptr(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME).to_str().unwrap())) {
        VkPhysicalDevicePushDescriptorPropertiesKHR push_properties {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
        };
        VkPhysicalDeviceProperties2 properties {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &push_properties,
        };
        device.m_gpu.GetProperties2KHR(properties);
        max_push_descriptors = push_properties.maxPushDescriptors;
    }
    device.m_capabilities = DeviceCapabilities {
        .timeline_semaphore = true,
        .geometry_shader    = supported2.features.geometryShader != VK_FALSE,
        .synchronization2   = enable_sync2,
        .push_descriptor    = tested_exts.contains(
            CStr::from_ptr(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME).to_str().unwrap()),
        .max_push_descriptors                 = max_push_descriptors,
        .multi_viewport                       = enable_multi_viewport,
        .shader_output_viewport_index         = enable_shader_output_viewport_index,
        .sampled_depth_d32                    = sampled_depth_d32,
        .depth_clamp                          = supported2.features.depthClamp != VK_FALSE,
        .depth_clip_enable                    = enable_depth_clip,
        .max_geometry_output_vertices         = device.m_limits.maxGeometryOutputVertices,
        .max_geometry_total_output_components = device.m_limits.maxGeometryTotalOutputComponents,
        .memory_budget                        = tested_exts.contains(
            CStr::from_ptr(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME).to_str().unwrap()),
        .external_memory_fd = tested_exts.contains(
            CStr::from_ptr(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME).to_str().unwrap()),
        .external_memory_dma_buf = tested_exts.contains(
            CStr::from_ptr(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME).to_str().unwrap()),
        .drm_format_modifier = tested_exts.contains(
            CStr::from_ptr(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME).to_str().unwrap()),
        .foreign_queue = tested_exts.contains(
            CStr::from_ptr(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME).to_str().unwrap()),
        .graphics_queue_family = device.m_graphics_queue.family_index,
        .present_queue_family  = device.m_present_queue.family_index,
    };

    if (rq_surface) {
        auto result = Swapchain::CreateResult(
            device, *inst.surface(), extent, device.m_swapchain, swapchain_options);
        if (result != VK_SUCCESS)
            return Err(BackendError { "create swapchain"_Str, BackendCause::Native(result) });
        device.set_out_extent(device.m_swapchain.extent());
    }
    {
        VkCommandPoolCreateInfo info { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = device.m_graphics_queue.family_index };
        auto result = device.m_device.CreateCommandPool(info, device.m_command_pool);
        if (result != VK_SUCCESS)
            return Err(BackendError { "create command pool"_Str, BackendCause::Native(result) });
    }
    {
        auto allocator =
            vvk::MemoryAllocator::Create(*device.m_gpu, inst.inst().Dispatch(), device.dld);
        if (allocator.is_err())
            return Err(BackendError { "create memory allocator"_Str,
                                      BackendCause::Memory(allocator.unwrap_err()) });
        device.m_allocator = allocator.unwrap_unchecked();
    }
    return Ok(empty {});
}

VkDeviceSize Device::GetUsage() const { return MemoryBudget().usage; }

auto Device::MemoryBudget() const -> MemoryBudgetSnapshot {
    const auto           budgets = m_allocator.budget();
    MemoryBudgetSnapshot snapshot;
    for (rstd::uint32_t index = 0; index < budgets.heap_count; ++index) {
        snapshot.usage += budgets.heaps[index].usage;
        snapshot.budget += budgets.heaps[index].budget;
    }
    return snapshot;
}

void Device::Destroy() { VVK_CHECK(m_device.WaitIdle()); }

Device::Device() = default;
Device::~Device() {}

bool Device::supportExt(ref<str> name) const { return m_extensions.contains(name); }

bool Device::recreateSwapchain(VkSurfaceKHR surface, VkExtent2D extent, SwapchainOptions options) {
    return recreateSwapchainResult(surface, extent, options) == VK_SUCCESS;
}
auto Device::recreateSwapchainResult(VkSurfaceKHR surface, VkExtent2D extent,
                                     SwapchainOptions options) -> VkResult {
    auto result =
        observe_result(Swapchain::CreateResult(*this, surface, extent, m_swapchain, options));
    if (result == VK_SUCCESS) set_out_extent(m_swapchain.extent());
    return result;
}

void Device::releaseSwapchain() {
    m_swapchain.reset();
    m_extent = {};
}
