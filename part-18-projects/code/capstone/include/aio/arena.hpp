// Arena (bump) memory resource, from Project 5. Not thread-safe: one arena per connection/loop.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <utility>

namespace aio {

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


}  // namespace aio
