export module vrento:vulkan.gpu_buffer;
export import :vulkan.texture;
import :vulkan.graph;
import rstd;

namespace vrento::vulkan
{
struct BufferRangeState {
    VkDeviceSize         begin {}, end {};
    VkAccessFlags        access {};
    VkPipelineStageFlags stages { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    rstd::uint32_t       family { VK_QUEUE_FAMILY_IGNORED };
    rstd::uint32_t       released_to { VK_QUEUE_FAMILY_IGNORED };
    bool                 defined {};
};
struct GpuBufferState {
    DeviceSession         session;
    vvk::AllocatedBuffer  allocation;
    VkBufferUsageFlags    usage;
    VkDeviceSize          bytes;
    tex::Identity         identity;
    Vec<BufferRangeState> ranges;
    tex::Serial           revision {}, recording {};
    tex::SubmissionToken  last_use;
    unsigned              readers {};
    bool                  uncertain {};
    GpuBufferState(DeviceSession owner, vvk::AllocatedBuffer value, VkDeviceSize bytes,
                   VkBufferUsageFlags flags)
        : session(rstd::move(owner)),
          allocation(rstd::move(value)),
          usage(flags),
          identity(next_texture_identity(session)),
          bytes(bytes) {
        identity.resource |= tex::Serial(1) << 63;
        ranges.push({ 0, bytes });
    }
};
struct BufferReadReservation {
    Arc<GpuBufferState> state;
    bool                active { true };
    explicit BufferReadReservation(Arc<GpuBufferState> owner): state(rstd::move(owner)) {
        ++state->readers;
    }
    void release() {
        if (active) {
            --state->readers;
            active = false;
        }
    }
    ~BufferReadReservation() { release(); }
};
struct BufferTransaction {
    Arc<GpuBufferState>             state;
    tex::Serial                     recording;
    Vec<BufferRangeState>           ranges;
    Vec<Arc<BufferReadReservation>> readers;
    bool                            written {};
    BufferTransaction(Arc<GpuBufferState> owner, tex::Serial id)
        : state(rstd::move(owner)), recording(id), ranges(state->ranges.clone()) {}
    void Commit(tex::SubmissionToken token) {
        state->ranges   = rstd::move(ranges);
        state->last_use = rstd::move(token);
        if (written) ++state->revision;
        state->recording = 0;
        for (auto& reader : readers) reader->release();
    }
    void Uncertain() { state->uncertain = true; }
    void Cancel() {
        if (state->recording == recording) state->recording = 0;
    }
    ~BufferTransaction() { Cancel(); }
};
struct BufferTransactionOwner {
    Arc<BufferTransaction> transaction;
};
} // namespace vrento::vulkan
export namespace rstd
{
template<>
struct Impl<vrento::vulkan::NativeSubmissionParticipant, vrento::vulkan::BufferTransactionOwner>
    : ImplBase<vrento::vulkan::BufferTransactionOwner> {
    void Commit(vrento::texture::SubmissionToken token) {
        self().transaction->Commit(rstd::move(token));
    }
    void Uncertain() { self().transaction->Uncertain(); }
    void Cancel() { self().transaction->Cancel(); }
};
} // namespace rstd
export namespace vrento::vulkan
{
struct GpuBufferUse {
    VkDeviceSize         offset {}, size {};
    VkAccessFlags        access {};
    VkPipelineStageFlags stages {};
    bool                 write {}, require_contents { true };
};
class GpuBuffer {
    Arc<GpuBufferState> state_;
    explicit GpuBuffer(Arc<GpuBufferState> state): state_(rstd::move(state)) {}
    auto transaction(tex::Submission& work) const -> Result<Arc<BufferTransaction>, tex::Error> {
        auto native = texture_recording(work, state_->session);
        if (native.is_err()) return Err(native.unwrap_err());
        auto recording = native.unwrap();
        if (state_->uncertain) return Err(tex::Error { tex::ErrorKind::Closed });
        if (state_->recording && state_->recording != recording->identity)
            return Err(tex::Error { tex::ErrorKind::Busy });
        for (const auto& value : recording->retained) {
            auto existing = rstd::any::downcast_ref<BufferTransactionOwner>(value.deref());
            if (existing &&
                Arc<GpuBufferState>::ptr_eq(existing.unwrap()->transaction->state, state_))
                return Ok(existing.unwrap()->transaction.clone());
        }
        auto value = Arc<BufferTransaction>::make(state_.clone(), recording->identity);
        if (state_->last_use.valid()) {
            auto waited = work.commands()->Wait(state_->last_use, tex::Stages::All);
            if (waited.is_err()) return Err(waited.unwrap_err());
        }
        recording->participants.push(
            Arc<dyn<NativeSubmissionParticipant>>::make(BufferTransactionOwner { value.clone() }));
        recording->retained.push(
            Box<dyn<rstd::any::Any>>::make(BufferTransactionOwner { value.clone() }));
        state_->recording = recording->identity;
        return Ok(rstd::move(value));
    }

public:
    GpuBuffer(const GpuBuffer& other): state_(other.state_.clone()) {}
    GpuBuffer(GpuBuffer&&) = default;
    static auto create(DeviceSession session, VkDeviceSize bytes, VkBufferUsageFlags usage,
                       vvk::MemoryRequest memory = {}) -> Result<GpuBuffer, tex::Error> {
        if (! session.valid() || ! bytes || ! usage)
            return Err(tex::Error { tex::ErrorKind::Invalid });
        if (session.lost()) return Err(tex::Error { tex::ErrorKind::Lost, VK_ERROR_DEVICE_LOST });
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size       = bytes;
        info.usage      = usage;
        auto allocation = session.device().memory_allocator().create_buffer(info, memory);
        if (allocation.is_err()) return Err(tex::Error { tex::ErrorKind::Backend });
        return Ok(GpuBuffer(Arc<GpuBufferState>::make(session, allocation.unwrap(), bytes, usage)));
    }
    auto identity() const -> tex::Identity { return state_->identity; }
    auto size() const -> VkDeviceSize { return state_->bytes; }
    auto native() const -> VkBuffer { return state_->allocation.handle(); }
    auto session() const -> const DeviceSession& { return state_->session; }
    auto content_revision() const -> tex::Serial { return state_->revision; }
    auto reserve_read() const -> Result<Arc<BufferReadReservation>, tex::Error> {
        if (state_->session.lost() || state_->uncertain || state_->recording)
            return Err(tex::Error { tex::ErrorKind::Busy });
        return Ok(Arc<BufferReadReservation>::make(state_.clone()));
    }
    auto use(tex::Submission& work, GpuBufferUse use,
             Option<Arc<BufferReadReservation>> reader = None()) const
        -> Result<empty, tex::Error> {
        if (! use.size || use.offset > size() || use.size > size() - use.offset || ! use.stages ||
            (use.write && (state_->readers || state_->revision == ~tex::Serial {})))
            return Err(tex::Error { use.write && state_->readers ? tex::ErrorKind::Busy
                                                                 : tex::ErrorKind::Invalid });
        struct Required {
            VkAccessFlags      access;
            VkBufferUsageFlags usage;
        };
        Required required[] {
            { VK_ACCESS_TRANSFER_READ_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
            { VK_ACCESS_TRANSFER_WRITE_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT },
            { VK_ACCESS_UNIFORM_READ_BIT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT },
            { VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
            { VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT },
            { VK_ACCESS_INDEX_READ_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
            { VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT }
        };
        for (auto flags : required)
            if ((use.access & flags.access) && ! (state_->usage & flags.usage))
                return Err(tex::Error { tex::ErrorKind::Unsupported });
        if (use.write !=
            bool(use.access & (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT)))
            return Err(tex::Error { tex::ErrorKind::Invalid });
        auto queue = native_queue(work, state_->session);
        if (queue.is_err()) return Err(queue.unwrap_err());
        auto                 flags   = queue.unwrap().flags;
        VkPipelineStageFlags allowed = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                                       VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (flags & VK_QUEUE_COMPUTE_BIT)
            allowed |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        if (flags & VK_QUEUE_GRAPHICS_BIT)
            allowed |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        if (use.stages & ~allowed) return Err(tex::Error { tex::ErrorKind::Unsupported });
        auto result = transaction(work);
        if (result.is_err()) return Err(result.unwrap_err());
        auto                  pending   = result.unwrap();
        auto                  recording = texture_recording(work, state_->session).unwrap();
        auto                  family    = recording->signal->queue.family_index;
        Vec<BufferRangeState> ranges;
        for (const auto& previous : pending->ranges) {
            if (previous.end <= use.offset || previous.begin >= use.offset + use.size) {
                ranges.push(BufferRangeState(previous));
                continue;
            }
            if ((use.require_contents && ! previous.defined) ||
                (previous.released_to != VK_QUEUE_FAMILY_IGNORED &&
                 previous.released_to != family) ||
                (previous.family != VK_QUEUE_FAMILY_IGNORED && previous.family != family &&
                 previous.released_to != family))
                return Err(tex::Error { tex::ErrorKind::Invalid });
            // Ownership acquire must match the range recorded by the release.
            if (previous.family != VK_QUEUE_FAMILY_IGNORED && previous.family != family &&
                (previous.begin < use.offset || previous.end > use.offset + use.size))
                return Err(tex::Error { tex::ErrorKind::Unsupported });
        }
        for (const auto& previous : pending->ranges) {
            if (previous.end <= use.offset || previous.begin >= use.offset + use.size) continue;
            auto begin = rstd::cmp::max(previous.begin, use.offset);
            auto end   = rstd::cmp::min(previous.end, use.offset + use.size);
            if (previous.begin < begin) {
                auto left = previous;
                left.end  = begin;
                ranges.push(rstd::move(left));
            }
            VkBufferMemoryBarrier barrier { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            bool acquire = previous.family != VK_QUEUE_FAMILY_IGNORED && previous.family != family;
            barrier.srcAccessMask       = acquire ? 0 : previous.access;
            barrier.dstAccessMask       = use.access;
            barrier.srcQueueFamilyIndex = acquire ? previous.family : VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = acquire ? family : VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer              = native();
            barrier.offset              = begin;
            barrier.size                = end - begin;
            state_->session.device().handle().Dispatch().vkCmdPipelineBarrier(
                recording->command,
                acquire ? VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) : previous.stages,
                use.stages,
                0,
                0,
                nullptr,
                1,
                &barrier,
                0,
                nullptr);
            ranges.push({ begin,
                          end,
                          use.access,
                          use.stages,
                          family,
                          VK_QUEUE_FAMILY_IGNORED,
                          use.write || previous.defined });
            if (end < previous.end) {
                auto right  = previous;
                right.begin = end;
                ranges.push(rstd::move(right));
            }
        }
        pending->ranges = rstd::move(ranges);
        pending->written |= use.write;
        if (reader) pending->readers.push(reader->clone());
        return Ok(empty {});
    }
    auto release(tex::Submission& work, VkDeviceSize offset, VkDeviceSize bytes,
                 rstd::uint32_t destination) const -> Result<empty, tex::Error> {
        if (! bytes || offset > size() || bytes > size() - offset)
            return Err(tex::Error { tex::ErrorKind::Invalid });
        bool found {};
        for (const auto& queue : state_->session.device().queues())
            found |= queue.family_index == destination &&
                     (queue.flags &
                      (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
        if (! found) return Err(tex::Error { tex::ErrorKind::Unsupported });
        auto result = transaction(work);
        if (result.is_err()) return Err(result.unwrap_err());
        auto pending   = result.unwrap();
        auto recording = texture_recording(work, state_->session).unwrap();
        auto family    = recording->signal->queue.family_index;
        if (family == destination) return Ok(empty {});
        for (const auto& range : pending->ranges)
            if (range.end > offset && range.begin < offset + bytes &&
                (range.family != family || range.released_to != VK_QUEUE_FAMILY_IGNORED))
                return Err(tex::Error { tex::ErrorKind::Invalid });
        Vec<BufferRangeState> ranges;
        for (auto range : pending->ranges) {
            if (range.end <= offset || range.begin >= offset + bytes) {
                ranges.push(rstd::move(range));
                continue;
            }
            auto begin = rstd::cmp::max(range.begin, offset),
                 end   = rstd::cmp::min(range.end, offset + bytes);
            if (range.begin < begin) {
                auto left = range;
                left.end  = begin;
                ranges.push(rstd::move(left));
            }
            if (end < range.end) {
                auto right  = range;
                right.begin = end;
                ranges.push(rstd::move(right));
            }
            range.begin = begin;
            range.end   = end;
            VkBufferMemoryBarrier barrier { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            barrier.srcAccessMask       = range.access;
            barrier.srcQueueFamilyIndex = family;
            barrier.dstQueueFamilyIndex = destination;
            barrier.buffer              = native();
            barrier.offset              = begin;
            barrier.size                = end - begin;
            state_->session.device().handle().Dispatch().vkCmdPipelineBarrier(
                recording->command,
                range.stages,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0,
                nullptr,
                1,
                &barrier,
                0,
                nullptr);
            range.released_to = destination;
            ranges.push(rstd::move(range));
        }
        pending->ranges = rstd::move(ranges);
        return Ok(empty {});
    }
    auto readback() const -> Result<Vec<u8>, tex::Error> {
        if (state_->recording || state_->uncertain) return Err(tex::Error { tex::ErrorKind::Busy });
        if (state_->last_use.valid()) {
            auto ready = state_->last_use.poll();
            if (ready.is_err()) return Err(ready.unwrap_err());
            if (! ready.unwrap()) return Err(tex::Error { tex::ErrorKind::Busy });
        }
        for (const auto& range : state_->ranges)
            if (! range.defined || ! (range.access & VK_ACCESS_HOST_READ_BIT))
                return Err(tex::Error { tex::ErrorKind::Invalid });
        auto allocation = state_->allocation.allocation();
        auto mapped     = allocation.map();
        if (mapped.is_err() || allocation.invalidate().is_err())
            return Err(tex::Error { tex::ErrorKind::Backend });
        auto    memory = mapped.unwrap();
        Vec<u8> bytes;
        for (VkDeviceSize i {}; i < size(); ++i)
            bytes.push(u8(static_cast<const rstd::uint8_t*>(memory.data())[i]));
        return Ok(rstd::move(bytes));
    }
};
class GraphGpuBufferBinding {
    GpuBuffer                          buffer_;
    tex::Serial                        revision_;
    bool                               frozen_ {};
    Option<Arc<BufferReadReservation>> reader_;

public:
    explicit GraphGpuBufferBinding(GpuBuffer buffer)
        : buffer_(rstd::move(buffer)), revision_(buffer_.content_revision()) {}
    auto Info() const -> GraphBindingInfo {
        return { GraphResourceKind::Buffer, buffer_.identity(), buffer_.size(), 1, 1, 1,
                 buffer_.content_revision() };
    }
    auto Texture() const -> Option<tex::TextureViewRef> { return None(); }
    auto Freeze(bool read_only) const -> Result<Arc<dyn<GraphBinding>>, GraphExecutionError>;
    auto Use(tex::Submission& work, const PassResourceUse& use) const
        -> Result<empty, GraphExecutionError> {
        if (frozen_ && use.version == 0 && use.access == GraphAccess::Read &&
            revision_ != buffer_.content_revision())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, use.resource });
        VkAccessFlags access {};
        auto          stages = texture_stages(graph_stages(use.stages));
        bool          write  = use.access == GraphAccess::Write;
        switch (use.usage) {
        case PassUsage::Storage:
            access = write ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            break;
        case PassUsage::Uniform: access = VK_ACCESS_UNIFORM_READ_BIT; break;
        case PassUsage::Vertex:
            access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            break;
        case PassUsage::Index:
            access = VK_ACCESS_INDEX_READ_BIT;
            stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            break;
        case PassUsage::Indirect:
            access = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            stages = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            break;
        case PassUsage::Transfer:
            access = write ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
            break;
        default:
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, {}, use.resource });
        }
        auto result =
            buffer_.use(work,
                        { use.range.offset, use.range.size, access, stages, write, ! write },
                        reader_ ? Some(reader_->clone()) : None<Arc<BufferReadReservation>>());
        if (result.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Access,
                                             {},
                                             use.resource,
                                             VK_SUCCESS,
                                             Some(result.unwrap_err()) });
        return Ok(empty {});
    }
    auto Release(tex::Submission& work, rstd::uint32_t family, const PassResourceUse& next) const
        -> Result<empty, GraphExecutionError> {
        auto result = buffer_.release(work, next.range.offset, next.range.size, family);
        if (result.is_err())
            return Err(GraphExecutionError { GraphExecutionStage::Access,
                                             {},
                                             next.resource,
                                             VK_SUCCESS,
                                             Some(result.unwrap_err()) });
        return Ok(empty {});
    }
};
} // namespace vrento::vulkan
export namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphBinding, vrento::vulkan::GraphGpuBufferBinding>
    : ImplBase<vrento::vulkan::GraphGpuBufferBinding> {
    auto Info() const -> vrento::vulkan::GraphBindingInfo { return self().Info(); }
    auto Texture() const -> Option<vrento::texture::TextureViewRef> { return self().Texture(); }
    auto Use(vrento::texture::Submission& work, const vrento::vulkan::PassResourceUse& use) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Use(work, use);
    }
    auto Freeze(bool read_only) const -> Result<sync::Arc<dyn<vrento::vulkan::GraphBinding>>,
                                                vrento::vulkan::GraphExecutionError> {
        return self().Freeze(read_only);
    }
    auto Release(vrento::texture::Submission& work, rstd::uint32_t family,
                 const vrento::vulkan::PassResourceUse& next) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Release(work, family, next);
    }
};
} // namespace rstd
namespace vrento::vulkan
{
auto GraphGpuBufferBinding::Freeze(bool read_only) const
    -> Result<Arc<dyn<GraphBinding>>, GraphExecutionError> {
    GraphGpuBufferBinding result(buffer_);
    result.frozen_ = true;
    if (read_only) {
        auto lease = buffer_.reserve_read();
        if (lease.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding, {}, {}, VK_SUCCESS, Some(lease.unwrap_err()) });
        result.reader_ = Some(lease.unwrap());
    }
    return Ok(Arc<dyn<GraphBinding>>::make(rstd::move(result)));
}
} // namespace vrento::vulkan
