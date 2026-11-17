export module vrento:vulkan.upload_plan;

import :vulkan.api;
import rstd;

using namespace rstd::prelude;

export namespace vrento::vulkan
{
// One buffer upload of a batch: `size` bytes from `source` (VK_NULL_HANDLE: zeros) into `destination`.
struct UploadCopy {
    VkBuffer     source { VK_NULL_HANDLE };
    VkBuffer     destination { VK_NULL_HANDLE };
    VkDeviceSize source_offset { 0 };
    VkDeviceSize destination_offset { 0 };
    VkDeviceSize size { 0 };
};

// A command recording a batch issues: copies [first, first + count) of the batch between the same two buffers as
// one vkCmdCopyBuffer, one zero fill, or a transfer-to-transfer barrier.
struct UploadCommand {
    enum class Kind { Copy, Fill, Barrier };
    Kind     kind { Kind::Copy };
    VkBuffer source { VK_NULL_HANDLE };
    VkBuffer destination { VK_NULL_HANDLE };
    usize    first {};
    usize    count {};
};

// The commands of a batch, in its order: consecutive copies between the same two buffers share one command, and a
// copy that writes bytes an earlier copy of the batch wrote (since the last barrier) waits behind a barrier. Each
// upload used to be recorded as a barrier, a copy and a barrier of its own: on Intel GPUs every barrier drains the
// pipeline, so hundreds of uniform uploads a frame cost milliseconds of GPU time.
inline auto PlanUploadCommands(slice<UploadCopy> copies) -> Vec<UploadCommand> {
    struct Written {
        VkBuffer     buffer;
        VkDeviceSize begin, end;
    };
    Vec<UploadCommand> commands;
    Vec<Written>       written;
    for (usize i {}; i < copies.len(); ++i) {
        const auto&        copy = copies[i];
        const VkDeviceSize end  = copy.destination_offset + copy.size;
        bool               overlaps { false };
        for (const auto& w : written)
            if (w.buffer == copy.destination && copy.destination_offset < w.end && w.begin < end) {
                overlaps = true;
                break;
            }
        if (overlaps) {
            commands.push({ .kind = UploadCommand::Kind::Barrier });
            written.clear();
        }
        auto last = commands.last_mut();
        if (copy.source != VK_NULL_HANDLE && last && (*last)->kind == UploadCommand::Kind::Copy &&
            (*last)->source == copy.source && (*last)->destination == copy.destination &&
            (*last)->first + (*last)->count == i) {
            ++(*last)->count;
        } else {
            commands.push({ .kind        = copy.source != VK_NULL_HANDLE ? UploadCommand::Kind::Copy
                                                                         : UploadCommand::Kind::Fill,
                            .source      = copy.source,
                            .destination = copy.destination,
                            .first       = i,
                            .count       = usize(1) });
        }
        written.push({ copy.destination, copy.destination_offset, end });
    }
    return commands;
}
// Adds a copy region to a command's list, extending the last region when the new one continues it in both buffers
// (uploads are packed in the order they are queued, so a frame's uniform blocks mostly form a few long runs): the
// driver copies each region as a separate blit.
inline void AppendUploadRegion(Vec<VkBufferCopy>& regions, VkBufferCopy region) {
    auto last = regions.last_mut();
    if (last && (*last)->srcOffset + (*last)->size == region.srcOffset &&
        (*last)->dstOffset + (*last)->size == region.dstOffset) {
        (*last)->size += region.size;
        return;
    }
    regions.push(rstd::move(region));
}
} // namespace vrento::vulkan
