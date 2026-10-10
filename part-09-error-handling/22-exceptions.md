# Chapter 22 — Exceptions

> **Part IX · Error Handling** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 7 hours**
> **Prerequisites:** [Chapter 4](../part-02-object-model-and-lifetime/04-raii.md), [Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 19](../part-08-polymorphism/19-runtime-polymorphism.md) &nbsp;|&nbsp; **Standards:** C++98 (exceptions), C++11 (`noexcept`, `exception_ptr`, nested), C++17 (`noexcept` in the type system), C++26 (`constexpr` exceptions) &nbsp;|&nbsp; **Tools:** `g++-14`, `readelf`, `objdump`, `gdb`, `perf`

[← Previous: Chapter 21](../part-08-polymorphism/21-type-erasure.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 23 — `std::expected` →](23-expected.md)

---

**In one sentence:** exceptions move error handling out of the normal path by making *the normal path pay nothing* and *the error path pay a lot*; understanding exactly what that bargain means (tables, landing pads, a two-phase unwinder, one heap-allocated object) tells you when it is the right trade and how to write code that survives being thrown through.

> **Layer legend.** ⚖️ *standard* · 🧩 *Itanium C++ ABI + the unwinder library (libgcc_eh/libunwind, libsupc++/libc++abi)* · 🔧 *compiler/optimizer*. The standard specifies *behaviour* (what is thrown, which handler is chosen, which destructors run). The "zero-cost" table-driven mechanism is an **implementation strategy** that GCC/Clang use on x86-64 Linux; MSVC x64 uses a related table scheme and 32-bit MSVC historically used a different one.

**By the end of this chapter you can:**

- trace a throw through both phases of unwinding and say what runs when
- explain "zero-cost exceptions" precisely: what is free, what is not
- read the unwind tables and landing pads in a binary
- measure the cost of throwing and of *not* throwing
- state and test the **basic / strong / no-throw** guarantees, and implement strong exception safety with copy-and-swap
- decide between exceptions, error codes, `optional` and `expected` (Chapter 23)

---

## 1. Problem

A function that can fail must report failure to its caller, and the caller is often several frames away from the code that knows what to do. The C answers:

```c
int rc = open_file(&f);   if (rc != 0) goto cleanup;          // every call, every layer
rc = read_header(f, &h);  if (rc != 0) goto cleanup;
...
cleanup: if (f) close_file(f);                                 // manual, easy to miss on one path
```

| Problem with return codes | Consequence |
|---|---|
| Every call site must check | Forgotten checks → silent failure; checks make up much of the code |
| Cleanup on every path | `goto cleanup` ladders; leaks when one path is missed |
| Constructors, operators, and conversions have **no return value** | `Foo f(args);` cannot report failure by code |
| Errors must be *translated* at each layer | Information is lost; every layer chooses its own codes |
| The failure path is as visible as the success path | Hot-path code is cluttered with error handling |

Exceptions are C++'s answer: **raise an object, unwind the stack running destructors, deliver to the nearest matching handler**. Combined with RAII (Chapter 4) the cleanup is automatic and the success path stays clean.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1960s–70s | PL/I ON-conditions, CLU and Ada exceptions; Goodenough's 1975 paper |
| 1989–90 | Koenig & Stroustrup add exceptions to C++ (ARM); the **termination model** (no resumption) is chosen after studying Mesa |
| 1992–94 | Early implementations use **setjmp/longjmp** chains: every `try` registers a handler at run time (cost on the *normal* path) |
| 1990s | Table-driven (**"zero-cost"**) implementations appear: no work on entry to a `try`; tables consulted only when something throws |
| 1997 | Tom Cargill's "Exception Handling: A False Sense of Security" and Herb Sutter / David Abrahams on **exception safety guarantees**; Abrahams formalizes *basic / strong / nothrow* |
| 1998 | C++98: `throw(...)` **dynamic exception specifications** (checked at run time, slow, rarely useful) |
| 1999–2001 | **Itanium C++ ABI** specifies the exception-handling ABI (LSDA, personality routines, `_Unwind_*`), adopted by GCC/Clang everywhere on Unix |
| 2011 | `noexcept` replaces `throw()`; `std::exception_ptr`, `nested_exception`, `current_exception`; move semantics make `noexcept` matter (Chapter 6) |
| 2017 | Dynamic exception specs **removed** (except `throw()` = `noexcept`); `noexcept` becomes part of the function type |
| 2019–24 | The "exceptions are too slow" debate: P1947, P2544 (*C++ exceptions are becoming more and more problematic*), Herbceptions (P0709 *zero-overhead deterministic exceptions*), `std::expected` (Chapter 23) |
| 2026 | C++26: `constexpr` exceptions (P3068) adopted; GCC 16 implements them |

---

## 3. Modern solution

```cpp
std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p);                                  // RAII: closes on any exit path
    if (!f) throw std::system_error(errno, std::generic_category(), p.string());    // raise
    std::string s((std::istreambuf_iterator<char>(f)), {});
    return s;                                            // no cleanup code, no error checks on the success path
}

try { auto cfg = parse(read_all("config.toml")); }
catch (const std::system_error& e) { log("io error: {}", e.what()); }
catch (const parse_error& e)       { log("syntax: {}", e.what()); }
```

Four pieces: `throw expr;` (create and raise), `try { } catch (T& e) { }` (handler), **stack unwinding** (destroy automatic objects between the throw and the handler), and `noexcept` (a promise that nothing escapes).

---

## 4. Mental model

### The throw, step by step  🧩

```text
  throw X{...};
     │
     ▼ 1. __cxa_allocate_exception(sizeof X)      ← the exception object lives on the HEAP (not on the stack being unwound);
     │                                               the thrown expression is copy/move-initialized into it
     ▼ 2. __cxa_throw(obj, typeinfo, destructor)   ← never returns
     ▼ 3. _Unwind_RaiseException                    ← the unwinder library starts
     │
     ├── PHASE 1  (search):   walk up the stack using .eh_frame, call each frame's *personality routine*,
     │                         which reads that function's LSDA table to ask "is there a matching handler here?"
     │                         NOTHING IS DESTROYED.   If no frame has one → std::terminate (the stack is NOT unwound)
     │
     └── PHASE 2  (cleanup):  walk again; for each frame up to the handler, jump to its *landing pad*
                               which runs the destructors of live automatic objects, then _Unwind_Resume to continue;
                               at the handler's frame, jump to the catch block
     ▼ 4. catch block runs; the exception object is destroyed when the handler exits (the __cxa_end_catch refcount reaches 0)
```

Two consequences to remember:

1. **The exception object is a copy on the heap.** `throw e;` copies `e` (and `throw;` rethrows *the same* object). Throwing needs memory: if allocation fails, libstdc++ falls back to an emergency pool, and ultimately `std::terminate`.
2. **Unwinding only happens if a handler is found.** If none exists, `terminate` is called *without* unwinding (⚖️ implementation-defined whether the stack is unwound in that case). That is why a destructor message may not print in a crash-on-uncaught-exception test.

### "Zero-cost": what exactly is free?

```text
                          happy path                          throwing path
   setjmp/longjmp era     pays at every try-entry              moderate
   table-driven (today)   pays NOTHING at try-entry            expensive: several µs per throw (3.4 µs at depth 1 in Experiment 4), +~1 µs per frame

   "zero cost" means: no instructions are executed for entering/leaving a try block or for the
   mere possibility of a throw. It does NOT mean "no cost at all":
     • the tables (.eh_frame, .gcc_except_table) take space in the binary and are mapped into memory (but not touched)
     • code layout: landing pads are placed out of line; can affect inlining decisions and register allocation slightly
     • the optimizer must assume any non-noexcept call can throw, which limits some reordering and forces cleanups to exist
```

### A fixed point of the design

The standard fixes the **termination model**: after a handler finishes, execution continues *after the try-catch*, never at the throw point. The unwinding destroys all automatic objects in between, which is why RAII is not optional for exception-safe code.

---

## 5. Language rules

### 5.1 Throwing and catching  `[except.throw]`, `[except.handle]` ⚖️

- `throw expr;` copy-initializes the exception object from `expr` (decayed type, `T[]`→`T*`). The type must be copy/move-constructible (guaranteed elision applies to prvalues since C++17).
- **Handlers are matched in order**, by the *static type of the handler* against the *dynamic type of the exception object*: exact type, base class (public, unambiguous), pointer conversions, `catch (...)`. First match wins, so put derived before base.
- **Catch by `const T&`.** By value *slices* and copies; by pointer needs a lifetime story; `catch (...)` cannot name the exception.
- `throw;` (no operand) rethrows the *currently handled* exception object itself (no copy, no slicing). `throw e;` inside a handler throws a *copy of `e`'s static type* and slices.
- **Function-try-blocks** `T::T() try : m_(init()) { } catch (...) { }`: handlers for a constructor's member initializers; the exception is **automatically rethrown** at the end of the handler of a constructor/destructor try block (you cannot swallow it).

### 5.2 Stack unwinding and destructors  `[except.ctor]` ⚖️

- On the way to the handler, every fully constructed automatic object, and every fully constructed subobject/member of a partially constructed object, is destroyed in reverse order of construction.
- If the exception escapes a **constructor**, the object was never fully constructed: its destructor does **not** run (members and bases already constructed are destroyed). This is the source of the RAII rule *"one resource per class"*: a class holding two raw resources leaks the first if acquiring the second throws.
- **A destructor that exits via an exception during unwinding calls `std::terminate`** (C++ allows only one in-flight exception per unwinding). Since C++11 destructors are **implicitly `noexcept`**; a throwing destructor terminates even when called normally unless declared `noexcept(false)`. `std::uncaught_exceptions()` (C++17) tells how many are in flight.

### 5.3 `noexcept`  `[except.spec]` ⚖️

- `noexcept` (≡ `noexcept(true)`) promises that no exception escapes. If one tries to, **`std::terminate` is called** (the unwinder may or may not unwind first: ⚖️ implementation-defined).
- `noexcept(expr)` is a conditional specifier; `noexcept(f())` is an *operator* returning a `constexpr bool`.
- Since C++17 `noexcept` is part of the **function type** (`void(*)() noexcept` converts to `void(*)()` but not back).
- **Implicit `noexcept`:** destructors, `operator delete`, and defaulted special members whose subobjects are all `noexcept`.
- `noexcept` is **a contract, not an optimization hint to sprinkle**: use it where the standard library queries it (move constructors/assignment, `swap`, destructors, hash functions) and where the function truly cannot throw. Marking a function that allocates `noexcept` converts `bad_alloc` into `terminate`; that is sometimes exactly what you want (see the Opinion).

### 5.4 Standard exceptions and carrying exceptions

- Hierarchy rooted at `std::exception` (`what()` is virtual; the string's lifetime is the exception's): `logic_error` (`invalid_argument`, `out_of_range`, …), `runtime_error` (`system_error`, `overflow_error`, …), `bad_alloc`, `bad_cast`, `bad_variant_access`, `bad_optional_access`, `bad_function_call`, …
- `std::exception_ptr` (C++11): a handle to a *captured* exception that may be rethrown elsewhere (`std::current_exception()`, `std::rethrow_exception(p)`). The basis of `std::promise::set_exception`, `std::async`, and propagating errors across threads.
- `std::nested_exception` / `std::throw_with_nested` / `std::rethrow_if_nested`: build a chain "what failed because of what".
- `std::terminate`, `std::set_terminate`, `std::uncaught_exceptions`. **Don't throw from `noexcept`, destructors during unwinding, or from a `terminate` handler.**

### 5.5 Exception-safety guarantees (Abrahams)

| Guarantee | Promise if an exception is thrown from an operation | Typical technique |
|---|---|---|
| **No-throw** (`noexcept`) | Never throws; always completes | `swap`, moves, destructors, deallocation |
| **Strong** (commit-or-rollback) | State is **exactly as before the call** | Copy-and-swap; do all throwing work on a temporary first, then commit with non-throwing operations |
| **Basic** | Invariants hold; no leaks; but the state may have **changed** (valid but unspecified) | RAII for every resource |
| **None** | Leaks, corrupted invariants | Don't |

> **Rule of thumb.** Every function should provide at least the **basic** guarantee; destructors, `swap`, move operations, and deallocation should be **no-throw**; offer the **strong** guarantee where it's cheap (it often is, via copy-and-swap) and say so in the docs when you do not.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | `throw`/`catch` semantics, destructor rules, `noexcept` and `terminate` conditions, the standard exception types, `exception_ptr`, and that exceptions must not cross `extern "C"` boundaries *portably* (UB / implementation-defined) |
| **Compiler** | How `try`/cleanups are lowered: landing pads (cleanup + catch), LSDA call-site tables, `-fno-exceptions`, `-fexceptions` for C, placement of cold code; whether `noexcept` removes the unwind tables for a function |
| **ABI (Itanium)** | `__cxa_allocate_exception`/`__cxa_throw`/`__cxa_begin_catch`/`__cxa_end_catch`/`__cxa_rethrow`, `_Unwind_RaiseException`/`_Unwind_Resume`/`_Unwind_ForcedUnwind`, the `.eh_frame` CFI format (DWARF), the LSDA format in `.gcc_except_table`, the exception header preceding the thrown object, and the personality routine `__gxx_personality_v0` |
| **OS** | The dynamic loader registers each module's `.eh_frame_hdr` (via `PT_GNU_EH_FRAME` and `dl_iterate_phdr`) so the unwinder can find tables in shared libraries; **the unwinder takes a lock** and walks loaded objects: multithreaded throw scalability depends on this (old glibc/libgcc serialized throws) |
| **CPU** | A throw is a long sequence of cache-cold table lookups and indirect jumps; the happy path is unaffected |

---

## 6. Implementation model

### What the compiler emits for a function with cleanup

For `void f() { Guard g; g_may_throw(); }`:

```text
 f:                                  .gcc_except_table (LSDA) for f
   ...ctor Guard...                    call-site table:  [range of f's code where "g is alive"]  →  landing pad L1
   call g_may_throw      ──────────►                       (and the action: cleanup, no catch)
   ...dtor Guard (normal path)...
   ret
 L1:  (landing pad, out of line, cold)
   mov  [saved exception ptr], rax
   call ~Guard                         ← run the cleanup
   call _Unwind_Resume                 ← keep unwinding
```

The normal path contains **no extra instructions** for the exception machinery: just the destructor call on the way out. The tables describe, for each call instruction that might throw, *which landing pad to jump to if it does*.

### The throw: cost breakdown (🧩)

```text
 throw:  malloc the exception object + copy it         (allocation + copy)
         phase 1: for each frame: find FDE (binary search in .eh_frame_hdr), run CFI to unwind registers,
                  call personality, parse LSDA
         phase 2: same walk again + run cleanups
         (both phases together measured at about 1 µs per frame in Experiment 4)
 total:  a few microseconds for a shallow throw (3.4 µs measured at depth 1); grows linearly with depth (about 1 µs per frame measured). Experiment 4.
```

### `noexcept` and codegen

For a `noexcept` function GCC/Clang emit no landing pads *for exceptions escaping it*; a call to a potentially-throwing function inside gets an implicit `catch (...) { std::terminate(); }` frame. A call **to** a `noexcept` function lets the caller omit cleanups that only existed for that call. Net effect: usually **smaller** code and fewer landing pads, rarely faster at run time (Experiment 3 shows the table size effect).

---

## 7. Experiments

### Experiment 1 ⚖️: Trace a throw: destructors, catch order, and copies

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <stdexcept>
#include <string>

struct Trace {
    std::string name;
    explicit Trace(std::string n) : name(std::move(n)) { std::printf("  ctor  %s\n", name.c_str()); }
    ~Trace() { std::printf("  dtor  %s\n", name.c_str()); }
};

struct MyError : std::runtime_error {
    using std::runtime_error::runtime_error;
    MyError(const MyError& o) : std::runtime_error(o) { std::puts("  [MyError copied]"); }
    MyError(MyError&& o) noexcept : std::runtime_error(std::move(o)) { std::puts("  [MyError moved]"); }
};

void leaf()   { Trace t("leaf");   std::puts("  leaf throws");  throw MyError("boom"); }
void middle() { Trace a("middle-a"); Trace b("middle-b"); leaf(); std::puts("  (never printed)"); }
void outer()  { Trace t("outer");  middle(); }

int main() {
    std::puts("1. unwinding order (reverse of construction, innermost first):");
    try { outer(); }
    catch (const std::exception& e) { std::printf("  caught std::exception&: %s\n", e.what()); }

    std::puts("2. catch by value copies (and slices) the exception object:");
    try { leaf(); }
    catch (std::runtime_error e) { std::printf("  caught by VALUE (sliced to runtime_error): %s\n", e.what()); }
    try { leaf(); }
    catch (MyError e) { std::printf("  caught by VALUE as MyError (note the copy above): %s\n", e.what()); }

    std::puts("3. handler order: the first match wins, even if a later one is better:");
    try { throw MyError("derived"); }
    catch (const std::exception&)  { std::puts("  matched std::exception& first"); }
    catch (const MyError&)         { std::puts("  (MyError handler: unreachable)"); }

    std::puts("4. `throw;` rethrows the same object; `throw e;` throws a copy of the static type:");
    try {
        try { throw MyError("original"); }
        catch (const std::exception& e) { std::puts("  inner handler, rethrowing with `throw;`"); throw; }
    } catch (const MyError& e) { std::printf("  outer still sees MyError (dynamic type preserved): %s\n", e.what()); }

    try {
        try { throw MyError("original"); }
        catch (const std::exception& e) { std::puts("  inner handler, `throw e;`"); throw e; }          // SLICES to std::exception
    } catch (const MyError&) { std::puts("  outer saw MyError"); }
      catch (const std::exception&) { std::puts("  outer saw only std::exception: the derived type was sliced away"); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1. unwinding order (reverse of construction, innermost first):
  ctor  outer
  ctor  middle-a
  ctor  middle-b
  ctor  leaf
  leaf throws
  dtor  leaf
  dtor  middle-b
  dtor  middle-a
  dtor  outer
  caught std::exception&: boom
2. catch by value copies (and slices) the exception object:
  ctor  leaf
  leaf throws
  dtor  leaf
  caught by VALUE (sliced to runtime_error): boom
  ctor  leaf
  leaf throws
  dtor  leaf
  [MyError copied]
  caught by VALUE as MyError (note the copy above): boom
3. handler order: the first match wins, even if a later one is better:
  matched std::exception& first
4. `throw;` rethrows the same object; `throw e;` throws a copy of the static type:
  inner handler, rethrowing with `throw;`
  outer still sees MyError (dynamic type preserved): original
  inner handler, `throw e;`
  outer saw only std::exception: the derived type was sliced away
```

In item 2 the first by-value handler slices to `runtime_error` (its copy constructor is not instrumented, so you see no message), while the second, `catch (MyError e)`, prints `[MyError copied]`: a by-value handler really copies the exception object. Note what you do *not* see: no copy at the `throw` itself. `throw MyError("boom")` initializes the heap exception object directly from a prvalue (guaranteed elision, C++17). Always **throw by value, catch by `const&`** and rethrow with a bare `throw;`.

### Experiment 2 ⚖️: Constructors, partial construction, and the one-resource rule

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>
#include <stdexcept>

struct Res {
    const char* n;
    explicit Res(const char* n, bool fail = false) : n(n) { std::printf("  acquire %s\n", n); if (fail) throw std::runtime_error("acquire failed"); }
    ~Res() { std::printf("  release %s\n", n); }
};

// BAD: raw pointers. If the second `new` throws, the first leaks: no destructor runs for a half-built object.
struct Bad {
    Res* a; Res* b;
    Bad() : a(new Res("A")), b(new Res("B", /*fail=*/true)) {}
    ~Bad() { delete a; delete b; }                         // never runs when the constructor throws
};

// GOOD: each resource owned by its own RAII member: already-constructed members ARE destroyed.
struct Good {
    std::unique_ptr<Res> a; std::unique_ptr<Res> b;
    Good() : a(std::make_unique<Res>("A")), b(std::make_unique<Res>("B", true)) {}
};

// Function-try-block: observe (but cannot swallow) a member-initializer failure.
struct Watched {
    Res a;
    Watched() try : a("W", true) { } catch (...) { std::puts("  function-try-block saw the failure; it WILL be rethrown automatically"); }
};

int main() {
    std::puts("Bad (raw pointers):");
    try { Bad b; } catch (const std::exception& e) { std::printf("  caught: %s   -> A was never released: LEAK\n", e.what()); }

    std::puts("Good (one RAII owner per resource):");
    try { Good g; } catch (const std::exception& e) { std::printf("  caught: %s\n", e.what()); }

    std::puts("Function-try-block:");
    try { Watched w; } catch (const std::exception& e) { std::printf("  outer caught: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Bad (raw pointers):
  acquire A
  acquire B
  caught: acquire failed   -> A was never released: LEAK
Good (one RAII owner per resource):
  acquire A
  acquire B
  release A
  caught: acquire failed
Function-try-block:
  acquire W
  function-try-block saw the failure; it WILL be rethrown automatically
  outer caught: acquire failed
```

Run `Bad` under AddressSanitizer or Valgrind to see the leak reported (Chapter 28): the output above shows `acquire A` with **no** matching `release A`.

### Experiment 3 🧩: The tables and the happy path (zero-cost, measured in bytes)

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=with_try,without_try,with_cleanup
#include <cstdio>
void may_throw(int);                       // opaque: could throw
struct Guard { ~Guard(); };

int without_try(int x) { may_throw(x); return x + 1; }
int with_try(int x) {
    try { may_throw(x); return x + 1; }
    catch (...) { std::puts("caught"); return -1; }
}
int with_cleanup(int x) { Guard g; may_throw(x); return x + 1; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
without_try(int):
	push	rbx
	mov	ebx, edi
	call	may_throw(int)@PLT
	lea	eax, 1[rbx]
	pop	rbx
	ret

with_try(int):
	push	rbx
	mov	ebx, edi
	call	may_throw(int)@PLT
	lea	eax, 1[rbx]
.L4:
	pop	rbx
	ret
.L8:
	mov	rdi, rax
	jmp	.L5

with_try(int) [clone .cold]:
.L5:
	call	__cxa_begin_catch@PLT
	lea	rdi, .LC0[rip]
	call	puts@PLT
	call	__cxa_end_catch@PLT
	or	eax, -1
	jmp	.L4
.L9:
	mov	rbx, rax
	call	__cxa_end_catch@PLT
	mov	rdi, rbx
	call	_Unwind_Resume@PLT

with_cleanup(int):
	push	rbx
	mov	ebx, edi
	sub	rsp, 16
	call	may_throw(int)@PLT
	lea	rdi, 15[rsp]
	call	Guard::~Guard()@PLT
	add	rsp, 16
	lea	eax, 1[rbx]
	pop	rbx
	ret
.L13:
	mov	rbx, rax
	jmp	.L12

with_cleanup(int) [clone .cold]:
.L12:
	lea	rdi, 15[rsp]
	call	Guard::~Guard()@PLT
	mov	rdi, rbx
	call	_Unwind_Resume@PLT
```

Compare `without_try` and the *hot path* of `with_try`: the instruction sequence up to `ret` is the same (a call and an add); the `catch` block and the cleanup live **after** the `ret`, at the landing pad label, never executed unless something throws. That is "zero cost" in the instruction stream. Now the other cost, **space**:

```bash
cat > ab.cpp <<'EOF'
void f(int); struct G { ~G(); };
int a(int x){ G g; f(x); return x; }
int b(int x){ G g; f(x); return x; }
EOF
g++-14 -std=c++23 -O2 -c ab.cpp -o ab.o
g++-14 -std=c++23 -O2 -fno-exceptions -c ab.cpp -o noexc.o
size -A ab.o noexc.o | grep -E "^\.(text|eh_frame|gcc_except_table)"
```

```text
# measured (GCC 14.2, bytes; run by hand, not auto-verified)
                         .text   .text.unlikely   .gcc_except_table   .eh_frame
default (exceptions on)    179          78                32               168
-fno-exceptions            149           -                 -                96
(adding one function with try/catch to the file:  .text 220, .text.unlikely 96, .gcc_except_table 64, .eh_frame 232)
```

The two functions each hold a destructible local across a call. With exceptions enabled the file carries an out-of-line landing pad per function (`.text.unlikely`, 78 B), an LSDA (`.gcc_except_table`, 32 B), and larger CFI (`.eh_frame` 168 B vs 96 B); with `-fno-exceptions` all of that shrinks away. (`.eh_frame` does not vanish entirely: GCC still emits CFI for stack unwinding by debuggers and profilers, which is why `-fno-exceptions` alone does not remove it; use `-fno-asynchronous-unwind-tables` if you do not need that.) The point: the cost of the machinery is **binary size and layout**, not instructions on the happy path.

### Experiment 4 🔧: What does a throw cost? And what does a non-throw cost?

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>

using Clock = std::chrono::steady_clock;

// A chain of `depth` non-inlined frames, each with a destructor to run during unwinding.
struct Cleanup { volatile int* p; ~Cleanup() { *p = *p + 1; } };
volatile int g_sink = 0;

[[gnu::noinline]] int fail_throw(int depth, int n) {
    Cleanup c{&g_sink};
    if (depth == 0) { if (n >= 0) throw std::runtime_error("x"); return 0; }
    return fail_throw(depth - 1, n) + 1;
}
[[gnu::noinline]] int fail_code(int depth, int n) {              // the same recursion returning an error code
    Cleanup c{&g_sink};
    if (depth == 0) return n >= 0 ? -1 : 0;
    int r = fail_code(depth - 1, n);
    return r < 0 ? r : r + 1;
}

double ns_per(int iters, auto&& body) {
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) body();
    return std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / iters;
}

int main() {
    std::puts("cost per FAILED call, by stack depth between throw and catch (ns):");
    std::printf("  %-8s %12s %12s\n", "depth", "throw/catch", "error code");
    for (int depth : {1, 5, 20, 100}) {
        int iters = depth >= 100 ? 20'000 : 100'000;
        double t = ns_per(iters, [&] { try { fail_throw(depth, 1); } catch (const std::exception&) { g_sink = g_sink + 1; } });
        double c = ns_per(iters * 10, [&] { if (fail_code(depth, 1) < 0) g_sink = g_sink + 1; });
        std::printf("  %-8d %12.0f %12.1f\n", depth, t, c);
    }

    std::puts("cost per SUCCESSFUL call (no failure), depth 20 (ns):");
    double ok_try = ns_per(2'000'000, [&] { try { fail_throw(20, -1); } catch (...) {} });
    double ok_nt  = ns_per(2'000'000, [&] { fail_throw(20, -1); });
    double ok_cd  = ns_per(2'000'000, [&] { (void)fail_code(20, -1); });
    std::printf("  with try/catch around it   %7.1f\n  without try/catch          %7.1f\n  error-code version         %7.1f\n", ok_try, ok_nt, ok_cd);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
cost per FAILED call, by stack depth between throw and catch (ns):
  depth     throw/catch   error code
  1                3310          4.3
  5                8457         12.3
  20              24385         75.6
  100            109973        717.6
cost per SUCCESSFUL call (no failure), depth 20 (ns):
  with try/catch around it      96.1
  without try/catch             96.6
  error-code version            81.2
```

Measured on one machine (GCC 14.2, `-O2`, the output above). **Failure path:** a throw/catch costs about **3.4 µs at depth 1** and then roughly **1 µs per additional frame** (7.4 µs at depth 5, 23 µs at depth 20, 101 µs at depth 100), against **4 ns to 756 ns** for the error-code version of the same recursion: a ratio from about 750× at depth 1 down to about 130× at depth 100 (the error code also gets slower with depth because each frame propagates it). **Success path:** the version with `try`/`catch` around the call (105 ns) and without it (98 ns) are within a few percent of each other, which is noise-level: the assembly in Experiment 3 has no instructions for it. The break-even rule follows: exceptions win when failures are *rare* (the happy-path code stays tight and error handling is cheap to write); they lose when failure is a *normal outcome* happening often (parsing untrusted input, "not found" lookups, validation of user data), where `optional`/`expected`/error codes (Chapter 23) are the right tools. (Absolute numbers depend heavily on the machine, the unwinder version and how many shared objects are loaded; trust the ratio and the linear growth in depth.)

### Experiment 5 ⚖️: `noexcept`, `terminate`, and `move_if_noexcept`

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <type_traits>
#include <utility>
#include <vector>

struct Safe   { Safe() = default; Safe(const Safe&) { std::puts("    Safe copy"); }   Safe(Safe&&) noexcept { std::puts("    Safe move"); } };
struct Unsafe { Unsafe() = default; Unsafe(const Unsafe&) { std::puts("    Unsafe copy"); } Unsafe(Unsafe&&) noexcept(false) { std::puts("    Unsafe move"); } };

void a() noexcept {}
void b() {}
int  c(int) noexcept(sizeof(int) == 4) { return 0; }

int main() {
    std::puts("noexcept is part of the type (C++17):");
    static_assert(noexcept(a()) && !noexcept(b()) && noexcept(c(1)));
    static_assert(!std::is_same_v<decltype(&a), decltype(&b)>);
    void (*pb)() = &a;            // noexcept -> non-noexcept conversion is fine
    // void (*pa)() noexcept = &b;  // ERROR: the reverse is not
    (void)pb;
    std::printf("  is_nothrow_move_constructible<Safe>=%d  <Unsafe>=%d\n",
                std::is_nothrow_move_constructible_v<Safe>, std::is_nothrow_move_constructible_v<Unsafe>);

    std::puts("vector growth uses move_if_noexcept (Chapter 6):");
    std::puts("  growing vector<Safe>:");
    { std::vector<Safe> v; v.reserve(1); v.emplace_back(); v.emplace_back(); }
    std::puts("  growing vector<Unsafe>:");
    { std::vector<Unsafe> v; v.reserve(1); v.emplace_back(); v.emplace_back(); }

    std::puts("a throw escaping a noexcept function calls std::terminate (installed handler runs):");
    std::set_terminate([] { std::puts("  >>> terminate handler called"); std::fflush(stdout); std::_Exit(0); });
    auto f = []() noexcept { throw 1; };
    f();
    std::puts("(not reached)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
noexcept is part of the type (C++17):
  is_nothrow_move_constructible<Safe>=1  <Unsafe>=0
vector growth uses move_if_noexcept (Chapter 6):
  growing vector<Safe>:
    Safe move
  growing vector<Unsafe>:
    Unsafe copy
a throw escaping a noexcept function calls std::terminate (installed handler runs):
  >>> terminate handler called
```

(The program exits with status 0 through our terminate handler so the checker can run it; normally `terminate` calls `abort()`.)

### Experiment 6 ⚖️: Strong exception safety by copy-and-swap, and how to *test* it

```cpp
// @test run -std=c++23 -O0
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

// A fault injector: the Nth allocation-like operation throws. Used to prove (not hope) a guarantee.
struct Fault { static inline int countdown = -1; static void tick() { if (countdown >= 0 && countdown-- == 0) throw std::runtime_error("injected"); } };

// Element type whose copies can fail.
struct Item {
    int v;
    explicit Item(int v) : v(v) {}
    Item(const Item& o) : v(o.v) { Fault::tick(); }
    Item& operator=(const Item&) = delete;
};

// A tiny dynamic array with TWO versions of assign(): naive (basic guarantee at best) and copy-and-swap (strong).
class Buf {
    std::unique_ptr<Item[]> p_;
    std::size_t n_ = 0;
public:
    Buf() = default;
    explicit Buf(std::size_t n, int v) : p_(static_cast<Item*>(::operator new[](n * sizeof(Item)))) {
        for (; n_ < n; ++n_) new (&p_[n_]) Item(v);       // construct one by one
    }
    Buf(const Buf& o) : p_(static_cast<Item*>(::operator new[](o.n_ * sizeof(Item)))) {
        try { for (; n_ < o.n_; ++n_) new (&p_[n_]) Item(o.p_[n_]); }
        catch (...) { destroy(); throw; }                  // basic guarantee for the constructor: no leak
    }
    Buf(Buf&& o) noexcept : p_(std::move(o.p_)), n_(std::exchange(o.n_, 0)) {}
    ~Buf() { destroy(); }
    void swap(Buf& o) noexcept { std::swap(p_, o.p_); std::swap(n_, o.n_); }                // NO-THROW

    // NAIVE: destroys the old contents first. If copying then throws, *this is damaged (basic at best: here, even invariants break).
    void assign_naive(const Buf& o) {
        destroy(); n_ = 0;
        p_.reset(static_cast<Item*>(::operator new[](o.n_ * sizeof(Item))));
        for (; n_ < o.n_; ++n_) new (&p_[n_]) Item(o.p_[n_]);          // may throw: *this now half-built
    }
    // COPY-AND-SWAP: all throwing work on a temporary; commit with a no-throw swap. STRONG guarantee.
    void assign_strong(const Buf& o) { Buf tmp(o); swap(tmp); }

    std::size_t size() const { return n_; }
    int sum() const { int s = 0; for (std::size_t i = 0; i < n_; ++i) s += p_[i].v; return s; }
private:
    void destroy() noexcept { for (std::size_t i = 0; i < n_; ++i) p_[i].~Item(); n_ = 0; }
};

template <class Op>
bool survives_every_failure_point(const char* label, Op op) {
    // Fault injection: make the 1st, 2nd, 3rd... copy fail in turn and check the object is unchanged after each failure.
    bool ok = true;
    for (int fail_at = 0; fail_at < 8; ++fail_at) {
        Buf target(3, 10);                       // size 3, sum 30
        Buf source(5, 1);                        // size 5, sum 5
        Fault::countdown = fail_at;
        bool threw = false;
        try { op(target, source); } catch (const std::exception&) { threw = true; }
        Fault::countdown = -1;
        if (threw && !(target.size() == 3 && target.sum() == 30)) {
            std::printf("  %-14s fail_at=%d: STATE CHANGED after exception (size=%zu sum=%d)\n", label, fail_at, target.size(), target.sum());
            ok = false;
        }
    }
    std::printf("  %-14s %s\n", label, ok ? "strong guarantee holds at every injection point" : "VIOLATES the strong guarantee");
    return ok;
}

int main() {
    std::puts("fault-injection test of assignment:");
    survives_every_failure_point("assign_naive",  [](Buf& t, const Buf& s) { t.assign_naive(s); });
    survives_every_failure_point("assign_strong", [](Buf& t, const Buf& s) { t.assign_strong(s); });
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
fault-injection test of assignment:
  assign_naive   fail_at=0: STATE CHANGED after exception (size=0 sum=0)
  assign_naive   fail_at=1: STATE CHANGED after exception (size=1 sum=1)
  assign_naive   fail_at=2: STATE CHANGED after exception (size=2 sum=2)
  assign_naive   fail_at=3: STATE CHANGED after exception (size=3 sum=3)
  assign_naive   fail_at=4: STATE CHANGED after exception (size=4 sum=4)
  assign_naive   VIOLATES the strong guarantee
  assign_strong  strong guarantee holds at every injection point
```

This harness is the standard way to **verify** exception safety rather than assert it: make each fallible operation fail in turn and check the post-condition. Libraries such as Boost.Test's `exception_safety` utilities and Abseil's `exception_safety_testing` generalize it (run under ASan/Valgrind to catch leaks as well as state corruption).

### Experiment 7 ⚖️: Carrying exceptions: `exception_ptr`, nesting, and threads

```cpp
// @test run -std=c++23 -O0 link=-pthread
#include <cstdio>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

void print_chain(const std::exception& e, int level = 0) {
    std::printf("%*s- %s\n", level * 2, "", e.what());
    try { std::rethrow_if_nested(e); }
    catch (const std::exception& inner) { print_chain(inner, level + 1); }
}

void load_config()   { throw std::runtime_error("file 'app.toml' not found"); }
void start_service() {
    try { load_config(); }
    catch (...) { std::throw_with_nested(std::runtime_error("service failed to start")); }    // wrap, keeping the cause
}

int main() {
    std::puts("nested exceptions:");
    try { start_service(); } catch (const std::exception& e) { print_chain(e); }

    std::puts("capture on one thread, rethrow on another:");
    std::exception_ptr captured;
    std::thread worker([&] { try { throw std::logic_error("failure inside worker"); } catch (...) { captured = std::current_exception(); } });
    worker.join();
    try { std::rethrow_exception(captured); } catch (const std::logic_error& e) { std::printf("  main thread rethrew: %s\n", e.what()); }

    std::puts("std::async / future transports the exception for you:");
    auto fut = std::async(std::launch::async, []() -> int { throw std::out_of_range("from async task"); });
    try { fut.get(); } catch (const std::out_of_range& e) { std::printf("  future::get rethrew: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
nested exceptions:
- service failed to start
  - file 'app.toml' not found
capture on one thread, rethrow on another:
  main thread rethrew: failure inside worker
std::async / future transports the exception for you:
  future::get rethrew: from async task
```

`exception_ptr` is reference-counted shared ownership of the heap exception object: that is why it can outlive the `catch` block and cross threads. It is also the reason errors *can* be propagated through `promise`/`future`, `std::async` and thread pools (Chapter 29) without losing their type.

---

## 8. Assembly / runtime investigation

Experiment 3 is the assembly investigation. To see the machinery itself:

```bash
# (1) The unwind tables of a real binary:
readelf -S -W a.out | grep -E "eh_frame|gcc_except_table"        # sizes of .eh_frame_hdr, .eh_frame, .gcc_except_table
readelf --debug-dump=frames a.out | head -40                      # CFI records (FDEs): how to restore registers for each PC range
objdump -s -j .gcc_except_table a.out | head                      # the LSDA: call-site table + action table + type table

# (2) The runtime calls a throw/catch becomes:
g++-14 -std=c++23 -O1 -S -masm=intel -o - prog.cpp | grep -E "__cxa_|_Unwind"
#   __cxa_allocate_exception, __cxa_throw, __cxa_begin_catch, __cxa_end_catch, __cxa_rethrow, _Unwind_Resume, __gxx_personality_v0

# (3) Watch a throw in the debugger:
gdb -ex 'catch throw' -ex 'catch catch' -ex run ./a.out           # breaks in __cxa_throw / __cxa_begin_catch; `bt` shows the throw site

# (4) Where does throwing time go?
perf record -g ./throw_bench && perf report --stdio | head -40    # expect: _Unwind_Find_FDE, _Unwind_RaiseException, uw_frame_state_for, __gxx_personality_v0
```

If `perf` shows time in `_Unwind_Find_FDE` / `dl_iterate_phdr`, you are paying the loaded-module walk; many shared objects make throws slower (see Exercise 6).

---

## 9. Implementation exercise

Build a small **exception-safety test toolkit** and apply it to a container you wrote:

1. A `FaultInjector` with `maybe_throw()` called from a *test element type*'s constructors, copy/move operations, comparison, and a *test allocator*'s `allocate`; a driver `for_each_failure_point(fn)` that runs `fn` repeatedly, failing the 1st, 2nd, … fallible operation, until a run completes without injection.
2. Tracking: the test element counts live instances (so a leak is a non-zero count at the end) and records invariants.
3. Run it on your `Vector<T>` from Project 2 for `push_back`, `insert`, `assign`, `resize`, `operator=`, `reserve`. For each operation state which guarantee it provides, then prove it.
4. Fix the operations that fail; keep the harness as a unit test.

<details>
<summary><strong>Solution sketch: the driver</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <functional>
#include <stdexcept>

struct FaultInjector {
    static inline long counter = 0;       // operations seen in this run
    static inline long fail_at = -1;      // which one to fail (-1 = none)
    static void maybe_throw() { if (counter++ == fail_at) throw std::runtime_error("injected fault"); }
};

// Runs fn with a fault at operation 0, 1, 2, ... until a full run completes with no fault.
// Returns the number of distinct failure points exercised.
long for_each_failure_point(const std::function<void()>& fn, const std::function<void()>& after_each_failure) {
    for (long point = 0;; ++point) {
        FaultInjector::counter = 0; FaultInjector::fail_at = point;
        try { fn(); FaultInjector::fail_at = -1; return point; }          // completed without hitting the fault: done
        catch (const std::runtime_error&) { FaultInjector::fail_at = -1; after_each_failure(); }   // verify invariants here
    }
}

int main() {
    int steps_done = 0;
    long points = for_each_failure_point(
        [&] { for (int i = 0; i < 5; ++i) { FaultInjector::maybe_throw(); ++steps_done; } },   // a 5-step operation
        [&] { /* check: object state, live-instance count == baseline */ });
    std::printf("operation has %ld failure points; all were exercised\n", points);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
operation has 5 failure points; all were exercised
```

The loop terminates when `fn` runs to completion: the number of iterations *is* the number of fallible operations, found automatically.

</details>

---

## 10. Real-world example

| Where | Exceptions in practice |
|---|---|
| **Standard library** | `bad_alloc`, `out_of_range` (`at()`), `bad_variant_access`, `system_error`/`filesystem_error`, stream failures (opt-in), `std::stoi`; containers' strong guarantee relies on `noexcept` moves |
| **Constructors & operators** | The *only* way for a constructor to report failure without a two-phase init |
| **Google, LLVM, Chromium, many game studios** | **Disable exceptions** (`-fno-exceptions`) for binary size, predictability and ABI/legacy reasons; use status codes / `StatusOr` / `expected`-like types |
| **Boost, Qt (partially)** | Qt itself does not use exceptions in its API (Chapter 48): it is exception-*neutral* (compiled so they can pass through, but signals/slots and event handlers must not let one escape, or behaviour is undefined) |
| **Embedded / kernel / real-time** | Usually off: unbounded latency of a throw, table size, no allocator for the exception object |
| **Python bindings (pybind11)** | Translate C++ exceptions into Python exceptions at the boundary (Chapter 47) |
| **C interop** | A C++ exception must not propagate through C frames (Chapter 46): catch at the `extern "C"` boundary and return an error code |

> **Opinion.** Exceptions are the right tool for **rare, truly exceptional failures that the immediate caller cannot sensibly handle**: out of memory, a missing mandatory file, a violated invariant that needs the whole operation abandoned. They are the **only** reasonable way to fail from a constructor, and with RAII they keep the success path honest and short. They are the wrong tool for **expected outcomes** (a parse failure on user input, "key not found", a socket that closed): use `optional`/`expected` (Chapter 23). Never use exceptions for ordinary control flow: at several microseconds per throw, a loop that throws on one input in ten is a performance bug and a design smell. Treat `noexcept` as a **contract**: put it on destructors, moves, `swap`, and low-level primitives, and do not sprinkle it as an optimization. If your project forbids exceptions, fine, but then own the consequences: constructors need factories, and `std::vector` can only abort on `bad_alloc`. And if you do use them: **catch by `const&`, rethrow with `throw;`, never let one cross a C boundary, a thread boundary (use `exception_ptr`), or a destructor.**

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Throwing from a destructor | `terminate` during unwinding | Destructors never throw; catch and log inside, or provide an explicit `close()` that can fail |
| `catch (std::exception e)` by value | Slicing and an extra copy; loses the derived `what()` | Catch by `const&` |
| `throw e;` instead of `throw;` in a handler | Slices; changes dynamic type | `throw;` |
| Two raw resources in one class | Leak when the second constructor step throws | One RAII owner per resource (Experiment 2) |
| Missing `noexcept` on move | `vector` growth copies instead of moves (performance), strong guarantee needs copies | `noexcept` on move ctor/assignment, `swap` |
| `noexcept` on a function that can actually throw | `std::terminate` at run time | Remove it, or handle the failure inside |
| Exception crossing `extern "C"` / a plugin / a thread boundary | UB / `terminate` / lost error | Catch at the boundary; translate to error code; `exception_ptr` for threads |
| `catch (...)` that swallows everything | Silent failure, hidden `bad_alloc`, thread cancellation (`abi::__forced_unwind`) swallowed | Catch specific types; rethrow if unknown |
| Using exceptions for control flow | Microseconds per iteration | `optional`/`expected`/return codes |
| Exception thrown while another is being handled in a destructor called by unwinding | `terminate` | `std::uncaught_exceptions()` to branch; or no-throw destructors |
| Relying on stack unwinding for an uncaught exception | Destructors may not run (implementation-defined) | Catch in `main` and exit cleanly |
| Exception objects with throwing copy constructors | `terminate` (copying the exception object throws) | Make exception types cheap and non-throwing to copy (`shared_ptr` payload) |
| Mixing `-fno-exceptions` and `-fexceptions` objects | `terminate`/link errors | Consistent flags across the whole program and its libraries |
| Large exception objects / formatting in `what()` at throw time | Slow throws; allocation during `bad_alloc` handling | Small, message-by-reference exception types; format lazily |

---

## 12. Exercises

1. **Trace it in gdb.** Break on `__cxa_throw`, `_Unwind_RaiseException`, `__gxx_personality_v0`; step through Experiment 1 and print the exception object's address and the caught type's `typeinfo`.
2. **Read the LSDA.** Compile a function with one `try` and two destructible locals; dump `.gcc_except_table` and decode the call-site table by hand using the format in the Itanium ABI (LSDA section). Verify against `readelf --debug-dump=frames`.
3. **Cost vs depth.** Extend Experiment 4 with a call stack that includes a shared-library frame and 1, 10, 50 loaded `.so` files. Does the throw cost grow? Explain via `dl_iterate_phdr` and the unwinder's caching.
4. **Strong guarantee audit.** Take `std::vector::insert` (middle), `std::map::operator[]`, `std::unordered_map::rehash`. Look up their documented guarantees (cppreference/standard) and write the fault-injection test to confirm each on your implementation.
5. **`noexcept` or not?** For ten functions from your code base (a destructor, a getter, a move constructor, an allocating factory, a callback invoker, …), decide `noexcept`/not and justify, including what happens to the program if one *does* throw.
6. **No-exception build.** Compile a small project with `-fno-exceptions`; list what stops compiling (`throw`, `try`, `dynamic_cast` to references, standard facilities); measure `.text` and `.eh_frame` size before/after.
7. **Exception-translation layer.** For a C API wrapper (Chapter 46), write a macro-free helper `guard(fn)` that catches all exceptions and maps them to an `int` error code plus a thread-local message, and its inverse (code → exception).
8. **Compare:** implement the same file-loading function with (a) exceptions, (b) `std::optional`, (c) error code + out-parameter, (d) `std::expected` (after Chapter 23). Count lines, branches, and microbenchmark the failure path.

---

## 13. Challenge: an exception-safety verifier

Build a library that, given a callable operating on a user-defined type, **automatically** finds every fallible operation and verifies a guarantee:

- instrument the type: wrap it in a template that injects faults in all special members and operators (through a CRTP or a policy wrapper), counts live instances, and records a state fingerprint (user-supplied `snapshot()`)
- a driver explores failure points iteratively (as in the solution sketch), and for each reports: leaked instances, post-state ≠ pre-state (strong guarantee violation), invariant failures (basic guarantee violation), or an unexpected `terminate`/crash (use fork-per-run to survive aborts)
- applies to `std::vector`, `std::map`, and your own containers; produce a table "operation × guarantee" for each
- handle allocation failures with a custom `operator new` that can be made to throw `bad_alloc` at the nth call
- discuss what it cannot prove (non-deterministic operations, concurrency, side effects outside the object)

---

## 14. Knowledge check

1. Describe the two phases of exception propagation. Which destructors run in each?
2. Where does the exception object live, and why does that matter for `throw e;` versus `throw;`?
3. What does "zero-cost" mean precisely? Name two costs that remain.
4. What happens if no handler matches? Are destructors guaranteed to run?
5. Why does a throwing constructor not run the object's own destructor, and what does that imply for resource-owning classes?
6. What happens when a destructor throws during unwinding? When is a destructor *allowed* to throw?
7. How does `noexcept` interact with `std::vector` reallocation? Why?
8. State the three exception-safety guarantees and give a technique for each.
9. Why is catching by value a bug?
10. About how much slower is a throw than an error-code return (Experiment 4), and when does the break-even point favour each?
11. How does an exception cross a thread boundary?
12. Which of these should be `noexcept`: a destructor, a move constructor, a function that allocates, `swap`, a logging callback?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Phase 1 (search): the unwinder walks frames, asking each personality routine whether a handler matches; no destructors run. Phase 2 (cleanup): it unwinds again, running landing pads (destructors of live automatic objects) up to the handler's frame; then the catch block runs.
2. On the heap, allocated by `__cxa_allocate_exception`, outside the unwinding stack. `throw;` rethrows the same object (preserves dynamic type); `throw e;` copies `e` by its static type, which may slice.
3. No instructions are executed on the happy path for entering a `try` or for the possibility of throwing; tables are consulted only on throw. Remaining costs: binary size (`.eh_frame`, LSDA), code layout/landing pads, and optimization limits around calls that may throw (and the expensive throw itself).
4. `std::terminate` is called. Whether the stack is unwound first is implementation-defined, so destructors are *not* guaranteed to run.
5. The object is not fully constructed, so its destructor must not run; only fully constructed bases/members are destroyed. A class that owns two raw resources leaks the first if the second acquisition throws; use one RAII member per resource.
6. During unwinding it calls `std::terminate`. Destructors are implicitly `noexcept` (C++11); one may throw only if declared `noexcept(false)` and not invoked during unwinding (check `uncaught_exceptions()`), which is almost always a bad idea.
7. `std::vector` growth uses `std::move_if_noexcept`: if the move constructor can throw and the type is copyable, it copies to keep the strong guarantee. A non-`noexcept` move silently costs a copy per element.
8. No-throw (e.g. `swap`, destructors: simple non-allocating operations); strong (copy-and-swap, do throwing work on a temporary then commit); basic (RAII for every resource so invariants hold and nothing leaks).
9. It copies the exception and slices it to the handler's static type, losing the dynamic type and derived data (and `what()` override).
10. A throw measured about 3.4 µs at depth 1 and ~1 µs more per frame, versus a few nanoseconds to under a microsecond for a code return: two to three orders of magnitude. Exceptions win when failure is rare (clean fast path); codes/`expected` win when failure is common or latency-bounded.
11. Capture with `std::current_exception()` into an `exception_ptr`, transport it (shared ownership), and `std::rethrow_exception` in the receiving thread; `promise`/`future`/`async` do this automatically.
12. Destructor: yes (implicit). Move constructor: yes (if it truly can't throw). `swap`: yes. Allocating function: usually no (it can throw `bad_alloc`, unless you deliberately want termination). Logging callback invoked from destructors/cleanup: often yes, with exceptions caught inside.

</details>

---

[← Previous: Chapter 21](../part-08-polymorphism/21-type-erasure.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 23 — `std::expected` →](23-expected.md)
