export module vrento:resource_registry.texture;
export import :texture.prepare;
import :texture.video_playback;
import :resource;
import :vulkan.texture;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento::resource_registry
{

struct TextureSlot {
    texture::TextureViewRef view;
    texture::SamplerRef     sampler;
    texture::TextureViewRef attachment_view;
};

struct TextureProducer {
    using Trait                  = TextureProducer;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureProducer;
        auto Pump(vulkan::TextureContext& context, double seconds)
            -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<0>(this, context, seconds);
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Pump>;
};

enum class ImageReadiness
{
    Allocated,
    Pending,
    Recorded,
    Submitted,
    Complete,
    Uncertain
};

class TextureAllocation {
public:
    explicit TextureAllocation(Vec<TextureSlot> slots, vulkan::ImageSlotsRef native,
                               Option<Arc<dyn<TextureProducer>>> producer = None())
        : m_slots(rstd::move(slots)),
          m_native(rstd::move(native)),
          m_producer(rstd::move(producer)) {}

    auto View() const -> vulkan::ImageSlotsRef { return m_native; }
    auto Slots() const -> slice<TextureSlot> { return m_slots.as_slice(); }
    auto SampledBinding(usize slot = usize()) const
        -> Result<vulkan::SampledImageBinding, texture::Error> {
        if (slot >= m_slots.len()) return Err(texture::Error { texture::ErrorKind::Invalid });
        return vulkan::sampled_image(m_slots[slot].view, m_slots[slot].sampler);
    }
    auto AttachmentBinding(usize slot = usize()) const
        -> Result<vulkan::SampledImageBinding, texture::Error> {
        if (slot >= m_slots.len() || ! m_slots[slot].attachment_view.valid())
            return Err(texture::Error { texture::ErrorKind::Invalid });
        return vulkan::attachment_image(m_slots[slot].attachment_view);
    }
    auto Completion(usize slot = usize()) const -> texture::SubmissionToken {
        if (slot < m_slots.len()) {
            auto completion = m_slots[slot].view.texture().ready();
            if (completion.valid()) return completion;
        }
        return m_completion;
    }
    auto UploadCompletion() const -> texture::SubmissionToken { return m_completion; }
    void MarkPending() { m_pending = true; }
    void MarkRecording(const texture::SubmissionReceipt& receipt) {
        if (m_pending) {
            m_receipt  = Some(texture::SubmissionReceipt(receipt));
            m_recorded = false;
        }
    }
    void MarkRecorded() {
        if (m_pending) m_recorded = true;
    }
    auto InitialReadiness(Option<texture::SubmissionReceipt> work = None()) const
        -> Result<ImageReadiness, texture::Error> {
        if (m_receipt && m_receipt->uncertain()) return Ok(ImageReadiness::Uncertain);
        if (m_pending) {
            if (m_recorded && m_receipt && m_receipt->submitted())
                return Ok(ImageReadiness::Submitted);
            if (m_recorded && m_receipt && ! m_receipt->submitted() && ! m_receipt->cancelled() &&
                work && m_receipt->same(*work))
                return Ok(ImageReadiness::Recorded);
            return Ok(ImageReadiness::Pending);
        }
        if (! m_completion.valid()) return Ok(ImageReadiness::Allocated);
        auto polled = m_completion.poll();
        if (polled.is_err()) return Err(polled.unwrap_err());
        return Ok(polled.unwrap() ? ImageReadiness::Complete : ImageReadiness::Submitted);
    }
    void MarkSubmitted(texture::SubmissionToken completion) {
        m_pending    = false;
        m_completion = rstd::move(completion);
    }
    void CancelPending() {
        m_pending = m_recorded = false;
        m_receipt              = None();
    }
    auto Pump(vulkan::TextureContext& context, double seconds)
        -> Result<empty, resource::ResourceError> {
        return m_producer.is_some() ? (*m_producer)->Pump(context, seconds) : Ok(empty {});
    }

private:
    Vec<TextureSlot>                   m_slots;
    vulkan::ImageSlotsRef              m_native;
    Option<Arc<dyn<TextureProducer>>>  m_producer;
    texture::SubmissionToken           m_completion;
    bool                               m_pending {}, m_recorded {};
    Option<texture::SubmissionReceipt> m_receipt;
};

struct TextureUpdateState {
    Option<texture::SubmissionReceipt> receipt;
    bool                               recorded {};
};
struct PreparedTextureUpdate {
    Arc<TextureAllocation>      allocation;
    Vec<texture::TextureUpload> uploads;
    bool                        transparent {}, sampled {};
    Arc<TextureUpdateState>     state { Arc<TextureUpdateState>::make() };
    auto                        Readiness(Option<texture::SubmissionReceipt> work = None()) const
        -> Result<ImageReadiness, texture::Error> {
        if (! state->receipt || state->receipt->cancelled()) return Ok(ImageReadiness::Pending);
        if (state->receipt->uncertain()) return Ok(ImageReadiness::Uncertain);
        if (! state->receipt->submitted())
            return Ok(state->recorded && work && state->receipt->same(*work)
                          ? ImageReadiness::Recorded
                          : ImageReadiness::Pending);
        if (! state->recorded) return Err(texture::Error { texture::ErrorKind::Invalid });
        bool complete = true;
        for (usize slot {}; slot < allocation->Slots().len(); ++slot) {
            auto token = allocation->Completion(slot);
            if (! token.valid()) return Err(texture::Error { texture::ErrorKind::Invalid });
            auto polled = token.poll();
            if (polled.is_err()) return Err(polled.unwrap_err());
            complete &= polled.unwrap();
        }
        return Ok(complete ? ImageReadiness::Complete : ImageReadiness::Submitted);
    }
    auto clone() const -> PreparedTextureUpdate {
        Vec<texture::TextureUpload> copy;
        for (const auto& upload : uploads) copy.push(upload.clone());
        return { allocation.clone(), rstd::move(copy), transparent, sampled, state.clone() };
    }
    auto Writes() const -> Vec<texture::TextureAccess> {
        Vec<texture::TextureAccess> writes;
        if (transparent)
            for (const auto& slot : allocation->Slots())
                writes.push({ slot.view,
                              texture::Access::TransferDestination,
                              texture::Stages::Transfer,
                              true });
        for (const auto& upload : uploads)
            writes.push({ upload.target,
                          texture::Access::TransferDestination,
                          texture::Stages::Transfer,
                          false,
                          Some(texture::Range { upload.target.description().range.aspect,
                                                upload.mip,
                                                1,
                                                upload.layer,
                                                1 }) });
        return writes;
    }
    auto Record(texture::Submission& work, const vulkan::DeviceSession& session) const
        -> Result<empty, texture::Error> {
        if (state->receipt && ! state->receipt->cancelled())
            return Err(texture::Error { texture::ErrorKind::Busy });
        state->recorded = false;
        state->receipt  = Some(work.receipt());
        allocation->MarkRecording(work.receipt());
        work.retain(allocation.clone());
        auto commands = work.commands();
        for (const auto& upload : uploads) {
            auto result = commands->Upload(upload.clone());
            if (result.is_err()) return result;
        }
        if (transparent)
            for (const auto& slot : allocation->Slots()) {
                auto used = commands->UseTexture({ slot.view,
                                                   texture::Access::TransferDestination,
                                                   texture::Stages::Transfer,
                                                   true });
                if (used.is_err()) return used;
                auto command = vulkan::native_recording(work, session);
                auto image   = vulkan::native_texture(slot.view.texture());
                if (command.is_err()) return Err(command.unwrap_err());
                if (image.is_err()) return Err(image.unwrap_err());
                auto                    range = slot.view.description().range;
                VkImageSubresourceRange native { VK_IMAGE_ASPECT_COLOR_BIT,
                                                 range.first_mip,
                                                 range.mip_count,
                                                 range.first_layer,
                                                 range.layer_count };
                VkClearColorValue       color {};
                session.device().handle().Dispatch().vkCmdClearColorImage(
                    command.unwrap(),
                    image.unwrap(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &color,
                    1,
                    &native);
            }
        if (sampled)
            for (const auto& slot : allocation->Slots())
                if (texture::has(slot.view.texture().description().usage,
                                 texture::Usage::Sampled)) {
                    auto used = commands->UseSampledView(
                        slot.view, slot.sampler, texture::Stages::Fragment);
                    if (used.is_err()) return used;
                }
        state->recorded = true;
        allocation->MarkRecorded();
        return Ok(empty {});
    }
};

struct PreparedImageAllocation {
    Arc<TextureAllocation>            allocation;
    Option<vulkan::ImageUploadTicket> upload;
};

struct ProducedTexture {
    Vec<TextureSlot>                  slots;
    Vec<texture::TextureUpload>       uploads;
    Option<Arc<dyn<TextureProducer>>> producer;
};

struct TextureProducerFactory {
    using Trait                  = TextureProducerFactory;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureProducerFactory;
        auto Create(vulkan::TextureContext& context, Arc<Image> image,
                    Option<Arc<dyn<VideoPlayback>>> playback)
            -> Result<ProducedTexture, resource::ResourceError> {
            return rstd::trait_call<0>(this, context, rstd::move(image), rstd::move(playback));
        }
        // Clear preserves producers retained by allocations and in-flight submissions.
        void Clear() { rstd::trait_call<1>(this); }
        auto Drain(u64 timeout_ns) -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<2>(this, timeout_ns);
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Create, &T::Clear, &T::Drain>;
};

struct TexturePrepareBackend {
    using Trait                  = TexturePrepareBackend;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TexturePrepareBackend;
        auto CreateImportedTexture(Arc<Image> image, Option<Arc<dyn<VideoPlayback>>> playback)
            -> Result<PreparedImageAllocation, resource::ResourceError> {
            return rstd::trait_call<0>(this, rstd::move(image), rstd::move(playback));
        }
        auto AllocateTexture(const resource::TextureDefinition& definition, bool transparent)
            -> Result<PreparedImageAllocation, resource::ResourceError> {
            return rstd::trait_call<1>(this, definition, transparent);
        }
        auto InitializeTexture(Arc<TextureAllocation> allocation)
            -> Result<PreparedImageAllocation, resource::ResourceError> {
            return rstd::trait_call<2>(this, rstd::move(allocation));
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::CreateImportedTexture, &T::AllocateTexture, &T::InitializeTexture>;
};

struct TextureUploadPublisher {
    using Trait                  = TextureUploadPublisher;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureUploadPublisher;
        void Reserve() { rstd::trait_call<0>(this); }
        void Submitted(slice<vulkan::ImageUploadTicket> tickets,
                       texture::SubmissionToken         completion) {
            rstd::trait_call<1>(this, tickets, rstd::move(completion));
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Reserve, &T::Submitted>;
};

} // namespace vrento::resource_registry

export namespace vrento::resource_registry
{

struct TexturePreparationStatistics {
    u64 image_preparations {}, image_reuses {}, target_allocations {};
    u64 shared_upload_bytes {}, pending_bytes {}, peak_pending_bytes {};
    u64 peak_pending_contents {}, peak_upload_batches {}, window_waits {}, window_wait_ns {};
};
class RegistryTextureContext {
    TexturePreparationStatistics statistics_;

public:
    auto Statistics() const -> TexturePreparationStatistics {
        auto result          = statistics_;
        result.pending_bytes = PendingBytes();
        return result;
    }
    static constexpr usize MaxPendingContents { 4 };
    static constexpr usize MaxUploadBatches { 3 };
    explicit RegistryTextureContext(vulkan::TextureContext context)
        : m_context(rstd::move(context)) {}

    auto Context() -> vulkan::TextureContext& { return m_context; }
    auto Context() const -> const vulkan::TextureContext& { return m_context; }
    auto SetProducerFactory(Arc<dyn<TextureProducerFactory>> factory)
        -> Result<empty, resource::ResourceError> {
        if (m_producers)
            return Err(resource::ResourceError {
                resource::ResourceErrorKind::Busy,
                rstd::format("texture producer factory already registered") });
        m_producers = Some(rstd::move(factory));
        return Ok(empty {});
    }
    void SetUploadPublisher(mut_ref<dyn<TextureUploadPublisher>> publisher) {
        m_publisher = Some(publisher);
    }
    void ClearProducers() {
        if (m_producers) (*m_producers)->Clear();
    }
    auto DrainProducers(u64 timeout) -> Result<empty, resource::ResourceError> {
        if (m_producers) return (*m_producers)->Drain(timeout);
        return Ok(empty {});
    }

    // Pixel contents are immutable; publish a new Image for a new revision.
    auto CreateImportedTexture(Arc<Image> image, Option<Arc<dyn<VideoPlayback>>> playback)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        if (image->header.kind == ImageKind::Video) {
            if (! m_producers)
                return Err(resource::ResourceError {
                    resource::ResourceErrorKind::Unsupported,
                    rstd::format("video texture requires a texture producer factory") });
            auto window = MakeRoom();
            if (window.is_err())
                return Failure("texture upload window failed", Some(window.unwrap_err()));
            auto result =
                (*m_producers)->Create(m_context, rstd::move(image), rstd::move(playback));
            if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
            auto prepared = rstd::move(result).unwrap_unchecked();
            return Queue(rstd::move(prepared.slots),
                         rstd::move(prepared.uploads),
                         false,
                         rstd::move(prepared.producer));
        }
        m_images.retain([](const CachedImage& cached) {
            return cached.source.strong_count() != usize();
        });
        for (const auto& cached : m_images) {
            auto source = cached.source.upgrade();
            if (source && Arc<Image>::ptr_eq(source, image)) {
                ++statistics_.image_reuses;
                return Ok(PreparedImageAllocation { cached.allocation.clone(),
                                                    cached.allocation->UploadCompletion().valid()
                                                        ? None<vulkan::ImageUploadTicket>()
                                                        : cached.ticket });
            }
        }
        auto window = MakeRoom();
        if (window.is_err())
            return Failure("texture upload window failed", Some(window.unwrap_err()));
        auto                         device = dyn<texture::TextureDevice>::from_ref(m_context);
        texture::ImagePrepareContext preparer(device.as_mut_ref());
        auto content = dyn<texture::TextureContentPreparer>::from_ref(preparer);
        auto result  = content->PrepareImage(image.clone());
        if (result.is_err())
            return Failure("prepare texture pixels failed", Some(result.unwrap_err()));
        ++statistics_.image_preparations;
        auto                        prepared = rstd::move(result).unwrap_unchecked();
        Vec<TextureSlot>            slots;
        Vec<texture::TextureUpload> uploads;
        for (auto& item : prepared) {
            slots.push({ rstd::move(item.view), rstd::move(item.sampler) });
            for (const auto& upload : item.uploads)
                statistics_.shared_upload_bytes += u64(upload.bytes.len().to_primitive());
            uploads.append(item.uploads);
        }
        auto queued = Queue(rstd::move(slots), rstd::move(uploads), false);
        if (queued.is_ok())
            m_images.push({ image.downgrade(), queued->allocation.clone(), queued->upload });
        return queued;
    }

    auto TryPrepareImage(Arc<Image> image) -> Result<bool, resource::ResourceError> {
        if (image->header.kind != ImageKind::Pixels) return Ok(false);
        if (m_recording && ! m_recording->cancelled()) return Ok(false);
        auto polled = Poll();
        if (polled.is_err())
            return Err(resource::ResourceError {
                .message = rstd::format("poll image uploads failed"),
                .source  = Some(resource::ResourceErrorSource::Texture(polled.unwrap_err())) });
        if (m_in_flight.len() >= MaxUploadBatches || m_pending.len() >= MaxPendingContents)
            return Ok(false);
        auto prepared = CreateImportedTexture(rstd::move(image), None());
        if (prepared.is_err()) return Err(prepared.unwrap_err());
        auto flushed = Flush();
        if (flushed.is_err())
            return Err(resource::ResourceError {
                .message = rstd::format("submit image uploads failed"),
                .source  = Some(resource::ResourceErrorSource::Texture(flushed.unwrap_err())) });
        return Ok(true);
    }

    auto AllocateTexture(const resource::TextureDefinition& definition, bool transparent)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        if (definition.width <= i32() || definition.height <= i32())
            return Failure("invalid texture extent");
        auto window = MakeRoom();
        if (window.is_err())
            return Failure("texture upload window failed", Some(window.unwrap_err()));
        texture::TextureDesc desc;
        desc.extent     = { texture::Count(definition.width.to_primitive()),
                            texture::Count(definition.height.to_primitive()) };
        desc.mip_levels = definition.mip_levels.to_primitive();
        desc.samples    = definition.samples.to_primitive();
        desc.format     = definition.format;
        desc.usage      = texture::Usage::None;
        if (resource::HasTextureUsage(definition.usage, resource::TextureUsage::Sampled))
            desc.usage = desc.usage | texture::Usage::Sampled;
        if (resource::HasTextureUsage(definition.usage, resource::TextureUsage::ColorAttachment))
            desc.usage = desc.usage | texture::Usage::ColorAttachment;
        if (resource::HasTextureUsage(definition.usage, resource::TextureUsage::DepthAttachment))
            desc.usage = desc.usage | texture::Usage::DepthAttachment;
        if (resource::HasTextureUsage(definition.usage, resource::TextureUsage::TransferSource))
            desc.usage = desc.usage | texture::Usage::TransferSource;
        if (resource::HasTextureUsage(definition.usage,
                                      resource::TextureUsage::TransferDestination))
            desc.usage = desc.usage | texture::Usage::TransferDestination;
        if (transparent && (texture::format_layout(desc.format).aspect != texture::Aspect::Color ||
                            ! texture::has(desc.usage, texture::Usage::TransferDestination)))
            return Failure("texture cannot be initialized transparent");
        auto support = m_context.QuerySupport(desc);
        if (support.is_err())
            return Failure("texture definition unsupported", Some(support.unwrap_err()));
        auto allocated = m_context.CreateTexture(desc);
        if (allocated.is_err())
            return Failure("allocate texture failed", Some(allocated.unwrap_err()));
        ++statistics_.target_allocations;
        auto storage = rstd::move(allocated).unwrap_unchecked();
        auto view    = m_context.CreateView(
            storage, { texture::Dimension::D2, { texture::format_layout(desc.format).aspect } });
        if (view.is_err()) return Failure("create texture view failed", Some(view.unwrap_err()));
        auto sampler = m_context.CreateSampler({ definition.sample });
        if (sampler.is_err())
            return Failure("create texture sampler failed", Some(sampler.unwrap_err()));
        texture::TextureViewRef attachment;
        if (texture::has(desc.usage, texture::Usage::ColorAttachment) ||
            texture::has(desc.usage, texture::Usage::DepthAttachment)) {
            auto bound = m_context.CreateView(
                storage,
                { texture::Dimension::D2,
                  { texture::format_layout(desc.format).aspect, 0, 1, 0, 1 } });
            if (bound.is_err())
                return Failure("create texture attachment view failed", Some(bound.unwrap_err()));
            attachment = rstd::move(bound).unwrap_unchecked();
        }
        Vec<TextureSlot> slots;
        slots.push({ rstd::move(view).unwrap_unchecked(),
                     rstd::move(sampler).unwrap_unchecked(),
                     rstd::move(attachment) });
        return Queue(rstd::move(slots), {}, transparent);
    }

    auto InitializeTexture(Arc<TextureAllocation> allocation)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        auto readiness = allocation->InitialReadiness();
        if (readiness.is_err())
            return Failure("texture readiness unavailable", Some(readiness.unwrap_err()));
        if (readiness.unwrap() == ImageReadiness::Uncertain)
            return Failure("texture initialization uncertain");
        if (readiness.unwrap() != ImageReadiness::Allocated)
            return Ok(PreparedImageAllocation { rstd::move(allocation), None() });
        // Only bootstrap an allocation that has never received a submitted write.
        for (const auto& slot : allocation->Slots())
            if (slot.view.texture().content_revision() != 0)
                return Ok(PreparedImageAllocation { rstd::move(allocation), None() });
        if (allocation->Slots().is_empty()) return Failure("texture slots unavailable");
        for (const auto& slot : allocation->Slots()) {
            const auto& desc = slot.view.texture().description();
            if (texture::format_layout(desc.format).aspect != texture::Aspect::Color ||
                ! texture::has(desc.usage, texture::Usage::TransferDestination))
                return Failure("texture cannot be initialized transparent");
        }
        auto window = MakeRoom();
        if (window.is_err())
            return Failure("texture upload window failed", Some(window.unwrap_err()));
        return QueueUpdate(rstd::move(allocation), {}, true);
    }

    bool HasPendingUploads() const { return ! m_pending.is_empty(); }
    auto PendingCount() const -> usize { return m_pending.len(); }
    auto InFlightCount() const -> usize { return m_in_flight.len(); }

    auto PreparePendingUploads(texture::Submission& work)
        -> Result<Vec<PreparedTextureUpdate>, texture::Error> {
        if (m_recording || ! work.valid()) return Err(texture::Error { texture::ErrorKind::Busy });
        m_in_flight.reserve(m_in_flight.len() + usize(1));
        if (m_publisher) (*m_publisher)->Reserve();
        m_recorded_tickets.clear();
        m_recorded_tickets.reserve(m_pending.len());
        Vec<PreparedTextureUpdate> updates;
        for (const auto& pending : m_pending) {
            m_recorded_tickets.push(vulkan::ImageUploadTicket(pending.ticket));
            PreparedTextureUpdate update {
                pending.allocation.clone(), {}, pending.transparent, true
            };
            for (const auto& upload : pending.uploads) update.uploads.push(upload.clone());
            updates.push(rstd::move(update));
        }
        m_recording = Some(work.receipt());
        return Ok(rstd::move(updates));
    }
    bool RecordPendingUploads(texture::Submission& work) {
        auto updates = PreparePendingUploads(work);
        if (updates.is_err()) return false;
        for (const auto& update : updates.unwrap())
            if (update.Record(work, m_context.session()).is_err()) return false;
        return true;
    }

    bool ConfirmSubmitted(const texture::SubmissionReceipt& receipt,
                          const texture::SubmissionToken&   token) {
        if (! m_recording || ! m_recording->same(receipt) || ! receipt.submitted() ||
            ! token.valid())
            return false;
        m_pending.retain([&](const Pending& pending) {
            for (const auto& ticket : m_recorded_tickets)
                if (ticket.value == pending.ticket.value) {
                    pending.allocation->MarkSubmitted(token);
                    return false;
                }
            return true;
        });
        if (m_publisher.is_some()) (*m_publisher)->Submitted(m_recorded_tickets.as_slice(), token);
        m_recorded_tickets.clear();
        m_recording = None();
        m_in_flight.push(texture::SubmissionToken(token));
        statistics_.peak_upload_batches =
            rstd::cmp::max(statistics_.peak_upload_batches, u64(m_in_flight.len().to_primitive()));
        return true;
    }
    void CancelRecording() {
        if (m_recording && m_recording->uncertain()) return;
        m_recording = None();
        m_recorded_tickets.clear();
    }
    void DiscardPendingUploads() {
        if (m_recording && m_recording->uncertain()) return;
        m_images.retain([](const CachedImage& cached) {
            return cached.allocation->UploadCompletion().valid();
        });
        for (const auto& pending : m_pending) pending.allocation->CancelPending();
        m_pending.clear();
        CancelRecording();
    }

    auto Poll() -> Result<empty, texture::Error> {
        m_images.retain([](const CachedImage& cached) {
            return cached.source.strong_count() != usize();
        });
        auto polled = m_context.Poll();
        if (polled.is_err()) return polled;
        for (usize index {}; index < m_in_flight.len();) {
            auto ready = m_in_flight[index].poll();
            if (ready.is_err()) return Err(ready.unwrap_err_unchecked());
            if (ready.unwrap_unchecked())
                (void)m_in_flight.remove(index);
            else
                ++index;
        }
        return Ok(empty {});
    }

    auto Flush() -> Result<empty, texture::Error> {
        if (m_pending.is_empty()) return Ok(empty {});
        auto window = WaitWindow();
        if (window.is_err()) return window;
        auto begun = m_context.Begin();
        if (begun.is_err()) return Err(begun.unwrap_err_unchecked());
        auto work = rstd::move(begun).unwrap_unchecked();
        if (! RecordPendingUploads(work)) {
            CancelRecording();
            return Err(texture::Error { texture::ErrorKind::Backend });
        }
        auto receipt   = work.receipt();
        auto submitted = m_context.Submit(rstd::move(work));
        if (submitted.is_err()) {
            CancelRecording();
            return Err(submitted.unwrap_err_unchecked());
        }
        if (! ConfirmSubmitted(receipt, submitted.unwrap_unchecked()))
            return Err(texture::Error { texture::ErrorKind::Backend });
        return Ok(empty {});
    }

    auto WaitWindow() -> Result<empty, texture::Error> {
        auto polled = Poll();
        if (polled.is_err()) return polled;
        if (m_in_flight.len() < MaxUploadBatches) return Ok(empty {});
        ++statistics_.window_waits;
        auto start  = rstd::time::Instant::now();
        auto waited = m_context.wait(m_in_flight[usize()]);
        statistics_.window_wait_ns +=
            u64(rstd::uint64_t(start.elapsed().as_nanos().to_primitive()));
        if (waited.is_err()) return waited;
        return Poll();
    }

    auto WaitForUploads(texture::Serial timeout) -> Result<empty, texture::Error> {
        for (const auto& token : m_in_flight) {
            auto waited = m_context.wait(token, timeout);
            if (waited.is_err()) return waited;
        }
        return Poll();
    }

private:
    auto PendingBytes() const -> u64 {
        u64 bytes {};
        for (const auto& pending : m_pending)
            for (const auto& upload : pending.uploads)
                bytes += u64(upload.bytes.len().to_primitive());
        return bytes;
    }
    struct CachedImage {
        rstd::sync::Weak<Image>           source;
        Arc<TextureAllocation>            allocation;
        Option<vulkan::ImageUploadTicket> ticket;
    };
    Vec<CachedImage> m_images;
    struct Pending {
        vulkan::ImageUploadTicket   ticket;
        Arc<TextureAllocation>      allocation;
        Vec<texture::TextureUpload> uploads;
        bool                        transparent;
    };
    template<rstd::size_t N>
    static auto Failure(const char (&message)[N], Option<texture::Error> source = None())
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        return Err(resource::ResourceError {
            .message = rstd::format("{}", message),
            .source  = source ? Some(resource::ResourceErrorSource::Texture(*source))
                              : None<resource::ResourceErrorSource>() });
    }
    auto MakeRoom() -> Result<empty, texture::Error> {
        if (m_recording.is_some()) {
            if (! m_recording->cancelled()) return Err(texture::Error { texture::ErrorKind::Busy });
            CancelRecording();
        }
        if (m_pending.len() >= MaxPendingContents) return Flush();
        return WaitWindow();
    }
    auto Queue(Vec<TextureSlot> slots, Vec<texture::TextureUpload> uploads, bool transparent,
               Option<Arc<dyn<TextureProducer>>> producer = None())
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        vulkan::ImageSlotsRef native;
        if (slots.is_empty()) return Failure("texture slots unavailable");
        for (const auto& slot : slots) {
            if (! slot.view.valid() || ! slot.sampler.valid())
                return Failure("invalid texture slot");
            const auto identity = slot.view.texture().identity();
            if (identity.device != m_context.session().epoch() ||
                identity.epoch != m_context.session().epoch() ||
                slot.sampler.identity().device != identity.device ||
                slot.sampler.identity().epoch != identity.epoch ||
                (slot.attachment_view.valid() &&
                 slot.attachment_view.texture().identity() != identity))
                return Failure("texture slot device mismatch");
            auto image   = vulkan::native_texture(slot.view.texture());
            auto view    = vulkan::native_view(slot.attachment_view.valid() ? slot.attachment_view
                                                                            : slot.view);
            auto sampler = vulkan::native_sampler(slot.sampler);
            if (image.is_err() || view.is_err() || sampler.is_err())
                return Failure("native texture binding unavailable");
            auto extent = texture::mip_extent(slot.view.texture().description(),
                                              slot.view.description().range.first_mip);
            vulkan::ImageParameters binding;
            binding.handle       = image.unwrap_unchecked();
            binding.view         = view.unwrap_unchecked();
            binding.sampler      = sampler.unwrap_unchecked();
            binding.extent       = { extent.width, extent.height, 1 };
            binding.mipmap_level = slot.view.description().range.mip_count;
            binding.generation   = u64(slot.view.texture().identity().resource);
            native.slots.push(rstd::move(binding));
        }
        for (const auto& upload : uploads) {
            if (texture::validate_upload(upload).is_err()) return Failure("invalid texture upload");
            bool target_found {};
            for (const auto& slot : slots) {
                if (slot.view.texture().identity() == upload.target.texture().identity()) {
                    target_found = true;
                    break;
                }
            }
            if (! target_found) return Failure("texture upload target mismatch");
        }
        auto allocation = Arc<TextureAllocation>::make(
            rstd::move(slots), rstd::move(native), rstd::move(producer));
        return QueueUpdate(rstd::move(allocation), rstd::move(uploads), transparent);
    }
    auto QueueUpdate(Arc<TextureAllocation> allocation, Vec<texture::TextureUpload> uploads,
                     bool transparent) -> Result<PreparedImageAllocation, resource::ResourceError> {
        Option<vulkan::ImageUploadTicket> ticket;
        if (! uploads.is_empty() || transparent) {
            ++m_next_ticket;
            if (m_next_ticket == u64()) ++m_next_ticket;
            ticket = Some(vulkan::ImageUploadTicket { .value = m_next_ticket });
            allocation->MarkPending();
            m_pending.push({ *ticket, allocation.clone(), rstd::move(uploads), transparent });
            statistics_.peak_pending_contents = rstd::cmp::max(statistics_.peak_pending_contents,
                                                               u64(m_pending.len().to_primitive()));
            statistics_.peak_pending_bytes =
                rstd::cmp::max(statistics_.peak_pending_bytes, PendingBytes());
        }
        return Ok(PreparedImageAllocation { rstd::move(allocation), ticket });
    }

    vulkan::TextureContext                       m_context;
    Option<Arc<dyn<TextureProducerFactory>>>     m_producers;
    Option<mut_ref<dyn<TextureUploadPublisher>>> m_publisher;
    Vec<Pending>                                 m_pending;
    Vec<texture::SubmissionToken>                m_in_flight;
    Option<texture::SubmissionReceipt>           m_recording;
    Vec<vulkan::ImageUploadTicket>               m_recorded_tickets;
    u64                                          m_next_ticket {};
};
} // namespace vrento::resource_registry

export namespace rstd
{
template<>
struct Impl<vrento::resource_registry::TexturePrepareBackend,
            vrento::resource_registry::RegistryTextureContext>
    : ImplBase<vrento::resource_registry::RegistryTextureContext> {
    auto CreateImportedTexture(sync::Arc<vrento::Image>                      image,
                               Option<sync::Arc<dyn<vrento::VideoPlayback>>> playback)
        -> Result<vrento::resource_registry::PreparedImageAllocation,
                  vrento::resource::ResourceError> {
        return self().CreateImportedTexture(rstd::move(image), rstd::move(playback));
    }
    auto AllocateTexture(const vrento::resource::TextureDefinition& definition, bool transparent)
        -> Result<vrento::resource_registry::PreparedImageAllocation,
                  vrento::resource::ResourceError> {
        return self().AllocateTexture(definition, transparent);
    }
    auto InitializeTexture(sync::Arc<vrento::resource_registry::TextureAllocation> allocation)
        -> Result<vrento::resource_registry::PreparedImageAllocation,
                  vrento::resource::ResourceError> {
        return self().InitializeTexture(rstd::move(allocation));
    }
};
} // namespace rstd
