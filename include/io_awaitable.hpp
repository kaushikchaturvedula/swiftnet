#ifndef io_awaitable_hpp
#define io_awaitable_hpp

#include "event_loop.hpp" // mask_from_poll / event_mask
#include <coroutine>
#include <cstdint>

namespace swiftnet
{

    // Suspends the current virtual thread until `fd` is ready for `poll_events`
    // (libc POLLIN/POLLOUT, normalized once here). The reactor arms a one-shot
    // watch and re-mounts the coroutine on completion. await_resume() returns the
    // reactor's result hint (>=0 ready, <0 error/closed); callers re-issue the
    // syscall and handle EAGAIN by awaiting again.
    class io_awaitable
    {
    public:
        io_awaitable(int fd, unsigned poll_events) noexcept
            : fd_(fd), mask_(mask_from_poll(poll_events)) {}

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h);
        int await_resume();

    private:
        int fd_;
        std::uint32_t mask_;
        std::coroutine_handle<> handle_{};
    };

    // Suspends the current virtual thread for `ms` milliseconds using a reactor
    // timer (no OS thread). Stands in for real async I/O latency in demos/tests.
    class timer_awaitable
    {
    public:
        explicit timer_awaitable(int ms) noexcept : ms_(ms) {}

        bool await_ready() const noexcept { return ms_ <= 0; }
        void await_suspend(std::coroutine_handle<> h);
        void await_resume() const noexcept {}

    private:
        int ms_;
        std::coroutine_handle<> handle_{};
    };

    inline timer_awaitable async_sleep(int ms) noexcept { return timer_awaitable{ms}; }

}

#endif
