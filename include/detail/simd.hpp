#pragma once
// SIMD-accelerated byte scans for the HTTP parser hot path (finding CRLFs, the
// header terminator, spaces and colons). Auto-dispatched per the detected arch:
//   arm64  -> NEON (always present; the path validated natively on Apple Silicon)
//   x86_64 -> AVX2 or SSE2 chosen at runtime from runtime_info (compiled with a
//             target attribute so it builds without -mavx2), else scalar
//   other  -> scalar
//
// HONESTY: byte-scanning is a small fraction of SwiftNet's per-request CPU (the
// profile in BENCHMARKS.md shows ~82% of on-CPU time is socket syscalls and only
// ~1.4% is parse/route/string work). This is correctness/portability completeness
// and a lever for CPU-bound deployments -- NOT a measured-throughput win on the
// loopback benchmark. The alignment/tail parity tests (tests/test_simd.cpp) are
// the real deliverable. find_crlf/find_double_crlf are built on the vectorized
// find_char (scan for '\r', verify the following bytes) -- simple and correct.

#include "detail/runtime_detect.hpp"
#include <cstddef>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace swiftnet::detail::simd
{
    inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // ---- scalar (always available; also the parity reference) ----
    inline std::size_t find_char_scalar(const char *p, std::size_t n, char c) noexcept
    {
        const void *r = std::memchr(p, static_cast<unsigned char>(c), n);
        return r ? static_cast<std::size_t>(static_cast<const char *>(r) - p) : npos;
    }

#if defined(__aarch64__)
    inline std::size_t find_char_neon(const char *p, std::size_t n, char c) noexcept
    {
        std::size_t i = 0;
        const uint8x16_t needle = vdupq_n_u8(static_cast<uint8_t>(c));
        for (; i + 16 <= n; i += 16)
        {
            uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t *>(p) + i);
            uint8x16_t cmp = vceqq_u8(chunk, needle);
            if (vmaxvq_u8(cmp)) // any lane matched?
            {
                for (std::size_t j = 0; j < 16; ++j)
                    if (p[i + j] == c)
                        return i + j;
            }
        }
        for (; i < n; ++i)
            if (p[i] == c)
                return i;
        return npos;
    }
#endif

#if defined(__x86_64__) || defined(_M_X64)
    __attribute__((target("avx2"))) inline std::size_t find_char_avx2(const char *p, std::size_t n, char c) noexcept
    {
        std::size_t i = 0;
        const __m256i needle = _mm256_set1_epi8(c);
        for (; i + 32 <= n; i += 32)
        {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
            unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, needle)));
            if (mask)
                return i + static_cast<std::size_t>(__builtin_ctz(mask));
        }
        for (; i < n; ++i)
            if (p[i] == c)
                return i;
        return npos;
    }
    __attribute__((target("sse2"))) inline std::size_t find_char_sse2(const char *p, std::size_t n, char c) noexcept
    {
        std::size_t i = 0;
        const __m128i needle = _mm_set1_epi8(c);
        for (; i + 16 <= n; i += 16)
        {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + i));
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, needle));
            if (mask)
                return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
        }
        for (; i < n; ++i)
            if (p[i] == c)
                return i;
        return npos;
    }
#endif

    // ---- dispatch ----
    inline std::size_t find_char(const char *p, std::size_t n, char c) noexcept
    {
#if defined(__aarch64__)
        return find_char_neon(p, n, c);
#elif defined(__x86_64__) || defined(_M_X64)
        using fn_t = std::size_t (*)(const char *, std::size_t, char) noexcept;
        static const fn_t fn = [] {
            switch (cached_runtime().simd)
            {
            case simd_level::avx2: return &find_char_avx2;
            case simd_level::sse2: return &find_char_sse2;
            default: return &find_char_scalar;
            }
        }();
        return fn(p, n, c);
#else
        return find_char_scalar(p, n, c);
#endif
    }

    // Find "\r\n": scan for '\r' (vectorized), verify the next byte is '\n'.
    inline std::size_t find_crlf(const char *p, std::size_t n) noexcept
    {
        std::size_t pos = 0;
        while (pos < n)
        {
            std::size_t r = find_char(p + pos, n - pos, '\r');
            if (r == npos)
                return npos;
            std::size_t at = pos + r;
            if (at + 1 < n && p[at + 1] == '\n')
                return at;
            pos = at + 1;
        }
        return npos;
    }

    // Find "\r\n\r\n" (the header terminator).
    inline std::size_t find_double_crlf(const char *p, std::size_t n) noexcept
    {
        std::size_t pos = 0;
        while (pos < n)
        {
            std::size_t r = find_char(p + pos, n - pos, '\r');
            if (r == npos)
                return npos;
            std::size_t at = pos + r;
            if (at + 3 < n && p[at + 1] == '\n' && p[at + 2] == '\r' && p[at + 3] == '\n')
                return at;
            pos = at + 1;
        }
        return npos;
    }

} // namespace swiftnet::detail::simd
