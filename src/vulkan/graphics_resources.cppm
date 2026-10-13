export module vrento:vulkan.graphics_resources;
export import :vulkan.passes.graphics;
import rstd;

export namespace vrento::vulkan
{
struct SharedDescriptorBufferUse {
    u32                       binding {};
    u64                       identity {};
    resource::BufferUseHandle buffer;
    VkDescriptorType          type { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
};
class PreparedGraphicsResources {
    Arc<resource_registry::RenderResourceSystem> resources_;
    PreparedGraphicsLayouts                      layouts_;
    Option<resource::DescriptorBindingHandle>    shared_descriptor_;
    Arc<GraphicsCompileResources>                compilation_;

public:
    PreparedGraphicsResources(Arc<resource_registry::RenderResourceSystem> resources,
                              PreparedGraphicsLayouts                      layouts,
                              Option<resource::DescriptorBindingHandle>    descriptor,
                              GraphicsCompileResources                     compilation)
        : resources_(rstd::move(resources)),
          layouts_(rstd::move(layouts)),
          shared_descriptor_(descriptor),
          compilation_(Arc<GraphicsCompileResources>::make(rstd::move(compilation))) {}
    PreparedGraphicsResources(const PreparedGraphicsResources&) = delete;
    ~PreparedGraphicsResources() {
        if (shared_descriptor_) resources_->RemovePreparedDescriptor(*shared_descriptor_);
    }
    static auto prepare(Arc<resource_registry::RenderResourceSystem> resources,
                        slice<PassInput> descriptions, slice<SharedDescriptorBufferUse> shared,
                        Vec<PipelineLayoutRequirement> additional = {},
                        PipelineLayoutPolicy           policy     = {})
        -> Result<Arc<PreparedGraphicsResources>, resource::ResourceError> {
        auto invalid = [] {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = "shared descriptor buffers do not match the layout"_Str });
        };
        if (! resources->Ready()) return invalid();
        auto session = resources->TextureContext().session();
        auto planned = prepare_graphics_layouts(
            *resources, session.device(), descriptions, rstd::move(additional), policy);
        if (planned.is_err()) return Err(planned.unwrap_err());
        auto                           layouts = planned.unwrap();
        Vec<SharedDescriptorBufferUse> unique;
        for (const auto& use : shared) {
            bool known {};
            for (const auto& prior : unique)
                if (prior.binding == use.binding) {
                    if (prior.identity != use.identity || prior.buffer != use.buffer ||
                        prior.type != use.type)
                        return invalid();
                    known = true;
                }
            if (! known) unique.push(SharedDescriptorBufferUse(use));
        }
        if (unique.len() != layouts.plan.global_bindings.len()) return invalid();
        for (const auto& binding : layouts.plan.global_bindings) {
            bool found {};
            for (const auto& use : unique)
                if (use.binding == binding.binding && use.identity == binding.identity)
                    found = true;
            if (! found) return invalid();
        }
        Option<resource::DescriptorBindingHandle> descriptor;
        if (! unique.is_empty()) {
            if (! policy.shared_set || layouts.assignments.entries.is_empty()) return invalid();
            Vec<resource_registry::DescriptorBufferBinding> buffers;
            for (const auto& use : unique) {
                auto prepared = resources->Prepared().Resolve(use.buffer);
                if (! prepared) return invalid();
                const auto& allocation = (**prepared).buffer.physical->buffer;
                buffers.push({ .binding = use.binding.to_primitive(),
                               .buffer  = allocation.buffer(),
                               .offset  = allocation.offset(),
                               .size    = allocation.size(),
                               .type    = use.type });
            }
            auto prepared =
                resources->PrepareDescriptor(session.device(),
                                             layouts.assignments.entries.first().unwrap()->layout,
                                             *policy.shared_set,
                                             {},
                                             buffers.as_slice(),
                                             resource_registry::DescriptorBindingReuse::Shared);
            if (prepared.is_err()) return Err(prepared.unwrap_err());
            descriptor = Some(prepared.unwrap());
        }
        PipelineLayoutAssignments assignments;
        assignments.entries = layouts.assignments.entries.clone();
        auto compilation =
            GraphicsCompileResources(session, resources.clone(), rstd::move(assignments));
        return Ok(Arc<PreparedGraphicsResources>::make(
            rstd::move(resources), rstd::move(layouts), descriptor, rstd::move(compilation)));
    }
    auto layouts() const -> const PipelineLayoutAssignments& { return layouts_.assignments; }
    auto conflicts() const -> slice<PipelineLayoutConflict> {
        return layouts_.plan.conflicts.as_slice();
    }
    auto shared_descriptor() const -> Option<resource::DescriptorBindingHandle> {
        return shared_descriptor_;
    }
    auto compilation() const -> Arc<GraphicsCompileResources> { return compilation_.clone(); }
    auto pipeline(resource::PipelineUseHandle use) const
        -> Option<ref<resource_registry::PipelinePreparation>> {
        return compilation_->prepared(use);
    }
};
} // namespace vrento::vulkan
