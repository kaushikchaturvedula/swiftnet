#pragma once
// Reactor backend interface + vocabulary types.
//
// `event_loop` (include/event_loop.hpp) is a thin façade that owns one
// reactor_backend chosen at construction. Concrete backends (kqueue / io_uring /
// epoll / IOCP) live in include/detail/backend/<name>_backend.{hpp,cpp} and are
// selected per platform at compile time and, on Linux, per machine at runtime
// (io_uring vs epoll -- see detail/runtime_detect.hpp). Keeping the vocabulary
// types here (not in event_loop.hpp) avoids an include cycle: event_loop.hpp and
// every backend include THIS header.

#include "detail/os_backend.hpp"
#include <cstdint>
#include <memory>

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

    namespace detail
    {

        // One reactor implementation. Every method runs on the OWNING engine
        // thread except wake(), which is the sole cross-thread entry (it must be a
        // single MT-safe kernel operation -- eventfd write / kevent EVFILT_USER /
        // PostQueuedCompletionStatus -- never anything that mutates engine-local
        // state from another thread).
        struct reactor_backend
        {
            virtual ~reactor_backend() = default;

            // One-shot readiness watch on `fd` for `mask`, tagged with `token`.
            virtual void arm(int fd, std::uint32_t mask, std::uint64_t token) = 0;
            // One-shot timer firing after `ms` ms, tagged `token`.
            virtual void arm_timer(std::uint64_t token, int ms) = 0;
            // Best-effort disarm of an outstanding readiness watch.
            virtual void cancel(int fd, std::uint32_t mask) = 0;
            // Block until >=1 event or timeout (<0 = forever); fill up to `max`;
            // return count (0 on timeout).
            virtual int wait(io_event *ev, int max, int timeout_ms) = 0;
            // Unblock a concurrent wait() from another thread.
            virtual void wake() = 0;

            // Optional capability: arm a *multishot* accept on a listener so the
            // kernel delivers each accepted connection without re-arming (io_uring).
            // Default returns false = "not supported, caller should one-shot arm()".
            virtual bool arm_accept_multishot(int /*listen_fd*/, std::uint64_t /*token*/)
            {
                return false;
            }
        };

        // Per-backend factories; each is defined only in its own .cpp under the
        // matching SWIFTNET_BACKEND_* guard, so only the platform's backend links.
        std::unique_ptr<reactor_backend> make_kqueue_backend();
        std::unique_ptr<reactor_backend> make_iouring_backend();
        std::unique_ptr<reactor_backend> make_epoll_backend();
        std::unique_ptr<reactor_backend> make_iocp_backend();

    } // namespace detail
} // namespace swiftnet
