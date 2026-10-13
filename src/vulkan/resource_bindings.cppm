export module vrento:vulkan.resource_bindings;
import rstd;
import :pass.resources;
import :vulkan.frame;
import :resource_registry.prepared;

export namespace vrento::vulkan
{
struct TextureSelection {
    resource::TextureUseHandle use;
    texture::TextureViewRef    view;
};

inline auto capture_resources(const pass::GraphResourceMap&                   mapping,
                              const resource_registry::PreparedResourceTable& prepared,
                              slice<TextureSelection> selections, GraphFrameRequest& frame)
    -> Result<empty, GraphExecutionError> {
    if (mapping.scope != frame.bindings.scope() || prepared.Generation() != u64(mapping.scope))
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    for (const auto& external : mapping.externals)
        if (! frame.bindings.resolve(external.version.resource, external.version.version))
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding, {}, external.version.resource });
    for (const auto& binding : mapping.textures) {
        auto version = binding.resource.version;
        auto value   = prepared.Resolve(binding.resource.handle);
        if (! value)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        auto definition = binding.content.texture();
        if (! definition || (**value).definition.id() != definition->id())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        auto selected = (**value).ActiveTexture();
        if (selected.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Binding,
                                             {},
                                             version.resource,
                                             VK_SUCCESS,
                                             Some(selected.unwrap_err()) });
        auto view = selected->view;
        bool overridden {};
        for (const auto& selection : selections)
            if (selection.use == binding.resource.handle) {
                if (overridden &&
                    (view.texture().identity() != selection.view.texture().identity() ||
                     view.description() != selection.view.description()))
                    return Err(
                        GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
                view       = selection.view;
                overridden = true;
            }
        if (! view.valid())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        const auto& range    = view.description().range;
        const auto& required = binding.range;
        if (required.aspect != (range.aspect == texture::Aspect::Depth ? 2u : 1u) ||
            required.first_mip > range.mip_count ||
            required.mip_count > range.mip_count - required.first_mip ||
            required.first_layer > range.layer_count ||
            required.layer_count > range.layer_count - required.first_layer)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        if (auto existing = frame.bindings.resolve(version.resource, version.version)) {
            auto previous = (*existing)->Texture();
            if (! previous || previous->texture().identity() != view.texture().identity() ||
                previous->description() != view.description())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
            continue;
        }
        auto bound = frame.bindings.bind_version(
            version, Arc<dyn<GraphBinding>>::make(GraphTextureBinding(view)));
        if (bound.is_err()) return Err(bound.unwrap_err());
    }
    for (const auto& binding : mapping.buffers) {
        auto version = binding.resource.version;
        auto value   = prepared.Resolve(binding.resource.use);
        if (! value)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        auto definition = binding.content.buffer();
        if (! definition || (**value).buffer.definition.id() != definition->id())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        const auto& allocation = (**value).buffer.physical->buffer;
        if (binding.range.offset > allocation.size() ||
            binding.range.size > allocation.size() - binding.range.offset)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        bool exists {};
        for (const auto& buffer : frame.buffers)
            if (buffer.version == version) {
                if (buffer.allocation.identity() != allocation.identity())
                    return Err(
                        GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
                exists = true;
            }
        if (! exists) frame.buffers.push({ version, allocation.clone() });
    }
    return Ok(empty {});
}
} // namespace vrento::vulkan
