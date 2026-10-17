module;

#include <unistd.h>

// Macros only — VVK_CHECK family.
#include "vvk/macros.hpp"

export module vrento:vulkan.api;
import rstd;
import rstd.log;
import :types.texture;
export import :texture;
export import :texture.image;
import :texture.video_playback;

// Export Vulkan FFI symbols for backend consumers.
export import vvk;
export import :vulkan.error;
export import :vulkan.surface_error;

export import :vulkan.shader_compile;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeMap;
using rstd::collections::BTreeSet;
using rstd::ffi::CString;
using rstd::sync::Arc;
using rstd::sync::Weak;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

export namespace vrento
{

// ---------- ExSwapchain (formerly Swapchain/ExSwapchain.hpp) ----------

namespace vulkan
{

inline VkCompareOp ToVkType(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

struct ImageParameters {
    VkImage        handle {};
    VkImageView    view {};
    VkSampler      sampler {};
    VkExtent3D     extent {};
    rstd::uint32_t mipmap_level { 1 };
    u64            generation { 0 };

    ImageParameters()  = default;
    ~ImageParameters() = default;
};

} // namespace vulkan

// Snapshot of the negotiated buffer set the producer should render into.
// Fired by ExSwapchain implementations whenever readiness flips or the
// negotiated extent/format changes.
struct ExSwapchainReadyEvent {
    bool     ready;
    unsigned width;
    unsigned height;
    VkFormat format;
};

enum class FrameSurfaceAcquireKind
{
    QueueOrdered,
    BinarySemaphore,
    ExternalProtocol,
};

struct FrameSurfaceIdentity {
    u64 owner_generation { 0 };
    u32 image_index { 0 };
    u64 acquire_serial { 0 };

    bool valid() const noexcept { return owner_generation != u64() && acquire_serial != u64(); }
    bool operator==(const FrameSurfaceIdentity&) const = default;
};

enum class FrameSurfaceReuseKind
{
    QueueOrdered,
    PresentationAcquired,
    NeverSubmitted,
    ConsumerReleased,
};

struct FrameSurfaceReuseProof {
    FrameSurfaceReuseKind kind { FrameSurfaceReuseKind::QueueOrdered };
    u64                   release_point { 0 };

    bool valid() const noexcept {
        return (kind == FrameSurfaceReuseKind::ConsumerReleased) == (release_point != u64());
    }
};

struct FrameSurfaceAcquireDependency {
    FrameSurfaceAcquireKind kind { FrameSurfaceAcquireKind::QueueOrdered };
    VkSemaphore             semaphore { VK_NULL_HANDLE };

    bool valid() const noexcept {
        return (kind == FrameSurfaceAcquireKind::BinarySemaphore) == (semaphore != VK_NULL_HANDLE);
    }
};

struct FrameSurfaceLease {
    FrameSurfaceIdentity          identity;
    FrameSurfaceReuseProof        reuse;
    vulkan::ImageParameters       image;
    VkFormat                      format { VK_FORMAT_UNDEFINED };
    VkImageLayout                 initial_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    rstd::uint32_t                initial_queue_family { VK_QUEUE_FAMILY_IGNORED };
    FrameSurfaceAcquireDependency acquire;
    VkImageLayout                 final_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    rstd::uint32_t                final_queue_family { VK_QUEUE_FAMILY_IGNORED };
    bool                          discard_content { true };

    bool valid() const noexcept {
        const bool acquire_matches_reuse =
            (acquire.kind == FrameSurfaceAcquireKind::QueueOrdered &&
             reuse.kind == FrameSurfaceReuseKind::QueueOrdered) ||
            (acquire.kind == FrameSurfaceAcquireKind::BinarySemaphore &&
             reuse.kind == FrameSurfaceReuseKind::PresentationAcquired) ||
            (acquire.kind == FrameSurfaceAcquireKind::ExternalProtocol &&
             (reuse.kind == FrameSurfaceReuseKind::NeverSubmitted ||
              reuse.kind == FrameSurfaceReuseKind::ConsumerReleased));
        return identity.valid() && reuse.valid() && acquire_matches_reuse &&
               image.handle != VK_NULL_HANDLE && image.extent.width != 0 &&
               image.extent.height != 0 && image.extent.depth != 0 &&
               format != VK_FORMAT_UNDEFINED && acquire.valid() && discard_content &&
               final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
               initial_queue_family != VK_QUEUE_FAMILY_IGNORED &&
               final_queue_family != VK_QUEUE_FAMILY_IGNORED;
    }
};

enum class FrameSurfaceCompletionStatus
{
    Submitted,
    Aborted,
    NotPending,
    StaleIdentity,
    SessionLost,
    ProtocolError,
};

struct FrameSurfaceCompletionResult {
    FrameSurfaceCompletionStatus status { FrameSurfaceCompletionStatus::NotPending };
    FrameSurfaceIdentity         identity;
    Option<FrameSurfaceError>    error { DefaultError(status) };

    static auto DefaultError(FrameSurfaceCompletionStatus status) -> Option<FrameSurfaceError> {
        switch (status) {
        case FrameSurfaceCompletionStatus::Submitted:
        case FrameSurfaceCompletionStatus::Aborted: return None();
        case FrameSurfaceCompletionStatus::NotPending:
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::NotPending });
        case FrameSurfaceCompletionStatus::StaleIdentity:
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::StaleIdentity });
        case FrameSurfaceCompletionStatus::SessionLost:
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::SessionLost });
        case FrameSurfaceCompletionStatus::ProtocolError:
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::ProtocolFailure });
        }
        return Some(FrameSurfaceError { FrameSurfaceErrorKind::ProtocolFailure });
    }

    bool completed() const noexcept {
        return ! error && (status == FrameSurfaceCompletionStatus::Submitted ||
                           status == FrameSurfaceCompletionStatus::Aborted);
    }
};

class ExSwapchain;

struct ExSwapchainObject {
    using Trait                  = ExSwapchainObject;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = ExSwapchainObject;
        auto AsSwapchain() -> ExSwapchain& { return rstd::trait_call<0>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::AsSwapchain>;
};

using ExSwapchainOwner = Arc<dyn<ExSwapchainObject>>;

class FrameSurfaceCompletionCapability {
public:
    FrameSurfaceCompletionCapability() = default;
    ~FrameSurfaceCompletionCapability();

    FrameSurfaceCompletionCapability(const FrameSurfaceCompletionCapability&)            = delete;
    FrameSurfaceCompletionCapability& operator=(const FrameSurfaceCompletionCapability&) = delete;
    FrameSurfaceCompletionCapability(FrameSurfaceCompletionCapability&&) noexcept;
    FrameSurfaceCompletionCapability& operator=(FrameSurfaceCompletionCapability&&) noexcept;

    bool valid() const noexcept { return m_owner.is_some() && m_identity.valid(); }

    FrameSurfaceCompletionResult Submit(int producer_sync_fd);
    FrameSurfaceCompletionResult Abort();

private:
    friend class ExSwapchain;

    FrameSurfaceCompletionCapability(ExSwapchainOwner owner, FrameSurfaceIdentity identity)
        : m_owner(Some(rstd::move(owner))), m_identity(identity) {}

    Option<ExSwapchainOwner> m_owner;
    FrameSurfaceIdentity     m_identity;
};

enum class FrameSurfaceAcquireStatus
{
    Acquired,
    Suboptimal,
    Busy,
    NotReady,
    SessionLost,
    ProtocolError,
};

struct FrameSurfaceAcquireResult {
    FrameSurfaceAcquireStatus        status { FrameSurfaceAcquireStatus::NotReady };
    FrameSurfaceLease                lease;
    FrameSurfaceCompletionCapability completion;
    Option<FrameSurfaceError>        error { DefaultError(status) };

    static auto DefaultError(FrameSurfaceAcquireStatus status) -> Option<FrameSurfaceError> {
        if (status == FrameSurfaceAcquireStatus::SessionLost)
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::SessionLost });
        if (status == FrameSurfaceAcquireStatus::ProtocolError)
            return Some(FrameSurfaceError { FrameSurfaceErrorKind::ProtocolFailure });
        return None();
    }

    bool acquired() const noexcept {
        return (status == FrameSurfaceAcquireStatus::Acquired ||
                status == FrameSurfaceAcquireStatus::Suboptimal) &&
               lease.valid() && completion.valid() && ! error;
    }
};

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

// Per-slot DMA-BUF descriptor surfaced to the IPC layer. Local backend
// fills these from its self-allocated `ExImageParameters`; bridge backend
// keeps them empty (the bridge already published the metadata via
// `bind_buffers` itself).
struct ExHandle {
    int   fd { -1 };
    i32   width { 0 };
    i32   height { 0 };
    usize size { 0 };

    rstd::uint32_t drm_fourcc { 0 };
    rstd::uint64_t drm_modifier { 0 };
    VkDeviceSize   plane0_offset { 0 };
    rstd::uint32_t plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    i32 id() const { return m_id; }

private:
    i32 m_id { 0 };
};

template<typename T>
class TripleSwapchain {
public:
    virtual ~TripleSwapchain() = default;

    TripleSwapchain(const TripleSwapchain&)            = delete;
    TripleSwapchain& operator=(const TripleSwapchain&) = delete;
    TripleSwapchain(TripleSwapchain&&)                 = delete;
    TripleSwapchain& operator=(TripleSwapchain&&)      = delete;

    T* eatFrame() {
        if (! dirty().exchange(false)) return nullptr;
        presented().store(ready().exchange(presented().load()));
        return presented().load();
    }
    void renderFrame() {
        inprogress().store(ready().exchange(inprogress().load()));
        dirty().exchange(true);
    }
    T* getInprogress() { return inprogress().load(); }

    rstd::array<T*, 3> snapshot_all_slots() {
        return rstd::array<T*, 3> { presented().load(), ready().load(), inprogress().load() };
    }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;

protected:
    TripleSwapchain() = default;

    virtual Atomic<T*>& presented()  = 0;
    virtual Atomic<T*>& ready()      = 0;
    virtual Atomic<T*>& inprogress() = 0;

private:
    Atomic<bool>& dirty() { return m_dirty; };
    Atomic<bool>  m_dirty { false };
};

// Producer-side abstraction over the offscreen swapchain. Two
// implementations exist:
//   - LocalExSwapchain: owns 3 DMA-BUF-backed VkImages; standalone viewers
//     and in-process hosts
//     drive bind_buffers / frame_ready themselves and consume `eatFrame()`
//     / `snapshot_all_slots()`.
//   - BridgeExSwapchain: wraps a `ww_pool_t`; bridge owns the slot images
//     and completes submission through the acquired frame capability.
using ExSwapchainReadyCallback = Arc<dyn<Fn<void(const ExSwapchainReadyEvent&)>>>;

class ExSwapchain {
public:
    virtual ~ExSwapchain() = default;

    template<typename T, typename... Args>
    static auto Make(Args&&... args) -> ExSwapchainOwner;

    ExSwapchain(const ExSwapchain&)            = delete;
    ExSwapchain& operator=(const ExSwapchain&) = delete;
    ExSwapchain(ExSwapchain&&)                 = delete;
    ExSwapchain& operator=(ExSwapchain&&)      = delete;

    virtual void poll() {}

    virtual FrameSurfaceAcquireResult acquireRenderTarget() = 0;

    virtual int takeLastFrameSyncFd() { return -1; }

    virtual ExHandle*                 eatFrame() { return nullptr; }
    virtual rstd::array<ExHandle*, 3> snapshot_all_slots() {
        return rstd::array<ExHandle*, 3> { nullptr, nullptr, nullptr };
    }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;
    virtual VkFormat format() const = 0;

    virtual bool ready() const = 0;

    virtual void setOnReadyChanged(Option<ExSwapchainReadyCallback>) = 0;

protected:
    ExSwapchain() = default;

    FrameSurfaceCompletionCapability MakeCompletionCapability(FrameSurfaceIdentity identity) {
        auto owner = m_owner.upgrade();
        if (! owner) return {};
        return FrameSurfaceCompletionCapability(rstd::move(owner), identity);
    }

private:
    friend class FrameSurfaceCompletionCapability;
    Weak<dyn<ExSwapchainObject>> m_owner { Weak<dyn<ExSwapchainObject>>::make() };

    virtual FrameSurfaceCompletionResult CompleteRendered(FrameSurfaceIdentity identity,
                                                          int producer_sync_fd)           = 0;
    virtual FrameSurfaceCompletionResult AbortRenderTarget(FrameSurfaceIdentity identity) = 0;
};

inline FrameSurfaceCompletionCapability::~FrameSurfaceCompletionCapability() {
    if (valid()) (void)Abort();
}

inline FrameSurfaceCompletionCapability::FrameSurfaceCompletionCapability(
    FrameSurfaceCompletionCapability&& other) noexcept
    : m_owner(rstd::move(other.m_owner)), m_identity(other.m_identity) {
    other.m_identity = {};
}

inline FrameSurfaceCompletionCapability&
FrameSurfaceCompletionCapability::operator=(FrameSurfaceCompletionCapability&& other) noexcept {
    if (this == &other) return *this;
    if (valid()) (void)Abort();
    m_owner          = rstd::move(other.m_owner);
    m_identity       = other.m_identity;
    other.m_identity = {};
    return *this;
}

inline FrameSurfaceCompletionResult FrameSurfaceCompletionCapability::Submit(int producer_sync_fd) {
    if (! valid()) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return {};
    }
    auto owner    = m_owner.take().unwrap();
    auto identity = m_identity;
    m_identity    = {};
    return owner->AsSwapchain().CompleteRendered(identity, producer_sync_fd);
}

inline FrameSurfaceCompletionResult FrameSurfaceCompletionCapability::Abort() {
    if (! valid()) return {};
    auto owner    = m_owner.take().unwrap();
    auto identity = m_identity;
    m_identity    = {};
    return owner->AsSwapchain().AbortRenderTarget(identity);
}

namespace vulkan
{

// ---------- Instance.hpp ----------

struct Extension {
    bool     required { false };
    ref<str> name;
};

using InstanceLayer = Extension;

using CheckGpuOp = dyn<FnMut<bool(const vvk::PhysicalDevice&)>>;

constexpr auto VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation"_str;

constexpr rstd::uint32_t WP_VULKAN_VERSION { VK_API_VERSION_1_1 };
constexpr const char*    WP_APPLICATION_NAME { "scene render" };

class Device;
class Instance {
public:
    Instance()  = default;
    ~Instance() = default;

    void Destroy();

    static auto TryCreate(Instance&, slice<Extension>, slice<InstanceLayer>,
                          rstd::uint32_t api_version = WP_VULKAN_VERSION)
        -> Result<empty, BackendError>;
    auto TryChoosePhysicalDevice(mut_ref<CheckGpuOp> checkgpu, slice<rstd::uint8_t> uuid = {})
        -> Result<rstd::empty, BackendError>;

    const vvk::Instance&       inst() const;
    const vvk::PhysicalDevice& gpu() const;
    const vvk::SurfaceKHR&     surface() const;
    rstd::uint32_t             api_version() const { return m_api_version; }
    slice<CString> enabled_extensions() const { return m_enabled_extensions.as_slice(); }

    bool offscreen() const;
    void setSurface(VkSurfaceKHR);
    bool supportExt(ref<str>) const;
    bool supportLayer(ref<str>) const;

private:
    Option<vvk::VulkanLoader> m_loader;
    vvk::InstanceDispatch     m_dld;
    vvk::Instance             m_vinst;

    vvk::DebugUtilsMessenger m_debug_utils;
    vvk::PhysicalDevice      m_gpu {};
    rstd::uint32_t           m_api_version { WP_VULKAN_VERSION };

    vvk::SurfaceKHR  m_surface {};
    BTreeSet<String> m_extensions { BTreeSet<String>::make() };
    Vec<CString>     m_enabled_extensions;
    BTreeSet<String> m_layers { BTreeSet<String>::make() };
};

// ShaderSpv / Uni_ShaderSpv now live in vrento.shader_compile (re-exported above).

// ---------- Parameters.hpp ----------

struct QueueParameters {
    vvk::Queue     handle;
    rstd::uint32_t family_index;
    VkQueueFlags   flags {};
    rstd::uint32_t queue_index {};
};

struct AllocatedBufferParameters {
    vvk::AllocatedBuffer handle;
    VkDeviceSize         req_size;

    AllocatedBufferParameters();
    ~AllocatedBufferParameters();
    AllocatedBufferParameters(AllocatedBufferParameters&& o) noexcept;
    AllocatedBufferParameters& operator=(AllocatedBufferParameters&& o) noexcept;
};

struct BufferParameters {
    VkBuffer     handle;
    VkDeviceSize req_size;
    BufferParameters()  = default;
    ~BufferParameters() = default;
    BufferParameters(const AllocatedBufferParameters& o) noexcept
        : handle(o.handle.handle()), req_size(o.req_size) {}
};

struct ExImageParameters {
    ExImageParameters(const ExImageParameters&)              = delete;
    ExImageParameters&   operator=(const ExImageParameters&) = delete;
    vvk::DeviceMemory    mem {};
    VkMemoryRequirements mem_reqs {};

    vvk::Image     handle;
    vvk::ImageView view;
    vvk::Sampler   sampler;
    VkExtent3D     extent;
    unsigned       mipmap_level { 1 };
    u64            generation { 0 };
    int            fd { 0 };

    rstd::uint32_t drm_fourcc { 0 };
    rstd::uint64_t drm_modifier { 0 };
    VkDeviceSize   plane0_offset { 0 };
    rstd::uint32_t plane0_stride { 0 };

    ExImageParameters();
    ~ExImageParameters();
    ExImageParameters(ExImageParameters&& o) noexcept;
    ExImageParameters& operator=(ExImageParameters&& o) noexcept;
};

// `ImageParameters` itself is global-attached (defined in classic
// Swapchain/ExSwapchain.hpp). These free helpers replace the conversion
// ctors that used to live on it — those ctors needed module-attached
// Allocated/Ex types which can't be visible in classic purview.
inline ImageParameters ToImageParameters(const ExImageParameters& o) noexcept {
    ImageParameters out;
    out.handle       = *o.handle;
    out.view         = *o.view;
    out.sampler      = *o.sampler;
    out.extent       = o.extent;
    out.mipmap_level = o.mipmap_level;
    out.generation   = o.generation;
    return out;
}

struct ImageSlotsRef {
    Vec<ImageParameters> slots;

    rstd::ptrdiff_t active { 0 };
    auto            active_slot() const -> usize {
        return active < 0 || active >= static_cast<rstd::ptrdiff_t>(slots.len().to_primitive())
                   ? usize()
                   : usize(active);
    }
    auto& getActive() const { return slots[active_slot()]; }
    ImageSlotsRef();
    ~ImageSlotsRef();
    ImageSlotsRef(const ImageSlotsRef& other): slots(other.slots.clone()), active(other.active) {}
    ImageSlotsRef& operator=(const ImageSlotsRef& other) {
        if (this != &other) {
            slots  = other.slots.clone();
            active = other.active;
        }
        return *this;
    }
    ImageSlotsRef(ImageSlotsRef&&) noexcept            = default;
    ImageSlotsRef& operator=(ImageSlotsRef&&) noexcept = default;
};

// ---------- Swapchain.hpp ----------

enum class PresentationMode { Fifo, Immediate, Mailbox, FifoRelaxed };
struct SwapchainOptions {
    bool              require_srgb { false };
    VkImageUsageFlags usage { VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    PresentationMode presentation { PresentationMode::Fifo };
};

class Swapchain {
public:
    static bool Create(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&, SwapchainOptions = {});
    static auto CreateResult(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&, SwapchainOptions = {})
        -> VkResult;
    const vvk::SwapchainKHR& handle() const;
    VkFormat                 format() const;
    VkExtent2D               extent() const;
    VkPresentModeKHR         presentMode() const;
    slice<ImageParameters>   images() const;
    void                     reset();

private:
    vvk::SwapchainKHR    m_handle;
    VkSurfaceFormatKHR   m_format {};
    VkExtent2D           m_extent {};
    VkPresentModeKHR     m_present_mode {};
    Vec<ImageParameters> m_images;
    Vec<vvk::ImageView>  m_imageviews;
};

// ---------- Texture formats and upload staging ----------

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

struct ImageUploadTicket {
    u64 value { 0 };

    bool        Valid() const noexcept { return value != u64(); }
    friend bool operator==(const ImageUploadTicket&, const ImageUploadTicket&) = default;
};

class ImageUploadManager;

class RecordedImageUploads {
public:
    RecordedImageUploads(const RecordedImageUploads&)            = delete;
    RecordedImageUploads& operator=(const RecordedImageUploads&) = delete;
    RecordedImageUploads();
    ~RecordedImageUploads();

    RecordedImageUploads(RecordedImageUploads&&) noexcept;
    RecordedImageUploads& operator=(RecordedImageUploads&&) noexcept;

    bool Valid() const noexcept;

private:
    struct State;
    friend class ImageUploadManager;
    friend class ImageUploadBatchLease;

    RecordedImageUploads(ImageUploadManager* owner, Arc<State> state);
    void Reset();

    ImageUploadManager* m_owner { nullptr };
    Option<Arc<State>>  m_state;
};

class ImageUploadBatchLease {
public:
    ImageUploadBatchLease(const ImageUploadBatchLease&)            = delete;
    ImageUploadBatchLease& operator=(const ImageUploadBatchLease&) = delete;
    ImageUploadBatchLease();
    ~ImageUploadBatchLease();

    ImageUploadBatchLease(ImageUploadBatchLease&&) noexcept;
    ImageUploadBatchLease& operator=(ImageUploadBatchLease&&) noexcept;

    bool                     Valid() const noexcept;
    slice<ImageUploadTicket> Tickets() const noexcept;

private:
    friend class ImageUploadManager;
    explicit ImageUploadBatchLease(Arc<RecordedImageUploads::State> state);

    Option<Arc<RecordedImageUploads::State>> m_state;
};

struct ImageUploadRows {
    // Counts format-block rows, not texel rows for compressed formats.
    VkDeviceSize row_bytes {}, rows {}, source_pitch {};
};

struct ImageUploadStatistics {
    VkDeviceSize live_bytes {}, capacity_bytes {}, free_capacity_bytes {};
};

class ImageUploadManager {
public:
    ImageUploadManager(const ImageUploadManager&)            = delete;
    ImageUploadManager& operator=(const ImageUploadManager&) = delete;
    ImageUploadManager(ImageUploadManager&&)                 = delete;
    ImageUploadManager& operator=(ImageUploadManager&&)      = delete;
    explicit ImageUploadManager(const Device&);
    ~ImageUploadManager();

    bool init();
    void destroy();

    // Copies into owned staging before returning. Source rows require a tightly packed,
    // single-layer 2D region; a zero source pitch means row_bytes.
    auto QueueRegion(VkImage image, VkBufferImageCopy region, slice<rstd::uint8_t> bytes,
                     rstd::uint32_t block_bytes = 4, Option<ImageUploadRows> source_rows = None())
        -> Result<ImageUploadTicket, BackendError>;
    // Live bytes exclude alignment gaps; free capacity belongs to unleased cached blocks.
    auto statistics() const -> ImageUploadStatistics;
    bool HasPendingUploads() const noexcept;
    auto RecordQueuedRegions(vvk::CommandBuffer& command, RecordedImageUploads& recorded)
        -> Result<rstd::empty, BackendError>;
    auto CommitRecordedUploads(RecordedImageUploads&& recorded) -> Option<ImageUploadBatchLease>;
    void CancelRecordedUploads(const Arc<RecordedImageUploads::State>& state);
    void DiscardPendingUploads();
    void Trim();

private:
    struct Impl;
    Box<Impl> m_impl;
};

// ---------- Device.hpp ----------

struct DeviceCapabilities {
    bool           timeline_semaphore { false };
    bool           geometry_shader { false };
    bool           synchronization2 { false };
    bool           push_descriptor { false };
    rstd::uint32_t max_push_descriptors { 0 };
    bool           multi_viewport { false };
    bool           shader_output_viewport_index { false };
    bool           sampled_depth_d32 { false };
    bool           depth_clamp { false };
    bool           depth_clip_enable { false };
    rstd::uint32_t max_geometry_output_vertices { 256 };
    rstd::uint32_t max_geometry_total_output_components { 1024 };
    bool           memory_budget { false };
    bool           external_memory_fd { false };
    bool           external_memory_dma_buf { false };
    bool           drm_format_modifier { false };
    bool           foreign_queue { false };
    rstd::uint32_t graphics_queue_family { VK_QUEUE_FAMILY_IGNORED };
    rstd::uint32_t present_queue_family { VK_QUEUE_FAMILY_IGNORED };

    bool shadow_multi_viewport() const noexcept {
        return multi_viewport && shader_output_viewport_index;
    }

    bool directional_shadow() const noexcept {
        return shadow_multi_viewport() && sampled_depth_d32;
    }
};

struct MemoryBudgetSnapshot {
    VkDeviceSize usage { 0 };
    VkDeviceSize budget { 0 };

    VkDeviceSize available() const noexcept { return budget > usage ? budget - usage : 0; }
};

struct MemoryBudgetSource {
    using Trait                  = MemoryBudgetSource;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = MemoryBudgetSource;

        auto MemoryBudget() const -> MemoryBudgetSnapshot { return rstd::trait_call<0>(this); }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::MemoryBudget>;
};

struct PipelineParameters;

class Device {
public:
    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&)                 = delete;
    Device& operator=(Device&&)      = delete;
    Device();
    ~Device();

    static auto TryCreate(Instance&, slice<Extension>, VkExtent2D, Device&, SwapchainOptions = {})
        -> Result<empty, BackendError>;
    static auto ValidateGPU(vvk::PhysicalDevice, slice<Extension>, VkSurfaceKHR)
        -> Result<empty, BackendError>;
    static bool CheckGPU(vvk::PhysicalDevice gpu, slice<Extension> exts, VkSurfaceKHR surface);

    void Destroy();

    const auto& graphics_queue() const { return m_graphics_queue; }
    auto        queues() const -> slice<QueueParameters> { return m_queues.as_slice(); }
    const auto& present_queue() const { return m_present_queue; }
    const auto& device() const { return m_device; }
    const auto& handle() const { return m_device; }
    auto        lost() const -> bool { return __atomic_load_n(&m_lost, __ATOMIC_ACQUIRE); }
    auto        observe_result(VkResult result) const -> VkResult {
        if (result == VK_ERROR_DEVICE_LOST) __atomic_store_n(&m_lost, true, __ATOMIC_RELEASE);
        return result;
    }
    const auto&                  gpu() const { return m_gpu; }
    const vvk::InstanceDispatch& instance_dispatch() const { return *m_instance_dispatch; }
    VkInstance                   instance_handle() const { return m_instance; }
    rstd::uint32_t               instance_api_version() const { return m_instance_api_version; }
    slice<CString>               enabled_instance_extensions() const {
        return m_enabled_instance_extensions.as_slice();
    }
    slice<CString> enabled_device_extensions() const {
        return m_enabled_device_extensions.as_slice();
    }
    const auto& limits() const { return m_limits; }
    const auto& memory_allocator() const { return m_allocator; }
    const auto& cmd_pool() const { return m_command_pool; }
    const auto& swapchain() const { return m_swapchain; }
    const auto& out_extent() const { return m_extent; }
    const auto& capabilities() const { return m_capabilities; }
    void        set_out_extent(VkExtent2D v) { m_extent = v; }
    // The caller must retire graphics and presentation work before replacement.
    bool recreateSwapchain(VkSurfaceKHR, VkExtent2D, SwapchainOptions = {});
    auto recreateSwapchainResult(VkSurfaceKHR, VkExtent2D, SwapchainOptions = {}) -> VkResult;
    void releaseSwapchain();

    bool supportExt(ref<str>) const;

    VkDeviceSize GetUsage() const;
    auto         MemoryBudget() const -> MemoryBudgetSnapshot;

private:
    auto ChooseDeviceQueue(VkSurfaceKHR = {}) -> Result<Vec<VkDeviceQueueCreateInfo>, BackendError>;

    const vvk::InstanceDispatch* m_instance_dispatch {};
    vvk::DeviceDispatch          dld;
    VkInstance                   m_instance { VK_NULL_HANDLE };
    rstd::uint32_t               m_instance_api_version { WP_VULKAN_VERSION };
    vvk::Device                  m_device;
    mutable bool                 m_lost {};
    vvk::PhysicalDevice          m_gpu;
    vvk::MemoryAllocator         m_allocator;

    VkPhysicalDeviceLimits m_limits;
    DeviceCapabilities     m_capabilities;
    BTreeSet<String>       m_extensions { BTreeSet<String>::make() };
    Vec<CString>           m_enabled_instance_extensions;
    Vec<CString>           m_enabled_device_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool m_command_pool;

    QueueParameters      m_graphics_queue;
    QueueParameters      m_present_queue;
    Vec<QueueParameters> m_queues;

    VkExtent2D m_extent { 1, 1 };
};

// ---------- Util.hpp ----------

inline bool CreateStagingBuffer(const vvk::MemoryAllocator& allocator, VkDeviceSize size,
                                AllocatedBufferParameters& buffer) {
    VkBufferCreateInfo ci {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .size  = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    buffer.req_size = ci.size;

    auto allocated = allocator.create_buffer(ci, vvk::MemoryRequest::Upload());
    if (allocated.is_err()) {
        const auto error = allocated.unwrap_err_unchecked();
        rstd_error("staging allocation failed: kind={}, vk={}",
                   static_cast<int>(error.kind),
                   static_cast<int>(error.api_result));
        return false;
    }
    buffer.handle = allocated.unwrap_unchecked();
    return true;
}

// ---------- BufferManager.hpp ----------

enum class BufferUploadClass;
class BufferAllocation {
public:
    BufferAllocation();
    ~BufferAllocation();

    BufferAllocation(const BufferAllocation&)            = delete;
    BufferAllocation& operator=(const BufferAllocation&) = delete;

    BufferAllocation(BufferAllocation&& o) noexcept;
    BufferAllocation& operator=(BufferAllocation&& o) noexcept;

    explicit operator bool() const noexcept;

    VkBuffer       buffer() const noexcept;
    VkDeviceSize   offset() const noexcept;
    VkDeviceSize   size() const noexcept;
    rstd::uint64_t identity() const noexcept;
    VkDevice       device() const noexcept;
    auto           usage() const noexcept -> BufferUploadClass;
    auto           clone() const -> BufferAllocation;

private:
    struct State;
    friend class BufferManager;
    friend class BufferUploadBatchLease;

    explicit BufferAllocation(Arc<State> state);

    Option<Arc<State>> m_state;
};

enum class BufferUploadClass
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Transfer,
};

struct BufferAllocationRequest {
    VkDeviceSize      size { 0 };
    VkDeviceSize      alignment { 1 };
    BufferUploadClass usage { BufferUploadClass::Vertex };
};

struct BufferUploadTicket {
    u64 value { 0 };

    bool        Valid() const noexcept { return value != u64(); }
    friend bool operator==(const BufferUploadTicket&, const BufferUploadTicket&) = default;
};

class BufferManager;

struct BufferUploadRange {
    rstd::uint64_t allocation {};
    VkDeviceSize   offset {}, size {};
};
class BufferUploadSnapshot {
    Option<Arc<Vec<BufferUploadRange>>> ranges_;
    friend class BufferManager;
    explicit BufferUploadSnapshot(Vec<BufferUploadRange> ranges)
        : ranges_(Some(Arc<Vec<BufferUploadRange>>::make(rstd::move(ranges)))) {}

public:
    BufferUploadSnapshot() = default;
    auto clone() const -> BufferUploadSnapshot {
        BufferUploadSnapshot result;
        result.ranges_ = ranges_.clone();
        return result;
    }
    auto writes() const -> slice<BufferUploadRange> {
        return ranges_ ? (*ranges_)->as_slice() : slice<BufferUploadRange> {};
    }
    auto ranges(rstd::uint64_t allocation) const -> slice<BufferUploadRange> {
        auto  values = writes();
        usize low {}, high = values.len();
        while (low < high) {
            auto middle = low + (high - low) / usize(2);
            if (values[middle].allocation < allocation)
                low = middle + usize(1);
            else
                high = middle;
        }
        auto first = low;
        high       = values.len();
        while (low < high) {
            auto middle = low + (high - low) / usize(2);
            if (values[middle].allocation <= allocation)
                low = middle + usize(1);
            else
                high = middle;
        }
        if (first == low) return {};
        return slice<BufferUploadRange>::from_raw_parts(values.as_raw_ptr() + first.to_primitive(),
                                                        low - first);
    }
};
struct BufferUploadWrite {
    BufferAllocation allocation;
    VkDeviceSize     offset {}, size {};
};
class RecordedBufferUploads {
public:
    RecordedBufferUploads(const RecordedBufferUploads&)            = delete;
    RecordedBufferUploads& operator=(const RecordedBufferUploads&) = delete;
    RecordedBufferUploads();
    ~RecordedBufferUploads();

    RecordedBufferUploads(RecordedBufferUploads&&) noexcept;
    RecordedBufferUploads& operator=(RecordedBufferUploads&&) noexcept;

    bool Valid() const noexcept;
    auto Writes() const -> Vec<BufferUploadWrite>;
    auto Snapshot() const -> BufferUploadSnapshot;

private:
    struct State;
    friend class BufferManager;
    friend class BufferUploadBatchLease;

    RecordedBufferUploads(BufferManager* owner, Arc<State> state);
    void Reset();

    BufferManager*     m_owner { nullptr };
    Option<Arc<State>> m_state;
};

class BufferUploadBatchLease {
public:
    BufferUploadBatchLease(const BufferUploadBatchLease&)            = delete;
    BufferUploadBatchLease& operator=(const BufferUploadBatchLease&) = delete;
    BufferUploadBatchLease();
    ~BufferUploadBatchLease();

    BufferUploadBatchLease(BufferUploadBatchLease&&) noexcept;
    BufferUploadBatchLease& operator=(BufferUploadBatchLease&&) noexcept;

    bool                      Valid() const noexcept;
    slice<BufferUploadTicket> Tickets() const noexcept;

private:
    friend class BufferManager;
    explicit BufferUploadBatchLease(Arc<RecordedBufferUploads::State> state);

    Option<Arc<RecordedBufferUploads::State>> m_state;
};

struct BufferUploadStatistics {
    rstd::uint64_t queued_copies[5] {}, queued_bytes[5] {};
    rstd::uint64_t recorded_copies[5] {}, recorded_bytes[5] {};
};

class BufferManager {
public:
    BufferManager(const BufferManager&)            = delete;
    BufferManager& operator=(const BufferManager&) = delete;
    BufferManager(BufferManager&&)                 = delete;
    BufferManager& operator=(BufferManager&&)      = delete;
    explicit BufferManager(const Device&);
    ~BufferManager();

    bool init();
    void destroy();

    auto TryAllocate(const BufferAllocationRequest& request)
        -> Result<BufferAllocation, BackendError>;
    auto TryQueueWrite(BufferAllocation&, slice<rstd::uint8_t>, VkDeviceSize destination_offset = 0)
        -> Result<BufferUploadTicket, BackendError>;
    auto TryPreparePendingUploads(RecordedBufferUploads&) -> Result<empty, BackendError>;
    auto TryRecordUploads(vvk::CommandBuffer&, RecordedBufferUploads&)
        -> Result<empty, BackendError>;
    Option<BufferAllocation>   Allocate(const BufferAllocationRequest& request);
    Option<BufferUploadTicket> QueueWrite(BufferAllocation& allocation, slice<rstd::uint8_t> data,
                                          VkDeviceSize destination_offset = 0);

    bool HasPendingUploads() const noexcept;
    auto statistics() const -> BufferUploadStatistics;
    bool PreparePendingUploads(RecordedBufferUploads& prepared);
    bool RecordUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& prepared);
    bool RecordPendingUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& recorded);
    Option<BufferUploadBatchLease> CommitRecordedUploads(RecordedBufferUploads&& recorded);
    bool                           RestoreUploads(BufferUploadBatchLease uploads);
    void CancelRecordedUploads(const Arc<RecordedBufferUploads::State>& state);
    void Trim();

private:
    struct Impl;
    Box<Impl> m_impl;
};

struct BufferBackend {
    using Trait                  = BufferBackend;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = BufferBackend;

        auto AllocateBuffer(const BufferAllocationRequest& request)
            -> Result<BufferAllocation, BackendError> {
            return rstd::trait_call<0>(this, request);
        }

        auto QueueBufferWrite(rstd::mut_ref<BufferAllocation> allocation,
                              rstd::slice<rstd::u8> content, VkDeviceSize destination_offset = 0)
            -> Result<BufferUploadTicket, BackendError> {
            return rstd::trait_call<1>(this, allocation, content, destination_offset);
        }
    };

    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::AllocateBuffer, &T::QueueBufferWrite>;
};

// ---------- GraphicsPipeline.hpp ----------

struct PipelineParameters {
    vvk::Pipeline    handle;
    VkPipelineLayout layout { VK_NULL_HANDLE };
};

struct DescriptorSetInfo {
    bool push_descriptor { false };

    Vec<VkDescriptorSetLayoutBinding> bindings;

    auto clone() const -> DescriptorSetInfo {
        auto cloned = Vec<VkDescriptorSetLayoutBinding>::with_capacity(bindings.len());
        for (const auto& binding : bindings) {
            auto copied = binding;
            cloned.push(rstd::move(copied));
        }
        return DescriptorSetInfo {
            .push_descriptor = push_descriptor,
            .bindings        = rstd::move(cloned),
        };
    }
};

class GraphicsPipeline {
public:
    GraphicsPipeline(const GraphicsPipeline&)            = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&&)                 = delete;
    GraphicsPipeline& operator=(GraphicsPipeline&&)      = delete;
    GraphicsPipeline();
    ~GraphicsPipeline();

    void toDefault();
    auto tryCreate(const Device&, VkRenderPass, VkPipelineLayout, PipelineParameters&,
                   VkPipelineCache cache = VK_NULL_HANDLE)
        -> Result<empty, resource::ResourceError>;
    bool create(const Device&, VkRenderPass, VkPipelineLayout, PipelineParameters&,
                VkPipelineCache cache = VK_NULL_HANDLE);

    VkPipelineMultisampleStateCreateInfo   multisample {};
    VkPipelineRasterizationStateCreateInfo raster {};
    Option<bool>                           depth_clip;
    VkPipelineDepthStencilStateCreateInfo  depth {};

    const ShaderSpv* getShaderSpv(VkShaderStageFlagBits) const;
    const auto&      pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(slice<VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setColorBlendOptions(VkPipelineColorBlendStateCreateFlags,
                                           const rstd::array<float, 4>&);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline& addInputAttributeDescription(slice<VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(slice<VkVertexInputBindingDescription>);
    GraphicsPipeline& setCreateInfoOptions(VkPipelineCreateFlags flags, rstd::uint32_t subpass);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);
    GraphicsPipeline& setPrimitiveRestartEnable(bool);
    GraphicsPipeline& setViewportScissorCount(rstd::uint32_t viewport_count,
                                              rstd::uint32_t scissor_count);
    GraphicsPipeline& setDynamicStates(slice<VkDynamicState>);
    GraphicsPipeline& setSampleCount(VkSampleCountFlagBits);

private:
    vvk::RenderPass m_pass;

    VkPipelineCreateFlags m_create_flags { 0 };
    rstd::uint32_t        m_subpass { 0 };

    VkPipelineInputAssemblyStateCreateInfo m_input_assembly {};
    Vec<VkVertexInputBindingDescription>   m_input_bind_descriptions;
    Vec<VkVertexInputAttributeDescription> m_input_attr_descriptions;

    VkPipelineViewportStateCreateInfo        m_view;
    VkPipelineColorBlendStateCreateInfo      m_color;
    Vec<VkDynamicState>                      m_dynamic_states;
    Vec<VkPipelineColorBlendAttachmentState> m_color_attachments;
    BTreeMap<u32, Uni_ShaderSpv>             m_stage_spv_map;
};

// ---------- VertexInputState.hpp ----------

struct VertexInputState {
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkPipelineVertexInputStateCreateInfo   input;
    Vec<VkVertexInputBindingDescription>   bind_descriptions;
    Vec<VkVertexInputAttributeDescription> attr_descriptions;
};

// ---------- LocalExSwapchain.hpp ----------

struct LocalExHandle {
    LocalExHandle(const LocalExHandle&)               = delete;
    LocalExHandle&    operator=(const LocalExHandle&) = delete;
    ExHandle          handle;
    ExImageParameters image;

    LocalExHandle()  = default;
    ~LocalExHandle() = default;
    LocalExHandle(LocalExHandle&& o) noexcept: handle(o.handle), image(rstd::move(o.image)) {}
    LocalExHandle& operator=(LocalExHandle&& o) noexcept {
        handle = o.handle;
        image  = rstd::move(o.image);
        return *this;
    }
};

// NB: this used to be `private TripleSwapchain<ExHandle>` in classic, but
// module-attached classes can't downcast `this` to a privately-inherited
// global-attached base — clang refuses the implicit `this` conversion at
// the qualified-call site. Public inheritance is semantically equivalent
// here (callers never reach for the base interface directly).
class LocalExSwapchain final : public ::vrento::ExSwapchain,
                               public ::vrento::TripleSwapchain<::vrento::ExHandle> {
public:
    LocalExSwapchain(rstd::array<LocalExHandle, 3> handles, VkExtent2D ext,
                     rstd::uint32_t queue_family)
        : m_handles(rstd::move(handles)), m_extent(ext), m_queue_family(queue_family) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle         = h.handle;
            handle               = ::vrento::ExHandle(index++);
            handle.width         = (i32)h.image.extent.width;
            handle.height        = (i32)h.image.extent.height;
            handle.fd            = h.image.fd;
            handle.size          = usize(h.image.mem_reqs.size);
            handle.drm_fourcc    = h.image.drm_fourcc;
            handle.drm_modifier  = h.image.drm_modifier;
            handle.plane0_offset = h.image.plane0_offset;
            handle.plane0_stride = h.image.plane0_stride;
        }
        m_presented.store(&m_handles[usize()].handle);
        m_ready.store(&m_handles[usize(1)].handle);
        m_inprogress.store(&m_handles[usize(2)].handle);
    }

    ~LocalExSwapchain() override {
        int fd = m_last_sync_fd.exchange(-1, Ordering::AcqRel);
        if (fd >= 0) ::close(fd);
    }

    ::vrento::FrameSurfaceAcquireResult acquireRenderTarget() override {
        if (m_surface_pending) {
            return { .status = ::vrento::FrameSurfaceAcquireStatus::Busy };
        }
        const u32 slot_index     = rstd::as_cast<u32>(this->getInprogress()->id());
        const u64 acquire_serial = m_next_acquire_serial;
        if (acquire_serial == u64()) {
            return { .status = ::vrento::FrameSurfaceAcquireStatus::ProtocolError,
                     .error  = Some(FrameSurfaceError { FrameSurfaceErrorKind::SerialExhausted }) };
        }
        ++m_next_acquire_serial;
        ::vrento::FrameSurfaceLease lease {
            .identity = { .owner_generation = u64(1),
                          .image_index      = slot_index,
                          .acquire_serial   = acquire_serial },
            .reuse    = { .kind = ::vrento::FrameSurfaceReuseKind::QueueOrdered },
            .image    = ToImageParameters(m_handles.at(rstd::as_cast<usize>(slot_index)).image),
            .format   = VK_FORMAT_R8G8B8A8_UNORM,
            .initial_layout       = VK_IMAGE_LAYOUT_GENERAL,
            .initial_queue_family = m_queue_family,
            .acquire              = { .kind = ::vrento::FrameSurfaceAcquireKind::QueueOrdered },
            .final_layout         = VK_IMAGE_LAYOUT_GENERAL,
            .final_queue_family   = m_queue_family,
            .discard_content      = true,
        };
        if (! lease.valid()) {
            return { .status = ::vrento::FrameSurfaceAcquireStatus::ProtocolError,
                     .error  = Some(FrameSurfaceError { FrameSurfaceErrorKind::InvalidLease }) };
        }
        m_surface_pending  = true;
        m_pending_identity = lease.identity;
        auto completion    = MakeCompletionCapability(lease.identity);
        return { .status     = ::vrento::FrameSurfaceAcquireStatus::Acquired,
                 .lease      = rstd::move(lease),
                 .completion = rstd::move(completion) };
    }

    int takeLastFrameSyncFd() override { return m_last_sync_fd.exchange(-1, Ordering::AcqRel); }

    ::vrento::ExHandle* eatFrame() override {
        return this->TripleSwapchain<::vrento::ExHandle>::eatFrame();
    }
    rstd::array<::vrento::ExHandle*, 3> snapshot_all_slots() override {
        return this->TripleSwapchain<::vrento::ExHandle>::snapshot_all_slots();
    }

    unsigned width() const override { return m_extent.width; }
    unsigned height() const override { return m_extent.height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }

    bool ready() const override { return true; }

    void setOnReadyChanged(Option<::vrento::ExSwapchainReadyCallback> cb) override {
        if (cb) {
            ::vrento::ExSwapchainReadyEvent e {
                .ready  = true,
                .width  = m_extent.width,
                .height = m_extent.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            (*cb)->operator()(e);
        }
    }

protected:
    Atomic<::vrento::ExHandle*>& presented() override { return m_presented; }
    Atomic<::vrento::ExHandle*>& ready() override { return m_ready; }
    Atomic<::vrento::ExHandle*>& inprogress() override { return m_inprogress; }

private:
    ::vrento::FrameSurfaceCompletionResult CompleteRendered(::vrento::FrameSurfaceIdentity identity,
                                                            int acquire_sync_fd) override {
        if (! m_surface_pending) {
            if (acquire_sync_fd >= 0) ::close(acquire_sync_fd);
            return { .status   = ::vrento::FrameSurfaceCompletionStatus::NotPending,
                     .identity = identity };
        }
        if (identity != m_pending_identity) {
            if (acquire_sync_fd >= 0) ::close(acquire_sync_fd);
            return { .status   = ::vrento::FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        }
        m_surface_pending  = false;
        m_pending_identity = {};
        int old            = m_last_sync_fd.exchange(acquire_sync_fd, Ordering::AcqRel);
        if (old >= 0) ::close(old);
        this->renderFrame();
        return { .status   = ::vrento::FrameSurfaceCompletionStatus::Submitted,
                 .identity = identity };
    }

    ::vrento::FrameSurfaceCompletionResult
    AbortRenderTarget(::vrento::FrameSurfaceIdentity identity) override {
        if (! m_surface_pending) {
            return { .status   = ::vrento::FrameSurfaceCompletionStatus::NotPending,
                     .identity = identity };
        }
        if (identity != m_pending_identity) {
            return { .status   = ::vrento::FrameSurfaceCompletionStatus::StaleIdentity,
                     .identity = identity };
        }
        m_surface_pending  = false;
        m_pending_identity = {};
        return { .status = ::vrento::FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }

    rstd::array<LocalExHandle, 3>  m_handles;
    Atomic<::vrento::ExHandle*>    m_presented { nullptr };
    Atomic<::vrento::ExHandle*>    m_ready { nullptr };
    Atomic<::vrento::ExHandle*>    m_inprogress { nullptr };
    VkExtent2D                     m_extent;
    rstd::uint32_t                 m_queue_family { VK_QUEUE_FAMILY_IGNORED };
    Atomic<int>                    m_last_sync_fd { -1 };
    u64                            m_next_acquire_serial { 1 };
    ::vrento::FrameSurfaceIdentity m_pending_identity;
    bool                           m_surface_pending { false };
};

} // namespace vulkan
} // namespace vrento

export namespace rstd
{

template<>
struct Impl<Copy, vrento::vulkan::Extension> {};

template<typename T>
    requires requires(T& value) { static_cast<vrento::ExSwapchain&>(value); }
struct Impl<vrento::ExSwapchainObject, Box<T>> : ImplBase<Box<T>> {
    auto AsSwapchain() -> vrento::ExSwapchain& { return *this->self(); }
};

template<>
struct Impl<Copy, vrento::vulkan::ImageUploadTicket> {};

template<>
struct Impl<Copy, vrento::vulkan::BufferUploadTicket> {};

template<>
struct Impl<vrento::vulkan::MemoryBudgetSource, vrento::vulkan::Device>
    : ImplBase<vrento::vulkan::Device> {
    auto MemoryBudget() const -> vrento::vulkan::MemoryBudgetSnapshot {
        return this->self().MemoryBudget();
    }
};

template<>
struct Impl<vrento::vulkan::BufferBackend, vrento::vulkan::BufferManager>
    : ImplBase<vrento::vulkan::BufferManager> {
    auto AllocateBuffer(const vrento::vulkan::BufferAllocationRequest& request)
        -> Result<vrento::vulkan::BufferAllocation, vrento::vulkan::BackendError> {
        return this->self().TryAllocate(request);
    }

    auto QueueBufferWrite(mut_ref<vrento::vulkan::BufferAllocation> allocation, slice<u8> content,
                          VkDeviceSize destination_offset)
        -> Result<vrento::vulkan::BufferUploadTicket, vrento::vulkan::BackendError> {
        return this->self().TryQueueWrite(
            *allocation,
            slice<rstd::uint8_t>::from_raw_parts(
                reinterpret_cast<const rstd::uint8_t*>(content.as_raw_ptr()), content.len()),
            destination_offset);
    }
};

} // namespace rstd

template<typename T, typename... Args>
auto vrento::ExSwapchain::Make(Args&&... args) -> ExSwapchainOwner {
    auto owner = ExSwapchainOwner::make(Box<T>::make(rstd::forward<Args>(args)...));
    owner->AsSwapchain().m_owner = owner.downgrade();
    return owner;
}
