export module vrento:resource_registry.decode_budget;
import rstd;

using namespace rstd::prelude;

// Texture decodes are admitted by the bytes they will hold, not only by worker count: four 4k textures decoding at once
// (and their finished results waiting to be uploaded) were most of a large scene's load-time memory peak.
export namespace vrento::resource_registry
{
class DecodeBudget {
    u64 limit_;
    u64 in_flight_ {};
    u64 running_ {};

public:
    explicit DecodeBudget(u64 limit): limit_(limit) {}
    // true (and the bytes counted) when they fit, or when nothing else is decoding: one texture always goes through
    bool Admit(u64 bytes) {
        if (running_ != u64() && in_flight_ + bytes > limit_) return false;
        in_flight_ += bytes;
        running_ += u64(1);
        return true;
    }
    void Release(u64 bytes) {
        in_flight_ = in_flight_ > bytes ? in_flight_ - bytes : u64();
        if (running_ != u64()) running_ -= u64(1);
    }
    auto InFlight() const -> u64 { return in_flight_; }
    // decoded bytes of a width x height texture with `bytes_per_pixel` and `mips` levels; 0 extent = unknown, 4 MiB
    static auto Estimate(rstd::int32_t width, rstd::int32_t height, rstd::uint32_t bytes_per_pixel,
                         rstd::uint32_t mips) -> u64 {
        if (width <= 0 || height <= 0) return u64(4ull << 20);
        u64 base = u64(static_cast<rstd::uint64_t>(width) * static_cast<rstd::uint64_t>(height) *
                       bytes_per_pixel);
        return mips > 1 ? base + base / u64(3) : base;
    }
};
} // namespace vrento::resource_registry
