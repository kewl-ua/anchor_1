#pragma once

#include <cstdint>

namespace engine {

// Deterministic PRNG (PCG32, pcg-random.org).
//
// Never use the C library generator or the standard library distributions in
// the simulation: their output is implementation-defined and differs between
// standard libraries.
class Rng {
public:
    explicit constexpr Rng(uint64_t seed) {
        next_u32();
        state_ += seed;
        next_u32();
    }

    constexpr uint32_t next_u32() {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + kIncrement;
        const auto xorshifted = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
        const auto rot = static_cast<uint32_t>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
    }

    // Uniform in [0, bound), without modulo bias.
    constexpr uint32_t next_below(uint32_t bound) {
        const uint32_t threshold = (UINT32_MAX - bound + 1) % bound;
        for (;;) {
            const uint32_t r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }

    // Uniform in [lo, hi].
    constexpr int32_t next_range(int32_t lo, int32_t hi) {
        return lo + static_cast<int32_t>(next_below(static_cast<uint32_t>(hi - lo + 1)));
    }

    constexpr uint64_t state() const { return state_; }

private:
    static constexpr uint64_t kIncrement = 1442695040888963407ULL;
    uint64_t state_ = 0;
};

}  // namespace engine
