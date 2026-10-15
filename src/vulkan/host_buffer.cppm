export module vrento:vulkan.host_buffer;
export import :vulkan.graph;
import rstd;

using namespace rstd::prelude;

export namespace vrento::vulkan
{
namespace tex = vrento::texture;
// Flush host writes before preparation; keep their bytes immutable until retirement.
struct GraphHostBufferBinding {
    DeviceSession        session;
    vvk::AllocatedBuffer buffer;
    GraphSerial          bytes;
    auto                 Info() const -> GraphBindingInfo {
        return { GraphResourceKind::Buffer,
                 { session.epoch(),
                   session.epoch(),
                   GraphSerial(reinterpret_cast<rstd::uintptr_t>(buffer.handle())) },
                 bytes };
    }
    auto Texture() const -> Option<tex::TextureViewRef> { return None(); }
    auto Freeze(bool) const -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError>;
    auto Use(tex::Submission& work, const PassResourceUse& use) const
        -> Result<empty, GraphExecutionError> {
        if (! buffer.valid() || ! bytes || use.range.offset > bytes || ! use.range.size ||
            use.range.size > bytes - use.range.offset || use.access != GraphAccess::Read ||
            use.version || (use.usage != PassUsage::Storage && use.usage != PassUsage::Transfer))
            return Err(GraphExecutionError { GraphExecutionStage::Access });
        auto command = native_recording(work, session);
        if (command.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Access,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Texture(command.unwrap_err()) });
        VkBufferMemoryBarrier barrier { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask       = use.usage == PassUsage::Storage ? VK_ACCESS_SHADER_READ_BIT
                                                                      : VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer                                            = buffer.handle();
        barrier.offset                                            = use.range.offset;
        barrier.size                                              = use.range.size;
        session.device().handle().Dispatch().vkCmdPipelineBarrier(
            command.unwrap(),
            VK_PIPELINE_STAGE_HOST_BIT,
            texture_stages(graph_stages(use.stages)),
            0,
            0,
            nullptr,
            1,
            &barrier,
            0,
            nullptr);
        return Ok(empty {});
    }
};
} // namespace vrento::vulkan
namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphBinding, vrento::vulkan::GraphHostBufferBinding>
    : ImplBase<vrento::vulkan::GraphHostBufferBinding> {
    auto Info() const -> vrento::vulkan::GraphBindingInfo { return self().Info(); }
    auto Texture() const -> Option<vrento::texture::TextureViewRef> { return self().Texture(); }
    auto Use(vrento::texture::Submission& work, const vrento::vulkan::PassResourceUse& use) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Use(work, use);
    }
    auto Freeze(bool read_only) const -> Result<sync::Arc<dyn<vrento::vulkan::GraphBinding>>,
                                                vrento::vulkan::GraphExecutionError> {
        return self().Freeze(read_only);
    }
    auto Release(vrento::texture::Submission& work, rstd::uint32_t family,
                 const vrento::vulkan::PassResourceUse& next) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        auto current = vrento::vulkan::native_queue(work, self().session);
        if (current.is_err() || current.unwrap().family_index != family)
            return Err(vrento::vulkan::GraphExecutionError {
                vrento::vulkan::GraphExecutionStage::Unsupported, {}, next.resource });
        return Ok(empty {});
    }
};
} // namespace rstd
namespace vrento::vulkan
{
auto GraphHostBufferBinding::Freeze(bool) const
    -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError> {
    return Ok(rstd::sync::Arc<dyn<GraphBinding>>::make(
        GraphHostBufferBinding { session, buffer.clone(), bytes }));
}
} // namespace vrento::vulkan
