#ifndef vthread_hpp
#define vthread_hpp

#include "detail/frame_pool.hpp"
#include <coroutine>
#include <cstddef>
#include <exception>
#include <new>
#include <utility>

// Coroutine-frame pooling gate. Measured on Apple Silicon (macOS/arm64) the pool
// is a ~2% regression -- libmalloc's per-thread magazine already serves same-size
// same-thread frames as fast -- so it is OFF by default on Apple. It is left ON
// for non-Apple platforms (e.g. glibc, where the default allocator is weaker for
// this pattern) but UNVERIFIED there. Override either way:
//   -DSWIFTNET_FORCE_FRAME_POOL  force ON   -DSWIFTNET_NO_FRAME_POOL  force OFF
#if defined(SWIFTNET_FORCE_FRAME_POOL)
#define SWIFTNET_USE_FRAME_POOL 1
#elif defined(SWIFTNET_NO_FRAME_POOL) || defined(__APPLE__)
#define SWIFTNET_USE_FRAME_POOL 0
#else
#define SWIFTNET_USE_FRAME_POOL 1
#endif

namespace swiftnet
{

    namespace detail
    {
        // Completion transfer for vthread_base::final_awaitable. Defined in
        // vthread.cpp (it needs the scheduler, which would otherwise create a
        // circular include). Returns the continuation to resume for a nested
        // task, or noop_coroutine after asking the scheduler to reap a completed
        // root. Kept as a non-template free function so final_awaitable can be
        // defined inline and work for any vthread_base<T>.
        std::coroutine_handle<> final_transfer(std::coroutine_handle<> continuation,
                                               std::coroutine_handle<> self) noexcept;
    }

    // A "virtual thread": a lazily-started, move-only coroutine task.
    //
    // Ownership model (single owner at all times):
    //  - The object owns its coroutine frame and destroys it in the destructor
    //    (RAII). Move transfers ownership; the moved-from object is empty.
    //  - When awaited (`co_await some_task()`), the task is a temporary living in
    //    the awaiting coroutine's frame; it is destroyed automatically when the
    //    co_await full-expression ends. Completion uses symmetric transfer to
    //    resume the awaiter with no scheduler involvement.
    //  - When scheduled as a root (fire-and-forget) via the scheduler, the
    //    scheduler owns the object until the coroutine completes, at which point
    //    final_suspend notifies the scheduler to reap (destroy) it.
    template <typename T = void>
    class vthread_base
    {
    public:
        struct promise_type
        {
            T result_{};
            std::coroutine_handle<> continuation_{};
            std::exception_ptr exception_{};

            // Pooled coroutine-frame allocation (see detail/frame_pool.hpp and the
            // SWIFTNET_USE_FRAME_POOL gate above; off by default on Apple Silicon).
#if SWIFTNET_USE_FRAME_POOL
            static void *operator new(std::size_t n) { return detail::frame_pool::local().allocate(n); }
            static void operator delete(void *p) noexcept { detail::frame_pool::deallocate(p); }
#endif

            auto get_return_object() noexcept
            {
                return vthread_base{handle_type::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct final_awaitable
            {
                bool await_ready() const noexcept { return false; }
                // Symmetric transfer: resume the continuation (nested await) or
                // hand a completed root back to the scheduler. Defined in
                // vthread.cpp (needs the scheduler).
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    return detail::final_transfer(h.promise().continuation_, h);
                }
                void await_resume() noexcept {}
            };

            final_awaitable final_suspend() noexcept { return {}; }

            void unhandled_exception() noexcept { exception_ = std::current_exception(); }

            void return_value(T value) { result_ = std::move(value); }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        vthread_base() noexcept : coro_{} {}
        explicit vthread_base(handle_type h) noexcept : coro_(h) {}

        vthread_base(vthread_base &&o) noexcept : coro_(o.coro_) { o.coro_ = {}; }

        vthread_base &operator=(vthread_base &&o) noexcept
        {
            if (&o != this)
            {
                if (coro_)
                    coro_.destroy();
                coro_ = o.coro_;
                o.coro_ = {};
            }
            return *this;
        }

        ~vthread_base()
        {
            if (coro_)
                coro_.destroy();
        }

        [[nodiscard]] bool is_done() const { return !coro_ || coro_.done(); }
        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(coro_); }
        handle_type handle() const noexcept { return coro_; }

        // Release ownership of the frame (caller becomes responsible for it).
        handle_type release() noexcept
        {
            auto h = coro_;
            coro_ = {};
            return h;
        }

        T result() const { return coro_.promise().result_; }

        // ---- Awaiter interface (so a task can be co_await'ed) ----
        bool await_ready() const noexcept { return !coro_ || coro_.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            coro_.promise().continuation_ = awaiting;
            return coro_; // symmetric transfer into the awaited coroutine
        }

        T await_resume()
        {
            if (coro_.promise().exception_)
                std::rethrow_exception(coro_.promise().exception_);
            if constexpr (!std::is_void_v<T>)
                return std::move(coro_.promise().result_);
        }

        static vthread_base from_handle(handle_type h) { return vthread_base{h}; }

    private:
        handle_type coro_;
    };

    // void specialization
    template <>
    class vthread_base<void>
    {
    public:
        struct promise_type
        {
            std::coroutine_handle<> continuation_{};
            std::exception_ptr exception_{};

            // Pooled coroutine-frame allocation (void specialization).
#if SWIFTNET_USE_FRAME_POOL
            static void *operator new(std::size_t n) { return detail::frame_pool::local().allocate(n); }
            static void operator delete(void *p) noexcept { detail::frame_pool::deallocate(p); }
#endif

            auto get_return_object() noexcept
            {
                return vthread_base{handle_type::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct final_awaitable
            {
                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    return detail::final_transfer(h.promise().continuation_, h);
                }
                void await_resume() noexcept {}
            };

            final_awaitable final_suspend() noexcept { return {}; }

            void unhandled_exception() noexcept { exception_ = std::current_exception(); }

            void return_void() noexcept {}
        };

        using handle_type = std::coroutine_handle<promise_type>;

        vthread_base() noexcept : coro_{} {}
        explicit vthread_base(handle_type h) noexcept : coro_(h) {}

        vthread_base(vthread_base &&o) noexcept : coro_(o.coro_) { o.coro_ = {}; }

        vthread_base &operator=(vthread_base &&o) noexcept
        {
            if (&o != this)
            {
                if (coro_)
                    coro_.destroy();
                coro_ = o.coro_;
                o.coro_ = {};
            }
            return *this;
        }

        ~vthread_base()
        {
            if (coro_)
                coro_.destroy();
        }

        [[nodiscard]] bool is_done() const { return !coro_ || coro_.done(); }
        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(coro_); }
        handle_type handle() const noexcept { return coro_; }

        handle_type release() noexcept
        {
            auto h = coro_;
            coro_ = {};
            return h;
        }

        // ---- Awaiter interface ----
        bool await_ready() const noexcept { return !coro_ || coro_.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            coro_.promise().continuation_ = awaiting;
            return coro_; // symmetric transfer
        }

        void await_resume()
        {
            if (coro_.promise().exception_)
                std::rethrow_exception(coro_.promise().exception_);
        }

        static vthread_base from_handle(handle_type h) { return vthread_base{h}; }
        static vthread_base from_handle(std::coroutine_handle<> h)
        {
            return vthread_base{handle_type::from_address(h.address())};
        }

    private:
        handle_type coro_;
    };

    using vthread = vthread_base<void>;

}

#endif
