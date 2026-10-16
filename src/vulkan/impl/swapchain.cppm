module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module vrento:vulkan.impl.swapchain;

import :vulkan.api;

import rstd;
import rstd.log;

using namespace rstd::prelude;
using namespace vrento::vulkan;

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    Vec<VkSurfaceFormatKHR>  formats;
    Vec<VkPresentModeKHR>    presentModes;
};

namespace
{

VkResult querySwapChainSupport(const vvk::PhysicalDevice& gpu, VkSurfaceKHR surface,
                           SwapChainSupportDetails& details) {
    auto result = gpu.GetSurfaceCapabilitiesKHR(surface, details.capabilities);
    if (result != VK_SUCCESS) return result;
    result = gpu.GetSurfaceFormatsKHR(surface, details.formats);
    if (result != VK_SUCCESS) return result;
    return gpu.GetSurfacePresentModesKHR(surface, details.presentModes);
}

Option<VkSurfaceFormatKHR> chooseSwapSurfaceFormat(slice<VkSurfaceFormatKHR> formats,
                                                       bool require_srgb) {
    if (formats.is_empty()) return None();
    if (formats.len() == usize(1) && formats[usize()].format == VK_FORMAT_UNDEFINED) {
        if (require_srgb && formats[usize()].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return None();
        return Some(VkSurfaceFormatKHR { require_srgb ? VK_FORMAT_B8G8R8A8_SRGB
                                                      : VK_FORMAT_B8G8R8A8_UNORM,
                                        formats[usize()].colorSpace });
    }
    for (const auto& available : formats) {
        bool preferred = require_srgb
            ? available.format == VK_FORMAT_B8G8R8A8_SRGB || available.format == VK_FORMAT_R8G8B8A8_SRGB
            : available.format == VK_FORMAT_B8G8R8A8_UNORM || available.format == VK_FORMAT_R8G8B8A8_UNORM;
        if (preferred && available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return Some(VkSurfaceFormatKHR(available));
    }
    return require_srgb ? None<VkSurfaceFormatKHR>() : Some(VkSurfaceFormatKHR(formats[usize()]));
}

VkExtent2D GetSwapChainExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extent) {
    if (caps.currentExtent.width != ~rstd::uint32_t{}) return caps.currentExtent;
    if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    return extent;
}

Result<vvk::ImageView, VkResult> CreateSwapImageView(const vvk::Device& device, VkFormat format,
                                           VkImage image) {
    VkImageViewCreateInfo ci {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    vvk::ImageView view;
    auto result = device.CreateImageView(ci, view);
    if (result != VK_SUCCESS) return Err(result);
    return Ok(rstd::move(view));
}
} // namespace

void Swapchain::reset() {
    m_imageviews.clear();
    m_images.clear();
    m_handle.reset();
    m_extent = {};
    m_format = {};
    m_present_mode = {};
}
const vvk::SwapchainKHR& Swapchain::handle() const { return m_handle; }
VkFormat                 Swapchain::format() const { return m_format.format; }
VkExtent2D               Swapchain::extent() const { return m_extent; }

slice<ImageParameters> Swapchain::images() const { return m_images.as_slice(); }

VkPresentModeKHR Swapchain::presentMode() const { return m_present_mode; }

bool Swapchain::Create(Device& device, VkSurfaceKHR surface, VkExtent2D extent, Swapchain& output,
                       SwapchainOptions options) {
    return CreateResult(device, surface, extent, output, options) == VK_SUCCESS;
}
auto Swapchain::CreateResult(Device& device, VkSurfaceKHR surface, VkExtent2D extent, Swapchain& output,
                             SwapchainOptions options) -> VkResult {
    Swapchain swap;
    SwapChainSupportDetails swap_details;
    auto support = querySwapChainSupport(device.gpu(), surface, swap_details);
    if (support != VK_SUCCESS) return support;
    if (unsigned(options.presentation) > unsigned(PresentationMode::FifoRelaxed))
        return VK_ERROR_INITIALIZATION_FAILED;

    auto format = chooseSwapSurfaceFormat(swap_details.formats.as_slice(), options.require_srgb);
    if (format.is_none()) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    swap.m_format = *format;

    auto& surfaceCapabilities = swap_details.capabilities;

    rstd::uint32_t image_count = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0 && image_count > surfaceCapabilities.maxImageCount)
        image_count = surfaceCapabilities.maxImageCount;
    swap.m_extent = GetSwapChainExtent(surfaceCapabilities, extent);
    if (!swap.m_extent.width || !swap.m_extent.height ||
        (surfaceCapabilities.supportedUsageFlags & options.usage) != options.usage) return VK_ERROR_INITIALIZATION_FAILED;

    swap.m_present_mode                          = VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR modes[] { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                     VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR };
    auto preferred = modes[unsigned(options.presentation)];
    for (auto available : swap_details.presentModes)
        if (available == preferred) swap.m_present_mode = preferred;
    VkSurfaceTransformFlagBitsKHR preTransform   = surfaceCapabilities.currentTransform;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    while (!(surfaceCapabilities.supportedCompositeAlpha & compositeAlpha)) {
        compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(static_cast<unsigned>(compositeAlpha) << 1);
        if (!compositeAlpha || static_cast<unsigned>(compositeAlpha) > VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    rstd::uint32_t families[] { device.graphics_queue().family_index, device.present_queue().family_index };
    bool separate = families[0] != families[1];

    VkSwapchainCreateInfoKHR sci {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext            = nullptr,
        .surface          = surface,
        .minImageCount    = image_count,
        .imageFormat      = swap.m_format.format,
        .imageColorSpace  = swap.m_format.colorSpace,
        .imageExtent      = swap.m_extent,
        .imageArrayLayers = 1,
        .imageUsage       = options.usage,
        .imageSharingMode = separate ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = separate ? 2u : 0u,
        .pQueueFamilyIndices = separate ? families : nullptr,
        .preTransform     = preTransform,
        .compositeAlpha   = compositeAlpha,
        .presentMode      = swap.m_present_mode,
        .clipped          = true,
        .oldSwapchain     = *output.m_handle,
    };

    auto created = device.device().CreateSwapchainKHR(sci, swap.m_handle);
    // Passing oldSwapchain retires it even when creation fails.
    output.reset();
    if (created != VK_SUCCESS) return created;
    {
        Vec<VkImage> images;
        auto result = swap.m_handle.GetImages(images);
        if (result != VK_SUCCESS) return result;
        swap.m_images.reserve(images.len());
        swap.m_imageviews.reserve(images.len());
        for (auto image : images) {
            ImageParameters image_paras {};
            image_paras.handle = image;
            image_paras.extent = { swap.m_extent.width, swap.m_extent.height, 1 };
            auto created_view = CreateSwapImageView(device.device(), swap.m_format.format, image);
            if (created_view.is_err()) return created_view.unwrap_err();
            auto view = created_view.unwrap();
            image_paras.view = *view;
            swap.m_imageviews.push(rstd::move(view));
            swap.m_images.push(rstd::move(image_paras));
        }
    }
    output = rstd::move(swap);
    return VK_SUCCESS;
}
