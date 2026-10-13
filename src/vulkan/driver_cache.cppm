export module vrento:vulkan.driver_cache;
import rstd;
import :vulkan.api;

export namespace vrento::vulkan
{
using namespace rstd::prelude;
enum class DriverCacheStage
{
    Device,
    Create,
    Query,
    Limit,
    File
};
struct DriverCacheError {
    DriverCacheStage stage;
    VkResult         native { VK_SUCCESS };
};
struct DriverCacheStats {
    bool  loaded {}, rejected {};
    usize bytes {};
};
class DriverPipelineCache {
    Option<rstd::sync::Arc<vvk::PipelineCache>> cache_;
    VkDevice                                    device_ {};
    DriverCacheStats                            stats_;
    static constexpr usize                      limit_ { 64 * 1024 * 1024 };

    static auto word(slice<u8> data, usize offset) -> rstd::uint32_t {
        rstd::uint32_t value {};
        for (usize i {}; i < usize(4); ++i)
            value |= rstd::uint32_t(data[offset + i].to_primitive()) << (i.to_primitive() * 8);
        return value;
    }
    static void append(Vec<u8>& data, rstd::uint32_t value) {
        for (unsigned i {}; i < 4; ++i) data.push(u8((value >> (i * 8)) & 255));
    }
    static auto checksum(slice<u8> data) -> rstd::uint32_t {
        rstd::uint32_t value { 0xffffffffu };
        for (auto byte : data) {
            value ^= byte.to_primitive();
            for (unsigned i {}; i < 8; ++i)
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
        }
        return ~value;
    }
    static auto compatible(slice<u8> encoded, const VkPhysicalDeviceProperties& gpu)
        -> Option<slice<u8>> {
        if (encoded.len() < usize(48) || encoded.len() > limit_) return None();
        if (word(encoded, usize()) != 0x31435056u || word(encoded, usize(4)) != 1u ||
            word(encoded, usize(8)) != encoded.len().to_primitive() - 16)
            return None();
        auto data = slice<u8>::from_raw_parts(encoded.as_raw_ptr() + 16, encoded.len() - usize(16));
        if (word(encoded, usize(12)) != checksum(data) || word(data, usize()) != 32u ||
            word(data, usize(4)) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            word(data, usize(8)) != gpu.vendorID || word(data, usize(12)) != gpu.deviceID)
            return None();
        for (usize i {}; i < usize(VK_UUID_SIZE); ++i)
            if (data[usize(16) + i].to_primitive() != gpu.pipelineCacheUUID[i.to_primitive()])
                return None();
        return Some(data);
    }

public:
    auto initialize(const Device& device, slice<u8> encoded = {})
        -> Result<empty, DriverCacheError> {
        if (! device.handle() || device.lost())
            return Err(DriverCacheError { DriverCacheStage::Device });
        auto             initial = compatible(encoded, device.gpu().GetProperties());
        DriverCacheStats stats { initial.is_some(), ! encoded.is_empty() && initial.is_none() };
        VkPipelineCacheCreateInfo info { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (initial) {
            info.initialDataSize = initial->len().to_primitive();
            info.pInitialData    = initial->as_raw_ptr();
        }
        vvk::PipelineCache created;
        auto result = device.observe_result(device.handle().CreatePipelineCache(info, created));
        if (result != VK_SUCCESS && initial && result != VK_ERROR_DEVICE_LOST) {
            stats.loaded         = false;
            stats.rejected       = true;
            info.initialDataSize = 0;
            info.pInitialData    = nullptr;
            result = device.observe_result(device.handle().CreatePipelineCache(info, created));
        }
        if (result != VK_SUCCESS) return Err(DriverCacheError { DriverCacheStage::Create, result });
        cache_  = Some(rstd::sync::Arc<vvk::PipelineCache>::make(rstd::move(created)));
        device_ = *device.handle();
        stats_  = stats;
        return Ok(empty {});
    }
    auto handle(const Device& device) const -> VkPipelineCache {
        return device_ == *device.handle() && cache_ ? ***cache_ : VK_NULL_HANDLE;
    }
    auto lease(const Device& device) const -> rstd::sync::Arc<vvk::PipelineCache> {
        if (! handle(device)) rstd::panic("pipeline cache device mismatch");
        return cache_->clone();
    }
    auto stats() const -> DriverCacheStats { return stats_; }
    auto encode(const Device& device) -> Result<Vec<u8>, DriverCacheError> {
        if (! handle(device) || device.lost())
            return Err(DriverCacheError { DriverCacheStage::Device });
        for (unsigned attempt {}; attempt < 3; ++attempt) {
            rstd::size_t size {};
            auto         result = device.observe_result(
                device.handle().GetPipelineCacheData(***cache_, &size, nullptr));
            if (result != VK_SUCCESS)
                return Err(DriverCacheError { DriverCacheStage::Query, result });
            if (size < 32 || size > limit_.to_primitive() - 16)
                return Err(DriverCacheError { DriverCacheStage::Limit });
            Vec<u8> data;
            data.resize(usize(size), u8());
            result = device.observe_result(
                device.handle().GetPipelineCacheData(***cache_, &size, data.data()));
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS)
                return Err(DriverCacheError { DriverCacheStage::Query, result });
            data.truncate(usize(size));
            Vec<u8> encoded;
            append(encoded, 0x31435056u);
            append(encoded, 1u);
            append(encoded, rstd::uint32_t(size));
            append(encoded, checksum(data.as_slice()));
            encoded.extend_from_slice(data.as_slice());
            stats_.bytes = encoded.len();
            return Ok(rstd::move(encoded));
        }
        return Err(DriverCacheError { DriverCacheStage::Query, VK_INCOMPLETE });
    }
    auto load(const Device& device, ref<rstd::path::Path> path) -> Result<empty, DriverCacheError> {
        auto file = rstd::fs::File::open(path);
        if (file.is_err()) return Err(DriverCacheError { DriverCacheStage::File });
        auto metadata = file->metadata();
        if (metadata.is_err()) return Err(DriverCacheError { DriverCacheStage::File });
        if (metadata->len() > u64(limit_.to_primitive())) {
            auto result = initialize(device);
            if (result.is_ok()) stats_.rejected = true;
            return result;
        }
        Vec<u8> data;
        data.resize(usize(metadata->len().to_primitive()), u8());
        usize offset {};
        while (offset < data.len()) {
            auto read = file->read(mut_ref<u8[]>::from_raw_parts(
                data.data() + offset.to_primitive(), data.len() - offset));
            if (read.is_err() || *read == usize())
                return Err(DriverCacheError { DriverCacheStage::File });
            offset += *read;
        }
        return initialize(device, data.as_slice());
    }
    auto save(const Device& device, ref<rstd::path::Path> path) -> Result<empty, DriverCacheError> {
        auto encoded = encode(device);
        if (encoded.is_err()) return Err(encoded.unwrap_err());
        if (rstd::fs::write(path, encoded->as_slice()).is_err())
            return Err(DriverCacheError { DriverCacheStage::File });
        return Ok(empty {});
    }
};
} // namespace vrento::vulkan
