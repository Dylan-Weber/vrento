export module vrento:fast_hash;
import rstd;

using namespace rstd::prelude;

// A hasher for the per-frame maps (graph bindings, pass resources): the standard one is SipHash fed a byte at a time,
// which at hundreds of lookups per object per frame was 7-10 % of a busy scene's CPU. Keys here are small integers
// and handles, so a multiply-and-fold over 8-byte words is enough (no untrusted keys: no need for DoS resistance).
export namespace vrento
{
class FastHasher {
    rstd::uint64_t state_ { 0 };

    void add(rstd::uint64_t word) noexcept {
        state_ = (((state_ << 5) | (state_ >> 59)) ^ word) * 0x517cc1b727220a95ull;
    }

public:
    void write(slice<u8> bytes) noexcept {
        auto const*      data      = bytes.as_raw_ptr();
        rstd::size_t     remaining = bytes.len().to_primitive();
        while (remaining >= 8) {
            rstd::uint64_t word;
            __builtin_memcpy(&word, data, 8);
            add(word);
            data += 8;
            remaining -= 8;
        }
        if (remaining) {
            rstd::uint64_t word = 0;
            __builtin_memcpy(&word, data, remaining);
            add(word ^ (static_cast<rstd::uint64_t>(remaining) << 56));
        }
    }
    auto finish() const noexcept -> u64 {
        // the multiply leaves the low bits weak and the table reads both ends: fold the high half down
        auto h = state_;
        h ^= h >> 32;
        h *= 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
        return u64(h);
    }
};
using FastHashState = rstd::hash::BuildHasherDefault<FastHasher>;
template<typename K, typename V>
using FastMap = rstd::collections::HashMap<K, V, FastHashState>;
} // namespace vrento
