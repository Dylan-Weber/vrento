export module vrento:vulkan.texture_output;
export import :vulkan.passes.graphics;
export import :vulkan.passes.surface;
export import :vulkan.frame;
import :vulkan.shader_compile;
import rstd;

namespace vrento::vulkan
{
constexpr auto fullscreen_vertex = R"(#version 320 es
layout(location = 0) out vec2 v_Texcoord;

const vec2 k_Position[4] = vec2[4](
    vec2(-1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0));
const vec2 k_Texcoord[4] = vec2[4](
    vec2(0.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0));

void main()
{
    v_Texcoord = k_Texcoord[gl_VertexIndex];
    gl_Position = vec4(k_Position[gl_VertexIndex], 0.0, 1.0);
}
)"_str;

}
export namespace vrento::vulkan
{
inline auto texture_output_layout(resource::PipelineUseHandle pipeline)
    -> PipelineLayoutRequirement {
    PipelineLayoutRequirement    result { .pipeline = pipeline };
    PipelineLayoutSetRequirement set { .set = u32(1) };
    set.bindings.push({ .binding          = u32(1),
                        .descriptor_type  = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                        .descriptor_count = u32(1),
                        .stage_flags      = u32(VK_SHADER_STAGE_FRAGMENT_BIT) });
    result.descriptor_sets.push(rstd::move(set));
    return result;
}
inline auto
texture_output_recipe(ref<dyn<ShaderBackend>> backend, resource::PipelineUseHandle pipeline,
                      resource::RenderPassUseHandle  render_pass,
                      resource::PipelineLayoutHandle layout, VkFormat format, bool opaque)
    -> Result<GraphicsDrawDesc, resource::ResourceError> {
    auto fragment = String::make(R"(#version 320 es
layout(location = 0) in vec2 v_Texcoord;
layout(location = 0) out vec4 out_FragColor;
layout(set = 1, binding = 1) uniform sampler2D u_Texture;
void main() {
    out_FragColor = texture(u_Texture, v_Texcoord);
)"_str);
    if (opaque) fragment.push_str("    out_FragColor.a = 1.0;\n"_str);
    fragment.push_str("}\n"_str);
    array<ShaderCompUnit, 2> units {
        ShaderCompUnit { ShaderType::VERTEX, String::make(fullscreen_vertex) },
        ShaderCompUnit { ShaderType::FRAGMENT, rstd::move(fragment) }
    };
    Vec<Uni_ShaderSpv> stages;
    auto               compiled = backend->CompileAndLinkShaderUnits(
        units.as_slice(),
        ShaderCompOpt { .target = VulkanTarget::Vulkan_1_1, .optimize = true },
        stages);
    if (compiled.is_err()) return Err(rstd::move(compiled).unwrap_err());
    GraphicsPipeline defaults;
    defaults.toDefault();
    PipelineResourceRequest request {
        .pipeline_layout = layout,
        .shader_stages   = rstd::move(stages),
        .color_blend  = { .blendEnable         = VK_FALSE,
                          .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                          .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                          .colorBlendOp        = VK_BLEND_OP_ADD,
                          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                          .alphaBlendOp        = VK_BLEND_OP_ADD,
                          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT },
        .depth        = defaults.depth,
        .raster       = defaults.raster,
        .multisample  = defaults.multisample,
        .topology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .color_format = format,
        .color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .color_final_layout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .color_load_op        = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .has_color_attachment = true,
        .has_depth_attachment = false
    };
    return Ok(GraphicsDrawDesc { {}, pipeline, render_pass, rstd::move(request) });
}
inline auto texture_output_frame(const DeviceSession&                                session,
                                 const resource_registry::PreparedDescriptorBinding& descriptor,
                                 VkExtent2D                                          extent)
    -> Result<GraphicsFrameData, resource::ResourceError> {
    auto& owner = session.graphics_resources();
    auto  snapshot =
        owner.descriptor_snapshots.Snapshot(session.device(), descriptor, owner.descriptors);
    if (snapshot.is_err()) return Err(snapshot.unwrap_err());
    GraphicsFrameData result;
    result.descriptors.push(snapshot.unwrap());
    result.viewports.push(
        { 0, float(extent.height), float(extent.width), -float(extent.height), 0, 1 });
    result.scissors.push({ { 0, 0 }, extent });
    result.draws.push({ 4, 0 });
    return Ok(rstd::move(result));
}
class TextureOutput {
    struct Prepared {
        Arc<resource_registry::RenderResourceSystem> resources;
        resource::DescriptorBindingHandle            descriptor;
        GraphicsDrawDesc                             graphics;
        ~Prepared() { resources->RemovePreparedDescriptor(descriptor); }
        Prepared(Arc<resource_registry::RenderResourceSystem> owner,
                 resource::DescriptorBindingHandle binding, GraphicsDrawDesc recipe)
            : resources(rstd::move(owner)), descriptor(binding), graphics(rstd::move(recipe)) {}
    };
    Option<resource::TextureUseHandle>                   source_;
    VkFormat                                             format_ { VK_FORMAT_UNDEFINED };
    Option<resource::PipelineUseHandle>                  pipeline_;
    Option<resource::RenderPassUseHandle>                render_pass_;
    Option<Arc<resource_registry::RenderResourceSystem>> resources_;
    Option<Arc<Prepared>>                                prepared_;

public:
    void configure(Option<resource::TextureUseHandle> source, VkFormat format) {
        clear();
        source_      = source;
        format_      = format;
        pipeline_    = None();
        render_pass_ = None();
    }
    void graphics_resources(resource::PipelineUseHandle   pipeline,
                            resource::RenderPassUseHandle pass) {
        clear();
        pipeline_    = Some(pipeline);
        render_pass_ = Some(pass);
    }
    auto source() const -> Option<resource::TextureUseHandle> { return source_; }
    auto pipeline() const -> Option<resource::PipelineUseHandle> { return pipeline_; }
    auto render_pass() const -> Option<resource::RenderPassUseHandle> { return render_pass_; }
    auto descriptor() const -> Option<resource::DescriptorBindingHandle> {
        return prepared_ ? Some((*prepared_)->descriptor)
                         : None<resource::DescriptorBindingHandle>();
    }
    auto graphics() const -> bool { return format_ != VK_FORMAT_UNDEFINED; }
    auto layout_requirement() const -> Option<PipelineLayoutRequirement> {
        return pipeline_ ? Some(texture_output_layout(*pipeline_))
                         : None<PipelineLayoutRequirement>();
    }
    void clear() {
        prepared_  = None();
        resources_ = None();
    }
    auto prepare(Arc<resource_registry::RenderResourceSystem> resources,
                 ref<dyn<ShaderBackend>> shaders, const PipelineLayoutAssignments& layouts)
        -> Result<empty, resource::ResourceError> {
        auto missing = [] {
            return Err(
                resource::ResourceError { .kind    = resource::ResourceErrorKind::MissingDefinition,
                                          .message = "texture output resources unavailable"_Str });
        };
        if (! source_ || ! resources->Prepared().Resolve(*source_)) return missing();
        if (! graphics()) {
            resources_ = Some(rstd::move(resources));
            return Ok(empty {});
        }
        if (! pipeline_ || ! render_pass_) return missing();
        auto layout = layouts.Resolve(*pipeline_);
        if (! layout) return missing();
        auto recipe =
            prepared_ && (*prepared_)->graphics.request.pipeline_layout == *layout
                ? Result<GraphicsDrawDesc, resource::ResourceError>(
                      Ok((*prepared_)->graphics.clone()))
                : texture_output_recipe(shaders, *pipeline_, *render_pass_, *layout, format_, true);
        if (recipe.is_err()) return Err(recipe.unwrap_err());
        auto source = resources->Prepared().Resolve(*source_).unwrap();
        Vec<resource_registry::DescriptorImageBinding> images;
        images.push({ .binding = 1,
                      .image   = source->image.getActive(),
                      .layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        auto descriptor =
            resources->PrepareDescriptor(resources->TextureContext().session().device(),
                                         *layout,
                                         u32(1),
                                         images.as_slice(),
                                         {},
                                         resource_registry::DescriptorBindingReuse::Exclusive);
        if (descriptor.is_err()) return Err(descriptor.unwrap_err());
        resources_ = Some(resources.clone());
        prepared_ =
            Some(Arc<Prepared>::make(rstd::move(resources), descriptor.unwrap(), recipe.unwrap()));
        return Ok(empty {});
    }
    auto description(const PassInput& input, GraphSerial revision) const
        -> Result<PassInput, GraphExecutionError> {
        if (! graphics()) return Ok(input.clone());
        if (! prepared_) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
        auto publish = [&]<typename T>(PassId id,
                                       T      desc) -> Result<PassInput, GraphExecutionError> {
            auto result = PassInput::publish(input.scope(), revision, id, rstd::move(desc));
            if (result.is_err())
                return Err(GraphExecutionError { GraphExecutionStage::Schema,
                                                 id,
                                                 {},
                                                 VK_SUCCESS,
                                                 None(),
                                                 Some(result.unwrap_err()) });
            return Ok(result.unwrap());
        };
        auto id = input.execution().id;
        if (auto release = input.get<SurfaceReleaseDesc>()) {
            auto desc   = **release;
            desc.stages = PassStages::Color;
            return publish(id, rstd::move(desc));
        }
        auto transfer = input.get<SurfaceTransferDesc>();
        if (! transfer) return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
        auto description = (*prepared_)->graphics.clone();
        description.resources.textures.push({ (*transfer)->source.clone() });
        description.resources.attachments.color = Some(GraphicsAttachment {
            .target = (*transfer)->target.clone(), .load = AttachmentLoad::Discard });
        return publish(id, rstd::move(description));
    }
    auto capture(const CompiledGraph& compiled, ResourceId logical_surface, PassId output,
                 PassId release, SurfaceTarget target, Arc<GraphicsScopeCache> scopes) const
        -> Result<GraphFrameRequest, GraphExecutionError> {
        auto        scope   = compiled.description().scope();
        const auto& lease   = target.lease();
        auto        invalid = [] {
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        };
        if (! source_ ||
            (graphics() && (! prepared_ || lease.format != format_ || ! lease.image.view)))
            return invalid();
        if (! resources_ ||
            target.session().epoch() != (*resources_)->TextureContext().session().epoch())
            return invalid();
        auto& resources = **resources_;
        scopes->collect();
        GraphFrameRequest request {
            .bindings = GraphBindings(scope),
            .scopes   = Box<dyn<GraphicsScopeBackend>>::make(
                SurfaceScopeBackend(logical_surface, lease, target.owner(), rstd::move(scopes)))
        };
        auto source = resources.Prepared().Resolve(*source_);
        if (! source) return invalid();
        auto selected = (*source)->ActiveTexture();
        if (selected.is_err()) return invalid();
        auto surface_binding = Arc<dyn<GraphBinding>>::make(
            SurfaceGraphBinding(target.session(), lease, target.owner()));
        auto source_binding = Arc<dyn<GraphBinding>>::make(GraphTextureBinding(selected->view));
        const PassId output_passes[] { output, release };
        for (auto id : output_passes) {
            auto found = compiled.description(id);
            if (! found) return invalid();
            const auto& desc = **found;
            for (const auto& use : desc.execution().uses) {
                if (request.bindings.resolve(use.resource, use.version)) continue;
                auto bound = request.bindings.bind_version({ use.resource, use.version },
                                                           use.resource == logical_surface
                                                               ? surface_binding.clone()
                                                               : source_binding.clone());
                if (bound.is_err()) return Err(bound.unwrap_err());
            }
            if (desc.execution().id == output && graphics()) {
                auto sampled = sampled_image(selected->view, selected->sampler);
                if (sampled.is_err()) return invalid();
                Vec<resource_registry::DescriptorImageBinding> images;
                images.push({ .binding = 1,
                              .image   = sampled->image,
                              .layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                auto updated =
                    resources.UpdateDescriptorImages((*prepared_)->descriptor, images.as_slice());
                if (updated.is_err()) return invalid();
                auto descriptor = resources.Prepared().Resolve((*prepared_)->descriptor);
                if (! descriptor) return invalid();
                auto frame =
                    texture_output_frame(target.session(),
                                         **descriptor,
                                         { lease.image.extent.width, lease.image.extent.height });
                if (frame.is_err()) return invalid();
                frame->textures.push(texture::TextureViewRef(selected->view));
                frame->samplers.push(texture::SamplerRef(selected->sampler));
                auto bound = compiled.frame<GraphicsDrawPass>(
                    desc.execution().id, desc.revision(), frame.unwrap());
                if (bound.is_err()) return Err(bound.unwrap_err());
                request.parameters.push(bound.unwrap());
            } else {
                auto bound = compiled.frame<SurfacePass>(
                    desc.execution().id, desc.revision(), SurfaceFrameData { lease });
                if (bound.is_err()) return Err(bound.unwrap_err());
                request.parameters.push(bound.unwrap());
            }
        }
        return Ok(rstd::move(request));
    }
};
} // namespace vrento::vulkan
