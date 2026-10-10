# Chapter 24 — Dynamic Memory: `new`, `delete`, and the Allocation Functions

> **Part X · Memory** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 2](../part-02-object-model-and-lifetime/02-object-model.md), [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md), [Chapter 22](../part-09-error-handling/22-exceptions.md) &nbsp;|&nbsp; **Standards:** C++98 core, C++11 `noexcept` deallocation, **C++14 sized deallocation**, **C++17 aligned allocation**, C++20 `destroying delete`, C++23 `std::start_lifetime_as` &nbsp;|&nbsp; **Tools:** `g++-14`

[← Previous: Chapter 23](../part-09-error-handling/23-expected.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 25 — Smart pointers →](25-smart-pointers.md)

---

**In one sentence:** `new` is two separate things (get raw storage from an *allocation function*, then construct an object in it), `delete` is the mirror image, and every feature in this chapter (placement, class-specific operators, alignment, sized delete) is a consequence of that split.

**By the end of this chapter you can:**

- say exactly which functions a `new`/`delete` expression calls, in what order, and what happens when construction throws
- replace the global allocation functions and class-specific ones, and know which overload set the compiler will select
- explain array cookies, sized deallocation and over-aligned allocation, and where `malloc` fits underneath
- read the allocator's footprint (`malloc_usable_size`) and know what "allocation" costs on glibc
- predict when a compiler is *allowed to remove* an allocation

---

## 1. Problem

Automatic and static storage are decided at compile time. Many objects have a lifetime and a size that are only known at run time: a parsed document, a connection table, a growable buffer. They need **storage obtained on demand and returned on demand**, in an order unrelated to scope.

C already had this: `malloc`/`free`. They deal in raw bytes. C++ objects are not raw bytes (Chapter 2): a `std::string` needs its constructor run before it is an object, and its destructor run before its storage is released. So C++ needs a mechanism that **(a) obtains storage, (b) starts the object's lifetime, and symmetrically (c) ends the lifetime, (d) releases storage** — and it needs to let programs *intercept (a) and (d)* to control where memory comes from, without changing how objects are constructed.

---

## 2. Historical context

| Era | What happened |
|---|---|
| C | `malloc`/`calloc`/`realloc`/`free`; storage only |
| 1980s, Cfront | `new T` becomes "call `operator new(sizeof(T))`, then run the constructor"; `operator new` can be overloaded per class and globally |
| C++98 | `operator new[]`; placement new; `std::nothrow`; `new_handler`; the rule that a throwing constructor releases the storage via the *matching* `operator delete` |
| C++11 | Deallocation functions are `noexcept` by default; `std::allocator` and allocator-aware containers formalised (Chapter 26) |
| C++14 | **Sized deallocation** (`operator delete(void*, size_t)`) — allocators can skip a size lookup |
| C++17 | **Aligned allocation** (`operator new(size_t, std::align_val_t)`) — `new` finally respects `alignas(64)`; `std::launder`; allocation-elision wording cleaned up |
| C++20 | **Destroying delete** (`operator delete(T*, std::destroying_delete_t)`), `std::construct_at`/`destroy_at` (Chapter 3) |
| C++23 | `std::start_lifetime_as` (implicit-lifetime types from raw bytes), `std::allocate_at_least` (allocator reports the real size obtained) |
| C++26 | Mostly library polish; `new` itself is unchanged |

---

## 3. Modern solution

Modern C++ keeps `new`/`delete` expressions as the **primitive** and tells you not to write them in application code. The shape of the modern answer:

```text
   application code:    std::make_unique<T>(...), containers, std::pmr   ← no raw new/delete
   library code:        allocator-aware containers, custom memory_resources
   the primitive:       new-expression  =  operator new(size[, align])  +  construct
                        delete-expression = destroy                      +  operator delete(ptr[, size][, align])
   below that:          malloc / mmap / brk / your arena                 ← the allocator
```

This chapter is the primitive layer. Chapters 25 and 26 build the ownership and allocator layers on top.

---

## 4. Mental model

### A `new`-expression is *two* operations

```text
   p = new T(args);                       delete p;

   ┌───────────────────────────┐          ┌───────────────────────────┐
   │ 1. void* raw =            │          │ 1. p->~T();               │
   │      operator new(sizeof T)│         │ 2. operator delete(p,     │
   │ 2. try { ::new(raw) T(args)}│        │        sizeof(T));        │
   │    catch(...) {            │          └───────────────────────────┘
   │      operator delete(raw); │          (null pointer: nothing happens)
   │      throw; }              │
   └───────────────────────────┘
```

Step 1 only *obtains storage*; no object exists until step 2 completes. This is exactly the "storage vs lifetime" split of Chapter 3.

### Three layers that are routinely confused

| Name | What it is |
|---|---|
| **`new` expression / `delete` expression** | Language constructs. Not replaceable. They call the functions below, then run constructors/destructors. |
| **`operator new` / `operator delete`** | *Allocation / deallocation functions*. Ordinary functions with special names. **Replaceable** globally and overloadable per class. They deal in raw bytes. |
| **`malloc` / `free` (and friends)** | The C library's heap. The default `operator new` is *typically* a thin wrapper over `malloc`, but the standard does not say so. |

### Where the bytes actually come from (Linux, glibc)

```text
   operator new ──► malloc ──► small request: bins inside the heap (brk / per-thread arenas)
                          └──► large request (≥ M_MMAP_THRESHOLD, default 128 KiB, dynamic): mmap
                                      │
                                      └─► kernel hands out zero-filled virtual pages; physical pages
                                          are committed on first touch (page faults)
```

`malloc` is a general-purpose allocator: it must serve any size, any lifetime, any thread. That generality is what Chapter 26 sets out to avoid when the pattern is known.

---

## 5. Language rules

### 5.1 The new-expression  `[expr.new]`

For `new T(args)`:

1. Select an allocation function by lookup of `operator new` in `T`'s class scope first (if `T` is a class), otherwise in the global scope. **A class-specific `operator new`, if any, hides the global ones** for that class.
2. Call it with `sizeof(T)` (plus the array cookie for `new T[n]`, below). If `T` is over-aligned (`alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`, 16 on x86-64 GCC) an `std::align_val_t` argument is passed (C++17).
3. If allocation fails: the throwing forms throw `std::bad_alloc` (after giving the installed `new_handler` a chance to free memory and retrying); the `std::nothrow` forms return `nullptr`.
4. Construct the object. **If the constructor throws, the storage is released with the *matching* deallocation function** (Experiment 2) and the exception continues to propagate.
5. The value of the expression is a pointer to the new object (a `prvalue` of type `T*`).

For `new T[n]`: the implementation may allocate **more** than `n * sizeof(T)` to remember `n` (the *array cookie*) when `T` has a non-trivial destructor so that `delete[]` knows how many destructors to run (Itanium ABI: an 8-byte cookie before the first element, only if the type needs it, Experiment 1). Elements are constructed in order; if element *k* throws, elements *k-1 … 0* are destroyed in reverse, then storage is released.

### 5.2 The delete-expression  `[expr.delete]`

`delete p`: if `p` is null, nothing happens. Otherwise the object's destructor runs (via the **dynamic type** if the destructor is virtual and `p` points to a base), then the deallocation function is called. `delete` on a pointer from `new[]`, `delete[]` on a pointer from `new`, deleting twice, or deleting a base pointer without a virtual destructor are all **undefined behaviour** (Chapter 28).

### 5.3 Replaceable allocation functions  `[new.delete]`

The global functions are *replaceable*: a program may define them once (in one TU, not `inline`, not `static`) and the linker uses yours program-wide, including inside the standard library:

```cpp
void* operator new(std::size_t);                       // throws bad_alloc
void* operator new(std::size_t, std::align_val_t);     // C++17
void* operator new(std::size_t, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t);   /* + aligned, nothrow */
void  operator delete(void*) noexcept;
void  operator delete(void*, std::size_t) noexcept;                  // sized (C++14)
void  operator delete(void*, std::align_val_t) noexcept;             // aligned (C++17)
void  operator delete(void*, std::size_t, std::align_val_t) noexcept;
/* + [] and nothrow variants */
```

Rules worth remembering:

- Defaults call each other (`new[]` → `new`, nothrow → throwing, sized `delete` → unsized `delete`), so replacing **only** `operator new(size_t)` and `operator delete(void*)` is usually enough and consistent. Replacing one half without the other is a bug.
- Returned storage must be suitably aligned for any object of that size up to `__STDCPP_DEFAULT_NEW_ALIGNMENT__`.
- Allocation of 0 bytes must return a distinct non-null pointer.
- A replacement must not, itself, call `new` recursively without care (it will find itself).

### 5.4 Class-specific and placement forms

- `static void* T::operator new(std::size_t)` and `static void T::operator delete(void*)` are implicitly `static`. They apply to `T` and, via inheritance, to derived classes (hence the `size` parameter: the derived size arrives).
- **Placement new** `::new (ptr) T(args)` calls `operator new(size_t, void*)`, the non-replaceable standard function that returns `ptr`. It allocates nothing; it only starts an object's lifetime at `ptr`.
- Any other extra arguments, `new (arena, tag) T`, select a user-provided overload `operator new(size_t, Arena&, Tag)`. If the constructor throws, the compiler calls the **matching placement `operator delete(void*, Arena&, Tag)`** if one exists; if none exists, **no deallocation function is called** (a silent leak — Experiment 2).
- **Destroying delete** (C++20): `void operator delete(T*, std::destroying_delete_t)` takes over destruction and deallocation entirely, e.g. for classes with a custom layout or a flexible array member. Rare; know it exists.

### 5.5 Alignment

`alignof(std::max_align_t)` is 16 on x86-64 Linux. Plain `operator new` guarantees that alignment (`__STDCPP_DEFAULT_NEW_ALIGNMENT__`). A type with stricter alignment (`alignas(64)`) makes the compiler choose the **aligned overload** automatically since C++17. Before C++17 `new alignas(64) T` silently produced a 16-byte-aligned object — a classic source of crashes on `movaps`/AVX loads.

### 5.6 The allocation may be elided  `[expr.new]/10`

> An implementation is allowed to omit a call to a replaceable global allocation function. When it does, the storage is instead provided by the implementation or provided by extending the allocation of another `new`-expression. (C++14 wording, unchanged in later standards.)

Consequences: a `new` whose pointer never escapes may become a stack slot or vanish; two `new`s may be merged; **counting allocations with a replaced `operator new` is therefore unreliable at `-O2`** (Experiment 4) and `new int` followed by `delete` may produce *no* call at all.

### 5.7 Failure handling

The `std::new_handler` (set with `std::set_new_handler`) is called repeatedly on failure until it frees memory, installs a different handler, throws, or terminates. Without a handler, the throwing forms throw `bad_alloc`. On Linux with default overcommit, an ordinary `malloc` rarely *fails*: the kernel promises virtual memory it may not have, and the process is killed by the OOM killer later when it touches the pages. `bad_alloc` therefore mostly occurs for absurd sizes or when `ulimit -v`/cgroup limits apply (Experiment 5). Most application code cannot meaningfully recover from `bad_alloc` — see the Opinion in §10.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What are the sequences of calls and the guarantees? | `new` = allocate + construct, with matching deallocation on constructor failure; replaceable functions; alignment rules; elision permission |
| **Compiler / library** | What does GCC/libstdc++ do? | Default `operator new` calls `malloc` and loops over the `new_handler`; `operator delete` calls `free`; sized `delete` forwards to unsized; may elide allocations at `-O1`+ |
| **ABI (Itanium)** | Array cookies, mangled names | Cookie = one `size_t` before the array, present only when the element type has a non-trivial destructor (or the class has a usual deallocation function taking a size); `_Znwm` = `operator new(unsigned long)` |
| **OS (Linux)** | Where do pages come from? | `brk`/`mmap`; page-granular; demand-zero pages; overcommit |
| **CPU** | What does allocation cost? | A function call plus pointer chasing in the allocator's bins; the first touch of a fresh page costs a page fault (~µs) |

---

## 6. Implementation model

### glibc `malloc` in one paragraph

glibc (ptmalloc) keeps memory in **chunks** with a header (8 bytes of size/flags that overlap the previous chunk's tail when it is in use). Requests are rounded to a multiple of 16 bytes with a 24-byte *usable* minimum (the next chunk's `prev_size` field is borrowed, Experiment 3). Freed small chunks go to per-thread **tcache** bins (a lock-free LIFO of same-size chunks, up to 7 by default), then **fastbins** and **smallbins**/**unsorted bin** under the arena lock. Large requests are carved from the top chunk or served by `mmap` and returned to the kernel on free. Consequences you can measure: allocating and freeing the same size repeatedly is fast (tcache hit, tens of nanoseconds); memory is rarely returned to the OS promptly; fragmentation is real; each live allocation costs ≥ 16 bytes of overhead plus rounding.

### What `new` costs in instructions

For `new T` with an inline-able constructor, GCC emits `mov edi, sizeof(T); call _Znwm` followed by the constructor body inlined; `_Znwm` does `malloc` and a null check. The `delete` side is `call _ZdlPvm` (sized, since C++14 in `-std=c++14` and later with GCC's `-fsized-deallocation` on by default). Experiment 4 shows both disappearing.

### Why sized delete exists

A general-purpose allocator must find a chunk's size from the pointer (read a header). If the compiler passes the size it already knows (`sizeof(T)`), size-class allocators (tcmalloc, jemalloc, mimalloc) can skip the lookup. glibc ignores the argument.

---

## 7. Experiments

### Experiment 1 ✅: Instrumenting the global allocation functions

All output goes through `write(2)` so that ordering is exact (we are *inside* `operator new`, where `printf` buffering and re-entrancy would mislead).

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#include <cstdarg>
static void say(const char* fmt, ...) {
    char buf[160]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    (void)!write(1, buf, n);
}

void* operator new(std::size_t n)           { void* p = std::malloc(n); say("  [operator new(%zu)]\n", n); return p; }
void* operator new[](std::size_t n)         { void* p = std::malloc(n); say("  [operator new[](%zu)]\n", n); return p; }
void  operator delete(void* p) noexcept                      { say("  [operator delete]\n"); std::free(p); }
void  operator delete(void* p, std::size_t n) noexcept       { say("  [operator delete(sized %zu)]\n", n); std::free(p); }
void  operator delete[](void* p) noexcept                    { say("  [operator delete[]]\n"); std::free(p); }
void  operator delete[](void* p, std::size_t n) noexcept     { say("  [operator delete[](sized %zu)]\n", n); std::free(p); }

struct Plain   { int x; };
struct Tracked { int x; Tracked() { say("  Tracked()\n"); } ~Tracked() { say("  ~Tracked()\n"); } };

int main() {
    say("new Plain / delete:\n");          { Plain* p = new Plain; delete p; }
    say("new Tracked / delete:\n");        { Tracked* p = new Tracked; delete p; }
    say("new int[3] / delete[] (trivial element, no cookie):\n");   { int* p = new int[3]; delete[] p; }
    say("new Plain[3] / delete[]:\n");     { Plain* p = new Plain[3]; delete[] p; }
    say("new Tracked[3] / delete[] (non-trivial dtor => cookie):\n");
    { Tracked* p = new Tracked[3]; delete[] p; }
    say("sizeof(Tracked)=%zu, so 3 elements = %zu bytes; the request above was larger by the cookie\n", sizeof(Tracked), 3 * sizeof(Tracked));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
new Plain / delete:
  [operator new(4)]
  [operator delete(sized 4)]
new Tracked / delete:
  [operator new(4)]
  Tracked()
  ~Tracked()
  [operator delete(sized 4)]
new int[3] / delete[] (trivial element, no cookie):
  [operator new[](12)]
  [operator delete[]]
new Plain[3] / delete[]:
  [operator new[](12)]
  [operator delete[]]
new Tracked[3] / delete[] (non-trivial dtor => cookie):
  [operator new[](20)]
  Tracked()
  Tracked()
  Tracked()
  ~Tracked()
  ~Tracked()
  ~Tracked()
  [operator delete[](sized 20)]
sizeof(Tracked)=4, so 3 elements = 12 bytes; the request above was larger by the cookie
```

Things to read off:

- `new Tracked` → `operator new(4)` **then** `Tracked()`; `delete` → `~Tracked()` **then** the sized `operator delete`. Two operations in each direction.
- The compiler calls the **sized** `operator delete` automatically (`-fsized-deallocation` is the default in C++14+ on GCC).
- Trivially destructible arrays need **no cookie**; the array of `Tracked` requests 8 extra bytes, which is how `delete[]` knows to run three destructors. The cookie is invisible to you, but `delete` (not `delete[]`) on that pointer would hand `free` an address 8 bytes past the real block: that is why mismatching them is undefined behaviour, not a harmless leak.

### Experiment 2 ✅: Constructor failure, placement forms, and the matching delete

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

struct Arena {                                    // a toy bump arena
    alignas(16) unsigned char buf[256]; std::size_t used = 0;
    void* take(std::size_t n) { n = (n + 15) & ~std::size_t(15); void* p = buf + used; used += n; return p; }
    void  give_back(void*, std::size_t) { std::puts("    [arena: give_back called (storage returned)]"); }
};

struct Widget {
    explicit Widget(bool fail) { std::puts("    Widget ctor"); if (fail) throw std::runtime_error("ctor failed"); }
    ~Widget() { std::puts("    Widget dtor"); }

    // class-specific forms: hide the global ones for Widget
    static void* operator new(std::size_t n)               { std::printf("    Widget::operator new(%zu)\n", n); return std::malloc(n); }
    static void  operator delete(void* p)                  { std::puts("    Widget::operator delete"); std::free(p); }

    // placement form taking an arena...
    static void* operator new(std::size_t n, Arena& a)     { std::printf("    Widget::operator new(%zu, Arena&)\n", n); return a.take(n); }
    // ...and its MATCHING placement delete: called only if the constructor throws
    static void  operator delete(void* p, Arena& a)        { a.give_back(p, 0); }
};

struct NoMatch {                                  // same, but WITHOUT a matching placement delete
    explicit NoMatch(bool fail) { if (fail) throw std::runtime_error("ctor failed"); }
    static void* operator new(std::size_t n, Arena& a)     { return a.take(n); }
    // (no operator delete(void*, Arena&) here)
};

int main() {
    std::puts("1. class-specific new/delete, successful:");
    { Widget* w = new Widget(false); delete w; }

    std::puts("2. class-specific new, constructor throws -> ordinary operator delete(void*) is called:");
    try { Widget* w = new Widget(true); (void)w; } catch (const std::exception& e) { std::printf("    caught: %s\n", e.what()); }

    Arena a;
    std::puts("3. placement-with-arena, constructor throws -> MATCHING operator delete(void*, Arena&) is called:");
    try { Widget* w = new (a) Widget(true); (void)w; } catch (const std::exception& e) { std::printf("    caught: %s\n", e.what()); }

    std::puts("4. placement-with-arena, but NO matching delete -> nothing is called (the arena just keeps the bytes):");
    try { NoMatch* n = new (a) NoMatch(true); (void)n; } catch (const std::exception& e) { std::printf("    caught: %s (arena used %zu bytes)\n", e.what(), a.used); }

    std::puts("5. plain placement new on a buffer you manage: construct and destroy by hand:");
    alignas(Widget) unsigned char raw[sizeof(Widget)];
    Widget* w = ::new (raw) Widget(false);          // global placement: no allocation, only construction
    w->~Widget();                                   // explicit destructor; the buffer is just bytes again
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1. class-specific new/delete, successful:
    Widget::operator new(1)
    Widget ctor
    Widget dtor
    Widget::operator delete
2. class-specific new, constructor throws -> ordinary operator delete(void*) is called:
    Widget::operator new(1)
    Widget ctor
    Widget::operator delete
    caught: ctor failed
3. placement-with-arena, constructor throws -> MATCHING operator delete(void*, Arena&) is called:
    Widget::operator new(1, Arena&)
    Widget ctor
    [arena: give_back called (storage returned)]
    caught: ctor failed
4. placement-with-arena, but NO matching delete -> nothing is called (the arena just keeps the bytes):
    caught: ctor failed (arena used 32 bytes)
5. plain placement new on a buffer you manage: construct and destroy by hand:
    Widget ctor
    Widget dtor
```

Case 2 shows the rule in §5.1 step 4: the compiler wraps the construction in a hidden `try/catch` and calls the deallocation function that *matches* the allocation function it used. Case 3 shows that the match includes the extra parameters. Case 4 is the trap: with no matching placement delete **the compiler silently does nothing**, which for a real allocator (not a bump arena) is a leak on every constructor failure. GCC does not warn by default; `-Wmismatched-new-delete` and static analysis catch some forms. **Rule: every placement-form `operator new` you write gets a matching `operator delete`.**

### Experiment 3 🔧: What the allocator really gives you (glibc)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <malloc.h>

int main() {
    std::puts("request -> usable bytes (glibc malloc_usable_size)");
    for (std::size_t n : std::initializer_list<std::size_t>{1, 8, 24, 25, 40, 41, 100, 1000}) {
        void* p = std::malloc(n);
        std::printf("  malloc(%4zu) -> %4zu usable\n", n, malloc_usable_size(p));
        std::free(p);
    }
    // alignment of consecutive small allocations
    void* a = std::malloc(1); void* b = std::malloc(1); void* c = std::malloc(1);
    std::printf("addresses: %p %p %p  (deltas %td, %td; each address %% 16 = %zu, %zu, %zu)\n", a, b, c,
                (char*)b - (char*)a, (char*)c - (char*)b,
                (std::size_t)a % 16, (std::size_t)b % 16, (std::size_t)c % 16);
    std::free(a); std::free(b); std::free(c);

    // freed chunk is reused immediately (tcache LIFO)
    void* x = std::malloc(40); std::free(x); void* y = std::malloc(40);
    std::printf("free then malloc(40) of the same size returns the same block: %s\n", x == y ? "yes" : "no");
    std::free(y);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
request -> usable bytes (glibc malloc_usable_size)
  malloc(   1) ->   24 usable
  malloc(   8) ->   24 usable
  malloc(  24) ->   24 usable
  malloc(  25) ->   40 usable
  malloc(  40) ->   40 usable
  malloc(  41) ->   56 usable
  malloc( 100) ->  104 usable
  malloc(1000) -> 1000 usable
addresses: 0x55fee7d9e2b0 0x55fee7d9e7a0 0x55fee7d9e7c0  (deltas 1264, 32; each address % 16 = 0, 0, 0)
free then malloc(40) of the same size returns the same block: yes
```

Take-aways: the *useful* size is rounded up to 24, 40, 56, … (8 mod 16): glibc steals the next chunk's size word. A million `new char` costs about 32 MB, not 1 MB, since each takes a 32-byte chunk. And immediate reuse of a just-freed block is what makes use-after-free bugs "work" in testing (Chapter 28) — and why AddressSanitizer deliberately quarantines freed memory.

### Experiment 4 🔧: The compiler may remove the allocation

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=sum_to,leaks_to_caller
int sum_to(int n) {
    int* p = new int(n);          // never escapes
    int r = *p + 1;
    delete p;                     // and is freed here
    return r;
}
int* leaks_to_caller(int n) {
    return new int(n);            // escapes: allocation is observable
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
sum_to(int):
	lea	eax, 1[rdi]
	ret

leaks_to_caller(int):
	push	rbx
	mov	ebx, edi
	mov	edi, 4
	call	operator new(unsigned long)@PLT
	mov	DWORD PTR [rax], ebx
	pop	rbx
	ret
```

`sum_to` compiles to `lea eax, [rdi+1]; ret`: no `_Znwm`, no `_ZdlPvm`, no memory at all. `leaks_to_caller` must call the allocator. The standard permits the first (§5.6). Compilers apply it only when they can see the whole lifetime and the allocation function is the *default replaceable one*: if you replace `operator new`, GCC and Clang still elide in these simple cases, so **never count allocations with a replaced `operator new` at `-O2` and call it a measurement**. Use a profiler, `ltrace`, `heaptrack` or the allocator's own statistics.

### Experiment 5 ✅: Failure: `bad_alloc`, `nothrow`, and `new_handler`

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <new>

static int handler_calls = 0;
static void my_handler() {
    ++handler_calls;
    std::printf("  new_handler called (%d); giving up by throwing bad_alloc\n", handler_calls);
    throw std::bad_alloc();
}

int main() {
    const std::size_t absurd = std::size_t(1) << 60;       // far beyond any address space the kernel will grant

    std::puts("throwing new:");
    try { char* p = new char[absurd]; (void)p; }
    catch (const std::bad_alloc& e) { std::printf("  caught std::bad_alloc: %s\n", e.what()); }

    std::puts("nothrow new:");
    char* q = new (std::nothrow) char[absurd];
    std::printf("  returned %s\n", q ? "non-null (!)" : "nullptr");

    std::puts("with a new_handler installed:");
    std::set_new_handler(my_handler);
    try { char* p = new char[absurd]; (void)p; } catch (const std::bad_alloc&) { std::puts("  caught bad_alloc"); }
    std::set_new_handler(nullptr);

    std::puts("array length overflow (n * sizeof(T) wraps):");
    try { std::size_t n = std::size_t(-1) / 2; long* p = new long[n]; (void)p; }
    catch (const std::bad_array_new_length& e) { std::printf("  caught std::bad_array_new_length: %s\n", e.what()); }
    catch (const std::bad_alloc& e) { std::printf("  caught std::bad_alloc: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
throwing new:
  caught std::bad_alloc: std::bad_alloc
nothrow new:
  returned nullptr
with a new_handler installed:
  new_handler called (1); giving up by throwing bad_alloc
  caught bad_alloc
array length overflow (n * sizeof(T) wraps):
  caught std::bad_array_new_length: std::bad_array_new_length
```

### Experiment 6 🔧: Over-aligned types and the aligned overloads

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <new>

static bool saw_aligned = false;
void* operator new(std::size_t n, std::align_val_t a) {
    saw_aligned = true;
    std::printf("  [aligned operator new(size=%zu, align=%zu)]\n", n, static_cast<std::size_t>(a));
    void* p = std::aligned_alloc(static_cast<std::size_t>(a), (n + static_cast<std::size_t>(a) - 1) / static_cast<std::size_t>(a) * static_cast<std::size_t>(a));
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

struct alignas(64) CacheLine { char bytes[64]; };
struct Normal { char c[64]; };

int main() {
    std::printf("__STDCPP_DEFAULT_NEW_ALIGNMENT__ = %zu\n", (std::size_t)__STDCPP_DEFAULT_NEW_ALIGNMENT__);
    Normal*    n = new Normal;                 // default overload
    CacheLine* c = new CacheLine;              // aligned overload chosen by the compiler
    std::printf("new Normal    -> %% 64 = %zu\n", (std::uintptr_t)n % 64);
    std::printf("new CacheLine -> %% 64 = %zu  (aligned overload used: %s)\n", (std::uintptr_t)c % 64, saw_aligned ? "yes" : "no");
    delete n; delete c;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
__STDCPP_DEFAULT_NEW_ALIGNMENT__ = 16
  [aligned operator new(size=64, align=64)]
new Normal    -> % 64 = 0
new CacheLine -> % 64 = 0  (aligned overload used: yes)
```

---

## 8. Assembly / runtime investigation

Three quick checks you can do on any `new`-using binary:

```bash
# (1) Which allocation functions does this object file actually call?   (mangled names)
nm -C --undefined-only prog.o | grep -E "operator (new|delete)"
#   _Znwm   = operator new(unsigned long)          _ZdlPvm  = operator delete(void*, unsigned long)  (sized)
#   _Znam   = operator new[](unsigned long)        _ZdaPv   = operator delete[](void*)
#   _ZnwmSt11align_val_t = aligned operator new    _ZnwmRKSt9nothrow_t = nothrow

# (2) See every allocation a program makes (glibc), with sizes and call sites
ltrace -e malloc+free ./prog 2>&1 | head
valgrind --tool=massif ./prog && ms_print massif.out.*        # heap profile over time

# (3) Is a given 'new' elided?  grep the optimised assembly
g++-14 -std=c++23 -O2 -S -o - prog.cpp | c++filt | grep -c "operator new"
```

Also try `strace -e trace=brk,mmap,munmap ./prog` on a program that allocates progressively larger blocks to see the `brk`→`mmap` switch at the mmap threshold (128 KiB initially, adaptive afterwards).

---

## 9. Implementation exercise

Write a **tracking allocator layer** you could drop into any program:

1. Replace `operator new/delete` (all 12 forms: plain, `[]`, aligned, nothrow, sized) so that every allocation is prefixed with a 16-byte header `{size, magic}`, aligned correctly.
2. `delete` verifies the magic (detects double delete and `delete` vs `delete[]` mismatch with a second magic), then poisons the block (`0xDD`) before freeing.
3. Maintain thread-safe counters: live bytes, peak bytes, total allocations. Dump them from a `static` destructor.
4. Add a **leak report**: an intrusive list of live blocks with a captured `__builtin_return_address(0)`.
5. Verify with one test per failure: leak, double delete, mismatched delete, overflow into the header.
6. Measure the overhead against plain `malloc` with a microbenchmark and compare with ASan.

<details>
<summary><strong>Solution sketch: the header trick and the alignment pitfall</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <new>

namespace track {
struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) Header { std::size_t size; std::size_t magic; };
static_assert(sizeof(Header) == __STDCPP_DEFAULT_NEW_ALIGNMENT__, "header must preserve the user pointer's alignment");
inline long live = 0, total = 0;
inline constexpr std::size_t kLive = 0xA110CA7EDULL, kDead = 0xDEADBEEFULL;

inline void* alloc(std::size_t n) {
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + n));
    if (!h) throw std::bad_alloc();
    h->size = n; h->magic = kLive; ++live; ++total;
    return h + 1;                                   // user pointer: just past the header, still 16-aligned
}
inline void dealloc(void* p) noexcept {
    if (!p) return;
    auto* h = static_cast<Header*>(p) - 1;
    if (h->magic != kLive) { std::puts("track: bad or double delete"); std::abort(); }
    h->magic = kDead; --live;
    std::free(h);
}
}  // namespace track

void* operator new(std::size_t n)   { return track::alloc(n); }
void* operator new[](std::size_t n) { return track::alloc(n); }
void operator delete(void* p) noexcept                { track::dealloc(p); }
void operator delete(void* p, std::size_t) noexcept   { track::dealloc(p); }
void operator delete[](void* p) noexcept              { track::dealloc(p); }
void operator delete[](void* p, std::size_t) noexcept { track::dealloc(p); }

int main() {
    int* a = new int(1); int* b = new int[10]; delete a; delete[] b;
    int* leak = new int(2); (void)leak;
    std::printf("total allocations %ld, still live %ld (the leak)\n", track::total, track::live);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
total allocations 3, still live 1 (the leak)
```

The pitfall: a 16-byte header preserves the 16-byte alignment guarantee; an 8-byte header would hand out misaligned storage that works on x86 until a `movaps` or `long double` shows up. The aligned (`std::align_val_t`) overloads need different logic, since the header must be padded to the requested alignment — one reason hand-rolled tracking allocators are usually replaced by `-fsanitize=address` or `heaptrack` in production.

</details>

---

## 10. Real-world example

| Where | How the primitives are used |
|---|---|
| **libstdc++/libc++** | `std::allocator<T>::allocate` calls `::operator new` (aligned if needed); `vector`, `string`, `map` nodes all route through it. Replacing global `operator new` therefore redirects *all* standard containers. |
| **tcmalloc / jemalloc / mimalloc** | Preloaded, or linked, they replace `malloc`; some also replace `operator new` to use sized deallocation (a reason sized `delete` exists). Typical wins: 10–30 % on allocation-heavy servers. |
| **Game engines, audio code** | Per-frame arenas and pools; `operator new` overloads taking an arena (`new (frameArena) Particle`) with matching placement deletes. |
| **LLVM** | Bump-pointer `BumpPtrAllocator` and `new (Allocator) Node` everywhere; objects are never individually deleted. |
| **Qt** | `QObject` allocation is plain `new`; ownership is by parent (Chapter 47); `QArrayData` uses `malloc` with its own header for implicit sharing; Qt overloads `operator new` only in a few internal classes. |
| **Embedded** | Replace `operator new` with a fixed-pool allocator or `= delete` it to forbid the heap entirely (a link-time guarantee of "no dynamic allocation"). |
| **Debugging** | ASan/LSan/Valgrind interpose on `malloc` *and* `operator new` to track every block; this is why replacing `operator new` in a program can silently disable some of their checks. |

> **Opinion.** In application code, **raw `new`/`delete` should be rare enough to stand out in review**. Use `std::make_unique`, containers, or an arena. Where you *do* write them (library internals, containers, arenas), obey the rules in this chapter without exception: matching placement deletes, correct alignment, `noexcept` deallocation. Do not replace global `operator new` to "count allocations" (it lies at `-O2`) or to "speed up" an application before you have profiled: the malloc you replace it with is usually the more important decision (try `LD_PRELOAD=libtcmalloc.so` first: zero code). And do not write elaborate `bad_alloc` recovery code for ordinary applications: on Linux with overcommit the failure you can actually catch is nearly always a bug (an absurd size), and the OOM killer will arrive before `bad_alloc` for the failure you cannot. Reserve `bad_alloc` handling for systems that run with strict overcommit, cgroup limits, or arenas with hard caps.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `delete` on `new[]` memory (or vice versa) | UB: heap corruption, `free(): invalid pointer` (cookie mismatch) | Never write `new[]`; use `std::vector`/`std::unique_ptr<T[]>` |
| Placement `operator new` without a matching `operator delete` | Silent leak whenever the constructor throws | Always provide the matching deallocation function |
| Deleting through a base pointer with a non-virtual destructor | UB; derived part never destroyed | Virtual destructor, or a protected non-virtual one in non-polymorphic bases (Chapter 19) |
| Replacing only one of `new`/`delete` | Mismatch with `free`/`malloc`; ASan "alloc-dealloc-mismatch" | Replace the pair consistently |
| Custom `operator new` ignoring alignment | Crashes on SIMD loads, `long double`, `alignas(64)` types | Honour `__STDCPP_DEFAULT_NEW_ALIGNMENT__`; implement the aligned overloads |
| `operator new` that throws/recurses through `new` inside itself | Infinite recursion, startup crash | Use `malloc` or static storage inside the replacement; avoid `printf`/iostreams that allocate |
| Throwing from `operator delete` or a destructor during delete | `std::terminate` (deallocation is `noexcept`) | Don't |
| Counting allocations via a replaced `operator new` at `-O2` | Under-count; benchmark "proves" an optimisation that does nothing | Use real profilers; compile with `-fno-builtin-malloc`-style flags only knowingly |
| Using memory after `delete` | "Works" because tcache hands it back unmodified | ASan; `-fsanitize=address` in CI |
| Using `malloc` for non-trivial types | Object never constructed: UB | `new`, or `malloc` + placement new + explicit destroy |
| Assuming `new` returns zeroed memory | Garbage in `new int[100]` (use `new int[100]()`) | Value-initialise explicitly, or use containers |
| Allocation in a signal handler or realtime thread | Deadlock on the allocator lock; priority inversion | Pre-allocate; arenas (Chapter 26) |

---

## 12. Exercises

1. **Trace.** For `auto* p = new Derived(args);` where `Derived` has a class-specific `operator new` and a virtual base, list in order every function called, including on constructor failure at each subobject.
2. **Cookies.** Using Experiment 1's technique, find which element types do and do not get an array cookie (trivial, trivially destructible with non-trivial constructor, non-trivial destructor, class with its own sized `operator delete[]`). Compare with the Itanium ABI rule.
3. **Mismatch.** Write a program that does `delete` on a `new[]`'d `Tracked[3]`. Run under plain glibc, then ASan. What do you see in each? Explain using the cookie.
4. **Alignment.** Allocate an `alignas(64)` struct with GCC `-std=c++14` and `-std=c++17`; compare addresses over 1000 allocations; explain the difference.
5. **Elision.** Find the smallest function in which GCC and Clang *do* and *do not* remove a `new`/`delete` pair. What blocks elision (escape, opaque calls, `-fno-builtin`, replaced `operator new`)?
6. **Mini-allocator.** Replace global `operator new/delete` with a size-class free-list allocator for sizes ≤ 256 bytes (one free list per 16-byte class, falling back to `malloc`). Benchmark against glibc with `std::list<int>` build/destroy loops. Where is it faster and why is it thread-unsafe?
7. **Destroying delete.** Implement a class with a flexible trailing buffer allocated as one block, using `operator delete(T*, std::destroying_delete_t)` so `delete p` frees the whole block correctly. Compare with a factory returning `unique_ptr` with a custom deleter.
8. **Overcommit.** On your machine, allocate and *touch* progressively larger blocks while watching `/proc/self/status` (`VmSize` vs `VmRSS`) and `/proc/sys/vm/overcommit_memory`. What happens at each stage?

---

## 13. Challenge: a leak-and-corruption detector

Build `memcheck.hpp`, a single-header replacement of the global allocation functions that: tags each block with size, thread id and a captured call stack (`backtrace()`); places *guard bands* (red zones, filled with a pattern) before and after the user block and verifies them on `delete`; quarantines freed blocks for N deallocations and checks for writes to freed memory; detects mismatched `new`/`delete[]`; and prints a deduplicated leak report at exit sorted by bytes. Use it on a deliberately buggy program, then compare what it finds against ASan and Valgrind (what does a library-level detector miss that a compiler-instrumented one catches, and what is the performance difference?).

---

## 14. Knowledge check

1. List the two steps of `new T(args)` and the two of `delete p`.
2. A constructor throws inside `new T`. What is called, and with which arguments?
3. What makes a `new`-expression use `operator new(size_t, std::align_val_t)`?
4. What is an array cookie, who writes it, and why can you not mix `new[]` with `delete`?
5. Why was sized `operator delete` added, and does glibc use the size?
6. Does the standard guarantee that `operator new` calls `malloc`? What does that imply for replacing `malloc` vs replacing `operator new`?
7. Under what condition may a compiler remove a `new`/`delete` pair? Name a consequence for benchmarking.
8. What happens on Linux when you `new char[1ull << 60]`? When you `new char[10ull << 30]` on a 4 GiB machine with default overcommit?
9. What is the difference between `::new (p) T` and `new T`?
10. What goes wrong if your placement `operator new` has no matching `operator delete`?
11. Why must deallocation functions be `noexcept`?
12. Why does `malloc(1)` use 32 bytes of address space on glibc?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `new`: call an allocation function for storage, then construct the object in it. `delete`: run the destructor, then call a deallocation function.
2. The compiler-generated cleanup calls the deallocation function matching the allocation function used, passing the pointer returned by allocation (and the same extra arguments for a placement form). If none matches, nothing is called.
3. The allocated type's alignment exceeds `__STDCPP_DEFAULT_NEW_ALIGNMENT__` (C++17), so the compiler selects the aligned overload.
4. A hidden element count stored before the array (Itanium ABI: 8 bytes) when elements need destruction; `new[]` writes it and `delete[]` reads it. A plain `delete` passes the user pointer, but the real block starts 8 bytes earlier, and only one destructor runs: corruption.
5. So size-class allocators can skip the size lookup from the pointer. glibc ignores it.
6. No. Replacing `malloc` affects `new` only if the default `operator new` calls `malloc` (which libstdc++ does), and also affects C code and libraries; replacing `operator new` affects only C++ allocations.
7. When the allocation is a replaceable global allocation function and the implementation can provide the storage otherwise (e.g. the pointer does not escape). Benchmarks of "allocation count" or "alloc/free loop" may measure nothing.
8. The first fails immediately (`bad_alloc`); the second likely *succeeds* (virtual memory is overcommitted) and fails or is OOM-killed only when pages are touched.
9. `::new (p) T` is the placement form: it calls the non-replaceable `operator new(size_t, void*)`, allocates nothing and constructs at `p`. `new T` obtains storage through the (replaceable) allocation function.
10. When the constructor throws, nothing releases the storage: a leak (or an arena that never gets the space back).
11. They are called during cleanup paths (constructor failure, `delete`), including while unwinding; throwing there would terminate or lose the original error. They are `noexcept` by default since C++11.
12. A 16-byte-aligned chunk with an 8-byte header; usable payload is 24 bytes, and `malloc(1)` still consumes a minimum chunk of 32 bytes.

</details>

---

[← Previous: Chapter 23](../part-09-error-handling/23-expected.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 25 — Smart pointers →](25-smart-pointers.md)
