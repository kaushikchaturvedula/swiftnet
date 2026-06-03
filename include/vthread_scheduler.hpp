#ifndef vthread_scheduler_hpp
#define vthread_scheduler_hpp

#include "detail/mpsc_queue.hpp"
#include "net/tcp_socket.hpp"
#include "vthread.hpp"
#include "config.hpp"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace swiftnet
{
    class event_loop;

    // Per-core "engine" scheduler (shared-nothing). Each engine is one pinned
    // thread that fuses the reactor and the worker: it owns its own event_loop
    // (kqueue/io_uring/IOCP), its own listener (SO_REUSEPORT), and engine-local
    // state (pending I/O ops, owned root frames, timers) accessed only on its own
    // thread -- so the per-request I/O path takes NO locks.
    //
    // A connection is pinned to the engine that accepted it: its coroutine, its
    // socket fds, and its io_ops all live on that one thread, and it always
    // resumes there. Cross-thread work (schedule() from another thread) is
    // injected through a lock-free MPSC "foreign inbox" and drained by the engine.
    //
    // (Phase 3d will add a conservative work-stealing release valve for compute
    // tasks on top of this; I/O coroutines remain pinned.)
    class vthread_scheduler
    {
    public:
        static vthread_scheduler &instance();

        // Primary entry: engines + valve come from the resolved Config.
        void start(const Config &cfg);
        // Convenience: build a Config (defaults seeded with `threads`), overlay
        // YAML+env, and start. threads==0 => all logical cores.
        void start(std::size_t threads = 0);
        void stop();

        // Create one SO_REUSEPORT listener per engine on `port`; each engine
        // accepts its own connections and runs `on_accept(socket)` pinned to it.
        void add_listener(std::uint16_t port, int backlog,
                          std::function<vthread(net::tcp_socket)> on_accept);

        // Schedule a root (fire-and-forget) virtual thread. May be called from any
        // thread; the frame is handed to an engine via its foreign inbox.
        void schedule(vthread t);
        void schedule_with_affinity(vthread t, std::size_t preferred_engine);

        // --- I/O suspension protocol (called from io_awaitable, ON the engine
        //     thread that owns the coroutine; routes to that engine, lock-free) ---
        void suspend_for_io(std::coroutine_handle<> h, int fd, std::uint32_t mask);
        void suspend_for_timer(std::coroutine_handle<> h, int ms);
        int take_io_result(std::coroutine_handle<> h);

        // Called by a coroutine's final_awaitable when a ROOT completes (on its
        // engine thread); the frame is reaped after the resume unwinds.
        void on_root_complete(std::coroutine_handle<> h) noexcept;

        // --- Compute offload + work-stealing valve (Phase 3d, scheduler experiment) ---
        // Offload CPU-heavy work off the connection's engine so it does not block
        // that engine's other (I/O-bound) connections. enqueue_compute() suspends
        // the calling coroutine, queues a stealable compute task on the current
        // engine, and (valve on) wakes peers so an IDLE engine can steal and run
        // it; the connection is then resumed ON ITS OWNING ENGINE (its I/O is
        // pinned there). Returns true if the work was queued (caller stays
        // suspended), false if run inline because there is no current engine.
        // Only compute tasks are stealable; I/O coroutines are never stolen.
        bool enqueue_compute(std::coroutine_handle<> h, std::function<void()> fn);

        // Valve controls (runtime-tunable; conservative defaults). Also read from
        // env at start(): SWIFTNET_STEAL=0|1, SWIFTNET_STEAL_THRESHOLD=<int>.
        void set_steal(bool on) noexcept { steal_enabled_.store(on, std::memory_order_relaxed); }
        void set_steal_threshold(int t) noexcept { steal_threshold_ = t < 0 ? 0 : t; }
        bool steal_enabled() const noexcept { return steal_enabled_.load(std::memory_order_relaxed); }

        std::pmr::memory_resource *local_resource(std::size_t engine);

        struct Stats
        {
            uint64_t total_scheduled{0};
            uint64_t total_io_suspended{0};
            uint64_t total_resumed{0};
            uint64_t work_stolen{0};
            uint64_t context_switches{0};
            uint64_t completed{0};
            std::vector<uint64_t> per_core_executed;
            std::vector<uint64_t> per_core_compute_depth; // current stealable-compute backlog per engine
        };
        Stats get_stats() const;
        std::size_t worker_count() const noexcept { return ncores_; }

    private:
        vthread_scheduler() = default;
        ~vthread_scheduler();

        struct Engine; // defined below; ComputeTask holds an Engine* (owner)

        struct IoOp
        {
            int fd{-1};
            std::uint32_t mask{0};
            int result{0};
            bool is_timer{false};
        };

        // A stealable unit of CPU work created by enqueue_compute(). `fn` runs on
        // whichever engine picks it up (its owner, or an idle thief); afterwards
        // `resume` (the suspended connection coroutine) is resumed on `owner` --
        // the engine where its socket/I/O is pinned.
        struct ComputeTask
        {
            std::function<void()> fn;
            std::coroutine_handle<> resume;
            Engine *owner;
        };

        // One per core. Touched only by its own thread except `inbox` (MPSC) and
        // the control fields used to hand it a listener at startup.
        struct Engine
        {
            std::size_t id{0};
            std::unique_ptr<event_loop> loop;
            std::thread thread;
            std::unique_ptr<std::pmr::monotonic_buffer_resource> arena;

            // Engine-local (no locks): result slots, owned roots, reap list.
            std::unordered_map<void *, IoOp> io_ops;
            std::unordered_map<void *, vthread> roots;
            std::vector<std::coroutine_handle<>> completed;

            // Cross-thread injection of new roots (lock-free MPSC).
            detail::mpsc_queue<vthread> inbox;

            // Stealable compute pool. Compute tasks are RARE (only compute-heavy
            // requests), so a small mutex here is acceptable -- the per-request
            // I/O path stays lock-free. `compute_depth` mirrors the deque size as
            // a relaxed atomic so the valve can check the steal threshold and
            // sample backlog without taking the lock.
            std::deque<ComputeTask *> compute_q;
            std::mutex compute_mtx;
            std::atomic<int> compute_depth{0};

            // Cross-engine "resume this connection" hand-back: a thief that ran a
            // stolen compute task pushes the connection handle here so the owning
            // engine resumes it (where its I/O is pinned). Lock-free MPSC.
            detail::mpsc_queue<std::coroutine_handle<>> resume_q;

            // Listener handed in by add_listener() (armed lazily on this thread).
            std::atomic<int> listen_fd{-1};
            std::function<vthread(net::tcp_socket)> on_accept;
            bool listener_armed{false};

            // per-engine stats
            std::atomic<uint64_t> executed{0};

            // Whether this engine is currently counted in idle_engines_ (for the
            // steal_min_idle gate). Touched only on its own thread.
            bool counted_idle{false};

            Engine() = default;
        };

        // The engine running on the current thread (set at engine start). The
        // I/O path uses this to reach engine-local state with no locking.
        static thread_local Engine *t_engine_;

        void run(Engine &e);
        void bind_core(std::size_t core);
        void drain_inbox(Engine &e);
        void do_accept(Engine &e);
        void reap(Engine &e);

        // Compute valve internals.
        void drain_resume(Engine &e);             // resume connections handed back by thieves
        bool service_compute(Engine &e, bool idle); // run/steal one compute task; true if it did work
        ComputeTask *steal_compute(Engine &thief); // take work from a victim over the threshold

        std::vector<std::unique_ptr<Engine>> engines_;
        std::atomic<std::size_t> next_engine_{0};
        std::atomic<bool> running_{false};
        std::size_t ncores_{0};
        mutable std::mutex global_mutex_;

        // Work-stealing valve config + global pending-compute counter (drives the
        // reactor wait timeout only when compute is in flight; zero otherwise, so
        // the pure-I/O path is unchanged).
        std::atomic<bool> steal_enabled_{false};
        int steal_threshold_{1};
        int steal_max_batch_{1};  // max compute tasks run/stolen per engine loop turn
        int steal_min_idle_{0};   // require >= this many idle engines before stealing
        std::atomic<int> idle_engines_{0}; // current count of idle engines (min_idle gate)
        std::atomic<int> g_pending_compute_{0};
        static constexpr int kComputeBacklogCap = 8; // anti-starvation: run own when backed up

        // Aggregate stats (relaxed atomics; never on a per-request lock).
        std::atomic<uint64_t> stat_scheduled_{0};
        std::atomic<uint64_t> stat_io_suspended_{0};
        std::atomic<uint64_t> stat_resumed_{0};
        std::atomic<uint64_t> stat_stolen_{0};
        std::atomic<uint64_t> stat_ctxsw_{0};
        std::atomic<uint64_t> stat_completed_{0};
    };

    // Awaitable returned by offload(): co_await it from a (coroutine) handler to
    // run `fn` as a stealable compute task off the connection's engine, then
    // continue on the connection's owning engine. See enqueue_compute().
    //
    //   app.get("/heavy", [](Request&, Response& r) -> vthread {
    //       co_await swiftnet::offload([&]{ /* CPU-heavy work */ });
    //       r.json(...);
    //   });
    struct offload_awaitable
    {
        std::function<void()> fn;
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h)
        {
            // returns true -> stay suspended (queued); false -> ran inline, resume
            return vthread_scheduler::instance().enqueue_compute(h, std::move(fn));
        }
        void await_resume() const noexcept {}
    };

    inline offload_awaitable offload(std::function<void()> fn)
    {
        return offload_awaitable{std::move(fn)};
    }

}

#endif
