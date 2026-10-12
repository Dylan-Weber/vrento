export module vrento:resource_registry.texture;
export import :texture.prepare;
import :texture.video_playback;
import :resource;
import :vulkan.texture;
import :vulkan.media;
import rstd;

export namespace vrento::resource_registry
{
using namespace rstd::prelude;
using rstd::sync::Arc;

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
            -> Result<empty, texture::Error> {
            return rstd::trait_call<0>(this, context, seconds);
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Pump>;
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
    void MarkSubmitted(texture::SubmissionToken completion) {
        m_completion = rstd::move(completion);
    }
    auto Pump(vulkan::TextureContext& context, double seconds) -> Result<empty, texture::Error> {
        return m_producer.is_some() ? (*m_producer)->Pump(context, seconds) : Ok(empty {});
    }

private:
    Vec<TextureSlot>                  m_slots;
    vulkan::ImageSlotsRef             m_native;
    Option<Arc<dyn<TextureProducer>>> m_producer;
    texture::SubmissionToken          m_completion;
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
        auto Create(Arc<Image> image, Option<Arc<dyn<VideoPlayback>>> playback)
            -> Result<ProducedTexture, resource::ResourceError> {
            return rstd::trait_call<0>(this, rstd::move(image), rstd::move(playback));
        }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Create>;
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
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::CreateImportedTexture, &T::AllocateTexture>;
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

export namespace rstd
{
template<>
struct Impl<vrento::resource_registry::TextureProducer,
            sync::Arc<dyn<vrento::vulkan::MediaTextureProducer>>>
    : ImplBase<sync::Arc<dyn<vrento::vulkan::MediaTextureProducer>>> {
    auto Pump(vrento::vulkan::TextureContext& context, double seconds)
        -> Result<empty, vrento::texture::Error> {
        auto result = self()->Pump(context, seconds);
        if (result.is_err()) return Err(result.unwrap_err_unchecked());
        return Ok(empty {});
    }
};
} // namespace rstd

export namespace vrento::resource_registry
{

class RegistryTextureContext {
public:
    static constexpr usize MaxPendingContents { 4 };
    static constexpr usize MaxUploadBatches { 3 };
    explicit RegistryTextureContext(vulkan::TextureContext context)
        : m_context(rstd::move(context)), m_media(m_context.session()) {}

    auto Context() -> vulkan::TextureContext& { return m_context; }
    auto Context() const -> const vulkan::TextureContext& { return m_context; }
    void SetProducerFactory(Arc<dyn<TextureProducerFactory>> factory) {
        m_producers = Some(rstd::move(factory));
    }
    void SetUploadPublisher(mut_ref<dyn<TextureUploadPublisher>> publisher) {
        m_publisher = Some(publisher);
    }
    void SetVideoDecodeOptions(vulkan::MediaDecodeOptions options) {
        m_media.SetOptions(rstd::move(options));
    }
    void ClearProducers() { m_media.Clear(); }
    auto DrainProducers(u64 timeout) -> Result<empty, texture::Error> {
        return m_media.Drain(timeout);
    }

    auto CreateImportedTexture(Arc<Image> image, Option<Arc<dyn<VideoPlayback>>> playback)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        if (image->header.kind == ImageKind::Video) {
            auto window = MakeRoom();
            if (window.is_err()) return Failure("texture upload window failed");
            if (m_producers.is_some()) {
                auto result = (*m_producers)->Create(rstd::move(image), rstd::move(playback));
                if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
                auto prepared = rstd::move(result).unwrap_unchecked();
                return Queue(rstd::move(prepared.slots),
                             rstd::move(prepared.uploads),
                             false,
                             rstd::move(prepared.producer));
            }
            auto result = m_media.Prepare(m_context, *image, rstd::move(playback));
            if (result.is_err()) return Failure("prepare texture producer failed");
            auto             media = rstd::move(result).unwrap_unchecked();
            Vec<TextureSlot> slots;
            slots.push({ rstd::move(media.view), rstd::move(media.sampler) });
            auto producer = Arc<dyn<TextureProducer>>::make(rstd::move(media.producer));
            return Queue(
                rstd::move(slots), rstd::move(media.uploads), false, Some(rstd::move(producer)));
        }
        auto window = MakeRoom();
        if (window.is_err()) return Failure("texture upload window failed");
        auto                         device = dyn<texture::TextureDevice>::from_ref(m_context);
        texture::ImagePrepareContext preparer(device.as_mut_ref());
        auto content = dyn<texture::TextureContentPreparer>::from_ref(preparer);
        auto result  = content->PrepareImage(*image);
        if (result.is_err()) return Failure("prepare texture pixels failed");
        auto                        prepared = rstd::move(result).unwrap_unchecked();
        Vec<TextureSlot>            slots;
        Vec<texture::TextureUpload> uploads;
        for (auto& item : prepared) {
            slots.push({ rstd::move(item.view), rstd::move(item.sampler) });
            for (auto& upload : item.uploads) uploads.push(rstd::move(upload));
        }
        return Queue(rstd::move(slots), rstd::move(uploads), false);
    }

    auto AllocateTexture(const resource::TextureDefinition& definition, bool transparent)
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        if (definition.width <= i32() || definition.height <= i32())
            return Failure("invalid texture extent");
        auto window = MakeRoom();
        if (window.is_err()) return Failure("texture upload window failed");
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
        if (support.is_err()) return Failure("texture definition unsupported");
        auto allocated = m_context.CreateTexture(desc);
        if (allocated.is_err()) return Failure("allocate texture failed");
        auto storage = rstd::move(allocated).unwrap_unchecked();
        auto view    = m_context.CreateView(
            storage, { texture::Dimension::D2, { texture::format_layout(desc.format).aspect } });
        if (view.is_err()) return Failure("create texture view failed");
        auto sampler = m_context.CreateSampler({ definition.sample });
        if (sampler.is_err()) return Failure("create texture sampler failed");
        texture::TextureViewRef attachment;
        if (texture::has(desc.usage, texture::Usage::ColorAttachment) ||
            texture::has(desc.usage, texture::Usage::DepthAttachment)) {
            auto bound = m_context.CreateView(
                storage,
                { texture::Dimension::D2,
                  { texture::format_layout(desc.format).aspect, 0, 1, 0, 1 } });
            if (bound.is_err()) return Failure("create texture attachment view failed");
            attachment = rstd::move(bound).unwrap_unchecked();
        }
        Vec<TextureSlot> slots;
        slots.push({ rstd::move(view).unwrap_unchecked(),
                     rstd::move(sampler).unwrap_unchecked(),
                     rstd::move(attachment) });
        return Queue(rstd::move(slots), {}, transparent);
    }

    bool HasPendingUploads() const { return ! m_pending.is_empty(); }
    auto PendingCount() const -> usize { return m_pending.len(); }
    auto InFlightCount() const -> usize { return m_in_flight.len(); }

    bool RecordPendingUploads(texture::Submission& work) {
        if (m_recording.is_some()) return false;
        m_in_flight.reserve(m_in_flight.len() + usize(1));
        if (m_publisher.is_some()) (*m_publisher)->Reserve();
        m_recorded_tickets.clear();
        m_recorded_tickets.reserve(m_pending.len());
        auto commands = work.commands();
        for (auto& pending : m_pending) {
            m_recorded_tickets.push(vulkan::ImageUploadTicket(pending.ticket));
            work.retain(pending.allocation.clone());
            for (const auto& upload : pending.uploads) {
                if (commands->Upload(upload.clone()).is_err()) return false;
            }
            if (pending.transparent) {
                for (const auto& slot : pending.allocation->Slots()) {
                    if (commands
                            ->UseTexture({ slot.view,
                                           texture::Access::TransferDestination,
                                           texture::Stages::Transfer,
                                           true })
                            .is_err())
                        return false;
                    auto command = vulkan::native_recording(work, m_context.session());
                    auto image   = vulkan::native_texture(slot.view.texture());
                    if (command.is_err() || image.is_err()) return false;
                    const auto&             range = slot.view.description().range;
                    VkImageSubresourceRange native { VK_IMAGE_ASPECT_COLOR_BIT,
                                                     range.first_mip,
                                                     range.mip_count,
                                                     range.first_layer,
                                                     range.layer_count };
                    VkClearColorValue       color {};
                    m_context.session().device().handle().Dispatch().vkCmdClearColorImage(
                        command.unwrap_unchecked(),
                        image.unwrap_unchecked(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &color,
                        1,
                        &native);
                }
            }
            for (const auto& slot : pending.allocation->Slots()) {
                if (texture::has(slot.view.texture().description().usage,
                                 texture::Usage::Sampled) &&
                    commands->UseSampledView(slot.view, slot.sampler, texture::Stages::Fragment)
                        .is_err())
                    return false;
            }
        }
        m_recording = Some(work.receipt());
        return true;
    }

    bool ConfirmSubmitted(const texture::SubmissionReceipt& receipt,
                          const texture::SubmissionToken&   token) {
        if (! receipt.submitted() || ! token.valid()) return false;
        for (auto& pending : m_pending) pending.allocation->MarkSubmitted(token);
        if (m_publisher.is_some()) (*m_publisher)->Submitted(m_recorded_tickets.as_slice(), token);
        m_pending.clear();
        m_recorded_tickets.clear();
        m_recording = None();
        m_in_flight.push(texture::SubmissionToken(token));
        return true;
    }
    void CancelRecording() {
        m_recording = None();
        m_recorded_tickets.clear();
    }
    void DiscardPendingUploads() {
        m_pending.clear();
        CancelRecording();
    }

    auto Poll() -> Result<empty, texture::Error> {
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
        auto waited = m_context.wait(m_in_flight[usize()]);
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
    struct Pending {
        vulkan::ImageUploadTicket   ticket;
        Arc<TextureAllocation>      allocation;
        Vec<texture::TextureUpload> uploads;
        bool                        transparent;
    };
    template<rstd::size_t N>
    static auto Failure(const char (&message)[N])
        -> Result<PreparedImageAllocation, resource::ResourceError> {
        return Err(resource::ResourceError { .message = rstd::format("{}", message) });
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
        Option<vulkan::ImageUploadTicket> ticket;
        if (! uploads.is_empty() || transparent) {
            ++m_next_ticket;
            if (m_next_ticket == u64()) ++m_next_ticket;
            ticket = Some(vulkan::ImageUploadTicket { .value = m_next_ticket });
            m_pending.push({ *ticket, allocation.clone(), rstd::move(uploads), transparent });
        }
        return Ok(PreparedImageAllocation { rstd::move(allocation), ticket });
    }

    vulkan::TextureContext                       m_context;
    vulkan::MediaRegistry                        m_media;
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
};
} // namespace rstd
