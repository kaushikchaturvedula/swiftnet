#ifndef mpsc_queue_hpp
#define mpsc_queue_hpp

#include <atomic>
#include <cstddef>
#include <utility>
#include <optional>

namespace swiftnet::detail
{

    /* Multiple-producer / single-consumer intrusive queue.
     * Producer: lock-free push()
     * Consumer: pop() - single thread.
     */
    template <typename T>
    class mpsc_queue
    {
        struct node
        {
            std::optional<T> value;
            std::atomic<node *> next{nullptr};
            
            node() = default;  // Dummy node constructor
            explicit node(T v) : value(std::move(v)) {}
        };

        std::atomic<node *> tail_;
        node *head_; // consumer-only

    public:
        mpsc_queue()
        {
            auto *dummy = new node();  // Default construct dummy node
            head_ = dummy;
            tail_.store(dummy, std::memory_order_relaxed);
        }

        // Delete copy constructor and assignment
        mpsc_queue(const mpsc_queue&) = delete;
        mpsc_queue& operator=(const mpsc_queue&) = delete;

        // Move constructor
        mpsc_queue(mpsc_queue&& other) noexcept
            : tail_(other.tail_.exchange(nullptr, std::memory_order_relaxed)), 
              head_(other.head_)
        {
            other.head_ = nullptr;
        }

        // Move assignment
        mpsc_queue& operator=(mpsc_queue&& other) noexcept
        {
            if (this != &other) {
                // Clean up current queue
                clear();
                
                // Take ownership of other's data
                tail_.store(other.tail_.exchange(nullptr, std::memory_order_relaxed), std::memory_order_relaxed);
                head_ = other.head_;
                other.head_ = nullptr;
            }
            return *this;
        }

        ~mpsc_queue()
        {
            clear();
        }

        void push(T v) noexcept
        {
            auto *n = new node(std::move(v));
            n->next.store(nullptr, std::memory_order_relaxed);
            node *prev = tail_.exchange(n, std::memory_order_acq_rel);
            if (prev) {
                prev->next.store(n, std::memory_order_release);
            }
        }

        bool pop(T &out) noexcept
        {
            if (!head_) return false;
            
            node *next = head_->next.load(std::memory_order_acquire);
            if (!next) return false;
            
            if (next->value.has_value()) {
                out = std::move(next->value.value());
            } else {
                return false;
            }
            
            delete head_;
            head_ = next;
            return true;
        }

        bool empty() const noexcept
        {
            return !head_ || head_->next.load(std::memory_order_acquire) == nullptr;
        }

    private:
        void clear() noexcept
        {
            node* current = head_;
            head_ = nullptr;
            tail_.store(nullptr, std::memory_order_relaxed);
            
            while (current) {
                node *next = current->next.load(std::memory_order_relaxed);
                delete current;
                current = next;
            }
        }
    };

}

#endif
