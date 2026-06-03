// kqueue reactor backend (macOS/BSD) -- the VERIFIED path.
// Moved verbatim from the original src/event_loop.cpp; behavior is unchanged.
#include "detail/backend/iface.hpp"

#if defined(SWIFTNET_BACKEND_KQUEUE)

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace swiftnet::detail
{
    namespace
    {
        // EVFILT_USER ident reserved for wake(); coroutine tokens are heap addresses
        // and never collide with this (and EVFILT_USER lives in its own ident space).
        constexpr std::uintptr_t kWakeIdent = 0;
        inline void *token_to_udata(std::uint64_t t) noexcept
        {
            return reinterpret_cast<void *>(static_cast<std::uintptr_t>(t));
        }
        inline std::uint64_t udata_to_token(void *u) noexcept
        {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(u));
        }
    } // namespace

    class kqueue_backend final : public reactor_backend
    {
        int kq_;

    public:
        kqueue_backend()
        {
            kq_ = kqueue();
            if (kq_ == -1)
                throw std::runtime_error("kqueue() failed");
            // Register a user event used solely to unblock wait() from wake().
            struct kevent ev;
            EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1)
                throw std::runtime_error("kqueue wake-event registration failed");
        }

        ~kqueue_backend() override { close(kq_); }

        void arm(int fd, std::uint32_t mask, std::uint64_t token) override
        {
            struct kevent ev[2];
            int n = 0;
            void *ud = token_to_udata(token);
            if (mask & READABLE)
                EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ud);
            if (mask & WRITABLE)
                EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ud);
            if (n == 0)
                return;
            if (kevent(kq_, ev, n, nullptr, 0, nullptr) == -1)
                throw std::runtime_error("kevent arm failed");
        }

        void arm_timer(std::uint64_t token, int ms) override
        {
            // Use the token as the timer ident (unique per suspended coroutine). On
            // macOS EVFILT_TIMER data is in milliseconds by default.
            struct kevent ev;
            EV_SET(&ev, static_cast<std::uintptr_t>(token), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, ms, token_to_udata(token));
            if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1)
                throw std::runtime_error("kevent arm_timer failed");
        }

        void cancel(int fd, std::uint32_t mask) override
        {
            struct kevent ev[2];
            int n = 0;
            if (mask & READABLE)
                EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            if (mask & WRITABLE)
                EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            if (n > 0)
                kevent(kq_, ev, n, nullptr, 0, nullptr); // ignore ENOENT (already fired)
        }

        int wait(io_event *evs, int max, int timeout_ms) override
        {
            std::vector<struct kevent> events(max);
            struct timespec ts;
            struct timespec *pts = nullptr;
            if (timeout_ms >= 0)
            {
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
                pts = &ts;
            }
            int n = kevent(kq_, nullptr, 0, events.data(), max, pts);
            if (n == -1)
            {
                if (errno == EINTR)
                    return 0;
                throw std::runtime_error("kevent wait failed");
            }
            int out = 0;
            for (int i = 0; i < n; ++i)
            {
                const struct kevent &k = events[i];
                if (k.flags & EV_ERROR)
                    continue; // stale/invalid registration
                if (k.filter == EVFILT_USER)
                    continue; // wake() nudge: nothing to report
                evs[out].token = udata_to_token(k.udata);
                if (k.filter == EVFILT_READ)
                    evs[out].mask = READABLE;
                else if (k.filter == EVFILT_WRITE)
                    evs[out].mask = WRITABLE;
                else
                    evs[out].mask = 0; // EVFILT_TIMER and friends
                evs[out].res = static_cast<int>(k.data);
                ++out;
            }
            return out;
        }

        void wake() override
        {
            struct kevent ev;
            EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        }
    };

    std::unique_ptr<reactor_backend> make_kqueue_backend()
    {
        return std::make_unique<kqueue_backend>();
    }

} // namespace swiftnet::detail

#endif // SWIFTNET_BACKEND_KQUEUE
