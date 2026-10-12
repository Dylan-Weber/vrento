export module vrento:resource_registry.state;
import rstd;
import :resource_registry.prepared;
import :resource_registry.barrier;
import :texture.core;

export namespace vrento::resource_registry
{
using namespace rstd::prelude;

inline auto TextureAccessState(TextureStateKind target) -> texture::AccessState {
    switch (target) {
    case TextureStateKind::Sampled:
    case TextureStateKind::DepthSampled:
        return { texture::Access::Sampled, texture::Stages::Fragment };
    case TextureStateKind::TransferSource:
        return { texture::Access::TransferSource, texture::Stages::Transfer };
    case TextureStateKind::TransferDestination:
        return { texture::Access::TransferDestination, texture::Stages::Transfer };
    case TextureStateKind::ColorAttachment:
        return { texture::Access::ColorAttachment, texture::Stages::Color };
    case TextureStateKind::DepthAttachment:
        return { texture::Access::DepthAttachment, texture::Stages::Depth };
    default: return {};
    }
}

inline bool UsePreparedTexture(texture::Submission& work, const PreparedTexture& prepared,
                               TextureStateKind target, TextureSubresourceRange range = {},
                               bool discard = false, usize slot = usize(),
                               texture::Stages stages = texture::Stages::None) {
    const auto slots = prepared.physical->Slots();
    if (! work.valid() || slot >= slots.len() || target == TextureStateKind::Undefined)
        return false;
    const auto& selected = slots[slot];
    auto        state    = TextureAccessState(target);
    if (stages != texture::Stages::None) state.stages = stages;
    const bool attachment =
        target == TextureStateKind::ColorAttachment || target == TextureStateKind::DepthAttachment;
    if (attachment && ! selected.attachment_view.valid()) return false;
    const auto& view       = attachment ? selected.attachment_view : selected.view;
    const auto  view_range = view.description().range;
    const auto  aspect = view_range.aspect == texture::Aspect::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                     : VK_IMAGE_ASPECT_COLOR_BIT;
    if (range.aspect != VK_IMAGE_ASPECT_COLOR_BIT && range.aspect != aspect) return false;
    texture::Range use { view_range.aspect,
                         range.base_mip_level.to_primitive(),
                         range.level_count.to_primitive(),
                         range.base_array_layer.to_primitive(),
                         range.layer_count.to_primitive() };
    if (use.first_mip < view_range.first_mip ||
        use.first_mip >= view_range.first_mip + view_range.mip_count ||
        use.first_layer < view_range.first_layer ||
        use.first_layer >= view_range.first_layer + view_range.layer_count)
        return false;
    if (use.mip_count == VK_REMAINING_MIP_LEVELS)
        use.mip_count = view_range.first_mip + view_range.mip_count - use.first_mip;
    if (use.layer_count == VK_REMAINING_ARRAY_LAYERS)
        use.layer_count = view_range.first_layer + view_range.layer_count - use.first_layer;
    auto commands   = work.commands();
    auto completion = prepared.physical->Completion(slot);
    if (completion.valid() && commands->Wait(completion, state.stages).is_err()) return false;
    if (target == TextureStateKind::Sampled || target == TextureStateKind::DepthSampled) {
        if (! discard && use == view_range)
            return commands->UseSampledView(selected.view, selected.sampler, state.stages).is_ok();
    }
    if (commands->UseTexture({ view, state.access, state.stages, discard, Some(use) }).is_err())
        return false;
    if (state.access == texture::Access::Sampled) work.retain(selected.sampler);
    return true;
}
} // namespace vrento::resource_registry
