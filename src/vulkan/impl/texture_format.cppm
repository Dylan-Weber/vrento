module;
#include <rstd/macro.hpp>

module vrento:vulkan.impl.texture_format;

import :vulkan.api;
import :types.texture;
import rstd;

namespace vrento::vulkan
{
VkFormat ToVkType(TextureFormat format) {
    switch (format) {
    case TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureFormat::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    rstd_assert(false);
    return VK_FORMAT_UNDEFINED;
}

VkSamplerAddressMode ToVkType(TextureWrap wrap) {
    switch (wrap) {
    case TextureWrap::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case TextureWrap::MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TextureWrap::REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
    rstd_assert(false);
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFilter ToVkType(TextureFilter filter) {
    switch (filter) {
    case TextureFilter::LINEAR: return VK_FILTER_LINEAR;
    case TextureFilter::NEAREST: return VK_FILTER_NEAREST;
    }
    rstd_assert(false);
    return VK_FILTER_NEAREST;
}
} // namespace vrento::vulkan
