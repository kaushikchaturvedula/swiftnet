// epoll reactor backend (Linux) -- UNVERIFIED for throughput.
// Used as the fallback when the io_uring probe fails (old kernel / seccomp /
// container) and as the only Linux backend when liburing is absent. Per-engine
// (no global state): EPOLLONESHOT matches the "arm once, resume once" contract;
// wake() is an eventfd write; timers are per-token timerfds.
#include "detail/backend/iface.hpp"

#if defined(__linux__)

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <errno.h>

namespace swiftnet::detail
{
    namespace
    {
        constexpr std::uint64_t kWakeToken = 0;
    }

    class epoll_backend final : public reactor_backend
    {
        int epfd_{-1};
        int wakefd_{-1};                              // eventfd for wake()
        std::unordered_map<std::uint64_t, int> timerfds_; // token -> timerfd (engine-local)

    public:
        epoll_backend()
        {
            epfd_ = epoll_create1(EPOLL_CLOEXEC);
            if (epfd_ < 0)
                throw std::runtime_error("epoll_create1 failed");
            wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (wakefd_ < 0)
                throw std::runtime_error("eventfd failed");
            // Level-triggered, persistent (NOT one-shot) so wake() always fires.
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = kWakeToken;
            if (epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev) < 0)
                throw std::runtime_error("epoll_ctl(wakefd) failed");
        }

        ~epoll_backend() override
        {
            for (auto &[tok, tfd] : timerfds_)
                ::close(tfd);
            if (wakefd_ >= 0)
                ::close(wakefd_);
            if (epfd_ >= 0)
                ::close(epfd_);
        }

        void arm(int fd, std::uint32_t mask, std::uint64_t token) override
        {
            epoll_event ev{};
            ev.events = EPOLLONESHOT;
            if (mask & READABLE)
                ev.events |= EPOLLIN;
            if (mask & WRITABLE)
                ev.events |= EPOLLOUT;
            ev.data.u64 = token;
            // With EPOLLONESHOT the fd stays registered but disarmed after firing,
            // so a re-arm must MOD (re-enable), not ADD. Try ADD; on EEXIST, MOD.
            if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0)
            {
                if (errno == EEXIST)
                    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
                else
                    throw std::runtime_error("epoll_ctl(arm) failed");
            }
        }

        void arm_timer(std::uint64_t token, int ms) override
        {
            int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (tfd < 0)
                throw std::runtime_error("timerfd_create failed");
            itimerspec its{};
            its.it_value.tv_sec = ms / 1000;
            its.it_value.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
            if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
                its.it_value.tv_nsec = 1; // 0 disarms a timerfd; fire ASAP instead
            timerfd_settime(tfd, 0, &its, nullptr);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLONESHOT;
            ev.data.u64 = token;
            if (epoll_ctl(epfd_, EPOLL_CTL_ADD, tfd, &ev) < 0)
            {
                ::close(tfd);
                throw std::runtime_error("epoll_ctl(timer) failed");
            }
            timerfds_[token] = tfd;
        }

        void cancel(int fd, std::uint32_t mask) override
        {
            (void)mask;
            epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr); // ignore ENOENT (already fired)
        }

        int wait(io_event *evs, int max, int timeout_ms) override
        {
            std::vector<epoll_event> events(max);
            int n = epoll_wait(epfd_, events.data(), max, timeout_ms);
            if (n < 0)
            {
                if (errno == EINTR)
                    return 0;
                throw std::runtime_error("epoll_wait failed");
            }
            int out = 0;
            for (int i = 0; i < n; ++i)
            {
                std::uint64_t token = events[i].data.u64;
                if (token == kWakeToken)
                {
                    std::uint64_t drain;
                    ssize_t r = ::read(wakefd_, &drain, sizeof(drain));
                    (void)r;
                    continue; // wake() nudge: nothing to report
                }
                auto it = timerfds_.find(token);
                if (it != timerfds_.end())
                {
                    std::uint64_t exp;
                    ssize_t r = ::read(it->second, &exp, sizeof(exp));
                    (void)r;
                    ::close(it->second); // one-shot timer: remove + close
                    timerfds_.erase(it);
                    evs[out].token = token;
                    evs[out].mask = 0; // timer (matches kqueue EVFILT_TIMER mask=0)
                    evs[out].res = 0;
                    ++out;
                    continue;
                }
                std::uint32_t m = 0;
                if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP))
                    m |= READABLE;
                if (events[i].events & EPOLLOUT)
                    m |= WRITABLE;
                evs[out].token = token;
                evs[out].mask = m;
                evs[out].res = 1; // readiness hint; the handler re-issues the syscall
                ++out;
            }
            return out;
        }

        void wake() override
        {
            std::uint64_t one = 1;
            ssize_t r = ::write(wakefd_, &one, sizeof(one));
            (void)r;
        }
    };

    std::unique_ptr<reactor_backend> make_epoll_backend()
    {
        return std::make_unique<epoll_backend>();
    }

} // namespace swiftnet::detail

#endif // __linux__
