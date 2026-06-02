#include "vthread_scheduler.hpp"
#include "event_loop.hpp"
#include "detail/os_backend.hpp"
#include "detail/log.hpp"
#include <cstdint>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#ifndef SWIFTNET_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace swiftnet;

// The engine running on the current thread (private static thread_local member).
thread_local vthread_scheduler::Engine *vthread_scheduler::t_engine_ = nullptr;

namespace
{
    // Reserved wait() tokens. Coroutine handle addresses are heap pointers and
    // never collide with these small constants (event_loop::wake uses 0).
    constexpr std::uint64_t kListenerToken = 1;

    inline std::coroutine_handle<> handle_from_token(std::uint64_t tok)
    {
        return std::coroutine_handle<>::from_address(
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(tok)));
    }

#ifndef SWIFTNET_PLATFORM_WINDOWS
    // Create a non-blocking SO_REUSEPORT listener so each engine can accept its
    // own share of connections (kernel-level connection sharding).
    int make_listener(std::uint16_t port, int backlog)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }
        if (listen(fd, backlog) < 0)
        {
            ::close(fd);
            return -1;
        }
        detail::platform::make_socket_nonblocking(fd);
        return fd;
    }
#endif
} // namespace

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

    engines_.clear();
    engines_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
    {
        auto e = std::make_unique<Engine>();
        e->id = i;
        e->loop = std::make_unique<event_loop>();
        e->arena = std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024);
        engines_.push_back(std::move(e));
    }

    running_ = true;
    for (std::size_t i = 0; i < ncores_; ++i)
        engines_[i]->thread = std::thread([this, i] { run(*engines_[i]); });

    SWIFTNET_LOG_INFO("scheduler online: {} per-core engines", ncores_);
}

void vthread_scheduler::stop()
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (!running_)
        return;
    running_ = false;

    for (auto &e : engines_)
        if (e->loop)
            e->loop->wake();
    for (auto &e : engines_)
        if (e->thread.joinable())
            e->thread.join();

    // Engines are stopped; tear down engine-local state (single-owner now).
    for (auto &e : engines_)
    {
        // Discard any not-yet-started injected roots (their dtors free frames).
        vthread t;
        while (e->inbox.pop(t)) { /* dtor destroys */ }
        e->completed.clear();
        e->roots.clear();   // destroys still-suspended root frames
        e->io_ops.clear();
#ifndef SWIFTNET_PLATFORM_WINDOWS
        int fd = e->listen_fd.load(std::memory_order_relaxed);
        if (fd >= 0)
            ::close(fd);
#endif
        e->loop.reset();
    }
    engines_.clear();
    SWIFTNET_LOG_INFO("scheduler stopped");
}

void vthread_scheduler::add_listener(std::uint16_t port, int backlog,
                                     std::function<vthread(net::tcp_socket)> on_accept)
{
#ifndef SWIFTNET_PLATFORM_WINDOWS
    std::lock_guard<std::mutex> lock(global_mutex_);
    for (auto &e : engines_)
    {
        int fd = make_listener(port, backlog);
        if (fd < 0)
        {
            SWIFTNET_LOG_ERROR("failed to create SO_REUSEPORT listener on port {}", port);
            continue;
        }
        e->on_accept = on_accept;                              // publish before fd
        e->listen_fd.store(fd, std::memory_order_release);     // engine arms it lazily
        if (e->loop)
            e->loop->wake();
    }
    SWIFTNET_LOG_INFO("listening on port {} across {} engines (SO_REUSEPORT)", port, ncores_);
#else
    (void)port; (void)backlog; (void)on_accept;
#endif
}

void vthread_scheduler::bind_core(std::size_t c)
{
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(c, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#else
    (void)c; // macOS/Apple Silicon: no true core pinning (KERN_NOT_SUPPORTED).
#endif
}

void vthread_scheduler::reap(Engine &e)
{
    if (e.completed.empty())
        return;
    auto done = std::move(e.completed);
    e.completed.clear();
    for (auto h : done)
    {
        auto it = e.roots.find(h.address());
        if (it != e.roots.end())
            e.roots.erase(it); // vthread dtor destroys the completed frame
    }
}

void vthread_scheduler::do_accept(Engine &e)
{
#ifndef SWIFTNET_PLATFORM_WINDOWS
    int lf = e.listen_fd.load(std::memory_order_relaxed);
    while (true)
    {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int cfd = detail::platform::platform_accept(lf, (sockaddr *)&addr, &len);
        if (cfd < 0)
            break; // EAGAIN / no more pending
        vthread task = e.on_accept(net::tcp_socket(cfd));
        auto h = task.handle();
        if (!h)
            continue;
        e.roots.emplace(h.address(), std::move(task));
        e.executed.fetch_add(1, std::memory_order_relaxed);
        if (!h.done())
            h.resume(); // run until it suspends on read (armed on this engine)
        reap(e);
    }
#else
    (void)e;
#endif
}

void vthread_scheduler::drain_inbox(Engine &e)
{
    vthread t;
    while (e.inbox.pop(t))
    {
        auto h = t.handle();
        if (!h)
            continue;
        e.roots.emplace(h.address(), std::move(t));
        e.executed.fetch_add(1, std::memory_order_relaxed);
        stat_ctxsw_.fetch_add(1, std::memory_order_relaxed);
        if (!h.done())
            h.resume();
        reap(e);
    }
}

void vthread_scheduler::run(Engine &e)
{
    t_engine_ = &e;
    bind_core(e.id);

    constexpr int kBatch = 256;
    io_event evs[kBatch];

    while (running_.load(std::memory_order_acquire))
    {
        // Lazily arm the listener handed to us by add_listener().
        if (!e.listener_armed)
        {
            int lf = e.listen_fd.load(std::memory_order_acquire);
            if (lf >= 0)
            {
                e.loop->arm(lf, READABLE, kListenerToken);
                e.listener_armed = true;
            }
        }

        int n = e.loop->wait(evs, kBatch, /*timeout_ms*/ -1);
        for (int i = 0; i < n; ++i)
        {
            std::uint64_t tok = evs[i].token;
            if (tok == 0)
                continue; // wake() nudge (e.g. shutdown / inbox inject / listener arm)
            if (tok == kListenerToken)
            {
                do_accept(e);
                int lf = e.listen_fd.load(std::memory_order_relaxed);
                if (lf >= 0)
                    e.loop->arm(lf, READABLE, kListenerToken); // re-arm one-shot
                continue;
            }
            auto h = handle_from_token(tok);
            auto it = e.io_ops.find(h.address());
            if (it != e.io_ops.end())
                it->second.result = evs[i].res;
            e.executed.fetch_add(1, std::memory_order_relaxed);
            stat_ctxsw_.fetch_add(1, std::memory_order_relaxed);
            stat_resumed_.fetch_add(1, std::memory_order_relaxed);
            if (!h.done())
                h.resume();
            reap(e);
        }

        drain_inbox(e); // cross-thread scheduled roots
    }
}

void vthread_scheduler::schedule(vthread t)
{
    if (!running_ || !t.valid())
        return;
    std::size_t i = next_engine_.fetch_add(1, std::memory_order_relaxed) % ncores_;
    Engine &e = *engines_[i];
    stat_scheduled_.fetch_add(1, std::memory_order_relaxed);
    e.inbox.push(std::move(t));
    if (e.loop)
        e.loop->wake();
}

void vthread_scheduler::schedule_with_affinity(vthread t, std::size_t preferred_engine)
{
    if (!running_ || !t.valid())
        return;
    std::size_t i = std::min(preferred_engine, ncores_ - 1);
    Engine &e = *engines_[i];
    stat_scheduled_.fetch_add(1, std::memory_order_relaxed);
    e.inbox.push(std::move(t));
    if (e.loop)
        e.loop->wake();
}

void vthread_scheduler::suspend_for_io(std::coroutine_handle<> h, int fd, std::uint32_t mask)
{
    Engine *e = t_engine_;
    if (!e)
        return;
    e->io_ops[h.address()] = IoOp{fd, mask, 0, false};
    stat_io_suspended_.fetch_add(1, std::memory_order_relaxed);
    if (e->loop)
        e->loop->arm(fd, mask, reinterpret_cast<std::uintptr_t>(h.address()));
}

void vthread_scheduler::suspend_for_timer(std::coroutine_handle<> h, int ms)
{
    Engine *e = t_engine_;
    if (!e)
        return;
    e->io_ops[h.address()] = IoOp{-1, 0, 0, true};
    stat_io_suspended_.fetch_add(1, std::memory_order_relaxed);
    if (e->loop)
        e->loop->arm_timer(reinterpret_cast<std::uintptr_t>(h.address()), ms);
}

int vthread_scheduler::take_io_result(std::coroutine_handle<> h)
{
    Engine *e = t_engine_;
    if (!e)
        return -1;
    auto it = e->io_ops.find(h.address());
    if (it == e->io_ops.end())
        return -1;
    int r = it->second.result;
    e->io_ops.erase(it);
    return r;
}

void vthread_scheduler::on_root_complete(std::coroutine_handle<> h) noexcept
{
    Engine *e = t_engine_;
    if (!e)
        return;
    try
    {
        e->completed.push_back(h);
        stat_completed_.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

std::pmr::memory_resource *vthread_scheduler::local_resource(std::size_t engine)
{
    if (engine >= engines_.size() || !engines_[engine]->arena)
        return std::pmr::get_default_resource();
    return engines_[engine]->arena.get();
}

auto vthread_scheduler::get_stats() const -> Stats
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    Stats s;
    s.total_scheduled = stat_scheduled_.load(std::memory_order_relaxed);
    s.total_io_suspended = stat_io_suspended_.load(std::memory_order_relaxed);
    s.total_resumed = stat_resumed_.load(std::memory_order_relaxed);
    s.work_stolen = stat_stolen_.load(std::memory_order_relaxed);
    s.context_switches = stat_ctxsw_.load(std::memory_order_relaxed);
    s.completed = stat_completed_.load(std::memory_order_relaxed);
    s.per_core_executed.reserve(engines_.size());
    for (const auto &e : engines_)
        s.per_core_executed.push_back(e->executed.load(std::memory_order_relaxed));
    return s;
}
