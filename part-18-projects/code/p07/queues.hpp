// Project 7 reference solution: four queues, from simplest to hardest.
//   MutexQueue<T>      unbounded, mutex + condvar
//   BoundedQueue<T>    bounded, blocking push/pop, close()
//   SpscRing<T>        lock-free single-producer/single-consumer ring (acquire/release)
//   MpmcRing<T>        lock-free bounded multi-producer/multi-consumer (Vyukov sequence-number design)
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

namespace edu {

inline constexpr std::size_t kCacheLine = 64;   // std::hardware_destructive_interference_size is not ABI-stable: see Chapter 27

// ---------------------------------------------------------------------------------------------
template <class T>
class MutexQueue {
public:
    void push(T v) { { std::lock_guard lk(m_); q_.push_back(std::move(v)); } cv_.notify_one(); }
    std::optional<T> try_pop() {
        std::lock_guard lk(m_);
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front()); q_.pop_front(); return v;
    }
    T pop() {                                     // blocks
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        T v = std::move(q_.front()); q_.pop_front(); return v;
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
};

// ---------------------------------------------------------------------------------------------
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t cap) : cap_(cap) {}
    // Blocks while full. Returns false if the queue was closed (the value is not enqueued).
    bool push(T v) {
        std::unique_lock lk(m_);
        not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return false;
        q_.push_back(std::move(v));
        lk.unlock();
        not_empty_.notify_one();
        return true;
    }
    // Blocks while empty. Returns nullopt only when closed AND drained.
    std::optional<T> pop() {
        std::unique_lock lk(m_);
        not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front()); q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return v;
    }
    void close() { { std::lock_guard lk(m_); closed_ = true; } not_full_.notify_all(); not_empty_.notify_all(); }
private:
    std::mutex m_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> q_;
    const std::size_t cap_;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------------------------
// Single producer, single consumer. Capacity is rounded up to a power of two; one slot is kept empty.
// Orders: the producer's release-store of head_ publishes the element written before it; the consumer's
// acquire-load of head_ makes that write visible. Each index is written by exactly one thread, so
// no read-modify-write is needed at all.
template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t cap) : mask_(round_up(cap + 1) - 1), buf_(new Slot[mask_ + 1]) {}
    ~SpscRing() {
        for (std::size_t i = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_relaxed); i != h; i = (i + 1) & mask_)
            buf_[i].ptr()->~T();
    }
    bool try_push(T v) {
        const std::size_t h = head_.load(std::memory_order_relaxed);          // only we write head_
        const std::size_t next = (h + 1) & mask_;
        if (next == tail_cache_) {                                             // looks full: refresh our view of tail_
            tail_cache_ = tail_.load(std::memory_order_acquire);               // acquire: the consumer finished reading the slot
            if (next == tail_cache_) return false;
        }
        ::new (buf_[h].raw) T(std::move(v));
        head_.store(next, std::memory_order_release);                          // publish
        return true;
    }
    std::optional<T> try_pop() {
        const std::size_t t = tail_.load(std::memory_order_relaxed);           // only we write tail_
        if (t == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);               // acquire: see the element written before the release
            if (t == head_cache_) return std::nullopt;
        }
        T v = std::move(*buf_[t].ptr());
        buf_[t].ptr()->~T();
        tail_.store((t + 1) & mask_, std::memory_order_release);               // hand the slot back
        return v;
    }
private:
    struct Slot { alignas(T) unsigned char raw[sizeof(T)]; T* ptr() { return std::launder(reinterpret_cast<T*>(raw)); } };
    static std::size_t round_up(std::size_t n) { std::size_t p = 2; while (p < n) p <<= 1; return p; }
    const std::size_t mask_;
    std::unique_ptr<Slot[]> buf_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};      // written by producer
    alignas(kCacheLine) std::size_t tail_cache_ = 0;            // producer's cached copy of tail_ (private to the producer)
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};      // written by consumer
    alignas(kCacheLine) std::size_t head_cache_ = 0;            // consumer's cached copy of head_ (private to the consumer)
};

// ---------------------------------------------------------------------------------------------
// Bounded MPMC queue after Dmitry Vyukov. Each cell carries a sequence number that says whose turn it is.
//   cell.seq == pos          : empty, a producer holding ticket `pos` may write
//   cell.seq == pos + 1      : full,  a consumer holding ticket `pos` may read
//   after read, seq = pos + capacity : empty for the next lap
// Producers/consumers claim tickets with a CAS on enqueue_pos_/dequeue_pos_. ABA is avoided because the
// sequence number grows monotonically (a lap counter), not because of hazard pointers: the cells are never freed.
template <class T>
class MpmcRing {
public:
    explicit MpmcRing(std::size_t cap) : mask_(round_up(cap) - 1), cells_(new Cell[mask_ + 1]) {
        for (std::size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }
    ~MpmcRing() { while (try_pop()) {} }
    bool try_push(T v) {
        std::size_t pos = enq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {     // claim the ticket
                    ::new (c.raw) T(std::move(v));
                    c.seq.store(pos + 1, std::memory_order_release);                           // publish to consumers
                    return true;
                }                                                                              // CAS failed: pos was reloaded, retry
            } else if (diff < 0) {
                return false;                                                                  // cell still holds last lap's value: full
            } else {
                pos = enq_.load(std::memory_order_relaxed);                                    // another producer got ahead
            }
        }
    }
    std::optional<T> try_pop() {
        std::size_t pos = deq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    T v = std::move(*c.ptr());
                    c.ptr()->~T();
                    c.seq.store(pos + mask_ + 1, std::memory_order_release);                   // free the cell for the next lap
                    return v;
                }
            } else if (diff < 0) {
                return std::nullopt;                                                           // empty
            } else {
                pos = deq_.load(std::memory_order_relaxed);
            }
        }
    }
private:
    struct Cell {
        std::atomic<std::size_t> seq;
        alignas(T) unsigned char raw[sizeof(T)];
        T* ptr() { return std::launder(reinterpret_cast<T*>(raw)); }
    };
    static std::size_t round_up(std::size_t n) { std::size_t p = 2; while (p < n) p <<= 1; return p; }
    const std::size_t mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(kCacheLine) std::atomic<std::size_t> enq_{0};
    alignas(kCacheLine) std::atomic<std::size_t> deq_{0};
};

}  // namespace edu
