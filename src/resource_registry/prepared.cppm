export module vrento:resource_registry.prepared;
import rstd;
import :resource;
import :vulkan.api;
import :texture.image;
import :texture.video_playback;

import :resource_registry.texture_registry;
import :resource_registry.buffer_registry;
import :resource_registry.shader_registry;
import :resource_registry.descriptor;
import :resource_registry.graphics;
import :resource_registry.external;

using rstd::collections::HashMap;
using rstd::sync::Arc;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::resource_registry
{

struct PreparedTexture {
    resource::TextureUseHandle   use;
    resource::TextureHandle      resource;
    resource::TextureResourceRef definition;
    auto request() const -> const resource::TextureRequest& { return definition.definition(); }
    Arc<TextureAllocation>   physical;
    vulkan::ImageSlotsRef    image;
    u64                      physical_generation { 0 };
    texture::SubmissionToken completion;
    auto                     ActiveTexture() const -> Result<TextureSlot, texture::Error> {
        auto slots = physical->Slots();
        auto slot  = image.active_slot();
        if (slot >= slots.len()) return Err(texture::Error { texture::ErrorKind::Invalid });
        return Ok(TextureSlot(slots[slot]));
    }
};

struct PreparedTextureLease {
    resource::TextureHandle resource;
    Arc<TextureAllocation>  physical;
    u64                     physical_generation { 0 };
};

struct PreparedBufferUse {
    resource::BufferUseHandle use;
    PreparedBuffer            buffer;
};

struct PreparedShaderUse {
    resource::ShaderUseHandle use;
    PreparedShader            shader;
};

struct PreparedPipeline {
    resource::PipelineUseHandle use;
    resource::PipelineHandle    resource;
    resource::RenderPassHandle  render_pass;
    Arc<PipelineResourceEntry>  physical;
};

struct PreparedRenderPass {
    resource::RenderPassUseHandle use;
    resource::RenderPassHandle    resource;
    vulkan::RenderPassCacheKey    cache_key;
    Arc<vvk::RenderPass>          physical;
};

struct PreparedFramebuffer {
    resource::FramebufferUseHandle use;
    resource::FramebufferHandle    resource;
    Arc<vvk::Framebuffer>          physical;
};

struct PreparedExternalUse {
    resource::ExternalUseHandle use;
    PreparedExternalFrame       frame;
};

struct PreparedBufferLease {
    resource::BufferHandle resource;
    Arc<BufferPhysical>    physical;
};

struct PreparedShaderLease {
    resource::ShaderHandle resource;
    Arc<ShaderPhysical>    physical;
};

struct PreparedPipelineLease {
    resource::PipelineHandle   resource;
    Arc<PipelineResourceEntry> physical;
};

struct PreparedRenderPassLease {
    resource::RenderPassHandle resource;
    Arc<vvk::RenderPass>       physical;
};

struct PreparedFramebufferLease {
    resource::FramebufferHandle resource;
    Arc<vvk::Framebuffer>       physical;
};

struct PreparedDescriptorLease {
    resource::DescriptorBindingHandle handle;
    Option<vvk::DescriptorSetLease>   set;
};

struct PreparedExternalLease {
    resource::ExternalUseHandle use;
    FrameSurfaceLease           lease;
};

struct PreparedResourceLeases {
    Vec<PreparedTextureLease>     textures;
    Vec<PreparedBufferLease>      buffers;
    Vec<PreparedShaderLease>      shaders;
    Vec<PreparedPipelineLease>    pipelines;
    Vec<PreparedRenderPassLease>  render_passes;
    Vec<PreparedFramebufferLease> framebuffers;
    Vec<PreparedDescriptorLease>  descriptors;
    Vec<PreparedExternalLease>    externals;
};

class PreparedResourceTable {
public:
    explicit PreparedResourceTable(u64 generation = u64()): m_generation(generation) {}

    bool Insert(PreparedTexture texture) {
        return m_textures.insert(texture.use, rstd::move(texture)).is_none();
    }

    auto Resolve(resource::TextureUseHandle use) const -> Option<ref<PreparedTexture>> {
        return m_textures.get(use);
    }

    auto ChangedTextureBindings(const PreparedResourceTable& previous) const
        -> Vec<resource::TextureUseHandle> {
        Vec<resource::TextureUseHandle> changed;
        for (const auto& [use, texture] : m_textures.iter()) {
            auto old = previous.Resolve(*use);
            if (! old || ! Arc<TextureAllocation>::ptr_eq(texture->physical, (*old)->physical))
                changed.push(resource::TextureUseHandle(*use));
        }
        for (const auto& [use, texture] : previous.m_textures.iter())
            if (! Resolve(*use)) changed.push(resource::TextureUseHandle(*use));
        return changed;
    }

    auto DescriptorTextures(resource::DescriptorBindingHandle handle) const
        -> slice<DescriptorTextureBinding> {
        auto found = m_descriptor_textures.get(handle);
        return found ? (**found).as_slice() : slice<DescriptorTextureBinding>();
    }
    void ForgetDescriptorTextures(resource::DescriptorBindingHandle handle) {
        auto previous = m_descriptor_textures.remove(handle);
        if (! previous) return;
        for (const auto& source : *previous) {
            auto uses = m_texture_descriptors.get_mut(source.texture);
            if (! uses) continue;
            for (usize i {}; i < (**uses).len();)
                if ((**uses)[i] == handle)
                    (**uses).remove(i);
                else
                    ++i;
            if ((**uses).is_empty()) (void)m_texture_descriptors.remove(source.texture);
        }
    }
    auto TrackDescriptorTextures(resource::DescriptorBindingHandle handle,
                                 slice<DescriptorTextureBinding>   sources) -> bool {
        auto descriptor = Resolve(handle);
        if (! descriptor || (*descriptor)->images.len() != sources.len()) return false;
        for (const auto& image : (*descriptor)->images) {
            usize matches {};
            for (const auto& source : sources)
                if (source.binding == image.binding) ++matches;
            if (matches != usize(1)) return false;
        }
        auto previous = DescriptorTextures(handle);
        bool same     = previous.len() == sources.len();
        if (same)
            for (usize i {}; i < sources.len(); ++i) same &= previous[i] == sources[i];
        if (same) return true;
        auto owned = Vec<DescriptorTextureBinding>::from(sources);
        ForgetDescriptorTextures(handle);
        if (owned.is_empty()) return true;
        for (const auto& source : owned) {
            if (! m_texture_descriptors.get(source.texture))
                (void)m_texture_descriptors.insert(source.texture,
                                                   Vec<resource::DescriptorBindingHandle>());
            auto& uses = **m_texture_descriptors.get_mut(source.texture);
            bool  found {};
            for (auto existing : uses) found |= existing == handle;
            if (! found) uses.push(resource::DescriptorBindingHandle(handle));
        }
        (void)m_descriptor_textures.insert(handle, rstd::move(owned));
        return true;
    }
    auto AffectedTextureDescriptors(slice<resource::TextureUseHandle> textures) const
        -> Vec<resource::DescriptorBindingHandle> {
        Vec<resource::DescriptorBindingHandle>           result;
        HashMap<resource::DescriptorBindingHandle, bool> seen;
        for (auto texture : textures)
            if (auto uses = m_texture_descriptors.get(texture))
                for (auto descriptor : **uses)
                    if (! seen.insert(descriptor, true)) result.push(rstd::move(descriptor));
        return result;
    }

    auto TextureUses(texture::TextureViewRef view) const -> Vec<resource::TextureUseHandle> {
        Vec<resource::TextureUseHandle> result;
        for (const auto& [use, prepared] : m_textures.iter())
            for (const auto& slot : prepared->physical->Slots())
                if (slot.view.texture().identity() == view.texture().identity()) {
                    result.push(resource::TextureUseHandle(*use));
                    break;
                }
        return result;
    }
    auto BufferUses(const vulkan::BufferAllocation& allocation) const
        -> Vec<resource::BufferUseHandle> {
        Vec<resource::BufferUseHandle> result;
        for (const auto& [use, prepared] : m_buffers.iter())
            if (prepared->buffer.physical->buffer.identity() == allocation.identity())
                result.push(resource::BufferUseHandle(*use));
        return result;
    }
    bool Insert(PreparedBufferUse buffer) {
        return m_buffers.insert(buffer.use, rstd::move(buffer)).is_none();
    }

    auto Resolve(resource::BufferUseHandle use) const -> Option<ref<PreparedBufferUse>> {
        return m_buffers.get(use);
    }

    bool Insert(PreparedShaderUse shader) {
        return m_shaders.insert(shader.use, rstd::move(shader)).is_none();
    }

    auto Resolve(resource::ShaderUseHandle use) const -> Option<ref<PreparedShaderUse>> {
        return m_shaders.get(use);
    }

    bool Insert(PreparedPipeline pipeline) {
        return m_pipelines.insert(pipeline.use, rstd::move(pipeline)).is_none();
    }

    auto Resolve(resource::PipelineUseHandle use) const -> Option<ref<PreparedPipeline>> {
        return m_pipelines.get(use);
    }

    void PublishPipeline(PreparedPipeline pipeline, PreparedRenderPass render_pass) {
        (void)m_render_passes.insert(render_pass.use, rstd::move(render_pass));
        (void)m_pipelines.insert(pipeline.use, rstd::move(pipeline));
    }

    bool Insert(PreparedRenderPass render_pass) {
        return m_render_passes.insert(render_pass.use, rstd::move(render_pass)).is_none();
    }

    auto Resolve(resource::RenderPassUseHandle use) const -> Option<ref<PreparedRenderPass>> {
        return m_render_passes.get(use);
    }

    bool Insert(PreparedFramebuffer framebuffer) {
        return m_framebuffers.insert(framebuffer.use, rstd::move(framebuffer)).is_none();
    }

    auto Resolve(resource::FramebufferUseHandle use) const -> Option<ref<PreparedFramebuffer>> {
        return m_framebuffers.get(use);
    }

    bool Insert(PreparedDescriptorBinding binding) {
        return m_descriptors.insert(binding.handle, rstd::move(binding)).is_none();
    }

    auto Resolve(resource::DescriptorBindingHandle handle) const
        -> Option<ref<PreparedDescriptorBinding>> {
        return m_descriptors.get(handle);
    }

    auto ResolveMut(resource::DescriptorBindingHandle handle)
        -> Option<mut_ref<PreparedDescriptorBinding>> {
        return m_descriptors.get_mut(handle);
    }

    void Insert(PreparedExternalUse external) {
        (void)m_externals.insert(external.use, rstd::move(external));
    }

    auto Resolve(resource::ExternalUseHandle use) const -> Option<ref<PreparedExternalUse>> {
        return m_externals.get(use);
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto TextureCount() const noexcept -> usize { return m_textures.len(); }
    auto BufferCount() const noexcept -> usize { return m_buffers.len(); }
    auto ShaderCount() const noexcept -> usize { return m_shaders.len(); }
    auto PipelineCount() const noexcept -> usize { return m_pipelines.len(); }
    auto RenderPassCount() const noexcept -> usize { return m_render_passes.len(); }
    auto FramebufferCount() const noexcept -> usize { return m_framebuffers.len(); }
    auto DescriptorCount() const noexcept -> usize { return m_descriptors.len(); }
    auto ExternalCount() const noexcept -> usize { return m_externals.len(); }

    auto TextureResources() const -> Vec<resource::TextureHandle> {
        auto resources = Vec<resource::TextureHandle>::with_capacity(m_textures.len());
        auto textures  = m_textures.values();
        for (auto texture = textures.next(); texture.is_some(); texture = textures.next()) {
            auto resource = (**texture).resource;
            resources.push(rstd::move(resource));
        }
        return resources;
    }

    auto Leases() const -> PreparedResourceLeases {
        auto texture_leases = Vec<PreparedTextureLease>::with_capacity(m_textures.len());
        auto textures       = m_textures.values();
        for (auto texture = textures.next(); texture.is_some(); texture = textures.next()) {
            texture_leases.push(PreparedTextureLease {
                .resource            = (**texture).resource,
                .physical            = (**texture).physical.clone(),
                .physical_generation = (**texture).physical_generation,
            });
        }

        auto descriptor_leases = Vec<PreparedDescriptorLease>::with_capacity(m_descriptors.len());
        auto descriptors       = m_descriptors.values();
        for (auto descriptor = descriptors.next(); descriptor.is_some();
             descriptor      = descriptors.next()) {
            descriptor_leases.push(PreparedDescriptorLease {
                .handle = (**descriptor).handle,
                .set    = (**descriptor).set.is_some() ? Some((**descriptor).set->clone())
                                                       : None<vvk::DescriptorSetLease>(),
            });
        }

        auto buffer_leases = Vec<PreparedBufferLease>::with_capacity(m_buffers.len());
        auto buffers       = m_buffers.values();
        for (auto buffer = buffers.next(); buffer.is_some(); buffer = buffers.next()) {
            buffer_leases.push(PreparedBufferLease {
                .resource = (**buffer).buffer.resource,
                .physical = (**buffer).buffer.physical.clone(),
            });
        }

        auto shader_leases = Vec<PreparedShaderLease>::with_capacity(m_shaders.len());
        auto shaders       = m_shaders.values();
        for (auto shader = shaders.next(); shader.is_some(); shader = shaders.next()) {
            shader_leases.push(PreparedShaderLease {
                .resource = (**shader).shader.resource,
                .physical = (**shader).shader.physical.clone(),
            });
        }

        auto pipeline_leases = Vec<PreparedPipelineLease>::with_capacity(m_pipelines.len());
        auto pipelines       = m_pipelines.values();
        for (auto pipeline = pipelines.next(); pipeline.is_some(); pipeline = pipelines.next()) {
            pipeline_leases.push(PreparedPipelineLease {
                .resource = (**pipeline).resource,
                .physical = (**pipeline).physical.clone(),
            });
        }

        auto render_pass_leases =
            Vec<PreparedRenderPassLease>::with_capacity(m_render_passes.len());
        auto render_passes = m_render_passes.values();
        for (auto render_pass = render_passes.next(); render_pass.is_some();
             render_pass      = render_passes.next()) {
            render_pass_leases.push(PreparedRenderPassLease {
                .resource = (**render_pass).resource,
                .physical = (**render_pass).physical.clone(),
            });
        }

        auto framebuffer_leases =
            Vec<PreparedFramebufferLease>::with_capacity(m_framebuffers.len());
        auto framebuffers = m_framebuffers.values();
        for (auto framebuffer = framebuffers.next(); framebuffer.is_some();
             framebuffer      = framebuffers.next()) {
            framebuffer_leases.push(PreparedFramebufferLease {
                .resource = (**framebuffer).resource,
                .physical = (**framebuffer).physical.clone(),
            });
        }
        auto external_leases = Vec<PreparedExternalLease>::with_capacity(m_externals.len());
        auto externals       = m_externals.values();
        for (auto external = externals.next(); external.is_some(); external = externals.next()) {
            external_leases.push(PreparedExternalLease {
                .use   = (**external).use,
                .lease = (**external).frame.lease,
            });
        }
        return PreparedResourceLeases {
            .textures      = rstd::move(texture_leases),
            .buffers       = rstd::move(buffer_leases),
            .shaders       = rstd::move(shader_leases),
            .pipelines     = rstd::move(pipeline_leases),
            .render_passes = rstd::move(render_pass_leases),
            .framebuffers  = rstd::move(framebuffer_leases),
            .descriptors   = rstd::move(descriptor_leases),
            .externals     = rstd::move(external_leases),
        };
    }

    void CarryForward(PreparedResourceTable&&        previous,
                      resource::ResourcePlanSections prepared_sections) {
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanTextures)) {
            m_textures = rstd::move(previous.m_textures);
        }
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanBuffers)) {
            m_buffers = rstd::move(previous.m_buffers);
        }
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanShaders)) {
            m_shaders = rstd::move(previous.m_shaders);
        }
        m_pipelines           = rstd::move(previous.m_pipelines);
        m_render_passes       = rstd::move(previous.m_render_passes);
        m_framebuffers        = rstd::move(previous.m_framebuffers);
        m_descriptors         = rstd::move(previous.m_descriptors);
        m_descriptor_textures = rstd::move(previous.m_descriptor_textures);
        m_texture_descriptors = rstd::move(previous.m_texture_descriptors);
        m_externals           = rstd::move(previous.m_externals);
    }

    auto clone() const -> PreparedResourceTable {
        PreparedResourceTable cloned(m_generation);

        auto textures = m_textures.values();
        for (auto value = textures.next(); value.is_some(); value = textures.next()) {
            const auto& texture = **value;
            (void)cloned.Insert(PreparedTexture {
                .use                 = texture.use,
                .resource            = texture.resource,
                .definition          = texture.definition.clone(),
                .physical            = texture.physical.clone(),
                .image               = texture.image,
                .physical_generation = texture.physical_generation,
                .completion          = texture.completion,
            });
        }

        auto buffers = m_buffers.values();
        for (auto value = buffers.next(); value.is_some(); value = buffers.next()) {
            const auto& buffer = **value;
            (void)cloned.Insert(PreparedBufferUse {
                .use    = buffer.use,
                .buffer = buffer.buffer.clone(),
            });
        }

        auto shaders = m_shaders.values();
        for (auto value = shaders.next(); value.is_some(); value = shaders.next()) {
            const auto& shader = **value;
            (void)cloned.Insert(PreparedShaderUse {
                .use    = shader.use,
                .shader = shader.shader.clone(),
            });
        }

        auto pipelines = m_pipelines.values();
        for (auto value = pipelines.next(); value.is_some(); value = pipelines.next()) {
            const auto& pipeline = **value;
            (void)cloned.Insert(PreparedPipeline {
                .use         = pipeline.use,
                .resource    = pipeline.resource,
                .render_pass = pipeline.render_pass,
                .physical    = pipeline.physical.clone(),
            });
        }

        auto render_passes = m_render_passes.values();
        for (auto value = render_passes.next(); value.is_some(); value = render_passes.next()) {
            const auto& render_pass = **value;
            (void)cloned.Insert(PreparedRenderPass {
                .use       = render_pass.use,
                .resource  = render_pass.resource,
                .cache_key = render_pass.cache_key.clone(),
                .physical  = render_pass.physical.clone(),
            });
        }

        auto framebuffers = m_framebuffers.values();
        for (auto value = framebuffers.next(); value.is_some(); value = framebuffers.next()) {
            const auto& framebuffer = **value;
            (void)cloned.Insert(PreparedFramebuffer {
                .use      = framebuffer.use,
                .resource = framebuffer.resource,
                .physical = framebuffer.physical.clone(),
            });
        }

        auto descriptors = m_descriptors.values();
        for (auto value = descriptors.next(); value.is_some(); value = descriptors.next()) {
            (void)cloned.Insert((**value).clone());
        }

        for (const auto& [handle, sources] : m_descriptor_textures.iter())
            (void)cloned.TrackDescriptorTextures(*handle, sources->as_slice());

        auto externals = m_externals.values();
        for (auto value = externals.next(); value.is_some(); value = externals.next()) {
            const auto& external = **value;
            cloned.Insert(PreparedExternalUse {
                .use   = external.use,
                .frame = external.frame.clone(),
            });
        }
        return cloned;
    }

    void ClearPreparedState() {
        m_pipelines.clear();
        m_render_passes.clear();
        m_framebuffers.clear();
        m_descriptors.clear();
        m_descriptor_textures.clear();
        m_texture_descriptors.clear();
        m_externals.clear();
    }

    void Remove(resource::PipelineUseHandle use) { (void)m_pipelines.remove(use); }
    void Remove(resource::RenderPassUseHandle use) { (void)m_render_passes.remove(use); }
    void Remove(resource::FramebufferUseHandle use) { (void)m_framebuffers.remove(use); }
    void Remove(resource::DescriptorBindingHandle use) {
        ForgetDescriptorTextures(use);
        (void)m_descriptors.remove(use);
    }
    void Remove(resource::ExternalUseHandle use) { (void)m_externals.remove(use); }

private:
    using TextureMap     = HashMap<resource::TextureUseHandle, PreparedTexture>;
    using DescriptorMap  = HashMap<resource::DescriptorBindingHandle, PreparedDescriptorBinding>;
    using BufferMap      = HashMap<resource::BufferUseHandle, PreparedBufferUse>;
    using ShaderMap      = HashMap<resource::ShaderUseHandle, PreparedShaderUse>;
    using PipelineMap    = HashMap<resource::PipelineUseHandle, PreparedPipeline>;
    using RenderPassMap  = HashMap<resource::RenderPassUseHandle, PreparedRenderPass>;
    using FramebufferMap = HashMap<resource::FramebufferUseHandle, PreparedFramebuffer>;
    using ExternalMap    = HashMap<resource::ExternalUseHandle, PreparedExternalUse>;

    u64                                                                       m_generation { 0 };
    TextureMap                                                                m_textures;
    BufferMap                                                                 m_buffers;
    ShaderMap                                                                 m_shaders;
    PipelineMap                                                               m_pipelines;
    RenderPassMap                                                             m_render_passes;
    FramebufferMap                                                            m_framebuffers;
    DescriptorMap                                                             m_descriptors;
    HashMap<resource::DescriptorBindingHandle, Vec<DescriptorTextureBinding>> m_descriptor_textures;
    HashMap<resource::TextureUseHandle, Vec<resource::DescriptorBindingHandle>>
                m_texture_descriptors;
    ExternalMap m_externals;
};

class ResourcePrepareService;

class TexturePrepareTrace {
public:
    enum class Kind
    {
        Plan,
        Decode,
        Upload,
    };

    TexturePrepareTrace(Option<mut_ref<dyn<resource::TexturePrepareObserver>>> observer, Kind kind)
        : m_observer(observer), m_kind(kind) {
        if (m_observer.is_none()) return;
        if (m_kind == Kind::Plan) {
            (*m_observer)->BeginTexturePlan();
        } else if (m_kind == Kind::Decode) {
            (*m_observer)->BeginTextureDecode();
        } else {
            (*m_observer)->BeginTextureUpload();
        }
    }

    ~TexturePrepareTrace() { Finish(); }

    TexturePrepareTrace(const TexturePrepareTrace&)                    = delete;
    TexturePrepareTrace(TexturePrepareTrace&&)                         = delete;
    auto operator=(const TexturePrepareTrace&) -> TexturePrepareTrace& = delete;
    auto operator=(TexturePrepareTrace&&) -> TexturePrepareTrace&      = delete;

    void Finish() {
        if (m_observer.is_none()) return;
        if (m_kind == Kind::Plan) {
            (*m_observer)->EndTexturePlan();
        } else if (m_kind == Kind::Decode) {
            (*m_observer)->EndTextureDecode();
        } else {
            (*m_observer)->EndTextureUpload();
        }
        m_observer = None();
    }

private:
    Option<mut_ref<dyn<resource::TexturePrepareObserver>>> m_observer;
    Kind                                                   m_kind;
};

struct ResourceContentProviders {
    Option<mut_ref<dyn<resource::TextureContentProvider>>> texture;
    Option<mut_ref<dyn<resource::BufferContentProvider>>>  buffer;
    Option<mut_ref<dyn<resource::ShaderArtifactProvider>>> shader;
};

enum class ResourcePrepareProgress
{
    BatchReady,
    Complete,
};

class ResourcePrepareSession {
public:
    ResourcePrepareSession(const ResourcePrepareSession&)                        = delete;
    auto operator=(const ResourcePrepareSession&) -> ResourcePrepareSession&     = delete;
    ResourcePrepareSession(ResourcePrepareSession&&) noexcept                    = default;
    auto operator=(ResourcePrepareSession&&) noexcept -> ResourcePrepareSession& = delete;

    auto Plan() const noexcept -> const resource::ResourcePlan& { return m_plan; }
    auto Sections() const noexcept -> resource::ResourcePlanSections { return m_sections; }
    auto TakeTable() && -> PreparedResourceTable { return rstd::move(m_table); }

private:
    friend class ResourcePrepareService;

    struct PendingUse {
        resource::TextureUseHandle   use;
        resource::TextureHandle      resource;
        resource::TextureResourceRef definition;
    };

    struct PendingContent {
        String                                key;
        Option<Arc<rstd::dyn<VideoPlayback>>> playback;
        Vec<PendingUse>                       uses;
    };

    struct DecodedContent {
        usize                                       index;
        Result<Arc<Image>, resource::ResourceError> image;
    };

    ResourcePrepareSession(const resource::ResourcePlan&  plan,
                           resource::ResourcePlanSections sections)
        : m_plan(plan.clone()), m_sections(sections), m_table(plan.generation) {}

    resource::ResourcePlan                                m_plan;
    resource::ResourcePlanSections                        m_sections;
    PreparedResourceTable                                 m_table;
    Vec<PendingContent>                                   m_pending { Vec<PendingContent>::make() };
    usize                                                 m_next_submit {};
    usize                                                 m_completed {};
    Option<Arc<dyn<resource::TextureLoader>>>             m_loader;
    Option<rstd::thread::ThreadPool>                      m_pool;
    Option<rstd::thread::BlockingTaskSet<DecodedContent>> m_tasks;
};

class ResourcePlanPrepareVisitor {
public:
    ResourcePlanPrepareVisitor(ResourcePrepareService& service, PreparedResourceTable& table,
                               ResourceContentProviders providers)
        : m_service(service), m_table(table), m_providers(rstd::move(providers)) {}

    auto VisitTexture(const resource::TexturePlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitBuffer(const resource::BufferPlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitShader(const resource::ShaderPlanEntry&) -> Result<empty, resource::ResourceError>;

private:
    ResourcePrepareService&  m_service;
    PreparedResourceTable&   m_table;
    ResourceContentProviders m_providers;
};

class ResourcePrepareService {
public:
    ResourcePrepareService(resource::TextureRegistry&                  textures,
                           Option<mut_ref<dyn<TexturePrepareBackend>>> textures_backend,
                           BufferRegistry&                             buffers,
                           mut_ref<dyn<vulkan::BufferBackend>>         buffer_backend,
                           ShaderRegistry&                             shaders)
        : m_textures(textures),
          m_textures_backend(textures_backend),
          m_buffers(buffers),
          m_buffer_backend(buffer_backend),
          m_shaders(shaders) {}

    auto Begin(const resource::ResourcePlan& plan, ResourceContentProviders providers = {},
               resource::ResourcePlanSections sections = resource::ResourcePlanAll,
               Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<ResourcePrepareSession, resource::ResourceError> {
        ResourcePrepareSession   session(plan, sections);
        auto                     texture_provider = rstd::move(providers.texture);
        ResourceContentProviders remaining {
            .buffer = rstd::move(providers.buffer),
            .shader = rstd::move(providers.shader),
        };
        if (resource::ResourcePlanIncludes(sections, resource::ResourcePlanTextures)) {
            auto prepared = BeginTextures(
                plan.textures.as_slice(), session, rstd::move(texture_provider), texture_observer);
            if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        }
        ResourcePlanPrepareVisitor visitor(*this, session.m_table, rstd::move(remaining));
        auto object  = rstd::dyn<resource::ResourcePlanVisitor>::from_ref(visitor);
        auto visited = resource::VisitResourcePlan(
            plan,
            object,
            sections & (resource::ResourcePlanBuffers | resource::ResourcePlanShaders));
        if (visited.is_err()) return Err(rstd::move(visited).unwrap_err_unchecked());
        return Ok(rstd::move(session));
    }

    auto Prepare(const resource::ResourcePlan& plan, ResourceContentProviders providers = {},
                 resource::ResourcePlanSections sections = resource::ResourcePlanAll,
                 Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<PreparedResourceTable, resource::ResourceError> {
        auto started = Begin(plan, rstd::move(providers), sections, texture_observer);
        if (started.is_err()) return Err(rstd::move(started).unwrap_err_unchecked());
        auto session = rstd::move(started).unwrap_unchecked();
        while (true) {
            auto progress = Continue(session, texture_observer);
            if (progress.is_err()) return Err(rstd::move(progress).unwrap_err_unchecked());
            if (progress.unwrap_unchecked() == ResourcePrepareProgress::Complete) break;
        }
        return Ok(rstd::move(session).TakeTable());
    }

    auto PrepareBuffer(const resource::BufferPlanEntry& entry, PreparedResourceTable& table,
                       Option<mut_ref<dyn<resource::BufferContentProvider>>> content)
        -> Result<empty, resource::ResourceError> {
        if (! entry.definition.valid())
            return Err(
                resource::ResourceError { .kind = resource::ResourceErrorKind::MissingDefinition });
        auto prepared = m_buffers.Ensure(entry.definition.clone(), content, m_buffer_backend);
        if (prepared.is_err()) {
            auto error = prepared.unwrap_err();
            error.contexts.push(resource::ResourceErrorContext::Buffer(
                entry.definition.id(), entry.definition.revision(), entry.handle));
            return Err(rstd::move(error));
        }
        if (! table.Insert(PreparedBufferUse {
                .use    = entry.handle,
                .buffer = rstd::move(prepared).unwrap_unchecked(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::DuplicateUse,
                .message = rstd::format("duplicate buffer use {}", entry.handle.index),
            });
        }
        return Ok(empty {});
    }

    auto PrepareShader(const resource::ShaderPlanEntry& entry, PreparedResourceTable& table,
                       Option<mut_ref<dyn<resource::ShaderArtifactProvider>>> provider)
        -> Result<empty, resource::ResourceError> {
        if (provider.is_none()) {
            return Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingContent,
                .message =
                    rstd::format("shader artifact {} unavailable", entry.request.name.as_str()),
            });
        }
        auto prepared = m_shaders.Prepare(entry.request.clone(), *provider);
        if (prepared.is_err()) {
            auto error = prepared.unwrap_err();
            error.contexts.push(
                resource::ResourceErrorContext::Shader(entry.handle, entry.request.name.clone()));
            return Err(rstd::move(error));
        }
        if (! table.Insert(PreparedShaderUse {
                .use    = entry.handle,
                .shader = rstd::move(prepared).unwrap_unchecked(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::DuplicateUse,
                .message = rstd::format("duplicate shader use {}", entry.handle.index),
            });
        }
        return Ok(empty {});
    }

    static auto TextureFailure(resource::ResourceError             error,
                               const resource::TextureResourceRef& definition,
                               resource::TextureUseHandle          use) -> resource::ResourceError {
        if (definition.valid())
            error.textures.push({ definition.id(),
                                  definition.revision(),
                                  use,
                                  definition.definition().name.clone() });
        return error;
    }
    auto BeginTextures(slice<resource::TexturePlanEntry> entries, ResourcePrepareSession& session,
                       Option<mut_ref<dyn<resource::TextureContentProvider>>> content,
                       Option<mut_ref<dyn<resource::TexturePrepareObserver>>> observer)
        -> Result<empty, resource::ResourceError> {
        auto pending_index = HashMap<resource::ImportedTextureContentIdentity, usize>::make();
        TexturePrepareTrace plan_trace(observer, TexturePrepareTrace::Kind::Plan);

        for (usize entry_index {}; entry_index < entries.len(); ++entry_index) {
            const auto& entry            = entries[entry_index];
            auto        imported_content = None<resource::ImportedTextureContentIdentity>();
            if (! entry.definition.valid())
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingDefinition });
            auto source_request = entry.source_request();
            if (entry.request().kind == resource::TextureRequestKind::Imported) {
                if (! content)
                    return Err(
                        resource::ResourceError { resource::ResourceErrorKind::MissingContent });
                auto resolved = (*content)->ResolveTextureContent(source_request);
                if (resolved.is_err())
                    return Err(TextureFailure(
                        rstd::move(resolved).unwrap_err(), entry.definition, entry.handle));
                imported_content = Some(resolved.unwrap());
            }
            auto registered = m_textures.RegisterResource(
                entry.definition.clone(),
                imported_content ? Some(imported_content->clone()) : None(),
                entry.allocation_key.is_empty() ? None<ref<str>>()
                                                : Some(entry.allocation_key.as_str()),
                entry.preserve_contents);
            if (registered.is_err())
                return Err(TextureFailure(
                    rstd::move(registered).unwrap_err(), entry.definition, entry.handle));
            auto handle = registered.unwrap();
            if (! handle.Valid()) {
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingDefinition,
                    .message =
                        rstd::format("invalid texture request {}", entry.request().name.as_str()),
                });
            }

            auto physical = m_textures.ResolveCurrent(handle);
            if (physical.is_none()) physical = m_textures.ResolvePending(handle);
            if (! physical && m_textures.ReusePhysical(handle)) {
                physical = m_textures.ResolveCurrent(handle);
                if (! physical) physical = m_textures.ResolvePending(handle);
            }
            if (physical.is_some()) {
                auto inserted = InsertPrepared(
                    entry.handle, handle, entry.definition.clone(), **physical, session.m_table);
                if (inserted.is_err())
                    return Err(TextureFailure(
                        rstd::move(inserted).unwrap_err(), entry.definition, entry.handle));
                continue;
            }

            if (entry.request().kind != resource::TextureRequestKind::Imported) {
                auto allocation_request = entry.request().clone();
                if (! entry.allocation_key.is_empty())
                    allocation_request.name = entry.allocation_key.clone();
                const bool initialize_transparent =
                    (entry.request().content &
                     resource::TextureContentFlag(
                         resource::TextureContent::InitializeTransparent)) != u32();
                if (initialize_transparent) {
                    auto prepared = AllocateTransparentTexture(allocation_request);
                    if (prepared.is_err()) {
                        return Err(TextureFailure(
                            rstd::move(prepared).unwrap_err(), entry.definition, entry.handle));
                    }
                    auto allocation = rstd::move(prepared).unwrap_unchecked();
                    auto published  = PublishPrepared(entry.handle,
                                                      handle,
                                                      entry.definition.clone(),
                                                      rstd::move(allocation.allocation),
                                                      allocation.upload,
                                                      session.m_table);
                    if (published.is_err()) {
                        return Err(TextureFailure(
                            rstd::move(published).unwrap_err(), entry.definition, entry.handle));
                    }
                    continue;
                }
                auto allocated = AllocateTexture(allocation_request);
                if (allocated.is_err())
                    return Err(TextureFailure(
                        rstd::move(allocated).unwrap_err(), entry.definition, entry.handle));
                auto published = PublishPrepared(entry.handle,
                                                 handle,
                                                 entry.definition.clone(),
                                                 rstd::move(allocated).unwrap_unchecked(),
                                                 None<vulkan::ImageUploadTicket>(),
                                                 session.m_table);
                if (published.is_err()) {
                    return Err(TextureFailure(
                        rstd::move(published).unwrap_err(), entry.definition, entry.handle));
                }
                continue;
            }

            auto  identity = rstd::move(imported_content).unwrap_unchecked();
            auto  found    = pending_index.get(identity);
            auto  playback = (*content)->ResolveVideoPlayback(source_request);
            usize index;
            if (found.is_some()) {
                index = **found;
                if (session.m_pending[index].playback.is_none() && playback.is_some()) {
                    session.m_pending[index].playback = rstd::move(playback);
                }
            } else {
                index = session.m_pending.len();
                (void)pending_index.insert(identity.clone(), index);
                session.m_pending.push(ResourcePrepareSession::PendingContent {
                    .key      = rstd::move(identity.key),
                    .playback = rstd::move(playback),
                    .uses     = Vec<ResourcePrepareSession::PendingUse>::make(),
                });
            }
            session.m_pending[index].uses.push(ResourcePrepareSession::PendingUse {
                .use        = entry.handle,
                .resource   = handle,
                .definition = entry.definition.clone(),
            });
        }
        plan_trace.Finish();

        if (session.m_pending.is_empty()) return Ok(empty {});
        auto loader = (*content)->OpenTextureLoader();
        if (loader.is_err()) return Err(rstd::move(loader).unwrap_err_unchecked());
        session.m_loader = Some(rstd::move(loader).unwrap_unchecked());

        const auto worker_count = rstd::min(usize(4), session.m_pending.len());
        auto       builder      = rstd::thread::ThreadPoolBuilder::make();
        builder.worker_count(worker_count);
        builder.thread_name("vrento-texture-decode"_Str);
        auto pool = builder.build();
        if (pool.is_err()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Worker,
                .message = "create texture decode thread pool failed"_Str,
                .source  = Some(resource::ResourceErrorSource::Io(pool.unwrap_err())),
            });
        }
        session.m_pool = Some(rstd::move(pool).unwrap_unchecked());
        auto tasks = rstd::thread::BlockingTaskSet<ResourcePrepareSession::DecodedContent>::make(
            session.m_pool->handle(), worker_count);
        if (tasks.is_err()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Worker,
                .message = "create texture decode task set failed"_Str,
            });
        }
        session.m_tasks = Some(rstd::move(tasks).unwrap_unchecked());
        return SubmitAvailable(session);
    }

    auto Continue(ResourcePrepareSession&                                session,
                  Option<mut_ref<dyn<resource::TexturePrepareObserver>>> observer = None())
        -> Result<ResourcePrepareProgress, resource::ResourceError> {
        if (session.m_completed == session.m_pending.len()) {
            FinishTasks(session);
            return Ok(ResourcePrepareProgress::Complete);
        }

        constexpr usize batch_size { 4 };
        usize           prepared_count {};
        while (prepared_count < batch_size && session.m_completed < session.m_pending.len()) {
            TexturePrepareTrace decode_trace(observer, TexturePrepareTrace::Kind::Decode);
            auto                completion = session.m_tasks->recv();
            decode_trace.Finish();
            if (completion.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::Closed,
                    .message = "texture decode task set closed early"_Str,
                });
            }
            auto decoded = rstd::move(*completion).into_value();
            if (decoded.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::Cancelled,
                    .message = "texture decode task cancelled"_Str,
                });
            }
            auto item    = rstd::move(decoded).unwrap_unchecked();
            auto failure = [&](resource::ResourceError error) {
                for (const auto& use : session.m_pending[item.index].uses)
                    error = TextureFailure(rstd::move(error), use.definition, use.use);
                return Err(rstd::move(error));
            };
            if (item.image.is_err()) {
                return failure(rstd::move(item.image).unwrap_err_unchecked());
            }
            if (m_textures_backend.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::Closed,
                    .message = "texture backend unavailable"_Str,
                });
            }

            {
                TexturePrepareTrace upload_trace(observer, TexturePrepareTrace::Kind::Upload);
                auto                image = rstd::move(item.image).unwrap_unchecked();
                auto playback = session.m_pending[item.index].playback.is_some()
                                    ? Some(session.m_pending[item.index].playback->clone())
                                    : None<Arc<rstd::dyn<VideoPlayback>>>();
                auto created = (*m_textures_backend)
                                   ->CreateImportedTexture(rstd::move(image), rstd::move(playback));
                if (created.is_err()) return failure(rstd::move(created).unwrap_err_unchecked());
                auto prepared   = rstd::move(created).unwrap_unchecked();
                auto allocation = rstd::move(prepared.allocation);
                for (auto& use : session.m_pending[item.index].uses) {
                    auto published = PublishPrepared(use.use,
                                                     use.resource,
                                                     use.definition.clone(),
                                                     allocation.clone(),
                                                     prepared.upload,
                                                     session.m_table);
                    if (published.is_err()) {
                        return Err(TextureFailure(
                            rstd::move(published).unwrap_err(), use.definition, use.use));
                    }
                }
                upload_trace.Finish();
            }

            ++session.m_completed;
            ++prepared_count;
            auto submitted = SubmitAvailable(session);
            if (submitted.is_err()) return Err(rstd::move(submitted).unwrap_err_unchecked());
        }
        return Ok(ResourcePrepareProgress::BatchReady);
    }

private:
    static auto SubmitAvailable(ResourcePrepareSession& session)
        -> Result<empty, resource::ResourceError> {
        while (session.m_next_submit < session.m_pending.len()) {
            const auto index     = session.m_next_submit;
            auto       key       = session.m_pending[index].key.clone();
            auto       loader    = session.m_loader->clone();
            auto       submitted = session.m_tasks->try_submit(
                [index, key = rstd::move(key), loader = rstd::move(loader)]() mutable {
                    return ResourcePrepareSession::DecodedContent {
                        .index = index,
                        .image = loader->LoadTexture(key.as_str()),
                    };
                });
            if (submitted.is_err()) {
                if (submitted.unwrap_err_unchecked() ==
                    rstd::thread::BlockingTaskSetSubmitError::Full) {
                    break;
                }
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::Worker,
                    .message = "submit texture decode task failed"_Str,
                });
            }
            ++session.m_next_submit;
        }
        if (session.m_next_submit == session.m_pending.len()) session.m_tasks->close();
        return Ok(empty {});
    }

    static void FinishTasks(ResourcePrepareSession& session) {
        {
            auto tasks = session.m_tasks.take();
        }
        auto pool = session.m_pool.take();
        if (pool.is_some()) rstd::move(*pool).join();
        session.m_loader = None();
    }

    auto AllocateTexture(const resource::TextureRequest& request)
        -> Result<Arc<TextureAllocation>, resource::ResourceError> {
        if (m_textures_backend.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Closed,
                .message = rstd::format("texture backend unavailable"),
            });
        }
        if (request.definition.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("texture definition {} unavailable", request.name.as_str()),
            });
        }
        auto image = (*m_textures_backend)->AllocateTexture(*request.definition, false);
        if (image.is_err()) return Err(rstd::move(image).unwrap_err_unchecked());
        return Ok(rstd::move(image).unwrap_unchecked().allocation);
    }

    auto AllocateTransparentTexture(const resource::TextureRequest& request)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        if (m_textures_backend.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Closed,
                .message = rstd::format("texture backend unavailable"),
            });
        }
        if (request.definition.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("texture definition {} unavailable", request.name.as_str()),
            });
        }
        return (*m_textures_backend)->AllocateTexture(*request.definition, true);
    }

    auto PublishPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                         resource::TextureResourceRef definition, Arc<TextureAllocation> allocation,
                         Option<vulkan::ImageUploadTicket> upload, PreparedResourceTable& table)
        -> Result<empty, resource::ResourceError> {
        auto physical = m_textures.ResolveCurrent(handle);
        if (physical.is_none()) {
            auto prepared_allocation = allocation.clone();
            auto published           = m_textures.Publish(handle, rstd::move(allocation), upload);
            if (published.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("publish texture {} failed",
                                            definition.definition().name.as_str()),
                });
            }
            if (upload.is_some()) {
                return InsertPrepared(use,
                                      handle,
                                      rstd::move(definition),
                                      rstd::move(prepared_allocation),
                                      *published,
                                      table);
            }
            physical = m_textures.ResolveCurrent(handle);
        }
        if (physical.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("resolve texture {} failed",
                                        definition.definition().name.as_str()),
            });
        }
        return InsertPrepared(use, handle, rstd::move(definition), **physical, table);
    }

    static auto InsertPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                               resource::TextureResourceRef     definition,
                               const resource::TexturePhysical& physical,
                               PreparedResourceTable&           table)
        -> Result<empty, resource::ResourceError> {
        return InsertPrepared(use,
                              handle,
                              rstd::move(definition),
                              physical.allocation.clone(),
                              physical.generation,
                              table);
    }

    static auto InsertPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                               resource::TextureResourceRef definition,
                               Arc<TextureAllocation> allocation, u64 physical_generation,
                               PreparedResourceTable& table)
        -> Result<empty, resource::ResourceError> {
        auto image      = allocation->View();
        auto completion = allocation->Completion();
        if (! table.Insert(PreparedTexture {
                .use                 = use,
                .resource            = handle,
                .definition          = rstd::move(definition),
                .physical            = rstd::move(allocation),
                .image               = rstd::move(image),
                .physical_generation = physical_generation,
                .completion          = completion,
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::DuplicateUse,
                .message = rstd::format("duplicate texture use {}", use.index),
            });
        }
        return Ok(empty {});
    }

    resource::TextureRegistry&                  m_textures;
    Option<mut_ref<dyn<TexturePrepareBackend>>> m_textures_backend;
    BufferRegistry&                             m_buffers;
    mut_ref<dyn<vulkan::BufferBackend>>         m_buffer_backend;
    ShaderRegistry&                             m_shaders;
};

inline auto ResourcePlanPrepareVisitor::VisitTexture(const resource::TexturePlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    (void)entry;
    return Ok(empty {});
}

inline auto ResourcePlanPrepareVisitor::VisitBuffer(const resource::BufferPlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    return m_service.PrepareBuffer(entry, m_table, m_providers.buffer);
}

inline auto ResourcePlanPrepareVisitor::VisitShader(const resource::ShaderPlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    return m_service.PrepareShader(entry, m_table, m_providers.shader);
}

} // namespace vrento::resource_registry
