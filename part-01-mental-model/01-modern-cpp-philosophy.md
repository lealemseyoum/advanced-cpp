# Chapter 1 — The Modern C++ Philosophy

> **Part I · Mental model** &nbsp;|&nbsp; **Level 1** (advanced usage) &nbsp;|&nbsp; **≈ 3 hours**
> **Prerequisites:** none &nbsp;|&nbsp; **Standards:** C++11 → C++26 &nbsp;|&nbsp; **Tools:** `g++`, `objdump`

[← Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 2 — The object model →](../part-02-object-model-and-lifetime/02-object-model.md)

---

**In one sentence:** modern C++ is a language for building abstractions that cost nothing *at runtime* by moving the cost somewhere else, into compile time, into the type system, and into the rules you must learn.

**By the end of this chapter you can:**

- state precisely what "zero-cost abstraction" promises and what it does not
- explain the *as-if rule* and why it is the foundation of every optimization in this course
- read C++ history as a sequence of problems rather than a list of features
- check an abstraction's cost yourself, with `sizeof` and assembly

---

## 1. Problem

Systems programmers face an old trade-off.

**C** gives you control. You know what every line costs and you decide where every byte lives. But C has almost no way to express *meaning*. A `char*` could be a string, a buffer, an array of bytes, or an owned allocation that someone must free. The compiler cannot tell the difference, so neither can the next programmer, and neither can a tool.

**Higher-level languages** give you abstraction. But the price is usually paid at runtime: a garbage collector, boxed values, virtual dispatch everywhere, a runtime that must be linked in.

The ambition behind C++ is to refuse this trade:

> *What you don't use, you don't pay for. And further: what you do use, you couldn't hand-code any better.*
> — Bjarne Stroustrup, the **zero-overhead principle**

That is a demanding specification. It means a `std::vector<int>` should be as fast as a hand-written dynamic array. A `std::unique_ptr` should be as cheap as a raw pointer. A user-defined `Meters` type should be as cheap as a `double` while refusing to be added to `Seconds`.

How is any of that possible? That is what this course is about.

---

## 2. Historical context

Don't memorize feature lists. Read the history as *problems found in the previous standard, then fixed*.

| Standard | The problem it attacked | The big answers |
|---|---|---|
| **C** | No abstraction, manual everything | (the baseline) |
| **C++98/03** | Resource leaks; no reusable data structures; macros for generics | Destructors (RAII), classes, **templates**, the **STL** (containers + iterators + algorithms), exceptions |
| **C++11** | Value semantics were too expensive (copy everything). Ownership was invisible. Threads were outside the language. | **Move semantics**, `unique_ptr`/`shared_ptr`, variadic templates, lambdas, `auto`, `constexpr`, **threads and the memory model**, `noexcept` |
| **C++14** | Rough edges in C++11's features | Generic lambdas, relaxed `constexpr`, return-type deduction, `make_unique` |
| **C++17** | Ceremony and missing vocabulary types | **Guaranteed copy elision**, structured bindings, `if constexpr`, fold expressions, CTAD, `optional`/`variant`/`any`/`string_view`, `filesystem`, `pmr`, parallel algorithms |
| **C++20** | Templates were unchecked. Iterator pairs were clumsy. Async code was inside-out. Headers were slow. | **Concepts**, **ranges**, **coroutines**, **modules**, `consteval`, `span`, `jthread`, `format`, `<=>` |
| **C++23** | Finish the C++20 story; fill vocabulary gaps | `std::expected`, `std::print`, deducing `this`, `mdspan`, `flat_map`, `generator`, `import std`, more range views |
| **C++26** | No compile-time introspection. No safety net for common bugs. No standard async model. | **Static reflection**, **contracts**, `std::execution`, erroneous behaviour for uninitialized reads, hardened standard library |

The same story told as a dependency chain:

```text
C++98   destructors ──► RAII ──► containers that own their elements
           │
           └─ templates ──► STL: algorithms work on any container

C++11   moves make value semantics cheap
           └─► unique_ptr: ownership in the type system
           └─► containers can hold move-only types
           └─► threads + atomics: a memory model for the abstract machine

C++17   guaranteed elision: returning by value is *free*, for prvalues
           └─► factory functions need no pointers

C++20   concepts: templates get checked interfaces
           └─► ranges: algorithms get composable, constrained pipelines
           └─► coroutines: suspend/resume as a language primitive

C++26   reflection: the compiler's knowledge becomes available to the program
```

> [!NOTE]
> **C++26 status.** WG21 finished the technical work in March 2026. Compiler support is uneven: GCC 16 is furthest ahead on reflection and contracts, while senders/receivers (`std::execution`) were not yet in the major standard libraries when this course was written. Each chapter that touches C++26 says what you can run today.

---

## 3. Modern solution: six ideas

Modern C++ is not "C++ plus a pile of features". It is a small set of ideas that the features serve.

| # | Idea | What it means | The mechanisms |
|:-:|---|---|---|
| 1 | **Value semantics** | Objects behave like `int`: copy them and you get an independent object | copy/move constructors, regular types, `std::vector` |
| 2 | **Deterministic destruction (RAII)** | Resources are released at a known point, by a destructor, even on exceptions | destructors, `unique_ptr`, `lock_guard`, scope guards |
| 3 | **Explicit ownership** | The type tells you who must free the thing | `unique_ptr`, `shared_ptr`, `span`, `string_view`, references |
| 4 | **Static typing and generics** | Mistakes are caught by the compiler, and code is reused without runtime dispatch | templates, concepts, overloads |
| 5 | **Compile-time computation** | Work that *can* be done before the program runs, *is* | `constexpr`, `consteval`, templates, reflection |
| 6 | **Abstraction without mandatory overhead** | You choose whether to pay, and the cost is visible | inlining, as-if rule, `noexcept`, allocators |

Ideas 1–3 are about **correctness**: eliminating whole classes of bugs by construction. Ideas 4–5 are about **expressiveness and speed**. Idea 6 is the one that holds the bargain together.

You will see the same shape over and over:

```text
RAII ─► value semantics ─► move semantics ─► smart pointers ─► containers ─► exception safety
                                                                    │
                                                  every one of these assumes the one before it
```

```text
templates ─► generic programming ─► type traits ─► concepts ─► ranges ─► compile-time programming
```

```text
threads ─► memory model ─► atomics ─► synchronization ─► lock-free programming
```

```text
coroutines ─► suspension ─► promise objects ─► awaiters ─► scheduler ─► async I/O
```

If a chapter ever feels like a list of unrelated rules, step back and ask: *which of the six ideas is this serving?*

---

## 4. Mental model: where does the cost go?

"Zero-cost" is the most misunderstood phrase in C++. Here is the correct model.

> [!IMPORTANT]
> **Zero-cost means zero *runtime* overhead *compared with the hand-written equivalent*, in an optimized build.** It does not mean free. It means the cost moved.

An abstraction has three budgets:

```text
                ┌─────────────────────────────────────────┐
  Runtime  ◄────┤  time, memory, indirections, allocations │  ← what "zero-cost" protects
                ├─────────────────────────────────────────┤
  Build    ◄────┤  compile time, binary size, link time    │  ← where the cost often moves to
                ├─────────────────────────────────────────┤
  Brain    ◄────┤  rules, lifetime hazards, diagnostics    │  ← where the cost *also* moves to
                └─────────────────────────────────────────┘
```

Every abstraction in this course will be judged against all three. A `std::unique_ptr` costs nothing at runtime, a little at compile time, and a lot less in bugs. A `std::regex` costs a great deal in every column. A `std::function` costs a call through a pointer, possibly an allocation, and defeats inlining; but sometimes the flexibility is worth exactly that.

Here is a first ledger. You will build it yourself in the experiments below.

| Abstraction | Runtime cost (optimized) | Notes |
|---|---|---|
| `struct Meters { double v; }` | **none** | Same registers, same instructions as `double` |
| `std::unique_ptr<T>` | **none** on the hot path | Same size as `T*`; adds a cold exception cleanup path |
| `std::accumulate`, range-`for`, `views::transform` | **none** | The compiler inlines the whole pipeline into one loop |
| Template + lambda | **none** | Fully inlinable: the callee's type is known |
| `std::function` | indirect call, maybe an allocation | The price of erasing the callable's type |
| `std::shared_ptr` copy | a reference-count update (atomic when multithreaded) | An *atomic* operation is a very different cost from a plain add |
| virtual call | indirect call, blocks inlining (unless devirtualized) | See [Chapter 19](../part-08-polymorphism/19-runtime-polymorphism.md) |
| exceptions | none on the non-throwing path; large on the throwing path | See [Chapter 22](../part-09-error-handling/22-exceptions.md) |
| `-O0` anything | **large** | None of this holds without optimization |

That last row is crucial. Zero-cost abstractions are a promise **made by the optimizer**, not by the language.

---

## 5. Language rules

### 5.1 The abstract machine and the as-if rule  `[intro.abstract]`

The standard does not define what your program *compiles to*. It defines the behaviour of an **abstract machine**, and then says:

> A conforming implementation may do anything it likes, as long as the **observable behaviour** is the same as the abstract machine would produce.

This is the **as-if rule**. Observable behaviour is deliberately narrow:

- accesses to `volatile` objects, which must follow the abstract machine's rules
- data written to files, which must be identical at program termination
- input and output on interactive devices, whose order must be preserved

(Programs with several threads add the synchronization guarantees of the memory model; see [Chapter 30](../part-12-concurrency/30-memory-model.md).)

Everything else is fair game: the compiler may delete variables, merge functions, reorder statements, unroll loops, replace a heap allocation by a register, or compute the entire result at compile time.

```text
 what you write           what the standard requires       what you get
 ───────────────          ─────────────────────────        ────────────────
 an abstraction     ─►    only observable behaviour   ─►   whatever the optimizer
 (class, template,        must match                        can prove is equivalent
  lambda, range)
```

> [!WARNING]
> **The standard promises no performance.** Not even for `std::vector`. It specifies *complexity* (for example, amortized constant `push_back`) but never a single machine instruction. "Zero-overhead" is a **quality-of-implementation** property that every good compiler delivers. Which means it can be missing, and you can check.

### 5.2 Exceptions to as-if

Two notable transformations are *allowed even though they can change observable behaviour*:

| Transformation | Standard rule | Effect |
|---|---|---|
| **Copy/move elision** in specific contexts | `[class.copy.elision]` | Constructors with side effects may be skipped (mandatory since C++17 for prvalues) |
| **Allocation elision** | `[expr.new]` | A call to the allocation function may be omitted, or several merged, when the memory is never observably used |

### 5.3 Undefined behaviour is an optimizer input

If a program has undefined behaviour, the standard places **no requirements** on it. Compilers use the *absence* of UB as a fact: if `x + 1 > x` for a signed `x` can only fail through overflow (UB), the optimizer may assume it is always true.

This is why so many of the abstractions below can be removed safely, *provided you obey the rules*. It is also why breaking a rule has consequences far from the broken line. [Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md) is devoted to it.

### 5.4 Layer check

| Layer | What it contributes to "zero-cost" |
|---|---|
| **Standard** | The as-if rule; guaranteed copy elision; complexity requirements |
| **Compiler** | Inlining, scalar replacement of aggregates, constant propagation, dead-code elimination |
| **ABI** | A `struct { double v; }` is passed in the same register as a `double` (System V AMD64: class SSE) |
| **OS** | Nothing at all: abstractions disappear before the kernel is involved |
| **CPU** | A well-predicted direct call or an inlined body is cheap; an unpredictable indirect call is not |

---

## 6. Implementation model: how abstractions disappear

Two compiler passes do most of the work of making abstractions free:

```text
 source ──► front end ──► IR ──► ┌────────────────────────────────┐ ──► back end ──► machine code
            parse,                │ 1. INLINING                    │     instruction
            overload resolution,  │    replace call with body      │     selection,
            template              │ 2. SROA / mem2reg              │     register
            instantiation         │    break structs into scalars, │     allocation
                                  │    keep them in registers      │
                                  │ 3. constant folding, DCE, CSE  │
                                  └────────────────────────────────┘
                                          the "middle end"
```

- **Inlining** turns `a.operator+(b)` or `std::get<0>(t)` into the code inside it. Once inlined, the function boundary no longer exists.
- **Scalar replacement of aggregates (SROA)** notices that a `struct Meters { double v; }` never needs to exist in memory and rewrites it as a lone `double` in a register.

After those two passes, a one-member wrapper and the raw type are *the same IR*, and therefore the same machine code. Nothing special happens "because it is a zero-cost abstraction"; the abstraction is simply unrecognizable to the later passes.

This also tells you when the bargain **fails**: when the optimizer cannot see through. That happens when

- the callee is behind a pointer the compiler cannot resolve (virtual call, `std::function`, function pointer),
- the definition is not visible (separate translation unit, no LTO),
- optimization is off,
- the object *escapes* (its address is stored somewhere the compiler cannot track).

Keep this list. You will use it in nearly every later chapter.

---

## 7. Experiments

All experiments use `g++-14` on x86-64 Linux. Run them; then run them again with `-O0`, and again with `clang++`.

### Experiment 1 — The cost ledger: `sizeof`

How much *space* do the standard abstractions occupy? This is the first thing to check, since size is part of cost (cache lines, move costs, ABI).

```cpp
// @test run -std=c++23 -O2
#include <any>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// variadic, because a template-id such as std::variant<int, double> contains a comma
#define SHOW(...) std::printf("%-28s %2zu bytes\n", #__VA_ARGS__, sizeof(__VA_ARGS__))

int main() {
    SHOW(int*);
    SHOW(std::unique_ptr<int>);
    SHOW(std::shared_ptr<int>);
    SHOW(std::span<int>);
    SHOW(std::string_view);
    SHOW(std::vector<int>);
    SHOW(std::string);
    SHOW(std::optional<int>);
    SHOW(std::variant<int, double>);
    SHOW(std::any);
    SHOW(std::function<void()>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
int*                          8 bytes
std::unique_ptr<int>          8 bytes
std::shared_ptr<int>         16 bytes
std::span<int>               16 bytes
std::string_view             16 bytes
std::vector<int>             24 bytes
std::string                  32 bytes
std::optional<int>            8 bytes
std::variant<int, double>    16 bytes
std::any                     16 bytes
std::function<void()>        32 bytes
```

Read the table:

- `unique_ptr<int>` is **exactly** a pointer. That is the zero-overhead principle in one number (the default deleter is an empty class, and empty members take no space here).
- `shared_ptr` is two pointers: the object and a *control block*. Sharing isn't free in space either.
- `span` and `string_view` are a pointer and a length. They own nothing: [Chapter 12](../part-05-standard-library/12-views-and-non-owning-types.md) is about the lifetime responsibility this creates.
- `std::string` is 32 bytes in libstdc++ because it carries a *small string buffer*: strings up to 15 characters never allocate. That is a space-for-time trade made on your behalf.
- `std::function` is 32 bytes: room for a small callable stored inline, plus pointers to the manager and invoker functions.

> [!NOTE]
> **Layer: library implementation, not standard.** Every number above is a property of **libstdc++ on x86-64**. The standard fixes none of them. Try `-stdlib=libc++` and compare (Exercise 1).

### Experiment 2 — A wrapper type is free, but only when optimized

```cpp
// @test asm -std=c++23 -O0 -fno-stack-protector filter=add_
struct Meters { double v; };
double add_raw(double a, double b) { return a + b; }
Meters add_strong(Meters a, Meters b) { return {a.v + b.v}; }
```

```asm
; asm (gcc 14.2.0, -O0, x86-64, Intel syntax)
add_raw(double, double):
	push	rbp
	mov	rbp, rsp
	movsd	QWORD PTR -8[rbp], xmm0
	movsd	QWORD PTR -16[rbp], xmm1
	movsd	xmm0, QWORD PTR -8[rbp]
	addsd	xmm0, QWORD PTR -16[rbp]
	pop	rbp
	ret

add_strong(Meters, Meters):
	push	rbp
	mov	rbp, rsp
	movsd	QWORD PTR -8[rbp], xmm0
	movsd	QWORD PTR -16[rbp], xmm1
	movsd	xmm1, QWORD PTR -8[rbp]
	movsd	xmm0, QWORD PTR -16[rbp]
	addsd	xmm0, xmm1
	pop	rbp
	ret
```

At `-O0` there are redundant stack spills (and the two functions even differ in operand order). **Nothing has been optimized**, so the abstraction is not yet erased. Now the same functions at `-O2`:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=add_
struct Meters { double v; };
double add_raw(double a, double b) { return a + b; }
Meters add_strong(Meters a, Meters b) { return {a.v + b.v}; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
add_raw(double, double):
	addsd	xmm0, xmm1
	ret

add_strong(Meters, Meters):
	addsd	xmm0, xmm1
	ret
```

Identical. One `addsd`, one `ret`. The wrapper type vanished. And notice **why this works at the ABI level**: the System V ABI classifies a struct containing one `double` as class SSE, so it is passed in `xmm0`, exactly like a plain `double`. Had `Meters` had a user-provided destructor, the ABI would have forced it into memory (see [Chapter 37](../part-14-compilation-and-linking/37-abi.md)).

### Experiment 3 — Loops, algorithms and ranges

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=sum_
#include <numeric>
#include <ranges>
#include <span>

long sum_loop(const int* p, long n) {
    long s = 0;
    for (long i = 0; i < n; ++i) s += p[i];
    return s;
}

long sum_accumulate(std::span<const int> v) {
    return std::accumulate(v.begin(), v.end(), 0L);
}

long sum_ranges(std::span<const int> v) {
    long s = 0;
    for (int x : v | std::views::transform([](int a) { return a; })) s += x;
    return s;
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
sum_loop(int const*, long):
	test	rsi, rsi
	jle	.L4
	lea	rcx, [rdi+rsi*4]
	xor	eax, eax
.L3:
	movsx	rdx, DWORD PTR [rdi]
	add	rdi, 4
	add	rax, rdx
	cmp	rdi, rcx
	jne	.L3
	ret
.L4:
	xor	eax, eax
	ret

sum_accumulate(std::span<int const, 18446744073709551615ul>):
	lea	rcx, [rdi+rsi*4]
	xor	eax, eax
	cmp	rcx, rdi
	je	.L10
.L9:
	movsx	rdx, DWORD PTR [rdi]
	add	rdi, 4
	add	rax, rdx
	cmp	rdi, rcx
	jne	.L9
	ret
.L10:
	ret

sum_ranges(std::span<int const, 18446744073709551615ul>):
	lea	rcx, [rdi+rsi*4]
	xor	eax, eax
	cmp	rcx, rdi
	je	.L15
.L14:
	movsx	rdx, DWORD PTR [rdi]
	add	rdi, 4
	add	rax, rdx
	cmp	rdi, rcx
	jne	.L14
	ret
.L15:
	ret
```

Three different spellings (a raw loop, an iterator-pair algorithm, and a lazy range adaptor pipeline) compile to the same loop: load, extend, add, advance, compare, branch. The abstraction *layers* (span → iterators → transform_view → iterator wrapper → lambda) were all inlined and then SROA'd into registers.

### Experiment 4 — `unique_ptr` vs raw pointer

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=with_
#include <memory>

void use(int*);

void with_unique() {
    auto p = std::make_unique<int>(5);
    use(p.get());
}

void with_raw() {
    int* p = new int(5);
    use(p);
    delete p;
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
with_unique():
	push	r14
	mov	edi, 4
	push	rbx
	sub	rsp, 8
	call	operator new(unsigned long)@PLT
	mov	DWORD PTR [rax], 5
	mov	rdi, rax
	mov	rbx, rax
	call	use(int*)@PLT
	add	rsp, 8
	mov	rdi, rbx
	mov	esi, 4
	pop	rbx
	pop	r14
	jmp	operator delete(void*, unsigned long)@PLT
.L3:
	mov	r14, rax
	jmp	.L2

with_unique() [clone .cold]:
.L2:
	mov	rdi, rbx
	mov	esi, 4
	call	operator delete(void*, unsigned long)@PLT
	mov	rdi, r14
	call	_Unwind_Resume@PLT

with_raw():
	push	rbx
	mov	edi, 4
	call	operator new(unsigned long)@PLT
	mov	DWORD PTR [rax], 5
	mov	rbx, rax
	mov	rdi, rax
	call	use(int*)@PLT
	mov	rdi, rbx
	mov	esi, 4
	pop	rbx
	jmp	operator delete(void*, unsigned long)@PLT
```

The hot paths are identical: `operator new`, store 5, call `use`, `operator delete`. Now look at what `with_unique` has *besides* that: a `[clone .cold]` section containing a second `operator delete` followed by `_Unwind_Resume`. That is an **exception landing pad**. If `use` throws, the stack unwinder jumps there, the `unique_ptr` destructor frees the memory, and unwinding continues. `with_raw` has no such path, because it simply **leaks** when `use` throws. The compiler parked the cleanup in a separate cold section so that it does not dilute the instruction cache on the normal path.

> [!TIP]
> This is the whole philosophy in a single diff. The abstraction is zero-cost on the path you exercise, adds cold code on the path you rarely do, and is *more correct* than the hand-written version.

### Experiment 5 — Template callable vs `std::function`

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=call_
#include <functional>

long call_function(const std::function<long(long)>& f, long x) { return f(x); }

template <class F>
long call_template(F&& f, long x) { return f(x); }

long call_template_site(long x) {
    return call_template([](long y) { return y * 3; }, x);
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
call_function(std::function<long (long)> const&, long):
	sub	rsp, 24
	cmp	QWORD PTR 16[rdi], 0
	mov	QWORD PTR 8[rsp], rsi
	je	.L3
	lea	rsi, 8[rsp]
	call	[QWORD PTR 24[rdi]]
	add	rsp, 24
	ret

call_function(std::function<long (long)> const&, long) [clone .cold]:
.L3:
	call	std::__throw_bad_function_call()@PLT

call_template_site(long):
	lea	rax, [rdi+rdi*2]
	ret
```

`call_template_site` collapsed to a single `lea` (`x + 2x`). The lambda's type is part of the template's instantiation, so the compiler knows exactly which function runs and inlines it. `call_function` cannot: it checks whether the function is empty (`cmp ..., 0` then `je` to the `bad_function_call` path) and then calls *through a pointer*. The type was **erased**, and with it the compiler's knowledge. That's the price; the benefit is that a single compiled function can accept any callable ([Chapter 21](../part-08-polymorphism/21-type-erasure.md)).

### Experiment 6 — The compiler deletes your allocation

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=heap_free
int heap_free() {
    int* p = new int(42);
    int v = *p;
    delete p;
    return v;
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
heap_free():
	mov	eax, 42
	ret
```

No call to `operator new`. This is one of the exceptions to as-if in 5.2: the standard explicitly permits eliding allocations whose storage is unobservable. Don't *rely* on it (it is an optimization, not a guarantee), but remember it when you read benchmark results ([Chapter 40](../part-16-performance/40-benchmarking.md)): the compiler may have deleted the work you are trying to time.

---

## 8. Assembly / runtime investigation: the shared-pointer copy

Every previous experiment ended in "no cost". Here is one where the cost is real, and you can see exactly what it is.

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=copy_sp
#include <memory>

std::shared_ptr<int> copy_sp(const std::shared_ptr<int>& a) { return a; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
copy_sp(std::shared_ptr<int> const&):
	movdqu	xmm0, XMMWORD PTR [rsi]
	mov	rax, rdi
	movhlps	xmm1, xmm0
	movups	XMMWORD PTR [rdi], xmm0
	movq	rdx, xmm1
	test	rdx, rdx
	je	.L1
	cmp	BYTE PTR __libc_single_threaded[rip], 0
	je	.L3
	add	DWORD PTR 8[rdx], 1
	ret
.L3:
	lock add	DWORD PTR 8[rdx], 1
.L1:
	ret
```

Copying a `shared_ptr` must increment a reference count in the control block. Look at what the compiler emitted: a test of `__libc_single_threaded`, then either a plain `add` (single-threaded process) or a `lock add` (an *atomic* read-modify-write) when more than one thread exists. libstdc++ chooses at run time whether to pay for atomics.

The atomic variant is not "one more instruction". It is a cache-coherence transaction that serializes the core's store buffer. When many threads copy the same `shared_ptr`, they all hit the same cache line (the control block), and the cost grows with contention. [Chapters 25 and 27](../part-10-memory/25-smart-pointers.md) return to this.

> [!NOTE]
> **Layer: library + compiler + glibc.** `__libc_single_threaded` is a glibc ≥ 2.32 variable that libstdc++ (≥ 11) consults; nothing in the C++ standard mentions it. A different implementation (libc++, MSVC STL) makes different choices.

---

## 9. Implementation exercise: strong types

**Goal:** build a zero-cost, compile-time-checked unit wrapper, then *prove* it is zero-cost.

```cpp
// @test compile -std=c++23
#include <compare>

// TODO 1: Strong<T, Tag> — holds a T, constructed explicitly, with a .value() accessor.
// TODO 2: operator+ and operator- for two Strong<T, Tag> of the SAME Tag.
// TODO 3: operator* by a plain scalar T (scaling), and operator<=> / operator== (defaulted).
// TODO 4: static_assert that sizeof(Strong<double, struct MetersTag>) == sizeof(double)
//         and that Strong<double, MetersTag> is trivially copyable.
// TODO 5: a static_assert (or a requires-expression) proving that Meters + Seconds does NOT compile.

template <class T, class Tag>
class Strong {
    T value_;

public:
    constexpr explicit Strong(T v) : value_(v) {}
    constexpr T value() const { return value_; }
    // ... your operators here ...
};

struct MetersTag;
struct SecondsTag;
using Meters  = Strong<double, MetersTag>;
using Seconds = Strong<double, SecondsTag>;

int main() {}
```

Then compare `add_raw(double, double)` against your `add_strong(Meters, Meters)` at `-O2`:

```bash
g++-14 -std=c++23 -O2 -S -masm=intel -fno-asynchronous-unwind-tables -o - strong.cpp | c++filt | grep -v '^\s*\.'
```

They must match. If they don't, find out why before moving on. (Hint: is your constructor `constexpr`? Does anything have a non-trivial destructor?)

---

## 10. Real-world example: `std::chrono`

The standard library's best zero-cost abstraction is exactly this idea, productized. `std::chrono::duration<Rep, Period>` carries its *unit* in the type and does the unit arithmetic **at compile time**.

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=ms_to_us
#include <chrono>

long ms_to_us_chrono(std::chrono::milliseconds ms) {
    return std::chrono::duration_cast<std::chrono::microseconds>(ms).count();
}

long ms_to_us_raw(long ms) { return ms * 1000; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
ms_to_us_chrono(std::chrono::duration<long, std::ratio<1l, 1000l> >):
	imul	rax, rdi, 1000
	ret

ms_to_us_raw(long):
	imul	rax, rdi, 1000
	ret
```

Both are one multiplication by 1000. The ratio `1000/1` was computed by the compiler; no division, no table, no function call. Meanwhile, `milliseconds + seconds` is legal and correct, while `milliseconds + int` is a compile error. This is the same trick as Experiment 2, scaled up to a production-quality library.

> **Verdict.** If a quantity has a unit, give it a type. It is among the cheapest bug-prevention techniques in the language, and it costs nothing at runtime.

---

## 11. Failure modes

| Mistake | Why it happens | Reality |
|---|---|---|
| **Judging abstraction cost at `-O0`** | Debug builds are what you run in the debugger | Nothing is inlined, so every wrapper is a call. Measure with `-O2` (and `-g` for symbols) |
| **Thinking "zero-cost" means "free"** | The slogan is shortened | Costs move to compile time, binary size and cognitive load. Heavy template code can add minutes to a build |
| **Assuming the optimizer will always see through** | It usually does | Virtual calls, `std::function`, separate translation units and escaping objects all block it ([§6](#6-implementation-model-how-abstractions-disappear)) |
| **Hand-optimizing instead of measuring** | It feels productive | Compilers beat hand-written "optimizations" of simple code. Spend the effort on algorithms and data layout |
| **Treating compiler behaviour as C++ behaviour** | It happens to work on your machine | The standard promises as-if behaviour, not particular instructions. A "guaranteed" optimization usually isn't |
| **Cargo-culting "modern" features** | They are newer, so they must be better | Every feature in this course has a cost model. Use it when the trade is right |

---

## 12. Exercises

1. **Space ledger, second opinion.** Run Experiment 1 with `clang++ -stdlib=libc++` (install `libc++-18-dev` first). Which sizes differ? Why might `libc++`'s `std::string` use 24 bytes while libstdc++'s uses 32? *(Hint: read the layout of each in the library source.)*
2. **Strong types.** Complete the implementation exercise in §9 and show the assembly is identical.
3. **Find the optimization level.** Compile Experiment 3 at `-O0`, `-Og`, `-O1`, `-O2`, `-O3`. At which level does `sum_accumulate` become identical to `sum_loop`? Which level first inlines the `std::views::transform` machinery? Use `objdump -dC | grep -c call` as a quick metric.
4. **Count the calls.** Write a function that pushes 1000 `int`s into a `std::vector`. How many `call` instructions are in its assembly at `-O0`? At `-O2`? What are the remaining calls for?
5. **The cost of an `#include`.** Create four files that include only `<vector>`, `<iostream>`, `<ranges>`, `<regex>`. Use `time g++ -std=c++23 -fsyntax-only` on each. Put the numbers in a table. What does this suggest about the build-time budget of an abstraction?
6. **A deliberately failing optimization.** Write a loop calling a function `f(int)` through a `std::function` and through a template parameter. Prove (with assembly) that the first prevents vectorization and the second doesn't, for a function body of your choosing.

---

## 13. Challenge: a dimensioned quantity

Build `Quantity<Dim>` where `Dim` encodes the exponents of length, mass and time as **non-type template parameters**:

```text
Quantity<L=1, M=0, T=0>   metres
Quantity<L=1, M=0, T=-1>  metres per second
```

Requirements:

- `Quantity<d1> * Quantity<d2>` yields `Quantity<d1 + d2>`; division subtracts.
- Adding quantities of different dimensions is a **compile error**, with a message that names the mismatch.
- `sizeof(Quantity<...>) == sizeof(double)`.
- A function computing kinetic energy `½·m·v²` produces assembly with exactly the multiplications you would write by hand.

Then answer: **under what circumstances would this abstraction *not* be free?** Find at least one concrete case with `-O0`, and one with `-O2`.

---

## 14. Knowledge check

Answer in your own words before opening the folds.

1. State the as-if rule. Which behaviours are *observable*?
2. Give two transformations the standard allows even though they change observable behaviour. Why are they allowed?
3. `std::unique_ptr<T>` and `T*` have the same `sizeof`. Is this guaranteed by the standard? Is it guaranteed *on your platform*? Who decides?
4. Name three situations where the optimizer cannot erase an abstraction.
5. Why does a `struct { double v; }` pass in `xmm0` on System V x86-64 but a struct with a user-provided destructor does not?
6. "C++ abstractions are zero-cost." Rewrite this sentence so it is **true**.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. A conforming implementation must produce the same *observable behaviour* as the abstract machine: volatile accesses, writes to files/interactive devices (including ordering), and the thread-synchronization guarantees. Everything else may be transformed freely.
2. Copy/move elision (it may skip constructor calls with side effects, and since C++17 is mandatory for prvalue initialization) and allocation elision (a `new` whose memory is never observably used may be removed or merged). They exist so that value-semantic code (returning objects, building temporaries) can be efficient without changing source.
3. The standard does not guarantee it. It is a property of the **library implementation**: libstdc++ and libc++ both store the deleter in a way that takes no space when it is an empty class (empty-base optimization, or `[[no_unique_address]]`). A custom deleter that holds state makes the `unique_ptr` larger.
4. Virtual calls and `std::function`/function pointers the compiler cannot resolve; callee definitions invisible to the compiler (other translation units without LTO); objects whose address escapes; optimization disabled; also recursion beyond the inliner's limits.
5. The Itanium/System V rule: a type with a non-trivial copy constructor or destructor is *non-trivial for the purposes of calls* and must be passed by invisible reference, because the object has an identity (address) that matters. A trivially copyable one-`double` struct is classified SSE and passed in a register.
6. "Well-designed C++ abstractions can compile to the same machine code as the hand-written equivalent, **when optimized and when the optimizer can see through them**; their costs move to compile time, binary size and the rules you need to know."

</details>

---

[← Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 2 — The object model →](../part-02-object-model-and-lifetime/02-object-model.md)
