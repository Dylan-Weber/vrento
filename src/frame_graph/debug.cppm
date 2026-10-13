export module vrento:frame_graph.debug;
export import :frame_graph.graph;
import rstd;

namespace vrento::fg
{
using namespace rstd::literals;
auto escape_name(ref<str> name) -> String {
    Vec<u8> result;
    for (auto byte : name) {
        switch (byte.to_primitive()) {
        case '\\':
            result.push(u8('\\'));
            result.push(u8('\\'));
            break;
        case '"':
            result.push(u8('\\'));
            result.push(u8('"'));
            break;
        case '\n':
            result.push(u8('\\'));
            result.push(u8('n'));
            break;
        case '\r': break;
        default: result.push(u8(byte)); break;
        }
    }
    return String::from_utf8(rstd::move(result)).unwrap();
}
auto version_name(ResourceVersion version) -> String {
    return rstd::format(
        "r{}_{}_v{}", version.resource.value, version.resource.generation, version.version);
}
} // namespace vrento::fg
export namespace vrento::fg
{
inline auto graphviz(const FrameGraph& graph) -> String {
    auto output = "digraph framegraph {\n"_Str;
    for (const auto& pass : graph.passes())
        output.push_str(rstd::format("p{} [shape=box,label=\"{}\\nqueue={}\"];\n",
                                     pass.id.value,
                                     escape_name(pass.name.as_str()).as_str(),
                                     pass.queue.value)
                            .as_str());
    for (const auto& version : graph.versions()) {
        Index slot = invalid_index, first = invalid_index, last {};
        for (auto allocation : graph.inspect().allocations)
            if (allocation.version == version.id) slot = allocation.slot;
        for (const auto& life : graph.inspect().lifetimes)
            if (life.version == version.id) {
                first = life.first;
                last  = life.last;
            }
        auto id = version_name(version.id);
        output.push_str(
            rstd::format(
                "{} [shape=ellipse,label=\"{}\\nresource={}\\nversion={}\\nslot={}\\nfirst={} "
                "last={}\"];\n",
                id.as_str(),
                escape_name(graph.find_resource(version.id.resource)->name.as_str()).as_str(),
                version.id.resource.value,
                version.id.version,
                slot,
                first,
                last)
                .as_str());
        if (version.previous)
            output.push_str(rstd::format("{} -> {} [style=dashed,label=\"{}\"];\n",
                                         version_name(*version.previous).as_str(),
                                         id.as_str(),
                                         version.inherits ? "inherits"_str : "version"_str)
                                .as_str());
    }
    for (const auto& pass : graph.passes())
        for (const auto& use : pass.uses) {
            auto  version = version_name(use.version);
            auto  id      = rstd::format("p{}", pass.id.value);
            auto& before  = use.access == Access::Read ? version : id;
            auto& after   = use.access == Access::Read ? id : version;
            output.push_str(rstd::format("{} -> {} [label=\"{} use={}\"];\n",
                                         before.as_str(),
                                         after.as_str(),
                                         use.access == Access::Read ? "read"_str : "write"_str,
                                         use.id)
                                .as_str());
        }
    for (auto dependency : graph.inspect().dependencies) {
        auto sources = "order"_Str;
        if (dependency.sources & Index(DependencySource::Explicit))
            sources.push_str(" explicit"_str);
        if (dependency.sources & Index(DependencySource::Contents))
            sources.push_str(" contents"_str);
        if (dependency.sources & Index(DependencySource::Storage)) sources.push_str(" storage"_str);
        if (dependency.sources & Index(DependencySource::Queue)) sources.push_str(" queue"_str);
        output.push_str(rstd::format("p{} -> p{} [label=\"{}\"];\n",
                                     graph.passes()[usize(dependency.before)].id.value,
                                     graph.passes()[usize(dependency.after)].id.value,
                                     sources.as_str())
                            .as_str());
    }
    output.push_str("}\n"_str);
    return output;
}
} // namespace vrento::fg
