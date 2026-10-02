export module vrento.sampled_frame;
export import vrento.device_session;
import rstd;

export namespace vrento::resource {
using namespace rstd::prelude;

enum class FrameColorSpace { LinearSrgb, Srgb };
enum class FrameAlpha { Opaque, Premultiplied, Straight };
enum class FrameOrigin { TopLeft, BottomLeft };
enum class SampledFrameError {
    Invalid,
    UnsupportedFormat,
    UnsupportedImage,
    UnsupportedLayout,
    UnsupportedQueue,
    DeviceMismatch,
    StaleEpoch,
    UnsupportedSampling,
    DeviceLost,
};

struct SampledFrameDescription {
    rstd::uint64_t producer_session{}, frame_version{}, device_epoch{};
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageUsageFlags usage{};
    VkImageTiling tiling{VK_IMAGE_TILING_OPTIMAL};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    rstd::uint32_t mip_levels{1}, array_layers{1};
    VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    rstd::uint32_t queue_family{VK_QUEUE_FAMILY_IGNORED};
    FrameColorSpace color_space{FrameColorSpace::LinearSrgb};
    FrameAlpha alpha{FrameAlpha::Premultiplied};
    FrameOrigin origin{FrameOrigin::TopLeft};
    VkSemaphore ready_timeline{};
    rstd::uint64_t ready_value{};
};

// Owns the device, image, view and timeline until the last frame reference is released.
class SampledFrameResource {
  public:
    virtual ~SampledFrameResource() = default;
    virtual auto description() const -> SampledFrameDescription = 0;
};

class SampledFrame {
    struct Data {
        SampledFrameDescription description;
        Box<SampledFrameResource> resource;
        Data(SampledFrameDescription d, Box<SampledFrameResource> r)
            : description(d), resource(rstd::move(r)) {}
    };
    rstd::sync::Arc<Data> data_;
    SampledFrame(SampledFrameDescription d, Box<SampledFrameResource> r)
        : data_(rstd::sync::Arc<Data>::make(d, rstd::move(r))) {}

  public:
    SampledFrame(const SampledFrame &other) : data_(other.data_.clone()) {}
    auto operator=(const SampledFrame &other) -> SampledFrame & {
        if (this != &other)
            data_ = other.data_.clone();
        return *this;
    }
    SampledFrame(SampledFrame &&) = default;
    auto operator=(SampledFrame &&) -> SampledFrame & = default;

    static auto validate_encoding(VkFormat format, FrameColorSpace color, FrameAlpha alpha,
                                  FrameOrigin origin) -> Result<empty, SampledFrameError> {
        if ((color != FrameColorSpace::LinearSrgb && color != FrameColorSpace::Srgb) ||
            (alpha != FrameAlpha::Opaque && alpha != FrameAlpha::Premultiplied &&
             alpha != FrameAlpha::Straight) ||
            (origin != FrameOrigin::TopLeft && origin != FrameOrigin::BottomLeft))
            return Err(SampledFrameError::Invalid);
        switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            if (color != FrameColorSpace::Srgb)
                return Err(SampledFrameError::UnsupportedFormat);
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            break;
        default:
            return Err(SampledFrameError::UnsupportedFormat);
        }
        return Ok(empty{});
    }
    static auto publish(Box<SampledFrameResource> resource)
        -> Result<SampledFrame, SampledFrameError> {
        auto d = resource->description();
        if (!d.producer_session || !d.frame_version || !d.device_epoch || !d.device || !d.image ||
            !d.view || !d.extent.width || !d.extent.height || !d.ready_timeline || !d.ready_value)
            return Err(SampledFrameError::Invalid);
        auto encoding = validate_encoding(d.format, d.color_space, d.alpha, d.origin);
        if (encoding.is_err())
            return Err(encoding.unwrap_err());
        if (!(d.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || d.tiling != VK_IMAGE_TILING_OPTIMAL ||
            d.samples != VK_SAMPLE_COUNT_1_BIT || d.mip_levels != 1 || d.array_layers != 1)
            return Err(SampledFrameError::UnsupportedImage);
        if (d.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            return Err(SampledFrameError::UnsupportedLayout);
        if (d.queue_family == VK_QUEUE_FAMILY_IGNORED ||
            d.queue_family == VK_QUEUE_FAMILY_EXTERNAL ||
            d.queue_family == VK_QUEUE_FAMILY_FOREIGN_EXT)
            return Err(SampledFrameError::UnsupportedQueue);
        return Ok(SampledFrame(d, rstd::move(resource)));
    }
    auto valid() const -> bool { return bool(data_); }
    auto description() const -> const SampledFrameDescription & { return data_->description; }
};

class PreparedSampledFrame {
    SampledFrame frame_;
    VkPipelineStageFlags stage_;
    PreparedSampledFrame(SampledFrame frame, VkPipelineStageFlags stage)
        : frame_(rstd::move(frame)), stage_(stage) {}

  public:
    // Keep this lease until the submission's actual GPU completion, including failed polls.
    static auto prepare(const vulkan::DeviceSession &session, const SampledFrame &frame,
                        VkPipelineStageFlags stage, bool linear_filter = false)
        -> Result<PreparedSampledFrame, SampledFrameError> {
        if (!session.valid())
            return Err(SampledFrameError::Invalid);
        if (session.lost())
            return Err(SampledFrameError::DeviceLost);
        return prepare(session.device(), session.epoch(), frame, stage, linear_filter);
    }
    static auto prepare(const vulkan::Device &device, rstd::uint64_t device_epoch,
                        const SampledFrame &frame, VkPipelineStageFlags stage,
                        bool linear_filter = false)
        -> Result<PreparedSampledFrame, SampledFrameError> {
        if (device.lost())
            return Err(SampledFrameError::DeviceLost);
        constexpr auto shader_stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (!frame.valid() || !device_epoch || !stage || (stage & ~shader_stages))
            return Err(SampledFrameError::Invalid);
        const auto &d = frame.description();
        if (d.device != *device.handle())
            return Err(SampledFrameError::DeviceMismatch);
        if (d.device_epoch != device_epoch)
            return Err(SampledFrameError::StaleEpoch);
        if (d.queue_family != device.graphics_queue().family_index)
            return Err(SampledFrameError::UnsupportedQueue);
        if (d.extent.width > device.limits().maxImageDimension2D ||
            d.extent.height > device.limits().maxImageDimension2D)
            return Err(SampledFrameError::UnsupportedImage);
        auto features = device.gpu().GetFormatProperties(d.format).optimalTilingFeatures;
        auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                        (linear_filter ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
        if ((features & required) != required)
            return Err(SampledFrameError::UnsupportedSampling);
        return Ok(PreparedSampledFrame(frame, stage));
    }
    auto description() const -> const SampledFrameDescription & { return frame_.description(); }
    auto wait_stage() const -> VkPipelineStageFlags { return stage_; }
};
} // namespace vrento::resource
