export module vrento:resource_registry.local_output;
import rstd;
import :vulkan.texture;

export namespace vrento::resource_registry
{
using namespace rstd::prelude;
using namespace rstd::literals;

#if defined(__linux__)
class SessionLocalOutput final : public ExSwapchain {
public:
    SessionLocalOutput(vulkan::DeviceSession session, ExSwapchainOwner output,
                       Vec<rstd::os::fd::OwnedFd> descriptors, u64 generation)
        : m_session(rstd::move(session)),
          m_descriptors(rstd::move(descriptors)),
          m_output(rstd::move(output)),
          m_generation(generation) {}
    ~SessionLocalOutput() override { (void)m_session.wait_idle(); }

    void poll() override { Inner().poll(); }
    auto acquireRenderTarget() -> FrameSurfaceAcquireResult override {
        auto result = Inner().acquireRenderTarget();
        if (result.status != FrameSurfaceAcquireStatus::Acquired) return result;
        m_pending                              = Some(rstd::move(result.completion));
        result.lease.identity.owner_generation = m_generation;
        m_identity                             = result.lease.identity;
        result.completion                      = MakeCompletionCapability(m_identity);
        return result;
    }
    int  takeLastFrameSyncFd() override { return Inner().takeLastFrameSyncFd(); }
    auto eatFrame() -> ExHandle* override { return Inner().eatFrame(); }
    auto snapshot_all_slots() -> rstd::array<ExHandle*, 3> override {
        return Inner().snapshot_all_slots();
    }
    unsigned width() const override { return Inner().width(); }
    unsigned height() const override { return Inner().height(); }
    VkFormat format() const override { return Inner().format(); }
    bool     ready() const override { return Inner().ready(); }
    void     setOnReadyChanged(Option<ExSwapchainReadyCallback> callback) override {
        Inner().setOnReadyChanged(rstd::move(callback));
    }

private:
    auto Inner() const -> ExSwapchain& { return m_output->AsSwapchain(); }
    auto CompleteRendered(FrameSurfaceIdentity identity, int fd)
        -> FrameSurfaceCompletionResult override {
        if (m_pending.is_none() || identity != m_identity) {
            if (fd >= 0) {
                auto close = rstd::os::fd::OwnedFd::from_raw_fd(fd);
            }
            return { .status   = m_pending.is_none() ? FrameSurfaceCompletionStatus::NotPending
                                                     : FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        }
        auto pending    = m_pending.take();
        auto result     = pending->Submit(fd);
        result.identity = identity;
        m_identity      = {};
        return result;
    }
    auto AbortRenderTarget(FrameSurfaceIdentity identity) -> FrameSurfaceCompletionResult override {
        if (m_pending.is_none() || identity != m_identity)
            return { .status   = m_pending.is_none() ? FrameSurfaceCompletionStatus::NotPending
                                                     : FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        auto pending    = m_pending.take();
        auto result     = pending->Abort();
        result.identity = identity;
        m_identity      = {};
        return result;
    }

    vulkan::DeviceSession                    m_session;
    Vec<rstd::os::fd::OwnedFd>               m_descriptors;
    ExSwapchainOwner                         m_output;
    u64                                      m_generation;
    Option<FrameSurfaceCompletionCapability> m_pending;
    FrameSurfaceIdentity                     m_identity;
};
#endif

inline auto CreateSessionLocalOutput(vulkan::TextureContext& context, unsigned width,
                                     unsigned height, VkImageTiling tiling)
    -> Option<ExSwapchainOwner> {
#if defined(__linux__)
    if (! width || ! height ||
        (tiling != VK_IMAGE_TILING_LINEAR && tiling != VK_IMAGE_TILING_OPTIMAL))
        return None();
    // The local output protocol exports linear planes, including legacy OPTIMAL requests.
    tiling              = VK_IMAGE_TILING_LINEAR;
    const auto& session = context.session();
    const auto& device  = session.device();
    if (! device.supportExt("VK_EXT_external_memory_dma_buf"_str) || session.lost()) return None();
    const auto& native = device.handle();
    if (! native.Dispatch().vkGetMemoryFdKHR) return None();
    constexpr VkFormat                      format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkImageUsageFlags             usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                     VK_IMAGE_USAGE_SAMPLED_BIT;
    VkPhysicalDeviceExternalImageFormatInfo external_query {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkPhysicalDeviceImageFormatInfo2 query { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                             &external_query,
                                             format,
                                             VK_IMAGE_TYPE_2D,
                                             tiling,
                                             usage,
                                             0 };
    VkExternalImageFormatProperties  external_support {
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
    };
    VkImageFormatProperties2 support { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                       &external_support };
    const auto&              dispatch = device.instance_dispatch();
    if (! dispatch.resolver) return None();
    auto query_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        dispatch.resolver(dispatch.instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (! query_fn)
        query_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            dispatch.resolver(dispatch.instance, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
    if (! query_fn || query_fn(*device.gpu(), &query, &support) != VK_SUCCESS ||
        ! (external_support.externalMemoryProperties.externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
        width > support.imageFormatProperties.maxExtent.width ||
        height > support.imageFormatProperties.maxExtent.height)
        return None();

    rstd::array<vulkan::LocalExHandle, 3> handles;
    Vec<rstd::os::fd::OwnedFd>            descriptors;
    Vec<VkImage>                          images;
    static Atomic<u64>                    next_generation { u64(1) };
    const auto generation = next_generation.fetch_add(u64(1), Ordering::Relaxed);
    if (generation == u64()) rstd::panic("local output generation exhausted");
    const auto memory = device.gpu().GetMemoryProperties().memoryProperties;
    for (auto& slot : handles) {
        auto&                           image = slot.image;
        VkExternalMemoryImageCreateInfo external {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            nullptr,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        };
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext       = &external;
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = format;
        info.extent      = { width, height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling      = tiling;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (native.CreateImage(info, image.handle) != VK_SUCCESS) return None();
        image.mem_reqs      = native.GetImageMemoryRequirements(*image.handle);
        rstd::uint32_t type = VK_MAX_MEMORY_TYPES;
        for (rstd::uint32_t index {}; index < memory.memoryTypeCount; ++index) {
            if (! (image.mem_reqs.memoryTypeBits & (1u << index))) continue;
            if (type == VK_MAX_MEMORY_TYPES) type = index;
            if (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = index;
                break;
            }
        }
        if (type == VK_MAX_MEMORY_TYPES) return None();
        // Exported DMA-BUF memory requires a dedicated native allocation chain.
        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, *image.handle, VK_NULL_HANDLE
        };
        VkExportMemoryAllocateInfo exported { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                              &dedicated,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkMemoryAllocateInfo       allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exported, image.mem_reqs.size, type
        };
        if (native.AllocateMemory(allocation, image.mem) != VK_SUCCESS ||
            image.handle.BindMemory(*image.mem, 0) != VK_SUCCESS)
            return None();
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = *image.handle;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (native.CreateImageView(view, image.view) != VK_SUCCESS) return None();
        VkSamplerCreateInfo sampler { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter    = VK_FILTER_NEAREST;
        sampler.minFilter    = VK_FILTER_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (native.CreateSampler(sampler, image.sampler) != VK_SUCCESS) return None();
        VkMemoryGetFdInfoKHR fd_info { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                       nullptr,
                                       *image.mem,
                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        int                  fd = -1;
        if (native.Dispatch().vkGetMemoryFdKHR(*native, &fd_info, &fd) != VK_SUCCESS || fd < 0)
            return None();
        descriptors.push(rstd::os::fd::OwnedFd::from_raw_fd(fd));
        const auto layout =
            native.GetImageSubresourceLayout(*image.handle, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 });
        if (layout.rowPitch > rstd::uint64_t(~rstd::uint32_t())) return None();
        image.extent        = info.extent;
        image.generation    = generation;
        image.fd            = fd;
        image.drm_fourcc    = rstd::uint32_t('A') | (rstd::uint32_t('B') << 8) |
                              (rstd::uint32_t('2') << 16) | (rstd::uint32_t('4') << 24);
        image.drm_modifier  = 0;
        image.plane0_offset = layout.offset;
        image.plane0_stride = rstd::uint32_t(layout.rowPitch);
        images.push(VkImage(*image.handle));
    }
    auto local = ExSwapchain::Make<vulkan::LocalExSwapchain>(
        rstd::move(handles), VkExtent2D { width, height }, device.graphics_queue().family_index);
    auto output = ExSwapchain::Make<SessionLocalOutput>(
        session, rstd::move(local), rstd::move(descriptors), generation);
    auto begun = context.Begin();
    if (begun.is_err()) return None();
    auto work = rstd::move(begun).unwrap_unchecked();
    work.retain(output.clone());
    auto command = vulkan::native_recording(work, session);
    if (command.is_err()) return None();
    for (auto image : images) {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        native.Dispatch().vkCmdPipelineBarrier(command.unwrap_unchecked(),
                                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);
    }
    auto submitted = context.Submit(rstd::move(work));
    if (submitted.is_err() || context.wait(submitted.unwrap_unchecked()).is_err()) return None();
    (void)context.Poll();
    return Some(rstd::move(output));
#else
    (void)context;
    (void)width;
    (void)height;
    (void)tiling;
    return None();
#endif
}
} // namespace vrento::resource_registry
