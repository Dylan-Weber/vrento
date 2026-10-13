export module vrento:vulkan.graphics_scope;
export import :vulkan.graph;
import :resource_registry.graphics;
import rstd;

export namespace vrento::vulkan
{
using namespace rstd::prelude;
struct GraphicsScopeResources {
    DeviceSession                     session;
    GraphicsScopeInfo                 info;
    Vec<texture::TextureViewRef>      views;
    rstd::sync::Arc<vvk::RenderPass>  pass;
    rstd::sync::Arc<vvk::Framebuffer> framebuffer;
    GraphicsScopeResources(DeviceSession device, GraphicsScopeInfo description,
                           Vec<texture::TextureViewRef>      attachments,
                           rstd::sync::Arc<vvk::RenderPass>  render_pass,
                           rstd::sync::Arc<vvk::Framebuffer> target)
        : session(rstd::move(device)),
          info(description),
          views(rstd::move(attachments)),
          pass(rstd::move(render_pass)),
          framebuffer(rstd::move(target)) {}
    auto Info() const -> GraphicsScopeInfo { return info; }
};
class GraphicsScopeCache {
    Option<DeviceSession>                  session_;
    Option<TextureContext>                 view_device_;
    resource_registry::RenderPassRegistry  passes_;
    resource_registry::FramebufferRegistry framebuffers_;

public:
    auto Prepare(const GraphicsDescription&, const GraphPrepareContext&)
        -> Result<rstd::sync::Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError>;
    void collect() {
        framebuffers_.PruneExpired();
        passes_.PruneExpired();
    }
};
} // namespace vrento::vulkan
export namespace rstd
{
template<>
struct Impl<vrento::vulkan::PreparedGraphicsScope, vrento::vulkan::GraphicsScopeResources>
    : ImplBase<vrento::vulkan::GraphicsScopeResources> {
    auto Info() const -> vrento::vulkan::GraphicsScopeInfo { return self().Info(); }
};
template<>
struct Impl<vrento::vulkan::GraphicsScopeBackend, vrento::vulkan::GraphicsScopeCache>
    : ImplBase<vrento::vulkan::GraphicsScopeCache> {
    auto Prepare(const vrento::vulkan::GraphicsDescription& description,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<rstd::sync::Arc<dyn<vrento::vulkan::PreparedGraphicsScope>>,
                  vrento::vulkan::GraphExecutionError> {
        return self().Prepare(description, context);
    }
};
} // namespace rstd
namespace vrento::vulkan
{
inline auto attachment_load(AttachmentLoad load) -> VkAttachmentLoadOp {
    return load == AttachmentLoad::Load    ? VK_ATTACHMENT_LOAD_OP_LOAD
           : load == AttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                           : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}
inline auto attachment_store(AttachmentStore store) -> VkAttachmentStoreOp {
    return store == AttachmentStore::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                           : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
auto GraphicsScopeCache::Prepare(const GraphicsDescription& description,
                                 const GraphPrepareContext& context)
    -> Result<rstd::sync::Arc<dyn<PreparedGraphicsScope>>, GraphExecutionError> {
    if (session_.is_none())
        session_ = Some(DeviceSession(context.session));
    else if (session_->epoch() != context.session.epoch())
        return Err(GraphExecutionError { GraphExecutionStage::Device });
    framebuffers_.PruneExpired();
    GraphicsScopeInfo info;
    info.signature.has_color_attachment   = description.color.is_some();
    info.signature.has_depth_attachment   = description.depth.is_some();
    info.signature.has_resolve_attachment = description.resolve.is_some();
    Vec<texture::TextureViewRef> views;
    FramebufferResourceRequest   framebuffer;
    Option<texture::Count>       samples;
    auto attachment = [&](const GraphAttachment& declared,
                          bool                   depth,
                          bool                   resolve) -> Result<empty, GraphExecutionError> {
        auto binding = context.bindings.resolve(declared.resource, declared.version);
        if (binding.is_none())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, declared.resource });
        auto view = (*binding)->Texture();
        if (view.is_none())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, declared.resource });
        if (! view->permits(true) ||
            (depth != (view->description().range.aspect == texture::Aspect::Depth)))
            return Err(
                GraphExecutionError { GraphExecutionStage::Unsupported, {}, declared.resource });
        if (view->description().range.mip_count != 1 ||
            view->description().range.layer_count != 1) {
            if (! view_device_) {
                auto created = TextureContext::create(context.session);
                if (created.is_err())
                    return Err(GraphExecutionError { GraphExecutionStage::Prepare });
                view_device_ = Some(rstd::move(created).unwrap());
            }
            auto selected            = view->description();
            selected.dimension       = texture::Dimension::D2;
            selected.range.mip_count = selected.range.layer_count = 1;
            auto sliced = view_device_->CreateView(view->texture(), selected);
            if (sliced.is_err()) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
            view = Some(rstd::move(sliced).unwrap());
        }
        const auto& range  = view->description().range;
        const auto& desc   = view->texture().description();
        auto        extent = texture::mip_extent(desc, range.first_mip);
        if (! info.extent.width) info.extent = { extent.width, extent.height };
        if (info.extent.width != extent.width || info.extent.height != extent.height ||
            (! resolve && samples.is_some() && *samples != desc.samples) ||
            (resolve && (desc.samples != 1 || samples.is_none() || *samples == 1 ||
                         ToVkType(desc.format) != info.signature.color_format)))
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, declared.resource });
        if (! resolve) samples = Some(texture::Count(desc.samples));
        if (depth) {
            info.signature.depth_format         = ToVkType(desc.format);
            info.signature.depth_load_op        = attachment_load(declared.load);
            info.signature.depth_store_op       = attachment_store(declared.store);
            info.signature.depth_initial_layout = info.signature.depth_final_layout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else if (! resolve) {
            info.signature.color_format         = ToVkType(desc.format);
            info.signature.color_load_op        = attachment_load(declared.load);
            info.signature.color_store_op       = attachment_store(declared.store);
            info.signature.color_initial_layout = info.signature.color_final_layout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            info.signature.resolve_initial_layout = info.signature.resolve_final_layout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.signature.resolve_store_op = attachment_store(declared.store);
        }
        VkClearValue clear {};
        if (depth)
            clear.depthStencil = { declared.clear[usize()], declared.stencil };
        else
            for (unsigned i {}; i < 4; ++i) clear.color.float32[i] = declared.clear[usize(i)];
        info.clear[views.len()]    = clear;
        info.clear_count           = unsigned(views.len().to_primitive()) + 1;
        auto              identity = view->texture().identity();
        PipelineKeyWriter writer;
        writer.writeU64(identity.device);
        writer.writeU64(identity.epoch);
        writer.writeU64(identity.resource);
        writer.writeU32(range.first_mip);
        writer.writeU32(range.first_layer);
        writer.writeU32(unsigned(range.aspect));
        writer.writeU64(reinterpret_cast<rstd::uintptr_t>(native_view(*view).unwrap()));
        auto key = rstd::move(writer).finish();
        framebuffer.attachments.push(
            { native_view(*view).unwrap(), { key.value, rstd::move(key.bytes) } });
        views.push(rstd::move(view).unwrap());
        return Ok(empty {});
    };
    if (description.color) {
        auto added = attachment(*description.color, false, false);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    if (description.resolve) {
        auto added = attachment(*description.resolve, false, true);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    if (description.depth) {
        auto added = attachment(*description.depth, true, false);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    info.signature.samples = VkSampleCountFlagBits(samples.unwrap());
    info.area = description.full_target
                    ? VkRect2D { { 0, 0 }, info.extent }
                    : VkRect2D { { rstd::int32_t(description.x), rstd::int32_t(description.y) },
                                 { description.width, description.height } };
    if (description.x > info.extent.width || description.y > info.extent.height ||
        info.area.extent.width > info.extent.width - description.x ||
        info.area.extent.height > info.extent.height - description.y)
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    auto render_pass = passes_.Ensure(context.session.device(), info.signature);
    if (render_pass.is_none()) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    framebuffer.render_pass     = **render_pass->render_pass;
    framebuffer.render_pass_key = render_pass->cache_key.clone();
    framebuffer.extent          = info.extent;
    auto target                 = framebuffers_.Ensure(context.session.device(), framebuffer);
    if (target.is_none()) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    info.render_pass = framebuffer.render_pass;
    info.framebuffer = **target->framebuffer;
    return Ok(rstd::sync::Arc<dyn<PreparedGraphicsScope>>::make(
        GraphicsScopeResources(context.session,
                               info,
                               rstd::move(views),
                               render_pass->render_pass.clone(),
                               target->framebuffer.clone())));
}
} // namespace vrento::vulkan
