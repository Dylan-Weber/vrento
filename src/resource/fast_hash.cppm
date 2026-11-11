export module vrento:resource.fast_hash;
import rstd;

using namespace rstd::prelude;
using rstd::collections::HashMap;

export namespace vrento
{
// A cheap hasher for the maps a frame builds and discards (bindings, validation): keys are small
// integer tuples, never attacker controlled, and the SipHash of the default state dominated the
// per-frame rebuild of a scene with hundreds of draws. Words are folded multiplicatively and the
// result is finalized so both the low and the high bits are usable by the table.
class FastHasher {
    rstd::uint64_t state_ {};

public:
    FastHasher() noexcept = default;
    void write(slice<u8> bytes) noexcept {
        const auto*    p = reinterpret_cast<const unsigned char*>(bytes.as_ptr().as_raw_ptr());
        rstd::size_t   n = bytes.len().to_primitive();
        constexpr auto k = 0x9e3779b97f4a7c15ULL;
        while (n >= 8) {
            rstd::uint64_t word;
            __builtin_memcpy(&word, p, 8);
            state_ = (state_ ^ word) * k;
            state_ ^= state_ >> 29;
            p += 8;
            n -= 8;
        }
        if (n >= 4) {
            rstd::uint32_t word;
            __builtin_memcpy(&word, p, 4);
            state_ = (state_ ^ word) * k;
            state_ ^= state_ >> 29;
            p += 4;
            n -= 4;
        }
        for (; n; --n, ++p) {
            state_ = (state_ ^ *p) * k;
            state_ ^= state_ >> 29;
        }
    }
    auto finish() const noexcept -> u64 {
        auto h = state_;
        h ^= h >> 32;
        h *= 0xd6e8feb86659fd93ULL;
        h ^= h >> 32;
        return u64(h);
    }
};
using FastBuildHasher = rstd::hash::BuildHasherDefault<FastHasher>;
template<typename K, typename V>
using FastMap = HashMap<K, V, FastBuildHasher>;
template<typename K>
using FastSet = rstd::collections::HashSet<K, FastBuildHasher>;
} // namespace vrento
