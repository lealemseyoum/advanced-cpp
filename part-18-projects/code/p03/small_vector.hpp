// Project 3 reference solution: SmallVector<T, N> with inline storage.
#pragma once
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace edu {

template <class T, std::size_t N>
class SmallVector {
    static_assert(N > 0, "use Vector for N == 0");

public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVector() noexcept = default;
    SmallVector(const SmallVector& o) {
        reserve(o.size_);
        for (const T& x : o) emplace_back(x);          // ~SmallVector not run if this throws: guard
    }
    SmallVector(SmallVector&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (!o.is_inline()) {                          // heap buffer: steal it, O(1), noexcept
            data_ = std::exchange(o.data_, o.inline_ptr());
            size_ = std::exchange(o.size_, 0);
            cap_ = std::exchange(o.cap_, N);
        } else {                                       // inline: elements must be moved one by one
            for (T& x : o) emplace_back(std::move(x));
            o.clear();
        }
    }
    SmallVector& operator=(SmallVector o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        // by-value parameter: copy-and-swap would need a swap that handles inline buffers; do it by moves
        clear();
        free_heap();
        data_ = inline_ptr(); cap_ = N;
        if (!o.is_inline()) {
            data_ = std::exchange(o.data_, o.inline_ptr());
            size_ = std::exchange(o.size_, 0);
            cap_ = std::exchange(o.cap_, N);
        } else {
            for (T& x : o) emplace_back(std::move(x));
            o.clear();
        }
        return *this;
    }
    ~SmallVector() { clear(); free_heap(); }

    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return cap_; }
    bool is_inline() const noexcept { return data_ == inline_ptr(); }
    T* data() noexcept { return data_; }
    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    T& operator[](size_type i) noexcept { return data_[i]; }
    const T& operator[](size_type i) const noexcept { return data_[i]; }

    void reserve(size_type n) { if (n > cap_) grow_to(n); }

    template <class... A>
    T& emplace_back(A&&... a) {
        if (size_ == cap_) {
            // construct the new element first (it may alias an existing one), then relocate the rest
            const size_type new_cap = cap_ * 2;
            T* nb = static_cast<T*>(::operator new(new_cap * sizeof(T), std::align_val_t{alignof(T)}));
            try { ::new (static_cast<void*>(nb + size_)) T(std::forward<A>(a)...); }
            catch (...) { ::operator delete(nb, std::align_val_t{alignof(T)}); throw; }
            relocate_into(nb, new_cap, true);
        } else {
            ::new (static_cast<void*>(data_ + size_)) T(std::forward<A>(a)...);
            ++size_;
        }
        return data_[size_ - 1];
    }
    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }
    void pop_back() noexcept { data_[--size_].~T(); }
    void clear() noexcept { std::destroy(begin(), end()); size_ = 0; }

private:
    // No std::launder: before elements are constructed there is no T object at buf_ to launder to.
    // Elements are only ever created with placement new at these addresses and accessed through them.
    T* inline_ptr() noexcept { return reinterpret_cast<T*>(buf_); }
    const T* inline_ptr() const noexcept { return reinterpret_cast<const T*>(buf_); }

    void grow_to(size_type n) {
        T* nb = static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
        relocate_into(nb, n, false);
    }

    void relocate_into(T* nb, size_type new_cap, bool extra) {
        size_type done = 0;
        try {
            for (; done < size_; ++done) ::new (static_cast<void*>(nb + done)) T(std::move_if_noexcept(data_[done]));
        } catch (...) {
            std::destroy(nb, nb + done);
            if (extra) nb[size_].~T();
            ::operator delete(nb, std::align_val_t{alignof(T)});
            throw;
        }
        const size_type old = size_;
        std::destroy(begin(), end());
        free_heap();
        data_ = nb; cap_ = new_cap; size_ = old + (extra ? 1 : 0);
    }

    void free_heap() noexcept {
        if (!is_inline()) ::operator delete(data_, std::align_val_t{alignof(T)});
        data_ = inline_ptr(); cap_ = N;
    }

    alignas(T) unsigned char buf_[N * sizeof(T)];     // raw storage: NO T objects live here yet
    T* data_ = reinterpret_cast<T*>(buf_);   // owned: points at buf_ (inline) or at an operator new block (heap)
    size_type size_ = 0;
    size_type cap_ = N;
};

}  // namespace edu
