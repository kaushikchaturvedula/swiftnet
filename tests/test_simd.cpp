#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "detail/simd.hpp"
#include "detail/runtime_detect.hpp"

#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// Differential / fuzz test for the SIMD header scans. The scalar implementation is
// the reference ORACLE; this proves the ACTIVE vectorized variant (NEON here on the
// M1; AVX2/SSE2 on x86 when the CPU supports them) returns the EXACT same index as
// the oracle on a large random input space plus the classic edge cases. It does not
// "review" the code -- it executes both and compares.

using namespace swiftnet::detail;

namespace
{
    // Independent naive oracle (byte-by-byte; equality is signedness-safe).
    std::size_t oracle_find_char(const char *p, std::size_t n, char c)
    {
        for (std::size_t i = 0; i < n; ++i)
            if (p[i] == c)
                return i;
        return simd::npos;
    }
    std::size_t oracle_find_pat(const char *p, std::size_t n, const char *pat, std::size_t pl)
    {
        if (pl == 0 || pl > n)
            return simd::npos;
        for (std::size_t i = 0; i + pl <= n; ++i)
        {
            std::size_t k = 0;
            for (; k < pl; ++k)
                if (p[i + k] != pat[k])
                    break;
            if (k == pl)
                return i;
        }
        return simd::npos;
    }

    std::string hexdump(const char *p, std::size_t n)
    {
        std::string s;
        char b[4];
        for (std::size_t i = 0; i < n; ++i)
        {
            std::snprintf(b, sizeof b, "%02x ", (unsigned)(unsigned char)p[i]);
            s += b;
        }
        return s;
    }
    std::string idx(std::size_t v) { return v == simd::npos ? std::string("npos") : std::to_string(v); }

    // Compare every variant + composite scan to the oracle for one window.
    // Returns "" if all agree, else a detailed report (with the failing input).
    std::string diff_check(const char *p, std::size_t n)
    {
        // needles: common, the delimiters, high-bit, 0xFF, embedded NUL, ascii
        static const char needles[] = {'a', '\r', '\n', ':', ' ', 'Z',
                                       (char)0x00, (char)0x80, (char)0xFF};
        const auto fail = [&](const char *fn, char c, std::size_t got, std::size_t exp) {
            std::ostringstream s;
            s << fn << " mismatch  n=" << n << "  needle=0x" << std::hex
              << (unsigned)(unsigned char)c << std::dec << "  got=" << idx(got)
              << "  expected=" << idx(exp) << "\n  buf=[" << hexdump(p, n) << "]";
            return s.str();
        };
        const auto failp = [&](const char *fn, std::size_t got, std::size_t exp) {
            std::ostringstream s;
            s << fn << " mismatch  n=" << n << "  got=" << idx(got) << "  expected=" << idx(exp)
              << "\n  buf=[" << hexdump(p, n) << "]";
            return s.str();
        };

        for (char c : needles)
        {
            const std::size_t o = oracle_find_char(p, n, c);
            if (simd::find_char(p, n, c) != o)
                return fail("find_char(dispatch)", c, simd::find_char(p, n, c), o);
            if (simd::find_char_scalar(p, n, c) != o)
                return fail("find_char_scalar", c, simd::find_char_scalar(p, n, c), o);
#if defined(__aarch64__)
            if (simd::find_char_neon(p, n, c) != o)
                return fail("find_char_neon", c, simd::find_char_neon(p, n, c), o);
#elif defined(__x86_64__) || defined(_M_X64)
            // SSE2 is baseline on x86-64 (always safe to call); AVX2 only if detected.
            if (simd::find_char_sse2(p, n, c) != o)
                return fail("find_char_sse2", c, simd::find_char_sse2(p, n, c), o);
            if (cached_runtime().simd == simd_level::avx2 && simd::find_char_avx2(p, n, c) != o)
                return fail("find_char_avx2", c, simd::find_char_avx2(p, n, c), o);
#endif
        }
        const std::size_t oc = oracle_find_pat(p, n, "\r\n", 2);
        if (simd::find_crlf(p, n) != oc)
            return failp("find_crlf", simd::find_crlf(p, n), oc);
        const std::size_t od = oracle_find_pat(p, n, "\r\n\r\n", 4);
        if (simd::find_double_crlf(p, n) != od)
            return failp("find_double_crlf", simd::find_double_crlf(p, n), od);
        return {};
    }
}

TEST_CASE("simd: active variant is exercised on this host")
{
    MESSAGE("detected SIMD path = " << std::string(simd_name(cached_runtime().simd)));
#if defined(__aarch64__)
    CHECK(cached_runtime().simd == simd_level::neon); // NEON is what gets tested below on the M1
#endif
}

TEST_CASE("simd: differential fuzz vs scalar oracle (delimiter-rich + full byte range)")
{
    std::mt19937 rng(0xC0FFEEu); // fixed seed -> deterministic + reproducible failures
    std::vector<char> base(512);

    // Two alphabets: (A) delimiter-rich so \r\n and \r\n\r\n land at random positions
    // and boundaries; (B) full 0..255 to exercise high-bit / NUL / non-ASCII bytes.
    const char rich[] = {'\r', '\n', '\r', '\n', 'a', 'b', ' ', ':', 'Z',
                         (char)0x00, (char)0x80, (char)0xFF};
    std::uniform_int_distribution<int> pickRich(0, (int)sizeof(rich) - 1);
    std::uniform_int_distribution<int> anyByte(0, 255);
    std::uniform_int_distribution<int> sizeD(0, 300);
    std::uniform_int_distribution<int> alignD(0, 63);

    std::size_t inputs = 0;
    std::string firstFail;
    const int ITERS = 100000;
    for (int phase = 0; phase < 2 && firstFail.empty(); ++phase)
    {
        for (int it = 0; it < ITERS; ++it)
        {
            std::size_t n = (std::size_t)sizeD(rng);
            std::size_t off = (std::size_t)alignD(rng);
            if (off + n > base.size())
                n = base.size() - off;
            for (std::size_t i = 0; i < n; ++i)
                base[off + i] = phase == 0 ? rich[pickRich(rng)] : (char)anyByte(rng);
            ++inputs;
            std::string r = diff_check(base.data() + off, n);
            if (!r.empty())
            {
                firstFail = r;
                break;
            }
        }
    }
    INFO(firstFail);
    CHECK(firstFail.empty());
    MESSAGE("fuzz inputs checked = " << inputs << " (all variants vs oracle)");
}

TEST_CASE("simd: explicit edge cases")
{
    auto run = [](std::vector<char> v) {
        std::string r = diff_check(v.data(), v.size());
        INFO(r);
        CHECK(r.empty());
    };
    auto buf = [](std::size_t n, char fill = 'a') { return std::vector<char>(n, fill); };

    // empty buffer
    run({});
    // buffers shorter than one NEON register (1..15), exactly 16, 17, and tails
    for (std::size_t n = 1; n <= 40; ++n)
        run(buf(n));

    // CRLF straddling the 16-byte (NEON) boundary: \r at 15, \n at 16
    { auto v = buf(40); v[15] = '\r'; v[16] = '\n'; run(v); }
    // CRLF straddling the 32-byte (AVX2) boundary: \r at 31, \n at 32
    { auto v = buf(48); v[31] = '\r'; v[32] = '\n'; run(v); }
    // \r\n\r\n straddling the 16-byte boundary (starts at 14: bytes 14,15,16,17)
    { auto v = buf(40); v[14]='\r'; v[15]='\n'; v[16]='\r'; v[17]='\n'; run(v); }
    // \r\n\r\n straddling the 32-byte boundary (starts at 30)
    { auto v = buf(48); v[30]='\r'; v[31]='\n'; v[32]='\r'; v[33]='\n'; run(v); }

    // delimiter at the very start
    { auto v = buf(20); v[0]='\r'; v[1]='\n'; run(v); }
    // delimiter at the very end
    { auto v = buf(20); v[18]='\r'; v[19]='\n'; run(v); }
    // one byte before the end (so the trailing '\n' is the last byte; and a \r as the
    // last byte with no room for \n)
    { auto v = buf(20); v[17]='\r'; v[18]='\n'; run(v); }
    { auto v = buf(20); v[19]='\r'; run(v); }                 // lone \r at end -> no CRLF
    { auto v = buf(33); v[32]='\r'; run(v); }                 // lone \r at end past a full register

    // lone \r (no following \n) and lone \n -> must NOT false-match find_crlf
    { auto v = buf(20); v[5]='\r'; run(v); }
    { auto v = buf(20); v[5]='\n'; run(v); }
    { auto v = buf(20); v[5]='\r'; v[7]='\n'; run(v); }       // \r and \n but not adjacent
    // \n\r (reversed) must not match \r\n
    { auto v = buf(20); v[5]='\n'; v[6]='\r'; run(v); }

    // no delimiter at all (sentinel agreement) at several sizes
    for (std::size_t n : {0u, 1u, 15u, 16u, 17u, 64u, 200u})
        run(buf(n, 'x'));

    // multiple delimiters -> must return the FIRST
    { auto v = buf(64); v[10]='\r'; v[11]='\n'; v[40]='\r'; v[41]='\n'; run(v); }
    { auto v = buf(64); v[6]='\r'; v[7]='\n'; v[8]='\r'; v[9]='\n';      // double-crlf then more
      v[50]='\r'; v[51]='\n'; v[52]='\r'; v[53]='\n'; run(v); }

    // high-bit / non-ASCII / embedded NUL filler with delimiters interleaved
    { auto v = buf(48, (char)0x80); v[20]='\r'; v[21]='\n'; v[22]='\r'; v[23]='\n'; run(v); }
    { auto v = buf(48, (char)0xFF); v[33]='\r'; v[34]='\n'; run(v); }
    { auto v = buf(48, (char)0x00); v[16]='\r'; v[17]='\n'; run(v); } // NUL filler
    // a buffer that is ALL high-bit bytes (no delimiter) -> npos, no false match
    run(buf(50, (char)0x80));
}
