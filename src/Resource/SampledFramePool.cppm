export module vrento.sampled_frame_pool;
export import vrento.sampled_frame;
import rstd;

export namespace vrento::resource {
using namespace rstd::prelude;
using rstd::sync::Arc;

enum class FramePoolStage {
    Invalid,
    Unsupported,
    Busy,
    Closed,
    Limit,
    Allocate,
    Record,
    Submit,
    Poll,
    Timeout,
    Lost
};
struct FramePoolError {
    FramePoolStage stage;
    VkResult result{VK_SUCCESS};
    Option<vvk::MemoryError> memory{};
};
enum class FrameWriteMode { ColorAttachment, TransferDestination };
struct FramePoolOptions {
    rstd::uint64_t producer_session{};
    rstd::uint64_t initial_version{};
    VkExtent2D extent{};
    rstd::uint32_t capacity{3};
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
    FrameColorSpace color_space{FrameColorSpace::LinearSrgb};
    FrameAlpha alpha{FrameAlpha::Premultiplied};
    FrameOrigin origin{FrameOrigin::TopLeft};
};
struct FramePoolWait {
    VkSemaphore semaphore{};
    rstd::uint64_t value{};
    VkPipelineStageFlags stages{};
};
class FrameSubmissionResources {
  public:
    virtual ~FrameSubmissionResources() = default;
};
struct FramePoolStats {
    rstd::uint32_t capacity{}, available{}, writing{}, pending{}, leased{};
    rstd::uint64_t submitted{}, completed{}, cancelled{}, busy{}, last_version{};
    bool closed{}, failed{};
};

// All operations and lease destruction are serialized by the device's owner thread.
class SampledFramePool {
    static constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct Slot {
        vvk::AllocatedImage image;
        vvk::ImageView view;
        vvk::Fence fence;
        VkCommandBuffer command{};
        bool writing{}, pending{}, leased{};
        Option<Box<FrameSubmissionResources>> inputs;
    };
    struct State {
        vulkan::DeviceSession session;
        FramePoolOptions options;
        vvk::Semaphore ready;
        Vec<Box<Slot>> slots;
        rstd::uint64_t submitted{}, completed{}, cancelled{}, busy{}, version{};
        bool closed{}, failed{};
        VkResult failure{VK_SUCCESS};
        explicit State(vulkan::DeviceSession device, FramePoolOptions opts)
            : session(rstd::move(device)), options(opts), version(opts.initial_version) {}
        void retire() {
            auto wait_idle = [&] {
                auto result = session.wait_idle();
                if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                    rstd::panic("cannot retire frame pool device resources");
            };
            if (session.lost()) {
                failed = true;
                failure = VK_ERROR_DEVICE_LOST;
            }
            if (failed)
                wait_idle();
            for (auto &slot : slots) {
                if (slot->pending && !failed) {
                    auto result = session.observe_result(slot->fence.Wait(~rstd::uint64_t{}));
                    if (result != VK_SUCCESS) {
                        failed = true;
                        failure = result;
                        wait_idle();
                    } else {
                        ++completed;
                    }
                }
                slot->pending = false;
                slot->inputs = None();
            }
        }
        ~State() {
            retire();
            const auto &device = session.device();
            for (auto &slot : slots) {
                if (slot->command)
                    device.handle().Dispatch().vkFreeCommandBuffers(
                        *device.handle(), *device.cmd_pool(), 1, &slot->command);
            }
        }
        auto poll() -> Result<empty, FramePoolError> {
            if (session.lost()) {
                failed = true;
                failure = VK_ERROR_DEVICE_LOST;
                return Err(FramePoolError{FramePoolStage::Lost, failure});
            }
            if (failed)
                return Err(FramePoolError{FramePoolStage::Poll, failure});
            for (auto &slot : slots) {
                if (!slot->pending)
                    continue;
                auto result = session.observe_result(slot->fence.GetStatus());
                if (result == VK_NOT_READY)
                    continue;
                if (result != VK_SUCCESS) {
                    failed = true;
                    failure = result;
                    return Err(FramePoolError{FramePoolStage::Poll, result});
                }
                slot->pending = false;
                slot->inputs = None();
                ++completed;
            }
            return Ok(empty{});
        }
    };
    struct Lease : SampledFrameResource {
        Arc<State> owner;
        usize slot;
        SampledFrameDescription value;
        Lease(Arc<State> state, usize index, SampledFrameDescription d)
            : owner(rstd::move(state)), slot(index), value(d) {}
        ~Lease() { owner->slots[slot]->leased = false; }
        auto description() const -> SampledFrameDescription override { return value; }
    };
    Arc<State> state_;
    explicit SampledFramePool(Arc<State> state) : state_(rstd::move(state)) {}
    static auto layout(FrameWriteMode mode) -> VkImageLayout {
        return mode == FrameWriteMode::ColorAttachment ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    static auto stage(FrameWriteMode mode) -> VkPipelineStageFlags {
        return mode == FrameWriteMode::ColorAttachment
                   ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                   : VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    static auto access(FrameWriteMode mode) -> VkAccessFlags {
        return mode == FrameWriteMode::ColorAttachment
                   ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                   : VK_ACCESS_TRANSFER_WRITE_BIT;
    }

  public:
    class Write {
        friend class SampledFramePool;
        Arc<State> owner_;
        usize index_;
        FrameWriteMode mode_;
        Write(Arc<State> state, usize index, FrameWriteMode mode)
            : owner_(rstd::move(state)), index_(index), mode_(mode) {}
        void release() {
            if (owner_) {
                owner_->slots[index_]->writing = false;
                ++owner_->cancelled;
                owner_.reset();
            }
        }

      public:
        Write(const Write &) = delete;
        auto operator=(const Write &) -> Write & = delete;
        Write(Write &&) = default;
        auto operator=(Write &&other) -> Write & {
            if (this != &other) {
                release();
                owner_ = rstd::move(other.owner_);
                index_ = other.index_;
                mode_ = other.mode_;
            }
            return *this;
        }
        ~Write() { release(); }
        auto valid() const -> bool { return bool(owner_); }
        auto command() const -> VkCommandBuffer {
            return owner_ ? owner_->slots[index_]->command : VK_NULL_HANDLE;
        }
        auto image() const -> VkImage {
            return owner_ ? owner_->slots[index_]->image.handle() : VK_NULL_HANDLE;
        }
        auto view() const -> VkImageView {
            return owner_ ? *owner_->slots[index_]->view : VK_NULL_HANDLE;
        }
        auto extent() const -> VkExtent2D { return owner_ ? owner_->options.extent : VkExtent2D{}; }
        auto format() const -> VkFormat {
            return owner_ ? owner_->options.format : VK_FORMAT_UNDEFINED;
        }
        auto target_layout() const -> VkImageLayout { return SampledFramePool::layout(mode_); }
        void cancel() { release(); }
    };

    SampledFramePool(const SampledFramePool &) = delete;
    auto operator=(const SampledFramePool &) -> SampledFramePool & = delete;
    SampledFramePool(SampledFramePool &&) = default;
    auto operator=(SampledFramePool &&other) -> SampledFramePool & {
        if (this != &other) {
            close();
            if (state_)
                state_->retire();
            state_ = rstd::move(other.state_);
        }
        return *this;
    }
    ~SampledFramePool() {
        close();
        if (state_)
            state_->retire();
    }

    static auto create(vulkan::DeviceSession session, FramePoolOptions options)
        -> Result<SampledFramePool, FramePoolError> {
        if (!session.valid() || !options.producer_session || !options.extent.width ||
            !options.extent.height || !options.capacity)
            return Err(FramePoolError{FramePoolStage::Invalid});
        if (session.lost())
            return Err(FramePoolError{FramePoolStage::Lost, VK_ERROR_DEVICE_LOST});
        const auto &device = session.device();
        if (options.extent.width > device.limits().maxImageDimension2D ||
            options.extent.height > device.limits().maxImageDimension2D || options.capacity > 64 ||
            options.initial_version == ~rstd::uint64_t{})
            return Err(FramePoolError{FramePoolStage::Limit});
        if (SampledFrame::validate_encoding(options.format, options.color_space, options.alpha,
                                            options.origin)
                .is_err())
            return Err(FramePoolError{FramePoolStage::Unsupported});
        auto features = device.gpu().GetFormatProperties(options.format).optimalTilingFeatures;
        constexpr auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((features & needed) != needed)
            return Err(FramePoolError{FramePoolStage::Unsupported});
        VkImageFormatProperties properties{};
        auto supported = device.instance_dispatch().vkGetPhysicalDeviceImageFormatProperties(
            *device.gpu(), options.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0,
            &properties);
        if (supported != VK_SUCCESS)
            return Err(FramePoolError{FramePoolStage::Unsupported, supported});
        if (options.extent.width > properties.maxExtent.width ||
            options.extent.height > properties.maxExtent.height ||
            !(properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            return Err(FramePoolError{FramePoolStage::Unsupported});
        auto state = Arc<State>::make(rstd::move(session), options);
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = options.initial_version;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphore.pNext = &type;
        auto result = device.handle().CreateSemaphore(semaphore, state->ready);
        if (result != VK_SUCCESS)
            return Err(
                FramePoolError{FramePoolStage::Allocate, state->session.observe_result(result)});
        for (rstd::uint32_t i = 0; i < options.capacity; ++i) {
            state->slots.push(Box<Slot>::make());
            auto &slot = *state->slots[usize(i)];
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.extent = {options.extent.width, options.extent.height, 1};
            info.format = options.format;
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            auto allocation = device.memory_allocator().create_image(info);
            if (allocation.is_err()) {
                auto error = allocation.unwrap_err();
                return Err(FramePoolError{FramePoolStage::Allocate,
                                          state->session.observe_result(error.api_result),
                                          Some(error)});
            }
            slot.image = allocation.unwrap();
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = slot.image.handle();
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = info.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result = device.handle().CreateImageView(view, slot.view);
            if (result != VK_SUCCESS)
                return Err(FramePoolError{FramePoolStage::Allocate,
                                          state->session.observe_result(result)});
            result = device.handle().CreateFence({VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, slot.fence);
            if (result != VK_SUCCESS)
                return Err(FramePoolError{FramePoolStage::Allocate,
                                          state->session.observe_result(result)});
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commands.commandPool = *device.cmd_pool();
            commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commands.commandBufferCount = 1;
            result = device.handle().Dispatch().vkAllocateCommandBuffers(*device.handle(),
                                                                         &commands, &slot.command);
            if (result != VK_SUCCESS)
                return Err(FramePoolError{FramePoolStage::Allocate,
                                          state->session.observe_result(result)});
        }
        return Ok(SampledFramePool(rstd::move(state)));
    }

    auto acquire(FrameWriteMode mode = FrameWriteMode::ColorAttachment)
        -> Result<Write, FramePoolError> {
        if (!state_ || (mode != FrameWriteMode::ColorAttachment &&
                        mode != FrameWriteMode::TransferDestination))
            return Err(FramePoolError{FramePoolStage::Invalid});
        if (state_->session.lost())
            return Err(FramePoolError{FramePoolStage::Lost, VK_ERROR_DEVICE_LOST});
        if (state_->closed)
            return Err(FramePoolError{FramePoolStage::Closed});
        auto done = state_->poll();
        if (done.is_err())
            return Err(done.unwrap_err());
        for (usize i{}; i < state_->slots.len(); ++i) {
            auto &slot = *state_->slots[i];
            if (slot.writing || slot.pending || slot.leased)
                continue;
            const auto &d = state_->session.device().handle().Dispatch();
            auto result = d.vkResetCommandBuffer(slot.command, 0);
            if (result != VK_SUCCESS)
                return Err(
                    FramePoolError{FramePoolStage::Record, state_->session.observe_result(result)});
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = d.vkBeginCommandBuffer(slot.command, &begin);
            if (result != VK_SUCCESS)
                return Err(
                    FramePoolError{FramePoolStage::Record, state_->session.observe_result(result)});
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = layout(mode);
            barrier.dstAccessMask = access(mode);
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = slot.image.handle();
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d.vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage(mode), 0,
                                   0, nullptr, 0, nullptr, 1, &barrier);
            slot.writing = true;
            return Ok(Write(state_.clone(), i, mode));
        }
        ++state_->busy;
        return Err(FramePoolError{FramePoolStage::Busy});
    }

    // The recorder must finish its render pass and leave the target in target_layout().
    // Inputs own every external resource and wait semaphore until producer GPU completion.
    auto submit(Write write, slice<FramePoolWait> waits = {},
                Option<Box<FrameSubmissionResources>> inputs = {})
        -> Result<SampledFrame, FramePoolError> {
        if (!state_ || !write.owner_ || &*write.owner_ != &*state_)
            return Err(FramePoolError{FramePoolStage::Invalid});
        if (state_->session.lost())
            return Err(FramePoolError{FramePoolStage::Lost, VK_ERROR_DEVICE_LOST});
        if (state_->closed)
            return Err(FramePoolError{FramePoolStage::Closed});
        if (state_->failed)
            return Err(FramePoolError{FramePoolStage::Submit, state_->failure});
        if (state_->version == ~rstd::uint64_t{} || waits.len() > usize(64))
            return Err(FramePoolError{FramePoolStage::Limit});
        Vec<VkSemaphore> semaphores;
        Vec<rstd::uint64_t> values;
        Vec<VkPipelineStageFlags> stages;
        for (const auto &wait : waits) {
            if (!wait.semaphore || !wait.stages || wait.semaphore == *state_->ready)
                return Err(FramePoolError{FramePoolStage::Invalid});
            for (auto previous : semaphores)
                if (previous == wait.semaphore)
                    return Err(FramePoolError{FramePoolStage::Invalid});
            semaphores.push(VkSemaphore(wait.semaphore));
            values.push(rstd::uint64_t(wait.value));
            stages.push(VkPipelineStageFlags(wait.stages));
        }
        auto &slot = *state_->slots[write.index_];
        const auto &device = state_->session.device();
        const auto &d = device.handle().Dispatch();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = access(write.mode_);
        barrier.oldLayout = layout(write.mode_);
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slot.image.handle();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d.vkCmdPipelineBarrier(slot.command, stage(write.mode_),
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &barrier);
        auto result = d.vkEndCommandBuffer(slot.command);
        if (result != VK_SUCCESS)
            return Err(
                FramePoolError{FramePoolStage::Record, state_->session.observe_result(result)});
        result = slot.fence.Reset();
        if (result != VK_SUCCESS)
            return Err(
                FramePoolError{FramePoolStage::Submit, state_->session.observe_result(result)});
        auto version = state_->version + 1;
        SampledFrameDescription description;
        description.producer_session = state_->options.producer_session;
        description.frame_version = description.ready_value = version;
        description.device_epoch = state_->session.epoch();
        description.device = *device.handle();
        description.image = slot.image.handle();
        description.view = *slot.view;
        description.extent = state_->options.extent;
        description.format = state_->options.format;
        description.usage = usage;
        description.queue_family = device.graphics_queue().family_index;
        description.color_space = state_->options.color_space;
        description.alpha = state_->options.alpha;
        description.origin = state_->options.origin;
        description.ready_timeline = *state_->ready;
        auto lease = Box<Lease>::make(state_.clone(), write.index_, description);
        slot.leased = true;
        auto published = SampledFrame::publish(Box<SampledFrameResource>::from_raw(
            rstd::move(lease).into_raw().template cast<SampledFrameResource>()));
        if (published.is_err())
            return Err(FramePoolError{FramePoolStage::Invalid});
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = static_cast<rstd::uint32_t>(waits.len().to_primitive());
        timeline.pWaitSemaphoreValues = values.data();
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &version;
        VkSemaphore signal = *state_->ready;
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.pNext = &timeline;
        info.waitSemaphoreCount = timeline.waitSemaphoreValueCount;
        info.pWaitSemaphores = semaphores.data();
        info.pWaitDstStageMask = stages.data();
        info.commandBufferCount = 1;
        info.pCommandBuffers = &slot.command;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &signal;
        slot.inputs = rstd::move(inputs);
        result = device.graphics_queue().handle.Submit(info, *slot.fence);
        if (result != VK_SUCCESS) {
            state_->failed = true;
            state_->failure = result;
            // Submission failure quarantines the pool until device teardown.
            return Err(
                FramePoolError{FramePoolStage::Submit, state_->session.observe_result(result)});
        }
        slot.pending = true;
        slot.writing = false;
        write.owner_.reset();
        ++state_->submitted;
        state_->version = version;
        return Ok(published.unwrap());
    }

    auto poll() -> Result<FramePoolStats, FramePoolError> {
        if (!state_)
            return Err(FramePoolError{FramePoolStage::Invalid});
        auto result = state_->poll();
        if (result.is_err())
            return Err(result.unwrap_err());
        return Ok(stats());
    }
    auto stats() const -> FramePoolStats {
        FramePoolStats result;
        if (!state_)
            return result;
        result.capacity = state_->options.capacity;
        result.submitted = state_->submitted;
        result.completed = state_->completed;
        result.cancelled = state_->cancelled;
        result.busy = state_->busy;
        result.last_version = state_->version;
        result.closed = state_->closed;
        result.failed = state_->failed || state_->session.lost();
        for (const auto &slot : state_->slots) {
            result.writing += slot->writing;
            result.pending += slot->pending;
            result.leased += slot->leased;
            result.available += !state_->closed && !result.failed && !slot->writing &&
                                !slot->pending && !slot->leased;
        }
        return result;
    }
    // Waits for producer submissions only; consumer-held frames may remain leased.
    auto drain(rstd::uint64_t timeout_nanoseconds) -> Result<FramePoolStats, FramePoolError> {
        auto current = poll();
        if (current.is_err())
            return Err(current.unwrap_err());
        Vec<VkFence> fences;
        for (const auto &slot : state_->slots)
            if (slot->pending)
                fences.push(VkFence(*slot->fence));
        if (fences.is_empty())
            return current;
        const auto &device = state_->session.device();
        auto result = device.handle().Dispatch().vkWaitForFences(
            *device.handle(), static_cast<rstd::uint32_t>(fences.len().to_primitive()),
            fences.data(), VK_TRUE, timeout_nanoseconds);
        if (result == VK_TIMEOUT)
            return Err(
                FramePoolError{FramePoolStage::Timeout, state_->session.observe_result(result)});
        if (result != VK_SUCCESS) {
            state_->failed = true;
            state_->failure = result;
            return Err(
                FramePoolError{FramePoolStage::Poll, state_->session.observe_result(result)});
        }
        return poll();
    }
    void close() {
        if (state_)
            state_->closed = true;
    }
};
} // namespace vrento::resource
