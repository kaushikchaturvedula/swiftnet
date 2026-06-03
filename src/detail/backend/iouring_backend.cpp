// io_uring reactor backend (Linux) -- UNVERIFIED for throughput.
//
// Modernized vs the original readiness port:
//  - Per-engine timer map (no process-global mutex; each backend instance is
//    single-threaded on its engine).
//  - wake() is an eventfd WRITE, never a cross-thread ring submit. Submitting to a
//    ring from a non-owning thread is UB under IORING_SETUP_SINGLE_ISSUER, so the
//    eventfd is registered with a multishot poll (armed lazily on the engine
//    thread at first wait()) and wake() just writes the counter from any thread.
//  - Ring setup tries SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN and gracefully
//    downgrades (COOP_TASKRUN, then bare init) so older kernels still work.
//
// Still readiness-based (io_uring_prep_poll_add) for the per-request byte path, to
// match the tcp_socket "ready -> issue syscall" model and the {token,mask,res}
// io_event contract. Multishot accept/recv and provided buffers are the remaining
// modernization (they require scheduler accept-path / recv-awaitable changes); see
// arm_accept_multishot() below. NONE of this is throughput-measured -- see
// BENCHMARKS.md; no speed is claimed.
#include "detail/backend/iface.hpp"

#if defined(SWIFTNET_BACKEND_IOURING) && defined(SWIFTNET_HAS_LIBURING)

#include "detail/log.hpp"
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <liburing.h>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <errno.h>

namespace swiftnet::detail
{
    namespace
    {
        unsigned to_poll_events(std::uint32_t mask)
        {
            unsigned ev = 0;
            if (mask & READABLE)
                ev |= POLLIN;
            if (mask & WRITABLE)
                ev |= POLLOUT;
            return ev;
        }
        constexpr std::uint64_t kWakeToken = 0;
    } // namespace

    class iouring_backend final : public reactor_backend
    {
        io_uring ring_{};
        int wakefd_{-1};
        bool wake_armed_{false};
        // Per-engine (single-threaded) -- the timespec must outlive the submission,
        // so it lives in this map until the matching completion is reaped. No mutex.
        std::unordered_map<std::uint64_t, __kernel_timespec> timers_;

        void arm_wake() // engine-thread only (first wait); keeps SINGLE_ISSUER valid
        {
            auto *sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_poll_multishot(sqe, wakefd_, POLLIN);
            io_uring_sqe_set_data64(sqe, kWakeToken);
            io_uring_submit(&ring_);
            wake_armed_ = true;
        }

    public:
        iouring_backend()
        {
            wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (wakefd_ < 0)
                throw std::runtime_error("eventfd failed");

            // Ring flags. We deliberately AVOID IORING_SETUP_SINGLE_ISSUER and
            // IORING_SETUP_DEFER_TASKRUN: they bind the ring's issuer task at
            // init/first-enter, but SwiftNet creates each engine's ring on the main
            // thread and then submits/waits on the engine thread -- that mismatch
            // aborts at runtime under those flags (observed in a Linux container),
            // and we cannot validate the fix on real hardware. COOP_TASKRUN is the
            // safe, thread-agnostic perf hint; downgrade to bare init if unsupported.
            // (This whole backend is UNVERIFIED for throughput -- see BENCHMARKS.md.)
            const struct
            {
                unsigned flags;
                const char *name;
            } attempts[] = {
                {IORING_SETUP_COOP_TASKRUN, "COOP_TASKRUN"},
                {0u, "default"},
            };
            bool ok = false;
            for (const auto &a : attempts)
            {
                io_uring_params p{};
                p.flags = a.flags;
                if (io_uring_queue_init_params(1024, &ring_, &p) == 0)
                {
                    SWIFTNET_LOG_INFO("io_uring ring flags: {}", a.name);
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                ::close(wakefd_);
                throw std::runtime_error("io_uring_queue_init_params failed");
            }
        }

        ~iouring_backend() override
        {
            io_uring_queue_exit(&ring_);
            if (wakefd_ >= 0)
                ::close(wakefd_);
        }

        void arm(int fd, std::uint32_t mask, std::uint64_t token) override
        {
            auto *sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_poll_add(sqe, fd, to_poll_events(mask)); // single-shot readiness
            io_uring_sqe_set_data64(sqe, token);
            io_uring_submit(&ring_);
        }

        void arm_timer(std::uint64_t token, int ms) override
        {
            __kernel_timespec &ts = timers_[token]; // stable address until erased
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = static_cast<long long>(ms % 1000) * 1000000LL;
            auto *sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_timeout(sqe, &ts, 0, 0);
            io_uring_sqe_set_data64(sqe, token);
            io_uring_submit(&ring_);
        }

        void cancel(int fd, std::uint32_t mask) override
        {
            // One-shot polls auto-remove on completion; closing the fd drops the
            // registration. Cancellation by fd alone isn't expressible here.
            (void)fd;
            (void)mask;
        }

        int wait(io_event *evs, int max, int timeout_ms) override
        {
            if (!wake_armed_)
                arm_wake(); // first call on the engine thread arms the eventfd poll

            struct __kernel_timespec ts;
            struct __kernel_timespec *pts = nullptr;
            if (timeout_ms >= 0)
            {
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1000000LL;
                pts = &ts;
            }

            io_uring_cqe *cqe = nullptr;
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, pts);
            if (ret == -ETIME || ret == -EAGAIN || ret == -EINTR)
                return 0;
            if (ret < 0)
                throw std::runtime_error("io_uring_wait_cqe_timeout failed");

            int cnt = 0;
            while (cqe && cnt < max)
            {
                std::uint64_t token = io_uring_cqe_get_data64(cqe);
                if (token == kWakeToken)
                {
                    // eventfd poll fired: drain the counter so the multishot poll
                    // re-arms cleanly; report nothing.
                    std::uint64_t drain;
                    ssize_t r = ::read(wakefd_, &drain, sizeof(drain));
                    (void)r;
                }
                else
                {
                    evs[cnt].token = token;
                    evs[cnt].mask = 0;
                    if (cqe->res & POLLIN)
                        evs[cnt].mask |= READABLE;
                    if (cqe->res & POLLOUT)
                        evs[cnt].mask |= WRITABLE;
                    evs[cnt].res = cqe->res;
                    ++cnt;
                    timers_.erase(token); // no-op unless this was a timer completion
                }
                io_uring_cqe_seen(&ring_, cqe);
                cqe = nullptr;
                if (cnt < max && io_uring_peek_cqe(&ring_, &cqe) != 0)
                    break;
            }
            return cnt;
        }

        void wake() override
        {
            // MT-safe: a single eventfd write from any thread. No ring submit here
            // (that would be a cross-thread issuer under SINGLE_ISSUER -> UB).
            std::uint64_t one = 1;
            ssize_t r = ::write(wakefd_, &one, sizeof(one));
            (void)r;
        }

        // arm_accept_multishot is intentionally NOT overridden: multishot accept
        // requires the scheduler's accept path to consume the accepted fd from the
        // completion (cqe->res) instead of calling accept(), and multishot recv +
        // provided buffers require a separate recv-awaitable and a buffer-return
        // step (the current readiness {token,mask,res} contract has no buffer
        // field). Those are the remaining UNVERIFIED io_uring modernizations; the
        // default (returns false) keeps the verified one-shot accept path. See
        // BENCHMARKS.md and include/event_loop.hpp.
    };

    std::unique_ptr<reactor_backend> make_iouring_backend()
    {
        return std::make_unique<iouring_backend>();
    }

} // namespace swiftnet::detail

#endif // SWIFTNET_BACKEND_IOURING && SWIFTNET_HAS_LIBURING
