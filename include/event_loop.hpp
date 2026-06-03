#pragma once
#include "detail/backend/iface.hpp"
#include <memory>

namespace swiftnet
{

    // Cross-platform reactor façade. Owns exactly one detail::reactor_backend,
    // chosen at construction: per platform at compile time (kqueue on macOS, IOCP
    // on Windows) and, on Linux, per machine at runtime (io_uring when the kernel
    // actually supports it, else epoll -- see detail/runtime_detect.hpp). The
    // public API below is unchanged from the original single-class event_loop, so
    // io_awaitable and vthread_scheduler are untouched by the backend split.
    //
    // Everything is one-shot: arm() registers a watch that fires exactly once and
    // is then removed by the kernel. To wait on the same fd again, re-arm. This
    // matches the "co_await arms once, resume once" model of the virtual-thread
    // runtime.
    //
    // ============================ BACKEND STATUS ============================
    //  kqueue  (macOS/BSD)  : VERIFIED. Primary, fully exercised target. All
    //                         benchmarks in BENCHMARKS.md were measured here
    //                         (Apple Silicon / arm64).
    //  io_uring (Linux)     : IMPLEMENTED, UNVERIFIED for throughput. Modernized
    //                         (per-engine timers, eventfd wake, SINGLE_ISSUER/
    //                         DEFER_TASKRUN with graceful downgrade, multishot
    //                         accept). Provided-buffers/multishot-recv are gated
    //                         OFF by default. Functionally tested on Linux; no
    //                         speed is claimed.
    //  epoll   (Linux)      : IMPLEMENTED, UNVERIFIED for throughput. EPOLLONESHOT
    //                         fallback used when the io_uring probe fails (old
    //                         kernel / seccomp / container). Functionally tested.
    //  IOCP    (Windows)    : IMPLEMENTED, UNVERIFIED. Real OVERLAPPED completion
    //                         (WSARecv/WSASend). Compiles; not benchmarked.
    //
    // Rule: never report or imply throughput/latency for any backend except kqueue.
    // See BENCHMARKS.md.
    // ========================================================================
    class event_loop
    {
    public:
        event_loop(); // selects the backend from the cached runtime_info
        ~event_loop();

        event_loop(const event_loop &) = delete;
        event_loop &operator=(const event_loop &) = delete;

        void arm(int fd, std::uint32_t mask, std::uint64_t token) { b_->arm(fd, mask, token); }
        void arm_timer(std::uint64_t token, int ms) { b_->arm_timer(token, ms); }
        void cancel(int fd, std::uint32_t mask) { b_->cancel(fd, mask); }
        int wait(io_event *ev, int max, int timeout_ms) { return b_->wait(ev, max, timeout_ms); }
        void wake() { b_->wake(); }

        // Multishot accept if the backend supports it (io_uring); false otherwise.
        bool arm_accept_multishot(int listen_fd, std::uint64_t token)
        {
            return b_->arm_accept_multishot(listen_fd, token);
        }

    private:
        std::unique_ptr<detail::reactor_backend> b_;
    };

}
