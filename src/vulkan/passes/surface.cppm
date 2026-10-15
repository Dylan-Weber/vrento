export module vrento:vulkan.passes.surface;
export import :vulkan.graph_surface;
export import :vulkan.renderer;
import rstd;

export namespace vrento::vulkan
{
struct SurfaceTransferDesc {
    using PassDescriptor = pass::PassDesc;
    pass::ContentRef source, target;
    auto             vulkan_io() const -> VulkanPassIO {
        VulkanPassIO result { .kind = surface_transfer_kind };
        result.inputs.push({ source.clone(), PassUsage::Transfer, PassStages::Transfer });
        result.outputs.push({ target.clone(), PassUsage::Transfer, PassStages::Transfer });
        return result;
    }
};
struct SurfaceReleaseDesc {
    using PassDescriptor = pass::PassDesc;
    pass::ContentRef source;
    PassStages       stages { PassStages::Transfer };
    auto             vulkan_io() const -> VulkanPassIO {
        VulkanPassIO result { .kind = surface_release_kind };
        result.inputs.push({ source.clone(), PassUsage::Release, stages });
        return result;
    }
};
struct SurfaceFrameData {
    FrameSurfaceLease lease;
};
struct DeclaredSurfaceOutput {
    PassInput           transfer, release;
    fg::ResourceVersion output;
};
inline auto declare_surface_output(fg::FrameGraphBuilder& graph, PassId transfer_id,
                                   PassId release_id, pass::ContentRef source,
                                   pass::ContentRef target, GraphSerial revision = 1)
    -> Result<DeclaredSurfaceOutput, GraphError> {
    auto added = graph.add_pass(transfer_id);
    if (added.is_err()) return Err(added.unwrap_err());
    auto output = pass::ContentRef::write(graph, transfer_id, target);
    if (output.is_err()) return Err(output.unwrap_err());
    auto transfer = declare_input(
        graph, transfer_id, SurfaceTransferDesc { rstd::move(source), output->clone() }, revision);
    if (transfer.is_err()) return Err(transfer.unwrap_err());
    auto release =
        declare_input(graph, release_id, SurfaceReleaseDesc { output->clone() }, revision);
    if (release.is_err()) return Err(release.unwrap_err());
    return Ok(DeclaredSurfaceOutput { transfer.unwrap(), release.unwrap(), output->version() });
}
inline auto declare_surface_output(fg::FrameGraphBuilder& graph, PassId transfer_id,
                                   PassId release_id, fg::ResourceVersion source,
                                   fg::ResourceVersion target, GraphSerial revision = 1)
    -> Result<DeclaredSurfaceOutput, GraphError> {
    auto input = pass::ContentRef::bind(graph, source);
    if (input.is_err()) return Err(input.unwrap_err());
    auto destination = pass::ContentRef::bind(graph, target);
    if (destination.is_err()) return Err(destination.unwrap_err());
    return declare_surface_output(
        graph, transfer_id, release_id, input.unwrap(), destination.unwrap(), revision);
}
struct SurfacePass {
    using FrameData = SurfaceFrameData;
    PassExecutionInfo declaration;
    bool              transfer {};
    auto              Instantiate(const GraphPrepareContext&, Arc<SurfaceFrameData>) const
        -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError>;
};
struct SurfaceTransferFactory {
    using Pass = SurfacePass;
    auto Compile(const SurfaceTransferDesc&, const PassExecutionInfo& pass,
                 const PassCompileContext&)
        -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError>;
};
struct SurfaceReleaseFactory {
    using Pass = SurfacePass;
    auto Compile(const SurfaceReleaseDesc&, const PassExecutionInfo& pass,
                 const PassCompileContext&)
        -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError>;
};
} // namespace vrento::vulkan
export namespace rstd
{

} // namespace rstd
namespace vrento::vulkan
{
auto SurfacePass::Instantiate(const GraphPrepareContext& context, Arc<SurfaceFrameData> data) const
    -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError> {
    const auto& frame = data;
    if (! frame->lease.valid())
        return Err(GraphExecutionError { GraphExecutionStage::Binding, declaration.id });
    return transfer ? SurfaceTransferBackend { frame->lease }.Prepare(declaration, context)
                    : SurfaceReleaseBackend { frame->lease }.Prepare(declaration, context);
}
auto SurfaceTransferFactory::Compile(const SurfaceTransferDesc&, const PassExecutionInfo& pass,
                                     const PassCompileContext&)
    -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError> {
    if (pass.kind != surface_transfer_kind || pass.graphics || ! pass.parameters.is_empty() ||
        pass.uses.len() != usize(2) || pass.uses[usize()].access != GraphAccess::Read ||
        pass.uses[usize(1)].access != GraphAccess::Write ||
        pass.uses[usize()].usage != PassUsage::Transfer ||
        pass.uses[usize(1)].usage != PassUsage::Transfer)
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    return Ok(Arc<dyn<VulkanPass>>::make(SurfacePass { pass.clone(), true }));
}
auto SurfaceReleaseFactory::Compile(const SurfaceReleaseDesc&, const PassExecutionInfo& pass,
                                    const PassCompileContext&)
    -> Result<vrento::vulkan::Preparation<Arc<dyn<VulkanPass>>>, GraphExecutionError> {
    if (pass.kind != surface_release_kind || pass.graphics || ! pass.parameters.is_empty() ||
        pass.uses.len() != usize(1) || pass.uses[usize()].access != GraphAccess::Read ||
        pass.uses[usize()].usage != PassUsage::Release)
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    return Ok(Arc<dyn<VulkanPass>>::make(SurfacePass { pass.clone(), false }));
}
} // namespace vrento::vulkan
export namespace vrento::vulkan
{
inline auto register_surface_factories(Renderer& renderer) -> Result<empty, GraphExecutionError> {
    auto transfer = renderer.bind(Arc<dyn<PassFactory>>::make(
        TypedPassFactory<SurfaceTransferDesc, SurfaceTransferFactory> {}));
    if (transfer.is_err()) return transfer;
    return renderer.bind(Arc<dyn<PassFactory>>::make(
        TypedPassFactory<SurfaceReleaseDesc, SurfaceReleaseFactory> {}));
}
} // namespace vrento::vulkan
