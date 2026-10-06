# Chapter 3 — Lifetime and Storage

> **Part II · Object model and lifetime** &nbsp;|&nbsp; **Level 2** (language mechanisms) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 2](02-object-model.md) &nbsp;|&nbsp; **Standards:** C++11 → C++26 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, ASan, UBSan, `nm`, `readelf`

[← Previous: Chapter 2](02-object-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 4 — RAII →](04-raii.md)

---

**In one sentence:** *storage duration* says how long the **bytes** exist, *lifetime* says how long the **object** exists, and almost every dangling reference, static-initialization bug and double-free in C++ is a mismatch between the two.

**By the end of this chapter you can:**

- name the four storage durations and, for each, say precisely when construction and destruction happen
- predict the exact destruction order of members, bases, temporaries, statics and thread-locals
- apply the lifetime-extension rules for temporaries, and say which tempting cases they do **not** cover
- explain why destruction order of globals bites, and fix it
- write exception-safe code that constructs objects into raw memory (`construct_at`, `uninitialized_*`)
- read the assembly the compiler emits for lifetime machinery (guard variables, TLS, `__cxa_atexit`)

---

## 1. Problem

Consider four lines that all look innocent:

```cpp
const std::string& name = get_user().name();          // is get_user()'s temporary still alive?
for (char c : make_config().path()) { /* … */ }       // and here?
static Logger log;                                    // who destroys it, and *when*, relative to other statics?
std::thread t([&]{ use(local); });                    // is `local` still alive when the thread runs?
```

Every one of these compiles. Some are fine. Some are undefined behaviour that passes testing for years. To tell them apart you need two separate pieces of knowledge:

- **How long does the storage exist?** (storage duration: *automatic, static, thread, dynamic*, plus *temporaries*)
- **How long does the object exist?** (lifetime: from completed construction to start of destruction)

The two are *different clocks*. Storage can outlive the object (a `std::vector`'s spare capacity; a stack slot after the destructor ran). The object **must not** outlive its storage. A reference or pointer is valid only while the **object** lives, and the compiler is entitled to assume you never use one after that.

---

## 2. Historical context

| Era | What existed | What went wrong |
|---|---|---|
| **C** | `auto`, `static`, `register`, `malloc`/`free`. No destructors, so lifetime ≈ storage | Dangling pointers to locals; leaks; but no *ordering* problems, because nothing runs automatically |
| **C++98** | Constructors and destructors; **temporaries**; `const T&` binding extends a temporary | Static-initialization-order fiasco; temporaries die at the semicolon, in ways that surprise |
| **C++11** | `thread_local`; function-local statics become **thread-safe**; rvalue references (`T&&` also extends); `constexpr` | Threads add a fourth storage duration and new destruction-order questions |
| **C++17** | **Temporary materialization** (a prvalue is not an object until needed); guaranteed elision; inline variables | Fewer temporaries than before, but the *rules* for when one exists got subtler |
| **C++20** | `constinit`; `construct_at`/`destroy_at`, constexpr dynamic allocation; parenthesized aggregate init (which does **not** extend lifetimes) | |
| **C++23** | Range-`for` fix (P2718R0): temporaries in the range-initializer live for the whole loop. Support: **GCC 15, Clang 19** | The *old* behaviour is still in GCC 14, which this course uses |
| **C++26** | 🆕 Reading an uninitialized automatic is **erroneous behaviour**, not UB (P2795R5). Returning a reference bound to a temporary becomes **ill-formed** (P2748R5) | Compiler support is arriving (GCC 16 first) |

The pattern: C++ keeps adding *tools to make lifetime explicit* (RAII, smart pointers, `construct_at`) and, slowly, fixing the corners where the rules were more surprising than useful.

---

## 3. Modern solution

There is no single mechanism. There is a **discipline** with four parts, each backed by language rules:

1. **Bind every resource to a scope.** Automatic storage + a destructor gives deterministic cleanup. This is RAII, and the whole of [Chapter 4](04-raii.md).
2. **Make ownership visible in types**, so that "who ends this lifetime?" has an answer in the signature ([Chapters 12 and 25](../part-05-standard-library/12-views-and-non-owning-types.md)).
3. **Construct on first use** for globals, so that dependencies are alive when you need them.
4. **When you leave the safe path** (raw storage, placement new), use the standard's *tools* (`construct_at`, `destroy_at`, `uninitialized_*`, `start_lifetime_as`, `bit_cast`) rather than casts, so that the compiler and sanitizers know what you mean.

And *tooling*: ASan (`stack-use-after-scope`, `heap-use-after-free`), `-Wdangling-reference`, Clang's `[[clang::lifetimebound]]`, and ([Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md)) the full UB-detection toolbox.

---

## 4. Mental model

Draw **two timelines** for every object: one for its *storage*, one for its *lifetime*.

```text
 storage    ├───────────────────────────────────────────────────────────────┤
 (bytes)    allocated                                                    released
                          ├───────────────────────────────┤
 lifetime                 ctor completes              dtor starts
 (object)
            └─ no object ─┘└──── object may be used ───────┘└─ no object ───┘
```

The four storage durations are four *rules for the outer timeline*:

| Duration | Storage begins | Object lifetime begins | Object lifetime ends | Storage ends |
|---|---|---|---|---|
| **Automatic** (locals, parameters, temporaries) | scope entry | when the declaration's initialization completes | scope exit, **reverse order of construction** | scope exit |
| **Static** (globals, `static` locals, `static` members) | program start | before `main`, **or** on first pass for function-local statics | after `main`, **reverse order of completion of construction** | program end |
| **Thread** (`thread_local`) | thread start | per thread: on first use or at thread start (implementation-defined) | thread exit | thread exit |
| **Dynamic** (`new`, `malloc`, allocators) | `operator new` | when construction completes | `delete` / `destroy_at` | `operator delete` |

Two rules to memorise, because they explain most of the rest:

> **Destruction is the mirror of construction**: reverse order, always, at every level (members, bases, locals, statics).
> **A temporary lives to the end of its full-expression**, unless something *extends* it.

---

## 5. Language rules

### 5.1 Construction and destruction order  `[class.base.init]`

When you construct a class object:

1. **Virtual base classes**, depth-first left-to-right order of declaration.
2. **Direct base classes**, in declaration order.
3. **Non-static data members**, in **declaration order**. (The order of the *mem-initializer list* is irrelevant; `-Wreorder` warns when it disagrees.)
4. The **constructor body** runs.

Destruction is exactly the reverse: destructor body, then members (reverse declaration order), then bases (reverse).

Consequences worth knowing:

- **Members' lifetimes begin before the constructor body**, so the body may use them. If a member's constructor throws, already-constructed members and bases are destroyed in reverse order, and the object's **own destructor does not run**, because the object never finished construction.
- With **delegating constructors**, the object is considered constructed as soon as *any* constructor completes. An exception thrown from the *delegating* constructor's body therefore **does** run the destructor.
- **Virtual calls in constructors and destructors** resolve to the version in the class *currently being constructed* (or destroyed), not the most-derived one. The derived part does not exist yet (or no longer does).

### 5.2 Temporaries  `[class.temporary]`

Since C++17 a prvalue is **not** an object until it must be. A temporary object comes into being at **temporary materialization**: when a prvalue is bound to a reference, when a member is accessed on it, when it is converted to an xvalue ([Chapter 5](../part-03-value-categories-and-move/05-value-categories.md)).

Temporaries are destroyed at the end of the **full-expression** that contains them, in reverse order of creation. A *full-expression* is, roughly, everything up to the semicolon (or the controlling expression of an `if`/`while`, or an initializer).

### 5.3 Lifetime extension of temporaries  `[class.temporary]`

If a temporary is bound to a **reference**, its lifetime is extended to match the reference. This applies to a reference variable (`const T& r = f();`, `T&& r = f();`) and to a **member of a prvalue** (`const int& r = f().x;` extends the *whole* temporary).

It does **not** apply when the reference does not *directly* bind the temporary:

| Situation | Extended? | Why |
|---|---|---|
| `const T& r = f();` | ✅ yes | Direct binding |
| `const int& r = f().member;` | ✅ yes | Subobject of a temporary |
| `Holder h{f()};` where `Holder` has a `const T&` member (**list**-init of an aggregate) | ✅ yes | Aggregate init binds directly |
| `const T& r = id(f());` where `id` returns its argument by reference | ❌ no | The temporary is bound to the **parameter**; it dies at the end of the full-expression |
| `const T& r = f().get();` where `get()` returns a reference | ❌ no | Same: the reference came *out of a call* |
| `Holder h(f());` (**parenthesized** aggregate init, C++20) | ❌ no | Only list-initialization extends |
| `new Holder{f()}` | ❌ no | Dies at the end of the full-expression of the `new` |
| `return f();` from a function returning `const T&` | ❌ no (🆕 ill-formed in C++26) | Dies at the `return`; always dangling |
| Range-`for` over `f().items()` | ❌ no, **until C++23's fix** | Only the *range* reference is extended, not temporaries inside the initializer |

> [!WARNING]
> "A temporary bound to a `const&` lives as long as the reference" is only true for **direct** binding. Any time the reference comes *out of a function call*, nothing is extended.

### 5.4 Function parameters  `[expr.call]`

> It is **implementation-defined** whether the lifetime of a parameter ends when the function returns or at the end of the enclosing full-expression.

Itanium (GCC, Clang on Linux): the **caller** destroys parameters, at the end of the full-expression. MSVC: the **callee** destroys them, at return. This is a *standard vs ABI* distinction that **changes observable behaviour** (Experiment 3). It is one reason to avoid depending on the order in which parameters are destroyed.

### 5.5 Static and thread storage  `[basic.start.static]`, `[basic.start.dynamic]`, `[stmt.dcl]`

Initialization of a static-storage object happens in phases:

```text
 1. zero-initialization                         always, first
 2. constant initialization                     if the initializer is a constant expression (guaranteed with constinit)
 ── static initialization finished; no code ran ──
 3. dynamic initialization                      otherwise, at program start (namespace scope) or first control pass (function-local)
```

| Kind | Initialization order |
|---|---|
| Non-inline variables, **same** translation unit | Ordered by definition |
| Variables in **different** translation units | **Unspecified.** The *static initialization order fiasco* |
| `inline` variables and static members of templates | Partially ordered / unordered |
| Function-local `static` | On **first** execution of the declaration; **thread-safe** since C++11 (a concurrent first call blocks the others) |
| `thread_local` | Implementation-defined between thread start and first odr-use |

**Destruction** of static objects occurs after `main` returns (or on `std::exit`), in **reverse order of completion of their construction**, and that includes function-local statics. This is the rule that bites (§10).

`std::exit` runs static destructors but **not** automatic ones. `std::quick_exit` and `std::_Exit` skip static destructors; `std::abort` skips everything. Throwing out of a `noexcept` function calls `std::terminate`, which by default calls `std::abort`, so *no destructors at all*.

### 5.6 Dynamic storage and raw memory  `[basic.stc.dynamic]`, `[specialized.algorithms]`

Dynamic storage is allocated and released explicitly ([Chapter 24](../part-10-memory/24-dynamic-memory.md)). Objects are created in it with a *new-expression* or, when storage already exists, with **placement new** or `std::construct_at`; they are ended with `std::destroy_at` (or an explicit `~T()` call). For ranges of objects the standard offers the **`<memory>` specialized algorithms**:

| Algorithm | Does | Exception behaviour |
|---|---|---|
| `std::uninitialized_copy(first, last, dst)` | copy-constructs into raw storage | **rolls back** (destroys what it built) if a constructor throws |
| `std::uninitialized_move`, `_fill`, `_default_construct`, `_value_construct` | likewise | likewise |
| `std::destroy(first, last)`, `std::destroy_n`, `std::destroy_at` | calls destructors, frees nothing | — |
| `std::construct_at(p, args…)` | `::new (voidify(*p)) T(args…)`; **constexpr** since C++20 | exception propagates; storage stays uninitialized |

That rollback is the whole reason these exist: it is the **basic guarantee for bulk construction**, and you will rewrite it by hand in §9.

### 5.7 Reading bytes as a different type: `std::bit_cast`  `[bit.cast]`

`std::bit_cast<To>(from)` (C++20) creates a `To` whose object representation equals that of `from`. Requirements: `sizeof(To) == sizeof(From)` and both trivially copyable. It is `constexpr`, creates a **new object** (a copy), and has no aliasing or lifetime problems. It is the correct replacement for union punning and `reinterpret_cast`ing value types.

### 5.8 Uninitialized variables  🆕

Reading an **indeterminate** value (for example an uninitialized `int` local) is **undefined behaviour** through C++23. In **C++26**, an uninitialized automatic variable instead holds an *erroneous value*: reading it is **erroneous behaviour**, which is well-defined but incorrect, and implementations are encouraged to diagnose it (P2795R5). You can opt out per variable with `[[indeterminate]]`. GCC 16 is first to implement it.

> [!NOTE]
> Erroneous behaviour is not a licence to leave variables uninitialized. It converts a *silent optimizer hazard* into a *diagnosable bug*, which is a big deal, but the code is still wrong.

---

## 6. Implementation model

How the compiler realises each storage duration on x86-64 Linux (Itanium ABI):

| Concept | Implementation |
|---|---|
| **Automatic** | A slot in the stack frame (or a register). "Allocation" is the frame's `sub rsp, N`. *Destruction* is an explicit call at scope exit **plus** an *exception cleanup landing pad* for the unwinder ([Chapter 22](../part-09-error-handling/22-exceptions.md)) |
| **Temporary** | A stack slot, destroyed at a sequence point at the end of the full-expression |
| **Static, constant-initialized** | Bytes in `.data` (or `.rodata`); **no code runs**. The loader maps them |
| **Static, zero-initialized** | `.bss` (no bytes in the file) |
| **Static, dynamically initialized** | An initializer function `_GLOBAL__sub_I_…`, listed in **`.init_array`**; the loader/CRT calls it before `main` |
| **Function-local static** | A hidden **guard variable** (`_ZGV…`) and calls to `__cxa_guard_acquire` / `__cxa_guard_release`; fast path is "load the guard byte and compare" |
| **Destructor of a static** | Registered at construction time via **`__cxa_atexit`**; run in reverse registration order at exit |
| **`thread_local`** | A TLS segment (`.tdata`/`.tbss`), addressed through `%fs`; non-trivial ones add a TLS-init guard and register their destructor with **`__cxa_thread_atexit`** |
| **Dynamic** | `operator new` → `malloc` → the allocator → `brk`/`mmap` |

### Layer check

| Layer | Who decides what |
|---|---|
| **Standard** | Lifetime rules; extension rules; static init phases; reverse destruction; thread-safe local statics |
| **Compiler** | Guard variables, landing pads, where a temporary's slot lives; warnings (`-Wdangling-reference`) |
| **ABI (Itanium)** | **When parameters are destroyed** (caller, end of full-expression); the guard-variable protocol (`__cxa_guard_*`); `__cxa_atexit` |
| **OS / loader** | Maps `.data`/`.bss`; runs `.init_array` and `.fini_array`; provides TLS (`fs`-based) |
| **CPU** | Nothing special; stack and TLS are ordinary memory. The guard check is one predictable load and branch |

---

## 7. Experiments

### Experiment 1: Construction order, destruction order, and an exception in a constructor

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <stdexcept>

struct Noisy {
    const char* n;
    explicit Noisy(const char* name) : n(name) { std::printf("  ctor %s\n", n); }
    ~Noisy() { std::printf("  dtor %s\n", n); }
};

struct Base {
    Noisy b{"Base.member"};
    Base()  { std::puts("  Base body"); }
    ~Base() { std::puts("  ~Base body"); }
};

struct Derived : Base {
    Noisy second{"Derived.second"};            // declared FIRST...
    Noisy first;                               // ...declared second
    Derived() : first("Derived.first") {       // the initializer list order is irrelevant
        std::puts("  Derived body");
    }
    ~Derived() { std::puts("  ~Derived body"); }
};

struct ThrowsOnConstruction {
    explicit ThrowsOnConstruction(const char*) {
        std::puts("  ThrowsOnConstruction ctor throws");
        throw std::runtime_error("boom");
    }
};

struct Fails : Base {
    Noisy a{"Fails.a"};
    ThrowsOnConstruction m{"x"};               // the second member throws
    Noisy z{"Fails.z"};                        // never constructed
    Fails()  { std::puts("  Fails body (never reached)"); }
    ~Fails() { std::puts("  ~Fails body (never runs)"); }
};

int main() {
    std::puts("== construct and destroy a Derived");
    { Derived d; }

    std::puts("== a member constructor throws");
    try { Fails f; } catch (const std::exception& e) { std::printf("  caught: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
== construct and destroy a Derived
  ctor Base.member
  Base body
  ctor Derived.second
  ctor Derived.first
  Derived body
  ~Derived body
  dtor Derived.first
  dtor Derived.second
  ~Base body
  dtor Base.member
== a member constructor throws
  ctor Base.member
  Base body
  ctor Fails.a
  ThrowsOnConstruction ctor throws
  dtor Fails.a
  ~Base body
  dtor Base.member
  caught: boom
```

Read the first block top to bottom: base first, then members **in declaration order** (`second` before `first`, whatever the initializer list says), then the body. Destruction is the exact mirror.

The second block is the exception-safety foundation: `Fails.a` and the whole `Base` are destroyed because they *were* constructed; `Fails.z` is not touched because it never was; and **`~Fails` never runs**, because a `Fails` object never came to exist. This is why a class holding **one** raw resource per member, each wrapped in its own RAII type, is automatically exception-safe, and why a constructor that grabs two raw resources manually is a leak waiting to happen ([Chapter 4](04-raii.md)).

### Experiment 2: When does a temporary die?

```cpp
// @test run -std=c++23 -O0 -Wno-dangling-reference
#include <cstdio>

struct T {
    const char* n;
    int x = 1;
    explicit T(const char* name) : n(name) { std::printf("    ctor %s\n", n); }
    T(const T& o) : n(o.n) { std::printf("    copy %s\n", n); }
    ~T() { std::printf("    dtor %s\n", n); }
    const T& self() const { return *this; }
};
T make(const char* n) { return T(n); }          // prvalue: guaranteed elision, no copy
const T& id(const T& t) { return t; }
struct Holder { const T& r; };

#define DONE std::puts("    [statement finished]")
#define END  std::puts("    [end of scope]")

void a() { std::puts("a) discarded temporary");                          make("a");                   DONE; END; }
void b() { std::puts("b) bound to const&");                              const T& r = make("b");      DONE; END; (void)r; }
void c() { std::puts("c) bound to a MEMBER of a temporary");             const int& r = make("c").x;  DONE; END; (void)r; }
void d() { std::puts("d) passed through id(), which returns const&");    const T& r = id(make("d"));  DONE; END; (void)r; }
void e() { std::puts("e) through a member function returning const&");   const T& r = make("e").self(); DONE; END; (void)r; }
void f() { std::puts("f) aggregate LIST-init of a reference member");     Holder h{make("f")};         DONE; END; (void)h; }
void g() { std::puts("g) aggregate PAREN-init of a reference member");    Holder h(make("g"));         DONE; END; (void)h; }

int main() { a(); b(); c(); d(); e(); f(); g(); }
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a) discarded temporary
    ctor a
    dtor a
    [statement finished]
    [end of scope]
b) bound to const&
    ctor b
    [statement finished]
    [end of scope]
    dtor b
c) bound to a MEMBER of a temporary
    ctor c
    [statement finished]
    [end of scope]
    dtor c
d) passed through id(), which returns const&
    ctor d
    dtor d
    [statement finished]
    [end of scope]
e) through a member function returning const&
    ctor e
    dtor e
    [statement finished]
    [end of scope]
f) aggregate LIST-init of a reference member
    ctor f
    [statement finished]
    [end of scope]
    dtor f
g) aggregate PAREN-init of a reference member
    ctor g
    dtor g
    [statement finished]
    [end of scope]
```

The position of `dtor` relative to the two bracketed markers is the answer:

- **dtor before `[statement finished]`**: the temporary died at the semicolon.
- **dtor after `[end of scope]`**: it was extended to the lifetime of the reference.

Cases **d**, **e** and **g** die at the semicolon, leaving `r` and `h.r` dangling. All three compile without error, and the compiler can warn about only some of them (`-Wdangling-reference` flags **d** and **e**, for GCC 13+; remove `-Wno-dangling-reference` and look). Case **g** differs from **f** only in brace-vs-paren. It was a deliberate C++20 decision, since parenthesized aggregate initialization is *not* list-initialization.

Both GCC and Clang agree on every line. This is **standard** behaviour, not implementation behaviour.

### Experiment 3: Parameter destruction time is implementation-defined

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

struct Noisy {
    const char* n;
    explicit Noisy(const char* name) : n(name) { std::printf("  ctor %s\n", n); }
    ~Noisy() { std::printf("  dtor %s\n", n); }
};

int g(Noisy n) { std::printf("  inside g(%s)\n", n.n); return 7; }

int main() {
    std::printf("  result printed: %d\n", g(Noisy("param")));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  ctor param
  inside g(param)
  result printed: 7
  dtor param
```

On Linux the parameter's `dtor` appears **after** `result printed`: the caller destroys it at the end of the full-expression, which includes the `printf`. On MSVC the callee destroys it, so `dtor` would print *before* `result printed`. Both are conforming. If your code's correctness depends on the order, it is wrong on one of the two ABIs. The cure is never to put an order-sensitive side effect in a destructor of an object passed by value.

### Experiment 4: A temporary that dies in the middle of a range-`for`

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=stack-use-after-scope lines=12
#include <cstdio>
#include <string>

struct Owner {
    std::string name = "a-reasonably-long-string-to-force-a-heap-allocation-xxxxxxxx";
    const std::string& get() const { return name; }   // returns a reference INTO the object
};
Owner make() { return Owner{}; }

int main() {
    int n = 0;
    for (char c : make().get()) {   // the temporary Owner dies before the first iteration (until P2718R0)
        n += c;
    }
    std::printf("%d\n", n);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==1509==ERROR: AddressSanitizer: stack-use-after-scope on address 0x7f9da7900060 at pc 0x55cc44bb53b1 bp 0x7ffd38bda210 sp 0x7ffd38bda200
READ of size 8 at 0x7f9da7900060 thread T0
    #0 0x55cc44bb53b0 in std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >::_M_data() const /usr/include/c++/14/bits/basic_string.h:228
    #1 0x55cc44bb546b in std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >::begin() const /usr/include/c++/14/bits/basic_string.h:970
    #2 0x55cc44bb4677 in main /tmp/snip-3zv0kwag/snippet.cpp:13
    #3 0x7f9da982a1c9 in __libc_start_call_main ../sysdeps/nptl/libc_start_call_main.h:58
    #4 0x7f9da982a28a in __libc_start_main_impl ../csu/libc-start.c:360
    #5 0x55cc44bb4364 in _start (/tmp/snip-3zv0kwag/snippet+0x2364) (BuildId: 44e4aac936926ad073fbe3987633f24c4bf381bf)

Address 0x7f9da7900060 is located in stack of thread T0 at offset 96 in frame
    #0 0x55cc44bb454f in main /tmp/snip-3zv0kwag/snippet.cpp:11
... (truncated)
```

`make()` produces a temporary `Owner`. `.get()` returns a reference into it. The range-`for` binds `auto&& __range = make().get();` and **only that reference** is extended, which is a reference to a *string that lives inside a temporary that has just died*. ASan catches it. Without ASan, GCC and Clang silently read freed stack and print a plausible number, and this passes tests for years.

> [!NOTE]
> **Fixed in C++23 (P2718R0), implemented in GCC 15 and Clang 19**: all temporaries in a range-`for` range-initializer now live for the whole loop, and this program becomes well-defined. This course's GCC 14 still has the old behaviour, which is why the sanitizer fires. Know both: you will meet code compiled by either.

Clang's `[[clang::lifetimebound]]` lets you mark such accessors so the compiler *diagnoses* the misuse:

```cpp
// @test fail -std=c++23 -Werror=dangling cxx=clang++-18 err=dangling
#include <string>

const std::string& pick([[clang::lifetimebound]] const std::string& s) { return s; }

int main() {
    const std::string& r = pick(std::string("temporary"));   // error: temporary bound to local reference
    return static_cast<int>(r.size());
}
```

### Experiment 5: Static destruction order, and the fix

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

struct Logger {
    Logger()  { std::puts("  Logger constructed"); }
    ~Logger() { std::puts("  Logger destroyed"); }
    void log(const char* m) { std::printf("  log: %s\n", m); }
};
Logger& logger() { static Logger l; return l; }        // construct on first use

struct RegistryBad {                                    // does NOT touch logger() when constructed
    RegistryBad()  { std::puts("  RegistryBad constructed"); }
    ~RegistryBad() { std::puts("  RegistryBad destroyed   (calling logger() here would be a use-after-destroy)"); }
};

struct RegistryGood {                                   // touches logger() FIRST
    RegistryGood()  { logger(); std::puts("  RegistryGood constructed"); }
    ~RegistryGood() { logger().log("RegistryGood says goodbye"); }
};

RegistryBad  g_bad;     // globals, constructed before main
RegistryGood g_good;

int main() {
    std::puts("main: start");
    logger().log("hello from main");
    std::puts("main: end");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  RegistryBad constructed
  Logger constructed
  RegistryGood constructed
main: start
  log: hello from main
main: end
  log: RegistryGood says goodbye
  Logger destroyed
  RegistryBad destroyed   (calling logger() here would be a use-after-destroy)
```

The rule is **destruction in reverse order of *completion* of construction**. Trace it: `g_bad` completed first, then the `Logger` (finished *inside* `RegistryGood`'s constructor), then `g_good`. Reverse: `g_good`, `Logger`, `g_bad`. So `RegistryGood`'s destructor may use the logger, and `RegistryBad`'s may not: the logger is already gone.

The fix is a pattern, not a feature: **a global that depends on another must touch it in its constructor**. This makes the dependency complete construction first and therefore die later. For cross-translation-unit globals (where you cannot even control construction order), prefer *function-local statics*, and consider the *leaky singleton* (`static auto& l = *new Logger;`), which is never destroyed and so can never be used after destruction, at the price of the leak being intentional.

### Experiment 6: What the compiler emits for static and thread storage

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=lazy_counter,const_counter,read_tl
int compute();

int& lazy_counter()  { static int v = compute(); return v; }   // dynamic initialization, thread-safe
int& const_counter() { static int v = 42;        return v; }   // constant initialization
thread_local int tl_plain = 5;                                  // constant-initialized thread_local
int read_tl() { return tl_plain; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
lazy_counter():
	movzx	eax, BYTE PTR guard variable for lazy_counter()::v[rip]
	test	al, al
	je	.L16
	lea	rax, lazy_counter()::v[rip]
	ret
.L16:
	push	r14
	push	rbx
	lea	rbx, guard variable for lazy_counter()::v[rip]
	mov	rdi, rbx
	sub	rsp, 8
	call	__cxa_guard_acquire@PLT
	test	eax, eax
	jne	.L17
	add	rsp, 8
	lea	rax, lazy_counter()::v[rip]
	pop	rbx
	pop	r14
	ret
.L17:
	call	compute()@PLT
	mov	rdi, rbx
	mov	DWORD PTR lazy_counter()::v[rip], eax
	call	__cxa_guard_release@PLT
	add	rsp, 8
	lea	rax, lazy_counter()::v[rip]
	pop	rbx
	pop	r14
	ret
.L6:
	mov	r14, rax
	jmp	.L5

lazy_counter() [clone .cold]:
.L5:
	mov	rdi, rbx
	call	__cxa_guard_abort@PLT
	mov	rdi, r14
	call	_Unwind_Resume@PLT

const_counter():
	lea	rax, const_counter()::v[rip]
	ret

read_tl():
	mov	eax, DWORD PTR fs:tl_plain@tpoff
	ret
```

Three different costs, three different storage-initialization strategies:

- **`const_counter`** is a bare `lea`. The value `42` is baked into `.data`, so there is **no code and no guard**. Constant initialization is free, and this is why `constexpr`/`constinit` matter for globals.
- **`lazy_counter`** loads the **guard byte** and returns if it is set (the hot path: one load, one compare, one branch). On first use it calls `__cxa_guard_acquire`, runs `compute()`, then `__cxa_guard_release`. The `.cold` clone calls `__cxa_guard_abort` if `compute()` throws, so that a later call may retry.
- **`read_tl`** is a single `mov` from `%fs:tl_plain@tpoff`: TLS access is as cheap as a global when the variable is constant-initialized.

The symbol table of the object file confirms the machinery (`nm -C`, with annotations added). Here it was run on both snippets of this experiment together:

```text
$ nm -C lifetime.o
0000000000000008 b guard variable for lazy_counter()::v      ← the guard byte, in .bss
0000000000000010 b lazy_counter()::v                          ← the variable itself: zero-initialized .bss
0000000000000004 d const_counter()::v                         ← 'd' = .data: the 42 is baked in, no guard
                 U __cxa_guard_acquire                        ← the ABI runtime that makes local statics thread-safe
                 U __cxa_guard_release
                 U __cxa_guard_abort
                 U __cxa_thread_atexit                        ← registers destructors of thread_local objects
0000000000000000 b __tls_guard                                ← per-thread "has TLS been initialized?" flag
0000000000000001 B tl_big                                     ← .tbss: thread-local storage, zero-initialized
0000000000000000 D tl_plain                                   ← .tdata: thread_local with a constant initializer
0000000000000000 B g_dyn                                      ← zero-initialized until its initializer runs
0000000000000000 D g_const                                    ← constinit: the 7 is already in .data
0000000000000000 t _GLOBAL__sub_I__Z12lazy_counterv           ← the dynamic initializer, listed in .init_array
```

Now a `thread_local` with a *constructor and destructor* (`Big`), and a namespace-scope global with a dynamic initializer, and one with `constinit`:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=get_tl,_GLOBAL__sub_I
int compute();
struct Big { Big(); ~Big(); };

thread_local Big tl_big;                  // dynamic initialization + destructor, per thread
Big& get_tl() { return tl_big; }

int g_dyn = compute();                    // dynamic initializer: a function in .init_array runs before main
constinit int g_const = 7;                // guaranteed constant initialization: just bytes in .data
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
get_tl():
	push	rbp
	push	rbx
	sub	rsp, 8
	cmp	BYTE PTR fs:__tls_guard@tpoff, 0
	mov	rbx, QWORD PTR fs:0
	je	.L6
	add	rsp, 8
	lea	rax, tl_big@tpoff[rbx]
	pop	rbx
	pop	rbp
	ret
.L6:
	mov	BYTE PTR fs:__tls_guard@tpoff, 1
	lea	rbp, tl_big@tpoff[rbx]
	mov	rdi, rbp
	call	Big::Big()@PLT
	mov	rdi, QWORD PTR Big::~Big()@GOTPCREL[rip]
	mov	rsi, rbp
	lea	rdx, __dso_handle[rip]
	call	__cxa_thread_atexit@PLT
	add	rsp, 8
	lea	rax, tl_big@tpoff[rbx]
	pop	rbx
	pop	rbp
	ret

_GLOBAL__sub_I_tl_big:
	sub	rsp, 8
	call	compute()@PLT
	mov	DWORD PTR g_dyn[rip], eax
	add	rsp, 8
	ret
```

`get_tl` checks `__tls_guard` first; on the first access in a thread it constructs `tl_big` and registers the destructor through `__cxa_thread_atexit`. `g_dyn` needs a whole initializer function, `_GLOBAL__sub_I…`, that the loader runs before `main`, **in an order you do not control across translation units**. `g_const` generates *nothing*. `constinit` is a promise the compiler checks: if the initializer were not a constant expression, it is a compile error rather than a silent ordering bug.

### Experiment 7: `bit_cast`, `memcpy`, union: same machine code, different legality

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=bits_
#include <bit>
#include <cstdint>
#include <cstring>

std::uint32_t bits_bitcast(float f) { return std::bit_cast<std::uint32_t>(f); }

std::uint32_t bits_memcpy(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

std::uint32_t bits_union(float f) {            // UB in ISO C++ (reading an inactive member);
    union { float f; std::uint32_t u; } x{f};   // GCC and Clang document it as supported: an extension
    return x.u;
}

static_assert(std::bit_cast<std::uint32_t>(1.0f) == 0x3f800000u);   // and bit_cast is constexpr
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
bits_bitcast(float):
	movd	eax, xmm0
	ret

bits_memcpy(float):
	movd	eax, xmm0
	ret

bits_union(float):
	movd	eax, xmm0
	ret
```

All three compile to one `movd` between register files. Only the first two are *standard C++*; only `bit_cast` is `constexpr` and states its intent. Prefer `std::bit_cast`; use `memcpy` when you need to copy into an existing object or into a different-sized destination.

### Experiment 8: Bulk construction in raw memory, with rollback

```cpp
// @test run -std=c++23 -O0 -fsanitize=address,undefined
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

struct Throwy {
    static inline int live = 0, copies = 0, throw_at = 3;
    int v;
    Throwy(int x = 0) : v(x) { ++live; }
    Throwy(const Throwy& o) : v(o.v) {
        if (++copies == throw_at) throw std::runtime_error("copy failed");
        ++live;
    }
    ~Throwy() { --live; }
};

int main() {
    Throwy src[5] = {1, 2, 3, 4, 5};
    Throwy::live = 5;                                       // five live objects: the sources
    auto* dst = static_cast<Throwy*>(std::malloc(5 * sizeof(Throwy)));   // raw storage: no Throwy objects
    try {
        std::uninitialized_copy(src, src + 5, dst);         // the third copy throws
    } catch (const std::exception& e) {
        std::printf("caught \"%s\"; live objects = %d\n", e.what(), Throwy::live);
    }
    std::free(dst);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
caught "copy failed"; live objects = 5
```

`uninitialized_copy` copied two elements, hit the exception on the third, **destroyed the two it had built**, and rethrew. Only the five originals remain. No leak, no half-constructed array. (If you replaced it with a hand-written loop and forgot the rollback, `live` would read 7 and ASan/LSan would say nothing, since the *memory* is fine: only the *objects* leaked.)

---

## 8. Assembly / runtime investigation

Two investigations were folded into Experiments 6 and 7. A third, which you should run yourself, is **the cost of destruction on the exception path**:

```bash
g++-14 -std=c++23 -O2 -S -masm=intel -fno-asynchronous-unwind-tables -o - raii.cpp | c++filt | less
```

where `raii.cpp` is a function that constructs a `std::string`, calls an opaque `may_throw()`, and returns. Find: (1) the normal-path destructor call; (2) the `[clone .cold]` section holding the **landing pad**; (3) `_Unwind_Resume`. Then add `noexcept` to `may_throw` and observe the landing pad disappear. Destruction on the exception path is *code you do not write and cannot see in the source*; the next two chapters build on it.

Inspect the section layout of any object file to see where each storage duration lives:

```bash
readelf -S -W lifetime.o | grep -E '\.(data|bss|tdata|tbss|init_array|rodata)'
objdump -s -j .init_array lifetime.o      # pointers to the dynamic-initialization functions
```

---

## 9. Implementation exercise: bulk construction with rollback

Implement the two primitives every container needs:

```cpp
template <class InputIt, class T>
T* my_uninitialized_copy(InputIt first, InputIt last, T* dest);   // basic guarantee on exception

template <class T>
void my_destroy(T* first, T* last) noexcept;
```

Requirements:

1. If a constructor throws, destroy everything already constructed (in **reverse** order of construction is conventional; the standard leaves it unspecified for `uninitialized_copy`) and rethrow.
2. Skip the destruction loop entirely when `std::is_trivially_destructible_v<T>` (compile-time, `if constexpr`).
3. When `T` is trivially copyable and the iterator is a pointer, dispatch to `std::memcpy`/`memmove` (this is what real implementations do and why `vector<int>` copies at memory bandwidth).
4. Test with the `Throwy` type from Experiment 8, with ASan on, throwing at *every* position from 1 to *n*.

<details>
<summary><strong>Reference solution</strong> (try first; tested code)</summary>

```cpp
// @test run -std=c++23 -O1 -fsanitize=address,undefined
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>

template <class T>
void my_destroy(T* first, T* last) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        while (last != first) std::destroy_at(--last);   // reverse order
    }
}

template <class InputIt, class T>
T* my_uninitialized_copy(InputIt first, InputIt last, T* dest) {
    if constexpr (std::is_pointer_v<InputIt> &&
                  std::is_trivially_copyable_v<T> &&
                  std::is_same_v<std::remove_cv_t<std::remove_pointer_t<InputIt>>, T>) {
        const auto n = static_cast<std::size_t>(last - first);
        if (n) std::memcpy(static_cast<void*>(dest), first, n * sizeof(T));   // implicit creation of the T objects
        return dest + n;
    } else {
        T* cur = dest;
        try {
            for (; first != last; ++first, (void)++cur)
                std::construct_at(cur, *first);
        } catch (...) {
            my_destroy(dest, cur);       // roll back exactly what was built
            throw;
        }
        return cur;
    }
}

struct Throwy {
    static inline int live = 0, copies = 0, throw_at = 0;
    int v;
    Throwy(int x = 0) : v(x) { ++live; }
    Throwy(const Throwy& o) : v(o.v) {
        if (++copies == throw_at) throw std::runtime_error("copy failed");
        ++live;
    }
    ~Throwy() { --live; }
};

int main() {
    constexpr int N = 6;
    Throwy src[N] = {1, 2, 3, 4, 5, 6};
    const int baseline = Throwy::live;                       // the N source objects

    for (int fail_at = 1; fail_at <= N; ++fail_at) {         // inject a failure at every position
        Throwy::copies = 0;
        Throwy::throw_at = fail_at;
        auto* dst = static_cast<Throwy*>(std::malloc(N * sizeof(Throwy)));
        bool threw = false;
        try { my_uninitialized_copy(src, src + N, dst); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw);
        assert(Throwy::live == baseline);                    // rollback left no stray objects
        std::free(dst);
    }

    Throwy::throw_at = 0;                                    // success path
    auto* dst = static_cast<Throwy*>(std::malloc(N * sizeof(Throwy)));
    Throwy* end = my_uninitialized_copy(src, src + N, dst);
    assert(end == dst + N && Throwy::live == baseline + N);
    my_destroy(dst, end);
    assert(Throwy::live == baseline);
    std::free(dst);

    int ia[4] = {1, 2, 3, 4};                                // trivially copyable fast path
    int ib[4];
    my_uninitialized_copy(ia, ia + 4, ib);
    assert(ib[3] == 4);
    std::puts("ok");
}
```

</details>

---

## 10. Real-world example: the destruction-order bug in production

A pattern that appears in every large codebase:

1. A **registry**, **cache** or **plugin manager** is a global (or `static` member).
2. Its destructor flushes state through a **logger**, a **thread pool**, or **a database handle**.
3. Those are *also* statics, constructed lazily, probably *after* the registry.
4. At exit, they are destroyed **first**. The registry's destructor then calls into a destroyed object.

The symptom is a crash on **shutdown only**, in code that "has not changed", usually reported as "random crash when closing the app". It is exactly Experiment 5. You see it in plugin systems (a plugin's static outlives the host library's services), in Qt applications (a `QObject`-owning global outliving `QCoreApplication`), and with `thread_local` objects touched during static destruction.

Remedies, in the order to try them:

| Remedy | When |
|---|---|
| **Don't have the global.** Pass the dependency in (constructor injection) and own it in `main` | Almost always the best answer. Lifetime becomes visible in the call graph |
| **Touch dependencies in the constructor** (`logger();`) | When the global must exist |
| **Function-local statics** instead of namespace-scope globals | Avoids cross-TU *initialization* order problems |
| **Leaky singleton** (`static auto& x = *new X;`) | The object must outlive everything; shutdown ordering is not worth the risk. Run LeakSanitizer with an allowlist |
| **Explicit shutdown** (`app.shutdown()` before `main` returns) | You need *orderly* teardown of threads and I/O. Statics are too late for that |

> **Verdict.** Static destructors are the wrong place for anything that does I/O, takes locks, or touches another object. Shut down explicitly, and let statics hold only memory.

---

## 11. Failure modes

| Mistake | What happens | Fix |
|---|---|---|
| `const T& r = f().get();` | Dangling; temporary dies at `;` | Copy (`T r = …`), or ensure `get()` returns by value |
| `std::string_view sv = make_string();` | `string_view` is a reference-like type; the temporary dies | Name the string first, or take `std::string` |
| `for (x : make().items())` | Temporary dies before the loop (pre-C++23 / pre-GCC 15) | `auto obj = make(); for (x : obj.items())` |
| `return` a reference/pointer/`string_view` to a local | Dangling the instant the function returns | Return by value; or accept an output parameter |
| Lambda captures `[&]` and outlives the scope (stored, returned, passed to a thread) | Dangles when the scope ends | Capture by value or `shared_ptr`; join/await before the scope exits |
| `std::thread t([&]{ … });` then `t.detach()` | The locals die; the thread keeps running | Never detach with reference captures; use `std::jthread` ([Ch. 29](../part-12-concurrency/29-threading.md)) |
| `static` objects used from another global's destructor | Use-after-destroy at exit | §10 |
| Doing work in a **static destructor** | Order is fragile; threads may still be running | Shut down explicitly |
| Relying on **construction order across translation units** | Unspecified | Function-local statics; `constinit`; no cross-TU dependencies |
| Throwing from a **destructor** | `std::terminate` (destructors are `noexcept` by default) | Catch inside; log; or provide an explicit `close()` that can fail |
| Calling a **virtual function from a constructor/destructor** | Calls the base version, not the override; pure virtual → crash | Two-phase init via a factory; don't |
| Relying on **`exit()`** to run local destructors | It runs *statics'* destructors only | Return from `main`; or use RAII + `std::terminate` thinking |
| `longjmp` over objects with destructors | Skips them: UB if they're non-trivial | Don't mix `longjmp` and C++ objects |
| Reading an uninitialized local | UB (pre-C++26); erroneous behaviour (C++26) | Initialize at declaration |
| `memcpy`ing an object holding a pointer **into itself** (SSO `std::string`) | The copy's pointer points into the *source* | Copy-construct; mark such types non-trivially-copyable |
| Treating "it passes ASan" as proof | ASan finds *used* dangling storage, not unreached paths | Test all paths; run under UBSan/TSan too |

---

## 12. Exercises

1. **Trace it.** Predict the output of Experiment 1 if `Derived`'s initializer list is `Derived() : first("f"), second("s")`. Then check with `-Wreorder`.
2. **Extend the table.** Add cases to Experiment 2: (a) a `new Holder{make("n")}` (b) a function `Holder make_holder() { return Holder{make("h")}; }` called as `auto h = make_holder();` (c) a **conditional expression** `cond ? make("x") : make("y")` bound to a `const&`. Predict, then run.
3. **Destruction order.** Write a program with a global, a function-local static, a `thread_local`, and a local in `main`, each a `Noisy`. Predict the destruction order, including a second thread that also touches the `thread_local`. Run it.
4. **Fiasco in two files.** Reproduce the static-initialization-order fiasco with *two* translation units (`a.cpp` defines a global `std::string`, `b.cpp` has a global whose constructor reads it). Show that swapping link order changes the behaviour. Fix it with a function-local static.
5. **`constinit`.** Take a global with a dynamic initializer that you can make constant-initialized (e.g. a `std::array` of lookup values or a `std::atomic<int>`). Mark it `constinit`. Compare the asm and the `.init_array` before and after. What breaks if you mark it `constinit` but the initializer is not constant?
6. **Parameter timing.** Make a program whose output **differs** between GCC and what you would expect on MSVC due to Experiment 3. Fix it so the output is the same on every ABI.
7. **Rollback.** Write `my_uninitialized_move_n` with the *strong* guarantee when the move constructor is `noexcept` and the *basic* guarantee otherwise. (Hint: this is the same decision `std::vector::push_back` makes with `std::move_if_noexcept`.)

---

## 13. Challenge: `ImmortalSingleton<T>`

Design `ImmortalSingleton<T>` with:

- a `constinit`-constructible handle (no dynamic initializer, so safe to use as a namespace-scope global from any translation unit)
- **thread-safe lazy construction** of the `T` on first use (try `std::call_once` first, then replace it with an atomic state machine and compare the generated code)
- storage in an `alignas(T) std::byte[sizeof(T)]` buffer, with the `T` created by `construct_at` and **never destroyed**
- an optional `destroy_for_leak_check()` that tests can call to keep LeakSanitizer quiet
- safe to call from **static destructors** of other objects, **from other threads**, and **re-entrantly** (what should happen if `T`'s constructor calls `instance()` again?)

Then:

1. Reproduce Experiment 5's bug with a plain function-local static, and show your singleton does not have it.
2. Run it under TSan with eight threads racing on first use.
3. Explain what `__cxa_guard_acquire` does about re-entrancy, and whether your version matches.

---

## 14. Knowledge check

1. What is the difference between *storage duration* and *lifetime*? Name one object whose storage outlives its lifetime.
2. A class has members declared `a, b, c`. Its constructor's initializer list is `: c(…), a(…), b(…)`. In what order are they constructed? Destroyed? Does the list order matter at all?
3. Member `b`'s constructor throws. Which destructors run? Does the enclosing object's destructor run?
4. Why does `const T& r = id(make());` dangle while `const T& r = make();` does not?
5. What is "temporary materialization", and why did C++17 introduce it?
6. Two globals in different translation units: A's constructor reads B. What does the standard guarantee? Name two ways to fix it.
7. In what order are static objects destroyed? Does a function-local static constructed *during* `main` participate?
8. What does `constinit` guarantee that `constexpr` does not, and what does `constexpr` guarantee that `constinit` does not?
9. It is implementation-defined when a by-value parameter is destroyed. Where does it happen on Linux, and why might that break code written on Windows?
10. `std::uninitialized_copy` throws halfway. What state is the destination in? What would a naïve loop leave behind?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. *Storage duration* is how long the bytes exist; *lifetime* is the interval from completed construction to start of destruction. An automatic variable after its destructor has run (stack slot still there), or a `std::vector`'s spare capacity, or `malloc`'d storage before construction.
2. Constructed `a, b, c` (declaration order); destroyed `c, b, a`. The initializer-list order is irrelevant to the *order* (only to what initializes what); `-Wreorder` warns when it disagrees with declaration order.
3. Members `a` (and any bases) that were already constructed are destroyed, in reverse order. The enclosing object's destructor does **not** run, because the object's lifetime never began.
4. Binding directly to a prvalue extends it. In `id(make())` the temporary is bound to `id`'s *parameter*, which is a different reference, and it dies at the end of the full-expression. The reference returned by `id` is a *copy of that reference*, with no lifetime-extension effect.
5. A prvalue is a *recipe for a value*, not an object, until some operation needs an object (binding to a reference, member access…). C++17 specified it this way so that guaranteed copy elision could work: `T x = f();` constructs `x` directly in place instead of creating a temporary and then copying.
6. Nothing: the relative order of initialization across translation units is **unspecified**. Fixes: make B a function-local static accessed through a function (construct on first use); or make B `constinit`/constant-initialized so no dynamic initialization is involved; or remove the dependency (pass it in).
7. In reverse order of the *completion* of their construction. Yes, function-local statics (and objects with static duration constructed late) participate: they are destroyed in that same reverse order, interleaved with the namespace-scope ones.
8. `constinit` guarantees the variable is **constant-initialized** (no dynamic init) but the variable may be mutable and need not be usable in constant expressions. `constexpr` makes it `const` and usable in constant expressions but does not let you opt into mutability. They address different promises, and both can be combined only when immutability is fine.
9. On Itanium ABI platforms (Linux) the **caller** destroys it at the end of the full-expression; on MSVC the **callee** does so before return. Code that relies on destructor side effects having happened "by the time the call returns" works on Windows and fails on Linux, or vice versa.
10. A conforming `uninitialized_copy` destroys the elements it constructed and leaves the destination as raw storage again. A naive loop leaves the first *k* objects alive in memory that the caller thinks is uninitialized, so they leak (resources held by those objects are never released) and any later "cleanup" that treats the buffer as empty or fully constructed misbehaves.

</details>

---

[← Previous: Chapter 2](02-object-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 4 — RAII →](04-raii.md)
