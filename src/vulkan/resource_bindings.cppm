export module vrento:vulkan.resource_bindings;
import rstd;
import :pass.resources;
import :vulkan.pass.execution;
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
    frame.bindings.reserve(mapping.externals.len() + mapping.textures.len() + mapping.buffers.len());
    for (const auto& external : mapping.externals)
        if (! frame.bindings.resolve(external.version.resource, external.version.version))
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding, {}, external.version.resource });
    // Selections by use handle, in their original order: a scene selects once per draw texture,
    // and scanning them all for every bound texture made this loop quadratic.
    constexpr usize                            none { ~rstd::size_t(0) };
    FastMap<resource::TextureUseHandle, usize> first;
    Vec<usize>                                 chain;
    chain.resize(selections.len(), none);
    for (usize k {}; k < selections.len(); ++k) {
        usize i = selections.len() - usize(1) - k;
        if (auto previous = first.get_mut(selections[i].use)) {
            chain[i]    = **previous;
            **previous  = i;
        } else
            first.insert(selections[i].use, i);
    }
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
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding,
                {},
                version.resource,
                vrento::vulkan::GraphExecutionCause::Texture(selected.unwrap_err()) });
        // By reference: copying a view (an Arc and its description) once per selection and again
        // to override was a large share of this loop in a scene with hundreds of draws.
        const texture::TextureViewRef* view = &selected->view;
        bool                           overridden {};
        if (auto head = first.get(binding.resource.handle))
            for (usize i = **head; i != none; i = chain[i]) {
                const auto& selection = selections[i];
                // draws sampling one texture hold copies of one view: nothing to compare
                if (overridden && ! view->same_view(selection.view) &&
                    (view->texture().identity() != selection.view.texture().identity() ||
                     view->description() != selection.view.description()))
                    return Err(
                        GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
                view       = &selection.view;
                overridden = true;
            }
        if (! view->valid())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        const auto& range    = view->description().range;
        const auto& required = binding.range;
        if (required.aspect != (range.aspect == texture::Aspect::Depth ? 2u : 1u) ||
            required.first_mip > range.mip_count ||
            required.mip_count > range.mip_count - required.first_mip ||
            required.first_layer > range.layer_count ||
            required.layer_count > range.layer_count - required.first_layer)
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        if (auto existing = frame.bindings.resolve(version.resource, version.version)) {
            auto previous = (*existing)->Texture();
            if (! previous || previous->texture().identity() != view->texture().identity() ||
                previous->description() != view->description())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
            continue;
        }
        auto bound = frame.bindings.bind_version(
            version, Arc<dyn<GraphBinding>>::make(GraphTextureBinding(*view)));
        if (bound.is_err()) return Err(bound.unwrap_err());
    }
    // One entry per buffer version, found by hash: the scan of every earlier buffer for each new
    // one was quadratic in the draws.
    FastMap<fg::ResourceVersion, usize> buffer_index;
    for (usize i {}; i < frame.buffers.len(); ++i) {
        if (! buffer_index.contains_key(frame.buffers[i].version))
            buffer_index.insert(frame.buffers[i].version, i);
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
        if (auto found = buffer_index.get(version)) {
            if (frame.buffers[**found].allocation.identity() != allocation.identity())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, {}, version.resource });
        } else {
            buffer_index.insert(version, frame.buffers.len());
            frame.buffers.push({ version, allocation.clone() });
        }
    }
    return Ok(empty {});
}
} // namespace vrento::vulkan
