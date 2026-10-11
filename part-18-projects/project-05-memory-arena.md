# Project 5 — Memory Arena, Pool and PMR Resource

[← Project 4](project-04-type-erased-function.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 6 — Thread pool →](project-06-thread-pool.md)

> **Level 4 · ≈ 8 hours · Builds on:** Chapters 24 (dynamic memory, alignment), 26 (allocators, `pmr`), 27 (cache behaviour)
> **Reference solution:** [`code/p05/arena.hpp`](code/p05/arena.hpp), [`test.cpp`](code/p05/test.cpp), [`bench.cpp`](code/p05/bench.cpp)

## Goal

Build three allocation strategies and plug them into standard containers through `std::pmr::memory_resource`:

| Strategy | Allocate | Free | Typical use |
|---|---|---|---|
| **Arena** (bump) | pointer increment | individual frees are no-ops; free everything at once | per-request or per-frame data |
| **Pool** (fixed size) | pop a free list | push onto a free list | many objects of one type |
| **PoolResource** (size classes) | pop from the right free list | push onto it | node containers (`list`, `map`) via `pmr` |

Then **measure**, and learn that a faster allocator is not automatically a faster program.

## Specification

1. **`Arena`** is a `std::pmr::memory_resource`.
   - Obtains chunks from an upstream resource (default: the global default resource).
   - Honours **every alignment** a caller requests (test 1 B to 64 B alignments).
   - A request larger than a chunk gets a dedicated chunk.
   - `release()` frees every chunk; `reset()` keeps the first chunk for reuse and frees the rest.
   - `do_deallocate` is a no-op. **Destructors are not run** by the arena; containers run theirs.
   - Not thread-safe; say so in a comment.
2. **`Pool<T>`** hands out uninitialised blocks of `sizeof(T)` with `alignof(T)`, using an **intrusive free list** (a free block stores the next pointer *inside itself*, via a union). `create(args...)`/`destroy(p)` construct and destroy. `live()` counts outstanding blocks.
3. **`PoolResource`** is a `memory_resource` with size classes 16, 32, 64, 128, 256 bytes backed by an `Arena`; larger or over-aligned requests go to upstream. Freed blocks go onto the free list of their class.
4. **Tests** check: alignment, chunk accounting against a *counting upstream* resource, `reset` reuse, LIFO reuse of freed pool blocks, `pmr` containers (`vector<string>`, `map<int, vector<int>>`, `list<int>`) working on the arena, and upstream calls balanced at destruction.
5. **Benchmark** against `new`/`delete`, `std::pmr::unsynchronized_pool_resource`, and the default `std::allocator`.

## Milestones

1. `Arena::try_bump` with alignment arithmetic. Unit-test it in isolation with a stack buffer.
2. Chunk management and `release`. Wrap upstream in a counting spy to prove no chunk leaks.
3. `reset`.
4. `Pool<T>` and the union-based free list.
5. `PoolResource` on top of the arena.
6. Benchmark. Then predict the results *before* you read mine.

## A bug the sanitizer found in the reference solution

The first version of `Pool<T>` allocated chunks with `::operator new(size, std::align_val_t{A})` but released them with the plain `::operator delete(ptr)`. It "worked", and ASan stopped it:

```text
AddressSanitizer: new-delete-type-mismatch
  alignment of the allocated type:   8 bytes;
  alignment of the deallocated type: default-aligned.
```

Over-aligned and default-aligned allocations use *different* allocation function overloads and may come from different allocators. The deallocation must name the same alignment. This is the kind of mistake that runs silently on one platform for years. Keep the alignment in one `constexpr` (`kAlign`) and use it on both sides.

## Think about (and write answers)

1. **Bump allocation arithmetic.** Why is `(p + align - 1) & ~(align - 1)` correct only for power-of-two `align`? What does the standard guarantee about `alignment` arguments of `memory_resource::allocate`?
2. Why does an arena make `deallocate` free but make **destructors your problem**? Which objects must never be placed in an arena without running their destructors? (Hint: anything that owns a resource other than arena memory: a `std::string` with the default allocator, a file handle.)
3. **Use-after-reset.** `reset()` makes every pointer into the arena dangling, but the memory is still mapped, so reads "work". Write a test that shows ASan can catch it if you poison the region on `reset` (`ASAN_POISON_MEMORY_REGION`). Why does your arena not do that by default?
4. How would you make `Pool<T>` thread-safe? Compare a mutex, per-thread pools, and a lock-free stack. What does the ABA problem do to the lock-free version? (Chapter 32.)
5. The free list is LIFO. What does that do to **cache locality** after a long run of random frees? Would a different order help?
6. `PoolResource` rounds a 17-byte request up to 32. Compute the worst-case internal fragmentation of the size-class scheme. How do production allocators (jemalloc, tcmalloc) pick their classes?
7. When is it correct for a `memory_resource::do_is_equal` to return `true` for two different arenas? (It is the equality that decides whether memory from one can be freed through the other.)

## Measurements

`-O2`, GCC 13.3 / libstdc++, best of 7 runs, 2-vCPU VM. **Read the caveats under the table.**

```text
1M-node list x5              new/delete  113 ms   Pool   35-47 ms   Arena(+reset)  72-77 ms
pmr::list<int> push/clear x5  new_delete  133 ms   std unsync_pool 281-287 ms   edu::PoolResource 54-58 ms
20k "requests" of 50 strings  std::allocator 32.6 ms   Arena + reset  31.1-32.1 ms
```

What I would claim from this, and what I would not:

- **Pool is 2.4-3.2× faster than `new`/`delete`** for a single node type. That is the expected result: a free-list pop and push are a handful of instructions, with no size classes and no thread-cache logic.
- **`Arena` was slower than `Pool` here**, which surprised me. Allocation is a pointer bump, but `reset()` returns all chunks but one, so each round re-requests ~24 MB of 1 MB chunks from the system and **touches fresh pages**. I did not profile this; the likely contributors are page faults and the cost of the first touch. The lesson I would take is the general one: *allocation speed is not the whole cost; where the memory comes from matters.* Try keeping all chunks on `reset`, and measure again.
- **`std::pmr::unsynchronized_pool_resource` was 2× slower than the default** in this test, and `edu::PoolResource` was 2.4× faster than the default. I did not investigate why the standard resource is slow here (its default pool options and chunk growth are the first things I would check). Do not conclude from one benchmark that the standard resource is "bad".
- **The request-style benchmark showed no gain at all** (32.6 vs 31.1 ms). glibc's `malloc` has a per-thread cache that makes small, short-lived allocations in a single thread very cheap, and 50 strings are mostly SSO-defeating but small. **An arena pays off when the allocator is slow, shared across threads, or fragmenting, or when you need bulk freeing: not automatically.** If you cannot measure a win, do not ship the complexity.

## Stretch goals

- A **thread-safe** `Pool<T>` with a per-thread cache and a global lock for refilling. Benchmark with 1, 2, 4 threads.
- **Poisoning:** call `ASAN_POISON_MEMORY_REGION` in `Arena::reset` and `Pool::deallocate` (guarded by `__has_feature(address_sanitizer)` / `__SANITIZE_ADDRESS__`) so use-after-free of arena memory is caught.
- A **growth policy** for chunk size (double each new chunk, up to a cap) and a high-water-mark statistic.
- `Arena::checkpoint()` / `rewind(checkpoint)`: free a *suffix* of allocations (a scoped sub-arena).
- Replace the global `operator new` with a pool-backed one for small sizes and run a real program under it.

## Reference solution notes

- **Arena chunks have a header** (`next`, `total`) at the start of the chunk memory itself; the bump region starts right after it.
- **`Pool::Node` is a union** of `Node* next` and raw `storage[sizeof(T)]` aligned for `T`. A free block *is* the free-list node, so the pool has no side table. The cost is that `sizeof(T)` must be at least a pointer: the union takes care of that.
- **`PoolResource` composes** the arena for refills and per-class free lists for reuse, which is the textbook shape of a size-class allocator in 40 lines.
- Chunks are never returned to upstream until destruction (pools never shrink).

Run it:

```bash
cd code/p05
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan && ./test_asan
g++ -std=c++23 -O2 bench.cpp -o bench && ./bench
```

```text
all tests passed
```

## What I learned (write it)

> Which of your predictions about the benchmark was wrong, and what did it teach you about where allocator time really goes?
