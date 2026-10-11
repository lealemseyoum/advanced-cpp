# Chapter 45 — C Compatibility: Living on the Boundary

> **Part XVII · C and Python interoperability** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 4 (RAII)](../part-02-object-model-and-lifetime/04-raii.md), [Chapter 22 (exceptions)](../part-09-error-handling/22-exceptions.md), [Chapter 25 (smart pointers)](../part-10-memory/25-smart-pointers.md), [Chapter 36 (compilation model)](../part-14-compilation-and-linking/36-compilation-model.md), [Chapter 37 (ABI)](../part-14-compilation-and-linking/37-abi.md), [Chapter 42 (API design)](../part-16-library-design/42-api-design.md) &nbsp;|&nbsp; **Standards:** language linkage ⚖️ [dcl.link]; C++23 `<stdatomic.h>` ⚖️ &nbsp;|&nbsp; **Tools:** `g++-14`, `gcc`, `nm`, `ldd`, SQLite 3 as the real C library, ASan

[← Previous: Chapter 44](../part-16-library-design/44-design-patterns-revisited.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 46 — C++ and Python →](46-python-interop.md)

---

**In one sentence:** the C ABI is the lingua franca of every language boundary on your system, so the craft is to keep the C-shaped interface **thin, explicit and boring** (handles, status codes, plain data, callbacks with a `void*`) and put all the C++ (RAII, exceptions, templates, ownership types) on either side of it, never across it.

**By the end of this chapter you can:**

- wrap any C library in a safe, idiomatic C++ interface: RAII handles with custom deleters, error translation, callback trampolines
- expose a C++ library through a C API that C, Python, Rust or Go can call, with no exception or `std::` type crossing the line
- state exactly what `extern "C"` changes (and what it does not), and what the real differences between C and C++ are
- predict and demonstrate what happens when an exception crosses C frames (it can "work" and still corrupt state)
- design the ownership and error conventions of a C API so that misuse is hard

---

## 1. Problem

Almost everything outside C++ talks to native code through the C calling convention: Python's `ctypes`/CFFI, Rust's FFI, Go's cgo, Java's JNI, the operating system's own libraries (POSIX, Win32), and every old library worth using (SQLite, zlib, OpenSSL, libcurl, FFmpeg). Two directions matter:

| Direction | Task | Hazard |
|---|---|---|
| **C → C++** (you call a C library) | Use `sqlite3*`, `FILE*`, `curl_easy_*` from modern C++ | Leaks on early return; ignored status codes; manual `free`; callbacks that cannot carry closures; exceptions escaping through C frames |
| **C++ → C** (you expose your library) | Let C, Python, Rust call your code | Name mangling; classes, templates, references, overloads and exceptions have no C representation; ABI-fragile structs; ownership ambiguity ("who frees this?") |

C++ is *not* a superset of C, and "C compatible" means different things at the **source level** (does this header compile as C++?), the **link level** (do the symbols match?), and the **ABI level** (do the bytes agree?). Confusing the three is the root of most interop bugs.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1972–89 | C and K&R; ANSI C89. C++ ("C with Classes", 1979; Cfront 1983) was designed to compile most C |
| 1983 | Cfront translates C++ to C; name mangling invented to let overloaded functions have unique C-level names |
| 1989–90 | `extern "C"` and `#ifdef __cplusplus` guards become the standard headers idiom |
| 1999 | C99 adds VLAs, designated initializers, `restrict`, `_Bool`, compound literals, complex: **C and C++ diverge** (C++ takes some later, differently) |
| 1998–2011 | C++98/11: language linkage formalised ([dcl.link]); `noexcept`; `<cstdio>`-style headers |
| 2011–17 | C11 adds `_Atomic` and `_Generic`; C++11 adds `std::atomic`: same hardware, incompatible spellings |
| 2017+ | Itanium ABI is the de-facto C++ ABI on Linux; C ABI remains the only universal one |
| 2023 | C++23 adds `<stdatomic.h>` so that C and C++ headers can share `_Atomic` declarations; C23 adopts `nullptr`, `constexpr`, `bool` as keyword, `#embed`; **the languages converge in places** |
| 2020s | Rust/Go/Zig/Python bindings generators (`cbindgen`, `bindgen`, `cffi`) all consume a C API; "C ABI as the stable surface of a C++ library" is standard practice (Qt does not do this; Sciter, ONNX Runtime, LLVM-C, and `wgpu` do) |

---

## 3. Modern solution

Two architectures, each a three-layer sandwich.

**Consuming a C library (C → C++):**

```text
   raw C API (header from the vendor)          sqlite3_open, sqlite3_exec, sqlite3_close, sqlite3_free ...
          │   keep it private in a .hpp/.cpp
          ▼
   thin RAII layer                              unique_ptr<sqlite3, Deleter>, Result<T> = expected<T, Error>
          │
          ▼
   idiomatic C++ API for your callers           Db::open, Stmt::prepare, callbacks as std::function
```

**Exposing a C++ library (C++ → C):**

```text
   your C++ library (classes, templates, exceptions)
          │
          ▼
   C shim (extern "C"): opaque handle, status codes, last-error, C callbacks with void* user data
          │                      ONE guard per function: try { ... } catch (...) { translate }
          ▼
   callers: C, Python (ctypes/cffi), Rust, Go, other C++ compilers/stdlibs
```

The rules that make both safe:

1. **Only C types cross**: integers, floats, pointers, `size_t`, `enum`s with an explicit underlying width, plain structs with fixed layout, function pointers with a `void*` context.
2. **Opaque handles** instead of structs: `typedef struct wc_counter wc_counter;` lets you change the C++ class freely.
3. **Status codes** (plus a thread-local last-error string), never exceptions, across the boundary.
4. **Explicit ownership**: for every pointer, say who frees it and with which function. Pair every `create` with a `destroy`; allocate and free on the *same side*.
5. **Callbacks carry a context**: `void (*)(…, void* user)`. A C++ lambda is adapted by a captureless trampoline (Experiment 1).
6. **A C++ guard on every exported function**: nothing may throw out of `extern "C"` (Experiment 3).

---

## 4. Mental model

### What crosses the line

```text
    ───────────── C++ side ────────────┐  ┌──────────── C side ──────────────
    classes, templates, overloads,      │  │
    references, std::string/vector,     │  │   int, long, double, size_t, T*
    exceptions, virtual functions,   ✗  │  │   structs of those (fixed layout)
    default arguments, namespaces       │  │   enums (fixed width), void*
                                        │  │   function pointers, opaque handles
    ───────────────────────────────────┘  └──────────────────────────────────
                      the `extern "C"` boundary: a function name, a calling convention, nothing else
```

`extern "C"` changes **only the linkage name** (no mangling, so exactly one function per name; no overloading) and says "this has C language linkage". It does *not* change the calling convention on SysV x86-64 (C and C++ share it), does not check that your types are C-compatible, and does not stop you from throwing. Everything else is on you.

### The three levels of "compatible"

| Level | Question | Fails when |
|---|---|---|
| **Source** | Does this header compile as both C and C++? | C99/C11 features C++ lacks or treats differently: implicit `void*` conversion, VLAs, designated-initializer order, `restrict`, compound literals (Experiment 4) |
| **Link** | Do the symbol names match? | Missing `extern "C"`, so the C++ side references a mangled name the C library doesn't have (`undefined reference to 'foo(int)'`) |
| **ABI** | Do sizes, alignment, calling convention and exception behaviour agree? | `bool`/`enum` width mismatch, non-standard-layout structs, a different libstdc++ in each module, exceptions unwinding through code without unwind tables |

### Ownership is the real interface

For every pointer parameter or result, a C API must answer: **who allocates, who frees, how long is it valid**. The C++ side expresses the answer in a *type* (`unique_ptr<T, Deleter>`, `std::span`, `string_view`), the C side only in documentation and naming. Your RAII wrapper is the executable form of that documentation.

| C convention | C++ expression |
|---|---|
| `T* create(); void destroy(T*);` | `std::unique_ptr<T, Destroyer>` |
| `int open(T** out)`; caller owns `*out` on success | Factory returning `expected<Handle, Error>`; ownership taken **before** inspecting the status (Experiment 1) |
| Borrowed pointer, valid until next call | `const T*` / `string_view` with documented lifetime; never store it |
| "Returns a string you must `free`/`lib_free`" | Wrap immediately in `unique_ptr<char, FreeDeleter>` or copy into `std::string` and free |
| Out-parameter + length | `std::span<const std::byte>` after the call |
| Callback + `void*` | `std::function` held by the wrapper; pass its address as the context |

---

## 5. Language rules

| Rule | Text | Consequence |
|---|---|---|
| **Language linkage** ⚖️ [dcl.link] | `extern "C"` gives functions and variables C language linkage; "C++" is the default. Linkage specifications nest; `extern "C" { ... }` can contain declarations of many entities | Names are not mangled; two functions with the same name in different namespaces (or overloads) under `extern "C"` refer to the **same** function |
| **Function type linkage** ⚖️ | Language linkage is formally part of a function type, so a pointer to `extern "C"` function and a pointer to a C++ function are *different types* | In practice GCC/Clang/MSVC ignore this distinction; the standard permits a diagnostic. Declare callbacks `extern "C"` when you want to be strictly correct |
| **Exceptions through C** ⚖️ | Implementation-defined/undefined: the standard says nothing about unwinding through non-C++ frames; a `noexcept` function that exits via exception calls `std::terminate` | Experiment 3: works on Linux x86-64 *because* libc and SQLite are built with unwind tables, and still corrupts library state |
| **`noexcept` on `extern "C"`** ⚖️ | Allowed and recommended on callbacks and exported functions that must not throw | Turns an escaping exception into immediate `terminate` instead of undefined state (Experiment 3) |
| **Standard-layout** ⚖️ [class.prop] | A standard-layout class has the same member layout rules as C (no virtual functions, no non-standard-layout members/bases, same access control on all non-static members, etc.) and its first member is at offset 0 | Required for a struct to be passed to C; check with `static_assert(std::is_standard_layout_v<T>)` (Experiment 6) |
| **Trivially copyable** ⚖️ | `memcpy`-able | Needed for structs copied by the C side or by FFI layers |
| **`bool`** ⚖️ | A distinct type of unspecified size in the standard; 1 byte on every ABI of interest. C's `_Bool`/`bool` (C23 keyword) is also 1 byte | Don't use `bool` in a cross-language struct if the other side might disagree; `uint8_t` is unambiguous |
| **`enum`** ⚖️ | C++ unscoped enums have an implementation-chosen underlying type unless fixed (`enum E : int32_t`); C's `enum` is `int`-compatible | Fix the width with `enum class E : std::int32_t` in C++ and a `uint32_t` typedef + constants in the C header |
| **Character literals** ⚖️ | `'a'` has type `int` in C, `char` in C++ (Experiment 4) | `sizeof('a')` differs: 4 vs 1 |
| **`const` objects** ⚖️ | In C++ a `const int n = 4;` is usable in constant expressions (`int a[n];` is a fixed array); in C it is a runtime constant (and `int a[n]` is a VLA) | A "constant" in a shared header means different things |
| **`void*` conversions** ⚖️ | C converts `void*` to `T*` implicitly; C++ does not | `int* p = malloc(...)` fails to compile in C++ (Experiment 4); write `static_cast` or, better, use `new`/containers |
| **Designated initializers** ⚖️ | C99: any order, nested, array designators. C++20: aggregate designators **in declaration order**, no nesting or array designators | Experiment 4 |
| **VLAs, `restrict`, `_Complex`, flexible array members, compound literals** | C99 features; not standard C++ (compilers offer extensions) | Keep them out of headers that C++ must include |
| **`<stdatomic.h>`** ⚖️ C++23 | Provides `_Atomic(T)` macro and `atomic_*` functions compatible with `std::atomic<T>` | Lets one header declare atomics for both languages (🔧 a one-line `_Atomic(int)` + `atomic_fetch_add` program compiled and ran with GCC 14.2 and Clang 18 in `-std=c++23`) |
| **Tag namespace** ⚖️ | `struct S {...};` makes `S` a type name in C++ but only `struct S` in C | `typedef struct S S;` works in both |
| **Reserved names** ⚖️ | C++ reserves names with `__` anywhere and `_` + capital; C reserves `_` + capital/`__` | Don't name macros or identifiers `__foo` |

### Layer check

| Layer | Question |
|---|---|
| **C++ standard** | Language linkage, standard-layout, `noexcept`; says *nothing* about exceptions through C frames or the C ABI |
| **Compiler** | Name mangling (Itanium), whether `extern "C"` function types are distinct, `-fexceptions` for C code, `-fno-exceptions` for pure-C callers |
| **ABI** | SysV x86-64 calling convention (identical for C and C++), struct passing classes (INTEGER/SSE/MEMORY), unwind tables (`.eh_frame`) present in most Linux libraries; Windows has different struct-return and exception models |
| **OS / linker** | Symbol visibility, `libstdc++.so` dependency, `dlopen`/`RTLD_LOCAL`, static initialisers and `atexit` order in shared objects |

---

## 6. Implementation model

### Names

```text
 C++:  int add(int,int)                    →  _Z3addii            (mangled: name + parameter types)
 C++:  extern "C" int add(int,int)         →  add                 (unmangled)
 C:    int add(int,int)                    →  add
```

`nm -D libwordcount.so | c++filt` after `extern "C"` shows plain names such as `wc_create`; without it you see `_Z9wc_createPP10wc_counter`. A C++ file that *declares* a C function without `extern "C"` compiles but fails to link: `undefined reference to 'sqlite3_open(char const*, sqlite3**)'`.

### A trampoline is a captureless function plus a context pointer

```text
  C library              trampoline (captureless, extern "C"-compatible)         your std::function
  ─────────             ─────────────────────────────────────────────────         ──────────────────
  calls fn(user, ...) ─► static int tramp(void* user, ...) {                       lambda with captures
                            auto* f = static_cast<std::function<...>*>(user);  ─► (*f)(...)
                            try { return (*f)(...); } catch (...) { return 1; }
                          }
```

Rules: (1) the `std::function` must outlive the C call (and any later callbacks, if the library stores the pointer); (2) catch everything and translate to the C library's error convention; (3) the trampoline is a plain function, so a lambda converted with unary `+` or a `static` member function works.

### Exceptions and unwind tables

C++ exception unwinding uses `.eh_frame` data describing each function's stack frame. Modern Linux distributions compile C code with `-fasynchronous-unwind-tables` (the default on x86-64), so the unwinder *can* walk through C frames, destroying nothing there (C has no destructors) and **skipping any cleanup the C code would have done** (closing, unlocking, freeing). Where C code lacks unwind tables (`-fno-asynchronous-unwind-tables`, some embedded builds, some JIT frames), unwinding fails and `std::terminate` is called.

### Exposing C++ safely: the guard pattern

```cpp
extern "C" wc_status wc_add_text(wc_counter* c, const char* t, size_t n) {
    if (!c || (!t && n)) { set_error("bad argument"); return WC_ERR_ARG; }     // validate C inputs
    return guarded([&] { /* all the C++ here, may throw */ return WC_OK; });     // translate every exception
}
```

`guarded` has `catch (std::bad_alloc&)`, `catch (std::exception&)`, `catch (...)` arms mapping to status codes and filling a `thread_local` last-error string. One helper, applied to every exported function, makes it impossible to forget.

---

## 7. Experiments

Code is in [`code/ch45/`](code/ch45/) (`bash run.sh`). **GCC 14.2, SQLite 3 (the system library), 2-vCPU VM.**

### Experiment 1 ✅: An RAII wrapper over a real C library (SQLite)

`sqlitepp.hpp` (in the code directory, ≈90 lines) wraps `sqlite3*`, `sqlite3_stmt*`, the callback API and the error convention. The demo creates a table, inserts three rows with bound parameters, runs a query with a lambda callback, and provokes two errors and one exception:

```text
rows with score >= 80:
  ada = 90
  cy = 85
bad sql -> code 1: near "SELEKT": syntax error
open missing -> code 14: unable to open database file
callback threw -> query aborted (code 4)
```

(Clean under `-fsanitize=address,undefined`.) The five ideas to take from the header:

1. **Deleters as types.** `std::unique_ptr<sqlite3, DbDeleter>` where `DbDeleter::operator()` calls `sqlite3_close_v2`. No manual `close`, early returns are safe, and the wrapper is move-only for free.
2. **Take ownership before reading the status.** `sqlite3_open` sets `*raw` even on failure, and the handle must still be closed. The wrapper constructs `Db db(raw)` *first*, then checks `rc`. Forgetting this is the classic leak-on-error.
3. **Free what the library allocated with the library's function.** The error message from `sqlite3_exec` is released with `sqlite3_free`, not `delete`/`free` (on some platforms they use different allocators).
4. **Length-aware text.** `sqlite3_prepare_v2(…, sql.data(), sql.size(), …)` and `sqlite3_bind_text(…, v.size(), SQLITE_TRANSIENT)` avoid relying on a trailing NUL and `SQLITE_TRANSIENT` tells SQLite to copy, so a `string_view` argument may be a temporary.
5. **The trampoline** converts `std::function<bool(const vector<string>&)>` into the C callback `int(*)(void*, int, char**, char**)`, with a catch-all because a C library cannot propagate exceptions.

```cpp
// @test fail -std=c++23 err=invalid
#include <cstdlib>
int main() {
    int* p = malloc(4 * sizeof(int));     // legal C; ill-formed C++: void* has no implicit conversion to int*
    std::free(p);
}
```

### Experiment 2 ✅: Same file, two languages: the source-level differences

One C source compiled with `gcc -std=c11` and with `g++ -std=c++23 -x c++`:

```text
C   sizeof('a')=4  sizeof(enum Color)=4  sizeof(bool)=1  sizeof(1==1)=4  sizeof(struct S)=1
C   int arr[n] with const int n: variable-length array (VLA)
C++ sizeof('a')=1  sizeof(enum Color)=4  sizeof(bool)=1  sizeof(1==1)=1  sizeof(struct S)=1
C++ int arr[n] with const int n: constant expression, fixed array
```

`sizeof('a')`: 4 in C (character constants are `int`), 1 in C++; `sizeof(1==1)` likewise (the result of a comparison is `int` in C, `bool` in C++). `const int n = 4; int arr[n];` is a VLA in C and a fixed-size array in C++. The same *header* can therefore mean different things to the two compilers. Two more differences that fail to compile:

```cpp
// @test fail -std=c++23 err=designator
struct P { int x, y, z; };
P a = { .z = 3, .x = 1 };       // C99: any order. C++20: designators must follow declaration order
int main() { return a.x; }
```

(`gcc -std=c11` accepts this file; GCC's C++ mode reports `designator order for field 'P::x' does not match declaration order`.)

### Experiment 3 ✅: Throwing through a C library: "works", and corrupts state

Three runs of `throw_through_c.cpp` under AddressSanitizer:

```text
mode 0  qsort with a throwing comparator:          qsort: caught 'cmp'
mode 1  throw from a sqlite3_exec callback:        sqlite: caught 'cb'
                                                   sqlite3_close -> 5   (SQLITE_BUSY)
mode 2  callback returns 1 (polite abort):         sqlite3_close -> 0   (SQLITE_OK)
```

Two surprises, both important.

- **It did not crash.** On Linux x86-64, glibc and libsqlite3 carry unwind tables, so the exception unwound through their C frames cleanly and the `catch` in `main` ran. A test on this machine would pass.
- **It broke the library anyway.** In mode 1 the prepared statement that `sqlite3_exec` was iterating was never finalised (the C code's cleanup path never ran), so `sqlite3_close` returns `SQLITE_BUSY` and the connection cannot be closed; ASan's leak checker reports the leaked allocations. Mode 2 is the control: the same abort, done by return code, leaves the connection closable.

That is why the rule is "no exception through C frames" rather than "it crashes if you try". The failure is *silent state corruption* that tests on your machine will not show, and may be a hard `terminate` elsewhere (a library built without unwind tables, a different platform). The defensive tool is a `noexcept` boundary:

```cpp
// @test crash -std=c++23 err=terminate
#include <cstdio>
extern "C" void c_callback() noexcept { throw 1; }      // exception tries to leave a noexcept function
int main() { std::puts("calling"); c_callback(); std::puts("unreachable"); }
```

`noexcept` converts "undefined/corrupting" into "`std::terminate` immediately", a deterministic crash you find in testing (GCC even warns: `'throw' will always call 'terminate'`).

### Experiment 4 ✅: Exposing C++ through a C API, and calling it from pure C

`shim/` contains `wordcount.h` (the C header, with `extern "C"` guards), `wordcount.cpp` (the C++ implementation with a `std::map` behind an opaque `wc_counter`), and `client.c` compiled with `gcc -std=c11 -Wall -Wextra -Werror`:

```text
add_text: ok
  and    2
  bat    1
  cat    1
visit: stopped by callback
bad arg: bad argument (bad argument)
```

The library's exported surface (`nm -D --defined-only libwordcount.so`) is exactly the six functions of the header (`wc_add_text`, `wc_create`, `wc_destroy`, `wc_last_error`, `wc_status_string`, `wc_visit`) as plain unmangled `T` symbols, while `ldd` shows the shared object depends on `libstdc++.so.6`. That is the point of the pattern: **the C++ runtime is an implementation detail on the library side**; the C client doesn't link it by name in its own source, doesn't see `std::map`, and could be replaced by Python's `ctypes` with no change to the library.

Design decisions visible in the code, each worth defending in review:

| Decision | Reason |
|---|---|
| `typedef struct wc_counter wc_counter;` (opaque) | The C++ class can change (even to a different container) without breaking binaries |
| `wc_status` enum + `wc_status_string` + thread-local `wc_last_error` | No exceptions across the boundary; error text for humans |
| `wc_create(wc_counter** out)` returns status | Creation can fail; `NULL` return conflates failure causes |
| `wc_destroy(NULL)` is a no-op | Mirrors `free`; makes cleanup paths simple |
| `wc_visit(…, callback, void* user)` and the callback returns non-zero to stop | C closures are `(function, void*)` pairs; early exit is a status, not an exception |
| `guarded(lambda)` on every export | One place translates `bad_alloc`, `std::exception` and `...` |
| Validate pointer arguments | Misuse by C callers is the *norm* in C |

### Experiment 5 ✅: A struct is C-compatible only if you can say so in the compiler

```cpp
// @test compile -std=c++23
#include <cstdint>
#include <type_traits>
struct Header { std::uint32_t magic; std::uint16_t version; std::uint16_t flags; };          // meant to cross a C boundary
static_assert(std::is_standard_layout_v<Header> && std::is_trivially_copyable_v<Header>);
static_assert(sizeof(Header) == 8);
```

```cpp
// @test fail -std=c++23 err=assertion
#include <string>
struct Bad { std::string name; virtual void f(); };
static_assert(std::is_standard_layout_v<Bad>, "a class with std::string and a vptr is not a C struct");
```

Make these `static_assert`s part of every header whose types cross a boundary. Add `sizeof`/`offsetof` assertions to freeze the layout (`static_assert(offsetof(Header, version) == 4)`). A struct that contains a `std::string`, a `virtual` function, a reference or a `bool` you did not intend is not a C struct no matter what the header looks like to the eye.

---

## 8. Assembly / runtime investigation

```bash
# Symbols: mangled or not? what does the library actually export?
nm -D --defined-only libwordcount.so | c++filt           # the exported API; should be only your C functions
nm -D --undefined-only libwordcount.so | c++filt | head  # what it needs (libstdc++, libc)
objdump -T libwordcount.so | grep -c ' g '                # global symbols; compare with -fvisibility=hidden

# Is the C++ runtime a dependency? (and which version?)
ldd libwordcount.so
readelf -d libwordcount.so | grep -E 'NEEDED|SONAME'
objdump -T libwordcount.so | grep GLIBCXX | head -3         # versioned libstdc++ symbols required (Chapter 37)

# Do the C frames carry unwind tables?  (the question behind Experiment 3)
readelf -S /lib/x86_64-linux-gnu/libsqlite3.so.0 | grep -E 'eh_frame'
readelf --debug-dump=frames /lib/x86_64-linux-gnu/libc.so.6 | grep -c FDE

# Watch the C boundary at run time
ltrace -e 'wc_*' ./client_c                                 # library calls with arguments
strace -f -e trace=memory ./client_c                        # allocations behind the shim (brk/mmap)
LD_DEBUG=bindings ./client_c 2>&1 | grep wc_                # which symbol binds to which library

# Struct layout seen by C vs C++
pahole -C Header libwordcount.so                            # needs debug info
gcc  -fdump-... / g++ -fdump-lang-class -c lay.cpp          # GCC class layout dump
```

---

## 9. Implementation exercise

**Wrap zlib's streaming API.** `z_stream` is the textbook awkward C interface: a struct the *caller* allocates, initialised by `deflateInit`, driven by `deflate()` with `next_in/avail_in/next_out/avail_out`, finished by `deflateEnd` (which must be called exactly once after a successful `deflateInit`). Build:

1. `class Deflater` with a constructor that throws (or returns `expected`) on `deflateInit` failure, a destructor that calls `deflateEnd` only if init succeeded, move-only semantics (the struct contains internal pointers into itself: **moving a `z_stream` is only safe if you `memcpy` it with its address fixed**: heap-allocate it).
2. `std::vector<std::byte> compress(std::span<const std::byte>)` with a loop that grows the output.
3. Error translation: `Z_MEM_ERROR`, `Z_STREAM_ERROR`, `Z_BUF_ERROR` → an error enum with `std::string_view message()`.
4. A test comparing against `gzip`/Python's `zlib.decompress`, and an ASan run with truncated and corrupted input.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <zlib.h>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <vector>

enum class ZError { memory, stream, data, buffer, version, other };
constexpr ZError map(int rc) { switch (rc) { case Z_MEM_ERROR: return ZError::memory; case Z_STREAM_ERROR: return ZError::stream;
    case Z_DATA_ERROR: return ZError::data; case Z_BUF_ERROR: return ZError::buffer; case Z_VERSION_ERROR: return ZError::version; default: return ZError::other; } }

class Deflater {
    std::unique_ptr<z_stream, void (*)(z_stream*)> s_{nullptr, [](z_stream* z) { deflateEnd(z); delete z; }};
public:
    static std::expected<Deflater, ZError> create(int level = Z_DEFAULT_COMPRESSION) {
        Deflater d;
        auto z = std::make_unique<z_stream>();                 // zero-init the struct: zalloc/zfree/opaque = Z_NULL
        *z = {};
        if (int rc = deflateInit(z.get(), level); rc != Z_OK) return std::unexpected(map(rc));   // init failed: NO deflateEnd, delete only
        d.s_.reset(z.release());                               // from here, the deleter calls deflateEnd
        return d;
    }
    std::expected<std::vector<std::byte>, ZError> compress(std::span<const std::byte> in) {
        std::vector<std::byte> out(deflateBound(s_.get(), in.size()));          // an upper bound: one pass is enough
        s_->next_in  = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
        s_->avail_in = static_cast<uInt>(in.size());
        s_->next_out = reinterpret_cast<Bytef*>(out.data());
        s_->avail_out = static_cast<uInt>(out.size());
        if (int rc = deflate(s_.get(), Z_FINISH); rc != Z_STREAM_END) return std::unexpected(map(rc));
        out.resize(out.size() - s_->avail_out);
        return out;
    }
};
```

Notes: `deflateInit` is a *macro* that passes `sizeof(z_stream)` and the version string (an ABI check); the `z_stream` must not move after init (it holds a back-pointer in its internal state), hence the heap allocation behind the `unique_ptr`; the deleter must not call `deflateEnd` when `deflateInit` failed; inputs larger than 4 GiB need a loop because `avail_in` is a 32-bit `uInt`.

</details>

---

## 10. Real-world example

| Where | The C boundary in practice |
|---|---|
| **SQLite, zlib, OpenSSL, libcurl, libpng, FFmpeg** | C APIs with opaque handles and `*_free`/`*_destroy` functions; every modern C++ codebase wraps them in RAII (`unique_ptr` with `curl_easy_cleanup`, `EVP_MD_CTX_free`, `avformat_free_context`) |
| **POSIX / Win32** | `int fd`, `HANDLE`, `errno`/`GetLastError` as the error channel: the template for "status code + thread-local last error" (compare the file-descriptor wrapper of Chapter 4) |
| **LLVM** | A stable **C API** (`llvm-c/`) over an intentionally unstable C++ API; language bindings (Rust `inkwell`, Python `llvmlite`) use the C one |
| **Vulkan / OpenGL / OpenCL / CUDA driver API** | Handle-based C APIs with explicit create/destroy and `VkResult` status codes; `vulkan.hpp` is a header-only RAII layer on top |
| **Python extensions** | CPython's C API (`PyObject*`, reference counts) is the boundary of Chapter 46; pybind11 hides it |
| **Rust ↔ C++** | `cxx`/`bindgen`/`cbindgen` generate or consume C-compatible shims; exceptions must be caught before the boundary |
| **Qt** | Qt itself is C++-only (no C ABI), which is why Qt bindings (PyQt/PySide) are generated wrappers (`sip`, Shiboken) rather than a C layer; for plug-ins Qt uses a C++ interface with a fixed-version ABI. When you need a Qt component callable from non-C++ code, give it a small C shim as in Experiment 4 |

> **Opinion.** **Your C API is a product; design it once, carefully, and freeze it.** If a library may ever be used from another language, or from a different C++ compiler or standard library, make the **C API the primary stable interface** and layer a header-only C++ wrapper on top (the pattern of `vulkan.hpp`, `llvm-c` + LLVM bindings, ONNX Runtime, `sqlite3`). Exporting C++ classes across a DSO boundary couples every consumer to your compiler, your standard library and your class layout (Chapter 37). And when you *consume* a C library, **write the RAII wrapper before you write the third call-site**: the leak-on-error paths are exactly the code nobody tests.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Missing `extern "C"` on a C header included from C++** | `undefined reference to 'foo(int)'` | `#ifdef __cplusplus extern "C" { #endif` guards in the C header, or wrap the `#include` |
| **`extern "C"` on a template or overloaded function** | Compile error (templates can't have C linkage); overloads collide | One C function per name; type-erase or instantiate explicitly with distinct names |
| **Exception escaping an `extern "C"` function** | Silent state corruption in the C caller (Experiment 3), `terminate`, or "works on my machine" | `guarded()` wrapper; `noexcept` on the exported function and callbacks |
| **Callback captures but C API has no `void*`** | Cannot pass state; globals/thread-locals as a workaround | Wrap with a per-call static; or use a library variant with user data; prefer APIs with context |
| **Callback lifetime too short** | `std::function` destroyed while the C library still holds its address | Own the `std::function` in the wrapper object; unregister in the destructor |
| **Freeing with the wrong function/allocator** | Heap corruption (especially across DSOs or CRTs) | Always the library's own `free`; custom deleter type |
| **Ownership taken after the error check** | Leak when `open()` fails but returns a handle | Construct the RAII object first (Experiment 1) |
| **Double free via copyable wrapper** | Crash at exit | Move-only; `= delete` copy; `unique_ptr` |
| **Struct not standard-layout** | Wrong field offsets on the C side | `static_assert(is_standard_layout_v<T>)` + `offsetof` checks |
| **`bool`/`enum`/`char` width mismatch** | Garbage values across languages | Fixed-width integer types in the boundary structs |
| **`std::string`/`std::vector` in the C API** | Breaks with a different standard library/ABI (`_GLIBCXX_USE_CXX11_ABI`, Chapter 37) | `const char*` + `size_t`; caller-allocated buffers |
| **Returning `c_str()` of a local** | Dangling pointer | Return into caller-supplied buffer, or an interned/thread-local string with documented lifetime |
| **Static initialisation order in a shared object loaded by `dlopen`** | Crash in constructors, or leaks at unload | Lazy init (function-local statics); explicit `init`/`shutdown` |
| **Two copies of libstdc++** | Mismatched exceptions, `bad_alloc` types, locale state | One runtime per process; stable C boundary |
| **Using `longjmp` through C++ frames** | Skipped destructors (UB if non-trivial destructors) | Don't; or wrap with `try`/convert; use `noexcept` |
| **C++ keywords as identifiers in a C header** (`class`, `new`, `template`, `this`) | C++ compile errors | Rename fields in the shared header |
| **Variadic C functions with non-trivial C++ args** | UB (passing `std::string` to `printf`) | Pass `.c_str()`; or use `std::format` |

---

## 12. Exercises

1. **libcurl wrapper.** Wrap `CURL*` (`curl_easy_init/cleanup/setopt/perform`) with a RAII `unique_ptr`; make `setopt` type-safe for the options you use; turn `CURLcode` into `expected`; adapt the write callback to a `std::function<size_t(std::string_view)>` through a trampoline.
2. **Callback lifetimes.** Make a C library (write one: 40 lines) that stores a callback and calls it later from `lib_poll()`. Build a C++ wrapper whose objects can be moved; where does moving break the stored `void*` context, and how do you fix it (stable heap storage)?
3. **C shim for a class.** Take a small C++ class with overloads, exceptions and a `std::vector<std::string>` result; design its C API (handles, status, iteration functions `next()`), implement it, and call it from C and from Python `ctypes`.
4. **Header portability.** Write a header that compiles as C99, C23, C++17 and C++23 with identical meaning (`bool`, `enum` width, `static_assert`/`_Static_assert`, `nullptr`/`NULL`, `[[nodiscard]]`/`__attribute__`). Test it with all four.
5. **Unwind tables.** Compile a C library with `-fno-asynchronous-unwind-tables -fno-exceptions`; repeat Experiment 3. What happens now?
6. **Layout checks.** Write a macro-free `static_assert` suite for five structs that cross a boundary: sizes, offsets, trivially-copyable, standard-layout. Break each one deliberately (add a `virtual`, add a `std::string`, reorder fields) and read the diagnostics.
7. **Error design.** Compare three error conventions for a C API: status codes + last-error; `errno`-style; out-parameter struct `{code, message}` per call. Implement one function in each; discuss thread-safety and ergonomics.
8. **ABI break hunt.** Take a C API with a public struct (`typedef struct { int a; int b; } cfg_t;`). Show three changes that break binary compatibility, and refactor to an opaque handle plus getters/setters that survive them.

---

## 13. Challenge: a C-ABI plug-in system

Design and implement a plug-in system where the host is C++ and plug-ins are shared objects written in C or C++ **by third parties with possibly different compilers**. Specify the C ABI: a `plugin_get_api(uint32_t host_version)` entry returning a table of function pointers (versioned, with `struct_size` as the first member to allow additive evolution), opaque instance handles, a host-callback table passed in at init, and status-code error handling. Implement a header-only C++ wrapper on each side (`PluginHost`, `PluginBase`) so that authors write idiomatic C++ but only C types cross the boundary. Load plug-ins with `dlopen(RTLD_LOCAL)`, enforce `noexcept` trampolines, and write tests with a deliberately misbehaving plug-in (throws, returns an error, crashes, leaks). Document the upgrade path: how a v2 host loads a v1 plug-in and vice versa.

---

## 14. Knowledge check

1. What exactly does `extern "C"` change, and what does it *not* change?
2. What are the three levels of C/C++ compatibility, with an example failure at each?
3. Name four differences between C and C++ that can make the same header mean different things (or not compile).
4. Why must you take ownership of a handle before checking the status code in a `*_open` wrapper?
5. How do you adapt a capturing lambda to a C callback `(fn, void*)` interface? What must you guarantee about lifetimes?
6. What did Experiment 3 show about throwing through C frames, and why is "it worked on my machine" insufficient?
7. Why use an opaque handle instead of exposing a struct in a C API?
8. What does a C++ `guarded()` boundary helper do, and why exactly once per exported function?
9. Why is `std::string` in a C API a bad idea even if both sides are C++?
10. What does `static_assert(std::is_standard_layout_v<T>)` check, and what else would you assert for a boundary struct?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. It gives the declared function/variable C language linkage: no name mangling (so no overloading, one entity per name). It does not change the calling convention on SysV x86-64, does not verify that types are C-compatible, and does not prevent exceptions.
2. Source: `int* p = malloc(...)` doesn't compile as C++. Link: a C function declared without `extern "C"` gives `undefined reference to 'f(int)'`. ABI: struct layout/`bool`/enum width mismatch, or an exception unwinding through C frames.
3. Implicit `void*` conversion; character literal type (`sizeof('a')` 4 vs 1); `const int` as a constant expression (VLA vs fixed array); designated-initializer order/nesting; VLAs, `restrict`, compound literals; tag namespace (`struct S` vs `S`); comparison result type.
4. Some APIs (SQLite) return a valid handle even on failure and require you to close it; checking the status first and returning early leaks it.
5. Pass the address of the `std::function` (or a context struct) as the `void*` user pointer and a captureless trampoline that casts it back, calls it inside a catch-all, and returns an error code. The `std::function` must outlive every possible callback invocation (until the C library can no longer call it).
6. It worked, because libc/SQLite carry unwind tables, but the connection could no longer be closed (`SQLITE_BUSY`) and memory leaked, since the C library's cleanup code was skipped. Other platforms or builds without unwind tables would terminate; either way the behaviour is not guaranteed.
7. The layout can then change without breaking binaries; callers can't depend on or corrupt internals; allocation and construction happen on the library's side.
8. Wraps the body in `try/catch(...)`, translating each exception type to a status code and recording a thread-local message, so no exception crosses `extern "C"`. Once per function so that none is forgotten; a helper keeps it uniform.
9. `std::string`'s layout and allocator depend on the compiler, standard library version and ABI macros (e.g. `_GLIBCXX_USE_CXX11_ABI`); mixing builds breaks silently. `const char*` + length is stable.
10. That the type follows C-like layout rules (no virtuals, uniform access control, etc., first member at offset 0). Also assert trivially-copyable, `sizeof`, and `offsetof` of each field, with fixed-width member types.

</details>

---

[← Previous: Chapter 44](../part-16-library-design/44-design-patterns-revisited.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 46 — C++ and Python →](46-python-interop.md)
