export module vrento:vulkan.graph_surface;
export import :vulkan.graphics_scope;
import rstd;
import :resource_registry.external;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
class SurfaceTarget {
    DeviceSession            session_;
    FrameSurfaceLease        lease_;
    Arc<dyn<rstd::any::Any>> owner_;
    SurfaceTarget(DeviceSession session, FrameSurfaceLease lease, Arc<dyn<rstd::any::Any>> owner)
        : session_(rstd::move(session)), lease_(lease), owner_(rstd::move(owner)) {}

public:
    SurfaceTarget(const SurfaceTarget& other)
        : session_(other.session_), lease_(other.lease_), owner_(other.owner_.clone()) {}
    SurfaceTarget(SurfaceTarget&&) = default;
    static auto create(DeviceSession session, FrameSurfaceLease lease,
                       Arc<dyn<rstd::any::Any>> owner)
        -> Result<SurfaceTarget, resource::ResourceError> {
        auto valid = resource_registry::ExternalResourceBridge::Validate(
            session.device().capabilities(), lease);
        if (valid.is_err()) return Err(valid.unwrap_err());
        if (! owner)
            return Err(
                resource::ResourceError { .kind    = resource::ResourceErrorKind::MissingDefinition,
                                          .message = "surface owner unavailable"_Str });
        return Ok(SurfaceTarget(rstd::move(session), lease, rstd::move(owner)));
    }
    auto session() const -> const DeviceSession& { return session_; }
    auto lease() const -> const FrameSurfaceLease& { return lease_; }
    auto owner() const -> Arc<dyn<rstd::any::Any>> { return owner_.clone(); }
};
class SurfaceGraphBinding {
    DeviceSession            session_;
    FrameSurfaceLease        lease_;
    Arc<dyn<rstd::any::Any>> owner_;
    texture::Identity        identity_;

public:
    SurfaceGraphBinding(const SurfaceGraphBinding& other)
        : session_(other.session_),
          lease_(other.lease_),
          owner_(other.owner_.clone()),
          identity_(other.identity_) {}
    SurfaceGraphBinding(DeviceSession session, FrameSurfaceLease lease,
                        Arc<dyn<rstd::any::Any>> owner)
        : session_(rstd::move(session)),
          lease_(lease),
          owner_(rstd::move(owner)),
          identity_(next_texture_identity(session_)) {}
    auto Info() const -> GraphBindingInfo { return { GraphResourceKind::Texture, identity_ }; }
    auto Texture() const -> Option<texture::TextureViewRef> { return None(); }
    auto Use(texture::Submission& work, const PassResourceUse& use) const
        -> Result<empty, GraphExecutionError> {
        if (! lease_.valid() || ! owner_ || use.range.aspect != 1 || use.range.first_mip ||
            use.range.mip_count != 1 || use.range.first_layer || use.range.layer_count != 1)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, use.resource });
        if (use.usage == PassUsage::Release) {
            auto dependency = recordSurfaceDependency(work, session_, lease_);
            if (dependency.is_err())
                return Err(GraphExecutionError { GraphExecutionStage::Access,
                                                 {},
                                                 use.resource,
                                                 VK_SUCCESS,
                                                 Some(dependency.unwrap_err()) });
            return Ok(empty {});
        }
        texture::Access access;
        if (use.usage == PassUsage::ColorAttachment)
            access = texture::Access::ColorAttachment;
        else if (use.usage == PassUsage::Transfer)
            access = use.access == GraphAccess::Write ? texture::Access::TransferDestination
                                                      : texture::Access::TransferSource;
        else
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, {}, use.resource });
        auto used = recordSurfaceAccess(work,
                                        session_,
                                        lease_,
                                        owner_.clone(),
                                        native_access({ access, graph_stages(use.stages) }));
        if (used.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Access,
                                             {},
                                             use.resource,
                                             VK_SUCCESS,
                                             Some(used.unwrap_err()) });
        return Ok(empty {});
    }
    auto Freeze(bool) const -> Result<Arc<dyn<GraphBinding>>, GraphExecutionError>;
};
struct SurfaceScopeResources {
    DeviceSession            session;
    GraphicsScopeInfo        info;
    Arc<vvk::RenderPass>     pass;
    Arc<vvk::Framebuffer>    framebuffer;
    Arc<dyn<rstd::any::Any>> owner;
    auto                     Info() const -> GraphicsScopeInfo { return info; }
};
class SurfaceScopeBackend {
    GraphicsScopeCache              textures_;
    Option<Arc<GraphicsScopeCache>> shared_textures_;
    ResourceId                      resource_;
    FrameSurfaceLease               lease_;
    Arc<dyn<rstd::any::Any>>        owner_;

public:
    SurfaceScopeBackend(ResourceId resource, FrameSurfaceLease lease,
                        Arc<dyn<rstd::any::Any>> owner)
        : resource_(resource), lease_(lease), owner_(rstd::move(owner)) {}
    SurfaceScopeBackend(ResourceId resource, FrameSurfaceLease lease,
                        Arc<dyn<rstd::any::Any>> owner, Arc<GraphicsScopeCache> textures)
        : shared_textures_(Some(rstd::move(textures))),
          resource_(resource),
          lease_(lease),
          owner_(rstd::move(owner)) {}
    auto Prepare(const GraphicsDescription&, const GraphPrepareContext&)
        -> Result<Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError>;
};
struct SurfaceReleasePass {
    FrameSurfaceLease lease;
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        auto released = recordRelease(context.submission, context.session, lease);
        if (released.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(released.unwrap_err()) });
        return Ok(empty {});
    }
};
struct SurfaceReleaseBackend {
    FrameSurfaceLease lease;
    auto              Prepare(const PassExecutionInfo&, const GraphPrepareContext&)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
inline constexpr GraphSerial surface_release_kind = 0x7672742e73757266ull;
} // namespace vrento::vulkan
namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphBinding, vrento::vulkan::SurfaceGraphBinding>
    : ImplBase<vrento::vulkan::SurfaceGraphBinding> {
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
    auto Release(vrento::texture::Submission&, rstd::uint32_t,
                 const vrento::vulkan::PassResourceUse& next) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return Err(vrento::vulkan::GraphExecutionError {
            vrento::vulkan::GraphExecutionStage::Unsupported, {}, next.resource });
    }
};
template<>
struct Impl<vrento::vulkan::PreparedGraphicsScope, vrento::vulkan::SurfaceScopeResources>
    : ImplBase<vrento::vulkan::SurfaceScopeResources> {
    auto Info() const -> vrento::vulkan::GraphicsScopeInfo { return self().Info(); }
};
template<>
struct Impl<vrento::vulkan::GraphicsScopeBackend, vrento::vulkan::SurfaceScopeBackend>
    : ImplBase<vrento::vulkan::SurfaceScopeBackend> {
    auto Prepare(const vrento::vulkan::GraphicsDescription& description,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<sync::Arc<dyn<vrento::vulkan::PreparedGraphicsScope>>,
                  vrento::vulkan::GraphExecutionError> {
        return self().Prepare(description, context);
    }
};

template<>
struct Impl<vrento::vulkan::VulkanPassBackend, vrento::vulkan::SurfaceReleaseBackend>
    : ImplBase<vrento::vulkan::SurfaceReleaseBackend> {
    auto Prepare(const vrento::vulkan::PassExecutionInfo&   pass,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<sync::Arc<dyn<vrento::vulkan::VulkanPass>>, vrento::vulkan::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};
} // namespace rstd
namespace vrento::vulkan
{
auto SurfaceGraphBinding::Freeze(bool) const
    -> Result<Arc<dyn<GraphBinding>>, GraphExecutionError> {
    if (! lease_.valid() || ! owner_)
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    return Ok(Arc<dyn<GraphBinding>>::make(SurfaceGraphBinding(*this)));
}
auto SurfaceReleaseBackend::Prepare(const PassExecutionInfo&, const GraphPrepareContext&)
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    return Ok(Arc<dyn<VulkanPass>>::make(SurfaceReleasePass { lease }));
}
auto SurfaceScopeBackend::Prepare(const GraphicsDescription& description,
                                  const GraphPrepareContext& context)
    -> Result<Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError> {
    if (! description.color || description.color->resource != resource_)
        return shared_textures_ ? (*shared_textures_)->Prepare(description, context)
                                : textures_.Prepare(description, context);
    if (! lease_.valid() || ! lease_.image.view || ! owner_ || description.depth ||
        description.resolve || description.color->load == AttachmentLoad::Load)
        return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
    GraphicsScopeInfo info;
    info.extent                         = { lease_.image.extent.width, lease_.image.extent.height };
    info.signature.color_format         = lease_.format;
    info.signature.color_load_op        = attachment_load(description.color->load);
    info.signature.color_store_op       = attachment_store(description.color->store);
    info.signature.color_initial_layout = info.signature.color_final_layout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    info.area = description.full_target
                    ? VkRect2D { { 0, 0 }, info.extent }
                    : VkRect2D { { rstd::int32_t(description.x), rstd::int32_t(description.y) },
                                 { description.width, description.height } };
    if (description.x > info.extent.width || description.y > info.extent.height ||
        info.area.extent.width > info.extent.width - description.x ||
        info.area.extent.height > info.extent.height - description.y)
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    info.clear_count = 1;
    for (unsigned i {}; i < 4; ++i)
        info.clear[usize()].color.float32[i] = description.color->clear[usize(i)];
    auto pass = context.session.graphics_resources().passes.Ensure(context.session.device(),
                                                                   info.signature);
    if (! pass) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    auto                    framebuffer = Arc<vvk::Framebuffer>::make();
    VkFramebufferCreateInfo create { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    create.renderPass      = **pass->render_pass;
    create.attachmentCount = 1;
    create.pAttachments    = &lease_.image.view;
    create.width           = info.extent.width;
    create.height          = info.extent.height;
    create.layers          = 1;
    auto result = context.session.device().handle().CreateFramebuffer(create, *framebuffer);
    if (result != VK_SUCCESS)
        return Err(GraphExecutionError { GraphExecutionStage::Prepare, {}, {}, result });
    info.render_pass = create.renderPass;
    info.framebuffer = **framebuffer;
    return Ok(
        Arc<dyn<PreparedGraphicsScope>>::make(SurfaceScopeResources { context.session,
                                                                      info,
                                                                      pass->render_pass.clone(),
                                                                      rstd::move(framebuffer),
                                                                      owner_.clone() }));
}
} // namespace vrento::vulkan

export namespace vrento::vulkan
{
struct SurfaceTransferPass {
    FrameSurfaceLease       lease;
    texture::TextureViewRef source;
    bool                    copy {};
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        auto native = native_recording(context.submission, context.session);
        if (native.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(native.unwrap_err()) });
        auto        image    = native_texture(source.texture()).unwrap();
        auto        extent   = texture::mip_extent(source.texture().description(),
                                                   source.description().range.first_mip);
        const auto& range    = source.description().range;
        const auto& dispatch = context.session.device().handle().Dispatch();
        if (copy) {
            VkImageCopy region {};
            region.srcSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, range.first_mip, range.first_layer, 1
            };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent         = { extent.width, extent.height, 1 };
            dispatch.vkCmdCopyImage(native.unwrap(),
                                    image,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    lease.image.handle,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);
        } else {
            VkImageBlit region {};
            region.srcSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, range.first_mip, range.first_layer, 1
            };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[1] = { rstd::int32_t(extent.width), rstd::int32_t(extent.height), 1 };
            region.dstOffsets[1] = { rstd::int32_t(lease.image.extent.width),
                                     rstd::int32_t(lease.image.extent.height),
                                     1 };
            dispatch.vkCmdBlitImage(native.unwrap(),
                                    image,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    lease.image.handle,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region,
                                    VK_FILTER_LINEAR);
        }
        return Ok(empty {});
    }
};
struct SurfaceTransferBackend {
    FrameSurfaceLease lease;
    auto              Prepare(const PassExecutionInfo&, const GraphPrepareContext&)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
inline constexpr GraphSerial surface_transfer_kind = 0x7672742e73747266ull;
} // namespace vrento::vulkan
namespace rstd
{

template<>
struct Impl<vrento::vulkan::VulkanPassBackend, vrento::vulkan::SurfaceTransferBackend>
    : ImplBase<vrento::vulkan::SurfaceTransferBackend> {
    auto Prepare(const vrento::vulkan::PassExecutionInfo&   pass,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<sync::Arc<dyn<vrento::vulkan::VulkanPass>>, vrento::vulkan::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};
} // namespace rstd
namespace vrento::vulkan
{
auto SurfaceTransferBackend::Prepare(const PassExecutionInfo&   pass,
                                     const GraphPrepareContext& context)
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    if (! lease.valid() || pass.uses.len() != usize(2) ||
        pass.uses[usize()].access != GraphAccess::Read ||
        pass.uses[usize(1)].access != GraphAccess::Write)
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    auto binding =
        context.bindings.resolve(pass.uses[usize()].resource, pass.uses[usize()].version);
    if (! binding) return Err(GraphExecutionError { GraphExecutionStage::Binding, pass.id });
    auto source = (*binding)->Texture();
    if (! source || source->description().range.aspect != texture::Aspect::Color ||
        source->texture().description().samples != 1)
        return Err(GraphExecutionError { GraphExecutionStage::Unsupported, pass.id });
    auto size =
        texture::mip_extent(source->texture().description(), source->description().range.first_mip);
    auto format = ToVkType(source->texture().description().format);
    bool copy   = size.width == lease.image.extent.width &&
                  size.height == lease.image.extent.height && format == lease.format;
    if (! copy) {
        VkFormatProperties from {}, to {};
        const auto&        dispatch = context.session.device().instance_dispatch();
        dispatch.vkGetPhysicalDeviceFormatProperties(
            *context.session.device().gpu(), format, &from);
        dispatch.vkGetPhysicalDeviceFormatProperties(
            *context.session.device().gpu(), lease.format, &to);
        if (! (from.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            ! (from.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
            ! (to.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, pass.id });
    }
    return Ok(Arc<dyn<VulkanPass>>::make(
        SurfaceTransferPass { lease, rstd::move(source).unwrap(), copy }));
}
} // namespace vrento::vulkan
