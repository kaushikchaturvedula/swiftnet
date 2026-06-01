#ifndef vthread_scheduler_hpp
#define vthread_scheduler_hpp

#include "detail/mpsc_queue.hpp"
#include "detail/work_queue.hpp"
#include "vthread.hpp"
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace swiftnet
{
    class event_loop;

    // Work-stealing virtual-thread scheduler with a single I/O reactor.
    //
    // Design (see plan): root coroutine frames are owned by `roots_` from
    // schedule() until completion, so ownership never moves while a coroutine is
    // suspended on I/O. Worker threads resume ready coroutine handles drawn from
    // per-core work-stealing queues. One reactor thread drains the platform event
    // loop (kqueue/io_uring/IOCP) and, per completion, re-enqueues the suspended
    // coroutine handle for a worker to resume. Because the owning root is always
    // live in `roots_`, there is no suspend/arm/resume ownership race.
    class vthread_scheduler
    {
    public:
        static vthread_scheduler &instance();

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

        // Schedule a root (fire-and-forget) virtual thread; the scheduler owns
        // its frame until completion.
        void schedule(vthread t);
        void schedule_with_affinity(vthread t, std::size_t preferred_core);

        // --- I/O suspension protocol (called from io_awaitable, on a worker) ---
        // Record the pending op for `h` and arm the reactor. `h` is already fully
        // suspended and its owning root is live in roots_, so arming here is safe.
        void suspend_for_io(std::coroutine_handle<> h, int fd, std::uint32_t mask);
        // Arm a one-shot timer for `h` (async_sleep / timers).
        void suspend_for_timer(std::coroutine_handle<> h, int ms);
        // Called by the reactor when `h`'s op completes: stash the result and
        // enqueue `h` for resumption on a worker.
        void resume_from_io(std::coroutine_handle<> h, int result);
        // Consume the stored result for `h` (from io_awaitable::await_resume).
        int take_io_result(std::coroutine_handle<> h);

        // Called by a coroutine's final_awaitable when a ROOT completes; the
        // handle is reaped (destroyed) by a worker shortly after.
        void on_root_complete(std::coroutine_handle<> h) noexcept;

        std::pmr::memory_resource *local_resource(std::size_t core);

        struct Stats
        {
            uint64_t total_scheduled{0};
            uint64_t total_io_suspended{0};
            uint64_t total_resumed{0};
            uint64_t work_stolen{0};
            uint64_t context_switches{0};
            uint64_t completed{0};
            std::vector<uint64_t> per_core_executed;
        };
        Stats get_stats() const;
        std::size_t worker_count() const noexcept { return ncores_; }

    private:
        vthread_scheduler() = default;
        ~vthread_scheduler();

        void worker(std::size_t core_id);
        void reactor_loop();
        void bind_core(std::size_t core);
        bool try_steal(std::size_t core, std::coroutine_handle<> &out);
        void wake_worker(std::size_t core);
        void sleep_worker(std::size_t core);
        void enqueue(std::coroutine_handle<> h);
        void enqueue_on(std::coroutine_handle<> h, std::size_t core);
        void reap_completed();
        std::size_t select_best_core() const;

        struct IoOp
        {
            int fd{-1};
            std::uint32_t mask{0};
            int result{0};
            bool is_timer{false};
        };

        // Per-core work-stealing: each core owns a Chase-Lev deque (owner
        // push/pop, thieves steal) plus a lock-free MPSC inbox that any thread
        // uses to inject work. The owning worker drains its inbox into its deque,
        // which keeps the deque's single-producer invariant intact.
        std::vector<std::unique_ptr<detail::work_queue>> queues_;
        std::vector<std::unique_ptr<detail::mpsc_queue<std::coroutine_handle<>>>> inboxes_;
        std::vector<std::unique_ptr<std::pmr::monotonic_buffer_resource>> arenas_;
        std::vector<std::thread> workers_;
        std::vector<std::unique_ptr<std::atomic<uint32_t>>> core_loads_;

        std::vector<std::unique_ptr<std::condition_variable>> worker_conditions_;
        std::vector<std::unique_ptr<std::mutex>> worker_mutexes_;
        std::vector<uint8_t> worker_sleeping_;

        // Owns root coroutine frames from schedule() until completion.
        std::unordered_map<void *, vthread> roots_;
        std::mutex roots_mutex_;

        // Roots that have completed and await reaping (destruction by a worker).
        std::vector<std::coroutine_handle<>> completed_;
        std::mutex completed_mutex_;

        // Pending I/O ops (result slots), keyed by coroutine handle address.
        std::unordered_map<void *, IoOp> io_ops_;
        std::mutex io_ops_mutex_;

        std::atomic<std::size_t> next_core_{0};
        std::atomic<bool> running_{false};
        std::atomic<bool> reactor_running_{false};
        std::thread reactor_thread_;
        std::size_t ncores_{0};
        mutable std::mutex global_mutex_;

        // Hot-path counters are relaxed atomics so statistics never touch a mutex.
        std::atomic<uint64_t> stat_scheduled_{0};
        std::atomic<uint64_t> stat_io_suspended_{0};
        std::atomic<uint64_t> stat_resumed_{0};
        std::atomic<uint64_t> stat_stolen_{0};
        std::atomic<uint64_t> stat_ctxsw_{0};
        std::atomic<uint64_t> stat_completed_{0};
        std::vector<std::unique_ptr<std::atomic<uint64_t>>> per_core_executed_;

        std::unique_ptr<event_loop> event_loop_;
    };

}

#endif
