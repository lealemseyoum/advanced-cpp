# Project 3 — `SmallVector<T, N>`

[← Project 2](project-02-custom-vector.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 4 — Type-erased function →](project-04-type-erased-function.md)

> **Level 3 · ≈ 6 hours · Builds on:** Chapters 2 (object model, alignment), 3 (lifetime, placement new), 27 (cache and data layout) and Project 2
> **Reference solution:** [`code/p03/small_vector.hpp`](code/p03/small_vector.hpp), [`test.cpp`](code/p03/test.cpp), [`bench.cpp`](code/p03/bench.cpp)

## Goal

A vector that keeps its first `N` elements **inside the object** and only allocates when it outgrows them. The data structure is simple; the project is about the rules underneath it: raw aligned storage, objects that do not exist yet, and a move operation whose cost and pointer-stability depend on which state the source is in.

## Specification

```cpp
template <class T, std::size_t N> class SmallVector;
```

Members: default / copy / move construction, assignment, destructor; `size`, `capacity`, `is_inline`, `data`, `begin`/`end`, `operator[]`, `reserve`, `push_back`, `emplace_back`, `pop_back`, `clear`.

Hard requirements:

1. **No dynamic allocation while `size() <= N`.** Prove it with a counting `operator new`.
2. **Inline storage is raw, aligned bytes** (`alignas(T) unsigned char buf[N * sizeof(T)]`), **not** `T buf[N]`: the latter default-constructs `N` objects you never asked for, and fails for types without a default constructor.
3. **Elements are created with placement new and destroyed explicitly**; the number of constructions equals the number of destructions on every path.
4. **Over-aligned types work** (`alignas(64)`) in both the inline and heap buffers (use `operator new(size, align_val_t)`).
5. **Move construction:**
   - source on the heap: steal the pointer, `O(1)`, `noexcept`, the elements keep their addresses.
   - source inline: the elements must be moved one by one; they get **new addresses**.
6. **Growth** uses `std::move_if_noexcept` and constructs the new element before relocating the old (self-aliasing `push_back(v[0])`).
7. **The object must never point into another object's inline buffer** after a copy or move. (The classic bug: defaulted copy of the `data_` pointer.)

## Milestones

1. Layout and inline `emplace_back`. Test that the 4th push into `SmallVector<int, 4>` does not call `operator new`.
2. Spill: `emplace_back` at capacity allocates `2 × cap`, relocates, frees nothing (the old buffer was inline).
3. Destructor and `clear` for both states; `Probe::live == 0` afterwards.
4. Copy and move for all four source/destination combinations (inline/heap × inline/heap).
5. Over-alignment test with `alignas(64)`.
6. Benchmark.

## Tests that find the bugs

| Bug | How it shows up |
|---|---|
| `T buf[N]` instead of raw bytes | Does not compile for non-default-constructible `T`; `Probe::live == N` after construction |
| Defaulted move/copy | `data_` points into the source's buffer; ASan stack-use-after-scope or garbage values |
| Heap buffer leaked on move assignment | LeakSanitizer |
| Inline buffer freed with `operator delete` | ASan "attempting free on address which was not malloc()-ed" |
| Alignment ignored | UBSan `misaligned address` with `alignas(64)` |
| `reserve` shrinking | Capacity assertion in your own test |
| `std::launder` on never-constructed storage | Passes in practice; wrong in theory: see the note below |

## Think about (and write answers)

1. `sizeof(SmallVector<int, 4>)` is 40 bytes here, `sizeof(std::vector<int>)` is 24. Account for every byte. How would you shrink it? (LLVM's `SmallVector` stores size and capacity as 32-bit values and shares a type-erased base, `SmallVectorImpl<T>`, so functions taking `SmallVectorImpl<T>&` do not depend on `N`. Why is that valuable?)
2. After `SmallVector b = std::move(a);`, are references into `a` still valid? Under what conditions? What does that do to an API contract?
3. Why is `std::launder` unnecessary (or wrong) when computing the pointer to inline storage that holds no object yet? When *would* it be required? (Chapter 3.)
4. Pick `N` for a vector that holds file-system path components. How do you choose? (Measure the distribution first.)
5. Is `SmallVector<T, 1024>` on the stack a good idea? What are the failure modes?
6. The standard does not provide this container. Why? (Hint: iterator and pointer stability on move, and `swap` complexity.)

## Measurements

2 million short lists of 6 `int`, `-O2`, GCC 13.3, best of 9 runs:

```text
std::vector               149 ms
SmallVector<int, 8>        19 ms   (always inline)
SmallVector<int, 4>        54 ms   (spills once to the heap)
```

- The inline version is **7.5× faster**. The work is almost entirely the allocator; `malloc`/`free` is fast, not free.
- The spilling version is still **2.8× faster than `std::vector`**, because `std::vector` starts at capacity 1 and allocates at 1, 2, 4, 8 (four allocations) while the reference `SmallVector` starts at `N` and allocates once. Part of the "small buffer" win is simply a better growth schedule; a `std::vector` with `reserve(6)` would narrow the gap. **Measure that variant** before you claim the inline buffer is the cause.
- What you pay: bigger objects (40 vs 24 bytes), a branch on every `is_inline()`-dependent path, moves that are `O(N)` when inline, and lost pointer stability on move.

## Stretch goals

- `insert` / `erase` with correct overlap handling and exception safety.
- A type-erased base `SmallVectorImpl<T>` so APIs do not template on `N`.
- A constexpr-friendly variant (C++20 allows `std::construct_at` in constant expressions, but not `reinterpret_cast`: what does that force?).
- `SmallString<N>`: a string with the same trick; compare against `std::string`'s own SSO (usually 15 bytes with libstdc++).
- Compare against `boost::container::small_vector` (or LLVM's) on the same benchmark.

## Reference solution notes

- **Storage and pointers:** the inline buffer is `alignas(T) unsigned char buf_[N * sizeof(T)]` and `data_` points at it or at a heap block. The reference solution computes the inline pointer with `reinterpret_cast<T*>`, **without `std::launder`**: no `T` object exists at that address until `emplace_back` constructs one, so there is nothing to launder to. The elements are only created with placement new at those addresses.
- **Move assignment** takes its argument by value, then moves from it. That makes self-assignment harmless and gives one place that handles the four cases.
- **Growth** doubles from `N`, not from 1.
- **A deliberate gap:** `reserve(n)` is correct, but there is no `shrink_to_fit` back to inline storage. Decide what it should do: moving elements back inline invalidates pointers, which is exactly what a `shrink_to_fit` caller does not expect.

Run it:

```bash
cd code/p03
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan && ./test_asan
g++ -std=c++23 -O2 bench.cpp -o bench && ./bench
```

```text
sizeof(SmallVector<int,4>)  = 40
sizeof(SmallVector<int,16>) = 88
sizeof(std::vector<int>)    = 24
all tests passed
```

## What I learned (write it)

> What did you have to know about *where objects live* that `std::vector` let you ignore?
