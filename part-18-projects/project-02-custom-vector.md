# Project 2 — Custom `Vector<T>`

[← Project 1](project-01-raii-library.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 3 — SmallVector →](project-03-small-vector.md)

> **Level 3 · ≈ 8 hours · Builds on:** Chapters 2 (object model), 3 (lifetime), 6 (moves), 11 (containers), 22 (exception safety), 24 (allocation), 26 (allocators)
> **Reference solution:** [`code/p02/vector.hpp`](code/p02/vector.hpp), [`test.cpp`](code/p02/test.cpp), [`bench.cpp`](code/p02/bench.cpp)

## Goal

Implement `Vector<T, Alloc>` with separate **storage** and **object lifetime**, the growth policy, move-aware reallocation, allocator support and honest exception-safety guarantees. `std::vector` is the best small lesson in the C++ object model there is; this project makes you earn it.

## Specification

```cpp
namespace edu {
template <class T, class Alloc = std::allocator<T>> class Vector;
}
```

Required members: default / allocator / `initializer_list` / count constructors; copy and move construction and assignment; destructor; `size`, `capacity`, `empty`, `data`, `begin`/`end` (pointers are fine), `operator[]`, `at`, `reserve`, `push_back` (both overloads), `emplace_back`, `pop_back`, `clear`, `swap`, `get_allocator`.

Hard requirements:

1. **Storage is allocated through `std::allocator_traits<Alloc>`; objects are constructed and destroyed with `construct` / `destroy`.** `new T[n]` is banned (it default-constructs elements you may not want).
2. **Capacity grows geometrically** (factor 2 here; say why not 1.5 or 1.1).
3. **Reallocation uses `std::move_if_noexcept`.** If `T`'s move constructor can throw and `T` is copyable, copy instead, to keep the strong guarantee.
4. **`emplace_back` / `push_back` of an element of the vector itself must work**: `v.push_back(v[0])` when `v` is full.
5. **Exception guarantees, stated and tested:**
   - `emplace_back` that reallocates: **strong** (vector unchanged if anything throws).
   - copy construction / copy assignment: **strong** (build into a temporary, then swap).
   - move construction, `swap`, destructor, `pop_back`, `clear`: **`noexcept`**.
6. **Moved-from vectors are empty and reusable.**
7. Allocator propagation traits (`propagate_on_container_move_assignment`, `is_always_equal`, `select_on_container_copy_construction`) are honoured for move assignment and copy construction.

You may skip `insert`/`erase` in the core spec; they are the first stretch goal.

## Milestones

1. Storage only: allocate, deallocate, `reserve`, destructor. Prove with `ASan` that nothing leaks.
2. `emplace_back` without reallocation, then with it. Count `Probe::live` (constructions minus destructions) to prove every object is destroyed exactly once.
3. `move_if_noexcept`: add a type with a **throwing** move constructor and check your vector copies it on growth (and moves a `noexcept`-movable one).
4. Strong guarantee: a type whose copy throws on the Nth call; verify size, capacity and contents are unchanged after the exception.
5. The aliasing case (`v.push_back(v[0])`).
6. Allocators: run the same code with `std::pmr::polymorphic_allocator` and a monotonic buffer; check that `data()` lies inside the buffer.

## The tests that find your bugs

Write an instrumented `Probe` type (live count, copy/move counters, "throw on the Nth copy") **before** writing the vector. It will catch, in order of how often it happens:

| Bug | Probe symptom |
|---|---|
| Elements not destroyed on reallocation | `live` too high after the vector is destroyed |
| Elements destroyed twice | `live` negative, ASan double-destroy |
| Move on growth for a throwing-move type | `copies == 0` where it should be positive |
| New element constructed *after* the old buffer is freed | ASan heap-use-after-free on `push_back(v[0])` |
| Leaked buffer when a constructor throws during growth | ASan leak report |
| Copy constructor leaks the half-built copy | `live` too high after caught exception |

## Think about (and write answers)

1. Why is `reserve(n)` allowed to be `O(size)` but `push_back` amortised `O(1)`? Prove the amortised bound for growth factor 2. What is the memory cost of the worst case?
2. GCC's `std::vector` uses factor 2; MSVC uses 1.5. What does 1.5 buy? (Hint: reuse of freed blocks.)
3. Why does `std::move_if_noexcept` look at *both* move-noexcept and copyability? What happens for a move-only type with a throwing move constructor?
4. `Vector(size_type n)` here builds into a temporary and swaps. What would go wrong if it constructed directly into `*this` and a constructor threw?
5. Why does `Vector(const Vector&)` need `select_on_container_copy_construction`? Which standard allocator overrides it?
6. Where does `[[no_unique_address]]` matter, and what is `sizeof(Vector<int>)` on your machine? (3 pointers' worth: 24 bytes here with an empty allocator.)

## Measurements

Reference solution against `std::vector`, `-O2`, GCC 13.3, best of 9 runs on a 2-vCPU VM:

```text
push_back 4M int      std::vector   18.99 ms   edu::Vector   20.03 ms
push_back 500k string std::vector   88.00 ms   edu::Vector   82.62 ms
```

The two are within noise of each other, which is the expected result: both are the same algorithm over the same allocator. Do not read the string row as "ours is faster". Run the benchmark ten times and look at the spread. If your numbers are much worse, check that you are not copying on growth for types that are `noexcept`-movable.

## Stretch goals

- `insert(pos, value)` and `erase(pos)`; this is where the strong guarantee stops being free. Decide what you promise and test it.
- `shrink_to_fit`, `resize`, `assign`, `emplace` at an arbitrary position (aliasing is nastier here).
- **Trivially-relocatable fast path:** for types where `memcpy` is a valid move, use `memmove` on growth. Measure; then explain why the standard does not (yet) let you assume it. (See Chapter 6 and the relocation proposals.)
- Proper iterator class (random-access, `std::contiguous_iterator` concept check) and `const` correctness.
- Make `Vector<bool>` *not* exist (and explain why `std::vector<bool>` does).

## Reference solution notes

- `emplace_back` constructs the new element in the **new** buffer first, then transfers the old elements. That ordering is what makes `push_back(v[0])` safe, and it is also what makes the strong guarantee cheap.
- `transfer_into` uses one `try`/`catch` that destroys exactly what was built. Compare with a RAII guard type (`struct Buffer { T* p; size_t n; ~Buffer() }`) and decide which you prefer. The guard version is shorter once you have three places that need it.
- The count constructor and copy constructor build into a local `Vector` and `swap`; the local's destructor is the cleanup on exception. This is the "copy and swap" idea applied to construction.
- The one thing the reference solution deliberately does not do: allocator-propagation for **copy assignment** (it uses copy-and-swap and keeps the destination's allocator). The standard container requirements do more; implement them if you want the full model.

Run it:

```bash
cd code/p02
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan && ./test_asan
g++ -std=c++23 -O2 bench.cpp -o bench && ./bench
```

```text
all tests passed
```

## What I learned (write it)

> Which of the six bugs in the table did you write? How long did the Probe type take to find it?
