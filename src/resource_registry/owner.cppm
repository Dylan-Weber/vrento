export module vrento:resource_registry.owner;
import rstd;
import :vulkan.api;

import :resource_registry.texture_registry;
import :resource_registry.buffer_registry;
import :resource_registry.shader_registry;
import :resource_registry.descriptor;
import :resource_registry.lifetime;
import :resource_registry.policy;
import :resource_registry.external;
import :resource_registry.graphics;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::resource_registry
{

class ResourceRegistries {
public:
    ResourceRegistries() = default;
    ~ResourceRegistries() { Reset(); }
    ResourceRegistries(const ResourceRegistries&)            = delete;
    ResourceRegistries& operator=(const ResourceRegistries&) = delete;
    ResourceRegistries(ResourceRegistries&&)                 = delete;
    ResourceRegistries& operator=(ResourceRegistries&&)      = delete;

    bool Initialize(vulkan::DeviceSession session) {
        return TryInitialize(rstd::move(session)).is_ok();
    }
    auto TryInitialize(vulkan::DeviceSession session) -> Result<empty, resource::ResourceError> {
        if (! session.valid() || session.lost())
            return Err(resource::ResourceError::backend(vulkan::BackendError {
                "initialize resource registries"_Str, vulkan::BackendCause::Closed() }));
        if (Ready()) {
            if (Textures().Context().session().epoch() == session.epoch()) return Ok(empty {});
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Device,
                .message = "resource registries belong to another device"_Str });
        }

        auto texture_context = vulkan::TextureContext::create(session);
        if (texture_context.is_err())
            return Err(resource::ResourceError::texture(texture_context.unwrap_err(),
                                                        "initialize texture context"_Str));
        auto buffer_manager = rstd::sync::Arc<RegistryBufferOwner>::make(session);
        (void)buffer_manager->manager.init();
        m_shared_graphics = Some(mut_ref<vulkan::GraphicsResources>::from_raw_parts(
            rstd::addressof(session.graphics_resources())));

        m_textures =
            Some(Box<RegistryTextureContext>::make(rstd::move(texture_context).unwrap_unchecked()));
        m_buffer_manager = Some(rstd::move(buffer_manager));
        Textures().SetUploadPublisher(
            dyn<TextureUploadPublisher>::from_ref(m_texture_entries).as_mut_ref());
        return Ok(empty {});
    }

    void Reset() {
        if (m_textures.is_some()) {
            (void)Textures().DrainProducers(u64(5'000'000'000));
            (void)Textures().Context().session().wait_idle();
            (void)Textures().Poll();
        }
        m_pipeline_cache.Reset();
        m_framebuffer_cache.Reset();
        m_descriptor_system.Reset();
        m_pipeline_layouts.Reset();
        m_descriptor_layouts.Reset();
        m_render_pass_cache.Reset();
        m_shared_graphics = None();
        m_buffer_entries.Reset();
        m_texture_entries.Reset();
        m_textures       = None();
        m_buffer_manager = None();
        m_shader_entries.Reset();
        m_memory.Reset();
        m_pipeline_diagnostics.Reset();
        m_framebuffer_diagnostics.Reset();
    }

    bool Ready() const noexcept { return m_textures.is_some() && m_buffer_manager.is_some(); }

    auto Textures() -> RegistryTextureContext& { return *m_textures->get(); }
    auto Textures() const -> const RegistryTextureContext& {
        return *m_textures->as_ptr().as_raw_ptr();
    }
    auto BufferOwner() -> rstd::sync::Arc<RegistryBufferOwner> { return m_buffer_manager->clone(); }
    auto BufferManager() -> vulkan::BufferManager& { return (*m_buffer_manager)->manager; }
    auto BufferManager() const -> const vulkan::BufferManager& {
        return (*m_buffer_manager)->manager;
    }

    auto TextureEntries() -> resource::TextureRegistry& { return m_texture_entries; }
    auto Buffers() -> BufferRegistry& { return m_buffer_entries; }
    auto Buffers() const -> const BufferRegistry& { return m_buffer_entries; }
    auto Shaders() -> ShaderRegistry& { return m_shader_entries; }
    auto DescriptorLayouts() -> DescriptorLayoutRegistry& {
        return m_shared_graphics ? (*m_shared_graphics)->descriptors : m_descriptor_layouts;
    }
    auto PipelineLayouts() -> PipelineLayoutRegistry& {
        return m_shared_graphics ? (*m_shared_graphics)->layouts : m_pipeline_layouts;
    }
    auto Descriptors() -> DescriptorSystem& { return m_descriptor_system; }
    auto Memory() -> MemoryBudgetPolicy& { return m_memory; }
    auto External() -> ExternalResourceBridge& { return m_external; }
    auto Pipelines() -> PipelineRegistry& {
        return m_shared_graphics ? (*m_shared_graphics)->pipelines : m_pipeline_cache;
    }
    auto PipelineCache() -> PipelineRegistry& { return Pipelines(); }
    auto RenderPasses() -> RenderPassRegistry& {
        return m_shared_graphics ? (*m_shared_graphics)->passes : m_render_pass_cache;
    }
    auto RenderPassCache() -> RenderPassRegistry& { return RenderPasses(); }
    auto Framebuffers() -> FramebufferRegistry& { return m_framebuffer_cache; }
    auto FramebufferCache() -> FramebufferRegistry& { return m_framebuffer_cache; }
    auto PipelineDiagnostics() -> PipelineCacheDiagnostics& { return m_pipeline_diagnostics; }
    auto FramebufferDiagnostics() -> FramebufferCacheDiagnostics& {
        return m_framebuffer_diagnostics;
    }

private:
    Option<Box<RegistryTextureContext>>          m_textures;
    Option<mut_ref<vulkan::GraphicsResources>>   m_shared_graphics;
    Option<rstd::sync::Arc<RegistryBufferOwner>> m_buffer_manager;
    resource::TextureRegistry                    m_texture_entries;
    BufferRegistry                               m_buffer_entries;
    ShaderRegistry                               m_shader_entries;
    DescriptorLayoutRegistry                     m_descriptor_layouts;
    PipelineLayoutRegistry                       m_pipeline_layouts;
    DescriptorSystem                             m_descriptor_system;
    MemoryBudgetPolicy                           m_memory;
    ExternalResourceBridge                       m_external;
    PipelineRegistry                             m_pipeline_cache;
    RenderPassRegistry                           m_render_pass_cache;
    FramebufferRegistry                          m_framebuffer_cache;
    PipelineCacheDiagnostics                     m_pipeline_diagnostics;
    FramebufferCacheDiagnostics                  m_framebuffer_diagnostics;
};

} // namespace vrento::resource_registry
