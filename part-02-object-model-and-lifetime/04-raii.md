# Chapter 4 — RAII

> **Part II · Object model and lifetime** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapters 2–3](03-lifetime-and-storage.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, ASan, `/proc/self/fd`

[← Previous: Chapter 3](03-lifetime-and-storage.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 5 — Value categories →](../part-03-value-categories-and-move/05-value-categories.md)

---

**In one sentence:** RAII ties the lifetime of a *resource* to the lifetime of an *object*, so that the one thing C++ **guarantees** (destructors run when a scope ends, on every path including exceptions) becomes the one mechanism that guarantees *every* kind of cleanup.

**By the end of this chapter you can:**

- design a resource-owning type: its empty state, its move semantics, what its destructor does if release fails
- explain why RAII makes exception safety *compositional*, and show the machine code that proves it
- avoid the traps that RAII itself creates (throwing destructors, unnamed guards, partial construction, static-destruction order)
- write a generic scope guard (`scope_exit`, `scope_fail`, `scope_success`) and a handle type
- choose correctly among `unique_ptr` + deleter, a purpose-built handle, a scope guard, and explicit shutdown

---

## 1. Problem

Every resource follows the same protocol:

```text
acquire ──► use ──► release          (exactly once, on every path)
```

Memory, file descriptors, sockets, mutexes, database transactions, GPU buffers, temporary files, reference counts, "busy" flags, trace spans: all of them. The protocol is trivial. **Following it on every path is not.**

```c
int process(const char *path) {          /* the C way */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    char *buf = malloc(4096);
    if (!buf) { close(fd); return -1; }

    if (read(fd, buf, 4096) < 0) {        /* now two resources to release */
        free(buf); close(fd); return -1;
    }
    FILE *log = fopen("log.txt", "a");
    if (!log) { free(buf); close(fd); return -1; }    /* three, and the list grows */
    /* … */
    fclose(log); free(buf); close(fd);
    return 0;
}
```

With *n* resources there are *n* exit paths, each releasing a different prefix. Every new resource edits every existing path. Every `return` added later is a chance to forget one. The `goto cleanup` idiom (used throughout the Linux kernel) tames this at the price of declaring everything up front, but it still cannot cope with **exceptions**, which add an invisible exit after *every function call*.

> **The invariant to maintain:** a resource has *exactly one owner at all times*, and the owner releases it *exactly once*.

---

## 2. Historical context

| Approach | Era | Cost / weakness |
|---|---|---|
| Manual release before every `return` | C | Combinatorial; breaks silently on edit |
| `goto cleanup` | C, kernel style | Variables hoisted; no exceptions; still manual |
| `try { … } catch (...) { cleanup; throw; }` | C++98 | Cleanup duplicated per path; ordering by hand |
| **RAII**: constructor acquires, destructor releases | C++ (Stroustrup, 1980s) | Requires a type per resource, which is the right cost |
| `std::auto_ptr` | C++98 | RAII for heap pointers, but *copy* secretly moved: a design error, deprecated in C++11, **removed in C++17** |
| `ScopeGuard` (Alexandrescu & Marginean) | 2000 | Ad-hoc cleanup at a call site without a dedicated type |
| `unique_ptr` + custom deleter, `lock_guard` | C++11 | RAII becomes standard vocabulary |
| `scoped_lock`, `jthread`, CTAD | C++17/20 | Deadlock-free multi-locking; threads that join in their destructor |
| `scope_exit` / `unique_resource` | Library Fundamentals TS v3 🧪 | Available as `<experimental/scope>` in libstdc++, not in the IS |

Other languages found their own answers to the same problem, each with a different flaw:

| Language | Mechanism | Limitation compared with RAII |
|---|---|---|
| Java | `try`/`finally`, try-with-resources | Opt-in at every use site; *finalizers* are nondeterministic |
| Python | `with` / context managers | Opt-in at every use site; refcount destruction is CPython-specific |
| Go | `defer` | Function-scoped, not block-scoped; opt-in; runs at function exit even in loops |
| C# | `using` | Opt-in at every use site |
| Rust | `Drop` | **The same idea as RAII**, and enforced by the borrow checker |

The distinguishing feature of C++ RAII is that cleanup is **part of the type**, not of the call site: you *cannot forget* it, and callers need not know it exists.

---

## 3. Modern solution

A handful of design rules, each of which you will see justified below:

1. **Acquire in the constructor** (or in a factory that returns the constructed object). If acquisition can fail routinely, prefer a factory returning `std::expected` ([Chapter 23](../part-09-error-handling/23-expected.md)); if failure is exceptional, throw from the constructor.
2. **Release in the destructor**, which must **not throw**.
3. Give the type a **well-defined empty state** (`fd == -1`, `ptr == nullptr`), so that moved-from objects are safe to destroy.
4. Make it **move-only** unless duplicating the resource has a clear meaning (`dup`, `shared_ptr`).
5. **One object, one resource.** A class owning two raw resources is two classes. This is what makes constructors exception-safe for free.
6. Provide an explicit `release()` (give up ownership), `reset()` (replace), and, if destruction can *fail*, an explicit `close()`/`commit()` that **reports** failure.

The library already provides the common cases, so reach for them first:

| Resource | Standard RAII type |
|---|---|
| Heap object | `std::unique_ptr<T>` (default), `std::shared_ptr<T>` (shared; [Ch. 25](../part-10-memory/25-smart-pointers.md)) |
| Mutex | `std::lock_guard`, `std::scoped_lock` (many mutexes, deadlock-free), `std::unique_lock` (deferred/timed) |
| File (stream) | `std::fstream`, `std::ifstream`, `std::ofstream` |
| Thread | `std::jthread` (joins in destructor) |
| Anything with a C-style close | `std::unique_ptr<T, Deleter>` |
| Arbitrary cleanup code | a scope guard (§9) |

### The ownership vocabulary

RAII is half of a bigger idea: **the type of a pointer-like parameter tells the reader who owns the object**. Compare the options (more in [Chapters 12 and 25](../part-05-standard-library/12-views-and-non-owning-types.md)):

| You write | It means | Owns? | May be null? | Typical use |
|---|---|:-:|:-:|---|
| `T*` | Observer *by convention*; or C-style "maybe owner" | ✗ (by convention) | yes | Optional, rebindable reference; C interop |
| `T&` | Non-owning, non-null, non-rebindable | ✗ | no | Default way to *use* an object |
| `std::unique_ptr<T>` | Exclusive ownership; **transfer** on move | ✔ exclusive | yes | Factory return, polymorphic owner, PImpl |
| `std::shared_ptr<T>` | Shared ownership by reference counting | ✔ shared | yes | Genuinely shared lifetime (*rare*; [Ch. 25](../part-10-memory/25-smart-pointers.md)) |
| `std::span<T>` | Non-owning view of contiguous elements | ✗ | (empty) | Function parameter for "a range of T" |
| `std::experimental::observer_ptr<T>` 🧪 | An *explicitly* non-owning pointer | ✗ | yes | Self-documenting `T*` |

> **Verdict.** A function that takes `std::unique_ptr<T>` by value *says* "I am taking ownership". A function that takes `T&` *says* "I only borrow". Raw owning `T*` says nothing, and that is the problem RAII solves.

---

## 4. Mental model

Think of a scope as a **ledger of obligations**:

```text
 function begins                         every acquisition is pushed on the ledger
 │
 ├─ Step a("lock")      ── push: release a
 ├─ Step b("file")      ── push: release b
 ├─ Step c("socket")    ── push: release c
 │       … work …
 └─ function exits  ◄── by return, by break, by goto-out, or by THROW
         │
         ▼   the ledger is unwound, LIFO
         release c ─► release b ─► release a
```

Three properties make this powerful:

1. **Last in, first out** matches *dependency order*. The thing acquired later usually depends on the thing acquired earlier (a transaction on a connection; a file under a lock), so it is released first.
2. **The ledger entries are written by the compiler, not by you.** The set of exit paths is *computed*, including exceptional ones. This is the same guarantee as in Experiment 7, made visible in assembly.
3. **Composition is free.** A class with three RAII members is itself RAII, with a *generated* destructor. A `std::vector<UniqueFd>` closes every descriptor when it dies.

The five questions you must answer for **every** handle type:

| # | Question | Typical answer |
|:-:|---|---|
| 1 | What is the **empty state**? | `-1`, `nullptr`, `INVALID_HANDLE_VALUE` |
| 2 | Can it be **copied**? | No (move-only), *or* copy = duplicate (`dup`, deep copy), *or* shared |
| 3 | What does **move** leave behind? | The empty state, and *only* the empty state |
| 4 | What if **release fails**? | Destructor swallows/logs; offer `close()` that returns the error |
| 5 | Does destruction **block** or take time? | Document it. A destructor that waits for I/O in a hot path is a latency bug |

---

## 5. Language rules

### 5.1 Destructors are `noexcept` by default  `[class.dtor]`

Since C++11, a destructor is implicitly `noexcept(true)` unless a base or member destructor is `noexcept(false)`. If an exception escapes a `noexcept` destructor, `std::terminate` is called. And if a destructor with `noexcept(false)` throws **while the stack is already unwinding** from another exception, `std::terminate` is called too. There is no way to propagate two exceptions at once.

> **Rule.** A destructor must not let exceptions escape. If cleanup can fail, either handle the failure inside, or provide an explicit operation that reports it.

### 5.2 Which destructors run  `[except.ctor]`

When an exception propagates, destructors run for every **fully constructed automatic object** in every frame being left (and for fully constructed members/bases of partially constructed objects, as seen in [Chapter 3](03-lifetime-and-storage.md)). Whether stack unwinding occurs at all if the exception is **never caught** is *implementation-defined*: GCC and Clang call `std::terminate` *without* unwinding when no handler exists (this is a good reason to have a catch-all in `main` for diagnostics and shutdown).

### 5.3 Special member functions: Rule of 0, 3, 5

| Rule | Statement |
|---|---|
| **Rule of Zero** | Own nothing directly. Compose RAII members. Declare none of the special members. **Prefer this.** |
| **Rule of Three** | If you declare a destructor, copy constructor or copy assignment, declare all three (pre-C++11 formulation) |
| **Rule of Five** | …and the move constructor and move assignment too. If you declare *any* of the five, decide each one: `= default`, `= delete`, or write it |

Declaring *any* of them changes the others' implicit generation: declaring a destructor suppresses implicit *move* operations (the class silently falls back to copying); declaring a move operation makes copies `= delete`d. This is why a "harmless" `~Foo() {}` makes a class slower and non-trivially-copyable ([Chapter 2](02-object-model.md)).

### 5.4 `std::uncaught_exceptions()`  `[uncaught.exceptions]` (C++17)

Returns the number of exceptions currently in flight (thrown and not yet caught) **on this thread**. A guard that records the value at construction can compare it at destruction to learn whether it is running *because of unwinding*:

```text
 destructor sees  uncaught_exceptions() > value at construction   ⇒   we are unwinding
```

This is the *only* correct way to implement `scope_fail` and `scope_success`. The older `std::uncaught_exception()` (singular, `bool`) was **deprecated in C++17 and removed in C++20**, because it breaks when a destructor itself uses a nested try block during unwinding.

### 5.5 Attributes that make RAII harder to misuse

| Attribute | Use |
|---|---|
| `[[nodiscard]]` on a **class** | Warns when a function returning it ignores the result (a guard returned and dropped) |
| `[[nodiscard]]` on a **constructor** (C++20) | Warns when `Guard{m};` creates a temporary that dies immediately (libstdc++ marks `lock_guard`'s constructor this way) |
| `explicit` constructors | A single-argument constructor that *acquires* must be `explicit`, so a stray `int` never becomes an owning handle |
| `noexcept` on move and on `release`/`reset` | Required for standard containers to *move* rather than *copy* your handles on reallocation ([Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md)) |

---

## 6. Implementation model

A destructor is **ordinary code the compiler inserts**. The only novelty is *where*:

```text
 normal path:        call ~T()  at every scope exit (return, break, end of block)   ← inlined if small
 exceptional path:   a landing pad, reached by the unwinder through the LSDA table
                     (.gcc_except_table), that calls ~T() then _Unwind_Resume()
```

On the Itanium ABI the landing pads and their ranges are described in **data** (`.gcc_except_table`, `.eh_frame`), not in instructions on the hot path. This is the *table-driven* ("zero-cost") exception model that [Chapter 22](../part-09-error-handling/22-exceptions.md) opens up. For RAII, the consequences are:

- **Non-throwing path:** no instruction is executed *for* exception safety. The destructor call is the destructor call you would have written by hand.
- **Throwing path:** slow (tens of microseconds is typical) and placed in a `.cold` clone, out of the instruction cache's way.
- **`-fno-exceptions`:** the landing pads vanish (Experiment 7). Destructors still run on normal exits.

### Layer check

| Layer | Who decides what |
|---|---|
| **Standard** | Destructors run at scope exit and during unwinding; reverse order; `noexcept` destructors; `terminate` on double exception; `uncaught_exceptions` |
| **Compiler** | Where cleanup is emitted; landing pads; `[clone .cold]`; elision of trivial destructors; `-fno-exceptions` |
| **ABI (Itanium EH)** | LSDA format, personality routine (`__gxx_personality_v0`), `_Unwind_Resume`; whether uncaught exceptions unwind at all |
| **OS** | The *resource semantics*: what `close()` does on error, whether `EINTR` means "closed" (Linux: **yes**, the descriptor is gone, so never retry `close`) |
| **CPU** | Cold landing pads cost nothing until taken; no special hardware support |

---

## 7. Experiments

### Experiment 1: Manual cleanup leaks, RAII doesn't

Two versions of "open three files, bail out at some stage". The manual version carries the two most common bugs in this genre: an early `return` that skips a `close`, and a copy-pasted cleanup block that is missing one line.

```cpp
// @test run -std=c++23 -O1
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

// Number of descriptors this process currently holds. Only *differences* matter:
// the constant offset (".", "..", and the DIR's own fd) cancels out.
int open_fds() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d)) ++n;
        closedir(d);
    }
    return n;
}

// ---- 1. manual cleanup, with two realistic bugs --------------------------------------
bool process_manual(int fail_stage) {
    int a = open("/dev/null", O_RDONLY);
    if (a < 0) return false;
    if (fail_stage == 1) { close(a); return false; }

    int b = open("/dev/zero", O_RDONLY);
    if (b < 0) { close(a); return false; }
    if (fail_stage == 2) { return false; }                      // BUG: forgot close(b) and close(a)

    int c = open("/dev/urandom", O_RDONLY);
    if (c < 0) { close(b); close(a); return false; }
    if (fail_stage == 3) { close(c); close(b); return false; }  // BUG: copy-paste lost close(a)

    close(c); close(b); close(a);
    return true;
}

// ---- 2. RAII: a minimal owning descriptor handle -------------------------------------
class UniqueFd {
    int fd_ = -1;                                               // the empty state
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    int  get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int  release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);       // on Linux, never retry close() after EINTR: the fd is already gone
        fd_ = fd;
    }
};

bool process_raii(int fail_stage) {
    UniqueFd a{open("/dev/null", O_RDONLY)};
    if (!a) return false;
    if (fail_stage == 1) return false;

    UniqueFd b{open("/dev/zero", O_RDONLY)};
    if (!b) return false;
    if (fail_stage == 2) return false;

    UniqueFd c{open("/dev/urandom", O_RDONLY)};
    if (!c) return false;
    if (fail_stage == 3) return false;
    return true;                                                // no cleanup code anywhere
}

int main() {
    std::printf("stage   leaked fds (manual)   leaked fds (RAII)\n");
    for (int stage = 0; stage <= 3; ++stage) {
        int before = open_fds();
        process_manual(stage);
        const int leaked_manual = open_fds() - before;

        before = open_fds();
        process_raii(stage);
        const int leaked_raii = open_fds() - before;
        std::printf("%5d   %19d   %17d\n", stage, leaked_manual, leaked_raii);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
stage   leaked fds (manual)   leaked fds (RAII)
    0                     0                   0
    1                     0                   0
    2                     2                   0
    3                     1                   0
```

The RAII version has **no cleanup code**, so there is nothing to forget. Adding a fourth resource means adding *one declaration*, not editing every `return`. The manual version's bugs are not exotic: they are what every long function accumulates under maintenance.

### Experiment 2: Release order on every kind of exit

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <stdexcept>

struct Step {
    const char* n;
    explicit Step(const char* name) : n(name) { std::printf("    acquire %s\n", n); }
    ~Step() { std::printf("    release %s\n", n); }
};

void work(int fail_at) {
    Step a("lock");
    if (fail_at == 1) throw std::runtime_error("failed after lock");
    Step b("file");
    if (fail_at == 2) return;                                   // early return
    Step c("socket");
    if (fail_at == 3) throw std::runtime_error("failed after socket");
    std::puts("    all work done");
}

int main() {
    for (int fail_at = 0; fail_at <= 3; ++fail_at) {
        std::printf("scenario %d\n", fail_at);
        try { work(fail_at); }
        catch (const std::exception& e) { std::printf("    caught: %s\n", e.what()); }
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
scenario 0
    acquire lock
    acquire file
    acquire socket
    all work done
    release socket
    release file
    release lock
scenario 1
    acquire lock
    release lock
    caught: failed after lock
scenario 2
    acquire lock
    acquire file
    release file
    release lock
scenario 3
    acquire lock
    acquire file
    acquire socket
    release socket
    release file
    release lock
    caught: failed after socket
```

Four different exits (normal, exception, early return, exception later) and the same invariant: every `acquire` is matched by a `release`, **in reverse order**. And look at where `caught:` appears relative to the `release` lines: *the releases happen first*, during unwinding, and the handler runs after the scope is already cleaned up.

### Experiment 3: A destructor that throws

```cpp
// @test crash -std=c++23 -O0 -Wno-terminate err=terminate
#include <cstdio>
#include <stdexcept>

struct Flusher {
    ~Flusher() {                                       // implicitly noexcept(true)
        std::puts("flushing...");
        throw std::runtime_error("flush failed");      // cannot escape a noexcept function
    }
};

int main() {
    try {
        Flusher f;
    } catch (...) {
        std::puts("caught (you will not see this)");
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
terminate called after throwing an instance of 'std::runtime_error'
  what():  flush failed
```

The `catch` block never gets a chance: since C++11 a destructor is `noexcept`, so the exception cannot leave it and `std::terminate` ends the program. Declaring the destructor `noexcept(false)` only moves the problem: it then terminates *whenever* it runs during unwinding. Real-world `flush`/`commit`/`close` operations **can** fail, which is why §8's design rule exists: **destructors release; explicit operations report.**

### Experiment 4: Am I being destroyed *because* of an exception?

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <exception>
#include <stdexcept>

struct Probe {
    int at_ctor = std::uncaught_exceptions();
    const char* label;
    explicit Probe(const char* l) : label(l) {}
    ~Probe() {
        std::printf("  %s: unwinding because of an exception? %s\n",
                    label, std::uncaught_exceptions() > at_ctor ? "YES" : "no");
    }
};

int main() {
    std::puts("normal scope exit:");
    { Probe p("p1"); }

    std::puts("scope exit by exception:");
    try { Probe p("p2"); throw std::runtime_error("x"); }
    catch (...) {}

    std::puts("a probe created *during* unwinding sees a baseline of 1:");
    struct Outer { ~Outer() { Probe inner("inner"); } };        // constructed and destroyed during unwinding
    try { Outer o; throw std::runtime_error("y"); }
    catch (...) {}
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
normal scope exit:
  p1: unwinding because of an exception? no
scope exit by exception:
  p2: unwinding because of an exception? YES
a probe created *during* unwinding sees a baseline of 1:
  inner: unwinding because of an exception? no
```

The third case is why the *count* matters. `inner` is destroyed while an exception is already in flight, but **it did not cause it**: it recorded `1` at construction and still sees `1` at destruction. The old boolean `std::uncaught_exception()` would have answered "yes" there, wrongly, and a `scope_fail` built on it would roll back work that was *not* failing. This is the subtle bug the C++17 replacement exists to fix.

### Experiment 5: The unnamed guard

```cpp
// @test run -std=c++23 -O0 -Wno-unused-result
#include <cstdio>
#include <mutex>

std::mutex m;

int main() {
    std::lock_guard<std::mutex>{m};          // a TEMPORARY: locks, then unlocks at the semicolon
    const bool free_now = m.try_lock();      // if the guard were doing its job, this would fail
    std::printf("mutex is free right after the 'guard' statement? %s\n",
                free_now ? "yes: the guard protected nothing" : "no: still locked");
    if (free_now) m.unlock();
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
mutex is free right after the 'guard' statement? yes: the guard protected nothing
```

The bug: **an RAII object must be named**. A temporary lives to the end of the full-expression, and a guard's work happens *between* construction and destruction. (Remove `-Wno-unused-result` and libstdc++ will warn, since it marks `lock_guard`'s constructor `[[nodiscard]]`; clang-tidy's `bugprone-unused-raii` flags it for your own types.) And the *other* spelling is even nastier:

```cpp
// @test fail -std=c++23 err=lock_guard
#include <mutex>

std::mutex m;

void f() {
    std::lock_guard<std::mutex>(m);   // NOT a temporary: parsed as the *declaration* "std::lock_guard<std::mutex> m;"
}
```

C++'s "anything that can be a declaration, is" rule turns the parenthesized form into a *variable named `m`* that hides the mutex. Here it fails to compile (no default constructor); for a type with a default constructor, it compiles and silently locks nothing.

### Experiment 6: The size of a custom deleter

```cpp
// @test run -std=c++23 -O2
#include <cstdio>
#include <memory>

struct FcloseDeleter { void operator()(std::FILE* f) const noexcept { std::fclose(f); } };

using FileA = std::unique_ptr<std::FILE, FcloseDeleter>;                               // empty functor
using FileB = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;                         // function pointer
using FileC = std::unique_ptr<std::FILE, decltype([](std::FILE* f) noexcept { std::fclose(f); })>;  // stateless lambda (C++20)

int main() {
    std::printf("FILE*           : %zu\n", sizeof(std::FILE*));
    std::printf("unique_ptr + empty functor    : %zu\n", sizeof(FileA));
    std::printf("unique_ptr + function pointer : %zu\n", sizeof(FileB));
    std::printf("unique_ptr + stateless lambda : %zu\n", sizeof(FileC));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
FILE*           : 8
unique_ptr + empty functor    : 8
unique_ptr + function pointer : 16
unique_ptr + stateless lambda : 8
```

A **stateless** deleter (an empty class, or a captureless lambda) costs nothing: `unique_ptr` is a bare pointer, and `fclose` is called directly (and can be inlined). A deleter that is a **function pointer** must *store* that pointer: the object doubles in size, and the call goes through the pointer. This is the zero-overhead principle of [Chapter 1](../part-01-mental-model/01-modern-cpp-philosophy.md) applied to RAII: use a functor type, not a function pointer, as the deleter.

### Experiment 7: What the compiler writes for you

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=two_guards
struct Guard { Guard(); ~Guard(); };
void may_throw();

void two_guards() {
    Guard a;
    may_throw();
    Guard b;
    may_throw();
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
two_guards():
	push	r12
	push	rbp
	push	rbx
	sub	rsp, 16
	lea	rbx, 14[rsp]
	mov	rdi, rbx
	call	Guard::Guard()@PLT
	call	may_throw()@PLT
	lea	rbp, 15[rsp]
	mov	rdi, rbp
	call	Guard::Guard()@PLT
	call	may_throw()@PLT
	mov	rdi, rbp
	call	Guard::~Guard()@PLT
	mov	rdi, rbx
	call	Guard::~Guard()@PLT
	add	rsp, 16
	pop	rbx
	pop	rbp
	pop	r12
	ret
.L4:
	mov	r12, rax
	jmp	.L3
.L5:
	mov	r12, rax
	jmp	.L2

two_guards() [clone .cold]:
.L2:
	mov	rdi, rbp
	call	Guard::~Guard()@PLT
.L3:
	mov	rdi, rbx
	call	Guard::~Guard()@PLT
	mov	rdi, r12
	call	_Unwind_Resume@PLT
```

Read it in three parts:

1. **The hot path** (top): construct `a`, call, construct `b`, call, then `~b`, `~a` in reverse order, and return.
2. **Two landing pads** (`.L4` and `.L5`, a stub that saves the in-flight exception in `r12` and jumps into the cleanup chain). Control arrives at `.L4` if the *first* `may_throw()` unwinds, and at `.L5` if the *second* does. The first needs to destroy only `a`; the second needs `b` *then* `a`. The compiler computed that, and shared the tail.
3. **The cold clone** (`two_guards() [clone .cold]`): a single cleanup chain with two entry points. `.L2` destroys `b` and falls through to `.L3`, which destroys `a`, then `_Unwind_Resume` hands the exception back to the unwinder. It lives out of line so it does not occupy instruction-cache lines on the path taken 100 % of the time.

Now the same function with exceptions disabled:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fno-exceptions filter=two_guards
struct Guard { Guard(); ~Guard(); };
void may_throw();

void two_guards() {
    Guard a;
    may_throw();
    Guard b;
    may_throw();
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
two_guards():
	push	rbp
	push	rbx
	sub	rsp, 24
	lea	rbx, 14[rsp]
	lea	rbp, 15[rsp]
	mov	rdi, rbx
	call	Guard::Guard()@PLT
	call	may_throw()@PLT
	mov	rdi, rbp
	call	Guard::Guard()@PLT
	call	may_throw()@PLT
	mov	rdi, rbp
	call	Guard::~Guard()@PLT
	mov	rdi, rbx
	call	Guard::~Guard()@PLT
	add	rsp, 24
	pop	rbx
	pop	rbp
	ret
```

The landing pads, the cold clone and `_Unwind_Resume` are all gone. The *normal* path is the same, destructors included. **RAII's runtime cost on the non-throwing path is zero.** The cost of exception safety is some bytes in `.gcc_except_table` and the cold section, and those are not touched unless something throws.

---

## 8. Assembly / runtime investigation

Two more places to look, both with command-line tools:

**1. How big is the exception-safety machinery?**

```bash
g++-14 -std=c++23 -O2 -c two_guards.cpp -o a.o
g++-14 -std=c++23 -O2 -fno-exceptions -c two_guards.cpp -o b.o
size a.o b.o
readelf -S -W a.o | grep -E 'gcc_except_table|eh_frame|text'
```

You will see the `.text.unlikely` (cold) section and `.gcc_except_table` appear only in `a.o`. For a large codebase, `-fno-exceptions` can shrink binaries by 10–20 %; this is a real cost of exceptions in *size*, not in speed.

**2. Which descriptors does the process hold?**

```bash
ls -l /proc/$PID/fd           # live; every open fd and what it points to
lsof -p $PID                  # same, with sockets and pipes decoded
```

Make this your first debugging step for "too many open files". The leak is almost always an owner that never ran its destructor: an exception thrown between `open` and the RAII wrapper's construction, a `std::thread` captured by reference that outlived its scope, or a `shared_ptr` cycle ([Chapter 25](../part-10-memory/25-smart-pointers.md)).

---

## 9. Implementation exercise: a generic scope guard

Implement three guards sharing one design:

| Guard | Runs its callable when… |
|---|---|
| `ScopeExit` | the scope exits, however it exits |
| `ScopeFail` | the scope exits **by exception** (rollback) |
| `ScopeSuccess` | the scope exits **normally** (commit) |

Requirements:

1. Class template on the callable type `F`, with CTAD from a lambda: `ScopeExit g{[&]{ … }};`.
2. **Not copyable**; **movable**, with the moved-from guard *disarmed* (a factory function can return a guard).
3. `release()`: disarm the guard ("dismiss": for the case where the commit succeeded and rollback is no longer wanted).
4. `[[nodiscard]]` on the class, so an unnamed guard is diagnosed.
5. The destructor is `noexcept`; if the callable throws, that is `terminate`, and you document it.
6. `ScopeFail` and `ScopeSuccess` use `std::uncaught_exceptions()` (Experiment 4), captured at construction.
7. Optional: a macro `ON_EXIT(code)` that creates an uniquely named guard. *(`__COUNTER__` is a GCC/Clang/MSVC extension, not standard C++, and that's worth a comment.)*

Then verify it with these properties: it runs on normal exit; it does not run after `release()`; it runs on a thrown exception; `ScopeFail` runs only on exception and `ScopeSuccess` only on normal exit; moving transfers the obligation exactly once; multiple guards run in **reverse order**.

<details>
<summary><strong>Reference solution</strong> (try first; tested code)</summary>

```cpp
// @test run -std=c++23 -O1 -fsanitize=address,undefined
#include <cassert>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace guard {

// One implementation of storage, move and release(); the Policy decides *when* the callable runs.
template <class F, class Policy>
class [[nodiscard]] GuardBase {
    F f_;
    Policy policy_;
    bool armed_ = true;

public:
    template <class G>
    explicit GuardBase(G&& f) noexcept(std::is_nothrow_constructible_v<F, G&&>)
        : f_(std::forward<G>(f)), policy_() {}

    GuardBase(GuardBase&& o) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(o.f_)), policy_(o.policy_), armed_(std::exchange(o.armed_, false)) {}

    GuardBase(const GuardBase&) = delete;
    GuardBase& operator=(const GuardBase&) = delete;
    GuardBase& operator=(GuardBase&&) = delete;

    // If f_ throws here, the exception cannot escape a noexcept destructor: std::terminate.
    ~GuardBase() { if (armed_ && policy_.should_run()) f_(); }

    void release() noexcept { armed_ = false; }
};

struct OnExit    { bool should_run() const noexcept { return true; } };
struct OnFailure {
    int at_ctor = std::uncaught_exceptions();           // captured when the guard is constructed
    bool should_run() const noexcept { return std::uncaught_exceptions() > at_ctor; }
};
struct OnSuccess {
    int at_ctor = std::uncaught_exceptions();
    bool should_run() const noexcept { return std::uncaught_exceptions() <= at_ctor; }
};

// Three real class templates (not aliases: aliases cannot carry user-written deduction guides).
template <class F> class [[nodiscard]] ScopeExit    : public GuardBase<F, OnExit>    { public: using GuardBase<F, OnExit>::GuardBase; };
template <class F> class [[nodiscard]] ScopeFail    : public GuardBase<F, OnFailure> { public: using GuardBase<F, OnFailure>::GuardBase; };
template <class F> class [[nodiscard]] ScopeSuccess : public GuardBase<F, OnSuccess> { public: using GuardBase<F, OnSuccess>::GuardBase; };

template <class F> ScopeExit(F) -> ScopeExit<F>;        // CTAD: ScopeExit g{[&]{ ... }};
template <class F> ScopeFail(F) -> ScopeFail<F>;
template <class F> ScopeSuccess(F) -> ScopeSuccess<F>;

template <class F> [[nodiscard]] auto scope_exit(F f)    { return ScopeExit<F>(std::move(f)); }
template <class F> [[nodiscard]] auto scope_fail(F f)    { return ScopeFail<F>(std::move(f)); }
template <class F> [[nodiscard]] auto scope_success(F f) { return ScopeSuccess<F>(std::move(f)); }

}  // namespace guard

// __COUNTER__ is a widely supported extension, not standard C++.
#define GUARD_CAT2(a, b) a##b
#define GUARD_CAT(a, b) GUARD_CAT2(a, b)
#define ON_EXIT(...) auto GUARD_CAT(on_exit_, __COUNTER__) = ::guard::scope_exit([&]() noexcept { __VA_ARGS__; })

int main() {
    using namespace guard;
    std::string log;

    // 1. runs on normal exit
    { auto g = scope_exit([&] { log += 'a'; }); }
    assert(log == "a");

    // 2. release() disarms
    { auto g = scope_exit([&] { log += 'X'; }); g.release(); }
    assert(log == "a");

    // 3. runs on exception; handler runs after the guard
    try { auto g = scope_exit([&] { log += 'b'; }); throw std::runtime_error("x"); }
    catch (...) { log += 'c'; }
    assert(log == "abc");

    // 4. fail / success are mutually exclusive
    log.clear();
    {
        auto f = scope_fail([&] { log += 'F'; });
        auto s = scope_success([&] { log += 'S'; });
    }
    assert(log == "S");
    log.clear();
    try {
        auto f = scope_fail([&] { log += 'F'; });
        auto s = scope_success([&] { log += 'S'; });
        throw 1;
    } catch (int) {}
    assert(log == "F");

    // 5. a guard created while unwinding does not think *it* is failing
    log.clear();
    struct Outer {
        std::string* log;
        ~Outer() { auto s = scope_success([this] { *log += 's'; }); }   // created during unwinding
    };
    try { Outer o{&log}; throw 1; } catch (int) {}
    assert(log == "s");

    // 6. moving transfers the obligation exactly once
    log.clear();
    {
        auto make = [&] { return scope_exit([&] { log += 'm'; }); };
        auto g = make();            // guaranteed elision
        auto h = std::move(g);      // g disarmed
    }
    assert(log == "m");

    // 7. reverse order of construction
    log.clear();
    {
        ON_EXIT(log += '1');
        ON_EXIT(log += '2');
        ON_EXIT(log += '3');
    }
    assert(log == "321");

    // 8. CTAD
    log.clear();
    { ScopeExit g{[&] { log += 'z'; }}; }
    assert(log == "z");
}
```

Design notes: one base template carries the storage, move and `release()` logic; the *policy* classes carry the only difference (*when* to run); the three public names are real class templates so each can have its own **deduction guide** (`ScopeExit g{lambda}`). Alias templates would be shorter, but user-written deduction guides cannot name an alias, and implicit alias CTAD (C++20) cannot deduce the policy here. Inheriting constructors (`using Base::Base`) keep the derived classes empty.

</details>

---

## 10. Real-world example

### 10.1 Locks, threads, files: the standard library's RAII

`std::lock_guard` and `std::scoped_lock` unlock on every exit. `std::jthread` joins in its destructor (its predecessor `std::thread` calls `std::terminate` if destroyed while joinable: an *anti-RAII* design that C++20 corrected; [Chapter 29](../part-12-concurrency/29-threading.md)). `std::ofstream` closes (and flushes) its file. All of these *swallow* errors in their destructors; if you must know the flush succeeded, call `close()` (or check the stream state) explicitly.

### 10.2 Database transactions: *commit-or-rollback*

The canonical RAII design where the destructor does **not** simply "free": it **rolls back unless you committed**.

```cpp
// @test run -std=c++23 -O1 -fsanitize=address,undefined
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct Db {                                            // a stand-in for a real connection
    std::vector<std::string> log;
    void begin()    { log.push_back("BEGIN"); }
    void exec(std::string sql) { log.push_back(std::move(sql)); }
    void commit()   { log.push_back("COMMIT"); }
    void rollback() { log.push_back("ROLLBACK"); }
};

class Transaction {
    Db* db_;                                           // nullptr once finished (the empty state)
public:
    explicit Transaction(Db& db) : db_(&db) { db_->begin(); }
    Transaction(Transaction&& o) noexcept : db_(std::exchange(o.db_, nullptr)) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    ~Transaction() {
        if (db_) {
            try { db_->rollback(); } catch (...) { /* a destructor must not throw: log and move on */ }
        }
    }

    // Reports failure: this is where errors are allowed to surface.
    void commit() {
        Db* d = std::exchange(db_, nullptr);           // disarm FIRST: if commit throws, we must not also roll back
        d->commit();
    }
};

// A connection pool: RAII where "release" means "give back", not "destroy".
struct Conn { int id; };
class Pool {
    std::vector<std::unique_ptr<Conn>> idle_;
public:
    explicit Pool(int n) { for (int i = 0; i < n; ++i) idle_.push_back(std::make_unique<Conn>(Conn{i})); }
    struct Returner {
        Pool* pool;
        void operator()(Conn* c) const noexcept { pool->idle_.emplace_back(c); }   // back to the pool
    };
    using Lease = std::unique_ptr<Conn, Returner>;
    Lease acquire() {
        Conn* c = idle_.back().release();
        idle_.pop_back();
        return Lease(c, Returner{this});
    }
    std::size_t idle() const { return idle_.size(); }
};

int main() {
    Db db;
    {                                                  // success: explicit commit
        Transaction tx(db);
        db.exec("INSERT 1");
        tx.commit();
    }
    try {                                              // failure: an exception; no commit
        Transaction tx(db);
        db.exec("INSERT 2");
        throw std::runtime_error("constraint violated");
    } catch (const std::exception&) {}
    assert((db.log == std::vector<std::string>{"BEGIN", "INSERT 1", "COMMIT",
                                                 "BEGIN", "INSERT 2", "ROLLBACK"}));

    Pool pool(2);
    {
        auto a = pool.acquire();
        auto b = pool.acquire();
        assert(pool.idle() == 0);
    }                                                  // both leases return their connections
    assert(pool.idle() == 2);
}
```

Notice the design decisions: `commit()` **disarms before it acts** (so a throwing commit does not trigger a second, nonsensical rollback); the destructor catches everything; and the pool's deleter is *stateful* (it needs `Pool*`), so the `unique_ptr` is two pointers wide: that is exactly the cost Experiment 6 predicts, and now it buys something. The lease **must not outlive the pool**: RAII makes release automatic, but it does not make *lifetimes of different owners* compose. That needs a design decision, not a language feature.

### 10.3 Sockets, GPUs and the `close()` that can fail

- **Sockets and files:** `close()` can report a deferred write error (notably on NFS). A destructor can only log it. Provide `close()` returning `std::expected<void, std::error_code>` for code that cares ([Chapter 23](../part-09-error-handling/23-expected.md)). On Linux, never retry `close()` on `EINTR`: the descriptor is already released, and a retry may close *someone else's* newly opened fd.
- **GPU resources (Vulkan, CUDA):** handles depend on a parent (`VkBuffer` on `VkDevice`), so **destruction order is a correctness issue**. Vulkan-Hpp's `UniqueHandle` carries the device in a stateful deleter. And **never** release GPU resources in *static* destructors: the CUDA runtime may already be shutting down, and `cudaFree` then fails with `cudaErrorCudartUnloading`. Shut down explicitly ([Chapter 3, §10](03-lifetime-and-storage.md)).
- **Database connections:** RAII "release" is *return to the pool*, not close (above).

---

## 11. Failure modes

| Mistake | Consequence | Fix |
|---|---|---|
| **Throwing destructor** | `std::terminate` (Experiment 3) | Catch inside; expose `close()`/`commit()` that reports |
| **Unnamed guard**: `Guard{m};` / `Guard(m);` | No protection (Experiment 5) | Name it; `[[nodiscard]]`; clang-tidy `bugprone-unused-raii` |
| **Copyable handle** with no `dup` semantics | Double `close()`: closes someone else's descriptor | `= delete` copy; make it move-only |
| **Move leaves the source "valid and owning"** | Double release | `std::exchange(o.h, empty)` in every move |
| **Self-move-assignment** | `reset()` then adopt the now-reset handle: leak or double-free | `if (this != &o)` in move assignment |
| **Owning two raw resources in one class** | Constructor throws after the first: leak, and `~T` never runs ([Ch. 3, Exp. 1](03-lifetime-and-storage.md)) | One RAII member per resource |
| **Releasing before an exception can occur**: `delete p; … p = nullptr;` | A throw between leaves a dangling pointer | Reset the handle in the same statement as the release |
| **`shared_ptr` used only to get a deleter** | Ownership becomes *shared*, so lifetime becomes unpredictable | `unique_ptr<T, D>` |
| **RAII object in a global/static** | Destruction order; I/O in static destructors ([Ch. 3](03-lifetime-and-storage.md)) | Scoped ownership in `main`; explicit shutdown |
| **A destructor that blocks** (`join`, flush, network) | Latency spikes where cleanup is "invisible" | Document; expose explicit async `shutdown()` |
| **Capturing a guard by reference in a lambda that outlives it** | The guard's scope ends; the lambda dangles | Capture by value/move |
| **`noexcept(false)` move operations** | `std::vector` copies your handles during growth (or cannot, if non-copyable) | Always `noexcept` moves |
| **Using RAII where `exit()`, `_exit`, `abort` or `longjmp` follow** | Destructors never run | Don't mix; or flush explicitly before |
| **Forgetting that the guard's callable can throw** | `terminate` from a destructor | Mark callables `noexcept`; assert on it |

---

## 12. Exercises

1. **`UniqueFile`.** Wrap `FILE*` as `std::unique_ptr<FILE, FcloseDeleter>`, then as a hand-written class. Compare `sizeof`, assembly of the destructor, and behaviour on `fclose` failure.
2. **`UniqueFd` complete.** Extend Experiment 1's class with `swap`, `operator==`, a `dup()`-based `clone()` that returns `std::expected<UniqueFd, std::error_code>`, and an `std::hash` specialization. Test with a `std::vector<UniqueFd>` that grows. Why must the move constructor be `noexcept`?
3. **Convert `goto cleanup`.** Take a 100-line C function with `goto cleanup` (the OpenSSL/kernel style). Convert it to RAII. Count the branches before and after.
4. **Strong-guarantee assignment.** Write `Buffer` (owning `char*`, `size_t`) with copy-assignment that gives the **strong guarantee** (copy-and-swap, then without the extra copy for the self-assignment case).
5. **Nested transactions.** Extend `Transaction` with *savepoints*: `Transaction::nested()` returns an object that rolls back to a savepoint unless committed. Test with injected exceptions at every statement.
6. **Cost of `lock_guard`.** Compare the assembly of a function using `std::lock_guard` and one calling `lock()`/`unlock()` manually, under `-O2`. Then add a function that may throw between them. What does the manual version do (or fail to do) that the guard does?
7. **Catch the unnamed guard.** Run `clang-tidy -checks='bugprone-unused-raii' file.cpp` on Experiment 5. Make your own guard type `[[nodiscard]]` on the constructor (C++20) and confirm GCC and Clang warn.

---

## 13. Challenge: `UniqueResource<R, D>`

Library Fundamentals TS v3 specifies `std::experimental::unique_resource`: a generic RAII wrapper for resources whose "invalid" value is *not* `nullptr`: file descriptors, `HANDLE`s, `GLuint`s, `SOCKET`s. Implement your own:

```cpp
auto fd = make_unique_resource_checked(::open("x", O_RDONLY), -1, [](int f) { ::close(f); });
```

Requirements:

- stores the resource, the deleter, and an *engaged* flag; `[[no_unique_address]]` on the deleter
- `release()`, `reset(r)`, `get()`, `get_deleter()`, `operator*`/`operator->` for pointer-like resources
- **the constructor must not leak**: `UniqueResource(R r, D d)` — if copying or moving `d` throws, it must call `d(r)` before propagating (this is the hard part; the TS specifies it precisely)
- the `_checked` factory produces a *non-owning* (disengaged) object if the resource equals the invalid value, so that the deleter is never called on `-1`
- move-only; moved-from is disengaged
- `sizeof(UniqueResource<int, EmptyLambda>)` equals `2 * sizeof(int)` (resource + flag, padded). Explain the number, and then show how to get it down to `sizeof(int)` with a *sentinel* variant. What does this change about the empty-state semantics?

Then compare against `<experimental/scope>`'s version on your compiler (`#include <experimental/scope>`, `-std=c++20`), and report any behavioural difference you find.

---

## 14. Knowledge check

1. State the invariant RAII maintains. Why does it hold for exceptional exits and not only for `return`?
2. Why must a destructor not throw? Give *two* distinct reasons (one of them involves unwinding).
3. List the five questions to answer when designing a handle type.
4. Why is `std::lock_guard<std::mutex>{m};` a bug, and why is `std::lock_guard<std::mutex>(m);` a *different* bug?
5. What does `std::uncaught_exceptions()` return, and why is it better than `std::uncaught_exception()` for building `scope_fail`?
6. A class has two `char*` members, both allocated with `new` in its constructor. The second `new` throws. What leaks? What is the one-line structural fix?
7. `unique_ptr<FILE, int(*)(FILE*)>` vs `unique_ptr<FILE, FcloseDeleter>`: what are the two costs of the first, and what principle does this illustrate?
8. Why must `commit()` in a `Transaction` disarm *before* calling the database?
9. On Linux, should you retry `close(fd)` when it returns `-1` with `EINTR`? Why?
10. When is RAII the *wrong* tool, and what do you use instead?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Every acquired resource has exactly one owner at all times and is released exactly once. It holds on exceptional exits because **destructors of fully constructed automatic objects run during stack unwinding**. The compiler emits landing pads that call them, so the set of exits is computed, not hand-maintained.
2. (a) Destructors are implicitly `noexcept`, so an escaping exception calls `std::terminate`. (b) Even a `noexcept(false)` destructor that throws *while another exception is propagating* causes `std::terminate`, since two exceptions cannot be in flight in the same unwinding. Also: a throwing destructor leaves objects half-destroyed, which makes recovery impossible.
3. Empty state? Copyable (and what copying means)? What move leaves behind? What if release fails? Does destruction block or take significant time?
4. `Guard{m};` creates a *temporary* that locks and unlocks within the same statement, protecting nothing. `Guard(m);` is parsed as a *declaration* of a variable named `m`, which hides the mutex (and either fails to compile or default-constructs a guard that never locked anything).
5. The number of exceptions thrown and not yet caught on the current thread. A guard records it at construction and compares at destruction. The boolean version cannot tell "I am being destroyed *because* of an exception" from "I was created and destroyed *during* unwinding, due to some other exception", so it would trigger rollbacks spuriously.
6. If the second `new` throws, the first allocation leaks, because the constructor never completed so `~T` never runs. Structural fix: make each member a `std::unique_ptr<char[]>` (or `std::string`/`std::vector<char>`); then the first member's destructor runs automatically.
7. The function-pointer deleter must be *stored* (`sizeof` doubles from 8 to 16) and called indirectly (harder to inline). Principle: zero-overhead abstractions require that the *type* carries the information (a stateless functor type), not a runtime value.
8. If `db->commit()` throws and the object is still armed, its destructor would then run `rollback()` on a transaction whose commit status is unknown or already partially applied. Disarming first guarantees that the destructor does nothing after commit has been attempted; the caller sees the exception and decides.
9. **No.** On Linux the descriptor is released even when `close()` fails with `EINTR`; retrying risks closing a different file that reused the same descriptor number (a race in multithreaded programs). (This differs from some other Unixes, so POSIX leaves it unspecified.)
10. When the cleanup must be **asynchronous, long-running, fallible and reported**, or must happen at a specific *global* point rather than at scope exit: orderly network shutdown, flushing to remote storage, shutting down GPU contexts or thread pools in an application with static objects. Use an **explicit** `shutdown()`/`close()` with error reporting, with RAII only as the safety net that logs "forgot to close".

</details>

---

[← Previous: Chapter 3](03-lifetime-and-storage.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 5 — Value categories →](../part-03-value-categories-and-move/05-value-categories.md)
