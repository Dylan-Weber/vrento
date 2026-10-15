export module vrento:vulkan.texture;
export import :texture;
export import :texture.prepare;
export import :vulkan.device_session;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
using rstd::sync::Weak;

export namespace vrento::vulkan
{
namespace tex = vrento::texture;

inline auto texture_error(VkResult result) -> tex::Error {
    return { result == VK_ERROR_DEVICE_LOST ? tex::ErrorKind::Lost
             : result == VK_TIMEOUT         ? tex::ErrorKind::Timeout
                                            : tex::ErrorKind::Backend,
             rstd::int32_t(result) };
}
inline auto texture_stages(tex::Stages stages) -> VkPipelineStageFlags {
    if (stages == tex::Stages::All) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags result {};
    auto                 bits = tex::Count(stages);
    if (bits & tex::Count(tex::Stages::Vertex)) result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Fragment)) result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Compute)) result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Geometry)) result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (bits & tex::Count(tex::Stages::Transfer)) result |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (bits & tex::Count(tex::Stages::Color))
        result |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (bits & tex::Count(tex::Stages::Depth))
        result |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    return result ? result : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}
struct NativeAccess {
    VkImageLayout        layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkAccessFlags        access {};
    VkPipelineStageFlags stages { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    rstd::uint32_t       queue_family { VK_QUEUE_FAMILY_IGNORED };
    rstd::uint32_t       released_from { VK_QUEUE_FAMILY_IGNORED };
    VkImageLayout        release_old_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    friend bool          operator==(const NativeAccess&, const NativeAccess&) = default;
};
inline auto native_access(tex::AccessState state, tex::Aspect aspect = tex::Aspect::Color)
    -> NativeAccess {
    auto shader = texture_stages(state.stages);
    switch (state.access) {
    case tex::Access::Sampled:
        return { aspect == tex::Aspect::Depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT,
                 shader };
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
struct NativeTextureStates {
    Vec<NativeAccess> states;
    bool              uncertain {};
};
struct TextureDeviceStatus {
    DeviceSession            session;
    Arc<NativeTextureStates> texture;
    auto                     Check() const -> Result<empty, tex::Error> {
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        if (texture && texture->uncertain) return Err(tex::Error { tex::ErrorKind::Closed });
        return Ok(empty {});
    }
};
struct NativeTexture {
    DeviceSession                    session;
    vvk::AllocatedImage              image;
    VkImage                          borrowed {};
    Option<Arc<dyn<rstd::any::Any>>> external;
    Arc<NativeTextureStates>         tracking { Arc<NativeTextureStates>::make() };
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
    if ((**value).tracking->uncertain) return Err(tex::Error { tex::ErrorKind::Closed });
    auto checked = texture.check();
    if (checked.is_err()) return Err(checked.unwrap_err());
    return Ok((**value).borrowed ? (**value).borrowed : (**value).image.handle());
}
inline auto native_view(const tex::TextureViewRef& view) -> Result<VkImageView, tex::Error> {
    if (! view.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto checked = native_texture(view.texture());
    if (checked.is_err()) return Err(checked.unwrap_err());
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
struct SampledImageBinding {
    ImageParameters       image;
    VkImageLayout         layout;
    tex::TextureViewRef   view;
    tex::SamplerRef       sampler;
    VkFormat              format;
    VkSampleCountFlagBits samples;
};
inline auto sampled_image(const tex::TextureViewRef& view, const tex::SamplerRef& sampler)
    -> Result<SampledImageBinding, tex::Error> {
    if (! view.valid() || ! sampler.valid() ||
        view.texture().identity().device != sampler.identity().device ||
        view.texture().identity().epoch != sampler.identity().epoch)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto& desc = view.texture().description();
    if (! tex::has(desc.usage, tex::Usage::Sampled) || desc.samples != 1 || ! view.permits(false))
        return Err(tex::Error { tex::ErrorKind::Invalid });
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
    return Ok(SampledImageBinding { result,
                                    native_access({ tex::Access::Sampled, tex::Stages::Fragment },
                                                  view.description().range.aspect)
                                        .layout,
                                    view,
                                    sampler,
                                    ToVkType(desc.format),
                                    VkSampleCountFlagBits(desc.samples) });
}
inline auto attachment_image(const tex::TextureViewRef& view)
    -> Result<SampledImageBinding, tex::Error> {
    if (! view.valid() || ! view.permits(true) || view.description().range.mip_count != 1)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto& desc  = view.texture().description();
    bool  depth = view.description().range.aspect == tex::Aspect::Depth;
    if (! tex::has(desc.usage, depth ? tex::Usage::DepthAttachment : tex::Usage::ColorAttachment))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto image  = native_texture(view.texture());
    auto native = native_view(view);
    if (image.is_err()) return Err(image.unwrap_err());
    if (native.is_err()) return Err(native.unwrap_err());
    auto            extent = tex::mip_extent(desc, view.description().range.first_mip);
    ImageParameters result;
    result.handle     = image.unwrap();
    result.view       = native.unwrap();
    result.extent     = { extent.width, extent.height, 1 };
    result.generation = u64(view.texture().identity().resource);
    return Ok(SampledImageBinding { result,
                                    depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    view,
                                    {},
                                    ToVkType(desc.format),
                                    VkSampleCountFlagBits(desc.samples) });
}
struct QueueSignal {
    DeviceSession                     session;
    vvk::Semaphore                    semaphore;
    PFN_vkGetSemaphoreCounterValueKHR counter {};
    PFN_vkWaitSemaphoresKHR           wait {};
    tex::Serial                       identity {};
    QueueParameters                   queue;
    vvk::CommandPool                  pool;
    QueueSignal(DeviceSession s, tex::Serial id, QueueParameters selected)
        : session(rstd::move(s)), identity(id), queue(selected) {}
    auto value() const -> Result<tex::Serial, tex::Error> {
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        tex::Serial current {};
        auto        result =
            session.observe_result(counter(*session.device().handle(), *semaphore, &current));
        if (result != VK_SUCCESS) return Err(texture_error(result));
        return Ok(current);
    }
};
struct SubmissionFence {
    DeviceSession                    session;
    vvk::Fence                       owned;
    VkFence                          borrowed {};
    Option<Arc<dyn<rstd::any::Any>>> owner;
    bool                             completed {};
    explicit SubmissionFence(DeviceSession value): session(rstd::move(value)) {}
    auto handle() const -> VkFence { return borrowed ? borrowed : *owned; }
    auto Poll() -> Result<bool, tex::Error> {
        if (completed) return Ok(true);
        auto result = session.observe_result(session.device().handle().Dispatch().vkGetFenceStatus(
            *session.device().handle(), handle()));
        if (result == VK_NOT_READY) return Ok(false);
        if (result != VK_SUCCESS) return Err(texture_error(result));
        completed = true;
        return Ok(true);
    }
    auto Wait(tex::Serial timeout) -> Result<empty, tex::Error> {
        if (completed) return Ok(empty {});
        auto fence  = handle();
        auto result = session.observe_result(session.device().handle().Dispatch().vkWaitForFences(
            *session.device().handle(), 1, &fence, VK_TRUE, timeout));
        if (result != VK_SUCCESS) return Err(texture_error(result));
        completed = true;
        return Ok(empty {});
    }
};
struct TextureCompletion {
    Arc<QueueSignal>     signal;
    tex::Serial          value;
    Arc<SubmissionFence> fence;
    auto                 IdentityValue() const -> tex::Identity {
        return { signal->session.epoch(), signal->session.epoch(), signal->identity };
    }
    auto Value() const -> tex::Serial { return value; }
    auto Poll() const -> Result<bool, tex::Error> {
        auto current = signal->value();
        if (current.is_err()) return Err(current.unwrap_err());
        return current.unwrap() < value ? Result<bool, tex::Error>(Ok(false)) : fence->Poll();
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
    Option<NativeAccess>     native_initial;
};
enum class NativeSemaphoreKind
{
    Binary,
    Timeline
};
struct NativeSemaphoreWait {
    VkDevice                 device {};
    VkSemaphore              semaphore {};
    NativeSemaphoreKind      kind { NativeSemaphoreKind::Binary };
    tex::Serial              value {};
    VkPipelineStageFlags     stages { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
    Arc<dyn<rstd::any::Any>> owner;
};
struct NativeSemaphoreSignal {
    VkDevice                 device {};
    VkSemaphore              semaphore {};
    NativeSemaphoreKind      kind { NativeSemaphoreKind::Binary };
    tex::Serial              value {};
    Arc<dyn<rstd::any::Any>> owner;
};
// Poll the context after host fence completion, before resetting or reusing that fence.
struct NativeFence {
    VkDevice                 device {};
    VkFence                  fence {};
    Arc<dyn<rstd::any::Any>> owner;
};
struct ProducerTarget {
    ImageParameters      image;
    NativeAccess         initial, final;
    tex::SubmissionToken dependency;
};
struct ProducerWriteDesc {
    tex::TextureViewRef      view;
    tex::AccessState         final { tex::Access::Sampled, tex::Stages::Fragment };
    Option<NativeAccess>     native_final;
    Arc<dyn<rstd::any::Any>> owner;
};
struct ProducerReservation {
    DeviceSession            session;
    tex::TextureViewRef      view;
    ProducerTarget           target;
    tex::AccessState         final;
    Arc<dyn<rstd::any::Any>> owner;
    tex::Serial              identity;
    bool                     active { true };
    bool                     uncertain {};
    ProducerReservation(DeviceSession s, ProducerWriteDesc desc, ProducerTarget value,
                        tex::Serial id)
        : session(rstd::move(s)),
          view(rstd::move(desc.view)),
          target(rstd::move(value)),
          final(desc.final),
          owner(rstd::move(desc.owner)),
          identity(id) {}
    ~ProducerReservation() {
        if (uncertain) {
            auto result = session.wait_idle();
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                rstd::panic("cannot retire uncertain texture producer");
        }
        release();
    }
    void release() {
        if (active && tex::BackendAccess::Storage(view.texture())->recording == identity)
            tex::BackendAccess::Storage(view.texture())->recording = 0;
        active = false;
    }
};
class TextureContext;
class ProducerWrite {
    Arc<ProducerReservation> reservation_;
    friend class TextureContext;
    explicit ProducerWrite(Arc<ProducerReservation> value): reservation_(rstd::move(value)) {}

public:
    ProducerWrite(const ProducerWrite&)                    = delete;
    auto operator=(const ProducerWrite&) -> ProducerWrite& = delete;
    ProducerWrite(ProducerWrite&&)                         = default;
    auto operator=(ProducerWrite&&) -> ProducerWrite&      = default;
    auto target() const -> const ProducerTarget& { return reservation_->target; }
    bool active() const { return bool(reservation_) && reservation_->active; }
    void cancel() {
        if (reservation_) reservation_->release();
    }
    // Use this instead of cancellation if the producer may have submitted work.
    void uncertain() {
        if (! active()) return;
        reservation_->uncertain = true;
        auto backing = tex::BackendAccess::Storage(reservation_->view.texture())->backing.deref();
        rstd::any::downcast_ref<NativeTexture>(backing).unwrap()->tracking->uncertain = true;
    }
};
struct NativeSubmission {
    using Trait                  = NativeSubmission;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = NativeSubmission;
        auto AttachWait(tex::Submission& work, NativeSemaphoreWait wait)
            -> Result<empty, tex::Error> {
            return rstd::trait_call<0>(this, work, rstd::move(wait));
        }
        auto AttachSignal(tex::Submission& work, NativeSemaphoreSignal signal)
            -> Result<empty, tex::Error> {
            return rstd::trait_call<1>(this, work, rstd::move(signal));
        }
        auto AttachFence(tex::Submission& work, NativeFence fence) -> Result<empty, tex::Error> {
            return rstd::trait_call<2>(this, work, rstd::move(fence));
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::AttachWait, &T::AttachSignal, &T::AttachFence>;
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
        auto BeginProducerWrite(ProducerWriteDesc desc) -> Result<ProducerWrite, tex::Error> {
            return rstd::trait_call<2>(this, rstd::move(desc));
        }
        auto CommitProducerWrite(ProducerWrite& write, tex::SubmissionToken completion)
            -> Result<empty, tex::Error> {
            return rstd::trait_call<3>(this, write, rstd::move(completion));
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::ImportTexture, &T::ImportDependency, &T::BeginProducerWrite,
                                   &T::CommitProducerWrite>;
};
inline auto next_texture_identity(const DeviceSession& session) -> tex::Identity {
    static tex::Serial next { 1 };
    auto               resource = __atomic_fetch_add(&next, tex::Serial(1), __ATOMIC_RELAXED);
    if (! resource) rstd::panic("texture identity exhausted");
    return { session.epoch(), session.epoch(), resource };
}
inline auto next_texture_recording() -> tex::Serial {
    static tex::Serial next { 1 };
    auto               result = __atomic_fetch_add(&next, tex::Serial(1), __ATOMIC_RELAXED);
    if (! result) rstd::panic("texture recording identity exhausted");
    return result;
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
    bool            native_only;
    explicit TextureReadReservation(tex::TextureRef value, bool native = true)
        : texture(rstd::move(value)), native_only(native) {
        ++tex::BackendAccess::Storage(texture)->prepared_reads;
        if (native_only) ++tex::BackendAccess::Storage(texture)->prepared_native_reads;
    }
    ~TextureReadReservation() { release(); }
    void release() {
        if (active) {
            --tex::BackendAccess::Storage(texture)->prepared_reads;
            if (native_only) --tex::BackendAccess::Storage(texture)->prepared_native_reads;
            active = false;
        }
    }
};
inline auto reserve_texture_read(const DeviceSession& session, const tex::TextureViewRef& view)
    -> Result<Arc<TextureReadReservation>, tex::Error> {
    if (! session.valid() || ! view.valid() || ! view.permits(false))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto id = view.texture().identity();
    if (id.device != session.epoch() || id.epoch != session.epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto native = native_view(view);
    if (native.is_err()) return Err(native.unwrap_err());
    const auto& storage = *tex::BackendAccess::Storage(view.texture());
    if (storage.recording) return Err(tex::Error { tex::ErrorKind::Busy });
    const auto range = view.description().range;
    for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
         ++layer)
        for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip)
            if (storage.states[usize(layer * storage.desc.mip_levels + mip)].access ==
                tex::Access::Undefined)
                return Err(tex::Error { tex::ErrorKind::Invalid });
    return Ok(Arc<TextureReadReservation>::make(view.texture(), false));
}
auto import_dependency(const DeviceSession& session, VkSemaphore semaphore, tex::Serial value,
                       Arc<dyn<rstd::any::Any>> owner) -> Result<tex::SubmissionToken, tex::Error>;
struct TextureRead {
    tex::TextureViewRef         view;
    VkImageView                 native {};
    tex::SubmissionToken        dependency;
    tex::Stages                 stages { tex::Stages::Fragment };
    Arc<TextureReadReservation> reservation;
};
inline auto validate_sampled_texture(const DeviceSession& session, const tex::TextureViewRef& view,
                                     tex::Stages stages, bool linear = false)
    -> Result<VkImageView, tex::Error> {
    if (! session.valid() || ! view.valid() || ! tex::Count(stages) ||
        (tex::Count(stages) & ~tex::Count(tex::Stages::Shaders)))
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
    return native_view(view);
}
inline auto prepare_texture_read(const DeviceSession& session, tex::TextureViewRef view,
                                 tex::Stages stages, bool linear = false)
    -> Result<TextureRead, tex::Error> {
    auto handle = validate_sampled_texture(session, view, stages, linear);
    if (handle.is_err()) return Err(handle.unwrap_err());
    const auto& desc         = view.texture().description();
    auto&       storage      = *tex::BackendAccess::Storage(view.texture());
    auto        native_state = rstd::any::downcast_ref<NativeTexture>(storage.backing.deref());
    if (native_state.is_none()) return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto range = view.description().range;
    if (storage.recording) return Err(tex::Error { tex::ErrorKind::Busy });
    for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
         ++layer)
        for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip)
            if (storage.states[usize(layer * desc.mip_levels + mip)].access !=
                    tex::Access::Sampled ||
                (**native_state).tracking->states[usize(layer * desc.mip_levels + mip)].layout !=
                    native_access({ tex::Access::Sampled, stages }, range.aspect).layout ||
                (**native_state)
                        .tracking->states[usize(layer * desc.mip_levels + mip)]
                        .queue_family != session.device().graphics_queue().family_index)
                return Err(tex::Error { tex::ErrorKind::Invalid });
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
            auto backend = rstd::any::downcast_ref<NativeTexture>(storage.backing.deref()).unwrap();
            backend->tracking->states[usize(layer * storage.desc.mip_levels + mip)].stages |=
                texture_stages(read.stages);
        }
    storage.last_use = completion;
    read.reservation->release();
    return Ok(empty {});
}
inline void cancel_texture_read(const TextureRead& read) { read.reservation->release(); }
struct RecordingTexture {
    tex::TextureRef       texture;
    Vec<tex::AccessState> states;
    Vec<NativeAccess>     native_states;
    bool                  written {};
};
struct RecordingDependency {
    tex::SubmissionToken token;
    tex::Stages          stages;
};
struct NativeSubmissionParticipant {
    using Trait                  = NativeSubmissionParticipant;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = NativeSubmissionParticipant;
        void Commit(tex::SubmissionToken token) { rstd::trait_call<0>(this, rstd::move(token)); }
        void Uncertain() { rstd::trait_call<1>(this); }
        void Cancel() { rstd::trait_call<2>(this); }
    };
    template<typename T>
    using Funcs = TraitFuncs<&T::Commit, &T::Uncertain, &T::Cancel>;
};
class TextureRecording {
public:
    Arc<QueueSignal>                 signal;
    VkCommandBuffer                  command {};
    tex::Serial                      identity {};
    bool                             submitted {}, uncertain {};
    ImageUploadManager               uploads;
    RecordedImageUploads             recorded_images;
    Vec<RecordingTexture>            textures;
    Vec<tex::TextureViewRef>         views;
    Vec<tex::SamplerRef>             samplers;
    Vec<RecordingDependency>         dependencies;
    Vec<Arc<TextureReadReservation>> prepared_reads;
    Vec<Box<dyn<rstd::any::Any>>>    retained;
    Vec<ImageUploadBatchLease>       upload_leases;
    Vec<NativeSemaphoreWait>         native_waits;
    Vec<NativeSemaphoreSignal>       native_signals;
    Option<NativeFence>              fence;
    Option<Arc<SubmissionFence>>     completion_fence;
    struct SurfaceAccess {
        FrameSurfaceLease        lease;
        NativeAccess             state;
        Arc<dyn<rstd::any::Any>> owner;
        bool                     released {};
    };
    Vec<SurfaceAccess>                         surfaces;
    Vec<Arc<dyn<NativeSubmissionParticipant>>> participants;
    TextureRecording(Arc<QueueSignal> s, tex::Serial id)
        : signal(rstd::move(s)), identity(id), uploads(signal->session.device()) {}
    ~TextureRecording() {
        if (uncertain) {
            auto result = signal->session.wait_idle();
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                rstd::panic("cannot retire uncertain texture submission");
        }
        if (! submitted && ! uncertain)
            for (auto& participant : participants) participant->Cancel();
        for (auto& entry : textures)
            if (tex::BackendAccess::Storage(entry.texture)->recording == identity)
                tex::BackendAccess::Storage(entry.texture)->recording = 0;
        if (command)
            signal->session.device().handle().Dispatch().vkFreeCommandBuffers(
                *signal->session.device().handle(), *signal->pool, 1, &command);
    }
    auto FlushUploads() -> Result<empty, tex::Error> {
        if (signal->session.lost())
            return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        vvk::CommandBuffer commands(command, signal->session.device().handle().Dispatch());
        auto               recorded = uploads.RecordQueuedRegions(commands, recorded_images);
        if (recorded.is_err()) return Err(tex::Error::from_backend(recorded.unwrap_err()));
        return Ok(empty {});
    }
    auto RecordAccess(tex::TextureAccess use, bool emit_barrier,
                      Option<NativeAccess> final = None(), bool handoff = false)
        -> Result<empty, tex::Error> {
        if (submitted) return Err(tex::Error { tex::ErrorKind::Invalid });
        auto checked = tex::validate_access(use);
        if (checked.is_err()) return Err(checked.unwrap_err());
        auto flushed = FlushUploads();
        if (flushed.is_err()) return flushed;
        auto& session = signal->session;
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        auto& texture = use.view.texture();
        if (texture.identity().device != session.epoch())
            return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
        if (texture.identity().epoch != session.epoch())
            return Err(tex::Error { tex::ErrorKind::Stale });
        auto native = native_texture(texture);
        if (native.is_err()) return Err(native.unwrap_err());
        auto  range   = checked.unwrap();
        auto& storage = *tex::BackendAccess::Storage(texture);
        if (storage.prepared_reads && (tex::writes(use.access) || storage.prepared_native_reads))
            return Err(tex::Error { tex::ErrorKind::Busy });
        if (storage.recording && storage.recording != identity)
            return Err(tex::Error { tex::ErrorKind::Busy });
        usize index {};
        while (index < textures.len() && textures[index].texture.identity() != texture.identity())
            ++index;
        if (index == textures.len()) {
            if (! emit_barrier) return Err(tex::Error { tex::ErrorKind::Invalid });
            RecordingTexture entry { texture };
            for (auto state : storage.states) entry.states.push(tex::AccessState(state));
            auto backend = rstd::any::downcast_ref<NativeTexture>(storage.backing.deref()).unwrap();
            for (auto state : backend->tracking->states)
                entry.native_states.push(NativeAccess(state));
            textures.push(rstd::move(entry));
            storage.recording = identity;
            auto dependency   = storage.last_use.valid() ? storage.last_use : storage.last_write;
            if (dependency.valid())
                dependencies.push(RecordingDependency { rstd::move(dependency), tex::Stages::All });
        }
        auto& entry = textures[index];
        if (use.require_contents)
            for (tex::Count layer = range.first_layer;
                 layer < range.first_layer + range.layer_count;
                 ++layer)
                for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count;
                     ++mip)
                    if (entry.states[usize(layer * texture.description().mip_levels + mip)]
                            .access == tex::Access::Undefined)
                        return Err(tex::Error { tex::ErrorKind::Invalid });
        auto destination =
            final.is_some() ? *final : native_access({ use.access, use.stages }, range.aspect);
        auto family = signal->queue.family_index;
        if (destination.queue_family == VK_QUEUE_FAMILY_IGNORED) destination.queue_family = family;
        for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
             ++layer)
            for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip) {
                auto source =
                    entry.native_states[usize(layer * texture.description().mip_levels + mip)];
                if ((! emit_barrier && source.queue_family != family) ||
                    (destination.queue_family != family && source.queue_family != family) ||
                    (source.released_from != VK_QUEUE_FAMILY_IGNORED &&
                     (source.queue_family != family || destination.queue_family != family ||
                      source.layout != destination.layout)) ||
                    (source.released_from == VK_QUEUE_FAMILY_IGNORED &&
                     source.queue_family != family &&
                     source.queue_family != VK_QUEUE_FAMILY_IGNORED))
                    return Err(tex::Error { tex::ErrorKind::Unsupported });
            }
        if (! handoff) entry.written |= tex::writes(use.access);
        auto& device = session.device().handle();
        for (tex::Count layer = range.first_layer; layer < range.first_layer + range.layer_count;
             ++layer) {
            for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip) {
                auto& previous =
                    entry.states[usize(layer * texture.description().mip_levels + mip)];
                auto& native_previous =
                    entry.native_states[usize(layer * texture.description().mip_levels + mip)];
                auto source  = native_previous;
                bool acquire = source.released_from != VK_QUEUE_FAMILY_IGNORED;
                if (use.discard) {
                    // Discarding contents does not remove prior GPU access dependencies.
                    source.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                }
                VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.srcAccessMask = source.access;
                barrier.dstAccessMask = destination.access;
                barrier.oldLayout     = acquire ? source.release_old_layout : source.layout;
                barrier.newLayout     = destination.layout;
                bool transfer = acquire || (source.queue_family != destination.queue_family &&
                                            source.queue_family != VK_QUEUE_FAMILY_IGNORED);
                barrier.srcQueueFamilyIndex =
                    transfer ? (acquire ? source.released_from : source.queue_family)
                             : VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex =
                    transfer ? destination.queue_family : VK_QUEUE_FAMILY_IGNORED;
                if (acquire) barrier.srcAccessMask = 0;
                if (transfer && ! acquire && source.queue_family == family)
                    barrier.dstAccessMask = 0;
                barrier.image            = native.unwrap();
                barrier.subresourceRange = { range.aspect == tex::Aspect::Depth
                                                 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                 : VK_IMAGE_ASPECT_COLOR_BIT,
                                             mip,
                                             1,
                                             layer,
                                             1 };
                if (emit_barrier)
                    device.Dispatch().vkCmdPipelineBarrier(
                        command,
                        acquire ? VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                : source.stages,
                        transfer && ! acquire && source.queue_family == family
                            ? VkPipelineStageFlags(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
                            : destination.stages,
                        0,
                        0,
                        nullptr,
                        0,
                        nullptr,
                        1,
                        &barrier);
                if (! handoff) previous = { use.access, use.stages };
                native_previous = destination;
                if (transfer && ! acquire) {
                    native_previous.released_from      = family;
                    native_previous.release_old_layout = source.layout;
                }
            }
        }
        views.push(rstd::move(use.view));
        return Ok(empty {});
    }
    auto UseTexture(tex::TextureAccess use) -> Result<empty, tex::Error> {
        return RecordAccess(rstd::move(use), true);
    }
    auto DiscardContents(const tex::TextureViewRef& view) -> Result<empty, tex::Error> {
        if (! view.valid() || ! view.permits(true))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        for (auto& entry : textures) {
            if (entry.texture.identity() != view.texture().identity()) continue;
            const auto range = view.description().range;
            for (tex::Count layer = range.first_layer;
                 layer < range.first_layer + range.layer_count;
                 ++layer)
                for (tex::Count mip = range.first_mip; mip < range.first_mip + range.mip_count;
                     ++mip)
                    entry.states[usize(layer * entry.texture.description().mip_levels + mip)] =
                        tex::AccessState {};
            entry.written = true;
            return Ok(empty {});
        }
        return Err(tex::Error { tex::ErrorKind::Invalid });
    }
    auto RecordFinalState(tex::TextureAccess use) -> Result<empty, tex::Error> {
        if (use.discard) return Err(tex::Error { tex::ErrorKind::Invalid });
        return RecordAccess(rstd::move(use), false);
    }
    auto RecordNativeAccess(tex::TextureAccess use, NativeAccess state)
        -> Result<empty, tex::Error> {
        auto family = signal->queue.family_index;
        if (state.layout == VK_IMAGE_LAYOUT_UNDEFINED || ! state.stages ||
            (state.queue_family != VK_QUEUE_FAMILY_IGNORED && state.queue_family != family &&
             state.queue_family != VK_QUEUE_FAMILY_EXTERNAL &&
             state.queue_family != VK_QUEUE_FAMILY_FOREIGN_EXT))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        return RecordAccess(rstd::move(use), true, Some(state));
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
        auto    bytes = slice<u8>::from_raw_parts(upload.bytes.as_slice().as_raw_ptr(),
                                                  usize(data.row_bytes * data.rows));
        if (data.source_pitch != data.row_bytes) {
            packed.reserve(usize(data.row_bytes * data.rows));
            for (tex::Serial row = 0; row < data.rows; ++row) {
                auto start = usize(row * data.source_pitch);
                packed.extend_from_slice(slice<u8>::from_raw_parts(
                    upload.bytes.as_slice().as_raw_ptr() + start.to_primitive(),
                    usize(data.row_bytes)));
            }
            bytes = packed.as_slice();
        }
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
        auto content            = slice<rstd::uint8_t>::from_raw_parts(
            reinterpret_cast<const rstd::uint8_t*>(bytes.as_raw_ptr()), bytes.len());
        auto queued = uploads.QueueRegion(
            image,
            region,
            content,
            tex::format_layout(upload.target.texture().description().format).block_bytes);
        if (queued.is_err()) return Err(tex::Error::from_backend(queued.unwrap_err()));
        return Ok(empty {});
    }
    auto UseSampledView(tex::TextureViewRef view, tex::SamplerRef sampler, tex::Stages stages)
        -> Result<empty, tex::Error> {
        if (! view.valid() || ! sampler.valid() || view.texture().description().samples != 1)
            return Err(tex::Error { tex::ErrorKind::Invalid });
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
        if (! tex::Count(stages) || (tex::Count(stages) & ~tex::Count(tex::Stages::All)))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        auto checked = dependency_wait(signal->session, token, stages);
        if (checked.is_err()) return Err(checked.unwrap_err());
        if (token.valid()) dependencies.push(RecordingDependency { rstd::move(token), stages });
        return Ok(empty {});
    }
    void Retain(Box<dyn<rstd::any::Any>> owner) { retained.push(rstd::move(owner)); }
    auto Backend() -> mut_ref<dyn<rstd::any::Any>> {
        return dyn<rstd::any::Any>::from_ref(*this).as_mut_ref();
    }
};
struct NativeQueueSubmit {
    using Trait                  = NativeQueueSubmit;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = NativeQueueSubmit;
        auto Submit(const vvk::Queue& queue, const VkSubmitInfo& info, VkFence fence) -> VkResult {
            return rstd::trait_call<0>(this, queue, info, fence);
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Submit>;
};
struct TextureRecordingReceiver {
    Box<TextureRecording> state;
};
class TextureContext {
    Arc<QueueSignal>                    signal_;
    Option<Box<dyn<NativeQueueSubmit>>> submitter_;
    Vec<tex::Submission>                pending_;
    Vec<tex::Serial>                    pending_values_;
    Vec<Weak<tex::ViewState>>           views_;
    Vec<tex::SamplerRef>                samplers_;
    struct ProducerPending {
        Arc<ProducerReservation> reservation;
        tex::SubmissionToken     completion;
    };
    Vec<ProducerPending> producers_;
    tex::Serial          next_submission_ {};
    bool                 failed_ {};
    explicit TextureContext(Arc<QueueSignal> signal, Option<Box<dyn<NativeQueueSubmit>>> submitter)
        : signal_(rstd::move(signal)), submitter_(rstd::move(submitter)) {}

public:
    TextureContext(const TextureContext&)                    = delete;
    auto operator=(const TextureContext&) -> TextureContext& = delete;
    TextureContext(TextureContext&&)                         = default;
    auto operator=(TextureContext&&) -> TextureContext&      = delete;
    ~TextureContext() {
        if (! signal_) return;
        if (! pending_.is_empty() || ! producers_.is_empty()) {
            auto result = signal_->session.wait_idle();
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                rstd::panic("cannot retire texture context");
        }
        pending_.clear();
        producers_.clear();
    }
    static auto create(DeviceSession                       session,
                       Option<Box<dyn<NativeQueueSubmit>>> submitter = None())
        -> Result<TextureContext, tex::Error>;
    static auto create_on_queue(DeviceSession session, QueueParameters queue,
                                Option<Box<dyn<NativeQueueSubmit>>> submitter = None())
        -> Result<TextureContext, tex::Error>;
    auto session() const -> const DeviceSession& { return signal_->session; }
    auto queue() const -> const QueueParameters& { return signal_->queue; }
    auto QuerySupport(const tex::TextureDesc& desc) const -> Result<empty, tex::Error>;
    auto CreateTexture(const tex::TextureDesc& desc) -> Result<tex::TextureRef, tex::Error>;
    auto CreateView(const tex::TextureRef& texture, tex::ViewDesc desc)
        -> Result<tex::TextureViewRef, tex::Error>;
    auto CreateSampler(const tex::SamplerDesc& desc) -> Result<tex::SamplerRef, tex::Error>;
    auto ImportTexture(TextureImport desc) -> Result<tex::TextureRef, tex::Error>;
    auto ImportDependency(VkSemaphore semaphore, tex::Serial value, Arc<dyn<rstd::any::Any>> owner)
        -> Result<tex::SubmissionToken, tex::Error>;
    auto AttachWait(tex::Submission& work, NativeSemaphoreWait wait) -> Result<empty, tex::Error>;
    auto AttachSignal(tex::Submission& work, NativeSemaphoreSignal signal)
        -> Result<empty, tex::Error>;
    auto AttachFence(tex::Submission& work, NativeFence fence) -> Result<empty, tex::Error>;
    auto BeginProducerWrite(ProducerWriteDesc desc) -> Result<ProducerWrite, tex::Error>;
    auto CommitProducerWrite(ProducerWrite& write, tex::SubmissionToken completion)
        -> Result<empty, tex::Error>;
    auto Begin() -> Result<tex::Submission, tex::Error>;
    auto Submit(tex::Submission work) -> Result<tex::SubmissionToken, tex::Error>;
    auto Poll() -> Result<empty, tex::Error>;
    auto wait(const tex::SubmissionToken& token, tex::Serial timeout = 5'000'000'000)
        -> Result<empty, tex::Error>;
};
inline auto texture_recording(tex::Submission& work, const DeviceSession& session)
    -> Result<mut_ref<TextureRecording>, tex::Error> {
    if (! work.valid() || ! session.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto recorder = rstd::any::downcast_mut<TextureRecording>(work.backend());
    if (recorder.is_none() || (**recorder).signal->session.epoch() != session.epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    if ((**recorder).submitted || (**recorder).uncertain)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    return Ok(*recorder);
}
inline auto texture_recorded_in(tex::Submission& work, const DeviceSession& session,
                                const tex::TextureViewRef& view) -> Result<bool, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    if (! view.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto identity = view.texture().identity();
    if (identity.device != session.epoch() || identity.epoch != session.epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    return Ok(tex::BackendAccess::Storage(view.texture())->recording ==
              recording.unwrap()->identity);
}
inline auto native_recording(tex::Submission& work, const DeviceSession& session)
    -> Result<VkCommandBuffer, tex::Error> {
    auto recorder = texture_recording(work, session);
    if (recorder.is_err()) return Err(recorder.unwrap_err());
    auto state   = recorder.unwrap();
    auto flushed = state->FlushUploads();
    if (flushed.is_err()) return Err(flushed.unwrap_err());
    return Ok(state->command);
}
inline auto native_queue(tex::Submission& work, const DeviceSession& session)
    -> Result<QueueParameters, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    return Ok(recording.unwrap()->signal->queue);
}
inline auto release_texture(tex::Submission& work, const DeviceSession& session,
                            tex::TextureAccess use, rstd::uint32_t destination)
    -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    if (recording.unwrap()->signal->queue.family_index == destination) return Ok(empty {});
    bool found {};
    for (const auto& queue : session.device().queues())
        found |=
            queue.family_index == destination &&
            (queue.flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    if (! found) return Err(tex::Error { tex::ErrorKind::Unsupported });
    auto access = native_access({ use.access, use.stages }, use.view.description().range.aspect);
    access.queue_family = destination;
    return recording.unwrap()->RecordAccess(rstd::move(use), true, Some(access), true);
}
inline auto discard_texture_contents(tex::Submission& work, const DeviceSession& session,
                                     const tex::TextureViewRef& view) -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    return recording.unwrap()->DiscardContents(view);
}
inline auto attach_prepared_texture_read(tex::Submission& work, const DeviceSession& session,
                                         Arc<TextureReadReservation> reservation)
    -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    if (! reservation || ! reservation->active) return Err(tex::Error { tex::ErrorKind::Stale });
    for (const auto& previous : recording.unwrap()->prepared_reads)
        if (Arc<TextureReadReservation>::ptr_eq(previous, reservation)) return Ok(empty {});
    recording.unwrap()->prepared_reads.push(rstd::move(reservation));
    return Ok(empty {});
}
inline auto native_command_recording(tex::Submission& work, const DeviceSession& session)
    -> Result<vvk::CommandBuffer, tex::Error> {
    auto command = native_recording(work, session);
    if (command.is_err()) return Err(command.unwrap_err());
    return Ok(vvk::CommandBuffer(command.unwrap(), session.device().handle().Dispatch()));
}
class TextureMipmapProgram {
    DeviceSession       session_;
    tex::TextureViewRef view_;
    tex::Range          range_;
    VkImage             image_;
    TextureMipmapProgram(DeviceSession session, tex::TextureViewRef view, tex::Range range,
                         VkImage image)
        : session_(rstd::move(session)), view_(rstd::move(view)), range_(range), image_(image) {}

public:
    static auto prepare(DeviceSession session, tex::TextureViewRef view,
                        Option<tex::Range> selection = None())
        -> Result<TextureMipmapProgram, tex::Error> {
        if (! session.valid() || ! view.valid() || ! view.permits(true))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        auto identity = view.texture().identity();
        if (identity.device != session.epoch() || identity.epoch != session.epoch())
            return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
        const auto& desc  = view.texture().description();
        auto        range = selection.unwrap_or(view.description().range);
        if (desc.samples != 1 || range.aspect != tex::Aspect::Color ||
            ! tex::has(desc.usage, tex::Usage::TransferSource) ||
            ! tex::has(desc.usage, tex::Usage::TransferDestination))
            return Err(tex::Error { tex::ErrorKind::Unsupported });
        auto checked = tex::validate_access(
            { view, tex::Access::TransferSource, tex::Stages::Transfer, false, Some(range) });
        if (checked.is_err()) return Err(checked.unwrap_err());
        if (range.mip_count > 1) {
            auto       features = session.device()
                                      .gpu()
                                      .GetFormatProperties(ToVkType(desc.format))
                                      .optimalTilingFeatures;
            const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((features & required) != required)
                return Err(tex::Error { tex::ErrorKind::Unsupported });
        }
        auto image = native_texture(view.texture());
        if (image.is_err()) return Err(image.unwrap_err());
        return Ok(
            TextureMipmapProgram(rstd::move(session), rstd::move(view), range, image.unwrap()));
    }
    auto record(tex::Submission& work) const -> Result<empty, tex::Error> {
        const auto& desc    = view_.texture().description();
        const auto  range   = range_;
        auto        command = native_recording(work, session_);
        if (command.is_err()) return Err(command.unwrap_err());
        for (tex::Count mip = range.first_mip + 1; mip < range.first_mip + range.mip_count; ++mip) {
            auto source           = range;
            source.first_mip      = mip - 1;
            source.mip_count      = 1;
            auto destination      = source;
            destination.first_mip = mip;
            auto read             = work.commands()->UseTexture({ view_,
                                                                  tex::Access::TransferSource,
                                                                  tex::Stages::Transfer,
                                                                  false,
                                                                  Some(source),
                                                                  true });
            if (read.is_err()) return Err(read.unwrap_err());
            auto write = work.commands()->UseTexture({ view_,
                                                       tex::Access::TransferDestination,
                                                       tex::Stages::Transfer,
                                                       true,
                                                       Some(destination) });
            if (write.is_err()) return Err(write.unwrap_err());
            const auto  from = tex::mip_extent(desc, mip - 1), to = tex::mip_extent(desc, mip);
            VkImageBlit blit {};
            blit.srcSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, range.first_layer, range.layer_count
            };
            blit.dstSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT, mip, range.first_layer, range.layer_count
            };
            blit.srcOffsets[1] = { rstd::int32_t(from.width), rstd::int32_t(from.height), 1 };
            blit.dstOffsets[1] = { rstd::int32_t(to.width), rstd::int32_t(to.height), 1 };
            session_.device().handle().Dispatch().vkCmdBlitImage(
                command.unwrap(),
                image_,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &blit,
                VK_FILTER_LINEAR);
        }
        return work.commands()->UseTexture({ view_,
                                             tex::Access::Sampled,
                                             tex::Stages::Fragment,
                                             false,
                                             Some(tex::Range(range)),
                                             true });
    }
};
inline auto generate_texture_mipmaps(tex::Submission& work, const DeviceSession& session,
                                     tex::TextureViewRef view,
                                     Option<tex::Range>  selection = None())
    -> Result<empty, tex::Error> {
    auto prepared = TextureMipmapProgram::prepare(session, rstd::move(view), selection);
    if (prepared.is_err()) return Err(prepared.unwrap_err());
    return prepared->record(work);
}
inline auto RecordFinalState(tex::Submission& work, const DeviceSession& session,
                             tex::TextureAccess access) -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    return recording.unwrap()->RecordFinalState(rstd::move(access));
}
inline auto RecordNativeAccess(tex::Submission& work, const DeviceSession& session,
                               tex::TextureAccess access, NativeAccess state)
    -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    return recording.unwrap()->RecordNativeAccess(rstd::move(access), state);
}
inline auto AcquireTexture(tex::Submission& work, const DeviceSession& session,
                           tex::TextureAccess access) -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    return recording.unwrap()->UseTexture(rstd::move(access));
}
inline auto ReleaseTexture(tex::Submission& work, const DeviceSession& session,
                           tex::TextureAccess access, NativeAccess external)
    -> Result<empty, tex::Error> {
    if (external.queue_family != VK_QUEUE_FAMILY_FOREIGN_EXT &&
        external.queue_family != VK_QUEUE_FAMILY_EXTERNAL)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    return RecordNativeAccess(work, session, rstd::move(access), external);
}
inline void record_surface_barrier(TextureRecording& recording, const FrameSurfaceLease& lease,
                                   NativeAccess source, NativeAccess destination) {
    auto                 family   = recording.signal->queue.family_index;
    bool                 transfer = source.queue_family != destination.queue_family;
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = transfer && destination.queue_family == family ? 0 : source.access;
    barrier.dstAccessMask = transfer && source.queue_family == family ? 0 : destination.access;
    barrier.oldLayout     = source.layout;
    barrier.newLayout     = destination.layout;
    barrier.srcQueueFamilyIndex = transfer ? source.queue_family : VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = transfer ? destination.queue_family : VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = lease.image.handle;
    barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    recording.signal->session.device().handle().Dispatch().vkCmdPipelineBarrier(
        recording.command,
        transfer && destination.queue_family == family
            ? VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
            : source.stages,
        transfer && source.queue_family == family
            ? VkPipelineStageFlags(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
            : destination.stages,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}
inline auto recordAcquire(tex::Submission& work, const DeviceSession& session,
                          const FrameSurfaceLease& lease, Arc<dyn<rstd::any::Any>> owner,
                          NativeAccess target = native_access({ tex::Access::ColorAttachment,
                                                                tex::Stages::Color }))
    -> Result<empty, tex::Error> {
    if (! lease.valid() || ! owner || target.layout == VK_IMAGE_LAYOUT_UNDEFINED || ! target.stages)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto checked = texture_recording(work, session);
    if (checked.is_err()) return Err(checked.unwrap_err());
    auto recording = checked.unwrap();
    for (auto& previous : recording->surfaces)
        if (previous.lease.identity == lease.identity ||
            previous.lease.image.handle == lease.image.handle)
            return Err(tex::Error { tex::ErrorKind::Busy });
    auto flushed = recording->FlushUploads();
    if (flushed.is_err()) return flushed;
    target.queue_family = session.device().graphics_queue().family_index;
    NativeAccess initial { lease.discard_content ? VK_IMAGE_LAYOUT_UNDEFINED : lease.initial_layout,
                           0,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           lease.initial_queue_family };
    if (lease.acquire.kind == FrameSurfaceAcquireKind::BinarySemaphore)
        recording->native_waits.push(NativeSemaphoreWait { *session.device().handle(),
                                                           lease.acquire.semaphore,
                                                           NativeSemaphoreKind::Binary,
                                                           0,
                                                           target.stages,
                                                           owner.clone() });
    record_surface_barrier(*recording, lease, initial, target);
    recording->surfaces.push(TextureRecording::SurfaceAccess { lease, target, rstd::move(owner) });
    return Ok(empty {});
}
inline auto recordSurfaceDependency(tex::Submission& work, const DeviceSession& session,
                                    const FrameSurfaceLease& lease) -> Result<empty, tex::Error> {
    auto recording = texture_recording(work, session);
    if (recording.is_err()) return Err(recording.unwrap_err());
    for (const auto& surface : recording.unwrap()->surfaces)
        if (surface.lease.identity == lease.identity &&
            surface.lease.image.handle == lease.image.handle)
            return surface.released
                       ? Result<empty, tex::Error>(Err(tex::Error { tex::ErrorKind::Invalid }))
                       : Result<empty, tex::Error>(Ok(empty {}));
    return Err(tex::Error { tex::ErrorKind::Invalid });
}
inline auto recordSurfaceAccess(tex::Submission& work, const DeviceSession& session,
                                const FrameSurfaceLease& lease, Arc<dyn<rstd::any::Any>> owner,
                                NativeAccess target) -> Result<empty, tex::Error> {
    auto checked = texture_recording(work, session);
    if (checked.is_err()) return Err(checked.unwrap_err());
    auto recording      = checked.unwrap();
    target.queue_family = session.device().graphics_queue().family_index;
    for (auto& surface : recording->surfaces)
        if (surface.lease.identity == lease.identity &&
            surface.lease.image.handle == lease.image.handle) {
            if (surface.released) return Err(tex::Error { tex::ErrorKind::Invalid });
            record_surface_barrier(*recording, lease, surface.state, target);
            surface.state = target;
            return Ok(empty {});
        }
    return recordAcquire(work, session, lease, rstd::move(owner), target);
}
inline auto recordSurfaceFinalState(tex::Submission& work, const DeviceSession& session,
                                    const FrameSurfaceLease& lease, NativeAccess state)
    -> Result<empty, tex::Error> {
    if (state.layout == VK_IMAGE_LAYOUT_UNDEFINED || ! state.stages)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto checked = texture_recording(work, session);
    if (checked.is_err()) return Err(checked.unwrap_err());
    for (auto& surface : checked.unwrap()->surfaces)
        if (surface.lease.identity == lease.identity &&
            surface.lease.image.handle == lease.image.handle && ! surface.released) {
            state.queue_family = session.device().graphics_queue().family_index;
            surface.state      = state;
            return Ok(empty {});
        }
    return Err(tex::Error { tex::ErrorKind::Stale });
}
inline auto recordRelease(tex::Submission& work, const DeviceSession& session,
                          const FrameSurfaceLease& lease) -> Result<empty, tex::Error> {
    auto checked = texture_recording(work, session);
    if (checked.is_err()) return Err(checked.unwrap_err());
    auto recording = checked.unwrap();
    for (auto& surface : recording->surfaces)
        if (surface.lease.identity == lease.identity &&
            surface.lease.image.handle == lease.image.handle && ! surface.released) {
            record_surface_barrier(*recording,
                                   surface.lease,
                                   surface.state,
                                   { surface.lease.final_layout,
                                     0,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     surface.lease.final_queue_family });
            surface.released = true;
            return Ok(empty {});
        }
    return Err(tex::Error { tex::ErrorKind::Stale });
}
} // namespace vrento::vulkan

export namespace rstd
{

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
    auto BeginProducerWrite(vrento::vulkan::ProducerWriteDesc desc)
        -> Result<vrento::vulkan::ProducerWrite, vrento::texture::Error> {
        return self().BeginProducerWrite(rstd::move(desc));
    }
    auto CommitProducerWrite(vrento::vulkan::ProducerWrite&   write,
                             vrento::texture::SubmissionToken completion)
        -> Result<empty, vrento::texture::Error> {
        return self().CommitProducerWrite(write, rstd::move(completion));
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
struct Impl<vrento::vulkan::NativeSubmission, vrento::vulkan::TextureContext>
    : ImplBase<vrento::vulkan::TextureContext> {
    auto AttachWait(vrento::texture::Submission& work, vrento::vulkan::NativeSemaphoreWait wait)
        -> Result<empty, vrento::texture::Error> {
        return self().AttachWait(work, rstd::move(wait));
    }
    auto AttachSignal(vrento::texture::Submission&          work,
                      vrento::vulkan::NativeSemaphoreSignal signal)
        -> Result<empty, vrento::texture::Error> {
        return self().AttachSignal(work, rstd::move(signal));
    }
    auto AttachFence(vrento::texture::Submission& work, vrento::vulkan::NativeFence fence)
        -> Result<empty, vrento::texture::Error> {
        return self().AttachFence(work, rstd::move(fence));
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
auto TextureContext::create(DeviceSession session, Option<Box<dyn<NativeQueueSubmit>>> submitter)
    -> Result<TextureContext, tex::Error> {
    if (! session.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    return create_on_queue(session, session.device().graphics_queue(), rstd::move(submitter));
}
auto TextureContext::create_on_queue(DeviceSession session, QueueParameters selected,
                                     Option<Box<dyn<NativeQueueSubmit>>> submitter)
    -> Result<TextureContext, tex::Error> {
    if (! session.valid()) return Err(tex::Error { tex::ErrorKind::Invalid });
    if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
    bool found {};
    for (const auto& queue : session.device().queues())
        found |= *queue.handle == *selected.handle && queue.family_index == selected.family_index &&
                 queue.queue_index == selected.queue_index && queue.flags == selected.flags;
    if (! found ||
        ! (selected.flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    static tex::Serial next_signal { 1 };
    auto               signal =
        Arc<QueueSignal>::make(rstd::move(session),
                               __atomic_fetch_add(&next_signal, tex::Serial(1), __ATOMIC_RELAXED),
                               selected);
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext                     = &type;
    auto&                   device = signal->session.device().handle();
    VkCommandPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = selected.family_index;
    auto pool_result           = device.CreateCommandPool(pool_info, signal->pool);
    if (pool_result != VK_SUCCESS) return Err(texture_error(pool_result));
    auto result = device.CreateSemaphore(info, signal->semaphore);
    if (result != VK_SUCCESS) return Err(texture_error(result));
    signal->counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        device.Dispatch().vkGetDeviceProcAddr(*device, "vkGetSemaphoreCounterValueKHR"));
    signal->wait = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
        device.Dispatch().vkGetDeviceProcAddr(*device, "vkWaitSemaphoresKHR"));
    if (! signal->counter || ! signal->wait) return Err(tex::Error { tex::ErrorKind::Unsupported });
    return Ok(TextureContext(rstd::move(signal), rstd::move(submitter)));
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
        desc.layers > properties.maxArrayLayers || desc.mip_levels > properties.maxMipLevels ||
        ! (properties.sampleCounts & VkSampleCountFlags(desc.samples)) ||
        (desc.samples != 1 && tex::has(desc.usage, tex::Usage::Storage)))
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
    info.samples     = VkSampleCountFlagBits(desc.samples);
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    info.usage       = image_usage(desc.usage);
    if (desc.dimension == tex::Dimension::Cube) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    auto allocation = session().device().memory_allocator().create_image(info);
    if (allocation.is_err()) {
        auto error = allocation.unwrap_err();
        session().observe_result(error.api_result);
        return Err(tex::Error::from_backend(
            { "allocate texture image"_Str, BackendCause::Memory(error) }));
    }
    auto image = allocation.unwrap();
    auto owner = Arc<dyn<rstd::any::Any>>::make(NativeTexture(session(), rstd::move(image)));
    tex::TextureRef texture(next_texture_identity(session()), desc, rstd::move(owner));
    auto            native = rstd::any::downcast_ref<NativeTexture>(
                                 tex::BackendAccess::Storage(texture)->backing.deref())
                                 .unwrap();
    for (tex::Count i = 0; i < desc.layers * desc.mip_levels; ++i)
        native->tracking->states.push(NativeAccess { VK_IMAGE_LAYOUT_UNDEFINED,
                                                     0,
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                     VK_QUEUE_FAMILY_IGNORED });
    tex::BackendAccess::Storage(texture)->status = Some(Arc<dyn<tex::ResourceStatus>>::make(
        TextureDeviceStatus { session(), native->tracking.clone() }));
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
    if (desc.queue_family != session().device().graphics_queue().family_index &&
        desc.queue_family != VK_QUEUE_FAMILY_IGNORED &&
        desc.queue_family != VK_QUEUE_FAMILY_FOREIGN_EXT &&
        desc.queue_family != VK_QUEUE_FAMILY_EXTERNAL)
        return Err(tex::Error { tex::ErrorKind::Unsupported });
    if (! desc.image || ! desc.owner || desc.initial.access == tex::Access::Undefined ||
        ! tex::Count(desc.initial.stages))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    if (desc.ready.valid() && dependency_wait(session(), desc.ready, desc.initial.stages).is_err())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto owner = Arc<dyn<rstd::any::Any>>::make(
        NativeTexture(session(), desc.image, rstd::move(desc.owner)));
    tex::TextureRef texture(next_texture_identity(session()), desc.description, rstd::move(owner));
    for (auto& state : tex::BackendAccess::Storage(texture)->states) state = desc.initial;
    auto native = rstd::any::downcast_ref<NativeTexture>(
                      tex::BackendAccess::Storage(texture)->backing.deref())
                      .unwrap();
    tex::BackendAccess::Storage(texture)->status = Some(Arc<dyn<tex::ResourceStatus>>::make(
        TextureDeviceStatus { session(), native->tracking.clone() }));
    auto initial =
        desc.native_initial.is_some()
            ? *desc.native_initial
            : native_access(desc.initial, tex::format_layout(desc.description.format).aspect);
    initial.queue_family = desc.queue_family;
    if (initial.layout == VK_IMAGE_LAYOUT_UNDEFINED || ! initial.stages)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    for (tex::Count i = 0; i < desc.description.layers * desc.description.mip_levels; ++i)
        native->tracking->states.push(NativeAccess(initial));
    tex::BackendAccess::Storage(texture)->last_write       = desc.ready;
    tex::BackendAccess::Storage(texture)->content_revision = 1;
    tex::BackendAccess::Storage(texture)->last_use         = desc.ready;
    return Ok(rstd::move(texture));
}
auto TextureContext::AttachWait(tex::Submission& work, NativeSemaphoreWait wait)
    -> Result<empty, tex::Error> {
    auto checked = texture_recording(work, session());
    if (checked.is_err()) return Err(checked.unwrap_err());
    if (wait.device != *session().device().handle() || &*checked.unwrap()->signal != &*signal_)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (! wait.semaphore || ! wait.owner || ! wait.stages ||
        wait.semaphore == *signal_->semaphore ||
        (wait.kind != NativeSemaphoreKind::Binary && wait.kind != NativeSemaphoreKind::Timeline) ||
        (wait.kind == NativeSemaphoreKind::Binary && wait.value))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    for (auto& previous : checked.unwrap()->native_waits)
        if (previous.semaphore == wait.semaphore) {
            if (previous.kind != wait.kind) return Err(tex::Error { tex::ErrorKind::Invalid });
            previous.value = rstd::cmp::max(previous.value, wait.value);
            previous.stages |= wait.stages;
            checked.unwrap()->Retain(Box<dyn<rstd::any::Any>>::make(rstd::move(wait.owner)));
            return Ok(empty {});
        }
    checked.unwrap()->native_waits.push(rstd::move(wait));
    return Ok(empty {});
}
auto TextureContext::AttachSignal(tex::Submission& work, NativeSemaphoreSignal signal)
    -> Result<empty, tex::Error> {
    auto checked = texture_recording(work, session());
    if (checked.is_err()) return Err(checked.unwrap_err());
    if (signal.device != *session().device().handle() || &*checked.unwrap()->signal != &*signal_)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (! signal.semaphore || ! signal.owner || signal.semaphore == *signal_->semaphore ||
        (signal.kind != NativeSemaphoreKind::Binary &&
         signal.kind != NativeSemaphoreKind::Timeline) ||
        (signal.kind == NativeSemaphoreKind::Binary && signal.value) ||
        (signal.kind == NativeSemaphoreKind::Timeline && ! signal.value))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    for (auto& previous : checked.unwrap()->native_signals)
        if (previous.semaphore == signal.semaphore)
            return Err(tex::Error { tex::ErrorKind::Invalid });
    checked.unwrap()->native_signals.push(rstd::move(signal));
    return Ok(empty {});
}
auto TextureContext::AttachFence(tex::Submission& work, NativeFence fence)
    -> Result<empty, tex::Error> {
    auto checked = texture_recording(work, session());
    if (checked.is_err()) return Err(checked.unwrap_err());
    if (fence.device != *session().device().handle() || &*checked.unwrap()->signal != &*signal_)
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    if (! fence.fence || ! fence.owner || checked.unwrap()->fence.is_some())
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto result = session().observe_result(
        session().device().handle().Dispatch().vkGetFenceStatus(fence.device, fence.fence));
    if (result != VK_NOT_READY)
        return result == VK_SUCCESS ? Err(tex::Error { tex::ErrorKind::Invalid })
                                    : Result<empty, tex::Error>(Err(texture_error(result)));
    checked.unwrap()->fence = Some(rstd::move(fence));
    return Ok(empty {});
}
auto TextureContext::BeginProducerWrite(ProducerWriteDesc desc)
    -> Result<ProducerWrite, tex::Error> {
    auto polled = Poll();
    if (polled.is_err()) return Err(polled.unwrap_err());
    auto checked = tex::validate_access({ desc.view, desc.final.access, desc.final.stages });
    if (checked.is_err()) return Err(checked.unwrap_err());
    auto  range       = checked.unwrap();
    auto& texture     = desc.view.texture();
    auto& description = texture.description();
    if (! desc.owner || ! desc.view.permits(true) || range.mip_count != 1 ||
        range.layer_count != 1 || description.samples != 1 ||
        ! (tex::Count(description.usage) &
           tex::Count(tex::Usage::TransferDestination | tex::Usage::ColorAttachment |
                      tex::Usage::DepthAttachment | tex::Usage::Storage)))
        return Err(tex::Error { tex::ErrorKind::Invalid });
    if (texture.identity().device != session().epoch() ||
        texture.identity().epoch != session().epoch())
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    auto image = native_texture(texture);
    auto view  = native_view(desc.view);
    if (image.is_err()) return Err(image.unwrap_err());
    if (view.is_err()) return Err(view.unwrap_err());
    auto& storage = *tex::BackendAccess::Storage(texture);
    if (storage.recording || storage.prepared_reads)
        return Err(tex::Error { tex::ErrorKind::Busy });
    auto backend = rstd::any::downcast_ref<NativeTexture>(storage.backing.deref()).unwrap();
    auto initial =
        backend->tracking
            ->states[usize(range.first_layer * description.mip_levels + range.first_mip)];
    auto expected = native_access(desc.final, range.aspect);
    auto final    = desc.native_final.is_some() ? *desc.native_final : expected;
    if (final.layout != expected.layout || ! final.stages)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    if (final.queue_family == VK_QUEUE_FAMILY_IGNORED)
        final.queue_family = session().device().graphics_queue().family_index;
    auto dependency       = storage.last_use.valid() ? storage.last_use : storage.last_write;
    auto valid_dependency = dependency_wait(session(), dependency, tex::Stages::All);
    if (valid_dependency.is_err()) return Err(valid_dependency.unwrap_err());
    auto            extent = tex::mip_extent(description, range.first_mip);
    ImageParameters binding;
    binding.handle     = image.unwrap();
    binding.view       = view.unwrap();
    binding.extent     = { extent.width, extent.height, 1 };
    binding.generation = u64(texture.identity().resource);
    auto identity      = next_texture_recording();
    auto reservation   = Arc<ProducerReservation>::make(
        session(),
        rstd::move(desc),
        ProducerTarget { binding, initial, final, rstd::move(dependency) },
        identity);
    storage.recording = identity;
    return Ok(ProducerWrite(rstd::move(reservation)));
}
auto TextureContext::CommitProducerWrite(ProducerWrite& write, tex::SubmissionToken completion)
    -> Result<empty, tex::Error> {
    if (! write.active() || write.reservation_->uncertain)
        return Err(tex::Error { tex::ErrorKind::Invalid });
    auto& reservation = *write.reservation_;
    if (reservation.session.epoch() != session().epoch()) {
        write.uncertain();
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    }
    auto checked = dependency_wait(session(), completion, tex::Stages::All);
    if (! completion.valid() || checked.is_err()) {
        write.uncertain();
        return Err(tex::Error { tex::ErrorKind::DeviceMismatch });
    }
    auto& storage = *tex::BackendAccess::Storage(reservation.view.texture());
    if (storage.recording != reservation.identity) {
        write.uncertain();
        return Err(tex::Error { tex::ErrorKind::Stale });
    }
    auto range = reservation.view.description().range;
    auto index = usize(range.first_layer * storage.desc.mip_levels + range.first_mip);
    producers_.push(ProducerPending { write.reservation_.clone(), completion });
    storage.states[index] = reservation.final;
    rstd::any::downcast_ref<NativeTexture>(storage.backing.deref())
        .unwrap()
        ->tracking->states[index] = reservation.target.final;
    storage.last_write            = completion;
    ++storage.content_revision;
    storage.last_use = completion;
    reservation.release();
    return Ok(empty {});
}
auto TextureContext::Begin() -> Result<tex::Submission, tex::Error> {
    auto completed = Poll();
    if (completed.is_err()) return Err(completed.unwrap_err());
    auto recorder = Box<TextureRecording>::make(signal_.clone(), next_texture_recording());
    if (! recorder->uploads.init()) return Err(tex::Error { tex::ErrorKind::Backend });
    VkCommandBufferAllocateInfo info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    info.commandPool        = *signal_->pool;
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
    for (auto& surface : recording.surfaces)
        if (! surface.released) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto flushed = recording.FlushUploads();
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
    Vec<NativeSemaphoreKind>  kinds;
    for (auto& dependency : recording.dependencies) {
        auto wait = dependency_wait(session(), dependency.token, dependency.stages);
        if (wait.is_err()) return Err(wait.unwrap_err());
        auto  native = wait.unwrap();
        usize index {};
        while (index < waits.len() && waits[index] != native.semaphore) ++index;
        if (index == waits.len()) {
            waits.push(VkSemaphore(native.semaphore));
            values.push(tex::Serial(native.value));
            stages.push(VkPipelineStageFlags(native.stages));
            kinds.push(NativeSemaphoreKind::Timeline);
        } else {
            values[index] = rstd::cmp::max(values[index], native.value);
            stages[index] |= native.stages;
        }
    }
    for (auto& native : recording.native_waits) {
        usize index {};
        while (index < waits.len() && waits[index] != native.semaphore) ++index;
        if (index == waits.len()) {
            waits.push(VkSemaphore(native.semaphore));
            values.push(tex::Serial(native.value));
            stages.push(VkPipelineStageFlags(native.stages));
            kinds.push(NativeSemaphoreKind(native.kind));
        } else {
            if (kinds[index] != native.kind) return Err(tex::Error { tex::ErrorKind::Invalid });
            values[index] = rstd::cmp::max(values[index], native.value);
            stages[index] |= native.stages;
        }
    }
    if (next_submission_ == ~tex::Serial {}) return Err(tex::Error { tex::ErrorKind::Invalid });
    auto             serial = next_submission_ + 1;
    Vec<VkSemaphore> signals;
    Vec<tex::Serial> signal_values;
    signals.push(VkSemaphore(*signal_->semaphore));
    signal_values.push(tex::Serial(serial));
    for (auto& signal : recording.native_signals) {
        for (usize i {}; i < waits.len(); ++i)
            if (waits[i] == signal.semaphore &&
                (kinds[i] != signal.kind ||
                 (signal.kind == NativeSemaphoreKind::Timeline && values[i] >= signal.value)))
                return Err(tex::Error { tex::ErrorKind::Invalid });
        signals.push(VkSemaphore(signal.semaphore));
        signal_values.push(tex::Serial(signal.value));
    }
    VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline.waitSemaphoreValueCount   = unsigned(values.len().to_primitive());
    timeline.pWaitSemaphoreValues      = values.data();
    timeline.signalSemaphoreValueCount = unsigned(signal_values.len().to_primitive());
    timeline.pSignalSemaphoreValues    = signal_values.data();
    VkSubmitInfo info { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    info.pNext                = &timeline;
    info.waitSemaphoreCount   = timeline.waitSemaphoreValueCount;
    info.pWaitSemaphores      = waits.data();
    info.pWaitDstStageMask    = stages.data();
    info.signalSemaphoreCount = timeline.signalSemaphoreValueCount;
    info.pSignalSemaphores    = signals.data();
    info.commandBufferCount   = 1;
    info.pCommandBuffers      = &recording.command;
    auto completion_fence     = Arc<SubmissionFence>::make(session());
    if (recording.fence.is_some()) {
        completion_fence->borrowed = recording.fence->fence;
        completion_fence->owner    = Some(recording.fence->owner.clone());
    } else {
        VkFenceCreateInfo fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        auto              created = session().observe_result(
            session().device().handle().CreateFence(fence_info, completion_fence->owned));
        if (created != VK_SUCCESS) return Err(texture_error(created));
    }
    recording.completion_fence = Some(completion_fence.clone());
    tex::SubmissionToken token(Arc<dyn<tex::Dependency>>::make(
        TextureCompletion { signal_.clone(), serial, completion_fence.clone() }));
    pending_.reserve(pending_.len() + usize(1));
    pending_values_.reserve(pending_values_.len() + usize(1));
    const auto& queue = signal_->queue.handle;
    result            = session().observe_result(
        submitter_ ? (*submitter_)->Submit(queue, info, completion_fence->handle())
                   : queue.Submit(info, completion_fence->handle()));
    if (result != VK_SUCCESS) {
        failed_             = true;
        recording.uncertain = true;
        tex::BackendAccess::Uncertain(work);
        for (auto& participant : recording.participants) participant->Uncertain();
        for (auto& entry : recording.textures)
            rstd::any::downcast_ref<NativeTexture>(
                tex::BackendAccess::Storage(entry.texture)->backing.deref())
                .unwrap()
                ->tracking->uncertain = true;
        pending_.push(rstd::move(work));
        pending_values_.push(tex::Serial(serial));
        return Err(texture_error(result));
    }
    next_submission_    = serial;
    recording.submitted = true;
    tex::BackendAccess::Submitted(work);
    for (auto& participant : recording.participants) participant->Commit(token);
    for (auto& reservation : recording.prepared_reads) reservation->release();
    for (auto& entry : recording.textures) {
        auto& state  = *tex::BackendAccess::Storage(entry.texture);
        state.states = rstd::move(entry.states);
        rstd::any::downcast_ref<NativeTexture>(state.backing.deref()).unwrap()->tracking->states =
            rstd::move(entry.native_states);
        state.recording = 0;
        if (entry.written) {
            state.last_write = token;
            ++state.content_revision;
        }
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
    Vec<bool>            ready_work;
    for (usize i {}; i < pending_.len(); ++i) {
        bool completed = false;
        if (pending_values_[i] <= current.unwrap()) {
            auto recording =
                rstd::any::downcast_mut<TextureRecording>(pending_[i].backend()).unwrap();
            auto ready = (*recording->completion_fence)->Poll();
            if (ready.is_err()) return Err(ready.unwrap_err());
            completed = ready.unwrap();
        }
        ready_work.push(bool(completed));
    }
    Vec<bool> ready_producers;
    for (auto& producer : producers_) {
        auto ready = producer.completion.poll();
        if (ready.is_err()) return Err(ready.unwrap_err());
        ready_producers.push(bool(ready.unwrap()));
    }
    for (usize i {}; i < pending_.len(); ++i) {
        if (! ready_work[i]) {
            keep.push(rstd::move(pending_[i]));
            values.push(tex::Serial(pending_values_[i]));
        }
    }
    pending_        = rstd::move(keep);
    pending_values_ = rstd::move(values);
    Vec<ProducerPending> producers;
    for (usize i {}; i < producers_.len(); ++i)
        if (! ready_producers[i]) producers.push(rstd::move(producers_[i]));
    producers_ = rstd::move(producers);
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
        auto own = rstd::any::downcast_ref<TextureCompletion>(token.backend());
        if (own.is_some()) {
            auto retired = (**own).fence->Wait(timeout);
            if (retired.is_err()) return retired;
        }
    }
    return Poll();
}
} // namespace vrento::vulkan
