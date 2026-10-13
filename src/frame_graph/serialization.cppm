export module vrento:frame_graph.serialization;
export import :frame_graph.graph;
import rstd;

namespace vrento::fg
{
class FrameGraphEncoder {
    Vec<rstd::uint8_t> bytes_;
    Serial             limit_;
    bool               valid_ { true };

public:
    explicit FrameGraphEncoder(Serial limit): limit_(limit) {}
    void integer(Serial value, Index width) {
        if (bytes_.len().to_primitive() > limit_ || width > limit_ - bytes_.len().to_primitive()) {
            valid_ = false;
            return;
        }
        for (Index i {}; i < width; ++i) bytes_.push(rstd::uint8_t(value >> (i * 8)));
    }
    void id(ResourceId value) {
        integer(value.value, 8);
        integer(value.generation, 8);
    }
    void version(ResourceVersion value) {
        id(value.resource);
        integer(value.version, 4);
    }
    void name(const String& value) {
        integer(value.len().to_primitive(), 4);
        for (auto byte : value->as_bytes()) integer(byte.to_primitive(), 1);
    }
    auto finish() -> Result<Vec<rstd::uint8_t>, Error> {
        if (! valid_) return Err(Error { ErrorKind::Limit });
        return Ok(rstd::move(bytes_));
    }
};
class FrameGraphDecoder {
    slice<rstd::uint8_t> bytes_;
    usize                offset_ {};
    bool                 valid_ { true };

public:
    explicit FrameGraphDecoder(slice<rstd::uint8_t> bytes): bytes_(bytes) {}
    auto integer(Index width) -> Serial {
        if (width > 8 || usize(width) > bytes_.len() - offset_) {
            valid_ = false;
            return 0;
        }
        Serial result {};
        for (Index i {}; i < width; ++i) result |= Serial(bytes_[offset_++]) << (i * 8);
        return result;
    }
    auto id() -> ResourceId { return { integer(8), integer(8) }; }
    auto version() -> ResourceVersion {
        auto resource = id();
        return { resource, Index(integer(4)) };
    }
    auto flag() -> bool {
        auto value = integer(4);
        if (value > 1) valid_ = false;
        return value == 1;
    }
    auto name(Serial& names, Limits limits) -> Result<String, Error> {
        auto count = integer(4);
        if (! valid_ || count > remaining()) return Err(Error { ErrorKind::Encoding });
        if (names > limits.name_bytes || count > limits.name_bytes - names)
            return Err(Error { ErrorKind::Limit });
        names += count;
        Vec<u8> bytes;
        for (Serial i {}; i < count; ++i) bytes.push(u8(integer(1)));
        auto result = String::from_utf8(rstd::move(bytes));
        if (result.is_err()) return Err(Error { ErrorKind::Encoding });
        return Ok(result.unwrap());
    }
    auto remaining() const -> Serial { return (bytes_.len() - offset_).to_primitive(); }
    bool valid() const { return valid_; }
};
} // namespace vrento::fg

export namespace vrento::fg
{
inline auto encode_frame_graph(const FrameGraph& graph, Limits limits = {})
    -> Result<Vec<rstd::uint8_t>, Error> {
    auto checked = graph.compile(limits);
    if (checked.is_err()) return Err(checked.unwrap_err());
    FrameGraphEncoder out(limits.encoded_bytes);
    out.integer(0x47465256, 4);
    out.integer(1, 4);
    out.integer(graph.scope(), 8);
    out.integer(graph.resources().len().to_primitive(), 4);
    out.integer(graph.versions().len().to_primitive(), 4);
    out.integer(graph.passes().len().to_primitive(), 4);
    out.integer(graph.dependencies().len().to_primitive(), 4);
    out.integer(graph.exports().len().to_primitive(), 4);
    for (const auto& resource : graph.resources()) {
        out.id(resource.id);
        out.integer(Index(resource.kind), 4);
        out.integer(resource.bytes, 8);
        out.integer(resource.mip_levels, 4);
        out.integer(resource.layers, 4);
        out.integer(resource.aspects, 4);
        out.integer(resource.spec, 8);
        out.integer(resource.allocation_class, 8);
        out.integer(resource.storage, 8);
        out.integer(resource.imported, 4);
        out.integer(resource.retained, 4);
        out.name(resource.name);
    }
    for (const auto& version : graph.versions()) {
        out.version(version.id);
        out.integer(version.writer ? version.writer->value : 0, 8);
        out.integer(version.previous.is_some(), 4);
        out.version(version.previous ? *version.previous : ResourceVersion {});
        out.integer(version.inherits, 4);
        out.integer(version.same_storage, 4);
        out.integer(version.available, 4);
    }
    for (const auto& pass : graph.passes()) {
        out.integer(pass.id.value, 8);
        out.integer(pass.queue.value, 4);
        out.name(pass.name);
        out.integer(pass.uses.len().to_primitive(), 4);
        for (const auto& use : pass.uses) {
            out.integer(use.id, 8);
            out.version(use.version);
            out.integer(Index(use.access), 4);
            out.integer(use.range.offset, 8);
            out.integer(use.range.size, 8);
            out.integer(use.range.aspect, 4);
            out.integer(use.range.first_mip, 4);
            out.integer(use.range.mip_count, 4);
            out.integer(use.range.first_layer, 4);
            out.integer(use.range.layer_count, 4);
            out.integer(use.requires_contents, 4);
        }
    }
    for (auto dependency : graph.dependencies()) {
        out.integer(dependency.before.value, 8);
        out.integer(dependency.after.value, 8);
    }
    for (auto output : graph.exports()) out.version(output);
    return out.finish();
}
inline auto decode_frame_graph(slice<rstd::uint8_t> bytes, Limits limits = {})
    -> Result<FrameGraph, Error> {
    if (bytes.len().to_primitive() > limits.encoded_bytes) return Err(Error { ErrorKind::Limit });
    FrameGraphDecoder in(bytes);
    if (in.integer(4) != 0x47465256 || in.integer(4) != 1) return Err(Error { ErrorKind::Schema });
    FrameGraphData data { .scope = in.integer(8) };
    auto           resources = in.integer(4), versions = in.integer(4), passes = in.integer(4),
                   dependencies = in.integer(4), exports = in.integer(4);
    if (! in.valid()) return Err(Error { ErrorKind::Encoding });
    if (resources > limits.resources || versions > limits.versions || passes > limits.passes ||
        dependencies > limits.dependencies || exports > limits.versions)
        return Err(Error { ErrorKind::Limit });
    if (resources * 76 + versions * 64 + passes * 20 + dependencies * 16 + exports * 20 >
        in.remaining())
        return Err(Error { ErrorKind::Encoding });
    Serial names {}, uses {};
    for (Serial i {}; i < resources; ++i) {
        ResourceDescription resource;
        resource.id               = in.id();
        resource.kind             = ResourceKind(in.integer(4));
        resource.bytes            = in.integer(8);
        resource.mip_levels       = Index(in.integer(4));
        resource.layers           = Index(in.integer(4));
        resource.aspects          = Index(in.integer(4));
        resource.spec             = in.integer(8);
        resource.allocation_class = in.integer(8);
        resource.storage          = in.integer(8);
        resource.imported         = in.flag();
        resource.retained         = in.flag();
        auto name                 = in.name(names, limits);
        if (name.is_err()) return Err(name.unwrap_err());
        resource.name = name.unwrap();
        data.resources.push(rstd::move(resource));
    }
    for (Serial i {}; i < versions; ++i) {
        VersionNode node;
        node.id     = in.version();
        auto writer = in.integer(8);
        if (writer) node.writer = Some(PassId { writer });
        auto present  = in.flag();
        auto previous = in.version();
        if (present)
            node.previous = Some(previous);
        else if (previous != ResourceVersion {})
            return Err(Error { ErrorKind::Encoding });
        node.inherits     = in.flag();
        node.same_storage = in.flag();
        node.available    = in.flag();
        data.versions.push(rstd::move(node));
    }
    for (Serial i {}; i < passes; ++i) {
        PassNode pass;
        pass.id    = { in.integer(8) };
        pass.queue = { Index(in.integer(4)) };
        auto name  = in.name(names, limits);
        if (name.is_err()) return Err(name.unwrap_err());
        pass.name  = name.unwrap();
        auto count = in.integer(4);
        uses += count;
        if (uses > limits.uses) return Err(Error { ErrorKind::Limit });
        if (count > in.remaining() / 72) return Err(Error { ErrorKind::Encoding });
        for (Serial j {}; j < count; ++j) {
            ResourceUse use;
            use.id                = in.integer(8);
            use.version           = in.version();
            use.access            = Access(in.integer(4));
            use.range.offset      = in.integer(8);
            use.range.size        = in.integer(8);
            use.range.aspect      = Index(in.integer(4));
            use.range.first_mip   = Index(in.integer(4));
            use.range.mip_count   = Index(in.integer(4));
            use.range.first_layer = Index(in.integer(4));
            use.range.layer_count = Index(in.integer(4));
            use.requires_contents = in.flag();
            pass.uses.push(rstd::move(use));
        }
        data.passes.push(rstd::move(pass));
    }
    for (Serial i {}; i < dependencies; ++i) {
        auto before = in.integer(8);
        data.dependencies.push({ { before }, { in.integer(8) } });
    }
    for (Serial i {}; i < exports; ++i) data.exports.push(in.version());
    if (! in.valid() || in.remaining()) return Err(Error { ErrorKind::Encoding });
    return FrameGraph::create(rstd::move(data), limits);
}
} // namespace vrento::fg
