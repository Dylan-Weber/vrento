export module vrento.tests.pass_set;
import vrento;
import rstd;

using namespace rstd::prelude;

using namespace vrento::vulkan;

export namespace vrento_test
{
namespace fg = vrento::fg;
inline auto texture_copy_pass(PassId id, ResourceId source, GraphIndex source_version,
                              ResourceId target, GraphIndex target_version,
                              GraphRange source_range = {}, GraphRange target_range = {})
    -> PassExecutionInfo {
    PassExecutionInfo pass { .id    = id,
                             .kind  = texture_copy_kind,
                             .queue = { 1, QueueClass::Transfer } };
    pass.uses.push({ source,
                     source_version,
                     GraphAccess::Read,
                     PassUsage::Transfer,
                     PassStages::Transfer,
                     source_range });
    pass.uses.push({ target,
                     target_version,
                     GraphAccess::Write,
                     PassUsage::Transfer,
                     PassStages::Transfer,
                     target_range });
    return pass;
}
inline auto texture_mips_pass(PassId id, ResourceId resource, GraphIndex source_version,
                              GraphIndex target_version, GraphIndex mips, GraphIndex layers = 1)
    -> PassExecutionInfo {
    PassExecutionInfo pass { .id    = id,
                             .kind  = texture_mips_kind,
                             .queue = { 1, QueueClass::Transfer } };
    pass.uses.push({ resource,
                     source_version,
                     GraphAccess::Read,
                     PassUsage::Transfer,
                     PassStages::Transfer,
                     { .layer_count = layers } });
    pass.uses.push({ resource,
                     target_version,
                     GraphAccess::Write,
                     PassUsage::Transfer,
                     PassStages::Transfer,
                     { .first_mip = 1, .mip_count = mips - 1, .layer_count = layers } });
    return pass;
}
inline auto texture_sampled_pass(PassId id, ResourceId resource, GraphIndex version,
                                 GraphRange range = {}) -> PassExecutionInfo {
    PassExecutionInfo pass { .id = id, .kind = texture_sampled_kind };
    pass.uses.push(
        { resource, version, GraphAccess::Read, PassUsage::Sampled, PassStages::Fragment, range });
    return pass;
}
inline auto declare_pass(fg::FrameGraphBuilder& graph, PassExecutionInfo& declaration,
                         String name = {}) -> Result<empty, GraphError> {
    auto existing = graph.find_pass(declaration.id);
    if (existing && (! existing->uses.is_empty() || existing->queue.value != declaration.queue.id))
        return Err(GraphError { GraphErrorKind::Conflict, declaration.id });
    if (! existing) {
        auto added = graph.add_pass(declaration.id, { declaration.queue.id }, rstd::move(name));
        if (added.is_err()) return Err(added.unwrap_err());
    }
    for (auto& use : declaration.uses) {
        auto version = fg::ResourceVersion { use.resource, use.version };
        auto result = use.access == GraphAccess::Read
                          ? graph.read(declaration.id,
                                       version,
                                       Some(GraphRange(use.range)),
                                       use.usage != PassUsage::Release)
                          : graph.write_range(declaration.id, version, Some(GraphRange(use.range)));
        if (result.is_err()) return Err(result.unwrap_err());
        use.logical_use = result.unwrap();
    }
    return Ok(empty {});
}

inline auto test_io(GraphSerial scope, PassExecutionInfo description) -> VulkanPassIO {
    VulkanPassIO result { .graphics       = description.graphics,
                          .queue          = description.queue,
                          .kind           = description.kind,
                          .schema_version = description.schema_version,
                          .parameters     = rstd::move(description.parameters) };
    for (const auto& use : description.uses) {
        VulkanResourceUse value { vrento::pass::ContentRef::from_version(
                                      scope, { use.resource, use.version }),
                                  use.usage,
                                  use.stages,
                                  use.range };
        if (use.access == GraphAccess::Read)
            result.inputs.push(rstd::move(value));
        else
            result.outputs.push(rstd::move(value));
    }
    return result;
}

inline auto make_pass_set(vrento::vulkan::VulkanPassData data,
                          vrento::vulkan::GraphLimits    limits = {})
    -> Result<vrento::vulkan::VulkanPassSet, vrento::fg::Error> {
    namespace vk = vrento::vulkan;
    namespace fg = vrento::fg;
    fg::FrameGraphBuilder builder(data.scope);
    for (const auto& resource : data.resources) {
        fg::ResourceDescription description { .id         = resource.id,
                                              .kind       = resource.kind,
                                              .bytes      = resource.bytes,
                                              .mip_levels = resource.mip_levels,
                                              .layers     = resource.layers,
                                              .aspects    = resource.aspects,
                                              .storage =
                                                  resource.fixed_storage ? resource.id.value : 0 };
        auto declared = resource.imported ? builder.import_resource(rstd::move(description))
                                          : builder.create_resource(rstd::move(description));
        if (declared.is_err()) return Err(declared.unwrap_err());
    }
    for (const auto& pass : data.passes) {
        auto added = builder.add_pass(pass.id, { pass.queue.id });
        if (added.is_err()) return Err(added.unwrap_err());
    }
    for (const auto& pass : data.passes) {
        auto valid = vk::validate_pass_execution(pass, data.resources.as_slice(), limits);
        if (valid.is_err()) return Err(valid.unwrap_err());
    }
    bool pending = true;
    while (pending) {
        pending = false;
        bool progressed {};
        for (const auto& pass : data.passes)
            for (const auto& use : pass.uses) {
                if (use.access != fg::Access::Write ||
                    builder.find_version({ use.resource, use.version }))
                    continue;
                pending = true;
                if (! builder.find_version({ use.resource, use.version - 1 })) continue;
                bool fixed     = builder.find_resource(use.resource)->storage != 0;
                bool available = true;
                if (pass.graphics) {
                    const Option<vk::GraphAttachment>* attachments[] { &pass.graphics->color,
                                                                       &pass.graphics->depth,
                                                                       &pass.graphics->resolve };
                    for (auto attachment : attachments)
                        if (*attachment && (**attachment).resource == use.resource &&
                            (**attachment).version == use.version)
                            available = (**attachment).store == vk::AttachmentStore::Store;
                }
                auto version = builder.reserve_write(
                    pass.id, { use.resource, use.version - 1 }, fixed, fixed, available);
                if (version.is_err()) return Err(version.unwrap_err());
                if (version->version != use.version)
                    return Err(fg::Error { fg::ErrorKind::Conflict, pass.id, use.resource });
                progressed = true;
            }
        if (pending && ! progressed) return Err(fg::Error { fg::ErrorKind::MissingProducer });
    }
    for (auto& pass : data.passes) {
        auto published = declare_pass(builder, pass);
        if (published.is_err()) return Err(published.unwrap_err());
    }
    for (auto edge : data.dependencies) {
        auto added = builder.depends_on(edge.after, edge.before);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    auto graph = rstd::move(builder).finish();
    if (graph.is_err()) return Err(graph.unwrap_err());
    return vk::VulkanPassSet::associate(
        graph.unwrap(), rstd::move(data.passes), limits, data.resources.as_slice());
}
} // namespace vrento_test
