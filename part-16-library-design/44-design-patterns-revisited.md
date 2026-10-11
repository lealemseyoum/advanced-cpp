# Chapter 44 — Design Patterns Revisited

> **Part XVI · Modern library design** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 19 (runtime polymorphism)](../part-08-polymorphism/19-runtime-polymorphism.md), [Chapter 20 (static polymorphism)](../part-08-polymorphism/20-static-polymorphism.md), [Chapter 21 (type erasure)](../part-08-polymorphism/21-type-erasure.md), [Chapter 13 (`optional`, `variant`, `any`, `expected`)](../part-05-standard-library/13-optional-variant-any-expected.md), [Chapter 42 (API design)](42-api-design.md) &nbsp;|&nbsp; **Standards:** `std::variant`/`visit` C++17, concepts C++20, `move_only_function` C++23 ⚖️ &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, ASan, Google Benchmark

[← Previous: Chapter 43](43-header-and-library-architecture.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 45 — C compatibility →](../part-17-c-and-python-interop/45-c-compatibility.md)

---

**In one sentence:** most of the GoF patterns are workarounds for the lack of first-class functions, closed sum types, value semantics and generic programming in 1994's C++ and Smalltalk; modern C++ offers cheaper, safer and shorter ways to express the same *intent*, so the useful question about each pattern is not "how do I implement it?" but **"which of the six mechanisms (inheritance, templates, concepts, variants, type erasure, function objects) fits this particular problem?"**

**By the end of this chapter you can:**

- recognise the *intent* behind a classic pattern and pick among the six modern mechanisms for it
- implement Visitor, Strategy, Observer, State, Command and Singleton in modern C++ and show the failure modes of each
- reason about the **expression problem** (adding types vs adding operations) to choose between virtual functions and `variant`
- measure dispatch costs instead of repeating folklore
- say, for each pattern, whether modern C++ eliminates it (often yes), reduces it (often), or leaves it alone (sometimes)

---

## 1. Problem

*Design Patterns* (Gamma, Helm, Johnson, Vlissides, 1994) catalogued 23 recurring solutions, in a world with **C++98 and Smalltalk**: no lambdas, no move semantics, no `variant`, no templates-as-policy culture, and objects as the main unit of abstraction. Patterns were a *vocabulary* for design intent, and a *recipe* for implementing it with classes and inheritance.

Thirty years later, three things changed:

| Then | Now |
|---|---|
| Behaviour is carried by objects with virtual methods | Behaviour is carried by **callables** (lambdas, function objects) |
| "One of several types" means a base class and subclasses | **`std::variant`** is a closed sum type; `std::visit` is the exhaustive dispatcher |
| Generic code means templates with SFINAE | **Concepts** state the requirement; static polymorphism is first-class |
| Value semantics were error-prone; objects lived on the heap behind base pointers | **Value semantics, move semantics, RAII**; heap allocation is a choice |
| Sharing meant raw pointers | **Ownership types** (`unique_ptr`, `shared_ptr`, `weak_ptr`) and **views** state it |

> [!IMPORTANT]
> This chapter does **not** say "patterns are dead". The *intent* of Observer, Command, Strategy remains and the words remain useful in code review. What changed is the **cheapest correct implementation**, and the cases where the original recipe (an abstract base class per pattern participant) is now the *wrong* default.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1977 | Christopher Alexander's *A Pattern Language* (architecture) inspires the idea |
| 1994 | *Design Patterns* (GoF): 23 patterns, examples in C++ and Smalltalk |
| 1996 | Peter Norvig, *Design Patterns in Dynamic Languages*: 16 of 23 patterns are "invisible or simpler" in Lisp/Dylan, thanks to first-class functions and types |
| 2001 | Alexandrescu, *Modern C++ Design*: policy-based design replaces Strategy/Template Method with templates |
| 2011 | C++11 lambdas, `std::function`, move semantics, smart pointers |
| 2017 | `std::variant`, `std::visit`, structured bindings, `if constexpr` |
| 2020 | Concepts, ranges (Iterator is now a language/library concept), `consteval` |
| 2021–23 | *Sean Parent: "Inheritance Is The Base Class of Evil"* and "Better Code" talks popularise value semantics and type erasure as the default polymorphism; `std::move_only_function`, `std::expected` |
| 2020s | Functional-style C++: Visitor ≈ `visit`, Command ≈ closure, Strategy ≈ callable, Iterator ≈ range |

---

## 3. Modern solution

The six mechanisms, with their defining property. This table is the chapter's backbone.

| Mechanism | Set of types | Binding time | Extensible by | Cost | Good for |
|---|---|---|---|---|---|
| **Inheritance + `virtual`** | Open (anyone can derive) | Run time | New *types* (derive) | vptr per object, indirect call, usually heap + pointer | Plug-in boundaries, framework callbacks, **ABI-stable** interfaces |
| **Templates** | Open (any type that fits) | Compile time | New types *and* operations | Code per instantiation, no indirection, compile-time errors | Hot paths, policies, containers |
| **Concepts** | Open (named requirement) | Compile time | As templates | None at run time | Documenting and enforcing the template contract |
| **`std::variant`** | **Closed** (listed alternatives) | Run time (a tag test/jump) | New *operations* (new visitors); adding a type means editing the `variant` | Value semantics, inline storage of largest alt, no heap | Closed hierarchies: AST nodes, messages, states |
| **Type erasure** (`std::function`, `any`, custom `Shape`) | Open | Run time | New types, with value semantics | One indirection, maybe an allocation (SBO) | Heterogeneous collections of *values*; callbacks; hiding templates behind a stable type |
| **Function objects / lambdas** | Open | Compile time (as template parameter) or run time (via `function`) | Anything | Zero if inlined | Strategy, Command, Observer slots, algorithms |

How to choose, in five questions:

```text
 1. Is the set of types CLOSED and known?                    → variant
 2. Does it cross a stable binary/plug-in boundary?           → virtual (or a C API; Chapter 37)
 3. Is it a single operation (callable)?                      → lambda / function_ref / move_only_function / template F
 4. Hot path and types known at compile time?                 → template + concept
 5. Heterogeneous collection of values, open set of types?    → type erasure (value-semantic wrapper)
```

---

## 4. Mental model

### The expression problem decides between virtual functions and `variant`

You have **types** (Circle, Rect, Tri) and **operations** (area, draw, serialise). Every design chooses which axis is easy to extend:

```text
                         add a NEW TYPE              add a NEW OPERATION
   virtual functions     one new class               edit every class  (or add a Visitor, which re-creates the problem)
   variant + visit       edit the variant alias      one new visitor
                         and every visitor           (nothing else changes)
```

- A **plug-in system** or an open framework (new shapes added by third parties, without recompiling the core) needs the first row.
- A **compiler AST**, a **message protocol**, a **parser's token set**, a **state machine** has a fixed set of types and an ever-growing set of operations (print, type-check, optimise, serialise). That is the second row, and the classic Visitor pattern (two virtual calls, an `accept` in every class) is a heavyweight way to simulate it.

### Patterns by what they really encode

| Pattern | The intent in one phrase | The modern carrier |
|---|---|---|
| **Strategy** | Swap an algorithm | A callable (template parameter or `function`) |
| **Template Method** | Fixed skeleton, variable steps | Non-virtual interface (NVI) with hooks, or a template taking callables |
| **Observer** | Notify subscribers of an event | A signal holding callables + RAII `Connection` |
| **Command** | Reify an action (queue, undo, log) | A closure (`move_only_function`), possibly with an undo closure |
| **State** | Behaviour depends on a mode | `variant` of states + `visit` (or `enum` + `switch`) |
| **Visitor** | New operation over a closed set of types | `variant` + `visit` + overloaded lambdas |
| **Iterator** | Traverse without exposing structure | Ranges/iterators (a language concept now) |
| **Decorator** | Add behaviour around an object | Function composition / wrappers templated on the wrapped type |
| **Factory / Abstract Factory** | Decouple creation from use | A function returning a value / `unique_ptr`; a map of callables |
| **Singleton** | One instance, global access | Rarely right: pass the dependency; a function-local `static` if you must |
| **Adapter** | Make X look like Y | A wrapper type, or a concept satisfied through a free function (ADL) |
| **Builder** | Build a complex object stepwise | Designated-initializer options structs (Chapter 42) or fluent API |
| **Proxy / Flyweight / Memento** | Control access / share state / snapshot | Smart pointers, `shared_ptr`, value copies (value semantics gives Memento for free) |
| **Pimpl, Bridge** | Hide implementation | Chapter 43 |

---

## 5. Language rules

| Rule | Text | Why it matters for patterns |
|---|---|---|
| **`std::visit` is exhaustive** ⚖️ [variant.visit] | The visitor must be callable with *every* alternative, or the program is ill-formed | A forgotten case is a **compile error**, not a run-time surprise (Experiment 1) |
| **`std::variant` storage** ⚖️ | Holds one alternative in inline storage of the largest alternative's size, plus a discriminator; `valueless_by_exception` is possible if an assignment throws | No heap; `sizeof(variant)` is the max, so keep alternatives similar in size |
| **Virtual dispatch** ⚖️ | Selected by the dynamic type; mechanism (vtable) is 🧩 ABI-defined | Cost and layout in Chapters 19, 37 |
| **Closures** ⚖️ [expr.prim.lambda] | A lambda is an object of unique unnamed class type with `operator()`; captures are members | A lambda is a Strategy object with no boilerplate; a capture-by-reference of a local is a lifetime hazard |
| **`std::function` / `move_only_function`** ⚖️ | Type-erased callable; `function` requires copyable targets; `move_only_function` (C++23) allows move-only captures and has const/noexcept/ref-qualified signatures | Right type for stored callbacks and Commands |
| **Static local initialisation** ⚖️ [stmt.dcl]/4 | Thread-safe since C++11 ("magic statics"): initialisation runs once, others block | Meyers Singleton is safe to *create* (Experiment 5) |
| **Destruction order of statics** ⚖️ | Reverse order of completed construction; across TUs unspecified for namespace-scope objects | Singletons used from other statics' destructors can already be dead |
| **Customisation points / ADL** ⚖️ | Free-function lookup at the call site | Adapter without inheritance: make your type satisfy the concept by providing `begin(x)`, `swap(a,b)`, `draw(x, out)` |
| **Concepts** ⚖️ C++20 | Named, composable requirements | Replace "abstract base class used as an interface contract" in generic code |

### Layer check

| Layer | Question |
|---|---|
| **C++ standard** | Which mechanisms are in the language/library, and what do they promise (exhaustiveness, thread-safe statics)? |
| **Compiler** | Can it inline through the dispatch? (Templates/lambdas: yes; virtual: sometimes; `function`: rarely; Experiment 2: **GCC and Clang disagree**) |
| **ABI** | vtable layout and `function`'s storage are ABI facts; a `variant` in a public interface freezes its alternatives |
| **OS / CPU** | Indirect-call prediction, branch mispredicts on a random tag (Experiment 1), cache behaviour of pointer-chasing vs inline storage |

---

## 6. Implementation model

### What each mechanism compiles to

```text
 virtual      obj ─► [vptr] ─► vtable ─► slot k ─► code        (2 dependent loads + indirect call)
 variant      obj = [ index | storage ]   visit = jump table on `index`, then a direct (inlinable) call
 enum+switch  obj = [ kind | fields ]     same jump table, no library layer
 function     obj = [ manager | invoker | storage ]   call = indirect call through `invoker`
 template/lambda  call site = the callee body, inlined
```

`std::visit` on a variant with N alternatives compiles to an indexed jump (or, for two-way, a compare-and-branch). GCC and Clang both lower it to a switch on `index()`; the visitor's calls are direct and inlinable, which is the real difference from virtual dispatch.

### The Observer lifetime problem

An observer registration stores a callable that may capture `this`. If the observer dies first, the stored callable dangles. Three designs: (1) the subject returns an **RAII `Connection`** that unregisters in its destructor (Experiment 3), (2) observers hold `shared_ptr` and the subject stores `weak_ptr`s, (3) a framework tracks both ends (Qt's `QObject` connections are auto-disconnected on destruction).

---

## 7. Experiments

Code in [`code/ch44/`](code/ch44/) (`dispatch.cpp`, `run.sh`). **GCC 14.2 and Clang 18.1, `-O2`, 2-vCPU VM, `libbenchmark` DEBUG build (see Chapter 39).**

### Experiment 1 ✅/🔧: Visitor: the compiler checks exhaustiveness; and what it costs

**(a) Exhaustiveness**: forget an alternative:

```cpp
// @test fail -std=c++23 err=invoke_result
#include <variant>
struct A {}; struct B {}; struct C {};
int f(const std::variant<A, B, C>& v) {
    struct Visitor { int operator()(A) const { return 1; } int operator()(B) const { return 2; } };   // forgot C
    return std::visit(Visitor{}, v);
}
```

It does not compile, but note the diagnostic: GCC prints `no type named 'type' in 'struct std::invoke_result<…Visitor, const C&>'` buried in `<variant>` internals: **correct and unfriendly**. (A `static_assert` on `std::is_invocable_v` per alternative gives a readable message.) Compare the classic Visitor: add a new type, and the compiler also tells you, via the pure virtual `visit(NewType&)` in every visitor, but only after you wrote the `accept` boilerplate in the new class.

**(b) Dispatch cost.** 65 536 shapes in random order of three kinds, summing `area()`:

| Mechanism | GCC 14 (M items/s) | Clang 18 (M items/s) |
|---|---:|---:|
| `virtual` (vector of `unique_ptr<Shape>`) | 114 | 110 |
| `std::variant` + `visit` | **168** | **173** |
| `enum` tag + `switch` | 164 | 166 |
| `std::function<double()>` per object | 146 | 151 |

With a **random** tag sequence, branch misprediction dominates every mechanism (they all land within 1.5× of each other, at ~6-9 ns per element). `variant` and the hand-written `switch` are identical, as expected (the same jump table); `virtual` is about 1.5× slower because each element is a separate heap object reached through a pointer, then through a vtable, then an indirect call; `function` is in between. **The surprising result is how small the spread is**: for heterogeneous collections the dominant cost is the unpredictable branch, not the abstraction. (If the kinds were *sorted* so the tag sequence is predictable, the gap would widen; I did not measure that.)

> **Opinion.** **For a closed set of types, `variant` + `visit` beats Visitor on every axis**: no `accept` boilerplate, no heap per element, values rather than pointers, exhaustive checking, no virtual calls, and the same or better speed. Use virtual functions and Visitor only when the set of types must be **open** (plug-ins, third-party extension) or must cross an ABI boundary.

### Experiment 2 🔧: Strategy: four ways to inject an algorithm

A loop applying `x + 1` to 65 536 ints through (a) a virtual `IStrategy`, (b) `std::function<int(int)>`, (c) a function pointer, (d) a template parameter (lambda):

| Strategy as… | GCC 14 | Clang 18 |
|---|---:|---:|
| `virtual` interface | 1 497 M/s | 797 M/s |
| `std::function` | 776 M/s | 666 M/s |
| function pointer (`noinline` callee) | 796 M/s | 580 M/s |
| **template parameter / lambda** | 1 665 M/s | **4 359 M/s** |

Read it carefully, because the compilers disagree:

- **Templates/lambdas win everywhere**, and on Clang by 5.5-7.5×: the callee is inlined into the loop, which Clang then vectorises. (GCC's `-fopt-info-vec` reports no vectorised loop there at `-O2`, while Clang reports `vectorized loop (vectorization width: 2, interleaved count: 2)`; so GCC's template result is the inlined-scalar case.) This is the gain that **inlining** buys (Chapter 41), not a property of "templates" as such.
- **GCC's virtual call is as fast as the template** here (1 497 vs 1 665 M/s): GCC speculatively devirtualised the call (Chapter 41, Experiment 3) because `AddOne` is the only implementation visible. Clang at `-O2` did not (797 M/s). A claim "virtual is X× slower than a template" is a claim about a **compiler and a program**, not about C++.
- **`std::function` and function pointers** both stop inlining: ~2× slower than the inlined version on GCC, 5.5-7.5× on Clang.

A Strategy that is chosen once and then called a million times wants to be a **template parameter**; one that is chosen at run time from a configuration, and called rarely, wants to be a `std::function` or `move_only_function` (the cost is invisible when each call does real work).

### Experiment 3 ✅: Observer: lifetime, the bug and the fix

**The textbook implementation (a vector of `std::function`s)** and its bug: the observer registers a lambda that captures `this`; the observer is destroyed before the subject emits again.

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=stack-use-after-scope
#include <cstdio>
#include <functional>
#include <vector>
struct Signal {
    std::vector<std::function<void(int)>> slots;
    void connect(std::function<void(int)> f) { slots.push_back(std::move(f)); }
    void emit(int x) const { for (auto& s : slots) s(x); }
};
struct Logger {
    int calls = 0;
    explicit Logger(Signal& s) { s.connect([this](int x) { ++calls; std::printf("got %d\n", x); }); }
};
int main() {
    Signal sig;
    { Logger log(sig); sig.emit(1); }     // Logger destroyed, its slot remains
    sig.emit(2);                          // calls a lambda whose `this` dangles
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: stack-use-after-scope on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 4 at 0xADDR thread T0
    #0 0xADDR in Logger::Logger(Signal&)::{lambda(int)#1}::operator()(int) const snippet.cpp:12
    [... libstdc++ internal frames elided ...]
    #5 0xADDR in Signal::emit(int) const snippet.cpp:8
    #6 0xADDR in main snippet.cpp:17

Address 0xADDR is located in stack of thread T0 at offset 48 in frame
    #0 0xADDR in main snippet.cpp:14

  This frame has 2 object(s):
    [48, 52) 'log' (line 16) <== Memory access at offset 48 is inside this variable
    [64, 88) 'sig' (line 15)
HINT: this may be a false positive if your program uses some custom stack unwind mechanism, swapcontext or vfork
      (longjmp and C++ exceptions *are* supported)
SUMMARY: AddressSanitizer: stack-use-after-scope snippet.cpp:12 in Logger::Logger(Signal&)::{lambda(int)#1}::operator()(int) const
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

ASan reports a read of the dead `Logger` (a stack variable that went out of scope) through the dangling `this`. Nothing in the *interface* (`connect(std::function<void(int)>)`) hints at the contract "the callable must outlive the subject, or be removed first".

**The fix: a connection handle with RAII.** `connect` returns a `Connection` (move-only, `[[nodiscard]]`) whose destructor removes the slot; the observer owns its `Connection`, so the registration cannot outlive the observer:

```cpp
// @test run -std=c++23 -fsanitize=address,undefined
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class Signal {
    struct Slot { int id; std::function<void(int)> fn; };
    std::vector<Slot> slots_; int next_ = 0;
public:
    class Connection {                                          // RAII: disconnects on destruction
        Signal* s_ = nullptr; int id_ = -1;
    public:
        Connection() = default;
        Connection(Signal* s, int id) : s_(s), id_(id) {}
        Connection(Connection&& o) noexcept : s_(std::exchange(o.s_, nullptr)), id_(o.id_) {}
        Connection& operator=(Connection&& o) noexcept { reset(); s_ = std::exchange(o.s_, nullptr); id_ = o.id_; return *this; }
        ~Connection() { reset(); }
        void reset() { if (s_) { s_->disconnect(id_); s_ = nullptr; } }
    };
    [[nodiscard]] Connection connect(std::function<void(int)> f) { slots_.push_back({next_, std::move(f)}); return {this, next_++}; }
    void emit(int x) const { for (auto& s : slots_) s.fn(x); }
private:
    void disconnect(int id) { std::erase_if(slots_, [&](const Slot& s) { return s.id == id; }); }
};

struct Logger {
    Signal::Connection conn;
    explicit Logger(Signal& s) : conn(s.connect([this](int x) { std::printf("logger got %d (this=%s)\n", x, this ? "ok" : "null"); })) {}
};

int main() {
    Signal sig;
    {
        Logger log(sig);
        sig.emit(1);
    }                    // Logger destroyed: Connection disconnects the slot capturing `this`
    sig.emit(2);         // safe: nothing left to call
    std::puts("done");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
logger got 1 (this=ok)
done
```

Two limitations the sketch still has (state them in a design review): it is **not thread-safe**, and **disconnecting during `emit`** (a slot that removes itself) invalidates the loop (`emit` should iterate over a copy or defer removals). Both are why production libraries (Qt, Boost.Signals2, `sigslot`) are bigger than 30 lines.

> **Opinion.** **Observer in modern C++ is "a list of callables plus an RAII handle".** The abstract `Observer` base class with a virtual `update()` is obsolete for in-process notification: it forces a class per subscriber, hides what changed, and says nothing about lifetime. Whatever you build, the API must answer one question in its signature: *what happens when the subscriber dies?*

### Experiment 4 ✅: State: `variant` of states, exhaustive and heap-free

```cpp
// @test run -std=c++23
#include <cstdio>
#include <variant>
struct Idle {}; struct Running { int ticks; }; struct Done { int total; };
using State = std::variant<Idle, Running, Done>;
struct Start {}; struct Tick {}; struct Stop {};
State on(Idle, Start) { return Running{0}; }
State on(Running r, Tick) { return r.ticks >= 2 ? State{Done{r.ticks + 1}} : State{Running{r.ticks + 1}}; }
State on(Running r, Stop) { return Done{r.ticks}; }
template <class S, class E> State on(S s, E) { return s; }          // everything else: ignore the event
template <class E> State step(const State& s, E e) { return std::visit([&](auto cur) { return on(cur, e); }, s); }
const char* name(const State& s) { return std::visit([](auto x) { return std::is_same_v<decltype(x), Idle> ? "Idle" : std::is_same_v<decltype(x), Running> ? "Running" : "Done"; }, s); }
int main() {
    State s = Idle{};
    s = step(s, Tick{});  std::printf("%s\n", name(s));
    s = step(s, Start{}); std::printf("%s\n", name(s));
    s = step(s, Tick{}); s = step(s, Tick{}); s = step(s, Tick{}); std::printf("%s\n", name(s));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Idle
Running
Done
```

Per-state data (`ticks`, `total`) lives *in the state type*, so it cannot be read in the wrong state (a classic GoF State has the context object hold the union of all state data). Transitions are functions `on(State, Event) → State`, which are pure, trivially testable and listable (`rg "State on("`). The catch-all template `on(S, E)` makes "ignore unknown events" the default; if you would rather make unhandled combinations a compile error, delete the template and add the missing overloads (the compiler will list them).

### Experiment 5 ✅: Command and Singleton

**Command with undo = a pair of closures.** `std::move_only_function<void()>` (C++23, available in libstdc++ 14) is a type-erased closure that need not be copyable, which makes it the right Command type when the action owns something (a file handle, a `unique_ptr`):

```cpp
// @test run -std=c++23
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
struct Editor {
    std::string text;
    std::vector<std::move_only_function<void()>> undo;
    void insert(std::string s) {
        auto n = s.size();
        text += s;
        undo.push_back([this, n] { text.resize(text.size() - n); });
    }
    void undo_last() { if (!undo.empty()) { undo.back()(); undo.pop_back(); } }
};
int main() {
    Editor e; e.insert("hello"); e.insert(", world");
    std::printf("%s\n", e.text.c_str());
    e.undo_last(); std::printf("%s\n", e.text.c_str());
    e.undo_last(); std::printf("[%s]\n", e.text.c_str());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
hello, world
hello
[]
```

(There is no `Command` base class, no `ConcreteCommand` per action, and no `execute()`: the closure *is* the command, and the capture list documents what it needs. The `this` capture is the same lifetime hazard as in Experiment 3.)

**Singleton: what the language gives you, and the price.** The Meyers form is thread-safe by language rule:

```cpp
// @test asm -std=c++20 -O2 -fno-stack-protector filter=instance
struct Config { int v = 42; Config(); };
Config& instance() { static Config c; return c; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
instance():
	movzx	eax, BYTE PTR guard variable for instance()::c[rip]
	test	al, al
	je	.L2
	lea	rax, instance()::c[rip]
	ret
.L2:
	push	r14
	push	rbx
	lea	rbx, guard variable for instance()::c[rip]
	mov	rdi, rbx
	sub	rsp, 24
	call	__cxa_guard_acquire@PLT
	mov	edx, eax
	lea	rax, instance()::c[rip]
	test	edx, edx
	jne	.L15
	add	rsp, 24
	pop	rbx
	pop	r14
	ret
.L15:
	mov	rdi, rax
	mov	QWORD PTR 8[rsp], rax
	call	Config::Config()@PLT
	mov	rdi, rbx
	call	__cxa_guard_release@PLT
	mov	rax, QWORD PTR 8[rsp]
	add	rsp, 24
	pop	rbx
	pop	r14
	ret
.L6:
	mov	r14, rax
	jmp	.L5

instance() [clone .cold]:
.L5:
	mov	rdi, rbx
	call	__cxa_guard_abort@PLT
	mov	rdi, r14
	call	_Unwind_Resume@PLT
```

Every call to `instance()` first tests a guard byte (fast path: one load and compare); the first call goes through `__cxa_guard_acquire` / constructor / `__cxa_guard_release`, with `__cxa_guard_abort` on an exception. That is the *entire* cost, and it is small. The problems of Singleton are not performance: (1) **hidden dependency**: any function can silently use it, so signatures lie (Chapter 42); (2) **untestable** without a reset hook; (3) **destruction order**: other statics' destructors may touch a dead singleton; (4) **it hard-codes "one"** where a program later wants two (per-tenant, per-test).

> **Opinion.** **Prefer passing the dependency explicitly** (constructor parameter, reference). If a process-wide instance is genuinely required (a logger, a metrics registry), a function-local static returning a reference is fine, but the *consumers* should take the thing as a parameter or interface so tests and future code are not welded to it.

---

## 8. Assembly / runtime investigation

```bash
# How does visit compile? Look for the jump table on the variant index
g++-14 -O2 -std=c++20 -S -masm=intel -o - dispatch.cpp | grep -B2 -A12 'jmp.*\*' | head -40
#   expect:  movzx eax, BYTE PTR [rdi+24]   (the index)  ->  jmp [table+rax*8]   or a short compare chain for 3 alternatives

# Is the virtual call devirtualised?  (Chapter 41)
g++-14 -O2 -fopt-info-optimized-missed 2>&1 -c dispatch.cpp | grep -i devirt
clang++-18 -O2 -Rpass=wholeprogramdevirt -Rpass-missed=inline -c dispatch.cpp

# Size of the abstractions (value semantics: is anything on the heap?)
cat > sz.cpp <<'S'
#include <functional>
#include <variant>
#include <cstdio>
struct C{double r;}; struct R{double w,h;}; struct T3{double b,h;};
int main(){ std::printf("variant<C,R,T3>=%zu function=%zu move_only_function=%zu\n",
   sizeof(std::variant<C,R,T3>), sizeof(std::function<void()>), sizeof(std::move_only_function<void()>)); }
S
g++-14 -std=c++23 sz.cpp -o sz && ./sz          # measured here: variant<C,R,T3>=24  function=32  move_only_function=40 (bytes)

# Watch allocations for the type-erased version
valgrind --tool=dhat ./dispatch --benchmark_filter=BM_function 2>&1 | grep Total     # one allocation per object? (SBO: closures > 16 bytes allocate)

# Observer bugs: always test with sanitizers
g++-14 -std=c++23 -g -fsanitize=address,undefined o2.cpp -o o2 && ./o2
```

---

## 9. Implementation exercise

**Port a classic Visitor-based expression evaluator to `variant`.** Start with this classic code (an `Expr` base class with `Num`, `Add`, `Mul`, `Neg` subclasses and a `Visitor` interface with `visit(Num&)`, …, plus `Evaluator` and `Printer` visitors). Re-implement it with:

1. `struct Num{double v;}; struct Add; struct Mul; struct Neg;` where child nodes must be recursive: use `std::unique_ptr<Expr>` inside the nodes, or a flat `std::vector<Node>` with indices (an *arena*, Chapter 26).
2. `using Expr = std::variant<Num, Add, Mul, Neg>;` and `eval`, `print`, `simplify` as free functions with overloaded lambdas.
3. A benchmark (Chapter 39) of evaluation on a 1M-node tree for the classic and `variant` versions, and the number of allocations (`dhat`).
4. A new operation `to_rpn` added **without touching the existing code**; then a new node type `Pow` added, listing every place the compiler tells you to change.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <memory>
#include <variant>

struct Num { double v; };
struct Add; struct Mul; struct Neg;
using Expr = std::variant<Num, std::unique_ptr<Add>, std::unique_ptr<Mul>, std::unique_ptr<Neg>>;
struct Add { Expr l, r; };
struct Mul { Expr l, r; };
struct Neg { Expr e; };

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

double eval(const Expr& e) {
    return std::visit(overloaded{
        [](const Num& n) { return n.v; },
        [](const std::unique_ptr<Add>& a) { return eval(a->l) + eval(a->r); },
        [](const std::unique_ptr<Mul>& m) { return eval(m->l) * eval(m->r); },
        [](const std::unique_ptr<Neg>& n) { return -eval(n->e); },
    }, e);
}
```

The recursive case forces one allocation per interior node (a `unique_ptr` per child); the **arena/index** version (a `std::vector<Node>` with `int` child indices) removes them and improves locality, which is the lesson of Chapter 27 applied to ASTs. In the benchmark, compare allocations (`dhat`): the classic version has one per node, the `unique_ptr` variant one per interior node, the arena version one in total.

</details>

---

## 10. Real-world example

| Where | Pattern in modern clothes |
|---|---|
| **LLVM / Clang** | AST nodes are a closed hierarchy but implemented with *tagged classes and `dyn_cast`* (hand-rolled RTTI) and `switch` over `getKind()`: variant-style dispatch before `std::variant` existed; visitors are CRTP templates (`RecursiveASTVisitor`) |
| **Rust-influenced C++ code, `std::expected`, `std::optional`** | Sum types as the default for state and results |
| **Boost.Signals2, Qt signals/slots, `sigslot`** | Observer with connection handles; Qt auto-disconnects on `QObject` destruction (the missing lifetime half of Experiment 3) |
| **Asio, Folly, libunifex** | Completion handlers and sender/receiver chains replace Command/Observer with callables; `std::execution` (C++26 🟡) standardises senders |
| **`std::sort(…, comp)`, `std::ranges`** | Strategy and Iterator are built into the library's shape |
| **Game engines** | ECS (entity-component-system) replaces inheritance hierarchies of game objects: data-oriented composition (Chapter 27) beats `GameObject` base classes |
| **Qt** | Qt keeps the 1990s forms where the framework needs them: `QObject` base class with `virtual` event handlers (Template Method), `QAbstractItemModel` (Adapter/Observer), signals/slots (Observer, with run-time reflection through `moc`). Modern code on top of Qt uses lambdas as slots (`connect(btn, &QPushButton::clicked, this, [=]{ ... })`) and `std::variant` for message types, but a plug-in or model/view interface still wants a virtual base |

> **Opinion.** **Patterns stay valuable as a *vocabulary* ("this is an Observer", "that's a Command") and as a checklist of intent. They stop being valuable as a *class diagram you must draw*.** When someone proposes `AbstractFactory`, `StrategyContext`, `VisitorImpl`, ask which of the six mechanisms makes the class go away. In 2026 code, the answer is "a lambda", "a variant", or "a function that returns a value" more often than not.

---

### Verdict: does modern C++ eliminate the pattern?

| Pattern | Verdict | Replace with | Keep the classic form when |
|---|---|---|---|
| **Strategy** | **Eliminated** | Callable parameter (template, `function`, `function_ref`) | The strategy has several operations and state: then an interface is honest |
| **Template Method** | **Reduced** | NVI (public non-virtual calling private virtual hooks), or a function taking callables | A framework calls into your subclass (Qt event handlers) |
| **Observer** | **Reduced** | Signal of callables + RAII `Connection` | Cross-ABI plug-in notification |
| **Command** | **Eliminated** | Closure (`move_only_function`), plus an undo closure | Commands must be serialised/inspected (then a `variant` of command structs) |
| **State** | **Reduced** | `variant` of state types (+ `visit`), or `enum` + `switch` | States are open or user-supplied |
| **Visitor** | **Eliminated** (closed sets) | `variant` + `visit` | The type set is open (plug-ins), or it crosses an ABI boundary |
| **Iterator** | **Eliminated** (it's the language) | Iterators/ranges/`generator<T>` (C++23) | Never |
| **Decorator** | **Reduced** | Function composition, wrapper templates over the wrapped type, type-erased `Shape` wrappers | You need to decorate *run-time-chosen* implementations behind a stable interface |
| **Factory Method / Abstract Factory** | **Reduced** | A function returning a value or `unique_ptr`; a `map<string, function<…>>` registry | Plug-in registries; ABI-stable creation |
| **Singleton** | **Discouraged** | Dependency injection; function-local static if truly global | Process-wide infrastructure (logging), accessed through an interface |
| **Adapter** | **Reduced** | A concept + ADL customisation; a thin wrapper type | Wrapping a C API (Chapter 45) |
| **Builder** | **Eliminated** (mostly) | Options structs with designated initializers; named parameters via structs | Truly stepwise multi-phase construction |
| **Memento / Prototype** | **Eliminated** | Value semantics: copy the object (copy = snapshot, clone) | Polymorphic copy needed: `clone()` virtual |
| **Proxy / Flyweight** | **Reduced** | Smart pointers, `shared_ptr<const T>`, interning | Lazy loading, remote proxies |
| **Bridge / Pimpl** | **Retained** | Chapter 43 | ABI stability, compile-time insulation |
| **Chain of Responsibility, Mediator, Interpreter** | **Reduced** | A vector of callables; a dispatcher function; `variant` ASTs | Rarely changes |

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Visitor/`accept` boilerplate for a closed set** | Every new type edits N classes; two virtual calls per visit | `variant` + `visit` |
| **`variant` for an open set** | Every plug-in edits the central `using`; recompiles everything | Virtual interface or type-erased wrapper |
| **Huge `variant` alternatives** | `sizeof(variant)` = the largest alternative everywhere | Store big alternatives behind `unique_ptr`, or use an arena |
| **`visit` with a non-exhaustive generic lambda `[](auto&&)` as a catch-all** | New alternatives silently ignored | Prefer explicit overloads; use the catch-all only on purpose |
| **`valueless_by_exception` ignored** | `std::visit` throws `bad_variant_access` | Alternatives with `noexcept` moves; handle or forbid throwing assignment |
| **Observer capturing `this`/locals by reference** | Use-after-free after the observer dies (Experiment 3) | RAII connection; `weak_ptr` guard |
| **Observer callback re-entrancy** | Slot disconnects/connects during `emit` → iterator invalidation | Iterate a snapshot; defer modifications |
| **Observer cycles** (A observes B observes A) with `shared_ptr` | Leaks | `weak_ptr`; clear ownership direction |
| **`std::function` in hot loops** | No inlining; allocation for large captures | Template parameter / `function_ref` |
| **`std::function` with move-only captures** | Compile error (requires copyable) | `std::move_only_function` (C++23) |
| **Strategy classes with one method** | A class hierarchy for a lambda | Pass the callable |
| **Singleton for convenience** | Hidden coupling, untestable, shutdown-order bugs | Inject; if global, hide behind an interface |
| **Static-init order dependence among singletons** | Crash at exit or in static constructors | Function-local statics (construct on first use); explicit lifetime owner |
| **Deep inheritance to share code** | Fragile base classes, surprising overrides | Composition; templates; free functions |
| **Template Method with public virtuals** | Subclasses can call or skip steps arbitrarily | NVI: public non-virtual, private/protected virtual hooks |
| **Factory returning raw `new`** | Leaks | `unique_ptr` or value |
| **Over-applying a pattern** (AbstractFactoryFactory) | Indirection with no variation point | Delete it: add the pattern when the second variant exists |
| **Dropping virtual where ABI matters** | Plug-in interface changes break binaries | Keep a C-style or stable virtual interface at the boundary (Chapter 37) |

---

## 12. Exercises

1. **Strategy ladder.** Implement a `Sorter` with a pluggable comparison four ways (virtual, function pointer, `std::function`, template), then measure on 10⁶ ints with both compilers. Explain every gap using the assembly.
2. **Variant AST.** Complete §9. Add a `simplify` pass (constant folding) written as `variant -> variant`. Count allocations before and after moving to the arena layout.
3. **Exhaustiveness diagnostics.** Write a `visit_exhaustive(v, handlers...)` helper that `static_assert`s each alternative is handled with a readable message. Compare with the raw `std::visit` error from Experiment 1.
4. **Observer v2.** Extend the `Signal` of Experiment 3: thread-safe `emit`, safe self-disconnect during `emit`, `ScopedConnection` blocking/unblocking. Stress it with TSan.
5. **Command log.** Build an undo/redo stack from `move_only_function` pairs; then redo it with a `variant` of command structs (each with `apply` and `revert`) so that commands can be **serialised** to disk. Which representation lets you do that, and why?
6. **State machine DSL.** Generalise Experiment 4 into a `StateMachine<Transitions...>` template that rejects undefined transitions at compile time (use a concept or `static_assert`).
7. **Singleton audit.** Find three singletons in a codebase. For each, list who calls it and whether a parameter/interface would work. Write a test that is impossible with the singleton and trivial without it.
8. **Expression problem lab.** Implement the same "shapes + operations" design as (a) virtual functions, (b) Visitor, (c) `variant`. Add one new type and one new operation to each and count the files and lines touched.

---

## 13. Challenge: a plug-in-capable AST

Design the AST of a small expression language (numbers, variables, arithmetic, function calls, `let`) whose **core node set is a closed `variant`** (fast, exhaustive, value-semantic) but which also allows **third-party extension nodes** loaded as plug-ins. Constraints: the closed part has no virtual calls; the extension node is one `variant` alternative that holds a type-erased `ExtensionNode` (small-buffer, `unique_ptr` fallback) with a stable C-ABI vtable; every operation (`eval`, `print`, `type_check`) is a free function over the variant; plug-ins can add new *operations* only through a registry keyed by node type. Benchmark closed-node dispatch versus extension-node dispatch (Chapter 39), and write the one-page design note explaining which half of the expression problem each part solves.

---

## 14. Knowledge check

1. What is the expression problem and which two mechanisms sit on opposite sides of it?
2. Why is `variant` + `visit` usually better than the classic Visitor for a closed set of types?
3. What did Experiment 1 show about the *cost* of `virtual` vs `variant` vs `switch`, and why was the spread small?
4. Why did the same Strategy benchmark give different results on GCC and Clang?
5. What is the lifetime bug in an Observer that stores `std::function`s capturing `this`, and what is the structural fix?
6. How does `move_only_function` differ from `function`, and why does it suit Command?
7. What does a function-local static guarantee, and what does it not?
8. Name three patterns that are *eliminated* (not just reduced) by modern C++ features, and the feature responsible for each.
9. When should you *keep* an abstract base class and Visitor?
10. What is NVI, and which pattern does it modernise?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. The trade-off between easily adding new *types* and easily adding new *operations*. Virtual functions make adding types easy (derive) and operations hard; `variant`+`visit` make adding operations easy (a new visitor) and types hard (edit the variant and every visitor).
2. No `accept` boilerplate in every type; values stored inline instead of heap objects behind pointers; no virtual calls; exhaustive compile-time checking; adding an operation touches no existing code.
3. They were within ~1.5× (114 vs 168 M items/s); `variant` and `switch` were equal; `virtual` was slowest. Random tags make branch misprediction dominate every mechanism, which hides the abstraction differences.
4. GCC speculatively devirtualised the virtual call (so it matched the template), while Clang `-O2` did not; and Clang vectorised the inlined template loop while GCC did not. The results measure compiler decisions.
5. The slot outlives the observer, so `this` dangles when the subject emits again. Fix: `connect` returns an RAII `Connection` owned by the observer (destruction disconnects), or use `weak_ptr` guards, or a framework that tracks both ends.
6. `function` requires a copyable target; `move_only_function` accepts move-only closures (owning `unique_ptr`s, file handles), supports const/noexcept/ref qualifiers in the signature, and is not copyable. Commands often own resources.
7. Thread-safe initialisation exactly once on first use (magic statics, C++11); it does not guarantee the object is still alive during other statics' destruction, nor that it is replaceable in tests.
8. Strategy → callables/lambdas; Command → closures (`move_only_function`); Visitor → `variant`+`visit`; Iterator → iterators/ranges/generators; Builder → designated initializers; Memento/Prototype → value semantics.
9. When the set of types must be open (plug-ins, third-party extension), when the interface crosses a stable ABI boundary, or when a framework calls into subclasses (Qt event handlers).
10. The Non-Virtual Interface idiom: a public non-virtual function calls private (or protected) virtual hooks. It modernises Template Method by keeping the skeleton in the base and preventing subclasses from calling or skipping steps arbitrarily.

</details>

---

[← Previous: Chapter 43](43-header-and-library-architecture.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 45 — C compatibility →](../part-17-c-and-python-interop/45-c-compatibility.md)
