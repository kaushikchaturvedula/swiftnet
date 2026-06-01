#include "io_awaitable.hpp"
#include "vthread_scheduler.hpp"

using namespace swiftnet;

void io_awaitable::await_suspend(std::coroutine_handle<> h)
{
    handle_ = h;
    // Record intent and arm the reactor. The coroutine is already suspended and
    // its owning root is held by the scheduler, so the reactor may resume `h`
    // (on another worker) the instant we arm — that is safe; we touch nothing
    // after this call.
    vthread_scheduler::instance().suspend_for_io(h, fd_, mask_);
}

int io_awaitable::await_resume()
{
    return vthread_scheduler::instance().take_io_result(handle_);
}

void timer_awaitable::await_suspend(std::coroutine_handle<> h)
{
    handle_ = h;
    vthread_scheduler::instance().suspend_for_timer(h, ms_);
}
