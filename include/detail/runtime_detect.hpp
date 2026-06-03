#pragma once
// Runtime platform detection. At startup SwiftNet detects the OS, kernel, arch,
// core topology, and available kernel/CPU features, then EMBEDS the universally-
// best choice for that machine (I/O backend, SIMD path, core-pinning) -- these are
// NOT developer-overridable; they are logged so the behavior is transparent.
//
// Design: fact-gathering (gather_facts(), syscalls/sysctl/probes) is split from a
// PURE selection function (select(), no I/O) so the selection rules are unit-tested
// against synthetic facts (see tests/test_detect.cpp). detect_runtime() combines
// them; cached_runtime() memoizes the result for the process.

#include "detail/backend/iface.hpp"
#include <string>

namespace swiftnet::detail
{

    enum class os_kind { linux_, macos, windows, other };
    enum class event_backend { kqueue, io_uring, epoll, iocp };
    enum class simd_level { scalar, sse2, avx2, neon };

    // Raw, gathered facts about the machine -- no choices made yet.
    struct platform_facts
    {
        os_kind os{os_kind::other};
        std::string os_version;          // uname release / Darwin version
        int kernel_major{0}, kernel_minor{0}; // Linux: io_uring gating
        std::string arch;                // "arm64" | "x86_64" | ...
        unsigned logical_cores{1};
        unsigned physical_cores{1};
        unsigned perf_cores{0};          // Apple P-cores (hw.perflevel0); 0 if N/A
        unsigned eff_cores{0};           // Apple E-cores (hw.perflevel1)
        bool iouring_available{false};   // Linux: actual io_uring_queue_init probe
        bool has_avx2{false};            // x86 cpuid
        bool has_sse2{false};            // x86 cpuid
        bool affinity_works{false};      // sched_setaffinity / SetThreadAffinityMask probe
    };

    // The embedded choices derived from facts (pure function output).
    struct selection
    {
        event_backend backend{event_backend::epoll};
        simd_level simd{simd_level::scalar};
        bool pinning{false};
    };

    // Full detection result: facts + selection, consumed by event_loop / scheduler /
    // config and logged at startup.
    struct runtime_info
    {
        os_kind os{os_kind::other};
        std::string os_version;
        int kernel_major{0}, kernel_minor{0};
        std::string arch;
        unsigned logical_cores{1};
        unsigned physical_cores{1};
        unsigned perf_cores{0};
        unsigned eff_cores{0};
        event_backend backend{event_backend::epoll};
        simd_level simd{simd_level::scalar};
        bool pinning_supported{false};
        bool iouring_available{false};
        bool unverified{true}; // true unless backend == kqueue (the only measured one)
    };

    // PURE: choose backend/simd/pinning from facts. No syscalls; fully testable.
    // Fail-safe: anything inconclusive -> epoll/scalar/no-pin (most portable correct).
    selection select(const platform_facts &f) noexcept;

    // Gather facts about THIS machine (syscalls/sysctl/probes; platform-specific).
    platform_facts gather_facts() noexcept;

    // gather_facts() + select() merged into a runtime_info.
    runtime_info detect_runtime() noexcept;

    // Process-wide memoized detection (function-local static; thread-safe init).
    const runtime_info &cached_runtime() noexcept;

    // Human-readable names for logging.
    const char *backend_name(event_backend) noexcept;
    const char *simd_name(simd_level) noexcept;
    const char *os_name(os_kind) noexcept;

    // Emit the startup runtime banner (os/arch/cores/P-E/backend+VERIFIED tag/simd/
    // pinning+why) via SWIFTNET_LOG_INFO. Called once at scheduler start.
    void log_runtime(const runtime_info &) noexcept;

} // namespace swiftnet::detail
