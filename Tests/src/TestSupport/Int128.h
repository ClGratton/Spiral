#pragma once

#include <cmath>
#include <cstdint>

namespace Spiral::Tests
{
    // Portable signed 128-bit integer for exact test oracles (MSVC has no
    // __int128). Two's complement, wraps like unsigned arithmetic on overflow;
    // the oracles stay far below 2^126. Cross-checked against the native
    // __int128 on compilers that have one.
    struct Int128
    {
        std::uint64_t Lo = 0;
        std::uint64_t Hi = 0;

        Int128() = default;
        Int128(std::int64_t value)
            : Lo(static_cast<std::uint64_t>(value))
            , Hi(value < 0 ? ~0ull : 0ull)
        {
        }

        static Int128 FromParts(std::uint64_t hi, std::uint64_t lo)
        {
            Int128 result;
            result.Hi = hi;
            result.Lo = lo;
            return result;
        }

        bool IsNegative() const { return (Hi >> 63) != 0; }
        bool IsZero() const { return Hi == 0 && Lo == 0; }
    };

    inline bool operator==(const Int128& a, const Int128& b) { return a.Hi == b.Hi && a.Lo == b.Lo; }
    inline bool operator!=(const Int128& a, const Int128& b) { return !(a == b); }

    inline bool operator<(const Int128& a, const Int128& b)
    {
        if (a.IsNegative() != b.IsNegative())
            return a.IsNegative();
        return a.Hi != b.Hi ? a.Hi < b.Hi : a.Lo < b.Lo;
    }

    inline bool operator>(const Int128& a, const Int128& b) { return b < a; }
    inline bool operator<=(const Int128& a, const Int128& b) { return !(b < a); }
    inline bool operator>=(const Int128& a, const Int128& b) { return !(a < b); }

    inline Int128 operator+(const Int128& a, const Int128& b)
    {
        const std::uint64_t lo = a.Lo + b.Lo;
        return Int128::FromParts(a.Hi + b.Hi + (lo < a.Lo ? 1 : 0), lo);
    }

    inline Int128 operator-(const Int128& a)
    {
        return Int128::FromParts(~a.Hi, ~a.Lo) + Int128(1);
    }

    inline Int128 operator-(const Int128& a, const Int128& b) { return a + (-b); }

    inline Int128 operator<<(const Int128& a, int shift)
    {
        if (shift == 0)
            return a;
        if (shift >= 64)
            return Int128::FromParts(a.Lo << (shift - 64), 0);
        return Int128::FromParts((a.Hi << shift) | (a.Lo >> (64 - shift)), a.Lo << shift);
    }

    inline Int128 operator>>(const Int128& a, int shift)
    {
        if (shift == 0)
            return a;
        if (shift >= 64)
            return Int128::FromParts(0, a.Hi >> (shift - 64));
        return Int128::FromParts(a.Hi >> shift, (a.Lo >> shift) | (a.Hi << (64 - shift)));
    }

    inline Int128 operator*(const Int128& a, const Int128& b)
    {
        const auto mulWide = [](std::uint64_t x, std::uint64_t y, std::uint64_t& high)
        {
            const std::uint64_t x0 = x & 0xffffffffull;
            const std::uint64_t x1 = x >> 32;
            const std::uint64_t y0 = y & 0xffffffffull;
            const std::uint64_t y1 = y >> 32;
            const std::uint64_t p00 = x0 * y0;
            const std::uint64_t p01 = x0 * y1;
            const std::uint64_t p10 = x1 * y0;
            const std::uint64_t p11 = x1 * y1;
            const std::uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
            high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
            return (middle << 32) | (p00 & 0xffffffffull);
        };

        std::uint64_t high = 0;
        const std::uint64_t low = mulWide(a.Lo, b.Lo, high);
        return Int128::FromParts(high + a.Lo * b.Hi + a.Hi * b.Lo, low);
    }

    // floor(numerator / denominator) for denominator > 0.
    inline Int128 FloorDiv(const Int128& numerator, const Int128& denominator)
    {
        const bool negative = numerator.IsNegative();
        const Int128 magnitude = negative ? -numerator : numerator;
        Int128 quotient;
        Int128 remainder;
        for (int bit = 127; bit >= 0; --bit)
        {
            remainder = remainder << 1;
            if (((bit >= 64 ? magnitude.Hi >> (bit - 64) : magnitude.Lo >> bit) & 1ull) != 0)
                remainder = remainder + Int128(1);
            quotient = quotient << 1;
            if (remainder >= denominator)
            {
                remainder = remainder - denominator;
                quotient = quotient + Int128(1);
            }
        }

        if (!negative)
            return quotient;
        return remainder.IsZero() ? -quotient : -quotient - Int128(1);
    }

    inline bool IsDivisible(const Int128& value, const Int128& divisor)
    {
        return FloorDiv(value, divisor) * divisor == value;
    }

    inline bool FitsInt64(const Int128& value)
    {
        return value >= Int128(INT64_MIN) && value <= Int128(INT64_MAX);
    }

    inline std::int64_t ToInt64(const Int128& value)
    {
        return static_cast<std::int64_t>(value.Lo);
    }

    inline long double ToLongDouble(const Int128& value)
    {
        const bool negative = value.IsNegative();
        const Int128 magnitude = negative ? -value : value;
        const long double result = std::ldexp(static_cast<long double>(magnitude.Hi), 64) + static_cast<long double>(magnitude.Lo);
        return negative ? -result : result;
    }
}
