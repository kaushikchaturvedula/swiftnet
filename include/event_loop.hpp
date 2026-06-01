#pragma once
#include "detail/os_backend.hpp"
#include <cstdint>

namespace swiftnet
{

    // A single readiness/completion event reported by the reactor.
    //
    // `token` is an opaque value supplied at arm()/arm_timer() time and handed
    // straight back here. SwiftNet uses the suspended coroutine handle's address
    // as the token, so the reactor can resume the right virtual thread with no
    // side table. A token of 0 is reserved for the internal wake() nudge.
    struct io_event
    {
        std::uint64_t token = 0;
        std::uint32_t mask = 0; // combination of event_mask values
        int res = 0;            // result hint (bytes for io_uring/IOCP; data for kqueue)
    };

    enum event_mask : std::uint32_t
    {
        READABLE = 1u << 0,
        WRITABLE = 1u << 1
    };

    // Normalize a libc poll() bitset (POLLIN / POLLOUT) into the canonical
    // event_mask. This is the ONE place POLL* constants are interpreted, so the
    // rest of the runtime only ever deals in READABLE/WRITABLE.
    constexpr std::uint32_t mask_from_poll(unsigned poll_bits) noexcept
    {
        std::uint32_t m = 0;
        if (poll_bits & 0x001u) // POLLIN
            m |= READABLE;
        if (poll_bits & 0x004u) // POLLOUT
            m |= WRITABLE;
        return m;
    }

    // Cross-platform reactor: kqueue (macOS), io_uring (Linux), IOCP (Windows).
    //
    // Everything is one-shot: arm() registers a watch that fires exactly once and
    // is then removed by the kernel. To wait on the same fd again, re-arm. This
    // matches the "co_await arms once, resume once" model of the virtual-thread
    // runtime and removes the need for an explicit del() after every completion.
    class event_loop
    {
    public:
        event_loop();
        ~event_loop();

        event_loop(const event_loop &) = delete;
        event_loop &operator=(const event_loop &) = delete;

        // Arm a one-shot readiness watch on `fd` for `mask`, tagged with `token`.
        void arm(int fd, std::uint32_t mask, std::uint64_t token);

        // Arm a one-shot timer firing after `ms` milliseconds, tagged `token`.
        void arm_timer(std::uint64_t token, int ms);

        // Best-effort disarm of an outstanding readiness watch (e.g. on close).
        void cancel(int fd, std::uint32_t mask);

        // Block until >=1 event is ready or `timeout_ms` elapses (<0 = forever).
        // Fills up to `max` io_event entries; returns the count (0 on timeout).
        int wait(io_event *ev, int max, int timeout_ms);

        // Unblock a concurrent wait() (used to stop the reactor on shutdown).
        void wake();

    private:
#if defined(SWIFTNET_BACKEND_IOURING)
        struct io_uring *ring_;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
        int kq_;
#elif defined(SWIFTNET_BACKEND_IOCP)
        void *iocp_;
#endif
    };

}
