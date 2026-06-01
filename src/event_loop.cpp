#include "event_loop.hpp"
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <vector>
#include <errno.h>

#if defined(SWIFTNET_BACKEND_IOURING)
#include <fcntl.h>
#include <poll.h>
#include <liburing.h>
#include <mutex>
#include <memory>
#include <unordered_map>
#elif defined(SWIFTNET_BACKEND_KQUEUE)
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#elif defined(SWIFTNET_BACKEND_IOCP)
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <chrono>
#endif

using namespace swiftnet;

namespace
{
#if defined(SWIFTNET_BACKEND_IOURING)
    unsigned to_poll_events(std::uint32_t mask)
    {
        unsigned ev = 0;
        if (mask & READABLE)
            ev |= POLLIN;
        if (mask & WRITABLE)
            ev |= POLLOUT;
        return ev;
    }
    // io_uring timeouts read the timespec asynchronously, so it must outlive the
    // submission. Keep it alive until the matching completion is reaped.
    std::mutex g_timer_mtx;
    std::unordered_map<std::uint64_t, std::unique_ptr<struct __kernel_timespec>> g_timers;
    constexpr std::uint64_t kWakeToken = 0;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
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
#endif
} // namespace

/* -----------------------------------------------------------
 * Constructor / destructor
 * ---------------------------------------------------------*/

event_loop::event_loop()
{
#if defined(SWIFTNET_BACKEND_IOURING)
    ring_ = new io_uring;
    if (io_uring_queue_init(1024, ring_, 0) != 0)
        throw std::runtime_error("io_uring_queue_init failed");
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    kq_ = kqueue();
    if (kq_ == -1)
        throw std::runtime_error("kqueue() failed");
    // Register a user event used solely to unblock wait() from wake().
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1)
        throw std::runtime_error("kqueue wake-event registration failed");
#elif defined(SWIFTNET_BACKEND_IOCP)
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_)
        throw std::runtime_error("CreateIoCompletionPort failed");
#endif
}

event_loop::~event_loop()
{
#if defined(SWIFTNET_BACKEND_IOURING)
    io_uring_queue_exit(ring_);
    delete ring_;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    close(kq_);
#elif defined(SWIFTNET_BACKEND_IOCP)
    CloseHandle(static_cast<HANDLE>(iocp_));
#endif
}

/* -----------------------------------------------------------
 * Arm (one-shot readiness watch)
 * ---------------------------------------------------------*/

void event_loop::arm(int fd, std::uint32_t mask, std::uint64_t token)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto *sqe = io_uring_get_sqe(ring_);
    io_uring_prep_poll_add(sqe, fd, to_poll_events(mask)); // single-shot
    io_uring_sqe_set_data64(sqe, token);
    io_uring_submit(ring_);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
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
#elif defined(SWIFTNET_BACKEND_IOCP)
    // IOCP is completion-based; associate the socket so completions issued by the
    // socket layer surface here, keyed by token. (Readiness arming is a no-op.)
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)),
                           static_cast<HANDLE>(iocp_), static_cast<ULONG_PTR>(token), 0);
    (void)mask;
#endif
}

/* -----------------------------------------------------------
 * Arm (one-shot timer)
 * ---------------------------------------------------------*/

void event_loop::arm_timer(std::uint64_t token, int ms)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto ts = std::make_unique<struct __kernel_timespec>();
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = static_cast<long long>(ms % 1000) * 1000000LL;
    struct __kernel_timespec *raw = ts.get();
    {
        std::lock_guard<std::mutex> lk(g_timer_mtx);
        g_timers[token] = std::move(ts);
    }
    auto *sqe = io_uring_get_sqe(ring_);
    io_uring_prep_timeout(sqe, raw, 0, 0);
    io_uring_sqe_set_data64(sqe, token);
    io_uring_submit(ring_);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    // Use the token as the timer ident (unique per suspended coroutine). On
    // macOS EVFILT_TIMER data is in milliseconds by default.
    struct kevent ev;
    EV_SET(&ev, static_cast<std::uintptr_t>(token), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, ms, token_to_udata(token));
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1)
        throw std::runtime_error("kevent arm_timer failed");
#elif defined(SWIFTNET_BACKEND_IOCP)
    // Stub backend: post a completion after ms via a throwaway thread.
    void *iocp = iocp_;
    std::thread([iocp, token, ms]
                {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        PostQueuedCompletionStatus(static_cast<HANDLE>(iocp), 0, static_cast<ULONG_PTR>(token), nullptr); })
        .detach();
#endif
}

/* -----------------------------------------------------------
 * Cancel (best-effort disarm)
 * ---------------------------------------------------------*/

void event_loop::cancel(int fd, std::uint32_t mask)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    // poll_remove targets the user_data of the poll to cancel; we don't have it
    // here (it's the token), so cancellation by fd is a no-op. One-shot polls are
    // auto-removed on completion, and closing the fd drops the registration.
    (void)fd;
    (void)mask;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    struct kevent ev[2];
    int n = 0;
    if (mask & READABLE)
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    if (mask & WRITABLE)
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    if (n > 0)
        kevent(kq_, ev, n, nullptr, 0, nullptr); // ignore ENOENT (already fired)
#elif defined(SWIFTNET_BACKEND_IOCP)
    (void)fd;
    (void)mask;
#endif
}

/* -----------------------------------------------------------
 * Wait
 * ---------------------------------------------------------*/

int event_loop::wait(io_event *evs, int max, int timeout_ms)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    struct __kernel_timespec ts;
    struct __kernel_timespec *pts = nullptr;
    if (timeout_ms >= 0)
    {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1000000LL;
        pts = &ts;
    }

    io_uring_cqe *cqe = nullptr;
    int ret = io_uring_wait_cqe_timeout(ring_, &cqe, pts);
    if (ret == -ETIME || ret == -EAGAIN || ret == -EINTR)
        return 0;
    if (ret < 0)
        throw std::runtime_error("io_uring_wait_cqe_timeout failed");

    int cnt = 0;
    while (cqe && cnt < max)
    {
        std::uint64_t token = io_uring_cqe_get_data64(cqe);
        if (token != kWakeToken)
        {
            evs[cnt].token = token;
            evs[cnt].mask = 0;
            if (cqe->res & POLLIN)
                evs[cnt].mask |= READABLE;
            if (cqe->res & POLLOUT)
                evs[cnt].mask |= WRITABLE;
            evs[cnt].res = cqe->res;
            ++cnt;
            // If this completion was a timer, release its timespec.
            std::lock_guard<std::mutex> lk(g_timer_mtx);
            g_timers.erase(token);
        }
        io_uring_cqe_seen(ring_, cqe);
        cqe = nullptr;
        if (cnt < max && io_uring_peek_cqe(ring_, &cqe) != 0)
            break;
    }
    return cnt;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
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
#elif defined(SWIFTNET_BACKEND_IOCP)
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = nullptr;
    DWORD to = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    BOOL ok = GetQueuedCompletionStatus(static_cast<HANDLE>(iocp_), &bytes, &key, &ov, to);
    if (!ok && ov == nullptr)
        return 0; // timeout / no completion
    if (key == 0)
        return 0; // wake() nudge
    evs[0].token = static_cast<std::uint64_t>(key);
    evs[0].mask = READABLE | WRITABLE; // completion-based: readiness unknown
    evs[0].res = static_cast<int>(bytes);
    return 1;
#endif
}

/* -----------------------------------------------------------
 * Wake (unblock a concurrent wait)
 * ---------------------------------------------------------*/

void event_loop::wake()
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto *sqe = io_uring_get_sqe(ring_);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, kWakeToken);
    io_uring_submit(ring_);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);
#elif defined(SWIFTNET_BACKEND_IOCP)
    PostQueuedCompletionStatus(static_cast<HANDLE>(iocp_), 0, 0, nullptr);
#endif
}
