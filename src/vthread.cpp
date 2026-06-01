#include "vthread.hpp"
#include "vthread_scheduler.hpp"

namespace swiftnet::detail
{
    // Completion transfer (see vthread.hpp):
    //  - Nested task (has a continuation): symmetric-transfer to the awaiter so
    //    it resumes inline. The completed frame stays suspended at final_suspend
    //    and is destroyed by its owner (the awaiting frame's temporary).
    //  - Root task (no continuation): hand the handle to the scheduler to reap
    //    (destroy) once we have fully unwound out of this frame.
    std::coroutine_handle<> final_transfer(std::coroutine_handle<> continuation,
                                           std::coroutine_handle<> self) noexcept
    {
        if (continuation)
            return continuation;
        vthread_scheduler::instance().on_root_complete(self);
        return std::noop_coroutine();
    }
}
