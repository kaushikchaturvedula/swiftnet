#ifndef swiftnet_detail_work_queue_hpp
#define swiftnet_detail_work_queue_hpp

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <vector>

namespace swiftnet::detail
{

    // Lock-free Chase-Lev work-stealing deque of coroutine handles.
    //
    // The owning worker push()es and pop()s at the bottom (LIFO -> warm cache);
    // thieves steal() from the top (FIFO). Implementation follows Lê, Pop,
    // Cohen & Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
    // Models" (PPoPP 2013): atomic slot buffer with relaxed put/get plus
    // seq_cst fences and a CAS on `top_`, which is correct under the C++ memory
    // model and clean under ThreadSanitizer. The buffer grows on overflow; old
    // buffers are retired to `garbage_` (owner-only) and freed at destruction,
    // by which point no thief is running (the scheduler stops workers first).
    class work_queue
    {
        using T = std::coroutine_handle<>;

        struct ring
        {
            explicit ring(std::int64_t cap)
                : cap_(cap), mask_(cap - 1), slots_(new std::atomic<void *>[cap]) {}
            ~ring() { delete[] slots_; }

            std::int64_t cap_;
            std::int64_t mask_;
            std::atomic<void *> *slots_;

            void put(std::int64_t i, T x) noexcept
            {
                slots_[i & mask_].store(x.address(), std::memory_order_relaxed);
            }
            T get(std::int64_t i) const noexcept
            {
                return T::from_address(slots_[i & mask_].load(std::memory_order_relaxed));
            }
            ring *grow(std::int64_t b, std::int64_t t) const
            {
                ring *r = new ring(cap_ * 2);
                for (std::int64_t i = t; i < b; ++i)
                    r->put(i, get(i));
                return r;
            }
        };

        std::atomic<std::int64_t> top_{0};
        std::atomic<std::int64_t> bottom_{0};
        std::atomic<ring *> array_;
        std::vector<ring *> garbage_; // retired rings, freed in dtor (owner-only)

    public:
        explicit work_queue(std::int64_t cap = 1024) : array_(new ring(cap)) {}
        work_queue(const work_queue &) = delete;
        work_queue &operator=(const work_queue &) = delete;
        ~work_queue()
        {
            delete array_.load(std::memory_order_relaxed);
            for (ring *r : garbage_)
                delete r;
        }

        // --- owner only ---
        void push(T x)
        {
            std::int64_t b = bottom_.load(std::memory_order_relaxed);
            std::int64_t t = top_.load(std::memory_order_acquire);
            ring *a = array_.load(std::memory_order_relaxed);
            if (b - t > a->cap_ - 1)
            {
                ring *na = a->grow(b, t);
                garbage_.push_back(a);
                array_.store(na, std::memory_order_release);
                a = na;
            }
            a->put(b, x);
            std::atomic_thread_fence(std::memory_order_release);
            bottom_.store(b + 1, std::memory_order_relaxed);
        }

        // --- owner only ---
        bool pop(T &out)
        {
            std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
            ring *a = array_.load(std::memory_order_relaxed);
            bottom_.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::int64_t t = top_.load(std::memory_order_relaxed);

            if (t <= b)
            {
                T x = a->get(b);
                if (t == b)
                {
                    // Last element: race against a concurrent steal for it.
                    bool won = top_.compare_exchange_strong(
                        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    if (!won)
                        return false;
                    out = x;
                    return true;
                }
                out = x;
                return true;
            }
            // Empty.
            bottom_.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        // --- any thief ---
        bool steal(T &out)
        {
            std::int64_t t = top_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::int64_t b = bottom_.load(std::memory_order_acquire);
            if (t < b)
            {
                ring *a = array_.load(std::memory_order_acquire);
                T x = a->get(t);
                if (top_.compare_exchange_strong(
                        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    out = x;
                    return true;
                }
                return false; // lost the race to another thief / the owner
            }
            return false; // empty
        }

        bool empty() const
        {
            std::int64_t b = bottom_.load(std::memory_order_relaxed);
            std::int64_t t = top_.load(std::memory_order_relaxed);
            return b <= t;
        }
    };

} // namespace swiftnet::detail

#endif
