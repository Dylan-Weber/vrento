export module vrento:frame_graph.graph;
export import :frame_graph.types;
import rstd;

using rstd::collections::HashMap;
using rstd::collections::HashSet;

export namespace vrento::fg
{
struct AnalysisStatistics {
    Serial attempts {}, failures {};
};
class FrameGraph {
    struct Data {
        FrameGraphData graph;
        FrameGraphPlan plan;
        Data(FrameGraphData input, FrameGraphPlan compiled)
            : graph(rstd::move(input)), plan(rstd::move(compiled)) {}
    };
    rstd::sync::Arc<Data> data_;
    explicit FrameGraph(FrameGraphData graph, FrameGraphPlan plan)
        : data_(rstd::sync::Arc<Data>::make(rstd::move(graph), rstd::move(plan))) {}
    static auto analyze(const FrameGraphData& data, Limits limits) -> Result<FrameGraphPlan, Error>;

public:
    FrameGraph(const FrameGraph& other): data_(other.data_.clone()) {}
    FrameGraph(FrameGraph&&) = default;
    auto operator=(const FrameGraph& other) -> FrameGraph& {
        if (this != &other) data_ = other.data_.clone();
        return *this;
    }
    auto        operator=(FrameGraph&&) -> FrameGraph& = default;
    static auto create(FrameGraphData data, Limits limits = {},
                       AnalysisStatistics* statistics = nullptr) -> Result<FrameGraph, Error> {
        if (statistics) ++statistics->attempts;
        auto plan = analyze(data, limits);
        if (plan.is_err()) {
            if (statistics) ++statistics->failures;
            return Err(plan.unwrap_err());
        }
        return Ok(FrameGraph(rstd::move(data), plan.unwrap()));
    }
    auto find_resource(ResourceId id) const -> const ResourceDescription* {
        for (const auto& value : resources())
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    auto find_version(ResourceVersion id) const -> const VersionNode* {
        for (const auto& value : versions())
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    auto find_pass(PassId id) const -> const PassNode* {
        for (const auto& value : passes())
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    auto scope() const -> Serial { return data_->graph.scope; }
    auto resources() const -> slice<ResourceDescription> {
        return data_->graph.resources.as_slice();
    }
    auto versions() const -> slice<VersionNode> { return data_->graph.versions.as_slice(); }
    auto passes() const -> slice<PassNode> { return data_->graph.passes.as_slice(); }
    auto dependencies() const -> slice<Dependency> { return data_->graph.dependencies.as_slice(); }
    auto exports() const -> slice<ResourceVersion> { return data_->graph.exports.as_slice(); }
    auto inspect() const -> const FrameGraphPlan& { return data_->plan; }
    auto clone_data() const -> FrameGraphData { return data_->graph.clone(); }
    auto compile(Limits limits = {}) const -> Result<FrameGraphPlan, Error> {
        if (resources().len().to_primitive() > limits.resources ||
            versions().len().to_primitive() > limits.versions ||
            passes().len().to_primitive() > limits.passes)
            return Err(Error { ErrorKind::Limit });
        Serial uses {}, edges {}, names {};
        for (const auto& pass : passes()) {
            uses += pass.uses.len().to_primitive();
            names += pass.name.len().to_primitive();
        }
        for (const auto& resource : resources()) names += resource.name.len().to_primitive();
        for (const auto& next : data_->plan.successors) edges += next.len().to_primitive();
        if (uses > limits.uses || edges > limits.dependencies || names > limits.name_bytes)
            return Err(Error { ErrorKind::Limit });
        return Ok(data_->plan.clone());
    }
};

class FrameGraphBuilder {
    FrameGraphData data_;
    Serial         next_use_ { 1 };
    auto           resource_index(ResourceId id) const -> Index {
        for (Index i {}; i < data_.resources.len().to_primitive(); ++i)
            if (data_.resources[usize(i)].id == id) return i;
        return invalid_index;
    }
    auto pass_index(PassId id) const -> Index {
        for (Index i {}; i < data_.passes.len().to_primitive(); ++i)
            if (data_.passes[usize(i)].id == id) return i;
        return invalid_index;
    }
    auto version_index(ResourceVersion id) const -> Index {
        for (Index i {}; i < data_.versions.len().to_primitive(); ++i)
            if (data_.versions[usize(i)].id == id) return i;
        return invalid_index;
    }
    auto declare_resource(ResourceDescription desc, bool imported)
        -> Result<ResourceVersion, Error> {
        if (! desc.id.valid()) return Err(Error { ErrorKind::Invalid, {}, desc.id });
        for (const auto& existing : data_.resources)
            if (existing.id.value == desc.id.value)
                return Err(Error { ErrorKind::Duplicate, {}, desc.id });
        desc.imported = imported;
        ResourceVersion first { desc.id, 0 };
        data_.resources.push(rstd::move(desc));
        data_.versions.push({ .id = first, .available = imported });
        return Ok(first);
    }

public:
    auto find_resource(ResourceId id) const -> const ResourceDescription* {
        for (const auto& value : data_.resources)
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    auto find_version(ResourceVersion id) const -> const VersionNode* {
        for (const auto& value : data_.versions)
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    auto find_pass(PassId id) const -> const PassNode* {
        for (const auto& value : data_.passes)
            if (value.id == id) return rstd::addressof(value);
        return nullptr;
    }
    explicit FrameGraphBuilder(Serial scope): data_ { .scope = scope } {}
    explicit FrameGraphBuilder(FrameGraphData data): data_(rstd::move(data)) {
        for (const auto& pass : data_.passes)
            for (const auto& use : pass.uses) next_use_ = rstd::cmp::max(next_use_, use.id + 1);
    }
    auto import_resource(ResourceDescription desc) -> Result<ResourceVersion, Error> {
        return declare_resource(rstd::move(desc), true);
    }
    auto create_resource(ResourceDescription desc) -> Result<ResourceVersion, Error> {
        return declare_resource(rstd::move(desc), false);
    }
    auto import_initial(ResourceId id, bool retained = true) -> Result<empty, Error> {
        auto r = resource_index(id), v = version_index({ id, 0 });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, id });
        data_.resources[usize(r)].imported = true;
        data_.resources[usize(r)].retained |= retained;
        data_.versions[usize(v)].available = true;
        return Ok(empty {});
    }
    auto reserve_write(PassId pass, ResourceVersion base, bool same_storage = false,
                       bool inherits = false, bool available = true)
        -> Result<ResourceVersion, Error> {
        if (pass_index(pass) == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (version_index(base) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, base.resource });
        Index next {};
        for (const auto& version : data_.versions)
            if (version.id.resource == base.resource)
                next = rstd::cmp::max(next, version.id.version);
        if (next == invalid_index) return Err(Error { ErrorKind::Limit, pass, base.resource });
        ResourceVersion output { base.resource, next + 1 };
        data_.versions.push({ output, Some(pass), Some(base), inherits, same_storage, available });
        return Ok(output);
    }
    auto prepend_write(PassId pass, ResourceId resource) -> Result<ResourceVersion, Error> {
        if (pass_index(pass) == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (resource_index(resource) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, resource });
        for (const auto& version : data_.versions)
            if (version.id.resource == resource && version.id.version == invalid_index)
                return Err(Error { ErrorKind::Limit, pass, resource });
        for (auto& version : data_.versions) {
            if (version.id.resource != resource) continue;
            if (version.id.version) ++version.id.version;
            if (version.previous) ++version.previous->version;
        }
        for (auto& node : data_.passes)
            for (auto& use : node.uses)
                if (use.version.resource == resource) ++use.version.version;
        for (auto& output : data_.exports)
            if (output.resource == resource) ++output.version;
        ResourceVersion output { resource, 1 };
        data_.versions.push(
            { output, Some(pass), Some(ResourceVersion { resource, 0 }), true, true, true });
        return Ok(output);
    }
    auto write_range(PassId pass, ResourceVersion output, Option<ResourceRange> selected = None())
        -> Result<Serial, Error> {
        auto p = pass_index(pass), r = resource_index(output.resource), v = version_index(output);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, output.resource });
        if (! data_.versions[usize(v)].writer || *data_.versions[usize(v)].writer != pass)
            return Err(Error { ErrorKind::Conflict, pass, output.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, output.resource });
        auto id = next_use_++;
        data_.passes[usize(p)].uses.push({ id, output, Access::Write, range });
        return Ok(id);
    }
    auto set_contents_available(ResourceVersion output, bool available) -> Result<empty, Error> {
        auto index = version_index(output);
        if (index == invalid_index || ! data_.versions[usize(index)].writer)
            return Err(Error { ErrorKind::MissingProducer, {}, output.resource });
        data_.versions[usize(index)].available = available;
        return Ok(empty {});
    }
    auto reuse_storage(ResourceVersion output) -> Result<empty, Error> {
        auto v = version_index(output);
        if (v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, output.resource });
        if (! data_.versions[usize(v)].previous)
            return Err(Error { ErrorKind::Invalid, {}, output.resource });
        data_.versions[usize(v)].same_storage = true;
        return Ok(empty {});
    }
    auto add_pass(PassId id, LogicalQueueId queue = {}, String name = {}) -> Result<PassId, Error> {
        if (! id.valid()) return Err(Error { ErrorKind::Invalid, id });
        if (pass_index(id) != invalid_index) return Err(Error { ErrorKind::Duplicate, id });
        data_.passes.push({ id, queue, rstd::move(name) });
        return Ok(id);
    }
    auto read(PassId pass, ResourceVersion version, Option<ResourceRange> selected = None(),
              bool requires_contents = true) -> Result<Serial, Error> {
        auto p = pass_index(pass), r = resource_index(version.resource), v = version_index(version);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, version.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, version.resource });
        auto id = next_use_++;
        data_.passes[usize(p)].uses.push({ id, version, Access::Read, range, requires_contents });
        return Ok(id);
    }
    auto write(PassId pass, ResourceVersion base, Option<ResourceRange> selected = None(),
               bool same_storage = false, bool inherits = false, bool available = true)
        -> Result<ResourceVersion, Error> {
        auto p = pass_index(pass), r = resource_index(base.resource), v = version_index(base);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, base.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, base.resource });
        auto reserved = reserve_write(pass, base, same_storage, inherits, available);
        if (reserved.is_err()) return reserved;
        auto output = reserved.unwrap();
        write_range(pass, output, Some(range)).unwrap();
        return Ok(output);
    }
    auto depends_on(PassId pass, PassId before) -> Result<empty, Error> {
        if (pass_index(pass) == invalid_index || pass_index(before) == invalid_index)
            return Err(Error { ErrorKind::MissingPass, pass });
        Dependency edge { before, pass };
        for (auto previous : data_.dependencies)
            if (previous == edge) return Ok(empty {});
        data_.dependencies.push(rstd::move(edge));
        return Ok(empty {});
    }
    auto export_resource(ResourceVersion version) -> Result<empty, Error> {
        if (version_index(version) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, version.resource });
        for (auto previous : data_.exports)
            if (previous == version) return Ok(empty {});
        data_.exports.push(rstd::move(version));
        return Ok(empty {});
    }
    auto data() const -> const FrameGraphData& { return data_; }
    auto finish(Limits              limits     = {},
                AnalysisStatistics* statistics = nullptr) && -> Result<FrameGraph, Error> {
        return FrameGraph::create(rstd::move(data_), limits, statistics);
    }
};
} // namespace vrento::fg

namespace vrento::fg
{
auto FrameGraph::analyze(const FrameGraphData& data, Limits limits)
    -> Result<FrameGraphPlan, Error> {
    if (! data.scope) return Err(Error { ErrorKind::Invalid });
    if (data.resources.len().to_primitive() > limits.resources ||
        data.versions.len().to_primitive() > limits.versions ||
        data.passes.len().to_primitive() > limits.passes ||
        data.dependencies.len().to_primitive() > limits.dependencies)
        return Err(Error { ErrorKind::Limit });
    HashMap<u64, Index> resources, passes, versions;
    auto                resource_index = [&](ResourceId id) -> Index {
        auto found = resources.get(u64(id.value));
        return found && data.resources[usize(**found)].id == id ? Index(**found) : invalid_index;
    };
    auto pass_index = [&](PassId id) -> Index {
        auto found = passes.get(u64(id.value));
        return found ? Index(**found) : invalid_index;
    };
    auto version_index = [&](ResourceVersion id) -> Index {
        auto r = resource_index(id.resource);
        if (r == invalid_index) return invalid_index;
        auto found = versions.get(u64((Serial(r) << 32) | id.version));
        return found ? Index(**found) : invalid_index;
    };
    Serial names {}, uses_count {};
    for (Index i {}; i < data.resources.len().to_primitive(); ++i) {
        const auto& r = data.resources[usize(i)];
        if (! r.id.valid() || Index(r.kind) > Index(ResourceKind::Buffer) ||
            (r.kind == ResourceKind::Buffer && ! r.bytes) ||
            (r.kind == ResourceKind::Texture &&
             (! r.mip_levels || ! r.layers || ! r.aspects || (r.aspects & ~Index(7)))))
            return Err(Error { ErrorKind::Invalid, {}, r.id });
        if (resources.insert(u64(r.id.value), Index(i)).is_some())
            return Err(Error { ErrorKind::Duplicate, {}, r.id });
        names += r.name.len().to_primitive();
    }
    for (Index i {}; i < data.passes.len().to_primitive(); ++i) {
        const auto& p = data.passes[usize(i)];
        if (! p.id.valid()) return Err(Error { ErrorKind::Invalid, p.id });
        if (passes.insert(u64(p.id.value), Index(i)).is_some())
            return Err(Error { ErrorKind::Duplicate, p.id });
        uses_count += p.uses.len().to_primitive();
        names += p.name.len().to_primitive();
    }
    if (names > limits.name_bytes || uses_count > limits.uses)
        return Err(Error { ErrorKind::Limit });
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto& v = data.versions[usize(i)];
        auto        r = resource_index(v.id.resource);
        if (r == invalid_index) return Err(Error { ErrorKind::MissingResource, {}, v.id.resource });
        if (versions.insert(u64((Serial(r) << 32) | v.id.version), Index(i)).is_some())
            return Err(Error { ErrorKind::Duplicate, {}, v.id.resource });
        if (v.writer && pass_index(*v.writer) == invalid_index)
            return Err(Error { ErrorKind::MissingPass, *v.writer });
        if (! v.id.version && v.writer)
            return Err(Error { ErrorKind::Invalid, *v.writer, v.id.resource });
        if (v.id.version && ! v.writer)
            return Err(Error { ErrorKind::MissingProducer, {}, v.id.resource });
        if (! v.id.version && v.available != data.resources[usize(r)].imported)
            return Err(Error { ErrorKind::Invalid, {}, v.id.resource });
        if (v.previous &&
            (v.previous->resource != v.id.resource || v.previous->version >= v.id.version))
            return Err(Error { ErrorKind::Invalid, {}, v.id.resource });
    }
    for (const auto& v : data.versions)
        if (v.previous && version_index(*v.previous) == invalid_index)
            return Err(Error { ErrorKind::MissingProducer, {}, v.id.resource });
    Vec<Vec<ResourceRange>> writes;
    Vec<Vec<Index>>         version_uses;
    for (usize i {}; i < data.versions.len(); ++i) {
        writes.push(Vec<ResourceRange> {});
        version_uses.push(Vec<Index> {});
    }
    HashSet<u64> use_ids;
    for (const auto& p : data.passes)
        for (const auto& use : p.uses) {
            auto r = resource_index(use.version.resource), v = version_index(use.version);
            if (! use.id || ! use_ids.insert(u64(use.id)))
                return Err(Error { ErrorKind::Duplicate, p.id, use.version.resource });
            if (r == invalid_index || v == invalid_index)
                return Err(Error { ErrorKind::MissingResource, p.id, use.version.resource });
            if (Index(use.access) > Index(Access::Write))
                return Err(Error { ErrorKind::Invalid, p.id, use.version.resource });
            if (! valid_range(data.resources[usize(r)], use.range))
                return Err(Error { ErrorKind::Range, p.id, use.version.resource });
            auto pass = pass_index(p.id);
            bool registered {};
            for (auto existing : version_uses[usize(v)]) registered |= existing == pass;
            if (! registered) version_uses[usize(v)].push(Index(pass));
            if (use.access == Access::Write) {
                const auto& node = data.versions[usize(v)];
                if (! node.writer || *node.writer != p.id)
                    return Err(Error { ErrorKind::Conflict, p.id, use.version.resource });
                writes[usize(v)].push(ResourceRange(use.range));
            }
        }
    for (usize i {}; i < data.versions.len(); ++i)
        if (data.versions[i].writer && writes[i].is_empty())
            return Err(Error { ErrorKind::MissingProducer,
                               *data.versions[i].writer,
                               data.versions[i].id.resource });
    auto available = [&](ResourceVersion requested, ResourceRange range) -> bool {
        Vec<ResourceRange> uncovered;
        uncovered.push(rstd::move(range));
        auto v = version_index(requested);
        while (v != invalid_index) {
            const auto& node = data.versions[usize(v)];
            if (! node.writer)
                return data.resources[usize(resource_index(node.id.resource))].imported;
            auto kind = data.resources[usize(resource_index(node.id.resource))].kind;
            for (auto written : writes[usize(v)]) {
                Vec<ResourceRange> remainder;
                for (auto part : uncovered) {
                    if (! node.available && overlaps(kind, part, written)) return false;
                    auto pieces = subtract_range(kind, part, written);
                    remainder.extend_from_slice(pieces.as_slice());
                }
                uncovered = rstd::move(remainder);
            }
            if (uncovered.is_empty()) return true;
            if (! node.inherits || ! node.previous) return false;
            v = version_index(*node.previous);
        }
        return false;
    };
    FrameGraphPlan  plan;
    Vec<Vec<Index>> predecessors;
    for (usize i {}; i < data.passes.len(); ++i) {
        plan.successors.push(Vec<Index> {});
        predecessors.push(Vec<Index> {});
    }
    HashMap<u64, Index> edges;
    auto                edge = [&](Index before, Index after, DependencySource source) -> bool {
        auto key      = u64((Serial(before) << 32) | after);
        auto existing = edges.get(key);
        if (existing) {
            plan.dependencies[usize(**existing)].sources |= Index(source);
            return true;
        }
        (void)edges.insert(key, Index(plan.dependencies.len().to_primitive()));
        if (edges.len().to_primitive() > limits.dependencies) return false;
        plan.dependencies.push({ before, after, Index(source) });
        plan.successors[usize(before)].push(Index(after));
        predecessors[usize(after)].push(Index(before));
        return true;
    };
    for (auto d : data.dependencies) {
        auto before = pass_index(d.before), after = pass_index(d.after);
        if (before == invalid_index || after == invalid_index)
            return Err(Error { ErrorKind::MissingPass });
        if (! edge(before, after, DependencySource::Explicit))
            return Err(Error { ErrorKind::Limit });
    }
    Vec<Index> roots;
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) roots.push(Index(i));
    auto root = [&](Index i) {
        while (roots[usize(i)] != i) i = roots[usize(i)];
        return i;
    };
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto& v = data.versions[usize(i)];
        const auto& r = data.resources[usize(resource_index(v.id.resource))];
        if (v.previous && (v.same_storage || r.storage))
            roots[usize(root(i))] = root(version_index(*v.previous));
        if (r.storage)
            for (Index j {}; j < i; ++j) {
                const auto& other =
                    data.resources[usize(resource_index(data.versions[usize(j)].id.resource))];
                if (other.storage == r.storage) roots[usize(root(i))] = root(j);
            }
        if (v.inherits && v.previous) {
            const auto& previous = data.versions[usize(version_index(*v.previous))];
            if (previous.writer && *previous.writer != *v.writer &&
                ! edge(pass_index(*previous.writer),
                       pass_index(*v.writer),
                       DependencySource::Contents))
                return Err(Error { ErrorKind::Limit });
        }
    }
    for (Index p {}; p < data.passes.len().to_primitive(); ++p)
        for (const auto& use : data.passes[usize(p)].uses) {
            auto        v    = version_index(use.version);
            const auto& node = data.versions[usize(v)];
            if (use.access == Access::Read) {
                if (use.requires_contents && ! available(use.version, use.range))
                    return Err(Error {
                        ErrorKind::Undefined, data.passes[usize(p)].id, use.version.resource });
                if (node.writer) {
                    auto before = pass_index(*node.writer);
                    if (before == p)
                        return Err(Error {
                            ErrorKind::Conflict, data.passes[usize(p)].id, use.version.resource });
                    if (! edge(before, p, DependencySource::Contents))
                        return Err(Error { ErrorKind::Limit });
                }
            }
        }

    struct StorageAccess {
        Index         pass;
        ResourceRange range;
    };
    HashMap<u64, Vec<StorageAccess>> frontiers;
    Vec<Index>                       ordered_versions;
    for (Index v {}; v < data.versions.len().to_primitive(); ++v) ordered_versions.push(Index(v));
    rstd::slice_::sort_unstable_by(ordered_versions.as_mut_slice().as_mut_ref(),
                                   [&](Index a, Index b) {
                                       const auto& left  = data.versions[usize(a)].id;
                                       const auto& right = data.versions[usize(b)].id;
                                       if (left.resource.value != right.resource.value)
                                           return left.resource.value < right.resource.value;
                                       return left.version < right.version;
                                   });
    for (auto v : ordered_versions) {
        const auto& node  = data.versions[usize(v)];
        auto        r     = resource_index(node.id.resource);
        auto        kind  = data.resources[usize(r)].kind;
        auto        key   = u64((Serial(r) << 32) | root(v));
        auto        found = frontiers.get_mut(key);
        if (found.is_none()) {
            (void)frontiers.insert(u64(key), Vec<StorageAccess> {});
            found = frontiers.get_mut(key);
        }
        auto& frontier = **found;
        if (node.writer) {
            auto p = pass_index(*node.writer);
            for (auto written : writes[usize(v)]) {
                Vec<StorageAccess> remaining;
                for (const auto& prior : frontier) {
                    if (p != prior.pass && overlaps(kind, prior.range, written) &&
                        ! edge(prior.pass, p, DependencySource::Storage))
                        return Err(Error { ErrorKind::Limit });
                    // The new writer orders covered accesses transitively; keep uncovered
                    // ranges so partial writes cannot hide outstanding storage hazards.
                    auto pieces = subtract_range(kind, prior.range, written);
                    for (auto range : pieces) remaining.push({ prior.pass, range });
                }
                remaining.push({ p, written });
                frontier = rstd::move(remaining);
            }
        }
        for (auto p : version_uses[usize(v)])
            for (const auto& use : data.passes[usize(p)].uses)
                if (use.access == Access::Read && use.version == node.id)
                    frontier.push({ p, use.range });
    }
    for (auto exported : data.exports) {
        auto r = resource_index(exported.resource), v = version_index(exported);
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, exported.resource });
        if (! available(exported, full_range(data.resources[usize(r)])))
            return Err(Error { ErrorKind::Undefined, {}, exported.resource });
    }
    Vec<Index> incoming, positions, batches;
    for (Index i {}; i < data.passes.len().to_primitive(); ++i) {
        incoming.push(Index(predecessors[usize(i)].len().to_primitive()));
        positions.push(Index(invalid_index));
        batches.push(Index(invalid_index));
    }
    Vec<Index> ready;
    for (Index i {}; i < incoming.len().to_primitive(); ++i)
        if (! incoming[usize(i)]) ready.push(Index(i));
    while (! ready.is_empty()) {
        usize chosen {};
        for (usize i { 1 }; i < ready.len(); ++i)
            if (ready[i] < ready[chosen]) chosen = i;
        auto current = ready.remove(chosen);
        plan.order.push(Index(current));
        for (auto next : plan.successors[usize(current)])
            if (--incoming[usize(next)] == 0) ready.push(Index(next));
    }
    if (plan.order.len() != data.passes.len()) return Err(Error { ErrorKind::Cycle });
    HashMap<u32, Index> last_queue;
    for (usize position {}; position < plan.order.len(); ++position) {
        auto p              = plan.order[position];
        positions[usize(p)] = Index(position.to_primitive());
        auto previous       = last_queue.get(u32(data.passes[usize(p)].queue.value));
        if (previous && ! edge(**previous, p, DependencySource::Queue))
            return Err(Error { ErrorKind::Limit });
        (void)last_queue.insert(u32(data.passes[usize(p)].queue.value), Index(p));
        ExecutionBatch batch { data.passes[usize(p)].queue };
        batch.passes.push(Index(p));
        for (auto before : predecessors[usize(p)]) batch.waits.push(Index(batches[usize(before)]));
        batches[usize(p)] = Index(plan.batches.len().to_primitive());
        plan.batches.push(rstd::move(batch));
    }
    for (Index p {}; p < data.passes.len().to_primitive(); ++p)
        for (const auto& use : data.passes[usize(p)].uses) {
            const auto& resource = data.resources[usize(resource_index(use.version.resource))];
            if (! resource.storage) continue;
            for (Index q {}; q <= p; ++q)
                for (const auto& other : data.passes[usize(q)].uses) {
                    if (use.version.resource == other.version.resource ||
                        (use.access == Access::Read && other.access == Access::Read))
                        continue;
                    const auto& other_resource =
                        data.resources[usize(resource_index(other.version.resource))];
                    if (other_resource.storage != resource.storage) continue;
                    if (resource.kind != other_resource.kind ||
                        (overlaps(resource.kind, use.range, other.range) && ! plan.precedes(p, q) &&
                         ! plan.precedes(q, p)))
                        return Err(Error {
                            ErrorKind::Conflict, data.passes[usize(p)].id, use.version.resource });
                }
        }
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto&     node = data.versions[usize(i)];
        const auto&     r    = data.resources[usize(resource_index(node.id.resource))];
        VersionLifetime life { .version = node.id, .retained = r.imported || r.retained };
        for (auto exported : data.exports) life.retained |= exported == node.id;
        for (auto p : version_uses[usize(i)]) {
            life.uses.push(Index(p));
            life.first = rstd::cmp::min(life.first, positions[usize(p)]);
            life.last  = rstd::cmp::max(life.last, positions[usize(p)]);
        }
        plan.lifetimes.push(rstd::move(life));
    }
    struct Group {
        Index      root, slot;
        Serial     allocation_class;
        Vec<Index> uses;
        bool       retained {};
    };
    Vec<Group> groups;
    for (auto p : plan.order)
        for (const auto& use : data.passes[usize(p)].uses) {
            auto  v = version_index(use.version), owner = root(v);
            Index group = invalid_index;
            for (Index i {}; i < groups.len().to_primitive(); ++i)
                if (groups[usize(i)].root == owner) group = i;
            if (group == invalid_index) {
                const auto& r = data.resources[usize(resource_index(use.version.resource))];
                group         = Index(groups.len().to_primitive());
                groups.push({ owner, invalid_index, r.allocation_class, {} });
            }
            auto& value = groups[usize(group)];
            value.retained |= plan.lifetimes[usize(v)].retained;
            bool seen {};
            for (auto previous : value.uses) seen |= previous == p;
            if (! seen) value.uses.push(Index(p));
        }
    Index next_slot {};
    for (usize i {}; i < groups.len(); ++i) {
        auto& group = groups[i];
        if (! group.retained && group.allocation_class)
            for (usize j {}; j < i; ++j) {
                const auto& previous = groups[j];
                if (previous.retained || previous.allocation_class != group.allocation_class)
                    continue;
                bool safe = true;
                for (usize k {}; k < i; ++k)
                    if (groups[k].slot == previous.slot)
                        for (auto before : groups[k].uses)
                            for (auto after : group.uses) safe &= plan.precedes(before, after);
                if (safe) {
                    group.slot = previous.slot;
                    break;
                }
            }
        if (group.slot == invalid_index) group.slot = next_slot++;
    }
    for (Index v {}; v < data.versions.len().to_primitive(); ++v) {
        auto  owner = root(v);
        Index slot  = invalid_index;
        for (const auto& group : groups)
            if (group.root == owner) slot = group.slot;
        if (slot == invalid_index) slot = next_slot++;
        plan.allocations.push({ data.versions[usize(v)].id, slot });
    }
    return Ok(rstd::move(plan));
}
} // namespace vrento::fg
