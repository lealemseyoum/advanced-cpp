// Project 4 reference solution: Function<R(Args...)> -- move-only, small-buffer-optimised, manual vtable.
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace edu {

template <class Sig> class Function;

template <class R, class... Args>
class Function<R(Args...)> {
    static constexpr std::size_t kBuf = 3 * sizeof(void*);       // 24 bytes of inline storage
    static constexpr std::size_t kAlign = alignof(std::max_align_t);

    struct VTable {
        R    (*invoke )(void* self, Args&&... args);
        void (*move   )(void* dst, void* src) noexcept;          // relocate: construct in dst, destroy src
        void (*destroy)(void* self) noexcept;
        bool  inline_storage;
    };

    template <class F>
    static constexpr bool fits_inline =
        sizeof(F) <= kBuf && alignof(F) <= kAlign && std::is_nothrow_move_constructible_v<F>;

    // ---- vtable instances, one per stored type, as constexpr statics --------------------------
    template <class F> static constexpr VTable inline_vt = {
        [](void* s, Args&&... a) -> R { return std::invoke(*static_cast<F*>(s), std::forward<Args>(a)...); },
        [](void* d, void* s) noexcept { ::new (d) F(std::move(*static_cast<F*>(s))); static_cast<F*>(s)->~F(); },
        [](void* s) noexcept { static_cast<F*>(s)->~F(); },
        true,
    };
    template <class F> static constexpr VTable heap_vt = {
        [](void* s, Args&&... a) -> R { return std::invoke(**static_cast<F**>(s), std::forward<Args>(a)...); },
        [](void* d, void* s) noexcept { *static_cast<F**>(d) = *static_cast<F**>(s); },   // just move the pointer
        [](void* s) noexcept { delete *static_cast<F**>(s); },
        false,
    };

public:
    Function() noexcept = default;
    Function(std::nullptr_t) noexcept {}

    template <class F, class D = std::decay_t<F>>
        requires(!std::is_same_v<D, Function> && std::is_invocable_r_v<R, D&, Args...>)
    Function(F&& f) {
        if constexpr (fits_inline<D>) {
            ::new (static_cast<void*>(buf_)) D(std::forward<F>(f));
            vt_ = &inline_vt<D>;
        } else {
            *reinterpret_cast<D**>(buf_) = new D(std::forward<F>(f));    // owned: freed by heap_vt::destroy
            vt_ = &heap_vt<D>;
        }
    }

    Function(Function&& o) noexcept : vt_(std::exchange(o.vt_, nullptr)) {
        if (vt_) vt_->move(buf_, o.buf_);
    }
    Function& operator=(Function&& o) noexcept {
        if (this != &o) {
            reset();
            vt_ = std::exchange(o.vt_, nullptr);
            if (vt_) vt_->move(buf_, o.buf_);
        }
        return *this;
    }
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;
    ~Function() { reset(); }

    explicit operator bool() const noexcept { return vt_ != nullptr; }
    R operator()(Args... args) const {
        if (!vt_) throw std::bad_function_call();
        return vt_->invoke(const_cast<unsigned char*>(buf_), std::forward<Args>(args)...);
    }
    bool is_inline() const noexcept { return vt_ && vt_->inline_storage; }

private:
    void reset() noexcept { if (vt_) { vt_->destroy(buf_); vt_ = nullptr; } }

    alignas(kAlign) unsigned char buf_[kBuf];
    const VTable* vt_ = nullptr;
};

}  // namespace edu
