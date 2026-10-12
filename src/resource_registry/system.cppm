module;

#include <span>

export module vrento:resource_registry.system;
import rstd;
import :resource;
import :vulkan.api;

import :resource_registry.owner;
import :resource_registry.prepared;
import :resource_registry.state;
import :resource_registry.lifetime;
import :resource_registry.local_output;
import :vulkan.texture;

export namespace vrento::resource_registry
{

using namespace rstd::prelude;

struct PipelinePreparation {
    resource::PipelineHandle   resource;
    resource::RenderPassHandle render_pass;
    vulkan::PipelineCacheKey   cache_key;
    vulkan::RenderPassCacheKey render_pass_key;
    bool                       cache_hit { false };
    u64                        cache_observed_count { 0 };
    bool                       render_pass_cache_hit { false };
    u64                        render_pass_cache_observed_count { 0 };
};

struct FramebufferPreparation {
    resource::FramebufferHandle resource;
    vulkan::FramebufferCacheKey cache_key;
    bool                        cache_hit { false };
    u64                         cache_observed_count { 0 };
};

struct RenderPassPreparation {
    resource::RenderPassHandle resource;
    vulkan::RenderPassCacheKey cache_key;
    bool                       cache_hit { false };
    u64                        cache_observed_count { 0 };
};

struct GraphicsResourcePreparer {
    using Trait                  = GraphicsResourcePreparer;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = GraphicsResourcePreparer;

        auto PreparePipeline(resource::PipelineUseHandle   pipeline_use,
                             resource::RenderPassUseHandle render_pass_use,
                             const vulkan::Device& device, vulkan::PipelineResourceRequest request)
            -> Result<PipelinePreparation, resource::ResourceError> {
            return rstd::trait_call<0>(
                this, pipeline_use, render_pass_use, device, rstd::move(request));
        }

        auto PrepareFramebuffer(resource::FramebufferUseHandle         framebuffer_use,
                                resource::RenderPassUseHandle          render_pass_use,
                                const vulkan::Device&                  device,
                                Vec<vulkan::FramebufferAttachmentDesc> attachments,
                                VkExtent2D                             extent)
            -> Result<FramebufferPreparation, resource::ResourceError> {
            return rstd::trait_call<1>(
                this, framebuffer_use, render_pass_use, device, rstd::move(attachments), extent);
        }

        auto PrepareRenderPass(resource::RenderPassUseHandle use, const vulkan::Device& device,
                               const vulkan::RenderPassResourceDesc& desc)
            -> Result<RenderPassPreparation, resource::ResourceError> {
            return rstd::trait_call<2>(this, use, device, desc);
        }

        auto PrepareDescriptor(const vulkan::Device&          device,
                               resource::PipelineLayoutHandle pipeline_layout, u32 set_index,
                               slice<DescriptorImageBinding>  images,
                               slice<DescriptorBufferBinding> buffers, DescriptorBindingReuse reuse)
            -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
            return rstd::trait_call<3>(
                this, device, pipeline_layout, set_index, images, buffers, reuse);
        }

        auto UpdateDescriptorImages(resource::DescriptorBindingHandle handle,
                                    slice<DescriptorImageBinding>     images)
            -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<4>(this, handle, images);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::PreparePipeline, &T::PrepareFramebuffer, &T::PrepareRenderPass,
                             &T::PrepareDescriptor, &T::UpdateDescriptorImages>;
};

struct ExternalResourcePreparer {
    using Trait                  = ExternalResourcePreparer;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ExternalResourcePreparer;

        auto PrepareExternal(resource::ExternalUseHandle       external_use,
                             resource::TextureUseHandle        source_use,
                             const vulkan::DeviceCapabilities& capabilities,
                             FrameSurfaceLease lease, rstd::uint32_t graphics_queue_family)
            -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<0>(this,
                                       external_use,
                                       source_use,
                                       capabilities,
                                       rstd::move(lease),
                                       graphics_queue_family);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::PrepareExternal>;
};

enum class FontAtlasUploadStatus
{
    NotResident,
    Recorded,
    Failed
};

class RenderResourceSystem {
public:
    bool Initialize(vulkan::DeviceSession session) {
        if (! m_registries.Initialize(rstd::move(session))) return false;
        auto options = m_video_options.take();
        if (options.is_some()) m_registries.Textures().SetVideoDecodeOptions(rstd::move(*options));
        return true;
    }

    auto TextureContext() -> vulkan::TextureContext& { return m_registries.Textures().Context(); }
    auto Native() -> mut_ref<dyn<vulkan::NativeSubmission>> {
        return dyn<vulkan::NativeSubmission>::from_ref(TextureContext()).as_mut_ref();
    }
    bool AttachWait(texture::Submission& work, vulkan::NativeSemaphoreWait wait) {
        return Native()->AttachWait(work, rstd::move(wait)).is_ok();
    }
    bool AttachSignal(texture::Submission& work, vulkan::NativeSemaphoreSignal signal) {
        return Native()->AttachSignal(work, rstd::move(signal)).is_ok();
    }
    bool AttachFence(texture::Submission& work, vulkan::NativeFence fence) {
        return Native()->AttachFence(work, rstd::move(fence)).is_ok();
    }
    void SetTextureProducerFactory(rstd::sync::Arc<dyn<TextureProducerFactory>> factory) {
        m_registries.Textures().SetProducerFactory(rstd::move(factory));
    }

    void Reset() {
        if (Ready()) {
            (void)TextureContext().session().wait_idle();
            (void)m_registries.Textures().Poll();
        }
        m_frame            = None();
        m_prepare_rollback = None();
        m_prepared         = PreparedResourceTable {};
        m_registries.Reset();
    }

    bool Ready() const noexcept { return m_registries.Ready(); }

    auto
    PreparePlan(const resource::ResourcePlan& plan, ResourceContentProviders providers,
                resource::ResourcePlanSections sections = resource::ResourcePlanAll,
                Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<empty, resource::ResourceError> {
        auto started = BeginPreparePlan(plan, rstd::move(providers), sections, texture_observer);
        if (started.is_err()) return Err(rstd::move(started).unwrap_err_unchecked());
        auto session = rstd::move(started).unwrap_unchecked();
        while (true) {
            auto progress = ContinuePreparePlan(session, texture_observer);
            if (progress.is_err()) return Err(rstd::move(progress).unwrap_err_unchecked());
            if (progress.unwrap_unchecked() == ResourcePrepareProgress::Complete) break;
        }
        CommitPreparePlan();
        return Ok(empty {});
    }

    auto BeginPreparePlan(const resource::ResourcePlan& plan, ResourceContentProviders providers,
                          resource::ResourcePlanSections sections = resource::ResourcePlanAll,
                          Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer =
                              None()) -> Result<ResourcePrepareSession, resource::ResourceError> {
        if (m_prepare_rollback.is_some()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("resource prepare transaction is already active"),
            });
        }
        if (! m_registries.TextureEntries().BeginPrepareTransaction()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("texture prepare transaction is already active"),
            });
        }
        auto image_backend  = dyn<TexturePrepareBackend>::from_ref(m_registries.Textures());
        auto buffer_backend = dyn<vulkan::BufferBackend>::from_ref(m_registries.BufferManager());
        ResourcePrepareService service(m_registries.TextureEntries(),
                                       Some(image_backend.as_mut_ref()),
                                       m_registries.Buffers(),
                                       buffer_backend.as_mut_ref(),
                                       m_registries.Shaders());
        auto started = service.Begin(plan, rstd::move(providers), sections, texture_observer);
        if (started.is_err()) {
            AbortPreparePlan();
            return Err(rstd::move(started).unwrap_err_unchecked());
        }
        return Ok(rstd::move(started).unwrap_unchecked());
    }

    auto ContinuePreparePlan(
        ResourcePrepareSession&                                session,
        Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<ResourcePrepareProgress, resource::ResourceError> {
        if (! CommitUploads()) {
            AbortPreparePlan();
            return Err(resource::ResourceError {
                .message = rstd::format("submit texture prepare batch failed") });
        }
        auto image_backend  = dyn<TexturePrepareBackend>::from_ref(m_registries.Textures());
        auto buffer_backend = dyn<vulkan::BufferBackend>::from_ref(m_registries.BufferManager());
        ResourcePrepareService service(m_registries.TextureEntries(),
                                       Some(image_backend.as_mut_ref()),
                                       m_registries.Buffers(),
                                       buffer_backend.as_mut_ref(),
                                       m_registries.Shaders());
        auto                   progress = service.Continue(session, texture_observer);
        if (progress.is_err()) {
            AbortPreparePlan();
            return Err(rstd::move(progress).unwrap_err_unchecked());
        }
        if (progress.unwrap_unchecked() == ResourcePrepareProgress::BatchReady) {
            return Ok(ResourcePrepareProgress::BatchReady);
        }

        const auto sections = session.Sections();
        auto       next     = rstd::move(session).TakeTable();
        auto       previous = m_prepared.clone();
        next.CarryForward(rstd::move(m_prepared), sections);
        m_prepared         = rstd::move(next);
        m_prepare_rollback = Some(PrepareRollback {
            .table = rstd::move(previous),
        });
        return Ok(ResourcePrepareProgress::Complete);
    }

    void AbortPreparePlan() {
        m_registries.Textures().DiscardPendingUploads();
        m_registries.TextureEntries().DiscardPendingUploads();
        m_registries.TextureEntries().AbortPrepareTransaction();
        auto rollback = m_prepare_rollback.take();
        if (rollback.is_none()) return;
        m_prepared = rstd::move(rollback->table);
    }

    void CommitPreparePlan() {
        m_prepare_rollback = None();
        m_registries.TextureEntries().CommitPrepareTransaction();
        auto active = m_prepared.TextureResources();
        m_registries.TextureEntries().RetainActive(active.as_slice());
    }

    auto Prepared() const -> const PreparedResourceTable& { return m_prepared; }

    void RemovePreparedGraphics(slice<resource::PipelineUseHandle>       pipelines,
                                slice<resource::RenderPassUseHandle>     render_passes,
                                slice<resource::FramebufferUseHandle>    framebuffers,
                                slice<resource::DescriptorBindingHandle> descriptors,
                                slice<resource::ExternalUseHandle>       externals) {
        for (usize index {}; index < pipelines.len(); ++index) m_prepared.Remove(pipelines[index]);
        for (usize index {}; index < render_passes.len(); ++index) {
            m_prepared.Remove(render_passes[index]);
        }
        for (usize index {}; index < framebuffers.len(); ++index) {
            m_prepared.Remove(framebuffers[index]);
        }
        for (usize index {}; index < descriptors.len(); ++index) {
            m_prepared.Remove(descriptors[index]);
        }
        for (usize index {}; index < externals.len(); ++index) {
            m_prepared.Remove(externals[index]);
        }
    }

    void RemovePreparedDescriptor(resource::DescriptorBindingHandle descriptor) {
        m_prepared.Remove(descriptor);
    }

    auto PreparePipelineLayout(const vulkan::Device& device, const PipelineLayoutRequest& request)
        -> Result<resource::PipelineLayoutHandle, resource::ResourceError> {
        auto prepared = m_registries.PipelineLayouts().Ensure(
            device, request, m_registries.DescriptorLayouts());
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        return Ok(rstd::move(prepared).unwrap_unchecked().handle);
    }

    auto PreparePipeline(resource::PipelineUseHandle   pipeline_use,
                         resource::RenderPassUseHandle render_pass_use,
                         const vulkan::Device& device, vulkan::PipelineResourceRequest request)
        -> Result<PipelinePreparation, resource::ResourceError> {
        PipelineResourceSystem system(device,
                                      m_registries.PipelineLayouts(),
                                      m_registries.PipelineCache(),
                                      m_registries.RenderPassCache());
        auto                   result = system.CreateGraphicsPipeline(rstd::move(request));
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare graphics pipeline failed"),
            });
        }
        if (! m_prepared.Insert(PreparedRenderPass {
                .use       = render_pass_use,
                .resource  = result->render_pass,
                .cache_key = result->render_pass_key.clone(),
                .physical  = result->render_pass_physical.clone(),
            }) ||
            ! m_prepared.Insert(PreparedPipeline {
                .use         = pipeline_use,
                .resource    = result->handle,
                .render_pass = result->render_pass,
                .physical    = result->pipeline.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared graphics use"),
            });
        }
        return Ok(PipelinePreparation {
            .resource                         = result->handle,
            .render_pass                      = result->render_pass,
            .cache_key                        = rstd::move(result->cache_key),
            .render_pass_key                  = rstd::move(result->render_pass_key),
            .cache_hit                        = result->cache_hit,
            .cache_observed_count             = result->cache_observed_count,
            .render_pass_cache_hit            = result->render_pass_cache_hit,
            .render_pass_cache_observed_count = result->render_pass_cache_observed_count,
        });
    }

    auto PrepareFramebuffer(resource::FramebufferUseHandle         framebuffer_use,
                            resource::RenderPassUseHandle          render_pass_use,
                            const vulkan::Device&                  device,
                            Vec<vulkan::FramebufferAttachmentDesc> attachments, VkExtent2D extent)
        -> Result<FramebufferPreparation, resource::ResourceError> {
        auto render_pass = m_prepared.Resolve(render_pass_use);
        if (render_pass.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared render pass unavailable"),
            });
        }
        FramebufferResourceSystem system(
            device, m_registries.FramebufferCache(), m_registries.FramebufferDiagnostics());
        auto result = system.CreateFramebuffer(vulkan::FramebufferResourceRequest {
            .render_pass     = **(**render_pass).physical,
            .render_pass_key = (**render_pass).cache_key.clone(),
            .attachments     = rstd::move(attachments),
            .extent          = extent,
        });
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare framebuffer failed"),
            });
        }
        if (! m_prepared.Insert(PreparedFramebuffer {
                .use      = framebuffer_use,
                .resource = result->handle,
                .physical = result->framebuffer.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared framebuffer use"),
            });
        }
        return Ok(FramebufferPreparation {
            .resource             = result->handle,
            .cache_key            = rstd::move(result->cache_key),
            .cache_hit            = result->cache_hit,
            .cache_observed_count = result->cache_observed_count,
        });
    }

    auto PrepareRenderPass(resource::RenderPassUseHandle use, const vulkan::Device& device,
                           const vulkan::RenderPassResourceDesc& desc)
        -> Result<RenderPassPreparation, resource::ResourceError> {
        auto result = m_registries.RenderPasses().Ensure(device, desc);
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare render pass failed"),
            });
        }
        if (! m_prepared.Insert(PreparedRenderPass {
                .use       = use,
                .resource  = result->handle,
                .cache_key = result->cache_key.clone(),
                .physical  = result->render_pass.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared render pass use"),
            });
        }
        return Ok(RenderPassPreparation {
            .resource             = result->handle,
            .cache_key            = rstd::move(result->cache_key),
            .cache_hit            = result->cache_hit,
            .cache_observed_count = result->cache_observed_count,
        });
    }

    auto PrepareDescriptor(const vulkan::Device&          device,
                           resource::PipelineLayoutHandle pipeline_layout, u32 set_index,
                           slice<DescriptorImageBinding>  images,
                           slice<DescriptorBufferBinding> buffers, DescriptorBindingReuse reuse)
        -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
        auto layout_resource = m_registries.PipelineLayouts().Resolve(pipeline_layout);
        if (layout_resource.is_none() ||
            rstd::as_cast<usize>(set_index) >= (**layout_resource).descriptor_layouts.len()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("pipeline descriptor layout unavailable"),
            });
        }
        auto layout = m_registries.DescriptorLayouts().Resolve(
            (**layout_resource).descriptor_layouts[rstd::as_cast<usize>(set_index)]);
        if (layout.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("descriptor layout unavailable"),
            });
        }
        auto prepared = m_registries.Descriptors().Prepare(
            device, set_index.to_primitive(), **layout, images, buffers, reuse);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        auto binding = rstd::move(prepared).unwrap_unchecked();
        auto handle  = binding.handle;
        if (! m_prepared.Insert(rstd::move(binding))) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate descriptor binding {}", handle.index),
            });
        }
        return Ok(handle);
    }

    auto UpdateDescriptorImages(resource::DescriptorBindingHandle handle,
                                slice<DescriptorImageBinding>     images)
        -> Result<empty, resource::ResourceError> {
        auto prepared = m_prepared.ResolveMut(handle);
        if (prepared.is_none()) {
            return Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingDefinition,
                .message =
                    rstd::format("prepared descriptor binding {} is unavailable", handle.index),
            });
        }
        return (**prepared).UpdateImages(images);
    }

    bool HasPendingUploads() const {
        return m_registries.BufferManager().HasPendingUploads() ||
               m_registries.Textures().HasPendingUploads();
    }

    auto BeginFrame() -> Result<texture::Submission, texture::Error> {
        if (! Ready()) return Err(texture::Error { texture::ErrorKind::Closed });
        if (m_frame.is_some()) {
            if ((*m_frame)->receipt.uncertain())
                return Err(texture::Error { texture::ErrorKind::Closed });
            if (! (*m_frame)->receipt.cancelled() && ! (*m_frame)->receipt.submitted())
                return Err(texture::Error { texture::ErrorKind::Busy });
            m_frame = None();
            m_registries.Textures().CancelRecording();
        }
        auto window = m_registries.Textures().WaitWindow();
        if (window.is_err()) return Err(window.unwrap_err_unchecked());
        auto begun = TextureContext().Begin();
        if (begun.is_err()) return Err(begun.unwrap_err_unchecked());
        auto work  = rstd::move(begun).unwrap_unchecked();
        auto frame = rstd::sync::Arc<FrameSubmissionLease>::make(
            m_registries.BufferOwner(), m_prepared.Leases(), work.receipt());
        work.retain(frame.clone());
        m_frame = Some(rstd::move(frame));
        return Ok(rstd::move(work));
    }

    bool RecordPendingUploads(texture::Submission& work) {
        if (m_frame.is_none() || (*m_frame)->uploads_recorded) return false;
        m_registries.TextureEntries().ReservePublications();
        if (! m_registries.Textures().RecordPendingUploads(work)) return false;
        auto native = vulkan::native_recording(work, TextureContext().session());
        if (native.is_err()) return false;
        vvk::CommandBuffer command(native.unwrap_unchecked(),
                                   TextureContext().session().device().handle().Dispatch());
        if (! m_registries.BufferManager().RecordPendingUploads(command,
                                                                (*m_frame)->recorded_buffers))
            return false;
        (*m_frame)->uploads_recorded = true;
        return true;
    }

    auto SubmitFrame(texture::Submission work) -> Result<texture::SubmissionToken, texture::Error> {
        if (m_frame.is_none()) return Err(texture::Error { texture::ErrorKind::Invalid });
        auto frame = m_frame->clone();
        if (! frame->uploads_recorded && HasPendingUploads())
            return Err(texture::Error { texture::ErrorKind::Invalid });
        if (frame->recorded_buffers.Valid()) {
            auto committed = m_registries.BufferManager().CommitRecordedUploads(
                rstd::move(frame->recorded_buffers));
            if (committed.is_none()) return Err(texture::Error { texture::ErrorKind::Backend });
            frame->buffer_uploads = Some(rstd::move(*committed));
        }
        auto submitted = TextureContext().Submit(rstd::move(work));
        if (submitted.is_err()) {
            if (frame->receipt.cancelled()) {
                auto buffers = frame->buffer_uploads.take();
                if (buffers.is_some() &&
                    ! m_registries.BufferManager().RestoreUploads(rstd::move(*buffers)))
                    return Err(texture::Error { texture::ErrorKind::Backend });
                m_registries.Textures().CancelRecording();
                m_frame = None();
            }
            return Err(submitted.unwrap_err_unchecked());
        }
        auto token = rstd::move(submitted).unwrap_unchecked();
        if (! frame->receipt.submitted())
            return Err(texture::Error { texture::ErrorKind::Backend });
        if (frame->uploads_recorded &&
            ! m_registries.Textures().ConfirmSubmitted(frame->receipt, token))
            return Err(texture::Error { texture::ErrorKind::Backend });
        if (frame->buffer_uploads.is_some())
            m_registries.Buffers().MarkUploadsSubmitted(frame->buffer_uploads->Tickets(), None());
        m_registries.TextureEntries().PublishSubmitted();
        m_frame = None();
        return Ok(rstd::move(token));
    }

    bool CommitUploads() {
        if (! Ready()) return false;
        m_registries.TextureEntries().ReservePublications();
        auto result = m_registries.Textures().Flush();
        m_registries.TextureEntries().PublishSubmitted();
        return result.is_ok();
    }

    bool WaitSubmission(const texture::SubmissionToken& token,
                        texture::Serial                 timeout = 5'000'000'000) {
        if (! Ready() || TextureContext().wait(token, timeout).is_err()) return false;
        return m_registries.Textures().Poll().is_ok();
    }

    bool WaitForUploads(texture::Serial timeout = 5'000'000'000) {
        if (! CommitUploads()) return false;
        return m_registries.Textures().WaitForUploads(timeout).is_ok();
    }

    bool UseTexture(texture::Submission& work, resource::TextureUseHandle use,
                    TextureStateKind target, TextureSubresourceRange range = {},
                    bool discard = false, usize slot = usize(),
                    texture::Stages stages = texture::Stages::None) {
        auto prepared = m_prepared.Resolve(use);
        return prepared.is_some() &&
               UsePreparedTexture(work, **prepared, target, range, discard, slot, stages);
    }

    bool GenerateMipmaps(texture::Submission& work, resource::TextureUseHandle use) {
        auto prepared = m_prepared.Resolve(use);
        if (prepared.is_none()) return false;
        const auto slots = (**prepared).physical->Slots();
        if (slots.is_empty()) return false;
        const auto& selected = slots[usize()];
        const auto& desc     = selected.view.texture().description();
        const auto  range    = selected.view.description().range;
        if (desc.samples != 1 || range.aspect != texture::Aspect::Color ||
            ! texture::has(desc.usage, texture::Usage::TransferSource) ||
            ! texture::has(desc.usage, texture::Usage::TransferDestination))
            return false;
        if (range.mip_count <= 1) return UseTexture(work, use, TextureStateKind::Sampled);
        auto       features = TextureContext()
                                  .session()
                                  .device()
                                  .gpu()
                                  .GetFormatProperties(vulkan::ToVkType(desc.format))
                                  .optimalTilingFeatures;
        const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((features & required) != required) return false;
        auto image = vulkan::native_texture(selected.view.texture());
        if (image.is_err()) return false;
        for (texture::Count mip = range.first_mip + 1; mip < range.first_mip + range.mip_count;
             ++mip) {
            TextureSubresourceRange source { VK_IMAGE_ASPECT_COLOR_BIT,
                                             u32(mip - 1),
                                             u32(1),
                                             u32(range.first_layer),
                                             u32(range.layer_count) };
            auto                    destination = source;
            destination.base_mip_level          = u32(mip);
            if (! UseTexture(work, use, TextureStateKind::TransferSource, source) ||
                ! UseTexture(work, use, TextureStateKind::TransferDestination, destination, true))
                return false;
            auto command = vulkan::native_recording(work, TextureContext().session());
            if (command.is_err()) return false;
            const auto  from = texture::mip_extent(desc, mip - 1);
            const auto  to   = texture::mip_extent(desc, mip);
            VkImageBlit blit {};
            blit.srcSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, range.first_layer, range.layer_count
            };
            blit.dstSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, mip, range.first_layer, range.layer_count
            };
            blit.srcOffsets[1] = { rstd::int32_t(from.width), rstd::int32_t(from.height), 1 };
            blit.dstOffsets[1] = { rstd::int32_t(to.width), rstd::int32_t(to.height), 1 };
            TextureContext().session().device().handle().Dispatch().vkCmdBlitImage(
                command.unwrap_unchecked(),
                image.unwrap_unchecked(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image.unwrap_unchecked(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &blit,
                VK_FILTER_LINEAR);
        }
        return UseTexture(work, use, TextureStateKind::Sampled);
    }

    auto UpdateBuffer(resource::BufferUseHandle use, slice<u8> content)
        -> Result<empty, resource::ResourceError> {
        auto prepared = m_prepared.Resolve(use);
        if (prepared.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared buffer use {} is unavailable", use.index),
            });
        }
        auto backend = dyn<vulkan::BufferBackend>::from_ref(m_registries.BufferManager());
        return m_registries.Buffers().Update(
            (**prepared).buffer.resource, content, backend.as_mut_ref());
    }

    auto PrepareExternal(resource::ExternalUseHandle       external_use,
                         resource::TextureUseHandle        source_use,
                         const vulkan::DeviceCapabilities& capabilities, FrameSurfaceLease lease,
                         rstd::uint32_t graphics_queue_family)
        -> Result<empty, resource::ResourceError> {
        auto source = m_prepared.Resolve(source_use);
        if (source.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared external source unavailable"),
            });
        }
        auto prepared =
            m_registries.External().Prepare(capabilities, rstd::move(lease), graphics_queue_family);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        m_prepared.Insert(PreparedExternalUse {
            .use   = external_use,
            .frame = rstd::move(prepared).unwrap_unchecked(),
        });
        return Ok(empty {});
    }

    void Collect(mut_ref<dyn<vulkan::MemoryBudgetSource>> memory) { Collect(memory.as_ref()); }
    void Collect(ref<dyn<vulkan::MemoryBudgetSource>> memory) {
        m_registries.PipelineCache().PruneExpired();
        m_registries.PipelineLayouts().PruneExpired();
        m_registries.RenderPassCache().PruneExpired();
        m_registries.FramebufferCache().PruneExpired();
        m_registries.Memory().Refresh(memory);
        if (! m_registries.Memory().ShouldEvictTransient()) return;
        m_registries.TextureEntries().EvictUnused();
        m_registries.Buffers().EvictUnused();
        m_registries.BufferManager().Trim();
        (void)m_registries.Textures().Poll();
    }

    bool PumpVideoTextures(double seconds) {
        return m_registries.TextureEntries().Pump(TextureContext(), seconds).is_ok();
    }
    void SetVideoDecodeOptions(vulkan::MediaDecodeOptions options) {
        if (Ready())
            m_registries.Textures().SetVideoDecodeOptions(rstd::move(options));
        else
            m_video_options = Some(rstd::move(options));
    }
    FontAtlasUploadStatus QueueRegion(texture::Submission& work, ref<str> key,
                                      texture::TextureUpload upload, usize slot = usize()) {
        auto handle =
            m_registries.TextureEntries().Find(resource::TextureRequestKind::Imported, key);
        if (handle.is_none()) return FontAtlasUploadStatus::NotResident;
        auto physical = m_registries.TextureEntries().ResolveCurrent(*handle);
        if (physical.is_none()) return FontAtlasUploadStatus::NotResident;
        const auto slots = (**physical).allocation->Slots();
        if (! work.valid() || slot >= slots.len()) return FontAtlasUploadStatus::Failed;
        upload.target = slots[slot].view;
        work.retain((**physical).allocation.clone());
        return work.commands()->Upload(rstd::move(upload)).is_ok() ? FontAtlasUploadStatus::Recorded
                                                                   : FontAtlasUploadStatus::Failed;
    }
    auto CreateLocalSwapchain(const vulkan::Device& device, unsigned width, unsigned height,
                              VkImageTiling tiling) -> Option<ExSwapchainOwner> {
        if (! Ready() || *device.handle() != *TextureContext().session().device().handle())
            return None();
        return CreateSessionLocalOutput(TextureContext(), width, height, tiling);
    }
    void ClearTextures() {
        m_prepare_rollback = None();
        m_prepared         = PreparedResourceTable {};
        m_registries.Textures().DiscardPendingUploads();
        m_registries.TextureEntries().Reset();
        m_registries.Textures().ClearProducers();
    }
    void ClearPreparedGraphics() { m_prepared.ClearPreparedState(); }
    void ClearTransientTextures() { m_registries.TextureEntries().ClearGraphResources(); }
    void EvictUnusedBuffers() { m_registries.Buffers().EvictUnused(); }

    auto PipelineDiagnostics() -> PipelineCacheDiagnostics& {
        return m_registries.PipelineDiagnostics();
    }
    auto FramebufferDiagnostics() -> FramebufferCacheDiagnostics& {
        return m_registries.FramebufferDiagnostics();
    }

private:
    struct PrepareRollback {
        PreparedResourceTable table;
    };

    ResourceRegistries                            m_registries;
    PreparedResourceTable                         m_prepared;
    Option<PrepareRollback>                       m_prepare_rollback;
    Option<rstd::sync::Arc<FrameSubmissionLease>> m_frame;
    Option<vulkan::MediaDecodeOptions>            m_video_options;
};

} // namespace vrento::resource_registry

export namespace rstd
{

template<>
struct Impl<vrento::resource_registry::GraphicsResourcePreparer,
            vrento::resource_registry::RenderResourceSystem>
    : ImplBase<vrento::resource_registry::RenderResourceSystem> {
    auto PreparePipeline(vrento::resource::PipelineUseHandle     pipeline_use,
                         vrento::resource::RenderPassUseHandle   render_pass_use,
                         const vrento::vulkan::Device&           device,
                         vrento::vulkan::PipelineResourceRequest request)
        -> Result<vrento::resource_registry::PipelinePreparation, vrento::resource::ResourceError> {
        return this->self().PreparePipeline(
            pipeline_use, render_pass_use, device, rstd::move(request));
    }

    auto PrepareFramebuffer(vrento::resource::FramebufferUseHandle         framebuffer_use,
                            vrento::resource::RenderPassUseHandle          render_pass_use,
                            const vrento::vulkan::Device&                  device,
                            Vec<vrento::vulkan::FramebufferAttachmentDesc> attachments,
                            VkExtent2D                                     extent)
        -> Result<vrento::resource_registry::FramebufferPreparation,
                  vrento::resource::ResourceError> {
        return this->self().PrepareFramebuffer(
            framebuffer_use, render_pass_use, device, rstd::move(attachments), extent);
    }

    auto PrepareRenderPass(vrento::resource::RenderPassUseHandle         use,
                           const vrento::vulkan::Device&                 device,
                           const vrento::vulkan::RenderPassResourceDesc& desc)
        -> Result<vrento::resource_registry::RenderPassPreparation,
                  vrento::resource::ResourceError> {
        return this->self().PrepareRenderPass(use, device, desc);
    }

    auto PrepareDescriptor(const vrento::vulkan::Device&          device,
                           vrento::resource::PipelineLayoutHandle pipeline_layout, u32 set_index,
                           slice<vrento::resource_registry::DescriptorImageBinding>  images,
                           slice<vrento::resource_registry::DescriptorBufferBinding> buffers,
                           vrento::resource_registry::DescriptorBindingReuse         reuse)
        -> Result<vrento::resource::DescriptorBindingHandle, vrento::resource::ResourceError> {
        return this->self().PrepareDescriptor(
            device, pipeline_layout, set_index, images, buffers, reuse);
    }

    auto UpdateDescriptorImages(vrento::resource::DescriptorBindingHandle                handle,
                                slice<vrento::resource_registry::DescriptorImageBinding> images)
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().UpdateDescriptorImages(handle, images);
    }
};

template<>
struct Impl<vrento::resource_registry::ExternalResourcePreparer,
            vrento::resource_registry::RenderResourceSystem>
    : ImplBase<vrento::resource_registry::RenderResourceSystem> {
    auto PrepareExternal(vrento::resource::ExternalUseHandle       external_use,
                         vrento::resource::TextureUseHandle        source_use,
                         const vrento::vulkan::DeviceCapabilities& capabilities,
                         vrento::FrameSurfaceLease lease, rstd::uint32_t graphics_queue_family)
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().PrepareExternal(
            external_use, source_use, capabilities, rstd::move(lease), graphics_queue_family);
    }
};

template<>
struct Impl<vrento::resource::BufferContentWriter, vrento::resource_registry::RenderResourceSystem>
    : ImplBase<vrento::resource_registry::RenderResourceSystem> {
    auto UpdateBuffer(vrento::resource::BufferUseHandle use, slice<u8> content)
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().UpdateBuffer(use, content);
    }
};

} // namespace rstd
