# Chapter 26 — Allocators and Memory Resources

> **Part X · Memory** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 7 hours**
> **Prerequisites:** [Chapter 21](../part-08-polymorphism/21-type-erasure.md), [Chapter 24](24-dynamic-memory.md), [Chapter 25](25-smart-pointers.md) &nbsp;|&nbsp; **Standards:** C++98 (`std::allocator`), C++11 (`allocator_traits`, stateful allocators, scoped allocators), **C++17 (`std::pmr`)**, C++20 (`allocator::construct` removed, `std::uses_allocator_construction_args`), C++23 (`allocate_at_least`) &nbsp;|&nbsp; **Tools:** `g++-14`, `valgrind`/`massif`, `perf`

[← Previous: Chapter 25](25-smart-pointers.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 27 — Cache and data-oriented C++ →](27-cache-and-data-oriented-cpp.md)

---

**In one sentence:** an allocator decouples *what a container stores* from *where its memory comes from*; the classic model bakes the answer into the type (fast, rigid), the PMR model bakes it into a runtime object (flexible, one indirect call), and arena/pool resources exploit the fact that **you know the lifetime pattern and `malloc` does not**.

**By the end of this chapter you can:**

- write a minimal C++11 allocator and explain `allocator_traits`, `rebind`, propagation and equality
- explain why the allocator is in the *type* of `std::vector<T, A>` and what that costs in API design
- use `std::pmr` containers with `monotonic_buffer_resource`, pools and your own `memory_resource`
- build an **Arena**, a **Pool** and a **PMR-based container**, and measure when they win and by how much
- decide when a custom allocator is the right tool and when it is premature optimisation

---

## 1. Problem

`operator new` (Chapter 24) is a general-purpose allocator. It must serve any size, any lifetime, any thread, and give each block an individual `free`. That generality has costs you pay even when you don't need it:

```text
   per-allocation:    a function call, a size-class lookup, a lock or a thread-cache probe, a header (≥ 16 bytes)
   per-free:          the mirror image, plus coalescing/fragmentation bookkeeping
   locality:          nodes of a std::list or std::map land wherever the heap has room
   lifetime:          you often free everything at once (a request, a frame, a parse tree) but pay to free it piece by piece
```

You frequently know things the allocator can't: "all of these die together", "all these blocks are 48 bytes", "this runs on one thread", "I have 64 KiB of stack to spare". The standard library therefore lets you **replace the memory source of any container**, without rewriting the container.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1994 | The STL (Stepanov) introduces allocators as a portability device for memory models (near/far pointers in 16-bit x86). Most of that rationale is long dead; the abstraction survived |
| C++98 | `std::allocator<T>` with `allocate/deallocate/construct/destroy`, `rebind`. **Stateless only** in practice: the standard allowed implementations to assume all allocators of a type are interchangeable |
| 2000s | Everyone writes their own `vector`/`list` (EASTL, Qt containers, Bloomberg BSL) because allocator support was weak and the type-in-container-type problem was painful |
| C++11 | **Stateful allocators** legitimised; `allocator_traits` supplies defaults (a minimal allocator is ~10 lines); propagation traits (`propagate_on_container_move_assignment`…); `scoped_allocator_adaptor` for nested containers |
| 2012–15 | Bloomberg (John Lakos, Pablo Halpern) proposes **polymorphic memory resources** based on years of production use in BSL |
| **C++17** | `<memory_resource>`: `std::pmr::memory_resource`, `polymorphic_allocator<T>`, `monotonic_buffer_resource`, `unsynchronized_pool_resource`, `synchronized_pool_resource`, and `pmr::vector` etc. aliases |
| C++20 | `allocator::construct/destroy` removed (use `allocator_traits`); `std::uses_allocator_construction_args` generalises "uses-allocator construction" |
| C++23 | `std::allocator_traits::allocate_at_least` and `std::allocator::allocate_at_least` (P0401): allocators can report extra capacity actually obtained. **Not available in GCC 14.2** (Experiment 7 shows the compile error); check your library |
| 2024–26 | Active work on `std::inplace_vector`, hardening and allocator-aware additions to the new containers (`flat_map`, `inplace_vector` are allocator-aware or allocation-free); `std::pmr::*` unchanged |

---

## 3. Modern solution

Two mechanisms, with different trade-offs:

```cpp
// (1) Classic: allocator is a template parameter of the container (compile-time, in the type)
std::vector<int, MyAllocator<int>> v;

// (2) PMR: allocator is a thin handle to a runtime memory_resource; the *type* stays std::pmr::vector<int>
std::array<std::byte, 4096> buffer;
std::pmr::monotonic_buffer_resource arena{buffer.data(), buffer.size()};
std::pmr::vector<int> v{&arena};
```

| | Classic allocator | PMR |
|---|---|---|
| Allocator in the container's type? | **Yes** (`vector<int, A>`) | No (`pmr::vector<int>` for all resources) |
| Dispatch | static; zero overhead; inlinable | one virtual call per allocate/deallocate |
| Interoperability | `vector<int, A1>` ≠ `vector<int, A2>`; can't be assigned/passed interchangeably | one type; swap the resource at runtime |
| State | the allocator object (stored in the container) | a pointer (8 bytes) to a resource |
| Propagation into nested containers | `scoped_allocator_adaptor` (verbose) | **automatic** (`uses_allocator` construction) |
| Typical use | libraries with a single fixed policy; very hot paths | application-level arenas, per-request/per-frame memory |
| Writing one | ~15 lines (+ `rebind` via traits) | override 3 virtual functions |

> **Rule of thumb.** Use `std::pmr` unless you have measured that one virtual call per allocation matters. Then you're probably replacing the container, not tuning the allocator.

---

## 4. Mental model

### Allocator = "where the bytes for this container come from"; a container never calls `new` directly

```text
   std::vector<T, A>                          std::pmr::vector<T>
   ┌───────────────────────┐                  ┌───────────────────────┐
   │ begin, end, cap       │                  │ begin, end, cap       │
   │ A  (the allocator)    │                  │ polymorphic_allocator │──► memory_resource* ──► do_allocate(bytes, align)
   └───────────────────────┘                  └───────────────────────┘                          do_deallocate(p, bytes, align)
        │                                                                                         do_is_equal(other)
        └─ allocator_traits<A>::allocate(a, n) → a.allocate(n)  (+ defaults for everything you omit)
```

### The bump (monotonic) allocator, the simplest useful allocator

```text
   buffer:  [########## used ##########|.......... free ..........]
                                       ^
                                       |  allocate(n, align): round `cur` up to `align`, return cur, cur += n
                                       |  deallocate: does NOTHING
                                       |  release/destroy: reset cur to the start (everything dies at once)
```

An allocation is two instructions and a compare. Nothing is ever freed individually. This is the right tool whenever objects share a lifetime: a request, a compile, a frame, a parse tree.

### The pool allocator: one size, an intrusive free-list

```text
   chunk: [slot][slot][slot][slot][slot]...      free list threaded through the *unused slots themselves*
   head → slot 2 → slot 0 → slot 4 → nullptr     allocate: pop head.   deallocate: push the slot back.   O(1), no header.
```

Perfect for the nodes of `list`/`map`/`unordered_map`, which are all one size per container.

### Locality is half the win

Arena- and pool-allocated nodes are **contiguous**; a `std::list` built from a monotonic buffer is a linked list laid out in memory order, so traversal prefetches well. The speed-up that people attribute to "faster malloc" is often the cache effect (Chapter 27).

---

## 5. Language rules

### 5.1 The allocator requirements  `[allocator.requirements]`

A minimal C++11 allocator (the rest comes from `allocator_traits`):

```cpp
template <class T> struct A {
    using value_type = T;
    A() = default;
    template <class U> A(const A<U>&) noexcept {}          // converting constructor: rebinding (node containers allocate nodes, not T)
    T*   allocate(std::size_t n);                          // storage for n objects, NOT constructed
    void deallocate(T* p, std::size_t n) noexcept;         // n must equal the value passed to allocate
};
template <class T, class U> bool operator==(const A<T>&, const A<U>&) noexcept;   // "can one free what the other allocated?"
```

- The container never constructs elements with `new`; it calls `std::allocator_traits<A>::construct(a, p, args…)` (which, by default, does `std::construct_at`/placement `new`) and `destroy`.
- **Rebinding.** `std::list<int, A<int>>` must allocate *nodes*, not `int`s: it converts `A<int>` to `A<Node>` via the converting constructor. Hence the template converting constructor above.
- **Equality** means interchangeability: `a == b` iff memory from `a` may be deallocated by `b`. Stateless allocators are always equal; allocators bound to different arenas are not.
- **Propagation traits** (defaults all `false`, except `is_always_equal` which follows `empty`): `propagate_on_container_copy_assignment`, `propagate_on_container_move_assignment`, `propagate_on_container_swap`. They answer "when I assign a container, does the *allocator* come with the elements?" — and, if not, whether the move can steal the buffer (only if allocators are equal; otherwise the elements are moved one by one).
- `select_on_container_copy_construction()`: which allocator does a *copy* get? (Default: a copy of the source's allocator.)
- The allocator type is part of the container type. `std::vector<int, A1>` and `std::vector<int, A2>` are unrelated types.

### 5.2 `std::pmr`  `[mem.res]`

```cpp
class std::pmr::memory_resource {                      // abstract base, 3 virtuals
public:
    void* allocate(std::size_t bytes, std::size_t align = alignof(max_align_t));   // → do_allocate
    void  deallocate(void* p, std::size_t bytes, std::size_t align = ...);          // → do_deallocate
    bool  is_equal(const memory_resource&) const noexcept;                          // → do_is_equal
private:
    virtual void* do_allocate(std::size_t, std::size_t) = 0;
    virtual void  do_deallocate(void*, std::size_t, std::size_t) = 0;
    virtual bool  do_is_equal(const memory_resource&) const noexcept = 0;
};
```

Standard resources:

| Resource | Behaviour | Thread-safe? | Use for |
|---|---|---|---|
| `new_delete_resource()` | `operator new/delete` (the **default**) | yes (as `new`) | baseline |
| `null_memory_resource()` | every allocation throws `bad_alloc` | yes | an **upstream** that proves "no heap use", used in tests |
| `monotonic_buffer_resource` | bump allocator over a user buffer, then upstream chunks (geometrically growing); `deallocate` is a no-op; `release()` frees chunks | **no** | per-request / per-frame, parse trees, short-lived containers |
| `unsynchronized_pool_resource` | size-class pools with chunks from upstream; frees to pools | **no** | many same-sized nodes, single-threaded |
| `synchronized_pool_resource` | same, with a mutex | yes | shared pools; typically slower than per-thread unsynchronized pools |

`polymorphic_allocator<T>` is an 8-byte wrapper around `memory_resource*`. **Its copy does not copy the resource**: both refer to the same one. It does **not** propagate on copy/move/swap assignment (the container keeps its own resource), so moving between containers with different resources copies elements.

### 5.3 Uses-allocator construction

When a `pmr::vector<pmr::string>` constructs an element, it passes its own resource to the element's constructor **if the element type is allocator-aware** (`std::uses_allocator_v<T, polymorphic_allocator<…>>` is true — e.g. `pmr::string`, `pmr::vector`, `pair`/`tuple` of such). That is how one arena is automatically shared by the whole nested structure, with no `scoped_allocator_adaptor` needed. A user type opts in with `using allocator_type = std::pmr::polymorphic_allocator<>;` and a constructor taking `(args…, allocator_type)` — see Experiment 4.

### 5.4 Lifetime rules for resources

The container holds only a **pointer** to the resource. The resource must **outlive every container (and every allocation) that uses it**. Destroying a `monotonic_buffer_resource` frees everything at once *without calling destructors* of objects living in it; if those objects own other resources (file handles, non-PMR members) you leak unless you destroy them first.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is guaranteed? | The allocator requirements, `allocator_traits` defaults, `pmr` interfaces and resource semantics; that `monotonic_buffer_resource::deallocate` has no effect; that `polymorphic_allocator` does not propagate |
| **Compiler / library** | How are `monotonic_buffer_resource`/pool resources implemented? | libstdc++'s `monotonic_buffer_resource` bumps a pointer with alignment in an inline function; each chunk's size grows geometrically from an initial size (default 128-ish bytes then ×1.5/2); pool resource keeps pools per power-of-two-ish size with `options{max_blocks_per_chunk, largest_required_pool_block}` |
| **ABI** | What does a polymorphic allocator cost in layout? | `pmr::vector<int>` is 32 bytes, vs 24 for `std::vector<int>`: the resource pointer; `pmr::string` is 40 vs 32 (Experiment 3) |
| **OS** | Where do arena chunks come from? | `upstream` (default `new_delete_resource` → `malloc` → `brk`/`mmap`); for large arenas, page-aligned `mmap` with `MAP_POPULATE`/huge pages is a separate decision |
| **CPU** | Why are arenas fast? | Fewer instructions per allocation **and** contiguous layout; sequential nodes mean hardware prefetch works for pointer-chasing structures |

---

## 6. Implementation model

### What `std::vector<int, A>::push_back` does when full

```text
   cap exceeded
     └─► new_cap = 2 * cap          (libstdc++; MSVC uses 1.5×; not specified by the standard beyond amortised O(1))
         └─► traits::allocate(a, new_cap)           ← YOUR allocator is called here
             └─► move/copy-construct elements into it (via traits::construct)
                 └─► destroy old elements, traits::deallocate(a, old, old_cap)
```

With a **monotonic** resource, growth's old buffers are never reused, so a vector growing from 1 to N elements consumes about *2N + …* elements' worth of buffer, not N. `reserve()` first is the standard remedy (Experiment 5).

### `monotonic_buffer_resource::do_allocate`

```text
   if (cur + padding + n <= end)  { p = align_up(cur, align); cur = p + n; return p; }
   else                            { new_chunk = upstream->allocate(next_size(), align); link it; retry }
```

Allocation is a handful of instructions and a branch. There is **no per-block header**, so memory overhead is only alignment padding.

### PMR dispatch cost

`polymorphic_allocator<T>::allocate(n)` → `resource_->allocate(n*sizeof(T), alignof(T))` → virtual `do_allocate`. One indirect call that the optimiser cannot inline unless it can prove the resource's dynamic type (it can, in simple cases where the resource is a local object and `final`). Mark your own `memory_resource` subclasses `final`.

### Pool resources: the free list is stored *inside* the free blocks

An unused slot's first 8 bytes hold the `next` pointer. A block is therefore at least pointer-sized and pointer-aligned; allocation and deallocation are both a couple of loads and stores. Chunks come from upstream and are only returned on `release()`/destruction.

---

## 7. Experiments

### Experiment 1 ✅: A minimal C++11 allocator, instrumented

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <list>
#include <new>
#include <vector>

template <class T> struct Logging {
    using value_type = T;
    Logging() = default;
    template <class U> Logging(const Logging<U>&) noexcept {}
    T* allocate(std::size_t n) {
        std::printf("  allocate(%zu x %zu bytes = %zu)\n", n, sizeof(T), n * sizeof(T));
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept { std::printf("  deallocate(%zu x %zu)\n", n, sizeof(T)); ::operator delete(p); }
};
template <class T, class U> bool operator==(const Logging<T>&, const Logging<U>&) noexcept { return true; }

int main() {
    std::puts("vector<int>: push_back x 5 (watch the growth policy)");
    { std::vector<int, Logging<int>> v; for (int i = 0; i < 5; ++i) v.push_back(i); }

    std::puts("list<int>: 2 elements (the allocator is REBOUND to the node type, not int)");
    { std::list<int, Logging<int>> l; l.push_back(1); l.push_back(2); }

    std::puts("vector with reserve: one allocation");
    { std::vector<int, Logging<int>> v; v.reserve(5); for (int i = 0; i < 5; ++i) v.push_back(i); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
vector<int>: push_back x 5 (watch the growth policy)
  allocate(1 x 4 bytes = 4)
  allocate(2 x 4 bytes = 8)
  deallocate(1 x 4)
  allocate(4 x 4 bytes = 16)
  deallocate(2 x 4)
  allocate(8 x 4 bytes = 32)
  deallocate(4 x 4)
  deallocate(8 x 4)
list<int>: 2 elements (the allocator is REBOUND to the node type, not int)
  allocate(1 x 24 bytes = 24)
  allocate(1 x 24 bytes = 24)
  deallocate(1 x 24)
  deallocate(1 x 24)
vector with reserve: one allocation
  allocate(5 x 4 bytes = 20)
  deallocate(5 x 4)
```

Read it as the container's contract with its allocator: **only** `allocate`/`deallocate` appear (construction goes through `allocator_traits`, which defaulted for us). The `list` calls show a node-sized request (16-byte header + 4-byte payload, rounded) although the allocator was written for `int` — that is `rebind` via the converting constructor.

### Experiment 2 🔧: Allocator in the type: the interoperability cost

```cpp
// @test fail -std=c++23 err=no match|cannot convert|could not convert|no viable|invalid
#include <memory>
#include <vector>

template <class T> struct ArenaAlloc {
    using value_type = T;
    int arena_id = 0;
    ArenaAlloc() = default;
    template <class U> ArenaAlloc(const ArenaAlloc<U>& o) noexcept : arena_id(o.arena_id) {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>{}.deallocate(p, n); }
};
template <class T, class U> bool operator==(const ArenaAlloc<T>& a, const ArenaAlloc<U>& b) noexcept { return a.arena_id == b.arena_id; }

void takes_default_vector(const std::vector<int>&) {}

int main() {
    std::vector<int, ArenaAlloc<int>> v;
    takes_default_vector(v);          // ERROR: vector<int, ArenaAlloc<int>> is a different type from vector<int>
}
```

```text
# output
```

*This is the central design complaint about classic allocators.* A function that accepts `const std::vector<int>&` cannot accept your arena-backed vector; a library taking `std::vector<int>` forces everyone to use the global heap. APIs end up as templates (`template <class A> void f(const std::vector<int, A>&)`) or `span<const int>` parameters. PMR exists to remove this barrier: `std::pmr::vector<int>` is one type no matter which resource backs it.

### Experiment 3 ✅: PMR in practice: a stack buffer, `null_memory_resource` as a proof, nested propagation

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

int main() {
    std::printf("sizes: pmr::vector<int>=%zu  vector<int>=%zu   pmr::string=%zu  string=%zu   polymorphic_allocator=%zu\n",
                sizeof(std::pmr::vector<int>), sizeof(std::vector<int>), sizeof(std::pmr::string), sizeof(std::string),
                sizeof(std::pmr::polymorphic_allocator<int>));

    // 4 KiB on the stack, with NULL upstream: if the data does not fit, bad_alloc proves we left the buffer
    alignas(std::max_align_t) std::array<std::byte, 4096> buffer;
    std::pmr::monotonic_buffer_resource arena{buffer.data(), buffer.size(), std::pmr::null_memory_resource()};

    std::pmr::vector<std::pmr::string> names{&arena};            // the vector uses the arena...
    names.reserve(4);
    names.emplace_back("an ordinary short string");              // SSO: no allocation at all
    names.emplace_back("a considerably longer string that does not fit in the small-string buffer");
    names.emplace_back(std::string("converted from std::string; still copied into the arena"));

    // ...and so does every element: uses-allocator construction handed it the same resource
    bool propagated = true;
    for (auto& s : names) propagated = propagated && s.get_allocator().resource() == &arena;
    std::printf("every nested pmr::string uses the arena: %s\n", propagated ? "yes" : "NO");

    // How much of the arena was consumed, and no heap involved
    std::puts("heap was never touched: upstream is null_memory_resource (it would have thrown)");

    // What happens when the buffer is too small
    alignas(std::max_align_t) std::array<std::byte, 64> tiny;
    std::pmr::monotonic_buffer_resource small{tiny.data(), tiny.size(), std::pmr::null_memory_resource()};
    try {
        std::pmr::vector<int> big{&small};
        for (int i = 0; i < 1000; ++i) big.push_back(i);
    } catch (const std::bad_alloc&) { std::puts("64-byte arena overflowed -> bad_alloc from null_memory_resource"); }

    // Copy/assign semantics: the container keeps its own resource
    std::pmr::vector<int> a{&arena};
    std::pmr::vector<int> b{std::pmr::new_delete_resource()};
    a.assign({1, 2, 3});
    b = a;                                                       // copy-assign: b KEEPS its heap resource; elements copied in
    std::printf("after b = a: b uses the heap resource still: %s\n", b.get_allocator().resource() == std::pmr::new_delete_resource() ? "yes" : "no");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizes: pmr::vector<int>=32  vector<int>=24   pmr::string=40  string=32   polymorphic_allocator=8
every nested pmr::string uses the arena: yes
heap was never touched: upstream is null_memory_resource (it would have thrown)
64-byte arena overflowed -> bad_alloc from null_memory_resource
after b = a: b uses the heap resource still: yes
```

### Experiment 4 ✅: Your own types: `uses_allocator` opt-in

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

struct Employee {
    using allocator_type = std::pmr::polymorphic_allocator<>;     // the opt-in: "I am allocator-aware"

    std::pmr::string name;
    std::pmr::vector<std::pmr::string> skills;

    Employee(std::string_view n, allocator_type a = {}) : name(n, a), skills(a) {}
    Employee(const Employee& o, allocator_type a = {}) : name(o.name, a), skills(o.skills, a) {}
    Employee(Employee&& o, allocator_type a) : name(std::move(o.name), a), skills(std::move(o.skills), a) {}
    Employee(Employee&&) = default;
};

int main() {
    alignas(std::max_align_t) std::array<std::byte, 8192> buf;
    std::pmr::monotonic_buffer_resource arena{buf.data(), buf.size(), std::pmr::null_memory_resource()};

    std::pmr::vector<Employee> staff{&arena};
    staff.emplace_back("Ada Lovelace, born in London, in 1815");        // the arena is injected into Employee's constructor...
    staff.back().skills.emplace_back("analytical engines and other considerable machinery");
    staff.emplace_back("Grace Hopper, rear admiral and compiler pioneer");

    bool all = true;
    for (auto& e : staff)
        all = all && e.name.get_allocator().resource() == &arena
                  && e.skills.get_allocator().resource() == &arena;
    std::printf("every Employee's name and skills live in the arena (including after vector reallocation): %s\n", all ? "yes" : "NO");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
every Employee's name and skills live in the arena (including after vector reallocation): yes
```

`pmr::vector<Employee>::emplace_back` calls `allocator_traits::construct`, which detects `allocator_type` and passes the container's allocator as the trailing constructor argument. The *allocator-extended* move constructor `Employee(Employee&&, allocator_type)` is what lets the vector move elements to a new buffer while keeping them in the arena: forget it and a reallocation silently moves elements out of the arena (or fails to compile).

### Experiment 5 🔧: Build the memory resources yourself: Arena and Pool

```cpp
// @test run -std=c++23 -O2
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <vector>

// ---- Arena: a bump allocator over upstream chunks. deallocate() is a no-op. ----
class ArenaResource final : public std::pmr::memory_resource {
    struct Chunk { Chunk* next; std::size_t size; };
    std::pmr::memory_resource* upstream_;
    Chunk* head_ = nullptr;
    std::byte* cur_ = nullptr;
    std::byte* end_ = nullptr;
    std::size_t next_chunk_;
public:
    std::size_t bytes_requested = 0, chunks = 0;
    explicit ArenaResource(std::size_t first_chunk = 4096, std::pmr::memory_resource* up = std::pmr::get_default_resource())
        : upstream_(up), next_chunk_(first_chunk) {}
    ~ArenaResource() { release(); }
    void release() {
        for (Chunk* c = head_; c;) { Chunk* n = c->next; upstream_->deallocate(c, c->size, alignof(std::max_align_t)); c = n; }
        head_ = nullptr; cur_ = end_ = nullptr; chunks = 0;
    }
private:
    void* bump(std::size_t n, std::size_t align) noexcept {          // fast path: nullptr if the current chunk is full
        auto p = reinterpret_cast<std::uintptr_t>(cur_);
        auto aligned = (p + align - 1) & ~(std::uintptr_t(align) - 1);
        if (!cur_ || aligned + n > reinterpret_cast<std::uintptr_t>(end_)) return nullptr;
        cur_ = reinterpret_cast<std::byte*>(aligned + n);
        return reinterpret_cast<void*>(aligned);
    }
    void* do_allocate(std::size_t n, std::size_t align) override {
        bytes_requested += n;
        if (void* p = bump(n, align)) return p;
        // slow path: new chunk (large enough for this request), geometric growth
        std::size_t need = n + align + sizeof(Chunk);
        std::size_t sz = next_chunk_ > need ? next_chunk_ : need;
        next_chunk_ = sz * 2;
        auto* c = static_cast<Chunk*>(upstream_->allocate(sz, alignof(std::max_align_t)));
        c->next = head_; c->size = sz; head_ = c; ++chunks;
        cur_ = reinterpret_cast<std::byte*>(c + 1); end_ = reinterpret_cast<std::byte*>(c) + sz;
        return bump(n, align);
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}                 // individual frees are ignored
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }
};

// ---- Pool: fixed-size blocks with an intrusive free list. ----
class PoolResource final : public std::pmr::memory_resource {
    struct Free { Free* next; };
    std::size_t block_;
    std::pmr::memory_resource* upstream_;
    Free* free_ = nullptr;
    std::pmr::vector<void*> chunks_;
public:
    std::size_t live = 0;
    explicit PoolResource(std::size_t block_size, std::pmr::memory_resource* up = std::pmr::get_default_resource())
        : block_(block_size < sizeof(Free) ? sizeof(Free) : (block_size + 15) & ~std::size_t(15)), upstream_(up), chunks_(up) {}
    ~PoolResource() { for (void* c : chunks_) upstream_->deallocate(c, block_ * kPerChunk, 16); }
private:
    static constexpr std::size_t kPerChunk = 256;
    void* do_allocate(std::size_t n, std::size_t align) override {
        if (n > block_ || align > 16) return upstream_->allocate(n, align);           // not our size: pass through
        if (!free_) {                                                                 // refill: carve a chunk into slots
            auto* chunk = static_cast<std::byte*>(upstream_->allocate(block_ * kPerChunk, 16));
            chunks_.push_back(chunk);
            for (std::size_t i = 0; i < kPerChunk; ++i) { auto* f = reinterpret_cast<Free*>(chunk + i * block_); f->next = free_; free_ = f; }
        }
        Free* f = free_; free_ = f->next; ++live; return f;
    }
    void do_deallocate(void* p, std::size_t n, std::size_t align) override {
        if (n > block_ || align > 16) { upstream_->deallocate(p, n, align); return; }
        auto* f = static_cast<Free*>(p); f->next = free_; free_ = f; --live;
    }
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }
};

int main() {
    // Arena: a vector<int> grows 1,2,4,... and the old buffers are never reused
    {
        ArenaResource arena(256, std::pmr::new_delete_resource());
        std::pmr::vector<int> v(&arena);
        for (int i = 0; i < 1000; ++i) v.push_back(i);
        std::printf("Arena : 1000 push_backs (final payload 4000 bytes) requested %zu bytes in total, %zu upstream chunks\n", arena.bytes_requested, arena.chunks);
        std::size_t before = arena.bytes_requested;
        std::pmr::vector<int> w(&arena); w.reserve(1000); for (int i = 0; i < 1000; ++i) w.push_back(i);
        std::printf("Arena : the same with reserve(1000) first requested %zu bytes (growth garbage from the first vector stays in the arena)\n", arena.bytes_requested - before);
    }
    // Pool: nodes are recycled; freeing and reallocating does not grow the pool
    {
        PoolResource pool(sizeof(void*) * 3);
        std::pmr::polymorphic_allocator<long> alloc(&pool);
        long* a = alloc.allocate(2); long* b = alloc.allocate(2);
        std::printf("Pool  : two live blocks, live=%zu\n", pool.live);
        alloc.deallocate(a, 2);
        long* c = alloc.allocate(2);
        std::printf("Pool  : freed one and allocated again -> got the same slot back: %s, live=%zu\n", c == a ? "yes" : "no", pool.live);
        alloc.deallocate(b, 2); alloc.deallocate(c, 2);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Arena : 1000 push_backs (final payload 4000 bytes) requested 8188 bytes in total, 6 upstream chunks
Arena : the same with reserve(1000) first requested 4000 bytes (growth garbage from the first vector stays in the arena)
Pool  : two live blocks, live=2
Pool  : freed one and allocated again -> got the same slot back: yes, live=2
```

Both classes are ~20 lines of logic. The interesting observations are in the output: the monotonic **vector growth garbage** (the total requested is about 2× payload; this is why `reserve` first, or a bounded growth strategy, is mandatory with arenas) and the pool **recycling the very slot** just freed (LIFO free list: hot in cache).

### Experiment 6 🔧: Does it matter? Node containers under four allocators

```cpp
// @test run -std=c++23 -O2 timeout=180
#include <chrono>
#include <cstdio>
#include <list>
#include <map>
#include <memory_resource>
#include <numeric>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
template <class F> double ms(F&& f) { auto t0 = clk::now(); f(); return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }

// Build a std::pmr::map<int,int> of N random keys, sum it, destroy it.
template <class MakeRes> double run(MakeRes make, int N, std::vector<int>& keys, long& sink) {
    return ms([&] {
        auto res = make();                                           // fresh resource per run
        std::pmr::map<int, int> m{res.get()};
        for (int k : keys) m.emplace(k, k);
        for (auto& [k, v] : m) sink += v;
    });                                                              // destruction of the map (and arena release) is inside the timing
}

int main() {
    constexpr int N = 200'000, REPS = 5;
    std::mt19937 rng(7);
    std::vector<int> keys(N); std::iota(keys.begin(), keys.end(), 0); std::shuffle(keys.begin(), keys.end(), rng);
    long sink = 0;

    auto best = [&](auto make) { double b = 1e9; for (int r = 0; r < REPS; ++r) b = std::min(b, run(make, N, keys, sink)); return b; };

    double t_new   = best([] { return std::unique_ptr<std::pmr::memory_resource, void (*)(std::pmr::memory_resource*)>(std::pmr::new_delete_resource(), [](std::pmr::memory_resource*) {}); });
    double t_mono  = best([] { return std::make_unique<std::pmr::monotonic_buffer_resource>(); });
    double t_unsyn = best([] { return std::make_unique<std::pmr::unsynchronized_pool_resource>(); });
    double t_sync  = best([] { return std::make_unique<std::pmr::synchronized_pool_resource>(); });

    std::printf("pmr::map<int,int>, %d random inserts + traversal + destruction (best of %d), ms:\n", N, REPS);
    std::printf("  new_delete_resource (malloc)   %7.2f   1.00x\n", t_new);
    std::printf("  monotonic_buffer_resource      %7.2f   %.2fx faster\n", t_mono, t_new / t_mono);
    std::printf("  unsynchronized_pool_resource   %7.2f   %.2fx faster\n", t_unsyn, t_new / t_unsyn);
    std::printf("  synchronized_pool_resource     %7.2f   %.2fx faster\n", t_sync, t_new / t_sync);
    return sink == 1 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
pmr::map<int,int>, 200000 random inserts + traversal + destruction (best of 5), ms:
  new_delete_resource (malloc)    144.37   1.00x
  monotonic_buffer_resource        73.34   1.97x faster
  unsynchronized_pool_resource    136.78   1.06x faster
  synchronized_pool_resource      162.24   0.89x faster
```

What the numbers say, and what they don't. The win has three sources that you can separate with `perf stat` (instructions, cache-misses): **fewer instructions per allocation**, **no per-node free** at destruction (monotonic: destruction of the map still walks and runs destructors, but no `free` calls), and **node locality** (nodes allocated back-to-back sit together in memory). The gains are largest for node-based structures with many small allocations and shrink toward nothing for a `vector` that does a handful of large ones. A well-tuned `malloc` (tcmalloc, jemalloc, mimalloc) narrows the gap — profile *your* allocator before writing your own.

### Experiment 7 🔧: C++23 `allocate_at_least`: status check

```cpp
// @test fail -std=c++23 err=allocate_at_least
#include <memory>
int main() {
    std::allocator<int> a;
    auto r = a.allocate_at_least(3);        // C++23 (P0401). Not provided by libstdc++ 14.2.
    (void)r;
}
```

```text
# output
```

⚖️ *Standardised* in C++23; 🔧 *library support varies*: this toolchain (GCC 14.2 / libstdc++ 14) does not implement it, so we tested for the failure. Check `__cpp_lib_allocate_at_least` before relying on it; MSVC STL and libc++ have it in recent releases (verify against your versions). Its purpose: `std::string`/`vector` can learn that the allocator handed back more than was asked for (malloc rounding up to 24, 40, 56… bytes, Chapter 24) and use the slack as capacity instead of wasting it.

---

## 8. Assembly / runtime investigation

```bash
# (1) How many instructions is a monotonic allocation?  Compile a function that makes one and inspect the fast path
g++-14 -std=c++23 -O2 -S -masm=intel -o - arena.cpp | c++filt | awk '/ArenaResource::do_allocate/,/ret/'

# (2) Count malloc/free calls with and without a pool:  the difference is the allocator's share of the work
ltrace -c -e malloc+free ./with_new   2>&1 | tail -4
ltrace -c -e malloc+free ./with_arena 2>&1 | tail -4

# (3) Locality: compare cache misses on traversal of a std::list built from new vs from an arena
perf stat -e cache-misses,instructions,cycles ./list_new
perf stat -e cache-misses,instructions,cycles ./list_arena

# (4) Heap profile: how much memory does a growing pmr::vector in a monotonic buffer really consume?
valgrind --tool=massif ./prog && ms_print massif.out.* | head -40
```

---

## 9. Implementation exercise

Build `Pmr`-based infrastructure for a small **request-processing** program:

1. **`ArenaResource`** (given in Experiment 5) with `reset()` (rewind to the first chunk and keep memory), alignment handling for 64-byte types, and statistics (peak bytes, number of upstream chunks).
2. **`PoolResource`** generalised: size classes 16/32/64/128/256 with fall-through to upstream.
3. **`CountingResource`**: a decorator `memory_resource` wrapping another and recording allocations, bytes and a high-water mark; use it as the upstream of the others to *measure* them.
4. A `Request` type that is allocator-aware (Experiment 4's pattern) containing `pmr::string path`, `pmr::vector<pmr::string> headers`, `pmr::map<pmr::string, pmr::string> params`.
5. A loop that, per request, creates an arena, builds a `Request`, "handles" it, and rewinds. Verify with `CountingResource` and `ltrace` that the steady state makes **zero** `malloc` calls.

<details>
<summary><strong>Solution sketch: a counting decorator</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

class CountingResource final : public std::pmr::memory_resource {
    std::pmr::memory_resource* up_;
public:
    std::size_t allocations = 0, deallocations = 0, bytes = 0, live = 0, peak = 0;
    explicit CountingResource(std::pmr::memory_resource* up = std::pmr::get_default_resource()) : up_(up) {}
private:
    void* do_allocate(std::size_t n, std::size_t a) override {
        ++allocations; bytes += n; live += n; if (live > peak) peak = live;
        return up_->allocate(n, a);
    }
    void do_deallocate(void* p, std::size_t n, std::size_t a) override { ++deallocations; live -= n; up_->deallocate(p, n, a); }
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }
};

int main() {
    CountingResource counter;
    {
        std::pmr::monotonic_buffer_resource arena{&counter};       // arena chunks come from the counter
        std::pmr::vector<std::pmr::string> v{&arena};
        for (int i = 0; i < 1000; ++i) v.emplace_back("a string long enough to leave the small-string buffer entirely");
        std::printf("1000 strings built: arena asked upstream for %zu chunk allocations, %zu bytes (not 1000+ mallocs)\n", counter.allocations, counter.bytes);
    }
    std::printf("arena destroyed: deallocations=%zu, live=%zu\n", counter.deallocations, counter.live);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1000 strings built: arena asked upstream for 12 chunk allocations, 264320 bytes (not 1000+ mallocs)
arena destroyed: deallocations=12, live=0
```

Decorator resources compose: `CountingResource` → `monotonic_buffer_resource` → `pmr::vector`. This "wrap and measure" structure is the standard way to prove an allocation strategy does what you claim.

</details>

---

## 10. Real-world example

| Where | How allocators/arenas are used |
|---|---|
| **Compilers (LLVM, GCC, Clang)** | AST nodes, IR instructions: bump arenas (`BumpPtrAllocator`, `obstack`); the whole pass or translation unit's memory is dropped at once. This is the canonical arena use-case |
| **Game engines** | Per-frame linear allocators, level-lifetime arenas, pool allocators for particles/entities; `-fno-exceptions`, no global `new` in the frame loop |
| **Web servers / RPC** | Per-request arena: parse, handle, respond, `release()`. Protobuf's `Arena` and capnproto do exactly this; Chapter 35 and the capstone use it |
| **Databases** | Memory contexts (PostgreSQL `palloc`/MemoryContext), query-lifetime arenas, buffer-pool pages |
| **Bloomberg BSL** | The origin of `pmr`: `bslma::Allocator` is passed through every API |
| **Embedded / realtime** | Fixed pools and stack buffers (`monotonic_buffer_resource` over a static array with `null_memory_resource` upstream = "no heap, hard cap") |
| **Qt** | Containers use `malloc` through `QArrayData`; `QtPrivate` has allocator hooks, but there is no allocator parameter in the public API: **Qt chose implicit sharing over allocator awareness**. For arena-style behaviour in Qt, keep STL/PMR containers for the hot data and convert at the boundary (Chapter 48) |
| **Standard library itself** | `std::pmr::string`, `std::pmr::vector`, `std::pmr::map` and friends are the same templates with `polymorphic_allocator` (aliases in `<memory_resource>`, `<vector>`…) |

> **Opinion.** Allocators are one of the highest-leverage and most over-applied tools in C++. **Do not** write one before measuring: profile first (`perf`, `massif`, `heaptrack`) and try, in order: (1) fewer allocations by design (reserve, SSO-friendly types, `string_view`, `flat_map`/`inplace_vector`), (2) a better `malloc` via `LD_PRELOAD` (tcmalloc/jemalloc/mimalloc, zero code), (3) **`std::pmr` with a monotonic or pool resource scoped to the unit of work** — this is the sweet spot and costs a few lines — and only then (4) bespoke allocators for a proven bottleneck. Prefer **PMR to classic allocators** in new code: one container type, runtime-selectable policy, automatic propagation. Classic allocator-parameterised types remain right for **library infrastructure that must be zero-overhead and policy-fixed** (e.g. an STL-compatible container in a lock-free subsystem). And remember the two rules that bite everyone: *the resource must outlive its users*, and *a monotonic resource never gives memory back until you destroy or `release()` it* — in a long-running process, scope it to a unit of work and drop it, or it is a leak with extra steps.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Resource destroyed before containers using it | Use-after-free in container destructors | Declare the resource *before* (so it is destroyed *after*) the containers |
| Long-lived `monotonic_buffer_resource` with churn | Unbounded memory growth: frees are no-ops | Scope it to a unit of work; `release()` per cycle; use a pool for churn |
| Growing vectors in an arena without `reserve` | ~2× waste, fragmentation of the arena | `reserve`, or pre-size from a known upper bound |
| Mixing resources: `a = std::move(b)` with different resources | Elements copied/moved one by one (move does not steal the buffer) | Use one resource per unit of work; check `get_allocator()` before assuming a cheap move |
| Forgetting the allocator-extended constructors on your type | Elements silently end up on the default resource, or compile error | Provide `(args…, allocator_type)` constructors, including copy/move |
| `std::string` inside a `pmr::vector` | Doesn't use the arena (it isn't allocator-aware with *this* resource) | `pmr::string`, or convert at insertion |
| Non-trivially-destructible objects in an arena that is simply dropped | Destructors never run: leaked file handles, sockets, non-PMR members | Destroy them explicitly, or keep only POD-ish/PMR data in arenas |
| Classic allocator without the converting constructor | Compile error in `list`/`map` (cannot rebind) | Provide `template <class U> A(const A<U>&)` |
| `operator==` returns `true` for allocators with different arenas | Container steals buffers across arenas → crash | Equality = "can free each other's memory" |
| Thread-safety: sharing an `unsynchronized_pool_resource` or monotonic across threads | Data races, heap corruption | One resource per thread, or `synchronized_pool_resource` (measure its contention!) |
| Alignment ignored in a hand-rolled resource | Misaligned SIMD loads, UB | Round up to `align` as in Experiment 5; test with `alignas(64)` types |
| Deallocating with the wrong size/alignment | UB (pools index on it) | Containers pass the right values; custom code must too |
| Believing arenas always win | Slower than a good malloc for large, few allocations | Measure; arenas shine for many small, same-lifetime allocations |

---

## 12. Exercises

1. **Trace the traits.** For `std::list<int, MyAlloc<int>>`, list every `allocator_traits` operation called by `push_back` and by the destructor; which are defaulted, and what does `rebind_alloc<Node>` return?
2. **Propagation.** Write a stateful allocator (with an id) and print what happens on copy-construct, copy-assign, move-assign and swap for each setting of the three `propagate_on_*` traits and for unequal ids. Which operations become O(n)?
3. **Stack-backed container.** Implement `StackAllocator<T, N>` (classic) that serves from an inline `alignas(T) std::byte[N * sizeof(T)]` and falls back to the heap. Why is copying such an allocator dangerous? Compare with a `pmr::monotonic_buffer_resource` over a stack array.
4. **Arena growth policy.** Change `ArenaResource` to grow chunks by 1.5× vs 2× vs constant; measure waste (requested vs obtained) for 1M mixed-size allocations.
5. **Pool variants.** Add (a) per-chunk free-counts to return empty chunks to upstream, (b) thread-local pools, (c) a "debug" mode that poisons freed slots and detects double free. What does each cost per operation?
6. **Prove the claim.** Use `perf stat` to decompose Experiment 6's speed-up into instruction count and cache-miss components. Repeat after shuffling the arena-built list's traversal order (random access) — what happens?
7. **PMR everywhere.** Take a parser (JSON, INI) of yours and convert it so that the whole parse tree lives in a single `monotonic_buffer_resource`. Measure parse+destroy time and peak RSS.
8. **Write the safe `reset`.** Implement `Arena::reset()` that keeps only the largest chunk. Show a use-after-reset bug and make AddressSanitizer catch it via `__asan_poison_memory_region` in `reset`.

---

## 13. Challenge: a request-scoped runtime allocator

Design `RequestMemory`, a `memory_resource` for the capstone's HTTP server:

- a per-thread, per-request arena with a **fixed first chunk** embedded in the request object (e.g. 16 KiB) and geometric overflow chunks from a thread-local pool;
- `std::pmr` containers for the parsed request (headers, query, path) and response body;
- zero `malloc` calls on the steady-state path for requests that fit the inline chunk (prove with `ltrace -c` or an interposed `malloc` counter);
- `reset()` between requests in O(number of chunks), poisoning the old memory under ASan builds;
- a **safety design**: how do you stop a handler from stashing a `pmr::string` that outlives the request? (Consider: debug-mode generation counters, move-only wrappers, and documenting that only copies into the default resource may escape.)
- benchmark against the default allocator at 1/4/16 threads and report p50/p99 latency, not just throughput.

---

## 14. Knowledge check

1. Which operations does a container call on an allocator, and which does `allocator_traits` supply for you?
2. Why must a classic allocator have a converting constructor from `A<U>`?
3. What does `operator==` mean for allocators, and what breaks if it lies?
4. Why can't a `std::vector<int, A1>` be passed to a function taking `std::vector<int>`? How does PMR address this?
5. What does `monotonic_buffer_resource::deallocate` do? What does this imply for a vector that grows by doubling?
6. What is uses-allocator construction? Why does a `pmr::vector<pmr::string>` need no `scoped_allocator_adaptor`?
7. Why does `polymorphic_allocator` not propagate on container assignment, and what does that mean for `v1 = std::move(v2)` with different resources?
8. What are the two reasons an arena-allocated `std::map` is faster than a heap-allocated one? Which one persists even against a very fast malloc?
9. When is the PMR dispatch cost significant, and what can you do about it?
10. Why use `null_memory_resource()` as the upstream of a stack-buffer arena?
11. Name two kinds of objects that must not be left in a "just drop it" arena.
12. What is `allocate_at_least` for, and what is its status in your toolchain?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. The container calls `allocate`/`deallocate` (on the allocator directly or through traits) and `allocator_traits::construct`/`destroy` for elements. `allocator_traits` supplies defaults for `construct`, `destroy`, `max_size`, `pointer` typedefs, `rebind_alloc`, propagation traits, `select_on_container_copy_construction`, `is_always_equal`.
2. Node-based containers allocate internal node types, not `T`; they rebind the allocator to the node type by converting `A<T>` to `A<Node>`.
3. `a == b` means memory allocated by `a` can be deallocated by `b`. If it lies (returns true for arenas that differ), moves/swaps will transfer buffers that the receiving allocator cannot free → corruption.
4. The allocator is part of the type, so `vector<int, A1>` is a different type from `vector<int>`. PMR fixes the allocator type (`polymorphic_allocator`) and moves the policy to a runtime `memory_resource*`.
5. Nothing. A doubling vector therefore leaves behind buffers of size 1, 2, 4, … that are never reused: total consumption ≈ 2× the final size unless you `reserve`.
6. When constructing an element of an allocator-aware type, the container passes its allocator to the element's constructor. Because `pmr::string` and `pmr::vector` are allocator-aware with `polymorphic_allocator`, a single resource flows into the nested structure automatically.
7. By design: the container keeps its own resource, so assignment never changes where a container's memory lives. Moving from a container with a *different* resource therefore moves element by element (allocating in the destination's resource) rather than stealing the buffer.
8. Fewer instructions per allocation/deallocation; and node locality (contiguous nodes → fewer cache misses). The locality benefit persists even with a very fast malloc.
9. When allocation is extremely frequent and cheap per call (tiny nodes in a hot loop). Reduce the number of allocations, make resources `final` and visible to the optimiser, or use a classic allocator type in that one container.
10. If the data outgrows the buffer, it throws immediately instead of silently falling back to the heap: it enforces and proves "no heap allocation".
11. Objects whose destructors release non-memory resources (file descriptors, sockets, locks); objects holding pointers into non-arena memory that must be released; anything with a non-PMR member that allocates.
12. It lets an allocator report that it provided more space than requested so containers can use the slack as capacity (P0401, C++23). Not implemented in GCC 14.2's libstdc++ (Experiment 7).

</details>

---

[← Previous: Chapter 25](25-smart-pointers.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 27 — Cache and data-oriented C++ →](27-cache-and-data-oriented-cpp.md)
