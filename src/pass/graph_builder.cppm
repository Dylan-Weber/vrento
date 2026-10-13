export module vrento:pass.graph_builder;
export import :pass.desc;
import :frame_graph.graph;
import rstd;
export namespace vrento::pass
{
inline auto declare_io(fg::FrameGraphBuilder& graph, fg::PassId id, fg::LogicalQueueId queue,
                       const PassIO& io, String name = {}) -> Result<empty, fg::Error> {
    auto existing = graph.find_pass(id);
    if (existing && (! existing->uses.is_empty() || existing->queue != queue))
        return Err(fg::Error { fg::ErrorKind::Conflict, id });
    if (! existing) {
        auto added = graph.add_pass(id, queue, rstd::move(name));
        if (added.is_err()) return Err(added.unwrap_err());
    }
    for (const auto& input : io.inputs) {
        auto source = input.source.resolve(graph);
        if (source.is_err()) return Err(source.unwrap_err());
        auto result = graph.read(id,
                                 input.source.version(),
                                 Some(fg::ResourceRange(input.range)),
                                 input.requires_contents);
        if (result.is_err()) return Err(result.unwrap_err());
    }
    for (const auto& output : io.outputs) {
        auto target = output.target.resolve(graph);
        if (target.is_err()) return Err(target.unwrap_err());
        if (auto reference = output.target.output(); reference && reference->producer != id)
            return Err(fg::Error { fg::ErrorKind::Conflict, id, target->resource });
        if (output.reuse) {
            auto version = graph.find_version(output.target.version());
            if (! version || (! version->previous || *version->previous != output.reuse->version()))
                return Err(
                    fg::Error { fg::ErrorKind::Conflict, id, output.target.version().resource });
            auto reused = graph.reuse_storage(output.target.version());
            if (reused.is_err()) return reused;
        }
        auto available = graph.set_contents_available(output.target.version(), output.available);
        if (available.is_err()) return available;
        auto result =
            graph.write_range(id, output.target.version(), Some(fg::ResourceRange(output.range)));
        if (result.is_err()) return Err(result.unwrap_err());
    }
    return Ok(empty {});
}
} // namespace vrento::pass
