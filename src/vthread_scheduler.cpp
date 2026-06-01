#include "vthread_scheduler.hpp"
#include "event_loop.hpp"
#include "detail/log.hpp"
#include <algorithm>
#include <random>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

using namespace swiftnet;

vthread_scheduler &vthread_scheduler::instance()
{
    static vthread_scheduler inst;
    return inst;
}

vthread_scheduler::~vthread_scheduler() { stop(); }

void vthread_scheduler::start(std::size_t threads)
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (running_)
        return;

    ncores_ = threads ? threads : std::thread::hardware_concurrency();
    if (ncores_ == 0)
        ncores_ = 1;

    queues_.clear();
    inboxes_.clear();
    arenas_.clear();
    core_loads_.clear();
    worker_conditions_.clear();
    worker_mutexes_.clear();
    worker_sleeping_.assign(ncores_, 0);

    queues_.reserve(ncores_);
    inboxes_.reserve(ncores_);
    arenas_.reserve(ncores_);
    core_loads_.reserve(ncores_);
    worker_conditions_.reserve(ncores_);
    worker_mutexes_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
    {
        queues_.emplace_back(std::make_unique<detail::work_queue>());
        inboxes_.emplace_back(std::make_unique<detail::mpsc_queue<std::coroutine_handle<>>>());
        arenas_.emplace_back(std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024)); // 1 MiB / core
        core_loads_.emplace_back(std::make_unique<std::atomic<uint32_t>>(0));
        worker_conditions_.emplace_back(std::make_unique<std::condition_variable>());
        worker_mutexes_.emplace_back(std::make_unique<std::mutex>());
    }

    per_core_executed_.clear();
    per_core_executed_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
        per_core_executed_.emplace_back(std::make_unique<std::atomic<uint64_t>>(0));

    event_loop_ = std::make_unique<event_loop>();

    running_ = true;
    reactor_running_ = true;

    workers_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
        workers_.emplace_back([this, i] { worker(i); });

    reactor_thread_ = std::thread([this] { reactor_loop(); });

    SWIFTNET_LOG_INFO("scheduler online: {} worker cores + 1 reactor", ncores_);
}

void vthread_scheduler::stop()
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (!running_)
        return;

    // 1. Stop workers: no new coroutine work runs after they drain/join.
    running_ = false;
    for (std::size_t i = 0; i < ncores_; ++i)
        wake_worker(i);
    for (auto &t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();

    // 2. Stop the reactor: no more resume_from_io after this.
    reactor_running_ = false;
    if (event_loop_)
        event_loop_->wake();
    if (reactor_thread_.joinable())
        reactor_thread_.join();

    // 3. Reap any roots that completed but were not yet destroyed.
    reap_completed();

    // 4. Destroy still-suspended root frames. Destroying a suspended coroutine
    //    runs in-scope destructors and frees the frame (and, transitively, any
    //    nested awaited task temporaries it owns).
    {
        std::lock_guard<std::mutex> lk(roots_mutex_);
        roots_.clear(); // vthread destructors destroy the frames
    }
    {
        std::lock_guard<std::mutex> lk(io_ops_mutex_);
        io_ops_.clear();
    }

    queues_.clear();
    inboxes_.clear(); // handle copies only; frames were owned by roots_ (cleared above)
    arenas_.clear();
    core_loads_.clear();
    worker_conditions_.clear();
    worker_mutexes_.clear();
    worker_sleeping_.clear();
    event_loop_.reset();

    SWIFTNET_LOG_INFO("scheduler stopped");
}

void vthread_scheduler::bind_core(std::size_t c)
{
#ifdef __linux__
    // Pin this worker to core `c` for cache locality and to reduce migration.
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(c, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#else
    // macOS provides no true thread->core pinning. The only knob is
    // thread_policy_set(THREAD_AFFINITY_POLICY), an affinity-*tag* hint, and even
    // that returns KERN_NOT_SUPPORTED on Apple Silicon. So core pinning is a
    // documented no-op here; the scheduler relies on the OS scheduler instead.
    (void)c;
#endif
}

std::size_t vthread_scheduler::select_best_core() const
{
    std::size_t best = 0;
    uint32_t min_load = core_loads_[0]->load(std::memory_order_relaxed);
    for (std::size_t i = 1; i < ncores_; ++i)
    {
        uint32_t load = core_loads_[i]->load(std::memory_order_relaxed);
        if (load < min_load)
        {
            min_load = load;
            best = i;
        }
    }
    return best;
}

void vthread_scheduler::enqueue_on(std::coroutine_handle<> h, std::size_t core)
{
    // Inject via the MPSC inbox: enqueue() may be called from any thread (the
    // reactor, or a worker running schedule()), and the Chase-Lev deque only
    // permits its owning worker to push. The owner drains the inbox in worker().
    inboxes_[core]->push(h);
    core_loads_[core]->fetch_add(1, std::memory_order_relaxed);
    wake_worker(core);
}

void vthread_scheduler::enqueue(std::coroutine_handle<> h)
{
    // Round-robin placement: O(1), spreads work (especially the single reactor's
    // resumed coroutines) evenly across cores. Work-stealing corrects any
    // residual imbalance. select_best_core() biased toward low-index cores on
    // ties, which starved high-index cores and produced multi-second tail latency.
    std::size_t core = next_core_.fetch_add(1, std::memory_order_relaxed) % ncores_;
    enqueue_on(h, core);
}

void vthread_scheduler::schedule(vthread t)
{
    if (!running_ || !t.valid())
        return;
    auto h = t.handle();
    {
        std::lock_guard<std::mutex> lk(roots_mutex_);
        roots_.emplace(h.address(), std::move(t));
    }
    stat_scheduled_.fetch_add(1, std::memory_order_relaxed);
    enqueue(h);
}

void vthread_scheduler::schedule_with_affinity(vthread t, std::size_t preferred_core)
{
    if (!running_ || !t.valid())
        return;
    auto h = t.handle();
    {
        std::lock_guard<std::mutex> lk(roots_mutex_);
        roots_.emplace(h.address(), std::move(t));
    }
    stat_scheduled_.fetch_add(1, std::memory_order_relaxed);
    enqueue_on(h, std::min(preferred_core, ncores_ - 1));
}

bool vthread_scheduler::try_steal(std::size_t core, std::coroutine_handle<> &out)
{
    // Try a handful of random victims.
    static thread_local std::mt19937 rng{static_cast<uint32_t>((core + 1) * 2654435761u)};
    for (int attempt = 0; attempt < 4 && ncores_ > 1; ++attempt)
    {
        std::size_t victim = rng() % ncores_;
        if (victim == core)
            continue;
        if (queues_[victim]->steal(out))
        {
            core_loads_[victim]->fetch_sub(1, std::memory_order_relaxed);
            stat_stolen_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void vthread_scheduler::worker(std::size_t core)
{
    bind_core(core);

    while (running_.load(std::memory_order_acquire))
    {
        std::coroutine_handle<> h{};
        bool got = false;

        // Drain this core's MPSC inbox into the owned Chase-Lev deque so the work
        // becomes locally poppable and stealable by other cores.
        {
            std::coroutine_handle<> injected{};
            while (inboxes_[core]->pop(injected))
                queues_[core]->push(injected);
        }

        // Dequeue FIFO from our own deque via steal() (not pop()): for a latency-
        // sensitive server, FIFO avoids the tail-latency starvation that LIFO
        // (owner pop) causes when all cores stay busy. push() remains the single
        // producer, so the deque is a lock-free SPMC FIFO queue.
        if (queues_[core]->steal(h))
        {
            core_loads_[core]->fetch_sub(1, std::memory_order_relaxed);
            got = true;
        }
        else if (try_steal(core, h))
        {
            got = true;
        }

        if (got && h && !h.done())
        {
            per_core_executed_[core]->fetch_add(1, std::memory_order_relaxed);
            stat_ctxsw_.fetch_add(1, std::memory_order_relaxed);
            h.resume(); // run until the coroutine next suspends or completes
            reap_completed();
        }
        else
        {
            reap_completed();
            sleep_worker(core);
        }
    }
}

void vthread_scheduler::wake_worker(std::size_t core)
{
    std::lock_guard<std::mutex> lock(*worker_mutexes_[core]);
    if (worker_sleeping_[core])
    {
        worker_sleeping_[core] = 0;
        worker_conditions_[core]->notify_one();
    }
}

void vthread_scheduler::sleep_worker(std::size_t core)
{
    std::unique_lock<std::mutex> lock(*worker_mutexes_[core]);
    worker_sleeping_[core] = 1;
    worker_conditions_[core]->wait_for(lock, std::chrono::milliseconds(10), [this, core] {
        return !running_.load(std::memory_order_acquire) || !worker_sleeping_[core];
    });
    worker_sleeping_[core] = 0;
}

void vthread_scheduler::reactor_loop()
{
    constexpr int kBatch = 128;
    io_event evs[kBatch];
    while (reactor_running_.load(std::memory_order_acquire))
    {
        int n = event_loop_->wait(evs, kBatch, /*timeout_ms*/ -1);
        for (int i = 0; i < n; ++i)
        {
            if (evs[i].token == 0)
                continue; // wake() nudge
            auto h = std::coroutine_handle<>::from_address(
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(evs[i].token)));
            resume_from_io(h, evs[i].res);
        }
    }
}

void vthread_scheduler::suspend_for_io(std::coroutine_handle<> h, int fd, std::uint32_t mask)
{
    {
        std::lock_guard<std::mutex> lk(io_ops_mutex_);
        io_ops_[h.address()] = IoOp{fd, mask, 0, false};
    }
    stat_io_suspended_.fetch_add(1, std::memory_order_relaxed);
    // Arm AFTER recording the op so the result slot exists before any completion.
    if (event_loop_)
        event_loop_->arm(fd, mask, reinterpret_cast<std::uintptr_t>(h.address()));
}

void vthread_scheduler::suspend_for_timer(std::coroutine_handle<> h, int ms)
{
    {
        std::lock_guard<std::mutex> lk(io_ops_mutex_);
        io_ops_[h.address()] = IoOp{-1, 0, 0, true};
    }
    stat_io_suspended_.fetch_add(1, std::memory_order_relaxed);
    if (event_loop_)
        event_loop_->arm_timer(reinterpret_cast<std::uintptr_t>(h.address()), ms);
}

void vthread_scheduler::resume_from_io(std::coroutine_handle<> h, int result)
{
    {
        std::lock_guard<std::mutex> lk(io_ops_mutex_);
        auto it = io_ops_.find(h.address());
        if (it == io_ops_.end())
            return; // unknown / already consumed (e.g. duplicate completion)
        it->second.result = result;
    }
    stat_resumed_.fetch_add(1, std::memory_order_relaxed);
    enqueue(h);
}

int vthread_scheduler::take_io_result(std::coroutine_handle<> h)
{
    std::lock_guard<std::mutex> lk(io_ops_mutex_);
    auto it = io_ops_.find(h.address());
    if (it == io_ops_.end())
        return -1;
    int r = it->second.result;
    io_ops_.erase(it);
    return r;
}

void vthread_scheduler::on_root_complete(std::coroutine_handle<> h) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> lk(completed_mutex_);
            completed_.push_back(h);
        }
        stat_completed_.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
        // noexcept: swallow (allocation failure on the completed_ vector).
    }
}

void vthread_scheduler::reap_completed()
{
    std::vector<std::coroutine_handle<>> done;
    {
        std::lock_guard<std::mutex> lk(completed_mutex_);
        if (completed_.empty())
            return;
        done.swap(completed_);
    }
    for (auto h : done)
    {
        vthread victim; // takes ownership, destroys the (done) frame on scope exit
        {
            std::lock_guard<std::mutex> lk(roots_mutex_);
            auto it = roots_.find(h.address());
            if (it != roots_.end())
            {
                victim = std::move(it->second);
                roots_.erase(it);
            }
        }
        // victim's destructor runs here (outside locks), destroying the frame.
    }
}

std::pmr::memory_resource *vthread_scheduler::local_resource(std::size_t core)
{
    if (core >= arenas_.size())
        return std::pmr::get_default_resource();
    return arenas_[core].get();
}

auto vthread_scheduler::get_stats() const -> Stats
{
    // Serialize with start()/stop() (which rebuild per_core_executed_).
    std::lock_guard<std::mutex> lock(global_mutex_);
    Stats s;
    s.total_scheduled = stat_scheduled_.load(std::memory_order_relaxed);
    s.total_io_suspended = stat_io_suspended_.load(std::memory_order_relaxed);
    s.total_resumed = stat_resumed_.load(std::memory_order_relaxed);
    s.work_stolen = stat_stolen_.load(std::memory_order_relaxed);
    s.context_switches = stat_ctxsw_.load(std::memory_order_relaxed);
    s.completed = stat_completed_.load(std::memory_order_relaxed);
    s.per_core_executed.reserve(per_core_executed_.size());
    for (const auto &c : per_core_executed_)
        s.per_core_executed.push_back(c->load(std::memory_order_relaxed));
    return s;
}
