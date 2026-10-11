export module vrento:vulkan.texture;
export import :texture;
export import :texture.prepare;
export import :vulkan.device_session;
import rstd;

export namespace vrento::vulkan
{
namespace tex = vrento::texture;
using namespace rstd::prelude;
using rstd::sync::Arc;
using rstd::sync::Weak;

inline auto texture_error(VkResult result) -> tex::Error {
    return { result == VK_ERROR_DEVICE_LOST ? tex::ErrorKind::Lost
             : result == VK_TIMEOUT         ? tex::ErrorKind::Timeout
                                            : tex::ErrorKind::Backend,
             rstd::int32_t(result) };
}
inline auto texture_stages(tex::Stages stages) -> VkPipelineStageFlags {
    VkPipelineStageFlags result {};
    auto                 bits = tex::Count(stages);
    if (bits & tex::Count(tex::Stages::Vertex)) result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Fragment)) result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Compute)) result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Transfer)) result |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (bits & tex::Count(tex::Stages::Color))
        result |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (bits & tex::Count(tex::Stages::Depth))
        result |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    return result ? result : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}
struct NativeAccess {
    VkImageLayout        layout;
    VkAccessFlags        access;
    VkPipelineStageFlags stages;
};
inline auto native_access(tex::AccessState state) -> NativeAccess {
    auto shader = texture_stages(state.stages);
    switch (state.access) {
    case tex::Access::Sampled:
        return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, shader };
    case tex::Access::ColorAttachment:
        return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    case tex::Access::DepthAttachment:
        return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };
    case tex::Access::TransferSource:
        return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT };
    case tex::Access::TransferDestination:
        return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT };
    case tex::Access::StorageRead:
        return { VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, shader };
    case tex::Access::StorageWrite:
        return { VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 shader };
    case tex::Access::Undefined:
        return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    }
    return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
}
inline auto image_usage(tex::Usage usage) -> VkImageUsageFlags {
    VkImageUsageFlags flags {};
    if (tex::has(usage, tex::Usage::Sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (tex::has(usage, tex::Usage::ColorAttachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (tex::has(usage, tex::Usage::DepthAttachment))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (tex::has(usage, tex::Usage::TransferSource)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (tex::has(usage, tex::Usage::TransferDestination)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (tex::has(usage, tex::Usage::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    return flags;
}
inline auto texture_format(VkFormat format) -> Result<TextureFormat, tex::Error> {
    TextureFormat values[] { TextureFormat::BC1,        TextureFormat::BC2,
                             TextureFormat::BC3,        TextureFormat::RGB8,
                             TextureFormat::RGBA8,      TextureFormat::RG8,
                             TextureFormat::R8,         TextureFormat::D32F,
                             TextureFormat::RGBA8_SRGB, TextureFormat::RGBA32F,
                             TextureFormat::BGRA8,      TextureFormat::BGRA8_SRGB,
                             TextureFormat::RGBA16F };
    for (auto value : values)
        if (ToVkType(value) == format) return Ok(TextureFormat(value));
    return Err(tex::Error { tex::ErrorKind::Unsupported });
}
struct TextureDeviceStatus {
    DeviceSession session;
    auto          Check() const -> Result<empty, tex::Error> {
        return session.lost() ? Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST })
                              : Result<empty, tex::Error>(Ok(empty {}));
    }
};
struct NativeTexture {
    DeviceSession                    session;
    vvk::AllocatedImage              image;
    VkImage                          borrowed {};
    Option<Arc<dyn<rstd::any::Any>>> external;
    NativeTexture(DeviceSession s, vvk::AllocatedImage value)
        : session(rstd::move(s)), image(rstd::move(value)) {}
    NativeTexture(DeviceSession s, VkImage value, Arc<dyn<rstd::any::Any>> owner)
        : session(rstd::move(s)), borrowed(value), external(Some(rstd::move(owner))) {}
};
struct NativeView {
    DeviceSession  session;
    vvk::ImageView view;
    NativeView(DeviceSession s, vvk::ImageView v): session(rstd::move(s)), view(rstd::move(v)) {}
};
struct NativeSampler {
    DeviceSession session;
    vvk::Sampler  sampler;
    NativeSampler(DeviceSession s, vvk::Sampler v)
        : session(rstd::move(s)), sampler(rstd::move(v)) {}
};
inline auto native_texture(const tex::TextureRef& texture) -> Result<VkImage, tex::Error> {
    if (! texture.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto value = rstd::any::downcast_ref<NativeTexture>(
        tex::BackendAccess::Storage(texture)->backing.deref());
    if (value.is_none()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    return Ok((**value).borrowed ? (**value).borrowed : (**value).image.handle());
}
inline auto native_view(const tex::TextureViewRef& view) -> Result<VkImageView, tex::Error> {
    if (! view.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto value = rstd::any::downcast_ref<NativeView>(tex::BackendAccess::View(view));
    if (value.is_none()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    return Ok(*(**value).view);
}
inline auto native_sampler(const tex::SamplerRef& sampler) -> Result<VkSampler, tex::Error> {
    if (! sampler.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto value = rstd::any::downcast_ref<NativeSampler>(tex::BackendAccess::Sampler(sampler));
    if (value.is_none()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    return Ok(*(**value).sampler);
}
inline auto sampled_image(const tex::TextureViewRef& view, const tex::SamplerRef& sampler)
    -> Result<ImageParameters, tex::Error> {
    if (! view.valid() || ! sampler.valid() ||
        view.texture().identity().device != sampler.identity().device)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto image  = native_texture(view.texture());
    auto native = native_view(view);
    auto sample = native_sampler(sampler);
    if (image.is_err()) return Err(image.unwrap_err());
    if (native.is_err()) return Err(native.unwrap_err());
    if (sample.is_err()) return Err(sample.unwrap_err());
    auto extent = tex::mip_extent(view.texture().description(), view.description().range.first_mip);
    ImageParameters result;
    result.handle       = image.unwrap();
    result.view         = native.unwrap();
    result.sampler      = sample.unwrap();
    result.extent       = { extent.width, extent.height, 1 };
    result.mipmap_level = view.description().range.mip_count;
    result.generation   = u64(view.texture().identity().resource);
    return Ok(rstd::move(result));
}
struct QueueSignal {
    DeviceSession                     session;
    vvk::Semaphore                    semaphore;
    PFN_vkGetSemaphoreCounterValueKHR counter {};
    PFN_vkWaitSemaphoresKHR           wait {};
    tex::Serial                       identity {};
    QueueSignal(DeviceSession s, tex::Serial id): session(rstd::move(s)), identity(id) {}
    auto value() const -> Result<tex::Serial, tex::Error> {
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        tex::Serial current {};
        auto        result =
            session.observe_result(counter(*session.device().handle(), *semaphore, &current));
        if (result != VK_SUCCESS) return Err(texture_error(result));
        return Ok(current);
    }
};
struct TextureCompletion {
    Arc<QueueSignal> signal;
    tex::Serial      value;
    auto             IdentityValue() const -> tex::Identity {
        return { signal->session.epoch(), signal->session.epoch(), signal->identity };
    }
    auto Value() const -> tex::Serial { return value; }
    auto Poll() const -> Result<bool, tex::Error> {
        auto current = signal->value();
        if (current.is_err()) return Err(current.unwrap_err());
        return Ok(current.unwrap() >= value);
    }
    auto Backend() const -> ref<dyn<rstd::any::Any>> {
        return dyn<rstd::any::Any>::from_ref(*this).as_ref();
    }
};
struct ExternalCompletion {
    DeviceSession                     session;
    PFN_vkGetSemaphoreCounterValueKHR counter;
    VkSemaphore                       semaphore;
    tex::Serial                       value;
    Arc<dyn<rstd::any::Any>>          owner;
    auto                              IdentityValue() const -> tex::Identity {
        return { session.epoch(),
                 session.epoch(),
                 tex::Serial(reinterpret_cast<rstd::uintptr_t>(semaphore)) };
    }
    auto Value() const -> tex::Serial { return value; }
    auto Poll() const -> Result<bool, tex::Error> {
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        tex::Serial current {};
        auto        result =
            session.observe_result(counter(*session.device().handle(), semaphore, &current));
        if (result != VK_SUCCESS) return Err(texture_error(result));
        return Ok(current >= value);
    }
    auto Backend() const -> ref<dyn<rstd::any::Any>> {
        return dyn<rstd::any::Any>::from_ref(*this).as_ref();
    }
};
struct TextureImport {
    VkDevice                 device;
    VkImage                  image;
    tex::TextureDesc         description;
    tex::AccessState         initial;
    rstd::uint32_t           queue_family;
    Arc<dyn<rstd::any::Any>> owner;
    tex::SubmissionToken     ready;
};
struct TextureInterop {
    using Trait                  = TextureInterop;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureInterop;
        auto ImportTexture(TextureImport desc) -> Result<tex::TextureRef, tex::Error> {
            return rstd::trait_call<0>(this, rstd::move(desc));
        }
        auto ImportDependency(VkSemaphore semaphore, tex::Serial value,
                              Arc<dyn<rstd::any::Any>> owner)
            -> Result<tex::SubmissionToken, tex::Error> {
            return rstd::trait_call<1>(this, semaphore, value, rstd::move(owner));
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::ImportTexture, &T::ImportDependency>;
};
inline auto next_texture_identity(const DeviceSession& session) -> tex::Identity {
    static tex::Serial next { 1 };
    auto               resource = __atomic_fetch_add(&next, tex::Serial(1), __ATOMIC_RELAXED);
    if (! resource) rstd::panic("texture identity exhausted");
    return { session.epoch(), session.epoch(), resource };
}
struct NativeWait {
    VkSemaphore          semaphore {};
    tex::Serial          value {};
    VkPipelineStageFlags stages {};
};
inline auto dependency_wait(const DeviceSession& session, const tex::SubmissionToken& token,
                            tex::Stages stages) -> Result<NativeWait, tex::Error> {
    if (! token.valid()) return Ok(NativeWait {});
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if (token.identity().device != session.epoch() || token.identity().epoch != session.epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto completion = rstd::any::downcast_ref<TextureCompletion>(token.backend());
    if (completion.is_some())
        return Ok(NativeWait {
            *(**completion).signal->semaphore, token.value(), texture_stages(stages) });
    auto external = rstd::any::downcast_ref<ExternalCompletion>(token.backend());
    if (external.is_none()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    return Ok(NativeWait { (**external).semaphore, token.value(), texture_stages(stages) });
}
struct TextureReadReservation {
    tex::TextureRef texture;
    bool            active { true };
    explicit TextureReadReservation(tex::TextureRef value): texture(rstd::move(value)) {
        ++tex::BackendAccess::Storage(texture)->prepared_reads;
    }
    ~TextureReadReservation() {
        if (active) --tex::BackendAccess::Storage(texture)->prepared_reads;
    }
    void release() {
        if (active) {
            --tex::BackendAccess::Storage(texture)->prepared_reads;
            active = false;
        }
    }
};
auto import_dependency(const DeviceSession& session, VkSemaphore semaphore, tex::Serial value,
                       Arc<dyn<rstd::any::Any>> owner) -> Result<tex::SubmissionToken, tex::Error>;
struct TextureRead {
    tex::TextureViewRef         view;
    VkImageView                 native {};
    tex::SubmissionToken        dependency;
    tex::Stages                 stages { tex::Stages::Fragment };
    Arc<TextureReadReservation> reservation;
};
inline auto prepare_texture_read(const DeviceSession& session, tex::TextureViewRef view,
                                 tex::Stages stages, bool linear = false)
    -> Result<TextureRead, tex::Error> {
    if (! session.valid() || ! view.valid() || ! tex::Count(stages) ||
        (tex::Count(stages) & ~tex::Count(7)))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    auto id = view.texture().identity();
    if (id.device != session.epoch()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (id.epoch != session.epoch()) return Err(tex::Error { tex::ErrorKind::Stale });
    if (! view.permits(false) ||
        ! tex::has(view.texture().description().usage, tex::Usage::Sampled))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto& desc = view.texture().description();
    auto  features =
        session.device().gpu().GetFormatProperties(ToVkType(desc.format)).optimalTilingFeatures;
    if (! (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        (linear && ! (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    auto& storage = *tex::BackendAccess::Storage(view.texture());
    auto  range   = view.description().range;
    if (storage.recording) return Err(tex::Error { tex::ErrorKind::Busy });
    for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
         ++layer)
        for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip)
            if (storage.states[usize(layer * desc.mip_levels + mip)].access != tex::Access::Sampled)
                return Err(tex::Error { tex::ErrorKind::Invalid });
    auto handle = native_view(view);
    if (handle.is_err()) return Err(handle.unwrap_err());
    auto dependency  = storage.last_use.valid() ? storage.last_use : storage.last_write;
    auto reservation = Arc<TextureReadReservation>::make(view.texture());
    return Ok(TextureRead { rstd::move(view),
                            handle.unwrap(),
                            rstd::move(dependency),
                            stages,
                            rstd::move(reservation) });
}
inline auto commit_texture_read(const TextureRead& read, const tex::SubmissionToken& completion)
    -> Result<empty, tex::Error> {
    auto& storage = *tex::BackendAccess::Storage(read.view.texture());
    if (! completion.valid() || completion.identity().device != storage.identity.device ||
        completion.identity().epoch != storage.identity.epoch || ! read.reservation->active ||
        storage.recording)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto range = read.view.description().range;
    for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
         ++layer)
        for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip) {
            auto& state = storage.states[usize(layer * storage.desc.mip_levels + mip)];
            if (state.access != tex::Access::Sampled)
                return Err(tex::Error { tex::ErrorKind::Invalid });
            state.stages = state.stages | read.stages;
        }
    storage.last_use = completion;
    read.reservation->release();
    return Ok(empty {});
}
inline void cancel_texture_read(const TextureRead& read) { read.reservation->release(); }
struct RecordingTexture {
    tex::TextureRef       texture;
    Vec<tex::AccessState> states;
    bool                  written {};
};
class TextureRecording {
public:
    Arc<QueueSignal>              signal;
    VkCommandBuffer               command {};
    tex::Serial                   identity {};
    bool                          submitted {}, uncertain {};
    ImageUploadManager            uploads;
    RecordedImageUploads          recorded_images;
    Vec<RecordingTexture>         textures;
    Vec<tex::TextureViewRef>      views;
    Vec<tex::SamplerRef>          samplers;
    Vec<tex::SubmissionToken>     dependencies;
    Vec<Box<dyn<rstd::any::Any>>> retained;
    Vec<ImageUploadBatchLease>    upload_leases;
    TextureRecording(Arc<QueueSignal> s, tex::Serial id)
        : signal(rstd::move(s)), identity(id), uploads(signal->session.device()) {}
    ~TextureRecording() {
        if (uncertain) {
            auto result = signal->session.wait_idle();
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                rstd::panic("cannot retire uncertain texture submission");
        }
        for (auto& entry : textures)
            if (tex::BackendAccess::Storage(entry.texture)->recording == identity)
                tex::BackendAccess::Storage(entry.texture)->recording = 0;
        if (command)
            signal->session.device().handle().Dispatch().vkFreeCommandBuffers(
                *signal->session.device().handle(),
                *signal->session.device().cmd_pool(),
                1,
                &command);
    }
    auto FlushUploads() -> Result<empty, tex::Error> {
        if (signal->session.lost())
            return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        vvk::CommandBuffer commands(command, signal->session.device().handle().Dispatch());
        if (! uploads.RecordQueuedRegions(commands, recorded_images))
            return Err(tex::Error { tex::ErrorKind::Backend });
        return Ok(empty {});
    }
    auto UseTexture(tex::TextureAccess use) -> Result<empty, tex::Error> {
        if (submitted || ! use.view.valid() || ! use.view.permits(tex::writes(use.access)) ||
            (! tex::Count(use.stages) || (tex::Count(use.stages) & ~tex::Count(tex::Stages::All))))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        auto flushed = FlushUploads();
        if (flushed.is_err()) return flushed;
        auto& session = signal->session;
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        auto& texture = use.view.texture();
        if (texture.identity().device != session.epoch())
            return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
        if (texture.identity().epoch != session.epoch())
            return Err(tex::Error { tex::ErrorKind::Stale });
        tex::Usage required = tex::Usage::None;
        switch (use.access) {
        case tex::Access::Undefined: return Err(tex::Error { tex::ErrorKind::Invalid });
        case tex::Access::Sampled: required = tex::Usage::Sampled; break;
        case tex::Access::ColorAttachment: required = tex::Usage::ColorAttachment; break;
        case tex::Access::DepthAttachment: required = tex::Usage::DepthAttachment; break;
        case tex::Access::TransferSource: required = tex::Usage::TransferSource; break;
        case tex::Access::TransferDestination: required = tex::Usage::TransferDestination; break;
        case tex::Access::StorageRead:
        case tex::Access::StorageWrite: required = tex::Usage::Storage; break;
        default: return Err(tex::Error { tex::ErrorKind::Invalid });
        }
        if ((use.access == tex::Access::Sampled || use.access == tex::Access::StorageRead ||
             use.access == tex::Access::StorageWrite) &&
            (tex::Count(use.stages) & ~tex::Count(7)))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        if (! tex::has(texture.description().usage, required) ||
            (use.discard && ! tex::writes(use.access)))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        auto native = native_texture(texture);
        if (native.is_err()) return Err(native.unwrap_err());
        auto range      = use.range.is_some() ? *use.range : use.view.description().range;
        auto normalized = tex::normalize(
            texture.description(),
            { range.layer_count == 1 ? tex::Dimension::D2 : tex::Dimension::Array2D, range });
        if (normalized.is_err()) return Err(normalized.unwrap_err());
        range        = normalized.unwrap().range;
        auto visible = use.view.description().range;
        if (range.aspect != visible.aspect || range.first_mip < visible.first_mip ||
            range.first_layer < visible.first_layer ||
            range.first_mip + range.mip_count > visible.first_mip + visible.mip_count ||
            range.first_layer + range.layer_count > visible.first_layer + visible.layer_count)
            return Err(tex::Error { tex::ErrorKind::Invalid });
        auto& storage = *tex::BackendAccess::Storage(texture);
        if (storage.prepared_reads) return Err(tex::Error { tex::ErrorKind::Busy });
        if (storage.recording && storage.recording != identity)
            return Err(tex::Error { tex::ErrorKind::Busy });
        usize index {};
        while (index < textures.len() && textures[index].texture.identity() != texture.identity())
            ++index;
        if (index == textures.len()) {
            RecordingTexture entry { texture };
            for (auto state : storage.states) entry.states.push(tex::AccessState(state));
            textures.push(rstd::move(entry));
            storage.recording = identity;
            auto dependency   = storage.last_use.valid() ? storage.last_use : storage.last_write;
            if (dependency.valid()) dependencies.push(rstd::move(dependency));
        }
        auto& entry = textures[index];
        entry.written |= tex::writes(use.access);
        auto& device = session.device().handle();
        for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
             ++layer) {
            for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip) {
                auto& previous =
                    entry.states[usize(layer * texture.description().mip_levels + mip)];
                auto source      = native_access(use.discard ? tex::AccessState {} : previous);
                auto destination = native_access({ use.access, use.stages });
                VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.srcAccessMask       = source.access;
                barrier.dstAccessMask       = destination.access;
                barrier.oldLayout           = source.layout;
                barrier.newLayout           = destination.layout;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image                                             = native.unwrap();
                barrier.subresourceRange = { range.aspect == tex::Aspect::Depth
                                                 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                                             mip,
                                             1,
                                             layer,
                                             1 };
                device.Dispatch().vkCmdPipelineBarrier(command,
                                                       source.stages,
                                                       destination.stages,
                                                       0,
                                                       0,
                                                       nullptr,
                                                       0,
                                                       nullptr,
                                                       1,
                                                       &barrier);
                previous = { use.access, use.stages };
            }
        }
        views.push(rstd::move(use.view));
        return Ok(empty {});
    }
    auto Upload(tex::TextureUpload upload) -> Result<empty, tex::Error> {
        auto layout = tex::validate_upload(upload);
        if (layout.is_err()) return Err(layout.unwrap_err());
        tex::ViewDesc desc {
            tex::Dimension::D2,
            { upload.target.description().range.aspect, upload.mip, 1, upload.layer, 1 }
        };
        auto access = UseTexture({ upload.target,
                                   tex::Access::TransferDestination,
                                   tex::Stages::Transfer,
                                   false,
                                   Some(desc.range) });
        if (access.is_err()) return access;
        auto    data = layout.unwrap();
        Vec<u8> packed;
        packed.reserve(usize(data.row_bytes * data.rows));
        for (tex::Serial row = 0; row < data.rows; ++row)
            for (tex::Serial byte = 0; byte < data.row_bytes; ++byte)
                packed.push(u8(upload.bytes[usize(row * data.source_pitch + byte)]));
        VkBufferImageCopy region {};
        region.imageSubresource = { desc.range.aspect == tex::Aspect::Depth
                                        ? VK_IMAGE_ASPECT_DEPTH_BIT
                                        : VK_IMAGE_ASPECT_COLOR_BIT,
                                    upload.mip,
                                    upload.layer,
                                    1 };
        region.imageOffset      = { rstd::int32_t(upload.x), rstd::int32_t(upload.y), 0 };
        region.imageExtent      = { upload.width, upload.height, 1 };
        auto image              = native_texture(upload.target.texture()).unwrap();
        auto bytes              = slice<rstd::uint8_t>::from_raw_parts(
            reinterpret_cast<const rstd::uint8_t*>(packed.data()), packed.len());
        if (uploads
                .QueueRegion(
                    image,
                    region,
                    bytes,
                    tex::format_layout(upload.target.texture().description().format).block_bytes)
                .is_none())
            return Err(tex::Error { tex::ErrorKind::Backend });
        return Ok(empty {});
    }
    auto UseSampledView(tex::TextureViewRef view, tex::SamplerRef sampler, tex::Stages stages)
        -> Result<empty, tex::Error> {
        if (! view.valid() || ! sampler.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
        if (sampler.identity().device != signal->session.epoch() ||
            sampler.identity().epoch != signal->session.epoch())
            return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
        auto& desc  = sampler.description();
        auto  flags = signal->session.device()
                          .gpu()
                          .GetFormatProperties(ToVkType(view.texture().description().format))
                          .optimalTilingFeatures;
        if ((desc.sample.magFilter == TextureFilter::LINEAR ||
             desc.sample.minFilter == TextureFilter::LINEAR) &&
            ! (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            return Err(tex::Error { tex::ErrorKind::Unsupported });
        auto result = UseTexture({ rstd::move(view), tex::Access::Sampled, stages });
        if (result.is_ok()) samplers.push(rstd::move(sampler));
        return result;
    }
    auto Wait(tex::SubmissionToken token, tex::Stages stages) -> Result<empty, tex::Error> {
        auto checked = dependency_wait(signal->session, token, stages);
        if (checked.is_err()) return Err(checked.unwrap_err());
        if (token.valid()) dependencies.push(rstd::move(token));
        return Ok(empty {});
    }
    void Retain(Box<dyn<rstd::any::Any>> owner) { retained.push(rstd::move(owner)); }
    auto Backend() -> mut_ref<dyn<rstd::any::Any>> {
        return dyn<rstd::any::Any>::from_ref(*this).as_mut_ref();
    }
};
struct TextureRecordingReceiver {
    Box<TextureRecording> state;
};
class TextureContext {
    Arc<QueueSignal>          signal_;
    Vec<tex::Submission>      pending_;
    Vec<tex::Serial>          pending_values_;
    Vec<Weak<tex::ViewState>> views_;
    Vec<tex::SamplerRef>      samplers_;
    tex::Serial               next_submission_ {};
    bool                      failed_ {};
    explicit TextureContext(Arc<QueueSignal> signal): signal_(rstd::move(signal)) {}

public:
    TextureContext(const TextureContext&)                    = delete;
    auto operator=(const TextureContext&) -> TextureContext& = delete;
    TextureContext(TextureContext&&)                         = default;
    auto operator=(TextureContext&&) -> TextureContext&      = delete;
    ~TextureContext() {
        if (! signal_) return;
        if (! pending_.is_empty()) {
            VkResult result;
            if (failed_ || session().lost())
                result = signal_->session.wait_idle();
            else {
                VkSemaphore         semaphore = *signal_->semaphore;
                VkSemaphoreWaitInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                info.semaphoreCount = 1;
                info.pSemaphores    = &semaphore;
                info.pValues        = &next_submission_;
                result              = session().observe_result(
                    signal_->wait(*session().device().handle(), &info, ~tex::Serial {}));
            }
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                rstd::panic("cannot retire texture context");
        }
        pending_.clear();
    }
    static auto create(DeviceSession session) -> Result<TextureContext, tex::Error>;
    auto        session() const -> const DeviceSession& { return signal_->session; }
    auto        QuerySupport(const tex::TextureDesc& desc) const -> Result<empty, tex::Error>;
    auto        CreateTexture(const tex::TextureDesc& desc) -> Result<tex::TextureRef, tex::Error>;
    auto        CreateView(const tex::TextureRef& texture, tex::ViewDesc desc)
        -> Result<tex::TextureViewRef, tex::Error>;
    auto CreateSampler(const tex::SamplerDesc& desc) -> Result<tex::SamplerRef, tex::Error>;
    auto ImportTexture(TextureImport desc) -> Result<tex::TextureRef, tex::Error>;
    auto ImportDependency(VkSemaphore semaphore, tex::Serial value, Arc<dyn<rstd::any::Any>> owner)
        -> Result<tex::SubmissionToken, tex::Error>;
    auto Begin() -> Result<tex::Submission, tex::Error>;
    auto Submit(tex::Submission work) -> Result<tex::SubmissionToken, tex::Error>;
    auto Poll() -> Result<empty, tex::Error>;
    auto wait(const tex::SubmissionToken& token, tex::Serial timeout = 5'000'000'000)
        -> Result<empty, tex::Error>;
};
inline auto native_recording(tex::Submission& work, const DeviceSession& session)
    -> Result<VkCommandBuffer, tex::Error> {
    if (! work.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto recorder = rstd::any::downcast_mut<TextureRecording>(work.backend());
    if (recorder.is_none() || (**recorder).signal->session.epoch() != session.epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if ((**recorder).submitted) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto flushed = (**recorder).FlushUploads();
    if (flushed.is_err()) return Err(flushed.unwrap_err());
    return Ok((**recorder).command);
}
} // namespace vrento::vulkan

export namespace rstd
{
using namespace rstd::prelude;
template<>
struct Impl<vrento::texture::ResourceStatus, vrento::vulkan::TextureDeviceStatus>
    : ImplBase<vrento::vulkan::TextureDeviceStatus> {
    auto Check() const -> Result<empty, vrento::texture::Error> { return self().Check(); }
};
template<>
struct Impl<vrento::texture::Dependency, vrento::vulkan::TextureCompletion>
    : ImplBase<vrento::vulkan::TextureCompletion> {
    auto IdentityValue() const -> vrento::texture::Identity { return self().IdentityValue(); }
    auto Value() const -> vrento::texture::Serial { return self().Value(); }
    auto Poll() const -> Result<bool, vrento::texture::Error> { return self().Poll(); }
    auto Backend() const -> ref<dyn<rstd::any::Any>> { return self().Backend(); }
};
template<>
struct Impl<vrento::texture::Dependency, vrento::vulkan::ExternalCompletion>
    : ImplBase<vrento::vulkan::ExternalCompletion> {
    auto IdentityValue() const -> vrento::texture::Identity { return self().IdentityValue(); }
    auto Value() const -> vrento::texture::Serial { return self().Value(); }
    auto Poll() const -> Result<bool, vrento::texture::Error> { return self().Poll(); }
    auto Backend() const -> ref<dyn<rstd::any::Any>> { return self().Backend(); }
};
template<>
struct Impl<vrento::texture::TextureCommands, vrento::vulkan::TextureRecordingReceiver>
    : ImplBase<vrento::vulkan::TextureRecordingReceiver> {
    auto Upload(vrento::texture::TextureUpload u) -> Result<empty, vrento::texture::Error> {
        return self().state->Upload(rstd::move(u));
    }
    auto UseTexture(vrento::texture::TextureAccess a) -> Result<empty, vrento::texture::Error> {
        return self().state->UseTexture(rstd::move(a));
    }
    auto UseSampledView(vrento::texture::TextureViewRef v, vrento::texture::SamplerRef s,
                        vrento::texture::Stages stages) -> Result<empty, vrento::texture::Error> {
        return self().state->UseSampledView(rstd::move(v), rstd::move(s), stages);
    }
    void Retain(Box<dyn<rstd::any::Any>> owner) { self().state->Retain(rstd::move(owner)); }
    auto Wait(vrento::texture::SubmissionToken token, vrento::texture::Stages stages)
        -> Result<empty, vrento::texture::Error> {
        return self().state->Wait(rstd::move(token), stages);
    }
    auto Backend() -> mut_ref<dyn<rstd::any::Any>> { return self().state->Backend(); }
};
template<>
struct Impl<vrento::vulkan::TextureInterop, vrento::vulkan::TextureContext>
    : ImplBase<vrento::vulkan::TextureContext> {
    auto ImportTexture(vrento::vulkan::TextureImport desc)
        -> Result<vrento::texture::TextureRef, vrento::texture::Error> {
        return self().ImportTexture(rstd::move(desc));
    }
    auto ImportDependency(VkSemaphore semaphore, vrento::texture::Serial value,
                          rstd::sync::Arc<dyn<rstd::any::Any>> owner)
        -> Result<vrento::texture::SubmissionToken, vrento::texture::Error> {
        return self().ImportDependency(semaphore, value, rstd::move(owner));
    }
};
template<>
struct Impl<vrento::texture::TextureDevice, vrento::vulkan::TextureContext>
    : ImplBase<vrento::vulkan::TextureContext> {
    auto QuerySupport(const vrento::texture::TextureDesc& d) const
        -> Result<empty, vrento::texture::Error> {
        return self().QuerySupport(d);
    }
    auto CreateTexture(const vrento::texture::TextureDesc& d)
        -> Result<vrento::texture::TextureRef, vrento::texture::Error> {
        return self().CreateTexture(d);
    }
    auto CreateView(const vrento::texture::TextureRef& t, vrento::texture::ViewDesc d)
        -> Result<vrento::texture::TextureViewRef, vrento::texture::Error> {
        return self().CreateView(t, d);
    }
    auto CreateSampler(const vrento::texture::SamplerDesc& d)
        -> Result<vrento::texture::SamplerRef, vrento::texture::Error> {
        return self().CreateSampler(d);
    }
};
template<>
struct Impl<vrento::texture::SubmissionQueue, vrento::vulkan::TextureContext>
    : ImplBase<vrento::vulkan::TextureContext> {
    auto Begin() -> Result<vrento::texture::Submission, vrento::texture::Error> {
        return self().Begin();
    }
    auto Submit(vrento::texture::Submission work)
        -> Result<vrento::texture::SubmissionToken, vrento::texture::Error> {
        return self().Submit(rstd::move(work));
    }
    auto Poll() -> Result<empty, vrento::texture::Error> { return self().Poll(); }
};
} // namespace rstd

namespace vrento::vulkan
{
auto TextureContext::create(DeviceSession session) -> Result<TextureContext, tex::Error> {
    if (! session.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    static tex::Serial next_signal { 1 };
    auto               signal = Arc<QueueSignal>::make(
        rstd::move(session), __atomic_fetch_add(&next_signal, tex::Serial(1), __ATOMIC_RELAXED));
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext   = &type;
    auto& device = signal->session.device().handle();
    auto  result = device.CreateSemaphore(info, signal->semaphore);
    if (result != VK_SUCCESS) return Err(texture_error(result));
    signal->counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        device.Dispatch().vkGetDeviceProcAddr(*device, "vkGetSemaphoreCounterValueKHR"));
    signal->wait = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
        device.Dispatch().vkGetDeviceProcAddr(*device, "vkWaitSemaphoresKHR"));
    if (! signal->counter || ! signal->wait) return Err(tex::Error { tex::ErrorKind::Unsupported });
    return Ok(TextureContext(rstd::move(signal)));
}
auto TextureContext::QuerySupport(const tex::TextureDesc& desc) const -> Result<empty, tex::Error> {
    auto checked = tex::validate(desc);
    if (checked.is_err()) return checked;
    if (session().lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    auto& device = session().device();
    if (desc.extent.width > device.limits().maxImageDimension2D ||
        desc.extent.height > device.limits().maxImageDimension2D ||
        desc.layers > device.limits().maxImageArrayLayers)
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    auto flags = device.gpu().GetFormatProperties(ToVkType(desc.format)).optimalTilingFeatures;
    VkFormatFeatureFlags required {};
    if (tex::has(desc.usage, tex::Usage::Sampled)) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (tex::has(desc.usage, tex::Usage::ColorAttachment))
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (tex::has(desc.usage, tex::Usage::DepthAttachment))
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (tex::has(desc.usage, tex::Usage::Storage)) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((flags & required) != required) return Err(tex::Error { tex::ErrorKind::Unsupported });
    VkImageFormatProperties properties {};
    auto result = device.instance_dispatch().vkGetPhysicalDeviceImageFormatProperties(
        *device.gpu(),
        ToVkType(desc.format),
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        image_usage(desc.usage),
        desc.dimension == tex::Dimension::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        &properties);
    if (result != VK_SUCCESS || desc.extent.width > properties.maxExtent.width ||
        desc.extent.height > properties.maxExtent.height ||
        desc.layers > properties.maxArrayLayers || desc.mip_levels > properties.maxMipLevels)
        return Err(tex::Error { tex::ErrorKind::Unsupported, rstd::int32_t(result) });
    return Ok(empty {});
}
auto TextureContext::CreateTexture(const tex::TextureDesc& desc)
    -> Result<tex::TextureRef, tex::Error> {
    auto supported = QuerySupport(desc);
    if (supported.is_err()) return Err(supported.unwrap_err());
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType   = VK_IMAGE_TYPE_2D;
    info.format      = ToVkType(desc.format);
    info.extent      = { desc.extent.width, desc.extent.height, 1 };
    info.mipLevels   = desc.mip_levels;
    info.arrayLayers = desc.layers;
    info.samples     = VK_SAMPLE_COUNT_1_BIT;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    info.usage       = image_usage(desc.usage);
    if (desc.dimension == tex::Dimension::Cube) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    auto allocation = session().device().memory_allocator().create_image(info);
    if (allocation.is_err())
        return Err(texture_error(session().observe_result(allocation.unwrap_err().api_result)));
    auto image = allocation.unwrap();
    auto owner = Arc<dyn<rstd::any::Any>>::make(NativeTexture(session(), rstd::move(image)));
    tex::TextureRef texture(next_texture_identity(session()), desc, rstd::move(owner));
    tex::BackendAccess::Storage(texture)->status =
        Some(Arc<dyn<tex::ResourceStatus>>::make(TextureDeviceStatus { session() }));
    return Ok(rstd::move(texture));
}
auto TextureContext::CreateView(const tex::TextureRef& texture, tex::ViewDesc desc)
    -> Result<tex::TextureViewRef, tex::Error> {
    if (session().lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if (! texture.valid() || texture.identity().device != session().epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto normalized = tex::normalize(texture.description(), desc);
    if (normalized.is_err()) return Err(normalized.unwrap_err());
    desc = normalized.unwrap();
    if (desc.dimension == tex::Dimension::Cube && desc.range.layer_count != 6)
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    views_.retain([](const auto& weak) {
        return ! weak.expired();
    });
    for (auto& weak : views_) {
        auto previous = weak.upgrade();
        if (previous && previous->texture.identity() == texture.identity() &&
            previous->desc == desc)
            return Ok(tex::TextureViewRef(rstd::move(previous)));
    }
    VkImageViewCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    auto                  image = native_texture(texture);
    if (image.is_err()) return Err(image.unwrap_err());
    info.image    = image.unwrap();
    info.format   = ToVkType(texture.description().format);
    info.viewType = desc.dimension == tex::Dimension::D2        ? VK_IMAGE_VIEW_TYPE_2D
                    : desc.dimension == tex::Dimension::Array2D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                    : desc.range.layer_count == 6               ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    info.subresourceRange = { desc.range.aspect == tex::Aspect::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                      : VK_IMAGE_ASPECT_COLOR_BIT,
                              desc.range.first_mip,
                              desc.range.mip_count,
                              desc.range.first_layer,
                              desc.range.layer_count };
    vvk::ImageView view;
    auto result = session().observe_result(session().device().handle().CreateImageView(info, view));
    if (result != VK_SUCCESS) return Err(texture_error(result));
    auto owner     = Arc<dyn<rstd::any::Any>>::make(NativeView(session(), rstd::move(view)));
    auto reference = tex::TextureViewRef(texture, desc, rstd::move(owner));
    views_.push(tex::BackendAccess::ViewStorage(reference).downgrade());
    return Ok(rstd::move(reference));
}
auto TextureContext::CreateSampler(const tex::SamplerDesc& desc)
    -> Result<tex::SamplerRef, tex::Error> {
    if (session().lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    auto valid = tex::validate_sampler(desc);
    if (valid.is_err()) return Err(valid.unwrap_err());
    if (rstd::f32(desc.lod_bias).abs().to_primitive() >
        session().device().limits().maxSamplerLodBias)
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    auto& s = desc.sample;
    for (auto& previous : samplers_) {
        auto& d = previous.description();
        auto& p = d.sample;
        if (p.wrapS == s.wrapS && p.wrapT == s.wrapT && p.magFilter == s.magFilter &&
            p.minFilter == s.minFilter && p.mipFilter == s.mipFilter &&
            p.compare_enable == s.compare_enable && p.compare_op == s.compare_op &&
            p.border_color == s.border_color && d.wrap_w == desc.wrap_w &&
            d.min_lod == desc.min_lod && d.max_lod == desc.max_lod && d.lod_bias == desc.lod_bias)
            return Ok(tex::SamplerRef(previous));
    }
    VkSamplerCreateInfo info { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter    = ToVkType(s.magFilter);
    info.minFilter    = ToVkType(s.minFilter);
    info.mipmapMode   = s.mipFilter == TextureMipmapFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                                    : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = ToVkType(s.wrapS);
    info.addressModeV = ToVkType(s.wrapT);
    info.addressModeW = ToVkType(desc.wrap_w);
    info.mipLodBias   = desc.lod_bias;
    info.minLod       = desc.min_lod;
    info.maxLod       = s.mipFilter == TextureMipmapFilter::None ? 0 : desc.max_lod;
    info.compareEnable = s.compare_enable;
    info.compareOp     = ToVkType(s.compare_op);
    info.borderColor   = s.border_color == TextureBorderColor::TransparentBlack
                             ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : s.border_color == TextureBorderColor::OpaqueBlack
                             ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                             : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    vvk::Sampler sampler;
    auto         result =
        session().observe_result(session().device().handle().CreateSampler(info, sampler));
    if (result != VK_SUCCESS) return Err(texture_error(result));
    auto owner = Arc<dyn<rstd::any::Any>>::make(NativeSampler(session(), rstd::move(sampler)));
    tex::SamplerRef reference(next_texture_identity(session()), desc, rstd::move(owner));
    samplers_.push(tex::SamplerRef(reference));
    return Ok(rstd::move(reference));
}
auto import_dependency(const DeviceSession& session, VkSemaphore semaphore, tex::Serial value,
                       Arc<dyn<rstd::any::Any>> owner) -> Result<tex::SubmissionToken, tex::Error> {
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if (! semaphore || ! value || ! owner) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkGetSemaphoreCounterValueKHR"));
    if (! counter) return Err(tex::Error { tex::ErrorKind::Unsupported });
    return Ok(tex::SubmissionToken(Arc<dyn<tex::Dependency>>::make(
        ExternalCompletion { session, counter, semaphore, value, rstd::move(owner) })));
}
auto TextureContext::ImportDependency(VkSemaphore semaphore, tex::Serial value,
                                      Arc<dyn<rstd::any::Any>> owner)
    -> Result<tex::SubmissionToken, tex::Error> {
    return import_dependency(session(), semaphore, value, rstd::move(owner));
}
auto TextureContext::ImportTexture(TextureImport desc) -> Result<tex::TextureRef, tex::Error> {
    auto supported = QuerySupport(desc.description);
    if (supported.is_err()) return Err(supported.unwrap_err());
    if (desc.device != *session().device().handle())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (desc.queue_family != session().device().graphics_queue().family_index)
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    if (! desc.image || ! desc.owner || desc.initial.access == tex::Access::Undefined ||
        ! tex::Count(desc.initial.stages))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    if (desc.ready.valid() && dependency_wait(session(), desc.ready, desc.initial.stages).is_err())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto owner = Arc<dyn<rstd::any::Any>>::make(
        NativeTexture(session(), desc.image, rstd::move(desc.owner)));
    tex::TextureRef texture(next_texture_identity(session()), desc.description, rstd::move(owner));
    tex::BackendAccess::Storage(texture)->status =
        Some(Arc<dyn<tex::ResourceStatus>>::make(TextureDeviceStatus { session() }));
    for (auto& state : tex::BackendAccess::Storage(texture)->states) state = desc.initial;
    tex::BackendAccess::Storage(texture)->last_write = desc.ready;
    tex::BackendAccess::Storage(texture)->last_use   = desc.ready;
    return Ok(rstd::move(texture));
}
auto TextureContext::Begin() -> Result<tex::Submission, tex::Error> {
    auto completed = Poll();
    if (completed.is_err()) return Err(completed.unwrap_err());
    static tex::Serial next_recording { 1 };
    auto               recorder = Box<TextureRecording>::make(
        signal_.clone(), __atomic_fetch_add(&next_recording, tex::Serial(1), __ATOMIC_RELAXED));
    if (! recorder->uploads.init()) return Err(tex::Error { tex::ErrorKind::Backend });
    VkCommandBufferAllocateInfo info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    info.commandPool        = *session().device().cmd_pool();
    info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    auto& device            = session().device().handle();
    auto  result            = session().observe_result(
        device.Dispatch().vkAllocateCommandBuffers(*device, &info, &recorder->command));
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result      = session().observe_result(
            device.Dispatch().vkBeginCommandBuffer(recorder->command, &begin));
    }
    if (result != VK_SUCCESS) return Err(texture_error(result));
    return Ok(tex::Submission(
        Box<dyn<tex::TextureCommands>>::make(TextureRecordingReceiver { rstd::move(recorder) })));
}
auto TextureContext::Submit(tex::Submission work) -> Result<tex::SubmissionToken, tex::Error> {
    if (! work.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    if (session().lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if (failed_) return Err(tex::Error { tex::ErrorKind::Closed });
    auto value = rstd::any::downcast_mut<TextureRecording>(work.backend());
    if (value.is_none() || &*(**value).signal != &*signal_ || (**value).submitted)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto& recording = **value;
    auto  flushed   = recording.FlushUploads();
    if (flushed.is_err()) return Err(flushed.unwrap_err());
    if (recording.recorded_images.Valid()) {
        auto lease = recording.uploads.CommitRecordedUploads(rstd::move(recording.recorded_images));
        if (lease.is_none()) return Err(tex::Error { tex::ErrorKind::Backend });
        recording.upload_leases.push(rstd::move(lease).unwrap());
    }
    auto result = session().observe_result(
        session().device().handle().Dispatch().vkEndCommandBuffer(recording.command));
    if (result != VK_SUCCESS) return Err(texture_error(result));
    Vec<VkSemaphore>          waits;
    Vec<tex::Serial>          values;
    Vec<VkPipelineStageFlags> stages;
    for (auto& dependency : recording.dependencies) {
        auto wait = dependency_wait(session(), dependency, tex::Stages::All);
        if (wait.is_err()) return Err(wait.unwrap_err());
        auto  native = wait.unwrap();
        usize index {};
        while (index < waits.len() && waits[index] != native.semaphore) ++index;
        if (index == waits.len()) {
            waits.push(VkSemaphore(native.semaphore));
            values.push(tex::Serial(native.value));
            stages.push(VkPipelineStageFlags(native.stages));
        } else if (values[index] < native.value)
            values[index] = native.value;
    }
    if (next_submission_ == ~tex::Serial {}) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto                          serial = next_submission_ + 1;
    VkSemaphore                   signal = *signal_->semaphore;
    VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline.waitSemaphoreValueCount   = unsigned(values.len().to_primitive());
    timeline.pWaitSemaphoreValues      = values.data();
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues    = &serial;
    VkSubmitInfo info { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    info.pNext                = &timeline;
    info.waitSemaphoreCount   = timeline.waitSemaphoreValueCount;
    info.pWaitSemaphores      = waits.data();
    info.pWaitDstStageMask    = stages.data();
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores    = &signal;
    info.commandBufferCount   = 1;
    info.pCommandBuffers      = &recording.command;
    result = session().observe_result(session().device().graphics_queue().handle.Submit(info));
    if (result != VK_SUCCESS) {
        failed_             = true;
        recording.uncertain = true;
        pending_.push(rstd::move(work));
        pending_values_.push(tex::Serial(serial));
        return Err(texture_error(result));
    }
    next_submission_    = serial;
    recording.submitted = true;
    tex::BackendAccess::Submitted(work);
    tex::SubmissionToken token(
        Arc<dyn<tex::Dependency>>::make(TextureCompletion { signal_.clone(), serial }));
    for (auto& entry : recording.textures) {
        auto& state     = *tex::BackendAccess::Storage(entry.texture);
        state.states    = rstd::move(entry.states);
        state.recording = 0;
        if (entry.written) state.last_write = token;
        state.last_use = token;
    }
    pending_.push(rstd::move(work));
    pending_values_.push(tex::Serial(serial));
    return Ok(rstd::move(token));
}
auto TextureContext::Poll() -> Result<empty, tex::Error> {
    if (session().lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if (failed_) return Err(tex::Error { tex::ErrorKind::Closed });
    auto current = signal_->value();
    if (current.is_err()) return Err(current.unwrap_err());
    Vec<tex::Submission> keep;
    Vec<tex::Serial>     values;
    for (usize i {}; i < pending_.len(); ++i) {
        if (pending_values_[i] > current.unwrap()) {
            keep.push(rstd::move(pending_[i]));
            values.push(tex::Serial(pending_values_[i]));
        }
    }
    pending_        = rstd::move(keep);
    pending_values_ = rstd::move(values);
    return Ok(empty {});
}
auto TextureContext::wait(const tex::SubmissionToken& token, tex::Serial timeout)
    -> Result<empty, tex::Error> {
    auto native = dependency_wait(session(), token, tex::Stages::All);
    if (native.is_err()) return Err(native.unwrap_err());
    auto dep = native.unwrap();
    if (dep.semaphore) {
        VkSemaphoreWaitInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        info.semaphoreCount = 1;
        info.pSemaphores    = &dep.semaphore;
        info.pValues        = &dep.value;
        auto result =
            session().observe_result(signal_->wait(*session().device().handle(), &info, timeout));
        if (result != VK_SUCCESS) return Err(texture_error(result));
    }
    return Poll();
}
} // namespace vrento::vulkan
