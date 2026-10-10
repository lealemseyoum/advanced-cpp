# Chapter 25 — Smart Pointers: Ownership as a Type

> **Part X · Memory** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 4](../part-02-object-model-and-lifetime/04-raii.md), [Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 24](24-dynamic-memory.md) &nbsp;|&nbsp; **Standards:** C++11 (`unique_ptr`, `shared_ptr`, `weak_ptr`), C++14 (`make_unique`), C++17 (`shared_ptr<T[]>`, `weak_from_this`), C++20 (`make_shared<T[]>`, `atomic<shared_ptr>`, `make_unique_for_overwrite`), C++23 (`out_ptr`, `inout_ptr`) &nbsp;|&nbsp; **Tools:** `g++-14`

[← Previous: Chapter 24](24-dynamic-memory.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 26 — Allocators and memory resources →](26-allocators-and-memory-resources.md)

---

**In one sentence:** a raw pointer says *where*; a smart pointer says *where and who is responsible for ending it*, and picking the wrong answer to "who" (especially `shared_ptr` by habit) is the most common ownership design error in C++.

**By the end of this chapter you can:**

- choose between `T*`, `T&`, `unique_ptr`, `shared_ptr`, `weak_ptr`, `span` and an observer type for any parameter, member or return value, and justify it
- draw the memory layout of `unique_ptr`, `shared_ptr` and `make_shared`, including the control block
- explain exactly what is atomic in `shared_ptr`, what is *not*, and what that costs (measured)
- use custom deleters, aliasing constructors, `enable_shared_from_this` and `weak_ptr` correctly
- explain why `shared_ptr` is frequently overused, and what to do instead

---

## 1. Problem

Chapter 4 gave us RAII: tie a resource to an object's lifetime. A raw pointer is not such an object. `T*` conflates **five different meanings**, none visible in the type:

```cpp
T* a();     // returns: a new object I must delete?  a pointer into a container?  a global?  nullptr on failure?
void b(T*); // takes:   ownership?  a borrow for the call?  an array?  may it be null?  may I keep it?
```

`new`/`delete` pairs written by hand leak on every early return and every exception (Chapter 22); `delete` called twice or on the wrong pointer is undefined behaviour (Chapter 28). We need ownership to be *explicit in the type* and enforced by the destructor.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1980s–90s | Hand-written ownership conventions: "caller frees", `Foo_destroy()`, naming (`GetFoo` vs `CreateFoo`) |
| 1998 | `std::auto_ptr` (C++98): "copy" silently *transfers* ownership. Broke in containers and algorithms, deprecated in C++11, **removed in C++17** |
| 1999–2005 | Boost `scoped_ptr`, `shared_ptr`, `weak_ptr`, `intrusive_ptr`; Loki policy-based pointers |
| 2005 | TR1 adds `shared_ptr` |
| **2011** | `unique_ptr` is possible because **move semantics** exist (Chapter 6); `shared_ptr`/`weak_ptr` standardised; `auto_ptr` deprecated |
| 2014 | `make_unique` completes the "no naked `new`" guideline |
| 2017 | `shared_ptr<T[]>`; `auto_ptr` removed; `weak_from_this` |
| 2020 | `atomic<shared_ptr<T>>`; `make_shared<T[]>`; `make_unique_for_overwrite` |
| 2023 | `std::out_ptr` / `std::inout_ptr`: adapt smart pointers to C APIs that hand back raw pointers |
| 2025–26 | Proposals for `std::observer_ptr`, `std::indirect<T>`/`std::polymorphic<T>` (value-semantic owning wrappers) — **`indirect`/`polymorphic` were adopted for C++26** (P3019); library support is still arriving, verify before use |

---

## 3. Modern solution

```cpp
// Ownership vocabulary (the rule of thumb you should be able to recite):
std::unique_ptr<T>        // sole owner.        Move-only. The default owner.
std::shared_ptr<T>        // shared owner.      Reference-counted. Use when lifetime truly is shared.
std::weak_ptr<T>          // non-owning observer of a shared_ptr-managed object that may disappear.
T&  /  const T&           // borrow: non-null, doesn't outlive the call (or the owner).
T*  /  std::span<T>       // borrow: nullable / a range. Never owns. Never deleted by the receiver.
std::optional<T>          // value that may be absent; owns its storage inline (no heap).
```

The function-signature rules that follow (C++ Core Guidelines R.30–R.37, F.7):

| You want to… | Parameter type | Why |
|---|---|---|
| use the object, no ownership | `T&` / `const T&` (or `T*` if nullable, `span` for ranges) | The caller keeps ownership; works with any owner |
| take over ownership | `std::unique_ptr<T>` **by value** | Visible at the call site (`f(std::move(p))`); callee owns it |
| share ownership (store a copy) | `std::shared_ptr<T>` **by value** | The callee will keep it; copying is the point |
| *maybe* share ownership | `const std::shared_ptr<T>&` | Rare: only if you sometimes copy it |
| use a `shared_ptr`'s pointee | **not** a `shared_ptr` parameter at all: `T&` | Don't couple the callee to how the caller manages lifetime |

---

## 4. Mental model

### Ownership is a graph; pick the sparsest one

```text
   unique ownership:  a tree             shared ownership:  a DAG (or worse: a graph with cycles)
        A                                     A ───► C
       / \                                     \     ▲
      B   C                                     B ───┘        who deletes C? whoever lets go last
                                                              (and when is that? read the whole program)
   lifetime = scope of the owner         lifetime = emergent property of the whole system
```

A `unique_ptr` makes the destruction point **local and deterministic**: it is where the owner dies. A `shared_ptr` makes it a global property: "the last holder" is not a place in the source code. That is why shared ownership should be an explicit design decision (a cache, a graph, an async operation outliving its starter), not a default.

### Layouts

```text
 unique_ptr<T>                        shared_ptr<T> (from make_shared<T>)
 ┌───────┐                            ┌──────────┬──────────┐        ┌───────────────────────┐
 │  T*   │ 8 bytes (empty deleter     │   T*     │ ctrl*  ──┼───────►│ vptr (control block)  │
 └───────┘ costs nothing: EBO /       └──────────┴──────────┘        │ use_count   (atomic)  │
           [[no_unique_address]])       16 bytes                      │ weak_count  (atomic)  │
                                        ptr to object + ptr to        │ T  (the object itself)│  ← one allocation
 unique_ptr<T, void(*)(T*)>             control block                 └───────────────────────┘
 ┌───────┬────────────┐
 │  T*   │ deleter*   │ 16 bytes       shared_ptr<T>(new T): two allocations (object, then control block)
 └───────┴────────────┘
```

The control block holds the **strong** count (owners), the **weak** count (`weak_ptr`s, plus one while any strong owner exists), and a type-erased deleter/allocator — why a `shared_ptr` can hold a custom deleter without it being part of its type (Chapter 21 in action).

### `weak_ptr` is a ticket, not a reference

A `weak_ptr` keeps the **control block** alive but not the object. `lock()` atomically checks "is the strong count non-zero?" and, if so, increments it and returns a `shared_ptr`. The object is destroyed when the strong count reaches 0; its *memory* (in the `make_shared` case) is released only when the weak count also reaches 0.

---

## 5. Language rules

### 5.1 `unique_ptr<T, D>`  `[unique.ptr]`

- **Move-only**: copy operations are deleted; move transfers the pointer and nulls the source. This is what move semantics were *for*.
- The deleter `D` is part of the type. The default `std::default_delete<T>` calls `delete`; for arrays `unique_ptr<T[]>` calls `delete[]`.
- Stateless deleters cost **nothing** (empty-base optimisation); function-pointer deleters add 8 bytes; capturing lambdas add their capture size.
- `unique_ptr` is not trivially destructible, so passing it by value uses an **invisible reference** in the Itanium C++ ABI (§6, Experiment 2): it is not literally "free like a pointer" across calls, though almost always cheap.
- Converting `unique_ptr<Derived>` → `unique_ptr<Base>` works implicitly; **the base needs a virtual destructor** (or a deleter that remembers the derived type).
- `reset()`, `release()`, `get()`, `operator bool`, `operator->`, `operator*` — `release()` gives up ownership *without* deleting (you now own the raw pointer).

### 5.2 `shared_ptr<T>`  `[util.smartptr.shared]`

- **Copying** increments the strong count; destroying/resetting decrements; at zero the deleter runs on the stored pointer (the control block's pointer, which can differ from `get()` — aliasing).
- **Thread-safety** (the sentence people misquote): the *control block* is thread-safe, i.e. copies of a `shared_ptr` made and destroyed in different threads are safe. **The pointee is not protected**, and **the same `shared_ptr` object being modified from two threads is a data race** (reassigning a shared `shared_ptr` global while another thread reads it). C++20 provides `std::atomic<std::shared_ptr<T>>` for that.
- `make_shared<T>(args…)`: one allocation for control block + object; exception-safe in argument lists; slightly better locality. Trade-off: **the object's memory outlives its destruction as long as any `weak_ptr` exists** (a big object can be pinned by a tiny `weak_ptr`).
- `shared_ptr<T>(new T)`: two allocations, but memory of `T` is released as soon as the strong count hits zero. Also needed when you must adopt a pointer with a custom deleter or allocation function.
- **Aliasing constructor** `shared_ptr<T>(const shared_ptr<U>& owner, T* p)`: shares `owner`'s control block but exposes `p`. Used to point at a member (`shared_ptr<Member>` keeping the whole parent alive).
- `owner_before` / `owner_equal`: compare by *control block* rather than by pointer, the right ordering for sets/maps of `weak_ptr`.
- **Never create two independent `shared_ptr`s from the same raw pointer**; each makes its own control block and both will delete (double free). Use `enable_shared_from_this` to hand out more `shared_ptr`s from inside the object.

### 5.3 `weak_ptr<T>`  `[util.smartptr.weak]`

Non-owning handle to an object managed by `shared_ptr`. `expired()` is a snapshot that can be stale the instant it returns; the correct pattern is to **`lock()` once, test the result, and use that `shared_ptr`**.

### 5.4 `enable_shared_from_this<T>`

A base class holding a `weak_ptr` that the first `shared_ptr` creation fills in. `shared_from_this()` is **undefined/throws `bad_weak_ptr` if no `shared_ptr` owns the object yet** (calling it from the constructor, or on a stack object, is the classic mistake).

### 5.5 Adapting to C APIs: `out_ptr` / `inout_ptr` (C++23)

C functions that return a freshly allocated object through `T**` can be wrapped: `c_api_create(std::out_ptr(uptr, deleter))` resets `uptr` after the call and manages the temporary raw pointer properly. Prior to C++23 you wrote `T* raw; c_api(&raw); up.reset(raw);` by hand and leaked if anything came in between.

### 5.6 Comparison table

| Property | `unique_ptr` | `shared_ptr` | `weak_ptr` | `T*` |
|---|---|---|---|---|
| Owns | yes, solely | yes, jointly | no | no |
| Copyable | no (move only) | yes (atomic inc) | yes | yes |
| Size (x86-64, default deleter) | 8 | 16 | 16 | 8 |
| Extra allocation | none | control block (or fused with `make_shared`) | shares block | none |
| Deleter in the type? | yes | no (type-erased) | no | n/a |
| Overhead on copy/destroy | none | atomic RMW (unless provably single-threaded) | atomic RMW | none |
| Destruction time | at owner's scope end | when last owner goes away (non-local) | n/a | n/a |
| Cycles | impossible by construction (tree) | **leak** | the cure | n/a |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is guaranteed? | `unique_ptr` is a zero-state-overhead owner (with a stateless deleter, `sizeof` is *not* required to equal a pointer's, but typical); `shared_ptr` control-block operations are thread-safe, with *one* strong count that is atomically updated; `make_shared` performs a single allocation "typically" (a recommendation) |
| **Compiler / library** | What does libstdc++ do? | Control block with `_M_use_count`/`_M_weak_count`; `make_shared` uses `_Sp_counted_ptr_inplace`; **skips atomics when the process is provably single-threaded** (`__libc_single_threaded`, GCC 11+/glibc 2.32+) — Experiment 3 |
| **ABI (Itanium)** | How are they passed? | `unique_ptr`/`shared_ptr` are **non-trivial for the purposes of calls** → passed by **invisible reference** and returned through a hidden pointer, not in registers |
| **OS** | — | Threads and futex-based scheduling matter only for contention, not for the count itself |
| **CPU** | What does a copy cost? | An atomic `lock xadd` on the count: ~5–20 cycles uncontended, **hundreds of cycles when the cache line ping-pongs between cores** |

---

## 6. Implementation model

### `unique_ptr` is a struct with one pointer — but its ABI is not a pointer's

```cpp
template <class T, class D = default_delete<T>>
class unique_ptr {
    [[no_unique_address]] D deleter_;      // libstdc++ uses a tuple/EBO to the same effect
    T* ptr_ = nullptr;
public:
    ~unique_ptr() { if (ptr_) deleter_(ptr_); }   // user-provided → NOT trivially destructible
    /* move ctor/assign: steal; copy: = delete */
};
```

Its destructor makes it **non-trivial for the purposes of calls**. Under SysV x86-64, such a class cannot live in a register across a call boundary: the caller materialises it in memory and passes its address. So a `void f(std::unique_ptr<int>)` is not as cheap as `void f(int*)`; the cost is a store and a load, plus the caller's responsibility to destroy the (now empty) parameter object. Experiment 2 shows the instructions. It is a few cycles — but it **does** mean `unique_ptr` is not a perfectly zero-cost replacement for a raw pointer in *calling-convention-sensitive hot paths*, and it is the argument for passing `T&`/`T*` to functions that merely use the object.

### `shared_ptr` control block

```text
   struct _Sp_counted_base { virtual dispose(); virtual destroy(); atomic<int> use_count=1; atomic<int> weak_count=1; };
   //  _Sp_counted_ptr<T*, Deleter>            : stores the pointer + deleter       (shared_ptr<T>(new T, d))
   //  _Sp_counted_ptr_inplace<T, Alloc>       : storage for T lives inside the block (make_shared / allocate_shared)
```

The two virtual functions are the *type erasure* of Chapter 21: `dispose()` runs the object's destructor/deleter when the strong count hits zero; `destroy()` frees the block itself when the weak count hits zero.

### Single-thread fast path

libstdc++ (GCC 11+) checks `__libc_single_threaded`: if the process has never created a thread, counts are updated with plain (non-atomic) instructions. The check happens on every copy, so you pay a load and a branch instead of a `lock`-prefixed instruction until the first `std::thread` is created. **That is why microbenchmarks of `shared_ptr` copies in `main` alone mislead**: they measure the non-atomic path. Experiment 3 demonstrates this switch directly.

---

## 7. Experiments

### Experiment 1 ✅: Sizes, layout, and the number of allocations

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

static int g_allocs = 0;
static std::size_t g_last = 0;
void* operator new(std::size_t n) { ++g_allocs; g_last = n; void* p = std::malloc(n); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Big { char data[100]; };
struct Deleter { void operator()(int* p) const { delete p; } };

int main() {
    std::puts("sizes (bytes):");
    std::printf("  int*                                  %zu\n", sizeof(int*));
    std::printf("  unique_ptr<int>                       %zu\n", sizeof(std::unique_ptr<int>));
    std::printf("  unique_ptr<int, stateless deleter>    %zu\n", sizeof(std::unique_ptr<int, Deleter>));
    std::printf("  unique_ptr<int, void(*)(int*)>        %zu\n", sizeof(std::unique_ptr<int, void (*)(int*)>));
    auto cap = [k = 1.0, j = 2.0](int* p) { delete p; (void)k; (void)j; };
    std::printf("  unique_ptr<int, lambda with 2 doubles> %zu\n", sizeof(std::unique_ptr<int, decltype(cap)>));
    std::printf("  shared_ptr<int>                       %zu\n", sizeof(std::shared_ptr<int>));
    std::printf("  weak_ptr<int>                         %zu\n", sizeof(std::weak_ptr<int>));

    std::puts("allocations:");
    int before = g_allocs;
    { std::shared_ptr<Big> a(new Big); }
    std::printf("  shared_ptr<Big>(new Big)      : %d allocations (object + control block)\n", g_allocs - before);
    before = g_allocs;
    { auto b = std::make_shared<Big>(); }
    std::printf("  make_shared<Big>()            : %d allocation  (control block and object fused)\n", g_allocs - before);
    before = g_allocs;
    { auto c = std::make_unique<Big>(); }
    std::printf("  make_unique<Big>()            : %d allocation\n", g_allocs - before);

    std::puts("fused layout: how big is the single block?");
    auto s = std::make_shared<Big>();
    std::printf("  make_shared<Big> requested %zu bytes; sizeof(Big) = %zu; difference = %zu bytes of control block\n",
                g_last, sizeof(Big), g_last - sizeof(Big));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizes (bytes):
  int*                                  8
  unique_ptr<int>                       8
  unique_ptr<int, stateless deleter>    8
  unique_ptr<int, void(*)(int*)>        16
  unique_ptr<int, lambda with 2 doubles> 24
  shared_ptr<int>                       16
  weak_ptr<int>                         16
allocations:
  shared_ptr<Big>(new Big)      : 2 allocations (object + control block)
  make_shared<Big>()            : 1 allocation  (control block and object fused)
  make_unique<Big>()            : 1 allocation
fused layout: how big is the single block?
  make_shared<Big> requested 120 bytes; sizeof(Big) = 100; difference = 20 bytes of control block
```

### Experiment 2 🧩: What a `unique_ptr` costs at the call boundary

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=take_raw,take_unique,use_ref,caller_unique
#include <memory>

void sink_raw(int*);
void sink_unique(std::unique_ptr<int>);
int* consume(int*);

int take_raw(int* p)                          { return *p + 1; }
int use_ref(const int& r)                     { return r + 1; }
void take_unique(std::unique_ptr<int> p)      { sink_raw(p.get()); }          // callee owns p, must destroy it
void caller_unique(int v)                     { sink_unique(std::make_unique<int>(v)); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
take_raw(int*):
	mov	eax, DWORD PTR [rdi]
	add	eax, 1
	ret

use_ref(int const&):
	mov	eax, DWORD PTR [rdi]
	add	eax, 1
	ret

take_unique(std::unique_ptr<int, std::default_delete<int> >):
	mov	rdi, QWORD PTR [rdi]
	jmp	sink_raw(int*)@PLT

caller_unique(int):
	push	rbx
	mov	ebx, edi
	mov	edi, 4
	sub	rsp, 16
	call	operator new(unsigned long)@PLT
	lea	rdi, 8[rsp]
	mov	DWORD PTR [rax], ebx
	mov	QWORD PTR 8[rsp], rax
	call	sink_unique(std::unique_ptr<int, std::default_delete<int> >)@PLT
	mov	rdi, QWORD PTR 8[rsp]
	test	rdi, rdi
	je	.L5
	mov	esi, 4
	call	operator delete(void*, unsigned long)@PLT
.L5:
	add	rsp, 16
	pop	rbx
	ret
.L9:
	mov	rbx, rax
	jmp	.L7

caller_unique(int) [clone .cold]:
.L7:
	mov	rdi, QWORD PTR 8[rsp]
	test	rdi, rdi
	je	.L8
	mov	esi, 4
	call	operator delete(void*, unsigned long)@PLT
.L8:
	mov	rdi, rbx
	call	_Unwind_Resume@PLT
```

`take_raw` and `use_ref` are identical: a load, an add, a return. The `unique_ptr` versions reveal the Itanium rule for non-trivially-destructible parameters:

- The argument arrives **by address**: `take_unique` starts with `mov rdi, [rdi]` to load the pointer out of the caller's object.
- **The callee does not destroy it.** In the Itanium C++ ABI the *caller* owns the parameter object's storage and runs its destructor after the call returns. That is why `take_unique` is just a load and a tail call (`jmp sink_raw`), and why `caller_unique` contains the null-check-and-`operator delete` *after* `call sink_unique`, plus a duplicate of that cleanup in the `.cold` landing pad for the exception path.
- Consequence for "ownership transfer": when you write `void sink(std::unique_ptr<T> p)`, the object is *moved into the parameter* and the caller's frame still frees it. For reasoning about **when** the destructor runs, treat the parameter's lifetime as ending at the end of the caller's full-expression (the standard says the implementation chooses; Itanium chooses "caller", MSVC's ABI chooses "callee"), so never depend on it.

The price is a store and a load per call instead of a register, and a cleanup path in every caller. Small, but **not zero**, which is the argument for passing `T&` to functions that merely use the object. (Compare Chapter 6's observation on passing `std::string` by value.)

### Experiment 3 🔧: What a `shared_ptr` copy costs (and the single-thread illusion)

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

[[gnu::noinline]] long by_value(std::shared_ptr<int> p)      { return *p; }          // copy: refcount in + out
[[gnu::noinline]] long by_cref(const std::shared_ptr<int>& p){ return *p; }          // no copy
[[gnu::noinline]] long by_raw(const int* p)                  { return *p; }          // no ownership

template <class F> double ns_per_iter(F&& f, int n, long& sink) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) { sink += f(); asm volatile("" ::: "memory"); }   // barrier: stop GCC hoisting the 'pure' calls
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
}

int main() {
    auto sp = std::make_shared<int>(3);
    long sink = 0;
    constexpr int N = 20'000'000;

    std::puts("Phase A: process has NEVER created a thread (libstdc++ uses non-atomic counts)");
    std::printf("  by value %6.2f ns   by const& %6.2f ns   raw %6.2f ns\n",
        ns_per_iter([&] { return by_value(sp); }, N, sink),
        ns_per_iter([&] { return by_cref(sp); }, N, sink),
        ns_per_iter([&] { return by_raw(sp.get()); }, N, sink));

    std::thread([] {}).join();            // after this, __libc_single_threaded is false for the rest of the process
    std::puts("Phase B: after one thread has been created and joined (counts are now atomic)");
    std::printf("  by value %6.2f ns   by const& %6.2f ns   raw %6.2f ns\n",
        ns_per_iter([&] { return by_value(sp); }, N, sink),
        ns_per_iter([&] { return by_cref(sp); }, N, sink),
        ns_per_iter([&] { return by_raw(sp.get()); }, N, sink));

    std::puts("Phase C: T threads copying THE SAME shared_ptr (contended cache line); ns per copy per thread");
    for (int T : {1, 2, 4}) {
        constexpr int M = 3'000'000;
        std::vector<std::thread> ts; std::atomic<long> total{0};
        auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < T; ++t) ts.emplace_back([&] { long l = 0; for (int i = 0; i < M; ++i) l += by_value(sp); total += l; });
        for (auto& t : ts) t.join();
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / M;
        std::printf("  %d thread(s): %7.1f ns per copy\n", T, ns);
        sink += total;
    }
    return sink == 12345 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Phase A: process has NEVER created a thread (libstdc++ uses non-atomic counts)
  by value   5.60 ns   by const&   1.76 ns   raw   2.45 ns
Phase B: after one thread has been created and joined (counts are now atomic)
  by value  17.75 ns   by const&   1.29 ns   raw   1.63 ns
Phase C: T threads copying THE SAME shared_ptr (contended cache line); ns per copy per thread
  1 thread(s):    17.4 ns per copy
  2 thread(s):    47.4 ns per copy
  4 thread(s):   120.9 ns per copy
```

How to read it: Phase A shows the cost of the count updates when libstdc++ can prove there is only one thread (a few ns above `const&`). In Phase B the same code performs `lock`-prefixed increments and decrements: still cheap when uncontended, but roughly three times slower. Phase C is the real warning: when several threads copy the *same* `shared_ptr`, every copy and every destruction bounces the control block's cache line between cores, and the cost per copy grows with the number of threads. **Sharing ownership across threads is not free, and the cost scales the wrong way.** (Numbers are machine-dependent; the shape is not.)

### Experiment 4 ✅: Aliasing constructor, `weak_ptr`, and `make_shared` memory pinning

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>

struct Node {
    int id; double payload[4];
    explicit Node(int i) : id(i), payload{} { std::printf("  Node(%d) constructed\n", id); }
    ~Node() { std::printf("  Node(%d) destroyed\n", id); }
};

int main() {
    std::puts("aliasing constructor: pointer to a MEMBER, ownership of the WHOLE object");
    std::shared_ptr<double> member;
    {
        auto node = std::make_shared<Node>(1);
        member = std::shared_ptr<double>(node, &node->payload[2]);      // shares node's control block
        std::printf("  node.use_count()=%ld, member.use_count()=%ld, get() differ: %d\n",
                    node.use_count(), member.use_count(), (void*)node.get() != (void*)member.get());
    }
    std::puts("  node's shared_ptr left scope; Node survives because 'member' keeps the control block's strong count");
    std::printf("  member still valid, use_count=%ld\n", member.use_count());
    member.reset();                                                      // now Node(1) is destroyed

    std::puts("weak_ptr: lock() once, then use the result");
    std::weak_ptr<Node> weak;
    {
        auto strong = std::make_shared<Node>(2);
        weak = strong;
        if (auto locked = weak.lock()) std::printf("  alive, id=%d, use_count=%ld\n", locked->id, locked.use_count());
    }
    std::printf("  after owner died: expired=%d, lock() is %s\n", weak.expired(), weak.lock() ? "non-null" : "null");

    std::puts("two independent shared_ptrs from the same raw pointer is a DOUBLE FREE (shown here with a safe analogue):");
    auto first = std::make_shared<Node>(3);
    std::shared_ptr<Node> second = first;                                // correct: copy the shared_ptr
    std::printf("  copy of shared_ptr: use_count=%ld (one control block)\n", first.use_count());
    // std::shared_ptr<Node> bad(first.get());                           // WRONG: new control block, double delete
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
aliasing constructor: pointer to a MEMBER, ownership of the WHOLE object
  Node(1) constructed
  node.use_count()=2, member.use_count()=2, get() differ: 1
  node's shared_ptr left scope; Node survives because 'member' keeps the control block's strong count
  member still valid, use_count=1
  Node(1) destroyed
weak_ptr: lock() once, then use the result
  Node(2) constructed
  alive, id=2, use_count=2
  Node(2) destroyed
  after owner died: expired=1, lock() is null
two independent shared_ptrs from the same raw pointer is a DOUBLE FREE (shown here with a safe analogue):
  Node(3) constructed
  copy of shared_ptr: use_count=2 (one control block)
  Node(3) destroyed
```

### Experiment 5 ✅: A cycle leaks; a `weak_ptr` breaks it

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>

struct Child;
struct Parent {
    std::shared_ptr<Child> child;
    ~Parent() { std::puts("  ~Parent"); }
};
struct Child {
    std::shared_ptr<Parent> parent_strong;        // BAD: cycle
    std::weak_ptr<Parent>   parent_weak;          // GOOD: back-reference does not own
    ~Child() { std::puts("  ~Child"); }
};

int main() {
    std::puts("cycle with shared_ptr back-reference:");
    {
        auto p = std::make_shared<Parent>();
        p->child = std::make_shared<Child>();
        p->child->parent_strong = p;
        std::printf("  parent use_count=%ld, child use_count=%ld\n", p.use_count(), p->child.use_count());
    }
    std::puts("  (scope ended: NO destructor ran: both objects leaked)");

    std::puts("cycle broken with weak_ptr back-reference:");
    {
        auto p = std::make_shared<Parent>();
        p->child = std::make_shared<Child>();
        p->child->parent_weak = p;
        std::printf("  parent use_count=%ld (weak refs don't count), child use_count=%ld\n", p.use_count(), p->child.use_count());
    }
    std::puts("  (both destroyed)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
cycle with shared_ptr back-reference:
  parent use_count=2, child use_count=1
  (scope ended: NO destructor ran: both objects leaked)
cycle broken with weak_ptr back-reference:
  parent use_count=1 (weak refs don't count), child use_count=1
  ~Parent
  ~Child
  (both destroyed)
```

Run this under `valgrind --leak-check=full` or `-fsanitize=leak`; the first block is reported as a leak even though no pointer was "lost". The fix is design, not tooling: decide which direction **owns**, and make the other direction non-owning (`weak_ptr`, raw pointer, or index).

### Experiment 6 ✅: Custom deleters and `unique_ptr` with C resources

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <memory>

// 1. A stateless deleter for FILE*: zero extra bytes
struct FileCloser { void operator()(std::FILE* f) const noexcept { if (f) { std::puts("  fclose"); std::fclose(f); } } };
using File = std::unique_ptr<std::FILE, FileCloser>;

// 2. A C API that hands back a raw pointer through T**  (like many C libraries)
extern "C" int make_buffer(char** out) { *out = static_cast<char*>(std::malloc(16)); return *out ? 0 : -1; }
struct FreeDeleter { void operator()(void* p) const noexcept { std::puts("  free"); std::free(p); } };

int main() {
    std::printf("sizeof(File) = %zu (same as a raw FILE*)\n", sizeof(File));
    { File f(std::fopen("/dev/null", "r")); std::printf("  opened: %s\n", f ? "yes" : "no"); }

    // C++23: out_ptr resets the unique_ptr after the call; no raw-pointer temporary to leak
    std::unique_ptr<char, FreeDeleter> buf;
    if (make_buffer(std::out_ptr(buf)) == 0) std::printf("buffer acquired through out_ptr: %s\n", buf ? "owned" : "null");
    buf.reset();

    // 3. A deleter that is type-erased by shared_ptr: the deleter is NOT part of the type
    std::shared_ptr<std::FILE> sf(std::fopen("/dev/null", "r"), [](std::FILE* f) { std::puts("  shared_ptr deleter: fclose"); std::fclose(f); });
    std::printf("sizeof(shared_ptr<FILE>) = %zu (deleter lives in the control block)\n", sizeof(sf));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(File) = 8 (same as a raw FILE*)
  opened: yes
  fclose
buffer acquired through out_ptr: owned
  free
sizeof(shared_ptr<FILE>) = 16 (deleter lives in the control block)
  shared_ptr deleter: fclose
```

---

## 8. Assembly / runtime investigation

```bash
# (1) Count atomic instructions in a shared_ptr-heavy function:  lock xadd / lock dec
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | grep -c "lock"

# (2) See libstdc++'s single-thread fast path:  it loads __libc_single_threaded then branches
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | grep -B2 -A4 "__libc_single_threaded"

# (3) How many allocations does this code path make?  (do NOT use a replaced operator new at -O2, Chapter 24)
ltrace -e malloc ./prog 2>&1 | wc -l        # or: heaptrack ./prog

# (4) Show the control block in gdb:
(gdb) p sp
(gdb) p *(std::_Sp_counted_base<2>*)sp._M_refcount._M_pi          # _M_use_count, _M_weak_count
```

---

## 9. Implementation exercise

Implement, in this order:

1. **`UniquePtr<T, D>`**: move-only, EBO for the deleter, `release/reset/swap/get`, comparison with `nullptr`, conversion from `UniquePtr<Derived>`; array specialisation.
2. **`SharedPtr<T>` + `WeakPtr<T>`** with a type-erased control block (strong/weak counts, `dispose()`/`destroy()` virtuals), `make_shared`-style fused allocation, aliasing constructor, and `enable_shared_from_this`.
3. **Thread-safety**: make the counts `std::atomic<long>` with the correct memory orders (`relaxed` increment; `acq_rel` decrement; the "last owner" acquire) — Chapters 30–31 explain why.
4. **Tests**: leak and double-free tests under ASan; a cycle test; a thread stress test under TSan.

<details>
<summary><strong>Solution sketch: `UniquePtr` with EBO</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

template <class T, class D = std::default_delete<T>>
class UniquePtr {
    [[no_unique_address]] D del_;
    T* p_ = nullptr;
public:
    constexpr UniquePtr() noexcept = default;
    explicit UniquePtr(T* p) noexcept : p_(p) {}
    UniquePtr(T* p, D d) noexcept : del_(std::move(d)), p_(p) {}
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;
    UniquePtr(UniquePtr&& o) noexcept : del_(std::move(o.del_)), p_(std::exchange(o.p_, nullptr)) {}
    UniquePtr& operator=(UniquePtr&& o) noexcept {
        if (this != &o) { reset(); del_ = std::move(o.del_); p_ = std::exchange(o.p_, nullptr); }
        return *this;
    }
    template <class U, class E> requires std::is_convertible_v<U*, T*> && std::is_convertible_v<E, D>
    UniquePtr(UniquePtr<U, E>&& o) noexcept : del_(std::move(o.get_deleter())), p_(o.release()) {}
    ~UniquePtr() { reset(); }

    T* release() noexcept { return std::exchange(p_, nullptr); }
    void reset(T* np = nullptr) noexcept { if (T* old = std::exchange(p_, np)) del_(old); }
    T* get() const noexcept { return p_; }
    D& get_deleter() noexcept { return del_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    T& operator*() const { return *p_; }
    T* operator->() const noexcept { return p_; }
};

struct Loud { ~Loud() { std::puts("  ~Loud"); } };
int main() {
    static_assert(sizeof(UniquePtr<int>) == sizeof(int*), "empty deleter must cost nothing");
    static_assert(sizeof(UniquePtr<int, void (*)(int*)>) == 2 * sizeof(int*));
    UniquePtr<Loud> a(new Loud);
    UniquePtr<Loud> b = std::move(a);
    std::printf("a is %s, b is %s\n", a ? "set" : "null", b ? "set" : "null");
}   // ~Loud runs once, when b dies
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a is null, b is set
  ~Loud
```

The two `static_assert`s are the point: `[[no_unique_address]]` (C++20) does what the empty-base trick did in C++11, so the stateless-deleter `UniquePtr` is exactly pointer-sized.

</details>

---

## 10. Real-world example

| Where | Pattern |
|---|---|
| **Everywhere sensible** | `std::unique_ptr` for owned heap objects, pimpl (`unique_ptr<Impl>`), polymorphic members, factories returning `unique_ptr<Base>` (convertible to `shared_ptr` if a caller needs sharing) |
| **LLVM** | Almost no `shared_ptr`. `unique_ptr`, arenas, intrusive reference counts (`IntrusiveRefCntPtr`: count lives *in* the object, 8-byte pointer, no control block) |
| **Chromium** | `std::unique_ptr` and `scoped_refptr` (intrusive, non-atomic for single-sequence classes) |
| **Qt** | Parent–child ownership (`QObject` deletes its children) instead of smart pointers for GUI trees; `QSharedPointer`/`QScopedPointer` exist; **implicit sharing** (`QString`, `QList`) is copy-on-write reference counting hidden inside value types (Chapter 47). `QPointer` is a `weak_ptr`-like guard for `QObject`s. |
| **Async callbacks** | `shared_ptr` captured in a lambda (or `weak_ptr` with `lock()`) to keep a session alive until its handler runs — one of the few *legitimate* spots for shared ownership |
| **Caches, graphs, observer lists** | `shared_ptr` for values, `weak_ptr` for registrations; or integer handles into a table |
| **C interop** | `unique_ptr<T, Deleter>` around `FILE*`, `sqlite3*`, `SSL*`, `GLuint`-like handles; `out_ptr` for the creator function |
| **Plugins / shared libraries** | A smart pointer's deleter must be defined where the object was allocated (the library that owns the heap), otherwise it frees across an allocator boundary; `shared_ptr` handles this naturally because the deleter is captured at creation |

> **Opinion: `shared_ptr` is overused.** It is reached for because it makes the compiler stop complaining about lifetimes — which is exactly the problem: *the lifetime question was never answered, only hidden.* Costs, all demonstrated above: 16-byte handles and a control block per object; an atomic RMW per copy that **scales badly across cores**; non-local destruction (destructors run on whatever thread drops the last reference, at an unpredictable time, possibly while holding a lock); cycles that leak; and an API that tells callers nothing about intent. Defaults, in order:
> 1. A value (member, local, container element). 2. `unique_ptr` for heap or polymorphic ownership. 3. A non-owning `T&`/`T*`/`span` for access. 4. An arena or index/handle for graphs. 5. `shared_ptr` only when several owners truly outlive one another *unpredictably* (async operations, caches, plugin lifetimes) — and then `make_shared`, pass by `const&` when not storing, and `weak_ptr` for back-edges.
>
> And never return `shared_ptr` from a factory "in case the caller needs it": return `unique_ptr`, which converts to `shared_ptr` for free when someone does.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Two `shared_ptr`s built from one raw pointer | Double delete | Copy the `shared_ptr`; `make_shared`; `enable_shared_from_this` |
| `shared_from_this()` in a constructor or on a non-shared object | `bad_weak_ptr` exception | Two-phase init, or a factory returning `shared_ptr` |
| Cycles of `shared_ptr` | Leak (no destructor runs) | Make one direction `weak_ptr`/raw/handle; redesign ownership |
| `f(std::shared_ptr<T> p)` by value for a function that only reads `*p` | Atomic RMW per call; couples callee to ownership | `f(const T&)` |
| `f(unique_ptr<T>&)` | Unclear intent: borrow? reseat? | `T&` (borrow) or `unique_ptr<T>` by value (take) or `unique_ptr<T>&` only if it will *reseat* it (document) |
| Using the raw pointer after `release()`/moving from a `unique_ptr` | Leak or null deref | Treat moved-from as null; `release()` only when handing to a C API |
| Aliasing a `shared_ptr` to a member, then modifying the parent's layout | Dangling interior pointer | Ensure the member's lifetime ≤ the parent's; use aliasing only on stable subobjects |
| `weak_ptr::expired()` followed by `lock()` or use | TOCTOU race | One `lock()`, test the result |
| Data race on a single `shared_ptr` object (assign vs. read) | UB (torn pointer/control pair) | `std::atomic<std::shared_ptr<T>>` (C++20) or a mutex |
| Polymorphic delete through `unique_ptr<Base>` with a non-virtual destructor | UB (derived part not destroyed) | Virtual destructor, or capture the correct deleter (`shared_ptr` does, `unique_ptr` does not) |
| `make_shared` with a huge object and long-lived `weak_ptr`s | Memory pinned long after destruction | `shared_ptr<T>(new T)` for that case |
| Deleter that throws or runs in a different heap than the allocation | UB / crash across a DLL/plugin boundary | `noexcept` deleters; allocate and free in the same module |
| Mixing owning and non-owning raw pointers in one struct | Nobody knows who frees | Make ownership a type (`unique_ptr`) and keep raw pointers as borrows only |
| `unique_ptr<T[]>` vs `unique_ptr<T>` mismatch | `delete` vs `delete[]` UB | Prefer `std::vector`/`std::array`; `make_unique<T[]>(n)` |
| Expecting `unique_ptr` to be as free as `T*` across ABI boundaries | Slightly worse code on hot calls | Pass `T&`; return `unique_ptr` only for transfers |

---

## 12. Exercises

1. **Signatures.** For each of: a function that (a) inspects an object, (b) stores it for later, (c) replaces a caller-owned pointer with a new one, (d) takes ownership and discards it, (e) may or may not take ownership, write the best parameter type and justify it with the table in §3.
2. **Layout.** Print the addresses of the managed object and of the control block for `make_shared<int>` and `shared_ptr<int>(new int)`; find the offset between them; check against the `_Sp_counted_ptr_inplace` layout in `<bits/shared_ptr_base.h>`.
3. **Cycle detector.** Build a tiny graph of `shared_ptr` nodes, create a cycle, and find it with `-fsanitize=leak`. Then fix it three ways (`weak_ptr`, raw back-pointer, indices into a `vector`) and compare the code.
4. **Single-thread illusion.** Reproduce Experiment 3 with `taskset -c 0` and with `-fno-threadsafe-statics`-like variations (hint: `__libc_single_threaded`); quantify the penalty of the first thread.
5. **Intrusive pointer.** Write `IntrusivePtr<T>` where `T` holds its own count (via CRTP). Measure: size, copy cost, allocation count, thread-contention scaling vs `shared_ptr`.
6. **Atomic `shared_ptr`.** Implement a lock-free-ish "read-mostly config" with `std::atomic<std::shared_ptr<const Config>>` (readers `load`, writer `store` a new snapshot). Check `is_lock_free()` on libstdc++ 14 and explain the answer.
7. **`unique_ptr` and the vtable-less deleter.** Show that `unique_ptr<Base>` constructed from `new Derived` with a non-virtual `~Base` is UB, then fix it with (a) a virtual destructor and (b) a deleter type-erased at construction. Compare with `shared_ptr<Base>(new Derived)`.
8. **C wrapper.** Wrap a real C library handle (`sqlite3`, `curl`, or `zlib`'s `z_stream`) in `unique_ptr` with a stateless deleter and `out_ptr`; verify `sizeof` equals one pointer.

---

## 13. Challenge: an observer registry without cycles or dangling

Implement `EventBus<Event>`: listeners register callbacks; listeners may be destroyed at any time, from any thread; publishing may happen concurrently with registration and destruction. Requirements:

- registration returns an RAII `Subscription` that unsubscribes in its destructor;
- the bus never keeps a listener alive (no shared ownership of the listener);
- a callback is never invoked after its `Subscription` was destroyed *and* the destructor has returned (explain the race and how you close it);
- no lock is held while user callbacks run;
- compare three designs: `vector<weak_ptr<Listener>>`, a `shared_ptr<Subscription-token>` captured by the registry as `weak_ptr`, and generation-counted integer handles. Measure publish throughput with 1/4/16 threads; defend your final choice.

---

## 14. Knowledge check

1. Why was `auto_ptr` removed and what language feature made `unique_ptr` possible?
2. What exactly does "`shared_ptr` is thread-safe" promise, and what does it *not* promise?
3. Where does a `make_shared<T>` object live relative to its control block? What is the downside?
4. Why is `sizeof(unique_ptr<T>)` equal to `sizeof(T*)` with the default deleter but 16 with a function-pointer deleter?
5. What does the aliasing constructor do, and when is it useful?
6. How does a cycle of `shared_ptr` leak, and why does a destructor never run?
7. Why does a `unique_ptr` argument not fit in a register even though it is pointer-sized?
8. Why do microbenchmarks of `shared_ptr` copies in a single-threaded `main` under-report the real cost?
9. When is `shared_ptr` the right tool? Give two situations.
10. What does `weak_ptr::lock()` do atomically, and what is the TOCTOU bug in `if (!w.expired()) use(*w.lock())`?
11. Why should a function that merely reads the object take `const T&` instead of `const shared_ptr<T>&`?
12. How does `shared_ptr` call the right destructor for `shared_ptr<Base>(new Derived)` with a non-virtual `~Base`, and why can `unique_ptr<Base>` not?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Its copy constructor *moved* ownership, violating copy semantics and breaking containers/algorithms. Move semantics (rvalue references) allow an honest move-only `unique_ptr`.
2. The control-block operations (copy, destroy, reset on *distinct* `shared_ptr` objects sharing a block) are thread-safe. It does **not** make the pointee thread-safe, and it does not make concurrent modification of the *same* `shared_ptr` object safe (use `atomic<shared_ptr>`).
3. Fused in one allocation, adjacent. Downside: object memory is not released until all `weak_ptr`s are gone, even though the object was destroyed.
4. A stateless deleter is an empty class, optimised away (EBO/`[[no_unique_address]]`); a function pointer is real state, 8 bytes, plus padding is not needed → 16.
5. It creates a `shared_ptr` sharing ownership with one object but exposing a different pointer (typically a member or element). It lets you hand out a pointer to a part that keeps the whole alive.
6. Each object holds a strong reference to the other, so neither strong count reaches zero when the external references go away; destructors run only on reaching zero.
7. It has a non-trivial destructor, so under the Itanium C++ ABI it is passed (and returned) indirectly, by address of a caller-owned temporary.
8. libstdc++ skips atomic operations while the process has never created a thread (`__libc_single_threaded`); real multi-threaded programs pay `lock`-prefixed operations and contention.
9. Genuinely shared lifetimes: asynchronous operations/callbacks that outlive their initiator; caches/registries with weak back-references; objects shared across plugin boundaries; graph nodes with unpredictable owners.
10. It checks the strong count is non-zero and increments it as one atomic step, yielding a `shared_ptr` or null. In the quoted code the object can die between `expired()` and `lock()`, and `*w.lock()` dereferences null — call `lock()` once and test it.
11. It decouples the callee from the caller's lifetime strategy (works with stack objects, `unique_ptr`, containers) and avoids refcount traffic and null questions.
12. The deleter captured at construction (`default_delete<Derived>`) is stored in the control block, type-erased. `unique_ptr<Base>`'s deleter type is `default_delete<Base>`, which calls `delete (Base*)` — UB without a virtual destructor.

</details>

---

[← Previous: Chapter 24](24-dynamic-memory.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 26 — Allocators and memory resources →](26-allocators-and-memory-resources.md)
