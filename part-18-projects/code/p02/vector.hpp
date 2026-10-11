// Project 2 reference solution: Vector<T, Alloc>, an educational std::vector.
#pragma once
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace edu {

template <class T, class Alloc = std::allocator<T>>
class Vector {
    using Traits = std::allocator_traits<Alloc>;

public:
    using value_type = T;
    using allocator_type = Alloc;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    // ---- construction / destruction -------------------------------------------------
    Vector() noexcept(noexcept(Alloc())) = default;
    explicit Vector(const Alloc& a) noexcept : alloc_(a) {}
    Vector(std::initializer_list<T> il, const Alloc& a = Alloc()) : alloc_(a) {
        reserve(il.size());
        for (const T& x : il) push_back(x);   // basic guarantee inside a ctor: dtor not run, so guard below
    }
    explicit Vector(size_type n, const Alloc& a = Alloc()) : alloc_(a) {
        Vector tmp(a);                        // tmp's destructor cleans up if a T constructor throws
        tmp.reserve(n);
        for (size_type i = 0; i < n; ++i) tmp.emplace_back();
        swap(tmp);
    }

    Vector(const Vector& o)
        : alloc_(Traits::select_on_container_copy_construction(o.alloc_)) {
        Vector tmp(alloc_);
        tmp.reserve(o.size_);
        for (const T& x : o) tmp.emplace_back(x);
        swap(tmp);
    }

    Vector(Vector&& o) noexcept
        : alloc_(std::move(o.alloc_)),
          data_(std::exchange(o.data_, nullptr)),
          size_(std::exchange(o.size_, 0)),
          cap_(std::exchange(o.cap_, 0)) {}

    Vector& operator=(const Vector& o) {
        if (this == &o) return *this;
        Vector tmp(o);                         // copy-and-swap: strong guarantee (allocator POCCA ignored: see notes)
        swap(tmp);
        return *this;
    }

    Vector& operator=(Vector&& o) noexcept(Traits::propagate_on_container_move_assignment::value ||
                                           Traits::is_always_equal::value) {
        if (this == &o) return *this;
        if constexpr (Traits::propagate_on_container_move_assignment::value) {
            clear_and_free();
            alloc_ = std::move(o.alloc_);
            steal(o);
        } else if (alloc_ == o.alloc_) {
            clear_and_free();
            steal(o);
        } else {                                // different allocators, no propagation: element-wise move
            clear();
            reserve(o.size_);
            for (T& x : o) emplace_back(std::move(x));
            o.clear();
        }
        return *this;
    }

    ~Vector() { clear_and_free(); }

    // ---- observers ------------------------------------------------------------------
    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }
    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }
    allocator_type get_allocator() const noexcept { return alloc_; }
    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    reference operator[](size_type i) noexcept { return data_[i]; }
    const_reference operator[](size_type i) const noexcept { return data_[i]; }
    reference at(size_type i) { if (i >= size_) throw std::out_of_range("Vector::at"); return data_[i]; }
    reference back() noexcept { return data_[size_ - 1]; }

    // ---- modifiers ------------------------------------------------------------------
    void reserve(size_type n) {
        if (n <= cap_) return;
        reallocate(n);
    }

    template <class... Args>
    reference emplace_back(Args&&... args) {
        if (size_ == cap_) {
            // Args may refer to an element of *this (v.push_back(v[0])), so construct the new element
            // in the NEW buffer before the old one is destroyed.
            const size_type new_cap = grow_capacity();
            T* nb = Traits::allocate(alloc_, new_cap);
            try {
                Traits::construct(alloc_, nb + size_, std::forward<Args>(args)...);
            } catch (...) { Traits::deallocate(alloc_, nb, new_cap); throw; }
            transfer_into(nb, new_cap, /*constructed_extra=*/true);
        } else {
            Traits::construct(alloc_, data_ + size_, std::forward<Args>(args)...);
            ++size_;
        }
        return data_[size_ - 1];
    }
    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }
    void pop_back() noexcept { Traits::destroy(alloc_, data_ + --size_); }
    void clear() noexcept { destroy_range(data_, data_ + size_); size_ = 0; }

    void swap(Vector& o) noexcept {
        using std::swap;
        if constexpr (Traits::propagate_on_container_swap::value) swap(alloc_, o.alloc_);
        swap(data_, o.data_); swap(size_, o.size_); swap(cap_, o.cap_);
    }

private:
    size_type grow_capacity() const noexcept { return cap_ ? cap_ * 2 : 1; }

    void destroy_range(T* f, T* l) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            for (; f != l; ++f) Traits::destroy(alloc_, f);
    }

    void clear_and_free() noexcept {
        destroy_range(data_, data_ + size_);
        if (data_) Traits::deallocate(alloc_, data_, cap_);
        data_ = nullptr; size_ = cap_ = 0;
    }

    void steal(Vector& o) noexcept {
        data_ = std::exchange(o.data_, nullptr);
        size_ = std::exchange(o.size_, 0);
        cap_ = std::exchange(o.cap_, 0);
    }

    void reallocate(size_type new_cap) {
        T* nb = Traits::allocate(alloc_, new_cap);
        transfer_into(nb, new_cap, /*constructed_extra=*/false);
    }

    // Moves (if nothrow-movable) or copies (strong guarantee) the old elements to `nb`.
    // `constructed_extra`: one new element already lives at nb[size_]; count it on success.
    void transfer_into(T* nb, size_type new_cap, bool constructed_extra) {
        size_type done = 0;
        try {
            for (; done < size_; ++done)
                Traits::construct(alloc_, nb + done, std::move_if_noexcept(data_[done]));
        } catch (...) {
            for (size_type i = 0; i < done; ++i) Traits::destroy(alloc_, nb + i);
            if (constructed_extra) Traits::destroy(alloc_, nb + size_);
            Traits::deallocate(alloc_, nb, new_cap);
            throw;                                 // old buffer untouched: strong guarantee
        }
        const size_type old_size = size_;
        destroy_range(data_, data_ + old_size);
        if (data_) Traits::deallocate(alloc_, data_, cap_);
        data_ = nb; cap_ = new_cap; size_ = old_size + (constructed_extra ? 1 : 0);
    }

    [[no_unique_address]] Alloc alloc_{};
    T* data_ = nullptr;      // owned: allocated with alloc_, [data_, data_+size_) are live objects
    size_type size_ = 0;
    size_type cap_ = 0;
};

}  // namespace edu
