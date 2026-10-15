export module vrento:vulkan.passes.graphics;
export import :vulkan.renderer;
export import :vulkan.pipeline_layout;
import :resource_registry.system;
import :pass.resources;
import rstd;

using namespace rstd::literals;

export namespace vrento::vulkan
{
inline constexpr GraphSerial graphics_draw_kind  = 0x7672742e64726177ULL;
inline constexpr GraphSerial graphics_clear_kind = 0x7672742e636c6561ULL;

struct GraphicsTextureRead {
    pass::ContentRef content;
    GraphRange       range;
    PassStages       stages { PassStages::Fragment };
    auto clone() const -> GraphicsTextureRead { return { content.clone(), range, stages }; }
};
struct GraphicsBufferRead {
    pass::ContentRef content;
    GraphRange       range;
    PassUsage        usage { PassUsage::Uniform };
    PassStages       stages { PassStages::Vertex | PassStages::Fragment };
    auto clone() const -> GraphicsBufferRead { return { content.clone(), range, usage, stages }; }
};
struct GraphicsAttachment {
    pass::ContentRef         target;
    Option<pass::ContentRef> previous;
    AttachmentLoad           load { AttachmentLoad::Clear };
    AttachmentStore          store { AttachmentStore::Store };
    array<float, 4>          clear {};
    GraphIndex               stencil {};
    auto                     clone() const -> GraphicsAttachment {
        return { target.clone(), previous.clone(), load, store, clear, stencil };
    }
    auto execution() const -> GraphAttachment {
        auto version = target.version();
        return { version.resource, version.version, load, store, clear, stencil };
    }
};
struct GraphicsAttachments {
    Option<GraphicsAttachment> color, depth, resolve;
    GraphIndex                 x {}, y {}, width {}, height {};
    bool                       full_target { true };
    auto                       clone() const -> GraphicsAttachments {
        return { color ? Some(color->clone()) : None(),
                 depth ? Some(depth->clone()) : None(),
                 resolve ? Some(resolve->clone()) : None(),
                 x,
                 y,
                 width,
                 height,
                 full_target };
    }
    auto execution() const -> GraphicsDescription {
        return { color ? Some(color->execution()) : None(),
                 depth ? Some(depth->execution()) : None(),
                 resolve ? Some(resolve->execution()) : None(),
                 x,
                 y,
                 width,
                 height,
                 full_target };
    }
};
struct GraphicsPassResources {
    Vec<GraphicsTextureRead> textures;
    Vec<GraphicsBufferRead>  buffers;
    GraphicsAttachments      attachments;
    auto                     clone() const -> GraphicsPassResources {
        return { textures.clone(), buffers.clone(), attachments.clone() };
    }
};
inline auto
graphics_buffer_reads(const fg::FrameGraphBuilder& graph, const pass::GraphResources& resources,
                      const resource::ResourcePlan& plan, slice<resource::BufferUseHandle> uses)
    -> Result<Vec<GraphicsBufferRead>, GraphError> {
    Vec<GraphicsBufferRead> reads;
    for (auto use : uses) {
        auto version = resources.version(use);
        if (! version) return Err(GraphError { GraphErrorKind::MissingResource });
        bool found {};
        for (const auto& entry : plan.buffers)
            if (entry.handle == use) {
                auto content =
                    pass::ContentRef::bind(graph, *version, None(), Some(entry.definition.clone()));
                if (content.is_err()) return Err(content.unwrap_err());
                const auto usage  = entry.request().definition.usage;
                auto       kind   = usage == resource::BufferUsage::Vertex    ? PassUsage::Vertex
                                    : usage == resource::BufferUsage::Index   ? PassUsage::Index
                                    : usage == resource::BufferUsage::Storage ? PassUsage::Storage
                                                                              : PassUsage::Uniform;
                auto       stages = PassStages::Vertex;
                if (kind == PassUsage::Uniform || kind == PassUsage::Storage)
                    stages = stages | PassStages::Fragment | PassStages::Geometry;
                reads.push({ content.unwrap(),
                             { .size = entry.request().definition.size.to_primitive() },
                             kind,
                             stages });
                found = true;
            }
        if (! found)
            return Err(GraphError { GraphErrorKind::MissingResource, {}, version->resource });
    }
    return Ok(rstd::move(reads));
}
inline auto graphics_io(const GraphicsPassResources& resources, GraphSerial kind) -> VulkanPassIO {
    VulkanPassIO io { .graphics = Some(resources.attachments.execution()), .kind = kind };
    auto         attachment = [&](const Option<GraphicsAttachment>& value, bool depth) {
        if (! value) return;
        auto       usage  = depth ? PassUsage::DepthAttachment : PassUsage::ColorAttachment;
        auto       stages = depth ? PassStages::Depth : PassStages::Color;
        GraphRange range { .aspect = depth ? 2u : 1u };
        if (value->load == AttachmentLoad::Load && value->previous) {
            io.inputs.push({ value->previous->clone(), usage, stages, range });
            io.storage.push({ value->target.clone(), value->previous->clone() });
        }
        io.outputs.push({ value->target.clone(), usage, stages, range });
    };
    attachment(resources.attachments.color, false);
    attachment(resources.attachments.resolve, false);
    attachment(resources.attachments.depth, true);
    for (const auto& input : resources.textures)
        io.inputs.push({ input.content.clone(), PassUsage::Sampled, input.stages, input.range });
    for (const auto& input : resources.buffers)
        io.inputs.push({ input.content.clone(), input.usage, input.stages, input.range });
    return io;
}
inline auto resolve_graphics_inputs(const fg::FrameGraphBuilder& graph, PassId id,
                                    GraphicsPassResources resources)
    -> Result<GraphicsPassResources, GraphError> {
    Option<GraphicsAttachment>* attachments[] { &resources.attachments.color,
                                                &resources.attachments.depth,
                                                &resources.attachments.resolve };
    for (auto attachment : attachments) {
        if (! *attachment) continue;
        auto& value   = **attachment;
        auto  version = value.target.resolve(graph.data().scope);
        if (version.is_err()) return Err(version.unwrap_err());
        auto node = graph.find_version(*version);
        if (! node || node->writer != Some(PassId(id)))
            return Err(GraphError { GraphErrorKind::MissingProducer, id, version->resource });
        if (value.load == AttachmentLoad::Load) {
            if (! node->previous)
                return Err(GraphError { GraphErrorKind::MissingProducer, id, version->resource });
            auto previous = pass::ContentRef::bind(
                graph, *node->previous, value.target.texture(), value.target.buffer());
            if (previous.is_err()) return Err(previous.unwrap_err());
            value.previous = Some(previous.unwrap());
        }
    }
    return Ok(rstd::move(resources));
}
inline auto validate_graphics(const GraphicsPassResources& resources) -> Result<empty, GraphError> {
    if (! resources.attachments.color && ! resources.attachments.depth)
        return Err(GraphError { GraphErrorKind::Conflict });
    const Option<GraphicsAttachment>* attachments[] { &resources.attachments.color,
                                                      &resources.attachments.depth,
                                                      &resources.attachments.resolve };
    for (auto attachment : attachments) {
        if (! *attachment) continue;
        const auto& value = **attachment;
        if (! value.target.valid()) return Err(GraphError { GraphErrorKind::MissingResource });
        if (value.load == AttachmentLoad::Load &&
            (! value.previous ||
             value.previous->version().resource != value.target.version().resource))
            return Err(GraphError { GraphErrorKind::MissingProducer });
    }
    return Ok(empty {});
}
struct GraphicsDrawDesc {
    using PassDescriptor = pass::PassDesc;
    GraphicsPassResources             resources;
    resource::PipelineUseHandle       pipeline;
    resource::RenderPassUseHandle     render_pass;
    PipelineResourceRequest           request;
    Option<PipelineLayoutRequirement> layout;
    auto vulkan_io() const -> VulkanPassIO { return graphics_io(resources, graphics_draw_kind); }
    auto validate() const -> Result<empty, GraphError> { return validate_graphics(resources); }
    auto clone() const -> GraphicsDrawDesc {
        return { resources.clone(),
                 pipeline,
                 render_pass,
                 request.clone(),
                 layout ? Some(layout->clone()) : None() };
    }
};
struct GraphicsClearDesc {
    using PassDescriptor = pass::PassDesc;
    GraphicsPassResources resources;
    auto vulkan_io() const -> VulkanPassIO { return graphics_io(resources, graphics_clear_kind); }
    auto validate() const -> Result<empty, GraphError> {
        auto valid = validate_graphics(resources);
        if (valid.is_err()) return valid;
        if (! resources.textures.is_empty() || ! resources.buffers.is_empty())
            return Err(GraphError { GraphErrorKind::Conflict });
        return Ok(empty {});
    }
};
inline auto declare_graphics_clear(fg::FrameGraphBuilder& graph, PassId id, pass::ContentRef target,
                                   array<float, 4> clear, GraphSerial revision = 1,
                                   String name = {}) -> Result<DeclaredContentPass, GraphError> {
    auto added = graph.add_pass(id, {}, rstd::move(name));
    if (added.is_err()) return Err(added.unwrap_err());
    auto output = pass::ContentRef::write(graph, id, target);
    if (output.is_err()) return Err(output.unwrap_err());
    GraphicsClearDesc desc;
    desc.resources.attachments.color = Some(GraphicsAttachment {
        .target = output->clone(), .load = AttachmentLoad::Clear, .clear = clear });
    auto published                   = declare_input(graph, id, rstd::move(desc), revision);
    if (published.is_err()) return Err(published.unwrap_err());
    return Ok(DeclaredContentPass { published.unwrap(), output.unwrap() });
}
inline auto declare_graphics_clear(fg::FrameGraphBuilder& graph, PassId id,
                                   fg::ResourceVersion target, array<float, 4> clear,
                                   GraphSerial revision = 1, String name = {})
    -> Result<PassInput, GraphError> {
    auto content = pass::ContentRef::bind(graph, target);
    if (content.is_err()) return Err(content.unwrap_err());
    auto result =
        declare_graphics_clear(graph, id, content.unwrap(), clear, revision, rstd::move(name));
    if (result.is_err()) return Err(result.unwrap_err());
    return Ok(rstd::move(result->description));
}
struct GraphicsDrawRange {
    rstd::uint32_t count {}, first {};
};
struct GraphicsFrameData {
    Vec<resource_registry::PreparedDescriptorBinding> descriptors;
    Vec<BufferAllocation>                             vertices, buffers;
    Option<BufferAllocation>                          indices;
    Vec<texture::TextureViewRef>                      textures;
    Vec<texture::SamplerRef>                          samplers;
    Vec<VkViewport>                                   viewports;
    Vec<VkRect2D>                                     scissors;
    Vec<GraphicsDrawRange>                            draws;
    rstd::uint32_t                                    instances { 1 };
    bool                                              visible { true };
};
struct PreparedGraphicsLayouts {
    PipelineLayoutPlan        plan;
    PipelineLayoutAssignments assignments;
};
inline auto prepare_graphics_layouts(resource_registry::RenderResourceSystem& resources,
                                     const Device& device, slice<PassInput> descriptions,
                                     Vec<PipelineLayoutRequirement> requirements = {},
                                     PipelineLayoutPolicy           policy       = {})
    -> Result<PreparedGraphicsLayouts, resource::ResourceError> {
    for (const auto& desc : descriptions) {
        auto recipe = desc.get<GraphicsDrawDesc>();
        if (recipe && (**recipe).layout) requirements.push((**recipe).layout->clone());
    }
    auto planned = PlanPipelineLayouts(requirements.as_slice(),
                                       policy,
                                       device.capabilities().push_descriptor,
                                       u32(device.capabilities().max_push_descriptors),
                                       u32(device.limits().maxPushConstantsSize),
                                       rstd::addressof(device.limits()));
    if (planned.is_err()) return Err(planned.unwrap_err());
    PreparedGraphicsLayouts result { planned.unwrap(), {} };
    for (const auto& family : result.plan.families) {
        auto layout = resources.PreparePipelineLayout(device, family.request);
        if (layout.is_err()) return Err(layout.unwrap_err());
        for (auto pipeline : family.pipelines)
            result.assignments.entries.push({ pipeline, *layout });
    }
    return Ok(rstd::move(result));
}

struct GraphicsDrawCommands {
    DeviceSession                                 session;
    Arc<resource_registry::PipelineResourceEntry> pipeline;
    Arc<GraphicsFrameData>                        frame;
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        if (! frame->visible) return Ok(empty {});
        auto recording = native_recording(context.submission, session);
        if (recording.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(recording.unwrap_err()) });
        vvk::CommandBuffer command(recording.unwrap(), session.device().handle().Dispatch());
        resource_registry::DescriptorBindingRecordState state;
        state.UsePipeline(pipeline->layout->handle,
                          pipeline->layout->descriptor_layouts.as_slice(),
                          pipeline->layout->push_constant_identity);
        for (const auto& descriptor : frame->descriptors)
            descriptor.Record(command, pipeline->pipeline.layout, state);
        command.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline->pipeline.handle);
        command.SetViewport(0, frame->viewports.as_slice());
        command.SetScissor(0, frame->scissors.as_slice());
        for (usize i {}; i < frame->vertices.len(); ++i) {
            auto buffer = frame->vertices[i].buffer();
            auto offset = frame->vertices[i].offset();
            command.BindVertexBuffers(i.to_primitive(), 1, &buffer, &offset);
        }
        if (frame->indices)
            command.BindIndexBuffer(
                frame->indices->buffer(), frame->indices->offset(), VK_INDEX_TYPE_UINT32);
        for (auto draw : frame->draws)
            if (frame->indices)
                command.DrawIndexed(draw.count, frame->instances, draw.first, 0, 0);
            else
                command.Draw(draw.count, frame->instances, draw.first, 0);
        return Ok(empty {});
    }
};
struct GraphicsDrawPass {
    using FrameData = GraphicsFrameData;
    DeviceSession                                 session;
    Arc<resource_registry::PipelineResourceEntry> pipeline;
    auto Instantiate(const GraphPrepareContext&, Arc<GraphicsFrameData>) const
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
struct GraphicsClearCommands {
    auto Record(GraphRecordContext&) const -> Result<empty, GraphExecutionError> {
        return Ok(empty {});
    }
};
struct GraphicsClearPass {
    auto Instantiate(const GraphPrepareContext&) const
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
struct GraphicsDrawFactory {
    using Pass = GraphicsDrawPass;
    auto Compile(const GraphicsDrawDesc&, const PassExecutionInfo&, const PassCompileContext&)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
struct GraphicsClearFactory {
    using Pass = GraphicsClearPass;
    auto Compile(const GraphicsClearDesc&, const PassExecutionInfo&, const PassCompileContext&)
        -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
} // namespace vrento::vulkan

export namespace rstd
{

} // namespace rstd

namespace vrento::vulkan
{
auto GraphicsDrawFactory::Compile(const GraphicsDrawDesc& desc, const PassExecutionInfo& pass,
                                  const PassCompileContext& context)
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    if (pass.kind != graphics_draw_kind || ! pass.graphics || ! pass.parameters.is_empty())
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    const auto& resources = context.graphics;
    if (! resources) return Err(GraphExecutionError { GraphExecutionStage::Prepare, pass.id });
    if ((*resources)->session().epoch() != context.session.epoch())
        return Err(GraphExecutionError { GraphExecutionStage::Device, pass.id });
    auto prepared =
        (*resources)->Prepare(desc.pipeline, desc.render_pass, desc.request, context.session);
    if (prepared.is_err()) {
        auto                source = rstd::move(prepared).unwrap_err();
        GraphExecutionError error { source.kind == resource::ResourceErrorKind::Pending
                                        ? GraphExecutionStage::Pending
                                        : GraphExecutionStage::Prepare,
                                    pass.id };
        error.preparation = resource::ResourceErrorSnapshot(rstd::move(source));
        return Err(rstd::move(error));
    }
    return Ok(Arc<dyn<VulkanPass>>::make(
        GraphicsDrawPass { context.session, prepared->physical.clone() }));
}
auto GraphicsClearFactory::Compile(const GraphicsClearDesc&, const PassExecutionInfo& pass,
                                   const PassCompileContext&)
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    if (pass.kind != graphics_clear_kind || ! pass.graphics || ! pass.parameters.is_empty())
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    return Ok(Arc<dyn<VulkanPass>>::make(GraphicsClearPass {}));
}
auto GraphicsDrawPass::Instantiate(const GraphPrepareContext& context,
                                   Arc<GraphicsFrameData>     frame) const
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    if (! context.scope || session.epoch() != context.session.epoch())
        return Err(GraphExecutionError { GraphExecutionStage::Schema });
    if (frame->viewports.is_empty() || frame->scissors.is_empty())
        return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    auto valid = [&](const BufferAllocation& allocation) {
        return allocation && allocation.device() == *session.device().handle();
    };
    for (const auto& allocation : frame->vertices)
        if (! valid(allocation)) return Err(GraphExecutionError { GraphExecutionStage::Binding });
    for (const auto& allocation : frame->buffers)
        if (! valid(allocation)) return Err(GraphExecutionError { GraphExecutionStage::Binding });
    if (frame->indices && ! valid(*frame->indices))
        return Err(GraphExecutionError { GraphExecutionStage::Binding });
    return Ok(Arc<dyn<VulkanPass>>::make(
        GraphicsDrawCommands { session, pipeline.clone(), rstd::move(frame) }));
}
auto GraphicsClearPass::Instantiate(const GraphPrepareContext& context) const
    -> Result<Arc<dyn<VulkanPass>>, GraphExecutionError> {
    if (! context.scope) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
    return Ok(Arc<dyn<VulkanPass>>::make(GraphicsClearCommands {}));
}
} // namespace vrento::vulkan

export namespace vrento::vulkan
{
inline auto register_graphics_factories(Renderer& renderer) -> Result<empty, GraphExecutionError> {
    auto draw = renderer.bind(
        Arc<dyn<PassFactory>>::make(TypedPassFactory<GraphicsDrawDesc, GraphicsDrawFactory> {}));
    if (draw.is_err()) return draw;
    return renderer.bind(
        Arc<dyn<PassFactory>>::make(TypedPassFactory<GraphicsClearDesc, GraphicsClearFactory> {}));
}
} // namespace vrento::vulkan
