module;

export module vrento:resource_registry.texture_registry;
import rstd;
import :resource;
import :vulkan.api;
export import :resource_registry.texture;

using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::resource
{

struct Texture {
    resource::TextureHandle id;
    TextureResourceRef      definition;
    String                  allocation_key;
    bool                    preserve_contents {};
    auto request() const -> const TextureRequest& { return definition.definition(); }
    Option<ImportedTextureContentIdentity> imported_content;
    u64                                    definition_version { 1 };
    u64                                    content_version { 1 };

    auto clone() const -> Texture {
        return Texture {
            .id                = id,
            .definition        = definition.clone(),
            .allocation_key    = allocation_key.clone(),
            .preserve_contents = preserve_contents,
            .imported_content = imported_content.is_some() ? Some(imported_content->clone())
                                                           : None<ImportedTextureContentIdentity>(),
            .definition_version = definition_version,
            .content_version    = content_version,
        };
    }
};

struct TexturePhysical {
    Arc<resource_registry::TextureAllocation> allocation;
    u64                                       generation { 1 };
    u64                                       source_generation { 0 };
    u64                                       definition_version { 0 };
    u64                                       content_version { 0 };
    texture::SubmissionToken                  completion;

    auto clone() const -> TexturePhysical {
        return TexturePhysical {
            .allocation         = allocation.clone(),
            .generation         = generation,
            .source_generation  = source_generation,
            .definition_version = definition_version,
            .content_version    = content_version,
            .completion         = completion,
        };
    }
};

struct PendingTextureUpload {
    TextureHandle   handle;
    TexturePhysical physical;
};

struct TextureRegistryIdentity {
    TextureRequestKind                 kind { TextureRequestKind::Imported };
    String                             key;
    ResourceHandle<TextureResourceTag> logical;

    friend bool operator==(const TextureRegistryIdentity& lhs, const TextureRegistryIdentity& rhs) {
        return lhs.kind == rhs.kind && lhs.key == rhs.key.as_str() && lhs.logical == rhs.logical;
    }

    auto clone() const -> TextureRegistryIdentity {
        return TextureRegistryIdentity {
            .kind    = kind,
            .key     = key.clone(),
            .logical = logical,
        };
    }
};

} // namespace vrento::resource

export namespace rstd
{

template<>
struct Impl<hash::Hash, vrento::resource::TextureRegistryIdentity>
    : ImplBase<vrento::resource::TextureRegistryIdentity> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(u32(static_cast<uint32_t>(this->self().kind)), state);
        hash::hash_into(this->self().key, state);
        hash::hash_into(this->self().logical, state);
    }
};

template<>
struct Impl<hash::Hash, vrento::resource::ImportedTextureContentIdentity>
    : ImplBase<vrento::resource::ImportedTextureContentIdentity> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(this->self().key, state);
        hash::hash_into(this->self().revision, state);
        hash::hash_into(this->self().scope, state);
    }
};

} // namespace rstd

export namespace vrento::resource
{

class TextureRegistry {
public:
    TextureRegistry()                                  = default;
    TextureRegistry(const TextureRegistry&)            = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    auto Register(TextureRequest request) -> resource::TextureHandle {
        return Register(LegacyDefinition(rstd::move(request)));
    }
    auto Register(TextureResourceRef definition, Option<ref<str>> allocation_key = None())
        -> resource::TextureHandle {
        if (! definition.valid() || definition.definition().kind == TextureRequestKind::Imported)
            return {};
        return RegisterResource(rstd::move(definition), None(), allocation_key).unwrap_or({});
    }
    auto RegisterImported(TextureRequest request, ImportedTextureContentIdentity content)
        -> resource::TextureHandle {
        return RegisterImported(LegacyDefinition(rstd::move(request)), rstd::move(content));
    }
    auto RegisterImported(TextureResourceRef definition, ImportedTextureContentIdentity content,
                          Option<ref<str>> allocation_key = None()) -> resource::TextureHandle {
        if (! definition.valid() || definition.definition().kind != TextureRequestKind::Imported ||
            content.key.is_empty())
            return {};
        return RegisterResource(rstd::move(definition), Some(rstd::move(content)), allocation_key)
            .unwrap_or({});
    }

    auto RegisterResource(TextureResourceRef                     definition,
                          Option<ImportedTextureContentIdentity> content = None(),
                          Option<ref<str>> allocation_key = None(), bool preserve_contents = false)
        -> Result<TextureHandle, ResourceError> {
        if (! definition.valid() || definition.definition().name.is_empty() ||
            (definition.definition().kind == TextureRequestKind::Imported &&
             (! content || content->key.is_empty())))
            return Err(ResourceError { ResourceErrorKind::MissingDefinition });
        const auto& request = definition.definition();
        for (const auto& [handle, texture] : m_textures.iter()) {
            if (texture->definition.id() != definition.id()) continue;
            if (texture->definition.revision() > definition.revision() ||
                (texture->definition.revision() == definition.revision() &&
                 ! SameTextureRequest(texture->request(), request)))
                return Err(ResourceError { ResourceErrorKind::RevisionMismatch,
                                           "conflicting texture definition revision"_Str });
        }
        TextureRegistryIdentity identity {
            .kind    = request.kind,
            .key     = allocation_key ? String::make(*allocation_key) : String {},
            .logical = definition.id(),
        };
        auto existing = m_handles.get(identity);
        if (existing.is_some()) {
            auto handle  = **existing;
            auto texture = m_textures.get_mut(handle);
            if (texture.is_none())
                return Err(ResourceError { ResourceErrorKind::MissingDefinition });
            if (! SameTextureRequest((**texture).request(), request)) {
                if ((**texture).request().definition != request.definition ||
                    (**texture).request().kind != request.kind ||
                    (**texture).request().lifetime != request.lifetime ||
                    (**texture).request().content != request.content) {
                    ++(**texture).definition_version;
                }
                if (request.kind != TextureRequestKind::Imported &&
                    (**texture).request().source != request.source) {
                    ++(**texture).content_version;
                }
                (**texture).definition = definition.clone();
            }
            if ((**texture).imported_content != content) {
                ++(**texture).content_version;
                (**texture).imported_content = rstd::move(content);
            }
            (**texture).definition        = definition.clone();
            (**texture).preserve_contents = preserve_contents;
            return Ok(handle);
        }

        resource::TextureHandle handle {
            .index      = m_next_index++,
            .generation = m_generation,
        };
        (void)m_textures.insert(handle,
                                Texture {
                                    .id                = handle,
                                    .definition        = rstd::move(definition),
                                    .allocation_key    = identity.key.clone(),
                                    .preserve_contents = preserve_contents,
                                    .imported_content  = rstd::move(content),
                                });
        (void)m_handles.insert(rstd::move(identity), handle);
        return Ok(handle);
    }

public:
    auto Publish(resource::TextureHandle                   handle,
                 Arc<resource_registry::TextureAllocation> allocation,
                 Option<vulkan::ImageUploadTicket>         upload = None()) -> Option<u64> {
        auto image = allocation->View();
        if (ResolveTexture(handle).is_none() || image.slots.is_empty()) return None();

        auto logical = ResolveTexture(handle);
        if (logical.is_none()) return None();
        auto source_generation   = image.getActive().generation;
        auto current             = m_resources.get(handle);
        u64  physical_generation = current.is_some() ? (**current).generation : u64(1);
        if (current.is_some() && (**current).source_generation != source_generation) {
            ++physical_generation;
        }
        auto completion =
            upload.is_some() ? allocation->UploadCompletion() : allocation->Completion();
        TexturePhysical physical {
            .allocation         = rstd::move(allocation),
            .generation         = physical_generation,
            .source_generation  = source_generation,
            .definition_version = (**logical).definition_version,
            .content_version    = (**logical).content_version,
            .completion         = completion,
        };
        if (upload.is_some() && upload->Valid() && ! completion.valid()) {
            auto pending = m_pending_uploads.get_mut(upload->value);
            if (pending.is_none()) {
                (void)m_pending_uploads.insert(upload->value, Vec<PendingTextureUpload>::make());
                pending = m_pending_uploads.get_mut(upload->value);
            }
            if (pending.is_some()) {
                for (auto& item : **pending) {
                    if (item.handle != handle) continue;
                    item.physical = rstd::move(physical);
                    return Some(physical_generation);
                }
                (*pending)->push(
                    PendingTextureUpload { .handle = handle, .physical = rstd::move(physical) });
            }
        } else {
            (void)m_resources.insert(handle, rstd::move(physical));
        }
        return Some(physical_generation);
    }

    void MarkUploadsSubmitted(slice<vulkan::ImageUploadTicket> tickets,
                              texture::SubmissionToken         completion) {
        if (! completion.valid()) return;
        for (const auto& ticket : tickets) {
            auto pending = m_pending_uploads.remove(ticket.value);
            if (pending.is_none()) continue;
            for (auto& item : *pending) {
                auto logical = ResolveTexture(item.handle);
                if (logical.is_none() ||
                    (**logical).definition_version != item.physical.definition_version ||
                    (**logical).content_version != item.physical.content_version)
                    continue;
                item.physical.completion = completion;
                (void)m_resources.insert(item.handle, rstd::move(item.physical));
            }
        }
    }

    void ReservePublications() {
        auto count   = m_resources.len();
        auto batches = m_pending_uploads.values();
        for (auto batch = batches.next(); batch.is_some(); batch = batches.next())
            count += (*batch)->len();
        m_resources.reserve(count);
    }

    void PublishSubmitted() {
        m_pending_uploads.retain([&](const u64&, Vec<PendingTextureUpload>& batch) {
            if (batch.is_empty() ||
                ! batch[usize()].physical.allocation->UploadCompletion().valid())
                return true;
            for (auto& item : batch) {
                auto logical = ResolveTexture(item.handle);
                if (logical.is_none() ||
                    (**logical).definition_version != item.physical.definition_version ||
                    (**logical).content_version != item.physical.content_version)
                    continue;
                item.physical.completion = item.physical.allocation->UploadCompletion();
                (void)m_resources.insert(item.handle, rstd::move(item.physical));
            }
            return false;
        });
    }

    auto Pump(vulkan::TextureContext& context, double seconds)
        -> Result<empty, resource::ResourceError> {
        rstd::collections::HashSet<texture::Serial> pumped;
        for (const auto& [handle, physical] : m_resources.iter()) {
            auto slots = physical->allocation->Slots();
            if (slots.is_empty()) continue;
            auto identity = slots[usize()].view.texture().identity().resource;
            if (! pumped.insert(identity)) continue;
            auto result = physical->allocation->Pump(context, seconds);
            if (result.is_err()) {
                auto error = result.unwrap_err();
                for (const auto& [logical_handle, shared] : m_resources.iter()) {
                    if (! Arc<resource_registry::TextureAllocation>::ptr_eq(physical->allocation,
                                                                            shared->allocation))
                        continue;
                    auto logical = m_textures.get(*logical_handle);
                    if (logical && (**logical).definition.valid()) {
                        const auto& definition = (**logical).definition;
                        error.textures.push({ definition.id(),
                                              definition.revision(),
                                              {},
                                              definition.definition().name.clone() });
                    }
                }
                return Err(rstd::move(error));
            }
        }
        return Ok(empty {});
    }

    void DiscardPendingUploads() { m_pending_uploads.clear(); }

    auto ResolveTexture(resource::TextureHandle handle) const noexcept -> Option<ref<Texture>> {
        return m_textures.get(handle);
    }

    auto ResolveTextureState(resource::TextureHandle handle) const noexcept
        -> Option<TextureLogicalState> {
        auto texture = ResolveTexture(handle);
        if (texture.is_none()) return None();
        return Some(TextureLogicalState {
            .handle             = handle,
            .definition_version = (**texture).definition_version,
            .content_version    = (**texture).content_version,
        });
    }

    auto Resolve(resource::TextureHandle handle) const noexcept -> Option<ref<TexturePhysical>> {
        return m_resources.get(handle);
    }

    auto ResolveCurrent(resource::TextureHandle handle) const noexcept
        -> Option<ref<TexturePhysical>> {
        auto logical  = ResolveTexture(handle);
        auto physical = Resolve(handle);
        if (logical.is_none() || physical.is_none()) return None();
        if ((**logical).definition_version != (**physical).definition_version ||
            (**logical).content_version != (**physical).content_version) {
            return None();
        }
        return physical;
    }

    auto ResolvePending(resource::TextureHandle handle) const noexcept
        -> Option<ref<TexturePhysical>> {
        auto logical = ResolveTexture(handle);
        if (logical.is_none()) return None();
        auto batches = m_pending_uploads.values();
        for (auto batch = batches.next(); batch.is_some(); batch = batches.next()) {
            for (const auto& item : **batch) {
                if (item.handle != handle) continue;
                if ((**logical).definition_version != item.physical.definition_version ||
                    (**logical).content_version != item.physical.content_version) {
                    continue;
                }
                return Some(ref<TexturePhysical>::from_raw_parts(rstd::addressof(item.physical)));
            }
        }
        return None();
    }

    auto Find(TextureRequestKind kind, ref<str> key) const -> Option<TextureHandle> {
        Option<TextureHandle> selected;
        for (const auto& [handle, texture] : m_textures.iter()) {
            if (texture->request().kind != kind || texture->request().name != key) continue;
            if (selected) return None();
            selected = Some(TextureHandle(*handle));
        }
        return selected;
    }

    auto Find(const TextureResourceRef& definition) const -> Option<TextureHandle> {
        if (! definition.valid()) return None();
        Option<TextureHandle> selected;
        for (const auto& [handle, texture] : m_textures.iter()) {
            if (texture->definition.id() != definition.id() ||
                texture->definition.revision() != definition.revision())
                continue;
            if (selected) return None();
            selected = Some(TextureHandle(*handle));
        }
        return selected;
    }

    bool ReusePhysical(TextureHandle handle) {
        auto target = ResolveTexture(handle);
        if (! target) return false;
        for (const auto& [other, texture] : m_textures.iter()) {
            if (*other == handle || texture->request().kind != (*target)->request().kind ||
                texture->request().definition != (*target)->request().definition ||
                texture->request().content != (*target)->request().content ||
                texture->request().lifetime != (*target)->request().lifetime)
                continue;
            if ((*target)->imported_content) {
                if (texture->imported_content != (*target)->imported_content) continue;
            } else if ((*target)->allocation_key.is_empty() ||
                       texture->allocation_key != (*target)->allocation_key.as_str() ||
                       texture->preserve_contents || (*target)->preserve_contents)
                continue;
            auto source = ResolveCurrent(*other);
            if (source) {
                auto physical               = (*source)->clone();
                physical.definition_version = (*target)->definition_version;
                physical.content_version    = (*target)->content_version;
                (void)m_resources.insert(handle, rstd::move(physical));
                return true;
            }
            for (auto [ticket, batch] : m_pending_uploads.iter_mut()) {
                for (usize i {}; i < batch->len(); ++i) {
                    const auto& pending = (*batch)[i];
                    if (pending.handle != *other ||
                        pending.physical.definition_version != texture->definition_version ||
                        pending.physical.content_version != texture->content_version)
                        continue;
                    auto physical               = pending.physical.clone();
                    physical.definition_version = (*target)->definition_version;
                    physical.content_version    = (*target)->content_version;
                    batch->push({ handle, rstd::move(physical) });
                    return true;
                }
            }
        }
        return false;
    }

    bool Unbind(resource::TextureHandle handle) { return m_resources.remove(handle).is_some(); }

    bool BeginPrepareTransaction() {
        if (m_transaction.is_some() || ! m_pending_uploads.is_empty()) return false;
        m_transaction = Some(Box<Snapshot>::make(CloneSnapshot()));
        return true;
    }

    void CommitPrepareTransaction() { (void)m_transaction.take(); }

    void AbortPrepareTransaction() {
        auto snapshot = m_transaction.take();
        if (snapshot.is_none()) return;
        m_textures  = rstd::move((*snapshot)->textures);
        m_resources = rstd::move((*snapshot)->resources);
        m_handles   = rstd::move((*snapshot)->handles);

        m_legacy_definitions = rstd::move((*snapshot)->legacy_definitions);
        m_pending_uploads.clear();
    }

    void RetainActive(slice<resource::TextureHandle> handles) {
        auto active = HashSet<resource::TextureHandle>::with_capacity(handles.len());
        for (usize index {}; index < handles.len(); ++index) {
            (void)active.insert(handles[index]);
        }
        m_resources.retain([&](const resource::TextureHandle& handle, TexturePhysical&) {
            return active.contains(handle);
        });
        m_textures.retain([&](const TextureHandle& handle, Texture&) {
            return active.contains(handle);
        });
        m_handles.retain([&](const TextureRegistryIdentity&, TextureHandle& handle) {
            return active.contains(handle);
        });
        m_legacy_definitions.retain(
            [&](const TextureRegistryIdentity&, TextureResourceRef& definition) {
                for (const auto& [handle, texture] : m_textures.iter())
                    if (texture->definition.id() == definition.id()) return true;
                return false;
            });
    }

    void EvictUnused(bool transient_only = false) {
        m_resources.retain([&](const resource::TextureHandle& handle, TexturePhysical& physical) {
            if (physical.allocation.strong_count() > usize(1)) return true;
            if (! transient_only) return false;
            auto texture = m_textures.get(handle);
            return texture.is_none() ||
                   (**texture).request().lifetime != TextureLifetimeClass::FrameLocal;
        });
    }

    void ClearGraphResources() {
        m_resources.retain([&](const resource::TextureHandle& handle, TexturePhysical&) {
            auto texture = m_textures.get(handle);
            return texture.is_some() &&
                   ((**texture).request().kind == TextureRequestKind::Imported ||
                    (**texture).preserve_contents);
        });
    }

    void Reset() {
        m_transaction = None();
        m_resources.clear();
        m_pending_uploads.clear();
        m_textures.clear();
        m_handles.clear();
        m_legacy_definitions.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto Size() const noexcept -> usize { return m_textures.len(); }

private:
    auto LegacyDefinition(TextureRequest request) -> TextureResourceRef {
        TextureRegistryIdentity identity { .kind = request.kind, .key = request.name.clone() };
        auto                    previous = m_legacy_definitions.get_mut(identity);
        if (previous) {
            if (! SameTextureRequest((*previous)->definition(), request))
                **previous = (*previous)->with_definition(rstd::move(request));
            return (*previous)->clone();
        }
        TextureResourceRef definition(rstd::move(request));
        (void)m_legacy_definitions.insert(rstd::move(identity), definition.clone());
        return definition;
    }
    template<typename Value>
    using HandleMap = HashMap<resource::TextureHandle, Value>;

    struct Snapshot {
        HandleMap<Texture>                                        textures;
        HandleMap<TexturePhysical>                                resources;
        HashMap<TextureRegistryIdentity, resource::TextureHandle> handles;
        HashMap<TextureRegistryIdentity, TextureResourceRef>      legacy_definitions;
    };

    auto CloneSnapshot() const -> Snapshot {
        Snapshot snapshot;
        snapshot.textures.reserve(m_textures.len());
        auto textures = m_textures.iter();
        for (auto item = textures.next(); item.is_some(); item = textures.next()) {
            (void)snapshot.textures.insert(*item->template get<0>(),
                                           item->template get<1>()->clone());
        }
        snapshot.resources.reserve(m_resources.len());
        auto resources = m_resources.iter();
        for (auto item = resources.next(); item.is_some(); item = resources.next()) {
            (void)snapshot.resources.insert(*item->template get<0>(),
                                            item->template get<1>()->clone());
        }
        snapshot.handles.reserve(m_handles.len());
        auto handles = m_handles.iter();
        for (auto item = handles.next(); item.is_some(); item = handles.next()) {
            (void)snapshot.handles.insert(item->template get<0>()->clone(),
                                          *item->template get<1>());
        }
        for (const auto& [key, definition] : m_legacy_definitions.iter())
            (void)snapshot.legacy_definitions.insert(key->clone(), definition->clone());
        return snapshot;
    }

    u64                                                       m_generation { 1 };
    u64                                                       m_next_index { 0 };
    HandleMap<Texture>                                        m_textures;
    HandleMap<TexturePhysical>                                m_resources;
    HashMap<TextureRegistryIdentity, resource::TextureHandle> m_handles;
    HashMap<u64, Vec<PendingTextureUpload>>                   m_pending_uploads;
    Option<Box<Snapshot>>                                     m_transaction;
    HashMap<TextureRegistryIdentity, TextureResourceRef>      m_legacy_definitions;
};

} // namespace vrento::resource

export namespace rstd
{

template<>
struct Impl<vrento::resource::TextureLogicalRegistryView, vrento::resource::TextureRegistry>
    : ImplBase<vrento::resource::TextureRegistry> {
    auto ResolveTextureState(vrento::resource::TextureHandle handle) const
        -> Option<vrento::resource::TextureLogicalState> {
        return this->self().ResolveTextureState(handle);
    }
};

template<>
struct Impl<vrento::resource_registry::TextureUploadPublisher, vrento::resource::TextureRegistry>
    : ImplBase<vrento::resource::TextureRegistry> {
    void Reserve() { self().ReservePublications(); }
    void Submitted(slice<vrento::vulkan::ImageUploadTicket> tickets,
                   vrento::texture::SubmissionToken         completion) {
        self().MarkUploadsSubmitted(tickets, rstd::move(completion));
    }
};

} // namespace rstd
