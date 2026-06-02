#ifndef swiftnet_detail_profiling_hpp
#define swiftnet_detail_profiling_hpp

// Compile-gated instrumentation. Entirely zero-cost unless SWIFTNET_PROFILE is
// defined (set via the CMake option of the same name). Used to settle, with
// data rather than narrative:
//   - Gate A: where on-CPU time goes (per-stage timing, hot-mutex wait time).
//   - Gate B / valve: per-core queue depth over time, total/steal counts.
//
// Usage:
//   SWIFTNET_PROF_SCOPE(handler);                 // time the enclosing scope
//   SWIFTNET_PROF_ADD(io_ops_lock_wait, ns);      // add a measured duration
//   SWIFTNET_PROF_QDEPTH(core, depth);            // record a queue-depth sample
//   swiftnet::profiling::dump();                   // emit a report (on shutdown)

#include <cstdint>

namespace swiftnet::profiling
{

    enum class metric
    {
        io_ops_lock_wait,
        roots_lock_wait,
        reactor_event,
        resume_from_io,
        parse,
        handler,
        write,
        COUNT
    };

#ifdef SWIFTNET_PROFILE

    std::uint64_t now_ns() noexcept;
    void add(metric m, std::uint64_t ns) noexcept;        // count + sum + max
    void qdepth_sample(std::size_t core, std::uint32_t depth) noexcept; // time series
    void dump() noexcept;                                  // print report to stderr

    struct scoped
    {
        metric m_;
        std::uint64_t t0_;
        explicit scoped(metric m) noexcept : m_(m), t0_(now_ns()) {}
        ~scoped() noexcept { add(m_, now_ns() - t0_); }
    };

    // RAII timed lock: records time spent blocked acquiring `mtx` into `which`.
    template <class Mutex>
    class timed_guard
    {
    public:
        timed_guard(Mutex &mtx, metric which) : mtx_(mtx)
        {
            std::uint64_t t0 = now_ns();
            mtx_.lock();
            add(which, now_ns() - t0);
        }
        ~timed_guard() { mtx_.unlock(); }
        timed_guard(const timed_guard &) = delete;

    private:
        Mutex &mtx_;
    };

#else
    inline void dump() noexcept {}
#endif

} // namespace swiftnet::profiling

#ifdef SWIFTNET_PROFILE
#define SWIFTNET_PROF_SCOPE(M) ::swiftnet::profiling::scoped _swprof_{::swiftnet::profiling::metric::M}
#define SWIFTNET_PROF_ADD(M, NS) ::swiftnet::profiling::add(::swiftnet::profiling::metric::M, (NS))
#define SWIFTNET_PROF_QDEPTH(CORE, DEPTH) ::swiftnet::profiling::qdepth_sample((CORE), (DEPTH))
// Timed lock_guard replacement for the hot mutexes.
#define SWIFTNET_PROF_LOCK(VAR, MTX, METRIC) ::swiftnet::profiling::timed_guard<std::remove_reference_t<decltype(MTX)>> VAR((MTX), ::swiftnet::profiling::metric::METRIC)
#else
#define SWIFTNET_PROF_SCOPE(M) ((void)0)
#define SWIFTNET_PROF_ADD(M, NS) ((void)0)
#define SWIFTNET_PROF_QDEPTH(CORE, DEPTH) ((void)0)
#define SWIFTNET_PROF_LOCK(VAR, MTX, METRIC) std::lock_guard<std::remove_reference_t<decltype(MTX)>> VAR(MTX)
#endif

#endif // swiftnet_detail_profiling_hpp
