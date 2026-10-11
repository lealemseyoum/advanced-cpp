// Project 5 reference solution: Arena (bump), Pool (fixed-size free list), and PMR adapters.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <utility>

namespace edu {

constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept { return (n + a - 1) & ~(a - 1); }

// ---- Arena: bump allocator over chunks obtained from an upstream resource -------------------
class Arena final : public std::pmr::memory_resource {
public:
    explicit Arena(std::size_t chunk_size = 64 * 1024,
                   std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(upstream), chunk_size_(chunk_size) {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() override { release(); }

    // Frees all chunks. All objects allocated from the arena become invalid; destructors are NOT run.
    void release() noexcept {
        while (head_) {
            Chunk* next = head_->next;
            upstream_->deallocate(head_, head_->total, alignof(Chunk));
            head_ = next;
        }
        cur_ = end_ = nullptr;
    }
    // Keeps the first chunk for reuse.
    void reset() noexcept {
        if (!head_) return;
        while (head_->next) {
            Chunk* next = head_->next;
            upstream_->deallocate(head_, head_->total, alignof(Chunk));
            head_ = next;
        }
        cur_ = reinterpret_cast<std::byte*>(head_ + 1);
        end_ = reinterpret_cast<std::byte*>(head_) + head_->total;
    }
    std::size_t chunks() const noexcept { std::size_t n = 0; for (Chunk* c = head_; c; c = c->next) ++n; return n; }

private:
    struct Chunk { Chunk* next; std::size_t total; };   // header lives at the start of each chunk

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        if (void* p = try_bump(bytes, align)) return p;
        // need a new chunk: big enough for the request plus worst-case alignment padding
        std::size_t need = sizeof(Chunk) + align + bytes;
        std::size_t total = need > chunk_size_ ? need : chunk_size_;
        auto* c = static_cast<Chunk*>(upstream_->allocate(total, alignof(Chunk)));
        c->next = head_; c->total = total; head_ = c;
        cur_ = reinterpret_cast<std::byte*>(c + 1);
        end_ = reinterpret_cast<std::byte*>(c) + total;
        return try_bump(bytes, align);                  // cannot fail now
    }
    void do_deallocate(void*, std::size_t, std::size_t) noexcept override {}   // individual frees are no-ops
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }

    void* try_bump(std::size_t bytes, std::size_t align) noexcept {
        auto p = reinterpret_cast<std::uintptr_t>(cur_);
        std::uintptr_t aligned = (p + align - 1) & ~(std::uintptr_t(align) - 1);
        if (!cur_ || aligned + bytes > reinterpret_cast<std::uintptr_t>(end_)) return nullptr;
        cur_ = reinterpret_cast<std::byte*>(aligned + bytes);
        return reinterpret_cast<void*>(aligned);
    }

    std::pmr::memory_resource* upstream_;
    std::size_t chunk_size_;
    Chunk* head_ = nullptr;
    std::byte* cur_ = nullptr;
    std::byte* end_ = nullptr;
};

// ---- Pool<T>: fixed-size blocks, O(1) allocate/free via an intrusive free list ----------------
template <class T>
class Pool {
    union Node { Node* next; alignas(T) std::byte storage[sizeof(T)]; };
public:
    explicit Pool(std::size_t block_count = 1024) : per_chunk_(block_count) {}
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    ~Pool() { for (Chunk* c = chunks_; c;) { Chunk* n = c->next; ::operator delete(c, kAlign); c = n; } }

    void* allocate() {
        if (!free_) grow();
        Node* n = free_; free_ = n->next; ++live_;
        return n->storage;
    }
    void deallocate(void* p) noexcept {
        Node* n = static_cast<Node*>(p);   // storage is the first member of the union: same address
        n->next = free_; free_ = n; --live_;
    }
    template <class... A> T* create(A&&... a) {
        void* p = allocate();
        try { return ::new (p) T(std::forward<A>(a)...); } catch (...) { deallocate(p); throw; }
    }
    void destroy(T* p) noexcept { p->~T(); deallocate(p); }
    std::size_t live() const noexcept { return live_; }

private:
    struct Chunk { Chunk* next; };
    static constexpr std::align_val_t kAlign{alignof(Node) > alignof(Chunk) ? alignof(Node) : alignof(Chunk)};
    void grow() {
        std::size_t header = align_up(sizeof(Chunk), alignof(Node));
        auto* raw = static_cast<std::byte*>(::operator new(header + per_chunk_ * sizeof(Node), kAlign));
        auto* c = reinterpret_cast<Chunk*>(raw); c->next = chunks_; chunks_ = c;
        Node* nodes = reinterpret_cast<Node*>(raw + header);
        for (std::size_t i = 0; i < per_chunk_; ++i) { nodes[i].next = free_; free_ = &nodes[i]; }
    }
    std::size_t per_chunk_;
    Chunk* chunks_ = nullptr;
    Node* free_ = nullptr;
    std::size_t live_ = 0;
};

// ---- PoolResource: a memory_resource that serves fixed-size requests from Pool-like free lists -----
// Size classes: 16, 32, 64, 128, 256 bytes; larger requests go to upstream.
class PoolResource final : public std::pmr::memory_resource {
public:
    explicit PoolResource(std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(upstream), arena_(32 * 1024, upstream) {}
    std::size_t upstream_allocations() const noexcept { return big_; }

private:
    static constexpr int kClasses = 5;
    static int class_of(std::size_t n) noexcept { int c = 0; std::size_t s = 16; while (s < n) { s <<= 1; ++c; } return c; }
    struct Free { Free* next; };

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        if (bytes > 256 || align > 16) { ++big_; return upstream_->allocate(bytes, align); }
        int c = class_of(bytes);
        if (Free* f = free_[c]) { free_[c] = f->next; return f; }
        return arena_.allocate(std::size_t(16) << c, 16);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) noexcept override {
        if (bytes > 256 || align > 16) { upstream_->deallocate(p, bytes, align); return; }
        int c = class_of(bytes);
        auto* f = static_cast<Free*>(p); f->next = free_[c]; free_[c] = f;
    }
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }

    std::pmr::memory_resource* upstream_;
    Arena arena_;
    Free* free_[kClasses] = {};
    std::size_t big_ = 0;
};

}  // namespace edu
