export module vrento:pass.resources;
import rstd;
import :frame_graph;
export import :pass.content;
import :resource;
import :texture.core;

using namespace rstd::prelude;

export namespace vrento::pass
{
namespace fg       = vrento::fg;
namespace resource = vrento::resource;

struct TextureSpec {
    String                       key;
    bool                         imported {};
    resource::TextureResourceRef definition;
    auto request() const -> const resource::TextureRequest& { return definition.definition(); }
    Option<String>                       allocation_family;
    Option<vrento::texture::TextureDesc> metadata;
    auto                                 clone() const -> TextureSpec {
        return { key.clone(), imported, definition.clone(), allocation_family.clone(), metadata };
    }
};
struct TextureUse {
    fg::ResourceVersion        version;
    resource::TextureUseHandle handle;
};
struct GraphTexture {
    fg::ResourceId id;
    TextureSpec    spec;
};
struct BufferUse {
    resource::BufferUseHandle use;
    fg::ResourceVersion       version;
};
struct TextureBinding {
    TextureUse        resource;
    fg::ResourceRange range;
    ContentRef        content;
};
struct BufferBinding {
    BufferUse         resource;
    fg::ResourceRange range;
    ContentRef        content;
};
struct GraphResourceMap {
    fg::Serial           scope {};
    Vec<TextureBinding>  textures;
    Vec<BufferBinding>   buffers;
    Vec<fg::ResourceUse> externals;
    auto                 same_bindings(const GraphResourceMap& other) const -> bool {
        if (scope != other.scope || textures.len() != other.textures.len() ||
            buffers.len() != other.buffers.len() || externals.len() != other.externals.len())
            return false;
        auto same_content = [](const ContentRef& a, const ContentRef& b) {
            if (a.version() != b.version()) return false;
            auto at = a.texture(), bt = b.texture();
            if (at.is_some() != bt.is_some()) return false;
            if (at && (at->id() != bt->id() ||
                       at->definition().definition != bt->definition().definition ||
                       at->definition().kind != bt->definition().kind))
                return false;
            auto ab = a.buffer(), bb = b.buffer();
            if (ab.is_some() != bb.is_some()) return false;
            return ! ab || (ab->id() == bb->id() &&
                            ab->definition().definition == bb->definition().definition);
        };
        for (usize i {}; i < textures.len(); ++i) {
            const auto& a = textures[i];
            const auto& b = other.textures[i];
            if (a.resource.version != b.resource.version ||
                a.resource.handle != b.resource.handle || a.range != b.range ||
                ! same_content(a.content, b.content))
                return false;
        }
        for (usize i {}; i < buffers.len(); ++i) {
            const auto& a = buffers[i];
            const auto& b = other.buffers[i];
            if (a.resource.version != b.resource.version || a.resource.use != b.resource.use ||
                a.range != b.range || ! same_content(a.content, b.content))
                return false;
        }
        for (usize i {}; i < externals.len(); ++i) {
            const auto& a = externals[i];
            const auto& b = other.externals[i];
            if (a.version != b.version || a.range != b.range || a.access != b.access ||
                a.requires_contents != b.requires_contents)
                return false;
        }
        return true;
    }
    auto validate(const ContentRef& content) const -> Result<empty, fg::Error> {
        auto version = content.resolve(scope);
        if (version.is_err()) return Err(version.unwrap_err());
        auto texture = content.texture();
        auto buffer  = content.buffer();
        bool found {};
        for (const auto& binding : textures)
            if (binding.resource.version == *version) {
                auto definition = binding.content.texture();
                if (buffer || ! definition || (texture && texture->id() != definition->id()) ||
                    binding.content.version() != *version)
                    return Err(fg::Error { fg::ErrorKind::Conflict, {}, version->resource });
                found = true;
            }
        for (const auto& binding : buffers)
            if (binding.resource.version == *version) {
                auto definition = binding.content.buffer();
                if (texture || ! definition || (buffer && buffer->id() != definition->id()) ||
                    binding.content.version() != *version)
                    return Err(fg::Error { fg::ErrorKind::Conflict, {}, version->resource });
                found = true;
            }
        for (const auto& binding : externals)
            if (binding.version == *version && ! texture && ! buffer) found = true;
        if (! found)
            return Err(fg::Error { fg::ErrorKind::MissingResource, {}, version->resource });
        return Ok(empty {});
    }
};
class GraphResources {
    u64               generation_;
    Vec<GraphTexture> textures_;
    Vec<TextureUse>   uses_;
    Vec<BufferUse>    buffers_;

public:
    explicit GraphResources(u64 generation): generation_(generation) {}
    auto content(const fg::FrameGraphBuilder& graph, fg::ResourceVersion version) const
        -> Result<ContentRef, fg::Error> {
        if (auto texture = find(version.resource))
            return ContentRef::bind(graph, version, Some(texture->spec.definition.clone()));
        return ContentRef::bind(graph, version);
    }
    auto initial(fg::FrameGraphBuilder& graph, TextureSpec spec) -> Result<ContentRef, fg::Error> {
        auto version = declare(graph, rstd::move(spec));
        if (version.is_err()) return Err(version.unwrap_err());
        return content(graph, *version);
    }
    auto output(fg::FrameGraphBuilder& graph, fg::PassId pass, const ContentRef& previous,
                bool reuse = false, bool inherits = false) -> Result<ContentRef, fg::Error> {
        return ContentRef::write(graph, pass, previous, reuse, inherits);
    }
    auto handle(const ContentRef& content) -> Result<resource::TextureUseHandle, fg::Error> {
        auto version = content.resolve(generation_.to_primitive());
        return version.is_ok() ? handle(*version) : Err(version.unwrap_err());
    }
    auto content(const fg::FrameGraphBuilder& graph, resource::TextureUseHandle use) const
        -> Result<ContentRef, fg::Error> {
        auto version = this->version(use);
        if (! version) return Err(fg::Error { fg::ErrorKind::MissingResource });
        return content(graph, *version);
    }
    auto find(const ContentRef& content) const -> const GraphTexture* {
        auto version = content.resolve(generation_.to_primitive());
        return version.is_ok() ? find(version->resource) : nullptr;
    }
    auto shape(const fg::FrameGraphBuilder& graph, const ContentRef& content) const
        -> const fg::ResourceDescription* {
        auto version = content.resolve(graph.data().scope);
        return version.is_ok() ? graph.find_resource(version->resource) : nullptr;
    }
    auto import_history(fg::FrameGraphBuilder& graph, const ContentRef& content)
        -> Result<empty, fg::Error> {
        auto version = content.resolve(graph.data().scope);
        if (version.is_err()) return Err(version.unwrap_err());
        if (! content.initial()) return Ok(empty {});
        return graph.import_initial(version->resource);
    }
    auto reuse_storage(fg::FrameGraphBuilder& graph, const ContentRef& content)
        -> Result<empty, fg::Error> {
        auto version = content.resolve(graph.data().scope);
        if (version.is_err()) return Err(version.unwrap_err());
        return graph.reuse_storage(*version);
    }
    auto buffers() const -> slice<BufferUse> { return buffers_.as_slice(); }
    auto declare_buffers(fg::FrameGraphBuilder& graph, slice<resource::BufferPlanEntry> entries)
        -> Result<empty, fg::Error> {
        if (graph.data().scope != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        fg::Serial next { 1 };
        for (const auto& resource : graph.data().resources)
            next = rstd::cmp::max(next, resource.id.value + 1);
        for (const auto& entry : entries) {
            if (! entry.definition.valid())
                return Err(fg::Error { fg::ErrorKind::MissingResource });
            bool exists {};
            for (const auto& buffer : buffers_) exists |= buffer.use == entry.handle;
            if (exists || entry.handle.generation != generation_)
                return Err(fg::Error { fg::ErrorKind::Conflict });
            fg::ResourceId id { next++, graph.data().scope };
            auto           declared =
                graph.import_resource({ .id      = id,
                                        .kind    = fg::ResourceKind::Buffer,
                                        .bytes   = entry.request().definition.size.to_primitive(),
                                        .spec    = id.value,
                                        .storage = id.value,
                                        .name    = entry.request().name.clone() });
            if (declared.is_err()) return Err(declared.unwrap_err());
            buffers_.push({ entry.handle, declared.unwrap() });
        }
        return Ok(empty {});
    }
    auto version(resource::BufferUseHandle handle) const -> Option<fg::ResourceVersion> {
        for (const auto& buffer : buffers_)
            if (buffer.use == handle) return Some(fg::ResourceVersion(buffer.version));
        return None();
    }
    auto generation() const -> u64 { return generation_; }
    auto textures() const -> slice<GraphTexture> { return textures_.as_slice(); }
    auto uses() const -> slice<TextureUse> { return uses_.as_slice(); }
    auto find(fg::ResourceId id) const -> const GraphTexture* {
        for (const auto& entry : textures_)
            if (entry.id == id) return rstd::addressof(entry);
        return nullptr;
    }
    auto find(ref<str> name) const -> const GraphTexture* {
        for (const auto& entry : textures_)
            if (entry.spec.key == name) return rstd::addressof(entry);
        return nullptr;
    }
    auto logical(fg::ResourceId id) const -> Option<resource::TextureResourceRef> {
        auto resource = find(id);
        return resource ? Some(resource->spec.definition.clone())
                        : None<resource::TextureResourceRef>();
    }
    auto handle(fg::ResourceVersion version) -> Result<resource::TextureUseHandle, fg::Error> {
        if (! find(version.resource))
            return Err(fg::Error { fg::ErrorKind::MissingResource, {}, version.resource });
        for (const auto& use : uses_)
            if (use.version == version) return Ok(use.handle);
        resource::TextureUseHandle result { rstd::as_cast<u64>(uses_.len()), generation_ };
        uses_.push({ version, result });
        return Ok(result);
    }
    auto version(resource::TextureUseHandle handle) const -> Option<fg::ResourceVersion> {
        if (handle.generation != generation_) return None();
        for (const auto& use : uses_)
            if (use.handle == handle) return Some(fg::ResourceVersion(use.version));
        return None();
    }
    auto resourceUses(const fg::PassNode& pass) const -> Vec<resource::TextureUseHandle> {
        Vec<resource::TextureUseHandle> result;
        for (const auto& use : uses_) {
            for (const auto& access : pass.uses)
                if (access.version == use.version) {
                    result.push(resource::TextureUseHandle(use.handle));
                    break;
                }
        }
        return result;
    }
    auto declare(fg::FrameGraphBuilder& graph, TextureSpec spec)
        -> Result<fg::ResourceVersion, fg::Error> {
        if (graph.data().scope != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        if (! spec.definition.valid()) return Err(fg::Error { fg::ErrorKind::MissingResource });
        if (spec.request().name != spec.key.as_str()) {
            auto request    = spec.request().clone();
            request.name    = spec.key.clone();
            spec.definition = spec.definition.with_definition(rstd::move(request));
        }
        if (auto existing = find(spec.key.as_str()); existing) {
            if (existing->spec.imported != spec.imported ||
                existing->spec.metadata != spec.metadata ||
                existing->spec.allocation_family != spec.allocation_family ||
                ! resource::SameTextureRequest(existing->spec.request(), spec.request()))
                return Err(fg::Error { fg::ErrorKind::Conflict, {}, existing->id });
            return Ok(fg::ResourceVersion { existing->id, 0 });
        }
        fg::Serial next { 1 };
        for (const auto& resource : graph.data().resources)
            next = rstd::cmp::max(next, resource.id.value + 1);
        fg::ResourceId id { next, generation_.to_primitive() };
        fg::Serial     allocation_class {};
        if (spec.allocation_family &&
            spec.request().lifetime == resource::TextureLifetimeClass::FrameLocal) {
            for (const auto& entry : textures_) {
                const auto& other = entry.spec;
                if (other.allocation_family == spec.allocation_family && other.definition.valid() &&
                    other.request().kind == spec.request().kind &&
                    other.request().definition == spec.request().definition &&
                    other.request().lifetime == spec.request().lifetime &&
                    other.request().content == spec.request().content) {
                    allocation_class = graph.find_resource(entry.id)->allocation_class;
                    break;
                }
            }
            if (! allocation_class) allocation_class = id.value;
        }
        fg::ResourceDescription description {
            .id               = id,
            .spec             = id.value,
            .allocation_class = allocation_class,
            .storage          = spec.allocation_family ? 0 : id.value,
            .retained = spec.request().lifetime != resource::TextureLifetimeClass::FrameLocal,
            .name     = spec.key.clone()
        };
        if (spec.request().definition) {
            auto mips              = spec.request().definition->mip_levels.to_primitive();
            description.mip_levels = mips ? mips : 1;
            if (resource::HasTextureUsage(spec.request().definition->usage,
                                          resource::TextureUsage::DepthAttachment))
                description.aspects = 2;
        }
        if (spec.metadata) {
            description.mip_levels = spec.metadata->mip_levels;
            description.layers     = spec.metadata->layers;
            description.aspects    = vrento::texture::format_layout(spec.metadata->format).aspect ==
                                             vrento::texture::Aspect::Depth
                                         ? 2
                                         : 1;
        }
        auto declared = spec.imported ? graph.import_resource(rstd::move(description))
                                      : graph.create_resource(rstd::move(description));
        if (declared.is_err()) return declared;
        textures_.push({ id, rstd::move(spec) });
        return declared;
    }
    auto bindings(const fg::FrameGraph& graph, const resource::ResourcePlan& plan) const
        -> Result<GraphResourceMap, fg::Error> {
        if (plan.generation != generation_) return Err(fg::Error { fg::ErrorKind::Conflict });
        auto valid = validate(graph);
        if (valid.is_err()) return Err(valid.unwrap_err());
        GraphResourceMap result { .scope = graph.scope() };
        for (const auto& pass : graph.passes())
            for (const auto& access : pass.uses) {
                bool found {};
                for (const auto& texture : uses_)
                    if (texture.version == access.version) {
                        for (const auto& entry : plan.textures)
                            if (entry.handle == texture.handle) {
                                auto content = ContentRef::bind(
                                    graph, access.version, Some(entry.definition.clone()));
                                if (content.is_err()) return Err(content.unwrap_err());
                                result.textures.push({ texture, access.range, content.unwrap() });
                                found = true;
                            }
                        if (! found)
                            return Err(fg::Error {
                                fg::ErrorKind::MissingResource, pass.id, access.version.resource });
                    }
                for (const auto& buffer : buffers_)
                    if (buffer.version.resource == access.version.resource) {
                        for (const auto& entry : plan.buffers)
                            if (entry.handle == buffer.use) {
                                auto content = ContentRef::bind(
                                    graph, access.version, None(), Some(entry.definition.clone()));
                                if (content.is_err()) return Err(content.unwrap_err());
                                result.buffers.push({ { buffer.use, access.version },
                                                      access.range,
                                                      content.unwrap() });
                                found = true;
                            }
                        if (! found)
                            return Err(fg::Error {
                                fg::ErrorKind::MissingResource, pass.id, access.version.resource });
                    }
                if (! found) {
                    auto resource = graph.find_resource(access.version.resource);
                    if (! resource || ! resource->imported)
                        return Err(fg::Error {
                            fg::ErrorKind::MissingResource, pass.id, access.version.resource });
                    result.externals.push(fg::ResourceUse(access));
                }
            }
        return Ok(rstd::move(result));
    }
    auto validate(const fg::FrameGraph& graph) const -> Result<empty, fg::Error> {
        if (graph.scope() != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        for (const auto& use : uses_) {
            const auto* texture = find(use.version.resource);
            if (! texture || ! texture->spec.definition.valid() ||
                ! graph.find_version(use.version) || ! graph.find_resource(use.version.resource))
                return Err(fg::Error { fg::ErrorKind::MissingResource, {}, use.version.resource });
        }
        return Ok(empty {});
    }
    auto resourcePlan(const fg::FrameGraph& graph) const
        -> Result<resource::ResourcePlan, fg::Error> {
        auto valid = validate(graph);
        if (valid.is_err()) return Err(valid.unwrap_err());
        resource::ResourcePlan result { .generation = generation_ };
        for (const auto& use : uses_) {
            const auto* texture  = find(use.version.resource);
            const auto* version  = graph.find_version(use.version);
            const auto* resource = graph.find_resource(use.version.resource);
            auto        request  = texture->spec.request().clone();
            if (resource->imported && ! texture->spec.imported) {
                request.lifetime = resource::TextureLifetimeClass::Retained;
                request.content |=
                    resource::TextureContentFlag(resource::TextureContent::PreserveAcrossFrames);
            }
            auto key = request.name.clone();
            if (texture->spec.allocation_family &&
                request.lifetime == resource::TextureLifetimeClass::FrameLocal)
                for (auto allocation : graph.inspect().allocations)
                    if (allocation.version == use.version)
                        key = rstd::format("{}::slot_{}",
                                           texture->spec.allocation_family->as_str(),
                                           allocation.slot);
            bool read {};
            for (const auto& pass : graph.passes())
                for (const auto& access : pass.uses)
                    read |= access.version == use.version && access.access == fg::Access::Read;
            result.textures.push(
                { .handle     = use.handle,
                  .definition = resource::SameTextureRequest(request, texture->spec.request())
                                    ? texture->spec.definition.clone()
                                    : texture->spec.definition.with_definition(rstd::move(request)),
                  .allocation_key = rstd::move(key),
                  .access         = version->writer ? (read ? resource::ResourceAccess::ReadWrite
                                                            : resource::ResourceAccess::Write)
                                                    : resource::ResourceAccess::Read,
                  .version        = u32(use.version.version) });
        }
        return Ok(rstd::move(result));
    }
};
} // namespace vrento::pass
