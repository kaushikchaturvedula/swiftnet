#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "detail/simd.hpp"

#include <cstring>
#include <string_view>
#include <vector>

using namespace swiftnet::detail;

namespace
{
    std::size_t ref_find_char(const char *p, std::size_t n, char c)
    {
        const void *r = std::memchr(p, static_cast<unsigned char>(c), n);
        return r ? static_cast<std::size_t>(static_cast<const char *>(r) - p) : simd::npos;
    }
    std::size_t ref_find(const char *p, std::size_t n, const char *pat, std::size_t pl)
    {
        std::string_view s(p, n);
        auto r = s.find(pat, 0, pl);
        return r == std::string_view::npos ? simd::npos : r;
    }

    // Compare every SIMD scan against its scalar reference for one (ptr,n) window.
    void check_window(const char *p, std::size_t n)
    {
        for (char c : {'x', 'a', '\r', '\n', ':', ' '})
        {
            CHECK(simd::find_char(p, n, c) == ref_find_char(p, n, c));
            CHECK(simd::find_char_scalar(p, n, c) == ref_find_char(p, n, c));
        }
        CHECK(simd::find_crlf(p, n) == ref_find(p, n, "\r\n", 2));
        CHECK(simd::find_double_crlf(p, n) == ref_find(p, n, "\r\n\r\n", 4));
    }
}

TEST_CASE("simd: parity with scalar across sizes and alignments")
{
    // A base buffer with needles sprinkled so that, as the window start alignment
    // and length vary, matches land at the head, the tail, and ACROSS the 16/32-byte
    // vector boundaries (the classic SIMD tail/first-lane bug sites).
    std::vector<char> base(512, 'a');
    base[5] = ':';
    base[9] = ' ';
    base[17] = 'x';
    base[30] = '\r'; base[31] = '\n'; base[32] = '\r'; base[33] = '\n'; // "\r\n\r\n" across the 32 boundary
    base[50] = '\r'; // lone CR (no following LF) -> find_crlf must NOT match here
    base[64] = '\r'; base[65] = '\n'; // CRLF straddling the 64 boundary
    base[200] = 'x';

    const std::size_t sizes[] = {0, 1, 2, 3, 4, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65, 100, 127, 128, 200, 300};
    for (std::size_t align = 0; align < 64; ++align)
        for (std::size_t n : sizes)
            if (align + n <= base.size())
                check_window(base.data() + align, n);
}

TEST_CASE("simd: explicit edge cases")
{
    CHECK(simd::find_char(nullptr, 0, 'x') == simd::npos); // empty
    const char a[] = "x";
    CHECK(simd::find_char(a, 1, 'x') == 0);
    CHECK(simd::find_char(a, 1, 'y') == simd::npos);

    const char crlf_only[] = "\r"; // lone CR, length 1
    CHECK(simd::find_crlf(crlf_only, 1) == simd::npos);

    const char term[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody";
    std::size_t n = sizeof(term) - 1;
    CHECK(simd::find_double_crlf(term, n) == ref_find(term, n, "\r\n\r\n", 4));
    CHECK(simd::find_crlf(term, n) == 14); // end of request line
    CHECK(simd::find_char(term, n, ' ') == 3);
}
