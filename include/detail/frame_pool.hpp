#ifndef swiftnet_detail_frame_pool_hpp
#define swiftnet_detail_frame_pool_hpp

// Thread-local coroutine-frame allocator. Gate A profiling ranked malloc/free of
// coroutine frames (3-4 per HTTP request) high among on-CPU costs. SwiftNet runs
// a connection's coroutines on a single engine thread, so frames are allocated
// and freed on the same thread -- an intrusive thread-local free-list reuses them
// with no locking and no malloc/free churn on the hot path.
//
// NOTE (measured, macOS/arm64): on Apple Silicon this is at best a wash and was a
// ~2% regression in the vector-backed form -- macOS libmalloc's per-thread
// magazine cache already serves same-size same-thread frames about as fast. The
// pool is therefore OFF by default on macOS (see vthread.hpp gating) and kept for
// platforms whose default allocator is weaker for this pattern (e.g. glibc) where
// it remains UNVERIFIED. Build with -DSWIFTNET_FORCE_FRAME_POOL to A/B it.
//
// Cross-thread safety (a frame created on one thread, destroyed on another -- e.g.
// schedule()'d compute tasks, or teardown on the main thread): each block is
// individually ::operator new'd and carries a header with its owning pool.
// deallocate() caches only when the owning pool is the current thread's (pointer
// compare, never dereferencing a foreign/dead pool); otherwise it ::operator
// delete's the block, which is always valid.

#include <cstddef>
#include <cstdint>
#include <new>

namespace swiftnet::detail
{

    class frame_pool
    {
        static constexpr std::size_t kStep = 64;        // size-class granularity
        static constexpr std::size_t kMaxBlock = 8192;  // cache blocks up to 8 KiB
        static constexpr std::size_t kNClasses = kMaxBlock / kStep;
        static constexpr std::uint32_t kCapPerClass = 1024; // bounded reuse cache

        struct Header
        {
            frame_pool *owner;       // compared, never dereferenced cross-thread
            std::size_t block_size;  // exact ::operator new size; 0 = uncached (oversized)
        };
        static_assert(sizeof(Header) == 16, "Header must keep 16-byte frame alignment");

        void *free_[kNClasses] = {};        // intrusive free-list heads (next ptr in block)
        std::uint32_t count_[kNClasses] = {}; // per-class cached count (bounded by kCapPerClass)

        static std::size_t class_of(std::size_t block) { return (block - 1) / kStep; }

    public:
        ~frame_pool()
        {
            for (std::size_t c = 0; c < kNClasses; ++c)
            {
                void *p = free_[c];
                while (p)
                {
                    void *next = *static_cast<void **>(p);
                    ::operator delete(p);
                    p = next;
                }
            }
        }

        static frame_pool &local()
        {
            thread_local frame_pool p;
            return p;
        }

        void *allocate(std::size_t user_size)
        {
            std::size_t block = sizeof(Header) + user_size;
            if (block <= kMaxBlock)
            {
                std::size_t c = class_of(block);
                void *mem;
                if (free_[c])
                {
                    mem = free_[c];
                    free_[c] = *static_cast<void **>(mem); // pop intrusive head
                    --count_[c];
                }
                else
                {
                    mem = ::operator new((c + 1) * kStep);
                }
                auto *h = static_cast<Header *>(mem);
                h->owner = this;
                h->block_size = (c + 1) * kStep;
                return static_cast<char *>(mem) + sizeof(Header);
            }
            void *mem = ::operator new(block);
            auto *h = static_cast<Header *>(mem);
            h->owner = this;
            h->block_size = 0; // oversized: never cached
            return static_cast<char *>(mem) + sizeof(Header);
        }

        static void deallocate(void *user_ptr) noexcept
        {
            void *mem = static_cast<char *>(user_ptr) - sizeof(Header);
            auto *h = static_cast<Header *>(mem);
            frame_pool *owner = h->owner;
            std::size_t bs = h->block_size;
            if (bs != 0 && owner == &local()) // our own thread's pool (no deref of foreign pool)
            {
                std::size_t c = class_of(bs);
                if (owner->count_[c] < kCapPerClass)
                {
                    *static_cast<void **>(mem) = owner->free_[c]; // push intrusive head
                    owner->free_[c] = mem;
                    ++owner->count_[c];
                    return;
                }
            }
            ::operator delete(mem);
        }
    };

} // namespace swiftnet::detail

#endif
