# Project 4 — Type-Erased Function

[← Project 3](project-03-small-vector.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 5 — Memory arena →](project-05-memory-arena.md)

> **Level 3 · ≈ 6 hours · Builds on:** Chapters 7 (forwarding, `invoke`), 21 (type erasure), and Project 3 (inline storage)
> **Reference solution:** [`code/p04/function.hpp`](code/p04/function.hpp), [`test.cpp`](code/p04/test.cpp), [`bench.cpp`](code/p04/bench.cpp)

## Goal

Build `Function<R(Args...)>`, a type-erased callable wrapper conceptually similar to `std::function`, but **move-only** and with a small-buffer optimisation you control. You will implement type erasure the way production code does it: a hand-built vtable of function pointers, not a class hierarchy.

## Specification

```cpp
edu::Function<int(int)> f = [k](int x) { return x + k; };
```

1. Holds any callable `F` for which `std::is_invocable_r_v<R, F&, Args...>`: function pointers, lambdas, functors, member-function pointers (via `std::invoke`).
2. **Move-only.** This is the one deliberate difference from `std::function`, and it is what allows holding `unique_ptr` captures. (C++23 standardised the same idea as `std::move_only_function`.)
3. **Small-buffer optimisation:** callables up to 3 pointers in size, with alignment ≤ `max_align_t`, and a **`noexcept` move constructor**, are stored inline. Anything else goes on the heap. *No allocation for small callables*, proven with a counting `operator new`.
4. **`Function`'s own move constructor and move assignment are `noexcept`**, even when the stored callable's move could throw. (How? Store such callables on the heap; then moving the `Function` moves a pointer.)
5. Calling an empty `Function` throws `std::bad_function_call`.
6. Destruction runs the callable's destructor exactly once, whichever storage it uses.
7. The erased operations (`invoke`, `relocate`, `destroy`) live in **one `constexpr` vtable per stored type**, referenced by a single pointer in the object.

## Milestones

1. A heap-only version with the vtable (`invoke`, `destroy`). Make all the lifetime tests pass.
2. Add `relocate` (move-construct into the destination, destroy the source), so that moving a `Function` does not need to know the stored type.
3. Add the inline buffer and the `fits_inline` predicate. Verify allocation counts.
4. Add the `noexcept`-move rule and the `ThrowsOnMove` test.
5. Constrain the converting constructor so that `Function` does not try to wrap *itself* and so that non-callable types give a clean error. (Use a `requires` clause.)
6. Benchmark against `std::function`.

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| Converting constructor hijacks the move constructor | Infinite wrapping; `Function(Function&&)` wraps the other `Function` in a heap node |
| Callable copied instead of moved into storage | `Probe`-style counters show extra constructions; fails for move-only callables |
| Inline callable relocated with `memcpy` | Breaks for types with self-pointers (`std::string` with SSO in libstdc++) |
| Heap callable deleted twice after a move | ASan double-free |
| `vt_` not cleared on the moved-from object | Double destruction |
| `operator()` takes `Args&&` | Does not compile for by-value parameters; or forwards a moved-from argument twice |
| Missing `const_cast` reasoning | `operator()` const on a stateful callable: see "Think about" |

## Think about (and write answers)

1. **Vtable by hand vs virtual functions.** Re-implement the erasure with an abstract base `Concept` and a template `Model<F>` (the classic way, Chapter 21). Compare `sizeof`, the number of indirections in a call, and the code size for 100 different lambdas. Which has the better binary-size story, and why?
2. `operator()` here is `const` yet calls a possibly-mutable callable. What does `std::function` do, and what does `std::move_only_function<R(Args...) const>` do differently? Which signature would you want in an API, and why?
3. Why does the vtable contain `relocate` (move + destroy source) and not `move` and `destroy` separately?
4. Without RTTI you cannot implement `target<T>()`. How would you? (A type-id function pointer in the vtable, e.g. the address of a per-type static.)
5. `Function` makes the heap case a *pointer* to a `D`. Why not `std::unique_ptr<D>` as the heap representation, and what do you lose by hand-writing `delete`?
6. How would you add copy support later? What must the vtable gain, and what does that force on the stored types?

## Measurements

`-O2`, GCC 13.3, libstdc++, best of 9 runs on a 2-vCPU VM:

```text
call  100M:                                  edu::Function   122 ms   std::function   153 ms
create+call+destroy, 12-byte capture, 20M:   edu              50 ms   std               49 ms
create+call+destroy, 72-byte capture, 20M:   edu             272 ms   std              307 ms
sizeof(edu::Function<int(int)>) = 32,   sizeof(std::function<int(int)>) = 32
```

How to read this honestly:

- Both versions make one indirect call per invocation. The 20% difference in the call loop is a difference in what libstdc++ does around the call (its `_M_invoker` and manager function), **not** in a fundamental cost. Do not generalise it.
- A 12-byte capture fits inline in both (libstdc++'s `std::function` has a 16-byte local buffer), so creation costs the same.
- A 72-byte capture heap-allocates in both. The reference solution is ~12% faster, which is within what I would call noisy for a VM. The take-away is the shape: **the heap case costs ~5× the inline case**, which is why the SBO threshold matters.

## Stretch goals

- Make the buffer size a template parameter: `Function<R(Args...), Cap>`. How does the API of a function taking a `Function` change?
- Add `target<T>()` and `target_type()` without `typeid`.
- A `FunctionRef<R(Args...)>`: a non-owning two-pointer view of a callable (C++26's `std::function_ref`). When is it better than `Function`? What new lifetime hazard does it introduce? (Chapter 12.)
- Make `Function` allocator-aware, using a `std::pmr::polymorphic_allocator` for the heap case.
- Implement `unique_function` variants with a `const` and an `&&`-qualified call operator.

## Reference solution notes

- **`inline_vt<F>` and `heap_vt<F>`** are `static constexpr` variable templates. Each used `F` gets exactly one table in read-only data; the object stores one pointer. Compare to a virtual function table; this is the same structure, built explicitly.
- **Heap storage reuses the inline buffer** to hold the owning pointer. `sizeof(Function)` stays 32 bytes (24-byte buffer + vtable pointer), identical to libstdc++'s `std::function`.
- **`relocate` for heap storage copies a pointer**; for inline storage it move-constructs and destroys the source. That's why the `Function` move constructor can be `noexcept` for any callable: types with throwing moves never go inline.
- **Known gap:** `operator()` is `const` and casts away constness of the buffer, matching the behaviour (and the problem) of `std::function`. A cleaner design has separate const and non-const signatures.

Run it:

```bash
cd code/p04
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan && ./test_asan
g++ -std=c++23 -O2 bench.cpp -o bench && ./bench
```

```text
sizeof(edu::Function<int(int)>) = 32,  sizeof(std::function<int(int)>) = 32
all tests passed
```

## What I learned (write it)

> Where does type erasure *spend* its cost: the indirect call, the allocation, the loss of inlining? Which one did your benchmark actually show?
