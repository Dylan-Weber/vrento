module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module vrento:vulkan.impl.buffer_manager;

import :vulkan.api;

import rstd;
import rstd.log;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cmp::max;
using rstd::cmp::min;
using rstd::sync::Arc;

namespace vrento::vulkan
{

namespace
{

constexpr VkDeviceSize kBufferPageSize  = 2 * 1024 * 1024;
constexpr VkDeviceSize kUploadBlockSize = 2 * 1024 * 1024;
constexpr VkDeviceSize kLargeAlignment  = 64 * 1024;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

VkBufferUsageFlags DestinationUsage() {
    return VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

VkAccessFlags DestinationAccess(BufferUploadClass usage) {
    switch (usage) {
    case BufferUploadClass::Vertex: return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    case BufferUploadClass::Index: return VK_ACCESS_INDEX_READ_BIT;
    case BufferUploadClass::Uniform: return VK_ACCESS_UNIFORM_READ_BIT;
    case BufferUploadClass::Storage: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case BufferUploadClass::Transfer:
        return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    return VK_ACCESS_MEMORY_READ_BIT;
}

VkPipelineStageFlags DestinationStages(BufferUploadClass usage, bool geometry, bool compute) {
    switch (usage) {
    case BufferUploadClass::Vertex:
    case BufferUploadClass::Index: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    case BufferUploadClass::Uniform:
    case BufferUploadClass::Storage:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
               (compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VkPipelineStageFlags()) |
               (geometry ? VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT : VkPipelineStageFlags());
    case BufferUploadClass::Transfer: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_TRANSFER_BIT;
}

class BufferPage {
public:
    static auto Create(const Device& device, VkDeviceSize size)
        -> Result<Arc<BufferPage>, BackendError> {
        auto page        = Arc<BufferPage>::make(device, size);
        auto initialized = page->Initialize();
        if (initialized.is_err()) return Err(initialized.unwrap_err());
        return Ok(rstd::move(page));
    }

    bool TryAllocate(VkDeviceSize size, VkDeviceSize alignment, alloc::RangeId& allocation,
                     VkDeviceSize& offset) {
        auto result = m_ranges.allocate(size, alignment);
        if (result.is_err()) return false;
        const auto range = result.unwrap_unchecked();
        allocation       = range.id;
        offset           = range.offset;
        return true;
    }

    void Release(alloc::RangeId allocation) { (void)m_ranges.deallocate(allocation); }

    VkBuffer     handle() const noexcept { return m_buffer.handle.handle(); }
    VkDevice     device() const noexcept { return *m_device.handle(); }
    VkDeviceSize size() const noexcept { return m_size; }
    bool         empty() const noexcept { return m_ranges.counters().allocation_count == 0; }

    BufferPage(const Device& device, VkDeviceSize size)
        : m_device(device), m_size(size), m_ranges(size) {}

private:
    auto Initialize() -> Result<rstd::empty, BackendError> {
        VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = m_size,
            .usage = DestinationUsage(),
        };
        auto buffer = m_device.memory_allocator().create_buffer(buffer_info);
        if (buffer.is_err())
            return Err(BackendError { "allocate buffer page"_Str,
                                      BackendCause::Memory(buffer.unwrap_err()) });
        m_buffer.req_size = m_size;
        m_buffer.handle   = buffer.unwrap_unchecked();
        return Ok(rstd::empty {});
    }

    const Device&             m_device;
    VkDeviceSize              m_size { 0 };
    AllocatedBufferParameters m_buffer;
    alloc::RangeAllocator<>   m_ranges;
};

class UploadBlock {
public:
    static auto Create(const Device& device, VkDeviceSize size)
        -> Result<Arc<UploadBlock>, BackendError> {
        auto block       = Arc<UploadBlock>::make(device, size);
        auto initialized = block->Initialize();
        if (initialized.is_err()) return Err(initialized.unwrap_err());
        return Ok(rstd::move(block));
    }

    void Reset() {
        m_cursor        = 0;
        m_touched_begin = m_size;
        m_touched_end   = 0;
    }

    bool TryAllocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
        auto aligned = AlignUp(m_cursor, alignment);
        if (aligned > m_size || size > m_size - aligned) return false;
        offset   = aligned;
        m_cursor = aligned + size;
        return true;
    }

    void Write(VkDeviceSize offset, slice<rstd::uint8_t> data) {
        auto* bytes = static_cast<rstd::uint8_t*>(m_mapping->data());
        rstd::mem::memcpy(bytes + offset, data.as_raw_ptr(), data.len());
        m_touched_begin = min(m_touched_begin, offset);
        m_touched_end =
            max(m_touched_end, offset + static_cast<VkDeviceSize>(data.len().to_primitive()));
    }

    auto Flush() const -> Result<rstd::empty, BackendError> {
        if (m_touched_end <= m_touched_begin) return Ok(rstd::empty {});
        auto flushed =
            m_buffer.handle.allocation().flush(m_touched_begin, m_touched_end - m_touched_begin);
        if (flushed.is_err())
            return Err(BackendError { "flush upload buffer"_Str,
                                      BackendCause::Memory(flushed.unwrap_err()) });
        return Ok(rstd::empty {});
    }

    VkBuffer     handle() const noexcept { return m_buffer.handle.handle(); }
    VkDeviceSize size() const noexcept { return m_size; }

    UploadBlock(const Device& device, VkDeviceSize size): m_device(device), m_size(size) {}

private:
    auto Initialize() -> Result<rstd::empty, BackendError> {
        VkBufferCreateInfo info { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size  = m_size,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        auto               allocated =
            m_device.memory_allocator().create_buffer(info, vvk::MemoryRequest::Upload());
        if (allocated.is_err())
            return Err(BackendError { "allocate staging buffer"_Str,
                                      BackendCause::Memory(allocated.unwrap_err()) });
        m_buffer.req_size = m_size;
        m_buffer.handle   = allocated.unwrap();
        auto mapped       = m_buffer.handle.allocation().map();
        if (mapped.is_err())
            return Err(BackendError { "map upload buffer"_Str,
                                      BackendCause::Memory(mapped.unwrap_err()) });
        m_mapping = Some(mapped.unwrap_unchecked());
        Reset();
        return Ok(rstd::empty {});
    }

    const Device&              m_device;
    VkDeviceSize               m_size { 0 };
    AllocatedBufferParameters  m_buffer;
    Option<vvk::MemoryMapping> m_mapping;
    VkDeviceSize               m_cursor { 0 };
    VkDeviceSize               m_touched_begin { 0 };
    VkDeviceSize               m_touched_end { 0 };
};

struct BufferCopyOperation {
    BufferAllocation  destination_lease;
    Arc<UploadBlock>  source;
    VkBuffer          destination { VK_NULL_HANDLE };
    VkDeviceSize      source_offset { 0 };
    VkDeviceSize      destination_offset { 0 };
    VkDeviceSize      size { 0 };
    BufferUploadClass usage { BufferUploadClass::Vertex };
};

struct ImageRegionOperation {
    VkImage           destination;
    VkBufferImageCopy region;
    Arc<UploadBlock>  source;
};

} // namespace

struct BufferAllocation::State {
    rstd::uint64_t          identity {};
    Option<Arc<BufferPage>> page;
    alloc::RangeId          allocation {};
    VkDeviceSize            offset { 0 };
    VkDeviceSize            size { 0 };
    BufferUploadClass       usage { BufferUploadClass::Vertex };

    ~State() {
        if (page) (*page)->Release(allocation);
    }
};

struct RecordedBufferUploads::State {
    BufferManager*           owner {};
    u64                      serial { 0 };
    bool                     reserved { false }, recorded { false };
    Vec<BufferCopyOperation> copies;
    Vec<Arc<UploadBlock>>    blocks;
    Vec<BufferUploadTicket>  tickets;
};

struct RecordedImageUploads::State {
    u64                       serial { 0 };
    bool                      recorded { false };
    Vec<ImageRegionOperation> regions;
    usize                     recorded_regions {};
    Vec<Arc<UploadBlock>>     blocks;
    Vec<ImageUploadTicket>    tickets;
};

struct BufferManager::Impl {
    explicit Impl(const Device& value): device(value) {}

    const Device&                             device;
    bool                                      initialized { false };
    u64                                       next_ticket { 0 };
    u64                                       next_batch { 0 };
    Vec<Arc<BufferPage>>                      pages;
    Vec<Arc<UploadBlock>>                     upload_blocks;
    Option<Arc<RecordedBufferUploads::State>> pending;
    BufferUploadStatistics                    statistics;
};

struct ImageUploadManager::Impl {
    explicit Impl(const Device& value): device(value) {}

    const Device&                            device;
    bool                                     initialized { false };
    u64                                      next_ticket { 0 };
    u64                                      next_batch { 0 };
    Vec<Arc<UploadBlock>>                    upload_blocks;
    Option<Arc<RecordedImageUploads::State>> pending;
    auto AllocateUpload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset)
        -> Result<Arc<UploadBlock>, BackendError> {
        Option<Arc<UploadBlock>> block;
        for (const auto& candidate : (*pending)->blocks) {
            if (candidate->TryAllocate(size, alignment, offset)) return Ok(candidate.clone());
        }
        for (const auto& candidate : upload_blocks) {
            if (candidate.strong_count() != usize(1) || candidate->size() < size) continue;
            candidate->Reset();
            if (candidate->TryAllocate(size, alignment, offset)) {
                block = Some(candidate.clone());
                break;
            }
        }
        if (! block) {
            auto created = UploadBlock::Create(
                device,
                size > kUploadBlockSize ? AlignUp(size, kLargeAlignment) : kUploadBlockSize);
            if (created.is_err()) return Err(created.unwrap_err());
            block = Some(created.unwrap());
            if (! (*block)->TryAllocate(size, alignment, offset))
                return Err(
                    BackendError { "allocate image upload"_Str, BackendCause::Capacity(size) });
            upload_blocks.push(block->clone());
        }
        (*pending)->blocks.push(block->clone());
        return Ok(block.take().unwrap());
    }
};

BufferAllocation::BufferAllocation()             = default;
RecordedBufferUploads::RecordedBufferUploads()   = default;
RecordedImageUploads::RecordedImageUploads()     = default;
BufferUploadBatchLease::BufferUploadBatchLease() = default;
ImageUploadBatchLease::ImageUploadBatchLease()   = default;

BufferAllocation::BufferAllocation(Arc<State> state): m_state(Some(rstd::move(state))) {}
BufferAllocation::~BufferAllocation() = default;
BufferAllocation::BufferAllocation(BufferAllocation&& other) noexcept
    : m_state(other.m_state.take()) {}
BufferAllocation& BufferAllocation::operator=(BufferAllocation&& other) noexcept {
    if (this != &other) m_state = other.m_state.take();
    return *this;
}

BufferAllocation::operator bool() const noexcept {
    return m_state && (*m_state)->page && (*m_state)->size > 0;
}

VkBuffer BufferAllocation::buffer() const noexcept {
    return m_state && (*m_state)->page ? (*(*m_state)->page)->handle() : VK_NULL_HANDLE;
}

VkDeviceSize BufferAllocation::offset() const noexcept { return m_state ? (*m_state)->offset : 0; }

VkDeviceSize   BufferAllocation::size() const noexcept { return m_state ? (*m_state)->size : 0; }
rstd::uint64_t BufferAllocation::identity() const noexcept {
    return m_state ? (*m_state)->identity : 0;
}
VkDevice BufferAllocation::device() const noexcept {
    return m_state && (*m_state)->page ? (*(*m_state)->page)->device() : VK_NULL_HANDLE;
}
auto BufferAllocation::clone() const -> BufferAllocation {
    return m_state ? BufferAllocation(m_state->clone()) : BufferAllocation {};
}

RecordedBufferUploads::RecordedBufferUploads(BufferManager* owner, Arc<State> state)
    : m_owner(owner), m_state(Some(rstd::move(state))) {}

RecordedBufferUploads::~RecordedBufferUploads() { Reset(); }

RecordedBufferUploads::RecordedBufferUploads(RecordedBufferUploads&& other) noexcept
    : m_owner(other.m_owner), m_state(other.m_state.take()) {
    other.m_owner = nullptr;
}

RecordedBufferUploads& RecordedBufferUploads::operator=(RecordedBufferUploads&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_owner       = other.m_owner;
    m_state       = other.m_state.take();
    other.m_owner = nullptr;
    return *this;
}

bool RecordedBufferUploads::Valid() const noexcept { return m_owner != nullptr && m_state; }

void RecordedBufferUploads::Reset() {
    if (m_owner && m_state) m_owner->CancelRecordedUploads(*m_state);
    m_owner = nullptr;
    m_state = None();
}

BufferUploadBatchLease::BufferUploadBatchLease(Arc<RecordedBufferUploads::State> state)
    : m_state(Some(rstd::move(state))) {}

BufferUploadBatchLease::~BufferUploadBatchLease() = default;
BufferUploadBatchLease::BufferUploadBatchLease(BufferUploadBatchLease&& other) noexcept
    : m_state(other.m_state.take()) {}

BufferUploadBatchLease& BufferUploadBatchLease::operator=(BufferUploadBatchLease&& other) noexcept {
    if (this != &other) m_state = other.m_state.take();
    return *this;
}

bool BufferUploadBatchLease::Valid() const noexcept { return static_cast<bool>(m_state); }

slice<BufferUploadTicket> BufferUploadBatchLease::Tickets() const noexcept {
    if (! m_state) return slice<BufferUploadTicket>();
    return (*m_state)->tickets.as_slice();
}

BufferManager::BufferManager(const Device& device): m_impl(Box<Impl>::make(device)) {}
BufferManager::~BufferManager() { destroy(); }

bool BufferManager::init() {
    m_impl->initialized = true;
    return true;
}

void BufferManager::destroy() {
    if (! m_impl->initialized) return;
    m_impl->pending = None();
    m_impl->upload_blocks.clear();
    m_impl->pages.clear();
    m_impl->initialized = false;
}

Option<BufferAllocation> BufferManager::Allocate(const BufferAllocationRequest& request) {
    return TryAllocate(request).ok();
}
auto BufferManager::TryAllocate(const BufferAllocationRequest& request)
    -> Result<BufferAllocation, BackendError> {
    if (! m_impl->initialized)
        return Err(BackendError { "allocate buffer"_Str, BackendCause::Closed() });
    if (! request.size)
        return Err(BackendError { "allocate buffer"_Str, BackendCause::Invalid("zero size"_Str) });

    VkDeviceSize alignment = max<VkDeviceSize>(request.alignment, 4);
    if (request.usage == BufferUploadClass::Uniform) {
        alignment = max(alignment, m_impl->device.limits().minUniformBufferOffsetAlignment);
    } else if (request.usage == BufferUploadClass::Storage) {
        alignment = max(alignment, m_impl->device.limits().minStorageBufferOffsetAlignment);
    }

    alloc::RangeId          allocation {};
    VkDeviceSize            offset { 0 };
    Option<Arc<BufferPage>> page;
    for (const auto& candidate : m_impl->pages) {
        if (candidate->TryAllocate(request.size, alignment, allocation, offset)) {
            page = Some(candidate.clone());
            break;
        }
    }
    if (! page) {
        const auto page_size = request.size > kBufferPageSize
                                   ? AlignUp(request.size, kLargeAlignment)
                                   : kBufferPageSize;
        auto       created   = BufferPage::Create(m_impl->device, page_size);
        if (created.is_err()) return Err(created.unwrap_err());
        page = Some(created.unwrap());
        if (! (*page)->TryAllocate(request.size, alignment, allocation, offset))
            return Err(
                BackendError { "allocate buffer range"_Str, BackendCause::Capacity(request.size) });
        m_impl->pages.push(page->clone());
        rstd_info(
            "new destination buffer page, size: {}, pages: {}", page_size, m_impl->pages.len());
    }

    auto                  state = Arc<BufferAllocation::State>::make();
    static rstd::uint64_t next_identity { 1 };
    state->identity = __atomic_fetch_add(&next_identity, rstd::uint64_t(1), __ATOMIC_RELAXED);
    if (! state->identity) rstd::panic("buffer identity exhausted");
    state->page       = rstd::move(page);
    state->allocation = allocation;
    state->offset     = offset;
    state->size       = request.size;
    state->usage      = request.usage;
    return Ok(BufferAllocation(rstd::move(state)));
}

Option<BufferUploadTicket> BufferManager::QueueWrite(BufferAllocation&    allocation,
                                                     slice<rstd::uint8_t> data,
                                                     VkDeviceSize         offset) {
    return TryQueueWrite(allocation, data, offset).ok();
}
auto BufferManager::TryQueueWrite(BufferAllocation& allocation, slice<rstd::uint8_t> data,
                                  VkDeviceSize destination_offset)
    -> Result<BufferUploadTicket, BackendError> {
    if (! m_impl->initialized)
        return Err(BackendError { "queue buffer upload"_Str, BackendCause::Closed() });
    if (! allocation)
        return Err(BackendError { "queue buffer upload"_Str,
                                  BackendCause::Invalid("empty allocation"_Str) });
    if (destination_offset > allocation.size() ||
        data.len().to_primitive() > allocation.size() - destination_offset)
        return Err(BackendError { "queue buffer upload"_Str,
                                  BackendCause::Range(destination_offset,
                                                      data.len().to_primitive(),
                                                      allocation.size()) });
    if (data.is_empty()) return Ok(BufferUploadTicket {});
    if (m_impl->pending && (*m_impl->pending)->reserved)
        return Err(BackendError { "queue buffer upload"_Str, BackendCause::Busy() });
    if (! m_impl->pending) {
        ++m_impl->next_batch;
        if (m_impl->next_batch == u64()) ++m_impl->next_batch;
        m_impl->pending            = Some(Arc<RecordedBufferUploads::State>::make());
        (*m_impl->pending)->serial = m_impl->next_batch;
        (*m_impl->pending)->owner  = this;
    }

    VkDeviceSize             upload_offset { 0 };
    Option<Arc<UploadBlock>> block;
    for (const auto& candidate : (*m_impl->pending)->blocks) {
        if (candidate->TryAllocate(
                static_cast<VkDeviceSize>(data.len().to_primitive()), 4, upload_offset)) {
            block = Some(candidate.clone());
            break;
        }
    }
    if (! block) {
        const auto required = static_cast<VkDeviceSize>(data.len().to_primitive());
        for (const auto& candidate : m_impl->upload_blocks) {
            if (candidate.strong_count() != usize(1) || candidate->size() < required) continue;
            candidate->Reset();
            if (candidate->TryAllocate(required, 4, upload_offset)) {
                block = Some(candidate.clone());
                break;
            }
        }
        if (! block) {
            const auto block_size =
                required > kUploadBlockSize ? AlignUp(required, kLargeAlignment) : kUploadBlockSize;
            auto created = UploadBlock::Create(m_impl->device, block_size);
            if (created.is_err()) return Err(created.unwrap_err());
            block = Some(created.unwrap());
            if (! (*block)->TryAllocate(required, 4, upload_offset))
                return Err(
                    BackendError { "allocate upload range"_Str, BackendCause::Capacity(required) });
            m_impl->upload_blocks.push(block->clone());
            rstd_info("new upload buffer block, size: {}, blocks: {}",
                      block_size,
                      m_impl->upload_blocks.len());
        }
        (*m_impl->pending)->blocks.push(block->clone());
    }

    (*block)->Write(upload_offset, data);
    ++m_impl->next_ticket;
    if (m_impl->next_ticket == u64()) ++m_impl->next_ticket;
    BufferUploadTicket ticket { .value = m_impl->next_ticket };
    (*m_impl->pending)
        ->copies.push(BufferCopyOperation {
            .destination_lease  = BufferAllocation(allocation.m_state->clone()),
            .source             = block.take().unwrap(),
            .destination        = allocation.buffer(),
            .source_offset      = upload_offset,
            .destination_offset = allocation.offset() + destination_offset,
            .size               = static_cast<VkDeviceSize>(data.len().to_primitive()),
            .usage              = (*allocation.m_state)->usage,
        });
    (*m_impl->pending)->tickets.push(decltype(ticket)(ticket));
    auto category = unsigned((*allocation.m_state)->usage);
    ++m_impl->statistics.queued_copies[category];
    m_impl->statistics.queued_bytes[category] += data.len().to_primitive();
    return Ok(ticket);
}

auto BufferManager::statistics() const -> BufferUploadStatistics { return m_impl->statistics; }
auto BufferAllocation::usage() const noexcept -> BufferUploadClass {
    return m_state ? (*m_state)->usage : BufferUploadClass::Vertex;
}

bool BufferManager::HasPendingUploads() const noexcept {
    return m_impl->pending && ! (*m_impl->pending)->copies.is_empty();
}

auto RecordedBufferUploads::Writes() const -> Vec<BufferUploadWrite> {
    Vec<BufferUploadWrite> pending;
    if (! m_state) return pending;
    for (const auto& copy : (*m_state)->copies)
        pending.push({ copy.destination_lease.clone(),
                       copy.destination_offset - copy.destination_lease.offset(),
                       copy.size });
    rstd::slice_::sort_unstable_by(
        pending.as_mut_slice().as_mut_ref(), [](const auto& lhs, const auto& rhs) {
            return lhs.allocation.identity() != rhs.allocation.identity()
                       ? lhs.allocation.identity() < rhs.allocation.identity()
                       : lhs.offset < rhs.offset;
        });
    Vec<BufferUploadWrite> result;
    for (auto& write : pending) {
        if (auto previous = result.last_mut();
            previous && (*previous)->allocation.identity() == write.allocation.identity() &&
            write.offset <= (*previous)->offset + (*previous)->size) {
            (*previous)->size =
                rstd::cmp::max((*previous)->offset + (*previous)->size, write.offset + write.size) -
                (*previous)->offset;
        } else
            result.push(rstd::move(write));
    }
    return result;
}
bool BufferManager::PreparePendingUploads(RecordedBufferUploads& prepared) {
    return TryPreparePendingUploads(prepared).is_ok();
}
auto BufferManager::TryPreparePendingUploads(RecordedBufferUploads& prepared)
    -> Result<rstd::empty, BackendError> {
    prepared.Reset();
    if (! HasPendingUploads()) return Ok(rstd::empty {});
    if ((*m_impl->pending)->reserved)
        return Err(BackendError { "reserve buffer uploads"_Str, BackendCause::Busy() });
    (*m_impl->pending)->reserved = true;
    prepared                     = RecordedBufferUploads(this, m_impl->pending->clone());
    return Ok(rstd::empty {});
}
bool BufferManager::RecordPendingUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& recorded) {
    return PreparePendingUploads(recorded) && RecordUploads(cmd, recorded);
}
bool BufferManager::RecordUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& prepared) {
    return TryRecordUploads(cmd, prepared).is_ok();
}
auto BufferManager::TryRecordUploads(vvk::CommandBuffer& cmd, RecordedBufferUploads& prepared)
    -> Result<rstd::empty, BackendError> {
    if (! prepared.Valid()) {
        if (! HasPendingUploads()) return Ok(rstd::empty {});
        return Err(BackendError { "record buffer uploads"_Str,
                                  BackendCause::State("uploads not reserved"_Str) });
    }
    if (prepared.m_owner != this || ! m_impl->pending ||
        ! Arc<RecordedBufferUploads::State>::ptr_eq(*prepared.m_state, *m_impl->pending) ||
        ! (*m_impl->pending)->reserved || (*m_impl->pending)->recorded)
        return Err(BackendError { "record buffer uploads"_Str,
                                  BackendCause::State("wrong or already recorded batch"_Str) });
    for (const auto& block : (*m_impl->pending)->blocks) {
        auto flushed = block->Flush();
        if (flushed.is_err()) return flushed;
    }
    for (usize i {}; i < (*m_impl->pending)->copies.len(); ++i) {
        const auto& copy = (*m_impl->pending)->copies[i];
        // Uploads may overwrite ranges read by an earlier submission.
        VkBufferMemoryBarrier overwrite { .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                                           VK_ACCESS_MEMORY_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .buffer              = copy.destination,
                                          .offset              = copy.destination_offset,
                                          .size                = copy.size };
        cmd.PipelineBarrier(
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, overwrite);
        cmd.CopyBuffer(copy.source->handle(),
                       copy.destination,
                       VkBufferCopy {
                           .srcOffset = copy.source_offset,
                           .dstOffset = copy.destination_offset,
                           .size      = copy.size,
                       });
        auto category = unsigned(copy.usage);
        ++m_impl->statistics.recorded_copies[category];
        m_impl->statistics.recorded_bytes[category] += copy.size;
    }
    for (const auto& copy : (*m_impl->pending)->copies) {
        VkBufferMemoryBarrier barrier {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = DestinationAccess(copy.usage),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = copy.destination,
            .offset              = copy.destination_offset,
            .size                = copy.size,
        };
        cmd.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            DestinationStages(copy.usage,
                              m_impl->device.capabilities().geometry_shader,
                              m_impl->device.graphics_queue().flags & VK_QUEUE_COMPUTE_BIT),
            VK_DEPENDENCY_BY_REGION_BIT,
            barrier);
    }
    (*m_impl->pending)->recorded = true;
    return Ok(rstd::empty {});
}

Option<BufferUploadBatchLease>
BufferManager::CommitRecordedUploads(RecordedBufferUploads&& recorded) {
    if (! recorded.Valid() || recorded.m_owner != this || ! m_impl->pending ||
        ! Arc<RecordedBufferUploads::State>::ptr_eq(*recorded.m_state, *m_impl->pending) ||
        ! (*recorded.m_state)->recorded) {
        return None();
    }
    auto state       = recorded.m_state.take().unwrap();
    recorded.m_owner = nullptr;
    state->reserved = state->recorded = false;
    m_impl->pending                   = None();
    return Some(BufferUploadBatchLease(rstd::move(state)));
}

bool BufferManager::RestoreUploads(BufferUploadBatchLease uploads) {
    if (! m_impl->initialized || ! uploads.m_state || (*uploads.m_state)->owner != this ||
        (m_impl->pending && (*m_impl->pending)->reserved))
        return false;
    auto state = uploads.m_state.take().unwrap();
    if (! m_impl->pending) {
        m_impl->pending = Some(rstd::move(state));
        return true;
    }
    Vec<BufferCopyOperation> copies;
    for (auto& copy : state->copies) copies.push(rstd::move(copy));
    for (auto& copy : (*m_impl->pending)->copies) copies.push(rstd::move(copy));
    (*m_impl->pending)->copies = rstd::move(copies);
    for (auto& block : state->blocks) (*m_impl->pending)->blocks.push(rstd::move(block));
    for (auto ticket : state->tickets) (*m_impl->pending)->tickets.push(BufferUploadTicket(ticket));
    return true;
}

void BufferManager::CancelRecordedUploads(const Arc<RecordedBufferUploads::State>& state) {
    if (m_impl->pending && Arc<RecordedBufferUploads::State>::ptr_eq(state, *m_impl->pending))
        state->reserved = state->recorded = false;
}

void BufferManager::Trim() {
    bool kept_empty_page = false;
    m_impl->pages.retain([&](const auto& page) {
        if (! page->empty()) return true;
        if (! kept_empty_page) {
            kept_empty_page = true;
            return true;
        }
        return page.strong_count() != usize(1);
    });

    bool kept_upload_block = false;
    m_impl->upload_blocks.retain([&](const auto& block) {
        if (block.strong_count() != usize(1)) return true;
        if (! kept_upload_block) {
            kept_upload_block = true;
            return true;
        }
        return false;
    });
}

RecordedImageUploads::RecordedImageUploads(ImageUploadManager* owner, Arc<State> state)
    : m_owner(owner), m_state(Some(rstd::move(state))) {}

RecordedImageUploads::~RecordedImageUploads() { Reset(); }

RecordedImageUploads::RecordedImageUploads(RecordedImageUploads&& other) noexcept
    : m_owner(other.m_owner), m_state(other.m_state.take()) {
    other.m_owner = nullptr;
}

RecordedImageUploads& RecordedImageUploads::operator=(RecordedImageUploads&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_owner       = other.m_owner;
    m_state       = other.m_state.take();
    other.m_owner = nullptr;
    return *this;
}

bool RecordedImageUploads::Valid() const noexcept { return m_owner != nullptr && m_state; }

void RecordedImageUploads::Reset() {
    if (m_owner && m_state) m_owner->CancelRecordedUploads(*m_state);
    m_owner = nullptr;
    m_state = None();
}

ImageUploadBatchLease::ImageUploadBatchLease(Arc<RecordedImageUploads::State> state)
    : m_state(Some(rstd::move(state))) {}

ImageUploadBatchLease::~ImageUploadBatchLease() = default;
ImageUploadBatchLease::ImageUploadBatchLease(ImageUploadBatchLease&& other) noexcept
    : m_state(other.m_state.take()) {}

ImageUploadBatchLease& ImageUploadBatchLease::operator=(ImageUploadBatchLease&& other) noexcept {
    if (this != &other) m_state = other.m_state.take();
    return *this;
}

bool ImageUploadBatchLease::Valid() const noexcept { return static_cast<bool>(m_state); }

slice<ImageUploadTicket> ImageUploadBatchLease::Tickets() const noexcept {
    if (! m_state) return {};
    return (*m_state)->tickets.as_slice();
}

ImageUploadManager::ImageUploadManager(const Device& device): m_impl(Box<Impl>::make(device)) {}
ImageUploadManager::~ImageUploadManager() { destroy(); }

bool ImageUploadManager::init() {
    m_impl->initialized = true;
    return true;
}

void ImageUploadManager::destroy() {
    if (! m_impl->initialized) return;
    m_impl->pending = None();
    m_impl->upload_blocks.clear();
    m_impl->initialized = false;
}

Result<ImageUploadTicket, BackendError>
ImageUploadManager::QueueRegion(VkImage image, VkBufferImageCopy region, slice<rstd::uint8_t> bytes,
                                rstd::uint32_t block_bytes) {
    if (! m_impl->initialized)
        return Err(BackendError { "queue image upload"_Str, BackendCause::Closed() });
    if (! block_bytes || ! image || bytes.is_empty())
        return Err(BackendError { "queue image upload"_Str,
                                  BackendCause::Invalid("empty image upload"_Str) });
    if (! m_impl->pending) {
        ++m_impl->next_batch;
        m_impl->pending            = Some(Arc<RecordedImageUploads::State>::make());
        (*m_impl->pending)->serial = m_impl->next_batch;
    }
    const auto   size = VkDeviceSize(bytes.len().to_primitive());
    VkDeviceSize offset {};
    VkDeviceSize gcd = 256, divisor = block_bytes;
    while (divisor) {
        auto remainder = gcd % divisor;
        gcd            = divisor;
        divisor        = remainder;
    }
    auto block = m_impl->AllocateUpload(size, 256 / gcd * block_bytes, offset);
    if (block.is_err()) return Err(block.unwrap_err());
    (*block)->Write(offset, bytes);
    region.bufferOffset = offset;
    (*m_impl->pending)->regions.push(ImageRegionOperation { image, region, block.unwrap() });
    ImageUploadTicket ticket { .value = ++m_impl->next_ticket };
    (*m_impl->pending)->tickets.push(decltype(ticket)(ticket));
    return Ok(ticket);
}

bool ImageUploadManager::HasPendingUploads() const noexcept {
    return m_impl->pending && ! (*m_impl->pending)->regions.is_empty();
}

auto ImageUploadManager::RecordQueuedRegions(vvk::CommandBuffer&   command,
                                             RecordedImageUploads& recorded)
    -> Result<rstd::empty, BackendError> {
    if (! m_impl->pending || (*m_impl->pending)->regions.is_empty()) return Ok(rstd::empty {});
    auto& state = **m_impl->pending;
    if (recorded.Valid() && (recorded.m_owner != this || ! Arc<RecordedImageUploads::State>::ptr_eq(
                                                             *recorded.m_state, *m_impl->pending)))
        return Err(BackendError { "record image upload"_Str,
                                  BackendCause::State("foreign upload batch"_Str) });
    for (const auto& block : state.blocks) {
        auto flushed = block->Flush();
        if (flushed.is_err()) return Err(flushed.unwrap_err());
    }
    for (usize index = state.recorded_regions; index < state.regions.len(); ++index) {
        auto& copy = state.regions[index];
        command.CopyBufferToImage(copy.source->handle(),
                                  copy.destination,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  copy.region);
    }
    state.recorded_regions = state.regions.len();
    state.recorded         = true;
    if (! recorded.Valid()) recorded = RecordedImageUploads(this, m_impl->pending->clone());
    return Ok(rstd::empty {});
}

auto ImageUploadManager::CommitRecordedUploads(RecordedImageUploads&& recorded)
    -> Option<ImageUploadBatchLease> {
    if (! recorded.Valid() || recorded.m_owner != this || ! m_impl->pending ||
        ! Arc<RecordedImageUploads::State>::ptr_eq(*recorded.m_state, *m_impl->pending) ||
        ! (*recorded.m_state)->recorded) {
        return None();
    }
    auto state       = recorded.m_state.take().unwrap();
    recorded.m_owner = nullptr;
    state->recorded  = false;
    m_impl->pending  = None();
    return Some(ImageUploadBatchLease(rstd::move(state)));
}

void ImageUploadManager::CancelRecordedUploads(const Arc<RecordedImageUploads::State>& state) {
    if (m_impl->pending && Arc<RecordedImageUploads::State>::ptr_eq(state, *m_impl->pending)) {
        state->recorded         = false;
        state->recorded_regions = usize();
    }
}

void ImageUploadManager::DiscardPendingUploads() { m_impl->pending = None(); }

void ImageUploadManager::Trim() {
    bool kept_upload_block = false;
    m_impl->upload_blocks.retain([&](const auto& block) {
        if (block.strong_count() != usize(1)) return true;
        if (! kept_upload_block) {
            kept_upload_block = true;
            return true;
        }
        return false;
    });
}

} // namespace vrento::vulkan
