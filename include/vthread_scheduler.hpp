#ifndef vthread_scheduler_hpp
#define vthread_scheduler_hpp

#include "detail/mpsc_queue.hpp"
#include "net/tcp_socket.hpp"
#include "vthread.hpp"
#include <atomic>
#include <coroutine>
#include <cstdint>
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

        void start(std::size_t threads = std::thread::hardware_concurrency());
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
        };
        Stats get_stats() const;
        std::size_t worker_count() const noexcept { return ncores_; }

    private:
        vthread_scheduler() = default;
        ~vthread_scheduler();

        struct IoOp
        {
            int fd{-1};
            std::uint32_t mask{0};
            int result{0};
            bool is_timer{false};
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

            // Listener handed in by add_listener() (armed lazily on this thread).
            std::atomic<int> listen_fd{-1};
            std::function<vthread(net::tcp_socket)> on_accept;
            bool listener_armed{false};

            // per-engine stats
            std::atomic<uint64_t> executed{0};

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

        std::vector<std::unique_ptr<Engine>> engines_;
        std::atomic<std::size_t> next_engine_{0};
        std::atomic<bool> running_{false};
        std::size_t ncores_{0};
        mutable std::mutex global_mutex_;

        // Aggregate stats (relaxed atomics; never on a per-request lock).
        std::atomic<uint64_t> stat_scheduled_{0};
        std::atomic<uint64_t> stat_io_suspended_{0};
        std::atomic<uint64_t> stat_resumed_{0};
        std::atomic<uint64_t> stat_stolen_{0};
        std::atomic<uint64_t> stat_ctxsw_{0};
        std::atomic<uint64_t> stat_completed_{0};
    };

}

#endif
