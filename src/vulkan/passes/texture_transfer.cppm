export module vrento:vulkan.passes.texture_transfer;
export import :vulkan.graph;
export import :vulkan.renderer;
import rstd;

using namespace rstd::prelude;

namespace vrento::vulkan
{
struct TextureTransferPrepared {
    graph_texture::TextureViewRef source, target;
    VkImageCopy                   copy {};
    Option<TextureMipmapProgram>  mips;
    auto Record(GraphRecordContext& context) const -> Result<empty, GraphExecutionError> {
        auto fail = [](graph_texture::Error error) -> Result<empty, GraphExecutionError> {
            return Err(GraphExecutionError {
                GraphExecutionStage::Record, {}, {}, VK_SUCCESS, Some(error) });
        };
        if (mips) {
            auto generated = mips->record(context.submission);
            if (generated.is_err()) return fail(generated.unwrap_err());
        } else if (source.valid()) {
            auto command = native_recording(context.submission, context.session);
            if (command.is_err()) return fail(command.unwrap_err());
            auto src = native_texture(source.texture()), dst = native_texture(target.texture());
            if (src.is_err()) return fail(src.unwrap_err());
            if (dst.is_err()) return fail(dst.unwrap_err());
            context.session.device().handle().Dispatch().vkCmdCopyImage(
                command.unwrap(),
                src.unwrap(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst.unwrap(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &copy);
        }
        return Ok(empty {});
    }
};
struct TextureTransferBackend {
    auto Prepare(const PassExecutionInfo& pass, const GraphPrepareContext& context)
        -> Result<rstd::sync::Arc<dyn<VulkanPass>>, GraphExecutionError>;
};
struct TextureTransferPass {
    PassExecutionInfo description;
    auto              Instantiate(const GraphPrepareContext& context) const
        -> Result<rstd::sync::Arc<dyn<VulkanPass>>, GraphExecutionError> {
        return TextureTransferBackend {}.Prepare(description, context);
    }
};
} // namespace vrento::vulkan
namespace rstd
{

template<>
struct Impl<vrento::vulkan::VulkanPassBackend, vrento::vulkan::TextureTransferBackend>
    : ImplBase<vrento::vulkan::TextureTransferBackend> {
    auto Prepare(const vrento::vulkan::PassExecutionInfo&   pass,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<sync::Arc<dyn<vrento::vulkan::VulkanPass>>, vrento::vulkan::GraphExecutionError> {
        return self().Prepare(pass, context);
    }
};

} // namespace rstd
namespace vrento::vulkan
{
auto TextureTransferBackend::Prepare(const PassExecutionInfo&   pass,
                                     const GraphPrepareContext& context)
    -> Result<rstd::sync::Arc<dyn<VulkanPass>>, GraphExecutionError> {
    auto unsupported = [&]() -> Result<rstd::sync::Arc<dyn<VulkanPass>>, GraphExecutionError> {
        return Err(GraphExecutionError { GraphExecutionStage::Unsupported, pass.id });
    };
    if (! pass.parameters.is_empty() || pass.graphics) return unsupported();
    if (pass.kind == texture_sampled_kind) {
        if (pass.uses.len() != usize(1) || pass.uses[usize()].usage != PassUsage::Sampled ||
            pass.uses[usize()].access != GraphAccess::Read)
            return unsupported();
        return Ok(rstd::sync::Arc<dyn<VulkanPass>>::make(TextureTransferPrepared {}));
    }
    if (pass.uses.len() != usize(2)) return unsupported();
    const auto &read = pass.uses[usize()], &write = pass.uses[usize(1)];
    if (read.access != GraphAccess::Read || write.access != GraphAccess::Write ||
        read.usage != PassUsage::Transfer || write.usage != PassUsage::Transfer ||
        read.range.mip_count != 1 || read.range.layer_count != write.range.layer_count)
        return unsupported();
    auto src = context.bindings.resolve(read.resource, read.version).unwrap()->Texture();
    auto dst = context.bindings.resolve(write.resource, write.version).unwrap()->Texture();
    if (! src || ! dst) return unsupported();
    auto source = src->description().range, target = dst->description().range;
    source.first_mip += read.range.first_mip;
    source.first_layer += read.range.first_layer;
    target.first_mip += write.range.first_mip;
    target.first_layer += write.range.first_layer;
    if (pass.kind == texture_mips_kind) {
        if (read.resource != write.resource || write.range.first_mip != read.range.first_mip + 1 ||
            src->texture().identity() != dst->texture().identity() ||
            read.range.first_layer != write.range.first_layer || read.range.aspect != 1 ||
            write.range.aspect != 1)
            return unsupported();
        source.mip_count   = write.range.mip_count + 1;
        source.layer_count = read.range.layer_count;
        auto prepared      = TextureMipmapProgram::prepare(context.session, *dst, Some(source));
        if (prepared.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Prepare,
                                             pass.id,
                                             write.resource,
                                             VK_SUCCESS,
                                             Some(prepared.unwrap_err()) });
        return Ok(rstd::sync::Arc<dyn<VulkanPass>>::make(TextureTransferPrepared {
            .target = rstd::move(dst).unwrap(), .mips = Some(rstd::move(prepared).unwrap()) }));
    }
    if (pass.kind != texture_copy_kind || write.range.mip_count != 1 ||
        read.range.aspect != write.range.aspect)
        return unsupported();
    const auto &source_desc = src->texture().description(),
               &target_desc = dst->texture().description();
    auto a                  = graph_texture::format_layout(source_desc.format),
         b                  = graph_texture::format_layout(target_desc.format);
    auto from               = graph_texture::mip_extent(source_desc, source.first_mip),
         to                 = graph_texture::mip_extent(target_desc, target.first_mip);
    if (from != to || source_desc.samples != target_desc.samples ||
        a.block_width != b.block_width || a.block_height != b.block_height ||
        a.block_bytes != b.block_bytes || a.aspect != b.aspect)
        return unsupported();
    VkImageCopy region {};
    auto        aspect = source.aspect == graph_texture::Aspect::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource = {
        VkImageAspectFlags(aspect), source.first_mip, source.first_layer, read.range.layer_count
    };
    region.dstSubresource = {
        VkImageAspectFlags(aspect), target.first_mip, target.first_layer, write.range.layer_count
    };
    region.extent = { from.width, from.height, 1 };
    return Ok(rstd::sync::Arc<dyn<VulkanPass>>::make(TextureTransferPrepared {
        .source = rstd::move(src).unwrap(), .target = rstd::move(dst).unwrap(), .copy = region }));
}
} // namespace vrento::vulkan
export namespace vrento::vulkan
{
struct TextureCopyDesc {
    using PassDescriptor = pass::PassDesc;
    pass::ContentRef source, target;
    GraphRange       source_range, target_range;
    LogicalQueue     queue { 1, QueueClass::Transfer };
    auto             vulkan_io() const -> VulkanPassIO {
        VulkanPassIO result { .queue = queue, .kind = texture_copy_kind };
        result.inputs.push(
            { source.clone(), PassUsage::Transfer, PassStages::Transfer, source_range });
        result.outputs.push(
            { target.clone(), PassUsage::Transfer, PassStages::Transfer, target_range });
        return result;
    }
};
struct TextureMipsDesc {
    using PassDescriptor = pass::PassDesc;
    pass::ContentRef source, target;
    GraphIndex       mip_levels {}, layers { 1 };
    LogicalQueue     queue { 1, QueueClass::Transfer };
    auto             validate() const -> Result<empty, GraphError> {
        if (mip_levels < 2 || ! layers || source.version().resource != target.version().resource)
            return Err(GraphError { GraphErrorKind::Conflict, {}, target.version().resource });
        return Ok(empty {});
    }
    auto vulkan_io() const -> VulkanPassIO {
        VulkanPassIO result { .queue = queue, .kind = texture_mips_kind };
        result.inputs.push({ source.clone(),
                             PassUsage::Transfer,
                             PassStages::Transfer,
                             { .layer_count = layers } });
        result.outputs.push({ target.clone(),
                              PassUsage::Transfer,
                              PassStages::Transfer,
                              { .first_mip   = 1,
                                .mip_count   = mip_levels > 1 ? mip_levels - 1 : 0,
                                .layer_count = layers } });
        result.storage.push({ target.clone(), source.clone() });
        return result;
    }
};
struct TextureTransferFactory {
    using Pass = TextureTransferPass;
    template<typename T>
    auto Compile(const T&, const PassExecutionInfo& declaration, const PassCompileContext&)
        -> Result<rstd::sync::Arc<dyn<VulkanPass>>, GraphExecutionError> {
        bool valid =
            (rstd::mtp::same_as<T, TextureCopyDesc> && declaration.kind == texture_copy_kind) ||
            (rstd::mtp::same_as<T, TextureMipsDesc> && declaration.kind == texture_mips_kind);
        if (! valid)
            return Err(GraphExecutionError { GraphExecutionStage::Schema, declaration.id });
        return Ok(
            rstd::sync::Arc<dyn<VulkanPass>>::make(TextureTransferPass { declaration.clone() }));
    }
};
struct DeclaredTexturePass {
    PassInput           description;
    fg::ResourceVersion output;
};
inline auto declare_texture_copy_output(fg::FrameGraphBuilder& builder, PassId id,
                                        pass::ContentRef source, pass::ContentRef output,
                                        GraphRange from = {}, GraphRange to = {},
                                        GraphSerial revision = 1) -> Result<PassInput, GraphError> {
    return declare_input(builder,
                         id,
                         TextureCopyDesc { rstd::move(source), rstd::move(output), from, to },
                         revision);
}
inline auto declare_texture_copy_output(fg::FrameGraphBuilder& builder, PassId id,
                                        fg::ResourceVersion source, fg::ResourceVersion output,
                                        GraphRange from = {}, GraphRange to = {},
                                        GraphSerial revision = 1) -> Result<PassInput, GraphError> {
    auto input = pass::ContentRef::bind(builder, source);
    if (input.is_err()) return Err(input.unwrap_err());
    auto target = pass::ContentRef::bind(builder, output);
    if (target.is_err()) return Err(target.unwrap_err());
    return declare_texture_copy_output(
        builder, id, input.unwrap(), target.unwrap(), from, to, revision);
}
inline auto declare_texture_copy(fg::FrameGraphBuilder& builder, PassId id, pass::ContentRef source,
                                 pass::ContentRef target, GraphRange from = {}, GraphRange to = {},
                                 GraphSerial revision = 1)
    -> Result<DeclaredContentPass, GraphError> {
    auto added = builder.add_pass(id, { 1 });
    if (added.is_err()) return Err(added.unwrap_err());
    auto output = pass::ContentRef::write(builder, id, target);
    if (output.is_err()) return Err(output.unwrap_err());
    auto description = declare_texture_copy_output(
        builder, id, rstd::move(source), output->clone(), from, to, revision);
    if (description.is_err()) return Err(description.unwrap_err());
    return Ok(DeclaredContentPass { description.unwrap(), output.unwrap() });
}
inline auto declare_texture_copy(fg::FrameGraphBuilder& builder, PassId id,
                                 fg::ResourceVersion source, fg::ResourceVersion target,
                                 GraphRange from = {}, GraphRange to = {}, GraphSerial revision = 1)
    -> Result<DeclaredTexturePass, GraphError> {
    auto input = pass::ContentRef::bind(builder, source);
    if (input.is_err()) return Err(input.unwrap_err());
    auto destination = pass::ContentRef::bind(builder, target);
    if (destination.is_err()) return Err(destination.unwrap_err());
    auto result =
        declare_texture_copy(builder, id, input.unwrap(), destination.unwrap(), from, to, revision);
    if (result.is_err()) return Err(result.unwrap_err());
    return Ok(DeclaredTexturePass { rstd::move(result->description), result->output.version() });
}
inline auto declare_texture_mips(fg::FrameGraphBuilder& builder, PassId id, pass::ContentRef source,
                                 GraphIndex mips, GraphIndex layers = 1, GraphSerial revision = 1)
    -> Result<DeclaredContentPass, GraphError> {
    if (mips < 2 || ! layers) return Err(GraphError { GraphErrorKind::Range, id });
    auto added = builder.add_pass(id, { 1 });
    if (added.is_err()) return Err(added.unwrap_err());
    auto output = pass::ContentRef::write(builder, id, source, true, true);
    if (output.is_err()) return Err(output.unwrap_err());
    auto description =
        declare_input(builder,
                      id,
                      TextureMipsDesc { rstd::move(source), output->clone(), mips, layers },
                      revision);
    if (description.is_err()) return Err(description.unwrap_err());
    return Ok(DeclaredContentPass { description.unwrap(), output.unwrap() });
}
inline auto declare_texture_mips(fg::FrameGraphBuilder& builder, PassId id,
                                 fg::ResourceVersion source, GraphIndex mips, GraphIndex layers = 1,
                                 GraphSerial revision = 1)
    -> Result<DeclaredTexturePass, GraphError> {
    auto input = pass::ContentRef::bind(builder, source);
    if (input.is_err()) return Err(input.unwrap_err());
    auto result = declare_texture_mips(builder, id, input.unwrap(), mips, layers, revision);
    if (result.is_err()) return Err(result.unwrap_err());
    return Ok(DeclaredTexturePass { rstd::move(result->description), result->output.version() });
}
inline auto register_texture_factories(Renderer& renderer) -> Result<empty, GraphExecutionError> {
    auto copy = renderer.bind(
        Arc<dyn<PassFactory>>::make(TypedPassFactory<TextureCopyDesc, TextureTransferFactory> {}));
    if (copy.is_err()) return copy;
    return renderer.bind(
        Arc<dyn<PassFactory>>::make(TypedPassFactory<TextureMipsDesc, TextureTransferFactory> {}));
}
inline auto register_texture_passes(VulkanPassBackends& backends)
    -> Result<empty, GraphExecutionError> {
    const GraphSerial kinds[] { texture_copy_kind, texture_mips_kind, texture_sampled_kind };
    for (auto kind : kinds) {
        auto bound =
            backends.bind(kind, 1, Box<dyn<VulkanPassBackend>>::make(TextureTransferBackend {}));
        if (bound.is_err()) return Err(bound.unwrap_err());
    }
    return Ok(empty {});
}
} // namespace vrento::vulkan
