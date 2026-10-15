export module vrento:frame_graph.types;
import rstd;

using namespace rstd::prelude;

export namespace vrento::fg
{
using Index                          = rstd::uint32_t;
using Serial                         = rstd::uint64_t;
inline constexpr Index invalid_index = ~Index {};

struct ResourceId {
    Serial      value {}, generation {};
    bool        valid() const { return value && generation; }
    friend bool operator==(ResourceId, ResourceId) = default;
};
struct PassId {
    Serial      value {};
    bool        valid() const { return value != 0; }
    friend bool operator==(PassId, PassId) = default;
};
struct LogicalQueueId {
    Index       value {};
    friend bool operator==(LogicalQueueId, LogicalQueueId) = default;
};
struct ResourceVersion {
    ResourceId  resource;
    Index       version {};
    friend bool operator==(ResourceVersion, ResourceVersion) = default;
};
enum class ResourceKind : Index
{
    Texture,
    Buffer
};
enum class Access : Index
{
    Read,
    Write
};
struct ResourceRange {
    Serial      offset {}, size {};
    Index       aspect { 1 }, first_mip {}, mip_count { 1 }, first_layer {}, layer_count { 1 };
    friend bool operator==(ResourceRange, ResourceRange) = default;
};
struct ResourceDescription {
    ResourceId   id;
    ResourceKind kind { ResourceKind::Texture };
    Serial       bytes {};
    Index        mip_levels { 1 }, layers { 1 }, aspects { 1 };
    Serial       spec {}, allocation_class {}, storage {};
    bool         imported {}, retained {};
    String       name;
    auto         clone() const -> ResourceDescription {
        return { id,      kind,     bytes,    mip_levels,  layers, aspects, spec, allocation_class,
                 storage, imported, retained, name.clone() };
    }
};
struct ResourceUse {
    Serial          id {};
    ResourceVersion version;
    Access          access { Access::Read };
    ResourceRange   range;
    bool            requires_contents { true };
};
struct VersionNode {
    ResourceVersion         id;
    Option<PassId>          writer;
    Option<ResourceVersion> previous;
    bool                    inherits {}, same_storage {}, available { true };
};
struct PassNode {
    PassId           id;
    LogicalQueueId   queue;
    String           name;
    Vec<ResourceUse> uses;
    auto             clone() const -> PassNode { return { id, queue, name.clone(), uses.clone() }; }
};
struct Dependency {
    PassId      before, after;
    friend bool operator==(Dependency, Dependency) = default;
};
struct FrameGraphData {
    Serial                   scope {};
    Vec<ResourceDescription> resources;
    Vec<VersionNode>         versions;
    Vec<PassNode>            passes;
    Vec<Dependency>          dependencies;
    Vec<ResourceVersion>     exports;
    auto                     clone() const -> FrameGraphData {
        FrameGraphData result { .scope = scope };
        for (const auto& resource : resources) result.resources.push(resource.clone());
        result.versions = versions.clone();
        for (const auto& pass : passes) result.passes.push(pass.clone());
        result.dependencies = dependencies.clone();
        result.exports      = exports.clone();
        return result;
    }
};
struct Limits {
    Index resources { 16384 }, versions { 65536 }, passes { 4096 }, uses { 65536 },
        dependencies { 262144 };
    Serial name_bytes { 1024 * 1024 }, encoded_bytes { 32 * 1024 * 1024 };
};
enum class ErrorKind
{
    Invalid,
    Limit,
    Duplicate,
    MissingResource,
    MissingPass,
    MissingProducer,
    Range,
    Conflict,
    Cycle,
    Undefined,
    Schema,
    Encoding
};
struct Error {
    ErrorKind  kind;
    PassId     pass {};
    ResourceId resource {};
};
struct ExecutionBatch {
    LogicalQueueId queue;
    Vec<Index>     passes, waits;
    auto clone() const -> ExecutionBatch { return { queue, passes.clone(), waits.clone() }; }
};
struct VersionLifetime {
    ResourceVersion version;
    Vec<Index>      uses;
    Index           first { invalid_index }, last {};
    bool            retained {};
    auto            clone() const -> VersionLifetime {
        return { version, uses.clone(), first, last, retained };
    }
};
struct AllocationSlot {
    ResourceVersion version;
    Index           slot {};
};
enum class DependencySource : Index
{
    Explicit = 1,
    Contents = 2,
    Storage  = 4,
    Queue    = 8
};
struct DependencyInfo {
    Index before {}, after {}, sources {};
};
struct FrameGraphPlan {
    Vec<Index>           order;
    Vec<ExecutionBatch>  batches;
    Vec<VersionLifetime> lifetimes;
    Vec<AllocationSlot>  allocations;
    Vec<DependencyInfo>  dependencies;
    Vec<Vec<Index>>      successors;
    auto                 clone() const -> FrameGraphPlan {
        FrameGraphPlan result { .order        = order.clone(),
                                .allocations  = allocations.clone(),
                                .dependencies = dependencies.clone() };
        for (const auto& batch : batches) result.batches.push(batch.clone());
        for (const auto& life : lifetimes) result.lifetimes.push(life.clone());
        for (const auto& next : successors) result.successors.push(next.clone());
        return result;
    }
    bool precedes(Index before, Index after) const {
        if (before >= successors.len().to_primitive() || after >= successors.len().to_primitive() ||
            before == after)
            return false;
        Vec<Index> pending;
        Vec<bool>  seen;
        seen.resize(successors.len(), false);
        pending.push(rstd::move(before));
        seen[usize(before)] = true;
        for (usize i {}; i < pending.len(); ++i)
            for (auto next : successors[usize(pending[i])]) {
                if (next == after) return true;
                if (! seen[usize(next)]) {
                    seen[usize(next)] = true;
                    pending.push(Index(next));
                }
            }
        return false;
    }
};

inline auto full_range(const ResourceDescription& resource) -> ResourceRange {
    if (resource.kind == ResourceKind::Buffer) return { .size = resource.bytes };
    return { .aspect      = resource.aspects,
             .mip_count   = resource.mip_levels,
             .layer_count = resource.layers };
}
inline bool valid_range(const ResourceDescription& resource, ResourceRange range) {
    if (resource.kind == ResourceKind::Buffer)
        return range.size && range.offset <= resource.bytes &&
               range.size <= resource.bytes - range.offset;
    return ! range.offset && ! range.size && range.aspect && ! (range.aspect & ~resource.aspects) &&
           range.mip_count && range.layer_count && range.first_mip < resource.mip_levels &&
           range.mip_count <= resource.mip_levels - range.first_mip &&
           range.first_layer < resource.layers &&
           range.layer_count <= resource.layers - range.first_layer;
}
inline bool overlaps(ResourceKind kind, ResourceRange a, ResourceRange b) {
    if (kind == ResourceKind::Buffer)
        return a.offset < b.offset + b.size && b.offset < a.offset + a.size;
    return (a.aspect & b.aspect) && a.first_mip < b.first_mip + b.mip_count &&
           b.first_mip < a.first_mip + a.mip_count &&
           a.first_layer < b.first_layer + b.layer_count &&
           b.first_layer < a.first_layer + a.layer_count;
}
inline auto subtract_range(ResourceKind kind, ResourceRange source, ResourceRange written)
    -> Vec<ResourceRange> {
    Vec<ResourceRange> result;
    if (! overlaps(kind, source, written)) {
        result.push(rstd::move(source));
        return result;
    }
    if (kind == ResourceKind::Buffer) {
        auto begin = rstd::cmp::max(source.offset, written.offset);
        auto end   = rstd::cmp::min(source.offset + source.size, written.offset + written.size);
        if (source.offset < begin)
            result.push({ .offset = source.offset, .size = begin - source.offset });
        if (end < source.offset + source.size)
            result.push({ .offset = end, .size = source.offset + source.size - end });
        return result;
    }
    auto separate = source.aspect & ~written.aspect;
    if (separate) {
        auto range   = source;
        range.aspect = separate;
        result.push(rstd::move(range));
    }
    source.aspect &= written.aspect;
    auto m0 = rstd::cmp::max(source.first_mip, written.first_mip);
    auto m1 =
        rstd::cmp::min(source.first_mip + source.mip_count, written.first_mip + written.mip_count);
    auto l0 = rstd::cmp::max(source.first_layer, written.first_layer);
    auto l1 = rstd::cmp::min(source.first_layer + source.layer_count,
                             written.first_layer + written.layer_count);
    if (source.first_mip < m0) {
        auto range      = source;
        range.mip_count = m0 - source.first_mip;
        result.push(rstd::move(range));
    }
    if (m1 < source.first_mip + source.mip_count) {
        auto range      = source;
        range.first_mip = m1;
        range.mip_count = source.first_mip + source.mip_count - m1;
        result.push(rstd::move(range));
    }
    source.first_mip = m0;
    source.mip_count = m1 - m0;
    if (source.first_layer < l0) {
        auto range        = source;
        range.layer_count = l0 - source.first_layer;
        result.push(rstd::move(range));
    }
    if (l1 < source.first_layer + source.layer_count) {
        auto range        = source;
        range.first_layer = l1;
        range.layer_count = source.first_layer + source.layer_count - l1;
        result.push(rstd::move(range));
    }
    return result;
}
} // namespace vrento::fg

export namespace rstd
{
template<>
struct Impl<Copy, vrento::fg::PassId> {};
template<>
struct Impl<hash::Hash, vrento::fg::PassId> : ImplBase<vrento::fg::PassId> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(u64(this->self().value), state);
    }
};
} // namespace rstd

export namespace rstd
{
template<>
struct Impl<fmt::Display, vrento::fg::Error> : ImplBase<vrento::fg::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& value = this->self();
        return formatter.write_str((rstd::format("frame graph error kind={} pass={} resource={}",
                                                 int(value.kind),
                                                 value.pass.value,
                                                 value.resource.value))
                                       .as_str());
    }
};
template<>
struct Impl<fmt::Debug, vrento::fg::Error> : ImplBase<vrento::fg::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, vrento::fg::Error> : DefaultInImpl<error::Error, vrento::fg::Error> {};
} // namespace rstd
