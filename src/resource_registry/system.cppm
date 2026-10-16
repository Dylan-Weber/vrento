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
import :vulkan.pipeline_prepare;
import :vulkan.graph;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace vrento::resource_registry
{

class PreparedResourceUploads {
    vulkan::DeviceSession                 session_;
    rstd::sync::Arc<FrameSubmissionLease> frame_;
    texture::Serial                       recording_;
    Vec<PreparedTextureUpdate>            updates_;
    vulkan::BufferUploadSnapshot          buffer_writes_;

public:
    PreparedResourceUploads(vulkan::DeviceSession                 session,
                            rstd::sync::Arc<FrameSubmissionLease> frame, texture::Serial recording,
                            Vec<PreparedTextureUpdate> updates)
        : session_(rstd::move(session)),
          frame_(rstd::move(frame)),
          recording_(recording),
          updates_(rstd::move(updates)),
          buffer_writes_(frame_->recorded_buffers.Snapshot()) {}
    auto BufferWrites() const -> Vec<vulkan::BufferUploadWrite> {
        return frame_->recorded_buffers.Writes();
    }
    auto BufferBinding(const vulkan::BufferAllocation& allocation) const
        -> rstd::sync::Arc<dyn<vulkan::GraphBinding>> {
        Vec<vulkan::GraphRange> writes;
        for (const auto& write : buffer_writes_.ranges(allocation.identity()))
            writes.push({ .offset = write.offset, .size = write.size });
        return rstd::sync::Arc<dyn<vulkan::GraphBinding>>::make(vulkan::GraphUploadedBufferBinding(
            session_, allocation.clone(), frame_->receipt, rstd::move(writes)));
    }
    auto Writes() const -> Vec<texture::TextureAccess> {
        Vec<texture::TextureAccess> writes;
        for (const auto& update : updates_)
            for (auto write : update.Writes()) writes.push(rstd::move(write));
        return writes;
    }
    auto Add(PreparedTextureUpdate update) -> Result<empty, texture::Error> {
        if (frame_->uploads_recorded) return Err(texture::Error { texture::ErrorKind::Invalid });
        updates_.push(rstd::move(update));
        return Ok(empty {});
    }
    auto Record(texture::Submission& work) -> Result<empty, resource::ResourceError> {
        auto recording = vulkan::texture_recording(work, session_);
        if (recording.is_err())
            return Err(
                resource::ResourceError::texture(recording.unwrap_err(), "record uploads"_Str));
        if (recording.unwrap()->identity != recording_ || frame_->uploads_recorded)
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Invalid,
                                                 .message = "wrong or recorded upload batch"_Str });
        vvk::CommandBuffer command(recording.unwrap()->command,
                                   session_.device().handle().Dispatch());
        auto               buffers =
            frame_->buffer_owner->manager.TryRecordUploads(command, frame_->recorded_buffers);
        if (buffers.is_err()) return Err(resource::ResourceError::backend(buffers.unwrap_err()));
        for (const auto& update : updates_) {
            auto result = update.Record(work, session_);
            if (result.is_err())
                return Err(resource::ResourceError::texture(result.unwrap_err(),
                                                            "record texture update"_Str));
        }
        frame_->uploads_recorded = true;
        // Staging owns recorded bytes; keep only access descriptions with the frame.
        for (auto& update : updates_)
            for (auto& upload : update.uploads) upload.bytes = {};
        return Ok(empty {});
    }
};

struct PipelinePreparation {
    resource::PipelineHandle               resource;
    resource::RenderPassHandle             render_pass;
    vulkan::PipelineCacheKey               cache_key;
    vulkan::RenderPassCacheKey             render_pass_key;
    bool                                   cache_hit { false };
    u64                                    cache_observed_count { 0 };
    bool                                   render_pass_cache_hit { false };
    u64                                    render_pass_cache_observed_count { 0 };
    rstd::sync::Arc<PipelineResourceEntry> physical;
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
            -> Result<vrento::Preparation<PipelinePreparation>, resource::ResourceError> {
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
        auto PrepareTextureDescriptor(const vulkan::Device&          device,
                                      resource::PipelineLayoutHandle layout, u32 set,
                                      slice<DescriptorTextureBinding> textures,
                                      slice<DescriptorBufferBinding>  buffers)
            -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
            return rstd::trait_call<5>(this, device, layout, set, textures, buffers);
        }
        auto UpdateDescriptorTextures(resource::DescriptorBindingHandle handle,
                                      slice<DescriptorTextureBinding>   textures)
            -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<6>(this, handle, textures);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::PreparePipeline, &T::PrepareFramebuffer, &T::PrepareRenderPass,
                             &T::PrepareDescriptor, &T::UpdateDescriptorImages,
                             &T::PrepareTextureDescriptor, &T::UpdateDescriptorTextures>;
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

enum class ResourcePrepareState
{
    Idle,
    Preparing,
    Prepared,
    Committed,
    Failed,
    Cancelled
};
struct ResourcePrepareStatus {
    u64                             id {};
    ResourcePrepareState            state { ResourcePrepareState::Idle };
    resource::ResourcePlanSections  sections {};
    Option<resource::ResourceError> error;
    auto                            clone() const -> ResourcePrepareStatus {
        return { id, state, sections, error ? Some(error->clone()) : None() };
    }
};

struct ResourceStatistics {
    u64                            buffer_allocations {};
    vulkan::BufferUploadStatistics buffer_uploads;
    TexturePreparationStatistics   textures;
};
class RenderResourceSystem {
public:
    auto Statistics() const -> ResourceStatistics {
        if (! Ready()) return {};
        return { m_registries.Buffers().Allocations(),
                 m_registries.BufferManager().statistics(),
                 m_registries.Textures().Statistics() };
    }
    ~RenderResourceSystem() {
        m_prepare_session = None();
        ClosePipelinePreparation();
    }
    auto SetPipelinePreparationMode(vulkan::PipelinePreparationMode mode)
        -> Result<empty, resource::ResourceError> {
        if (! Ready() || ! m_pipeline_recipes.is_empty())
            return Err(resource::ResourceError { resource::ResourceErrorKind::Busy,
                                                 rstd::format("pipeline preparation is busy") });
        auto created = vulkan::PipelinePreparationQueue::create(TextureContext().session(), mode);
        if (created.is_err()) return Err(rstd::move(created).unwrap_err());
        m_pipeline_preparation = Some(created.unwrap());
        return Ok(empty {});
    }
    bool Initialize(vulkan::DeviceSession session) {
        return TryInitialize(rstd::move(session)).is_ok();
    }
    auto TryInitialize(vulkan::DeviceSession session) -> Result<empty, resource::ResourceError> {
        auto initialized = m_registries.TryInitialize(rstd::move(session));
        if (initialized.is_err()) return initialized;
        return Ok(empty {});
    }

    auto TryPrepareImage(rstd::sync::Arc<Image> image) -> Result<bool, resource::ResourceError> {
        if (! Ready()) return Ok(false);
        return m_registries.Textures().TryPrepareImage(rstd::move(image));
    }
    auto TextureContext() -> vulkan::TextureContext& { return m_registries.Textures().Context(); }
    auto RenderedTarget(ref<str> name)
        -> Result<Preparation<texture::TextureViewRef>, resource::ResourceError> {
        return RenderedTarget(
            m_registries.TextureEntries().Find(resource::TextureRequestKind::RenderTarget, name));
    }
    auto RenderedTarget(const resource::TextureResourceRef& definition)
        -> Result<Preparation<texture::TextureViewRef>, resource::ResourceError> {
        return RenderedTarget(m_registries.TextureEntries().Find(definition));
    }

private:
    auto RenderedTarget(Option<resource::TextureHandle> handle)
        -> Result<Preparation<texture::TextureViewRef>, resource::ResourceError> {
        if (! Ready())
            return Err(resource::ResourceError { resource::ResourceErrorKind::Closed,
                                                 rstd::format("resources are closed") });
        auto physical = handle ? m_registries.TextureEntries().ResolveCurrent(*handle) : None();
        if (! physical || (**physical).allocation->Slots().is_empty())
            return Err(resource::ResourceError { resource::ResourceErrorKind::MissingContent,
                                                 rstd::format("rendered target unavailable") });
        auto view    = (**physical).allocation->Slots()[usize()].view;
        auto checked = view.texture().check();
        if (checked.is_err())
            return Err(resource::ResourceError::texture(checked.unwrap_err(),
                                                        "check rendered target"_Str));
        if (! view.texture().ready().valid()) return Ok(PendingPreparation {});
        texture::ViewDesc selected;
        selected.range = { view.description().range.aspect, 0, 1, 0, 1 };
        auto result    = TextureContext().CreateView(view.texture(), selected);
        if (result.is_err())
            return Err(resource::ResourceError::texture(result.unwrap_err(),
                                                        "create rendered target view"_Str));
        return Ok(result.unwrap());
    }

public:
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
    auto SetTextureProducerFactory(rstd::sync::Arc<dyn<TextureProducerFactory>> factory)
        -> Result<empty, resource::ResourceError> {
        if (! Ready())
            return Err(
                resource::ResourceError { resource::ResourceErrorKind::Closed,
                                          rstd::format("resource system is not initialized") });
        return m_registries.Textures().SetProducerFactory(rstd::move(factory));
    }

    void Reset() {
        AbortPreparePlan();
        m_prepare_status = {};
        ClosePipelinePreparation();
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
        return CommitPreparePlan(session);
    }

    auto BeginPreparePlan(const resource::ResourcePlan& plan, ResourceContentProviders providers,
                          resource::ResourcePlanSections sections = resource::ResourcePlanAll,
                          Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer =
                              None()) -> Result<u64, resource::ResourceError> {
        if (PrepareActive()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Busy,
                .message = rstd::format("resource prepare transaction is already active"),
            });
        }
        if (! Ready())
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Closed,
                                                 .message = rstd::format("resources are closed") });
        m_prepare_status = {
            ++m_next_prepare_id, ResourcePrepareState::Preparing, sections, None()
        };
        if (! m_registries.TextureEntries().BeginPrepareTransaction()) {
            auto error = resource::ResourceError {
                .kind    = resource::ResourceErrorKind::Busy,
                .message = rstd::format("texture prepare transaction is already active")
            };
            m_prepare_status.state = ResourcePrepareState::Failed;
            m_prepare_status.error = Some(error.clone());
            return Err(rstd::move(error));
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
            auto error = rstd::move(started).unwrap_err_unchecked();
            FailPreparePlan(error);
            return Err(rstd::move(error));
        }
        m_prepare_session.insert(rstd::move(started).unwrap_unchecked());
        return Ok(m_prepare_status.id);
    }

    auto ContinuePreparePlan(
        u64 id, Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<ResourcePrepareProgress, resource::ResourceError> {
        if (id != m_prepare_status.id ||
            m_prepare_status.state != ResourcePrepareState::Preparing || ! m_prepare_session) {
            auto error =
                resource::ResourceError { .kind = resource::ResourceErrorKind::StaleTransaction,
                                          .message =
                                              "resource prepare transaction is not active"_Str };
            error.contexts.push(
                resource::ResourceErrorContext::Transaction(id, m_prepare_status.id));
            return Err(rstd::move(error));
        }
        auto& session   = *m_prepare_session;
        auto  committed = TryCommitUploads();
        if (committed.is_err()) {
            auto error = resource::ResourceError::texture(committed.unwrap_err(),
                                                          "submit texture prepare batch"_Str);
            FailPreparePlan(error);
            return Err(rstd::move(error));
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
            auto error = rstd::move(progress).unwrap_err_unchecked();
            FailPreparePlan(error);
            return Err(rstd::move(error));
        }
        if (progress.unwrap_unchecked() == ResourcePrepareProgress::BatchReady) {
            return Ok(ResourcePrepareProgress::BatchReady);
        }

        const auto sections = session.Sections();
        auto       next     = rstd::move(session).TakeTable();
        auto       previous = m_prepared.clone();
        auto       changed  = sections == resource::ResourcePlanTextures
                                  ? next.ChangedTextureBindings(m_prepared)
                                  : Vec<resource::TextureUseHandle>();
        next.CarryForward(rstd::move(m_prepared), sections);
        m_prepared             = rstd::move(next);
        m_prepare_rollback     = Some(PrepareRollback {
            .table = rstd::move(previous),
        });
        m_prepare_session      = None();
        m_prepare_status.state = ResourcePrepareState::Prepared;
        auto refreshed         = RefreshTextureDescriptors(changed.as_slice());
        if (refreshed.is_err()) {
            auto error = refreshed.unwrap_err();
            FailPreparePlan(error);
            return Err(rstd::move(error));
        }
        return Ok(ResourcePrepareProgress::Complete);
    }

    auto PrepareStatus() const -> const ResourcePrepareStatus& { return m_prepare_status; }
    auto PrepareActive() const -> bool {
        return m_prepare_status.state == ResourcePrepareState::Preparing ||
               m_prepare_status.state == ResourcePrepareState::Prepared;
    }
    auto CancelPreparePlan(u64 id) -> bool {
        if (id != m_prepare_status.id || ! PrepareActive()) return false;
        AbortPreparePlan();
        return true;
    }
    void AbortPreparePlan() {
        m_prepare_session = None();
        if (PrepareActive()) m_prepare_status.state = ResourcePrepareState::Cancelled;
        ReleasePipelineRecipes();
        if (Ready()) m_registries.Textures().DiscardPendingUploads();
        m_registries.TextureEntries().DiscardPendingUploads();
        m_registries.TextureEntries().AbortPrepareTransaction();
        auto rollback = m_prepare_rollback.take();
        if (rollback.is_none()) return;
        m_prepared = rstd::move(rollback->table);
    }

    auto CommitPreparePlan(u64 id) -> Result<empty, resource::ResourceError> {
        if (id != m_prepare_status.id || m_prepare_status.state != ResourcePrepareState::Prepared) {
            auto error =
                resource::ResourceError { .kind = resource::ResourceErrorKind::StaleTransaction,
                                          .message =
                                              "resource prepare transaction is not prepared"_Str };
            error.contexts.push(
                resource::ResourceErrorContext::Transaction(id, m_prepare_status.id));
            return Err(rstd::move(error));
        }
        auto submitted = TryCommitUploads();
        if (submitted.is_err()) {
            auto error = resource::ResourceError {
                .message = rstd::format("submit prepared uploads failed"),
                .source  = Some(resource::ResourceErrorSource::Texture(submitted.unwrap_err()))
            };
            FailPreparePlan(error);
            return Err(rstd::move(error));
        }
        m_prepare_status.state = ResourcePrepareState::Committed;
        m_prepare_rollback     = None();
        m_registries.TextureEntries().CommitPrepareTransaction();
        auto active = m_prepared.TextureResources();
        m_registries.TextureEntries().RetainActive(active.as_slice());
        return Ok(empty {});
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
        -> Result<vrento::Preparation<PipelinePreparation>, resource::ResourceError> {
        Option<PipelineResourceResult> result;
        if (m_pipeline_preparation) {
            usize index {};
            for (; index < m_pipeline_recipes.len(); ++index)
                if (m_pipeline_recipes[index].use == pipeline_use) break;
            if (index < m_pipeline_recipes.len() &&
                ! m_registries.Pipelines().MatchesRecipe(m_pipeline_recipes[index].recipe,
                                                         request)) {
                m_registries.Pipelines().ReleaseRecipe(m_pipeline_recipes[index].recipe);
                m_pipeline_recipes.remove(index);
                index = m_pipeline_recipes.len();
            }
            if (index == m_pipeline_recipes.len()) {
                auto signature = vulkan::MakeRenderPassResourceDesc(request);
                auto recipe    = m_registries.Pipelines().RegisterRecipe(
                    rstd::move(request), m_registries.PipelineLayouts());
                if (recipe.is_none())
                    return Err(resource::ResourceError { resource::ResourceErrorKind::Invalid,
                                                         rstd::format("invalid pipeline recipe") });
                m_pipeline_recipes.push({ pipeline_use, recipe.unwrap(), signature });
            }
            auto& entry    = m_pipeline_recipes[index];
            auto  prepared = m_pipeline_preparation->prepare(entry.recipe, entry.signature);
            if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err());
            auto value = rstd::move(prepared).unwrap();
            if (value.error) return Err(rstd::move(*value.error));
            if (value.status == PipelineReadiness::Pending) return Ok(PendingPreparation {});
            result = value.pipeline.take();
            m_registries.Pipelines().ReleaseRecipe(entry.recipe);
            m_pipeline_recipes.remove(index);
        } else {
            PipelineResourceSystem system(device,
                                          m_registries.PipelineLayouts(),
                                          m_registries.PipelineCache(),
                                          m_registries.RenderPassCache());
            auto                   prepared = system.TryCreateGraphicsPipeline(rstd::move(request));
            if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err());
            if (! prepared->ready()) return Ok(PendingPreparation {});
            result = Some(prepared->take_ready());
        }
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("prepare graphics pipeline failed"),
            });
        }
        m_prepared.PublishPipeline(
            PreparedPipeline { .use         = pipeline_use,
                               .resource    = result->handle,
                               .render_pass = result->render_pass,
                               .physical    = result->pipeline.clone() },
            PreparedRenderPass { .use       = render_pass_use,
                                 .resource  = result->render_pass,
                                 .cache_key = result->render_pass_key.clone(),
                                 .physical  = result->render_pass_physical.clone() });
        return Ok(PipelinePreparation {
            .resource                         = result->handle,
            .render_pass                      = result->render_pass,
            .cache_key                        = rstd::move(result->cache_key),
            .render_pass_key                  = rstd::move(result->render_pass_key),
            .cache_hit                        = result->cache_hit,
            .cache_observed_count             = result->cache_observed_count,
            .render_pass_cache_hit            = result->render_pass_cache_hit,
            .render_pass_cache_observed_count = result->render_pass_cache_observed_count,
            .physical                         = result->pipeline.clone(),
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
        auto result = system.TryCreateFramebuffer(vulkan::FramebufferResourceRequest {
            .render_pass     = **(**render_pass).physical,
            .render_pass_key = (**render_pass).cache_key.clone(),
            .attachments     = rstd::move(attachments),
            .extent          = extent,
        });
        if (result.is_err()) return Err(rstd::move(result).unwrap_err());
        if (! m_prepared.Insert(PreparedFramebuffer {
                .use      = framebuffer_use,
                .resource = result->handle,
                .physical = result->framebuffer.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::DuplicateUse,
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
        auto result = m_registries.RenderPasses().TryEnsure(device, desc);
        if (result.is_err()) return Err(rstd::move(result).unwrap_err());
        if (! m_prepared.Insert(PreparedRenderPass {
                .use       = use,
                .resource  = result->handle,
                .cache_key = result->cache_key.clone(),
                .physical  = result->render_pass.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::DuplicateUse,
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
                .kind    = resource::ResourceErrorKind::DuplicateUse,
                .message = rstd::format("duplicate descriptor binding {}", handle.index),
            });
        }
        return Ok(handle);
    }

    auto UpdateDescriptorImages(resource::DescriptorBindingHandle handle,
                                slice<DescriptorImageBinding>     images)
        -> Result<empty, resource::ResourceError> {
        auto updated = ReplaceDescriptorImages(handle, images);
        if (updated.is_err()) return updated;
        m_prepared.ForgetDescriptorTextures(handle);
        return Ok(empty {});
    }
    auto PrepareTextureDescriptor(const vulkan::Device&          device,
                                  resource::PipelineLayoutHandle layout, u32 set,
                                  slice<DescriptorTextureBinding> textures,
                                  slice<DescriptorBufferBinding>  buffers)
        -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
        auto images = ResolveDescriptorTextures(textures);
        if (images.is_err()) return Err(images.unwrap_err());
        auto descriptor = PrepareDescriptor(
            device, layout, set, images->as_slice(), buffers, DescriptorBindingReuse::Exclusive);
        if (descriptor.is_err()) return descriptor;
        if (! m_prepared.TrackDescriptorTextures(*descriptor, textures)) {
            m_prepared.Remove(*descriptor);
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::InvalidUse,
                .message = rstd::format("descriptor texture bindings invalid") });
        }
        return descriptor;
    }
    auto UpdateDescriptorTextures(resource::DescriptorBindingHandle handle,
                                  slice<DescriptorTextureBinding>   textures)
        -> Result<empty, resource::ResourceError> {
        auto sources = Vec<DescriptorTextureBinding>::from(textures);
        auto images  = ResolveDescriptorTextures(sources.as_slice());
        if (images.is_err()) return Err(images.unwrap_err());
        auto updated = ReplaceDescriptorImages(handle, images->as_slice());
        if (updated.is_err()) return updated;
        if (! m_prepared.TrackDescriptorTextures(handle, sources.as_slice()))
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::InvalidUse,
                .message = rstd::format("descriptor texture bindings invalid") });
        return Ok(empty {});
    }
    auto RefreshTextureDescriptors(slice<resource::TextureUseHandle> changed)
        -> Result<empty, resource::ResourceError> {
        auto affected = m_prepared.AffectedTextureDescriptors(changed);
        for (auto descriptor : affected) {
            auto updated =
                UpdateDescriptorTextures(descriptor, m_prepared.DescriptorTextures(descriptor));
            if (updated.is_err()) return updated;
        }
        return Ok(empty {});
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
            if ((*m_frame)->receipt.cancelled())
                (*m_frame)->recorded_buffers = vulkan::RecordedBufferUploads {};
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

    auto PreparePendingUploads(texture::Submission& work)
        -> Result<PreparedResourceUploads, resource::ResourceError> {
        if (! m_frame || (*m_frame)->uploads_prepared || ! (*m_frame)->receipt.same(work.receipt()))
            return Err(resource::ResourceError { .kind    = resource::ResourceErrorKind::Invalid,
                                                 .message = "wrong or prepared upload batch"_Str });
        auto recording = vulkan::texture_recording(work, TextureContext().session());
        if (recording.is_err())
            return Err(
                resource::ResourceError::texture(recording.unwrap_err(), "prepare uploads"_Str));
        m_registries.TextureEntries().ReservePublications();
        auto updates = m_registries.Textures().PreparePendingUploads(work);
        if (updates.is_err())
            return Err(resource::ResourceError::texture(updates.unwrap_err(),
                                                        "prepare texture updates"_Str));
        auto buffers =
            m_registries.BufferManager().TryPreparePendingUploads((*m_frame)->recorded_buffers);
        if (buffers.is_err()) return Err(resource::ResourceError::backend(buffers.unwrap_err()));
        (*m_frame)->uploads_prepared = true;
        return Ok(PreparedResourceUploads(TextureContext().session(),
                                          m_frame->clone(),
                                          recording.unwrap()->identity,
                                          updates.unwrap()));
    }
    bool RecordPendingUploads(texture::Submission& work) {
        auto prepared = PreparePendingUploads(work);
        return prepared.is_ok() && prepared->Record(work).is_ok();
    }

    auto SubmitFrame(texture::Submission work) -> Result<texture::SubmissionToken, texture::Error> {
        if (m_frame.is_none() || ! (*m_frame)->receipt.same(work.receipt()))
            return Err(texture::Error { texture::ErrorKind::Invalid });
        auto frame = m_frame->clone();
        if (! frame->uploads_recorded && (frame->uploads_prepared || HasPendingUploads()))
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

    auto TryCommitUploads() -> Result<empty, texture::Error> {
        if (! Ready()) return Err(texture::Error { texture::ErrorKind::Closed });
        m_registries.TextureEntries().ReservePublications();
        auto result = m_registries.Textures().Flush();
        m_registries.TextureEntries().PublishSubmitted();
        return result;
    }
    bool CommitUploads() { return TryCommitUploads().is_ok(); }

    auto TryWaitSubmission(const texture::SubmissionToken& token,
                           texture::Serial                 timeout = 5'000'000'000)
        -> Result<empty, texture::Error> {
        if (! Ready()) return Err(texture::Error { texture::ErrorKind::Closed });
        auto waited = TextureContext().wait(token, timeout);
        if (waited.is_err()) return waited;
        return m_registries.Textures().Poll();
    }

    auto TryWaitForUploads(texture::Serial timeout = 5'000'000'000)
        -> Result<empty, texture::Error> {
        auto committed = TryCommitUploads();
        if (committed.is_err()) return committed;
        return m_registries.Textures().WaitForUploads(timeout);
    }
    bool WaitForUploads(texture::Serial timeout = 5'000'000'000) {
        return TryWaitForUploads(timeout).is_ok();
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
        auto selected = (**prepared).ActiveTexture();
        if (selected.is_err()) return false;
        return vulkan::generate_texture_mipmaps(work, TextureContext().session(), selected->view)
            .is_ok();
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

    auto TryPumpTextureProducers(double seconds) -> Result<empty, resource::ResourceError> {
        return m_registries.TextureEntries().Pump(TextureContext(), seconds);
    }
    bool PumpTextureProducers(double seconds) { return TryPumpTextureProducers(seconds).is_ok(); }
    auto PrepareRegion(ref<str> key, texture::TextureUpload upload, usize slot = usize())
        -> Result<Option<PreparedTextureUpdate>, resource::ResourceError> {
        return PrepareRegion(
            m_registries.TextureEntries().Find(resource::TextureRequestKind::Imported, key),
            rstd::move(upload),
            slot);
    }
    auto PrepareRegion(const resource::TextureResourceRef& definition,
                       texture::TextureUpload upload, usize slot = usize())
        -> Result<Option<PreparedTextureUpdate>, resource::ResourceError> {
        return PrepareRegion(
            m_registries.TextureEntries().Find(definition), rstd::move(upload), slot);
    }

private:
    auto PrepareRegion(Option<resource::TextureHandle> handle, texture::TextureUpload upload,
                       usize slot)
        -> Result<Option<PreparedTextureUpdate>, resource::ResourceError> {
        if (! handle) return Ok(None());
        auto physical = m_registries.TextureEntries().ResolveCurrent(*handle);
        if (! physical) return Ok(None());
        auto       allocation = (**physical).allocation.clone();
        const auto slots      = allocation->Slots();
        if (slot >= slots.len())
            return Err(resource::ResourceError { resource::ResourceErrorKind::InvalidUse,
                                                 rstd::format("invalid texture slot") });
        upload.target = slots[slot].view;
        if (texture::validate_upload(upload).is_err())
            return Err(resource::ResourceError { resource::ResourceErrorKind::Invalid,
                                                 rstd::format("invalid texture region") });
        Vec<texture::TextureUpload> uploads;
        uploads.push(rstd::move(upload));
        return Ok(Some(PreparedTextureUpdate { rstd::move(allocation), rstd::move(uploads) }));
    }

public:
    FontAtlasUploadStatus QueueRegion(texture::Submission& work, ref<str> key,
                                      texture::TextureUpload upload, usize slot = usize()) {
        if (! work.valid()) return FontAtlasUploadStatus::Failed;
        auto prepared = PrepareRegion(key, rstd::move(upload), slot);
        if (prepared.is_err()) return FontAtlasUploadStatus::Failed;
        auto region = prepared.unwrap();
        if (! region) return FontAtlasUploadStatus::NotResident;
        return region->Record(work, TextureContext().session()).is_ok()
                   ? FontAtlasUploadStatus::Recorded
                   : FontAtlasUploadStatus::Failed;
    }
    auto TryCreateLocalSwapchain(const vulkan::Device& device, unsigned width, unsigned height,
                                 VkImageTiling tiling)
        -> Result<ExSwapchainOwner, resource::ResourceError> {
        if (! Ready() || *device.handle() != *TextureContext().session().device().handle())
            return Err(resource::ResourceError::backend(
                { "local output"_Str, vulkan::BackendCause::Closed() }));
        return CreateSessionLocalOutput(TextureContext(), width, height, tiling);
    }
    auto CreateLocalSwapchain(const vulkan::Device& device, unsigned width, unsigned height,
                              VkImageTiling tiling) -> Option<ExSwapchainOwner> {
        return TryCreateLocalSwapchain(device, width, height, tiling).ok();
    }
    void ClearTextures() {
        AbortPreparePlan();
        m_prepare_status   = {};
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
    auto ResolveDescriptorTextures(slice<DescriptorTextureBinding> textures) const
        -> Result<Vec<DescriptorImageBinding>, resource::ResourceError> {
        auto images = Vec<DescriptorImageBinding>::with_capacity(textures.len());
        for (const auto& source : textures) {
            auto prepared = m_prepared.Resolve(source.texture);
            if (! prepared)
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingDefinition,
                    .message =
                        rstd::format("descriptor texture {} unavailable", source.texture.index) });
            auto sampled = (*prepared)->physical->SampledBinding(source.slot);
            if (sampled.is_err())
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::InvalidUse,
                    .message = rstd::format("descriptor texture slot unavailable") });
            images.push({ source.binding, sampled->image, sampled->layout });
        }
        return Ok(rstd::move(images));
    }
    auto ReplaceDescriptorImages(resource::DescriptorBindingHandle handle,
                                 slice<DescriptorImageBinding>     images)
        -> Result<empty, resource::ResourceError> {
        auto prepared = m_prepared.ResolveMut(handle);
        if (! prepared)
            return Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingDefinition,
                .message =
                    rstd::format("prepared descriptor binding {} unavailable", handle.index) });
        if ((*prepared)->MatchesImages(images)) return Ok(empty {});
        auto replacement =
            m_registries.Descriptors().ReplaceImages(TextureContext().session().device(),
                                                     **prepared,
                                                     images,
                                                     m_registries.DescriptorLayouts());
        if (replacement.is_err()) return Err(replacement.unwrap_err());
        **prepared = replacement.unwrap();
        return Ok(empty {});
    }
    void FailPreparePlan(const resource::ResourceError& error) {
        AbortPreparePlan();
        m_prepare_status.state = ResourcePrepareState::Failed;
        m_prepare_status.error = Some(error.clone());
    }
    struct PipelineRecipeEntry {
        resource::PipelineUseHandle    use;
        resource::PipelineRecipeHandle recipe;
        vulkan::RenderPassResourceDesc signature;
    };
    void ReleasePipelineRecipes() {
        for (const auto& entry : m_pipeline_recipes)
            m_registries.Pipelines().ReleaseRecipe(entry.recipe);
        m_pipeline_recipes.clear();
    }
    void ClosePipelinePreparation() {
        if (m_pipeline_preparation) m_pipeline_preparation->close();
        ReleasePipelineRecipes();
        m_pipeline_preparation = None();
    }
    struct PrepareRollback {
        PreparedResourceTable table;
    };

    ResourceRegistries                            m_registries;
    u64                                           m_next_prepare_id {};
    ResourcePrepareStatus                         m_prepare_status;
    Option<ResourcePrepareSession>                m_prepare_session;
    Vec<PipelineRecipeEntry>                      m_pipeline_recipes;
    Option<vulkan::PipelinePreparationQueue>      m_pipeline_preparation;
    PreparedResourceTable                         m_prepared;
    Option<PrepareRollback>                       m_prepare_rollback;
    Option<rstd::sync::Arc<FrameSubmissionLease>> m_frame;
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
        -> Result<vrento::Preparation<vrento::resource_registry::PipelinePreparation>,
                  vrento::resource::ResourceError> {
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
    auto
    PrepareTextureDescriptor(const vrento::vulkan::Device&          device,
                             vrento::resource::PipelineLayoutHandle layout, u32 set,
                             slice<vrento::resource_registry::DescriptorTextureBinding> textures,
                             slice<vrento::resource_registry::DescriptorBufferBinding>  buffers)
        -> Result<vrento::resource::DescriptorBindingHandle, vrento::resource::ResourceError> {
        return this->self().PrepareTextureDescriptor(device, layout, set, textures, buffers);
    }
    auto
    UpdateDescriptorTextures(vrento::resource::DescriptorBindingHandle                  handle,
                             slice<vrento::resource_registry::DescriptorTextureBinding> textures)
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().UpdateDescriptorTextures(handle, textures);
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
