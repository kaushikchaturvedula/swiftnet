#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "detail/runtime_detect.hpp"

using namespace swiftnet::detail;

// Build synthetic facts for a machine class so select() can be tested without
// touching the real host (the rules, not the gathering).
static platform_facts facts(os_kind os, const char *arch, bool iouring,
                            bool avx2, bool sse2, bool affinity)
{
    platform_facts f;
    f.os = os;
    f.arch = arch;
    f.iouring_available = iouring;
    f.has_avx2 = avx2;
    f.has_sse2 = sse2;
    f.affinity_works = affinity;
    f.logical_cores = 8;
    f.physical_cores = 8;
    return f;
}

TEST_CASE("select(): backend per OS + io_uring probe")
{
    CHECK(select(facts(os_kind::linux_, "x86_64", true, true, true, true)).backend == event_backend::io_uring);
    // io_uring probe fails (old kernel / seccomp / container) -> epoll fallback
    CHECK(select(facts(os_kind::linux_, "x86_64", false, true, true, true)).backend == event_backend::epoll);
    CHECK(select(facts(os_kind::linux_, "arm64", true, false, false, true)).backend == event_backend::io_uring);
    CHECK(select(facts(os_kind::macos, "arm64", false, false, false, false)).backend == event_backend::kqueue);
    CHECK(select(facts(os_kind::windows, "x86_64", false, true, true, true)).backend == event_backend::iocp);
    // fail-safe to epoll when inconclusive
    CHECK(select(facts(os_kind::other, "unknown", false, false, false, false)).backend == event_backend::epoll);
}

TEST_CASE("select(): SIMD path per arch/features")
{
    CHECK(select(facts(os_kind::macos, "arm64", false, false, false, false)).simd == simd_level::neon);
    CHECK(select(facts(os_kind::linux_, "aarch64", true, false, false, true)).simd == simd_level::neon);
    CHECK(select(facts(os_kind::linux_, "x86_64", true, true, true, true)).simd == simd_level::avx2);
    CHECK(select(facts(os_kind::linux_, "x86_64", true, false, true, true)).simd == simd_level::sse2);
    CHECK(select(facts(os_kind::linux_, "x86_64", true, false, false, true)).simd == simd_level::scalar);
}

TEST_CASE("select(): pinning never on Apple Silicon; OS-gated elsewhere")
{
    // macOS: never, even if affinity_works were somehow true
    CHECK(select(facts(os_kind::macos, "arm64", false, false, false, true)).pinning == false);
    // Linux: yes when affinity probe succeeds, no when denied (cgroup/container)
    CHECK(select(facts(os_kind::linux_, "x86_64", true, true, true, true)).pinning == true);
    CHECK(select(facts(os_kind::linux_, "x86_64", true, true, true, false)).pinning == false);
    CHECK(select(facts(os_kind::windows, "x86_64", false, true, true, true)).pinning == true);
    CHECK(select(facts(os_kind::other, "unknown", false, false, false, true)).pinning == false);
}

TEST_CASE("detect_runtime(): self-consistent on this host")
{
    runtime_info ri = detect_runtime();
    CHECK(ri.logical_cores >= 1);
    CHECK(ri.physical_cores >= 1);
    // unverified iff the backend is not the measured kqueue path
    CHECK(ri.unverified == (ri.backend != event_backend::kqueue));
    // backend must match the host OS
    if (ri.os == os_kind::macos)
    {
        CHECK(ri.backend == event_backend::kqueue);
        CHECK(ri.pinning_supported == false); // Apple: KERN_NOT_SUPPORTED
    }
    else if (ri.os == os_kind::linux_)
    {
        CHECK((ri.backend == event_backend::io_uring || ri.backend == event_backend::epoll));
    }
    // arm64 hosts must select NEON
    if (ri.arch.find("arm") != std::string::npos)
        CHECK(ri.simd == simd_level::neon);
    // cached_runtime returns the same selection
    CHECK(cached_runtime().backend == ri.backend);
}
