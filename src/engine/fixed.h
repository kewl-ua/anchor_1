#pragma once

#include <compare>
#include <cstdint>

namespace engine {

// Q16.16 fixed-point number.
//
// The simulation never uses floating-point: its results can differ between
// compilers, CPUs and optimization flags, and a single differing bit desyncs
// a lockstep game. Integer math is bit-identical everywhere.
//
// Range is about +-32767 with a precision of 1/65536, which is plenty for
// world coordinates measured in pixels.
struct Fixed {
    static constexpr int kFracBits = 16;
    static constexpr int32_t kOneRaw = 1 << kFracBits;

    int32_t raw = 0;

    static constexpr Fixed from_raw(int32_t r) {
        Fixed f;
        f.raw = r;
        return f;
    }
    static constexpr Fixed from_int(int32_t i) { return from_raw(i * kOneRaw); }
    // from_ratio(3, 2) == 1.5
    static constexpr Fixed from_ratio(int32_t num, int32_t den) {
        return from_raw(static_cast<int32_t>((static_cast<int64_t>(num) << kFracBits) / den));
    }

    // Rounds toward negative infinity.
    constexpr int32_t to_int() const { return raw >> kFracBits; }

    friend constexpr Fixed operator+(Fixed a, Fixed b) { return from_raw(a.raw + b.raw); }
    friend constexpr Fixed operator-(Fixed a, Fixed b) { return from_raw(a.raw - b.raw); }
    friend constexpr Fixed operator-(Fixed a) { return from_raw(-a.raw); }
    friend constexpr Fixed operator*(Fixed a, Fixed b) {
        return from_raw(static_cast<int32_t>((static_cast<int64_t>(a.raw) * b.raw) >> kFracBits));
    }
    friend constexpr Fixed operator/(Fixed a, Fixed b) {
        return from_raw(static_cast<int32_t>((static_cast<int64_t>(a.raw) << kFracBits) / b.raw));
    }
    friend constexpr Fixed operator*(Fixed a, int32_t k) { return from_raw(a.raw * k); }
    friend constexpr Fixed operator/(Fixed a, int32_t k) { return from_raw(a.raw / k); }

    constexpr Fixed& operator+=(Fixed o) { raw += o.raw; return *this; }
    constexpr Fixed& operator-=(Fixed o) { raw -= o.raw; return *this; }

    constexpr auto operator<=>(const Fixed&) const = default;
};

constexpr Fixed min(Fixed a, Fixed b) { return a < b ? a : b; }
constexpr Fixed max(Fixed a, Fixed b) { return a < b ? b : a; }
constexpr Fixed clamp(Fixed v, Fixed lo, Fixed hi) { return max(lo, min(v, hi)); }

// Integer square root, exact floor(sqrt(n)).
constexpr uint64_t isqrt(uint64_t n) {
    uint64_t result = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= result + bit) {
            n -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

struct FixedVec2 {
    Fixed x;
    Fixed y;

    friend constexpr FixedVec2 operator+(FixedVec2 a, FixedVec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr FixedVec2 operator-(FixedVec2 a, FixedVec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr FixedVec2 operator*(FixedVec2 v, Fixed s) { return {v.x * s, v.y * s}; }
    constexpr FixedVec2& operator+=(FixedVec2 o) { x += o.x; y += o.y; return *this; }
    constexpr FixedVec2& operator-=(FixedVec2 o) { x -= o.x; y -= o.y; return *this; }

    constexpr bool operator==(const FixedVec2&) const = default;

    // Squared length in raw units (Q32.32). Use it for distance comparisons:
    // it cannot overflow, unlike squaring a Fixed.
    constexpr uint64_t length_sq_raw() const {
        const int64_t rx = x.raw;
        const int64_t ry = y.raw;
        return static_cast<uint64_t>(rx * rx) + static_cast<uint64_t>(ry * ry);
    }

    // sqrt(raw_x^2 + raw_y^2) is already the length in raw Q16.16 units.
    constexpr Fixed length() const {
        return Fixed::from_raw(static_cast<int32_t>(isqrt(length_sq_raw())));
    }
};

constexpr uint64_t square_raw(Fixed f) {
    const int64_t r = f.raw;
    return static_cast<uint64_t>(r * r);
}

}  // namespace engine
