#include "detail/runtime_detect.hpp"
#include "detail/log.hpp"

#include <cstdio>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(SWIFTNET_HAS_LIBURING)
#include <liburing.h>
#endif
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace swiftnet::detail
{
    namespace
    {
#if defined(__APPLE__)
        unsigned sysctl_uint(const char *name) noexcept
        {
            unsigned v = 0;
            std::size_t sz = sizeof(v);
            if (sysctlbyname(name, &v, &sz, nullptr, 0) != 0)
                return 0;
            return v;
        }
#endif
#if defined(__linux__)
        bool probe_affinity() noexcept
        {
            cpu_set_t cur;
            CPU_ZERO(&cur);
            if (sched_getaffinity(0, sizeof(cur), &cur) != 0)
                return false;
            return sched_setaffinity(0, sizeof(cur), &cur) == 0; // set-to-same probe
        }
        bool probe_iouring() noexcept
        {
#if defined(SWIFTNET_HAS_LIBURING)
            struct io_uring ring;
            if (io_uring_queue_init(8, &ring, 0) != 0)
                return false; // old kernel / seccomp / container denial
            io_uring_queue_exit(&ring);
            return true;
#else
            return false;
#endif
        }
#endif
    } // namespace

    selection select(const platform_facts &f) noexcept
    {
        selection s;
        // Backend: universally-best for the detected OS; fail-safe to epoll.
        switch (f.os)
        {
        case os_kind::linux_:
            s.backend = f.iouring_available ? event_backend::io_uring : event_backend::epoll;
            break;
        case os_kind::macos:
            s.backend = event_backend::kqueue;
            break;
        case os_kind::windows:
            s.backend = event_backend::iocp;
            break;
        default:
            s.backend = event_backend::epoll;
            break;
        }
        // SIMD: NEON is baseline on arm64; on x86 pick the best available; else scalar.
        if (f.arch.find("arm") != std::string::npos || f.arch.find("aarch64") != std::string::npos)
            s.simd = simd_level::neon;
        else if (f.has_avx2)
            s.simd = simd_level::avx2;
        else if (f.has_sse2)
            s.simd = simd_level::sse2;
        else
            s.simd = simd_level::scalar;
        // Pinning: real on Linux/Windows when the OS allows it; never on Apple Silicon.
        s.pinning = (f.os == os_kind::linux_ || f.os == os_kind::windows) && f.affinity_works;
        return s;
    }

    platform_facts gather_facts() noexcept
    {
        platform_facts f;

#if defined(__aarch64__) || defined(_M_ARM64)
        f.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        f.arch = "x86_64";
#else
        f.arch = "unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
        __builtin_cpu_init();
        f.has_avx2 = __builtin_cpu_supports("avx2");
        f.has_sse2 = __builtin_cpu_supports("sse2");
#endif

#if defined(__APPLE__)
        f.os = os_kind::macos;
        struct utsname u{};
        if (uname(&u) == 0)
            f.os_version = u.release;
        f.logical_cores = sysctl_uint("hw.logicalcpu");
        if (f.logical_cores == 0)
            f.logical_cores = sysctl_uint("hw.ncpu");
        f.physical_cores = sysctl_uint("hw.physicalcpu");
        if (f.physical_cores == 0)
            f.physical_cores = f.logical_cores;
        f.perf_cores = sysctl_uint("hw.perflevel0.logicalcpu"); // P-cores (0 on Intel)
        f.eff_cores = sysctl_uint("hw.perflevel1.logicalcpu");  // E-cores
        f.affinity_works = false; // Apple Silicon: KERN_NOT_SUPPORTED
        f.iouring_available = false;
#elif defined(__linux__)
        f.os = os_kind::linux_;
        struct utsname u{};
        if (uname(&u) == 0)
        {
            f.os_version = u.release;
            std::sscanf(u.release, "%d.%d", &f.kernel_major, &f.kernel_minor);
        }
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        f.logical_cores = (n > 0) ? static_cast<unsigned>(n) : 1u;
        f.physical_cores = f.logical_cores; // distinct-core count not parsed; conservative
        f.affinity_works = probe_affinity();
        f.iouring_available = probe_iouring();
#elif defined(_WIN32)
        f.os = os_kind::windows;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        f.logical_cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
        f.physical_cores = f.logical_cores;
        f.affinity_works = true; // SetThreadAffinityMask is generally available
        f.iouring_available = false;
        f.os_version = "windows";
#else
        f.os = os_kind::other;
        f.logical_cores = 1;
        f.physical_cores = 1;
#endif
        if (f.logical_cores == 0)
            f.logical_cores = 1;
        return f;
    }

    runtime_info detect_runtime() noexcept
    {
        platform_facts f = gather_facts();
        selection s = select(f);
        runtime_info ri;
        ri.os = f.os;
        ri.os_version = f.os_version;
        ri.kernel_major = f.kernel_major;
        ri.kernel_minor = f.kernel_minor;
        ri.arch = f.arch;
        ri.logical_cores = f.logical_cores;
        ri.physical_cores = f.physical_cores;
        ri.perf_cores = f.perf_cores;
        ri.eff_cores = f.eff_cores;
        ri.backend = s.backend;
        ri.simd = s.simd;
        ri.pinning_supported = s.pinning;
        ri.iouring_available = f.iouring_available;
        ri.unverified = (s.backend != event_backend::kqueue);
        return ri;
    }

    const runtime_info &cached_runtime() noexcept
    {
        static const runtime_info ri = detect_runtime();
        return ri;
    }

    const char *backend_name(event_backend b) noexcept
    {
        switch (b)
        {
        case event_backend::kqueue: return "kqueue";
        case event_backend::io_uring: return "io_uring";
        case event_backend::epoll: return "epoll";
        case event_backend::iocp: return "IOCP";
        }
        return "?";
    }
    const char *simd_name(simd_level s) noexcept
    {
        switch (s)
        {
        case simd_level::scalar: return "scalar";
        case simd_level::sse2: return "SSE2";
        case simd_level::avx2: return "AVX2";
        case simd_level::neon: return "NEON";
        }
        return "?";
    }
    const char *os_name(os_kind o) noexcept
    {
        switch (o)
        {
        case os_kind::linux_: return "Linux";
        case os_kind::macos: return "macOS";
        case os_kind::windows: return "Windows";
        case os_kind::other: return "other";
        }
        return "?";
    }

    void log_runtime(const runtime_info &ri) noexcept
    {
        SWIFTNET_LOG_INFO("SwiftNet runtime: os={} ({}) arch={} cores={} logical/{} physical",
                          os_name(ri.os), ri.os_version.empty() ? "?" : ri.os_version, ri.arch,
                          ri.logical_cores, ri.physical_cores);
        if (ri.perf_cores > 0)
            SWIFTNET_LOG_INFO("  topology: P-cores={} E-cores={}", ri.perf_cores, ri.eff_cores);
        SWIFTNET_LOG_INFO("  backend: {} [{}]   simd: {}",
                          backend_name(ri.backend), ri.unverified ? "UNVERIFIED" : "VERIFIED",
                          simd_name(ri.simd));
        if (ri.pinning_supported)
            SWIFTNET_LOG_INFO("  pinning: ENABLED");
        else if (ri.os == os_kind::macos)
            SWIFTNET_LOG_INFO("  pinning: DISABLED (macOS: KERN_NOT_SUPPORTED)");
        else
            SWIFTNET_LOG_INFO("  pinning: DISABLED (not permitted by OS/cgroup)");
    }

} // namespace swiftnet::detail
