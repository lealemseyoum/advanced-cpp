# Advanced Modern C++

### From language mechanics to systems programming

A rigorous, project-driven course for programmers who already write C++ and want a **precise mental model** of the language: what the standard guarantees, what the compiler does, what the ABI imposes, and what the machine finally executes.

This is not a beginner course. There is no chapter on `for` loops. The course targets **C++17, C++20, C++23 and the freshly finished C++26**, and goes back to older mechanisms only where they explain why modern C++ looks the way it does.

> [!IMPORTANT]
> The question behind every chapter is not *"what does this feature do?"* but ***"why does modern C++ look like this?"***

---

## Contents

- [How the course works](#how-the-course-works)
- [The five layers](#the-five-layers)
- [Difficulty levels](#difficulty-levels)
- [Feature status legend](#feature-status-legend)
- [Course map](#course-map)
- [Suggested pacing](#suggested-pacing)
- [Toolchain](#toolchain)
- [Verified code](#verified-code)
- [Repository layout](#repository-layout)

---

## How the course works

Every chapter follows the same fourteen-step shape, so you always know where you are:

| # | Section | What it gives you |
|---|---|---|
| 1 | **Problem** | The pain that motivated the feature |
| 2 | **Historical context** | How C and pre-modern C++ dealt with it, and why that was not enough |
| 3 | **Modern solution** | The mechanism that was introduced |
| 4 | **Mental model** | A picture you can reason with |
| 5 | **Language rules** | The actual rules, with the standard's vocabulary |
| 6 | **Implementation model** | What GCC and Clang commonly do |
| 7 | **Experiments** | Small programs to run, not to read |
| 8 | **Assembly / runtime investigation** | What the compiler really emitted |
| 9 | **Implementation exercise** | You build a simplified version |
| 10 | **Real-world example** | Where the technique matters in practice |
| 11 | **Failure modes** | The mistakes people actually make |
| 12 | **Exercises** | Progressively harder |
| 13 | **Challenge** | A problem that combines several ideas |
| 14 | **Knowledge check** | Questions first, answers hidden until you click |

Three habits make the course work:

1. **Run the experiments.** Reading about a moved-from `std::string` teaches you far less than printing one. Every snippet marked as runnable has been compiled and executed.
2. **Build the exercises before reading the solutions.** The point of implementing a `unique_ptr` is the three bugs you hit on the way.
3. **Look at the assembly.** When a chapter says "zero-cost", it will show you the instructions, and you can check the claim yourself.

The course is **opinionated**. Where something is legal but unwise, it says so, and says why. Opinions come with reasons, so you can disagree with the reasoning rather than the verdict.

---

## The five layers

Most confusion about C++ comes from mixing up *who decides* a behaviour. Chapters label claims with the layer that owns them:

```text
  C++ standard      what the language guarantees           (portable contract)
        │
  Compiler          what GCC / Clang / MSVC implement      (may change between versions)
        │
  ABI               how types and calls look in binary     (platform contract: Itanium ABI on Linux)
        │
  Operating system  what Linux provides                    (syscalls, virtual memory, scheduler)
        │
  CPU               what the hardware actually does        (caches, reordering, pipelines)
```

| Layer | The question it answers |
|---|---|
| **Standard** | Is this behaviour guaranteed, unspecified, or undefined? |
| **Compiler** | What does this toolchain actually generate? |
| **ABI** | What is the binary representation, and what breaks if it changes? |
| **OS** | What does the kernel do when I ask for this? |
| **CPU** | What does the machine really execute, and how fast? |

> [!WARNING]
> If a chapter says "GCC does X", that is **not** a promise from C++. It is an observation you can verify and must not rely on.

---

## Difficulty levels

Chapters start at Level 1 and the course drifts steadily toward Level 5.

| Level | Meaning |
|:---:|---|
| **1** | Advanced usage |
| **2** | Language-mechanism understanding |
| **3** | Implementation |
| **4** | Compiler / runtime understanding |
| **5** | Systems / hardware understanding |

---

## Feature status legend

C++ moves quickly, and a feature being *in the standard* is not the same as being *usable on your compiler*. Version-sensitive features carry one of these tags:

| Tag | Meaning |
|---|---|
| ✅ **Standardized** | In a published standard (C++17, C++20, C++23) and implemented by current GCC and Clang |
| 🟡 **Standardized, patchy** | In the standard, but compiler or library support is incomplete or uneven |
| 🆕 **C++26** | Technical work finished in March 2026; implementations are still arriving (GCC 16 leads) |
| 🧪 **Proposed / experimental** | A WG21 paper or compiler-specific experiment, not in any standard |
| 🔧 **Extension** | A GCC/Clang extension, not portable |

> [!NOTE]
> **Where C++26 stands.** WG21 completed the technical work on C++26 at its March 2026 meeting in London (Croydon). The standard then moves through the ISO Draft International Standard ballot and publication. Headline features: static **reflection**, **contracts**, `std::execution` (senders/receivers), erroneous behaviour for uninitialized reads, and a hardened standard library. Compiler support varies widely; chapters give the details and say what you can try today. Check [cppreference's compiler support table](https://en.cppreference.com/w/cpp/compiler_support) before relying on any C++26 feature.

---

## Course map

```text
 Mental model ─► Object model ─► Value categories ─► Generic programming ─► Library
      │              │                 │                    │                  │
      └──── lifetime, ownership, RAII ─┴── move, forwarding ┴── templates,    ├── containers, views
                                                               concepts        └── ranges, sum types
        ┌───────────────────────────────────────────────────────────────────────────┘
        ▼
 Compile-time ─► Polymorphism ─► Errors ─► Memory ─► UB ─► Concurrency ─► Coroutines
        │                                                                       │
        ▼                                                                       ▼
 Compilation / ABI / modules ─► CMake ─► Performance ─► Library design ─► Interop ─► Qt
                                                                    │
                                                                    ▼
                                             11 projects  ─►  capstone runtime library
```

### Part I — Modern C++ mental model

| Ch | Title | Level |
|:--:|---|:--:|
| 1 | [The modern C++ philosophy](part-01-mental-model/01-modern-cpp-philosophy.md) | 1 |

### Part II — Object model and lifetime

| Ch | Title | Level |
|:--:|---|:--:|
| 2 | [The C++ object model](part-02-object-model-and-lifetime/02-object-model.md) | 2 |
| 3 | [Lifetime and storage](part-02-object-model-and-lifetime/03-lifetime-and-storage.md) | 2 |
| 4 | [RAII](part-02-object-model-and-lifetime/04-raii.md) | 3 |

### Part III — Value categories and move semantics

| Ch | Title | Level |
|:--:|---|:--:|
| 5 | [Value categories](part-03-value-categories-and-move/05-value-categories.md) | 2 |
| 6 | [Move semantics](part-03-value-categories-and-move/06-move-semantics.md) | 3 |
| 7 | [Perfect forwarding](part-03-value-categories-and-move/07-perfect-forwarding.md) | 3 |

### Part IV — Generic programming

| Ch | Title | Level |
|:--:|---|:--:|
| 8 | [Templates deep dive](part-04-generic-programming/08-templates-deep-dive.md) | 2 |
| 9 | [Type traits and compile-time introspection](part-04-generic-programming/09-type-traits.md) | 3 |
| 10 | [Concepts](part-04-generic-programming/10-concepts.md) | 3 |

### Part V — The modern standard library

| Ch | Title | Level |
|:--:|---|:--:|
| 11 | [Modern containers](part-05-standard-library/11-containers.md) | 4 |
| 12 | [Views and non-owning types](part-05-standard-library/12-views-and-non-owning-types.md) | 2 |
| 13 | [`optional`, `variant`, `any`, `expected`](part-05-standard-library/13-optional-variant-any-expected.md) | 3 |

### Part VI — Ranges

| Ch | Title | Level |
|:--:|---|:--:|
| 14 | [C++20 ranges](part-06-ranges/14-ranges.md) | 3 |
| 15 | [Algorithms and customization](part-06-ranges/15-algorithms-and-customization.md) | 3 |

### Part VII — Compile-time C++

| Ch | Title | Level |
|:--:|---|:--:|
| 16 | [`constexpr`, `consteval`, `constinit`](part-07-compile-time/16-constexpr.md) | 3 |
| 17 | [Template metaprogramming](part-07-compile-time/17-template-metaprogramming.md) | 3 |
| 18 | [The reflection landscape](part-07-compile-time/18-reflection-landscape.md) | 2 |

### Part VIII — Polymorphism

| Ch | Title | Level |
|:--:|---|:--:|
| 19 | [Runtime polymorphism](part-08-polymorphism/19-runtime-polymorphism.md) | 4 |
| 20 | [Static polymorphism](part-08-polymorphism/20-static-polymorphism.md) | 3 |
| 21 | [Type erasure](part-08-polymorphism/21-type-erasure.md) | 4 |

### Part IX — Error handling

| Ch | Title | Level |
|:--:|---|:--:|
| 22 | [Exceptions](part-09-error-handling/22-exceptions.md) | 4 |
| 23 | [`std::expected`](part-09-error-handling/23-expected.md) | 3 |

### Part X — Memory

| Ch | Title | Level |
|:--:|---|:--:|
| 24 | [Dynamic memory](part-10-memory/24-dynamic-memory.md) | 3 |
| 25 | [Smart pointers](part-10-memory/25-smart-pointers.md) | 3 |
| 26 | [Allocators and memory resources](part-10-memory/26-allocators-and-memory-resources.md) | 4 |
| 27 | [Cache and data-oriented C++](part-10-memory/27-cache-and-data-oriented-cpp.md) | 5 |

### Part XI — Undefined behavior

| Ch | Title | Level |
|:--:|---|:--:|
| 28 | [Undefined behavior](part-11-undefined-behavior/28-undefined-behavior.md) | 4 |

### Part XII — Concurrency

| Ch | Title | Level |
|:--:|---|:--:|
| 29 | [C++ threading](part-12-concurrency/29-threading.md) | 3 |
| 30 | [The C++ memory model](part-12-concurrency/30-memory-model.md) | 5 |
| 31 | [Atomics](part-12-concurrency/31-atomics.md) | 5 |
| 32 | [Lock-free programming](part-12-concurrency/32-lock-free.md) | 5 |

### Part XIII — Coroutines

| Ch | Title | Level |
|:--:|---|:--:|
| 33 | [C++20 coroutines](part-13-coroutines/33-coroutines.md) | 4 |
| 34 | [Building a coroutine type](part-13-coroutines/34-building-task.md) | 4 |
| 35 | [Coroutines and networking](part-13-coroutines/35-coroutines-and-networking.md) | 5 |

### Part XIV — Compilation and linking

| Ch | Title | Level |
|:--:|---|:--:|
| 36 | [The C++ compilation model](part-14-compilation-and-linking/36-compilation-model.md) | 4 |
| 37 | [ABI](part-14-compilation-and-linking/37-abi.md) | 4 |
| 38 | [Modules](part-14-compilation-and-linking/38-modules.md) | 3 |

### Part XV — Build systems

| Ch | Title | Level |
|:--:|---|:--:|
| 39 | [Modern CMake](part-15-build-systems/39-modern-cmake.md) | 3 |

### Part XVI — Performance engineering

| Ch | Title | Level |
|:--:|---|:--:|
| 40 | [Benchmarking](part-16-performance/40-benchmarking.md) | 4 |
| 41 | [Profiling](part-16-performance/41-profiling.md) | 5 |
| 42 | [Compiler optimization](part-16-performance/42-compiler-optimization.md) | 4 |

### Part XVII — Modern library design

| Ch | Title | Level |
|:--:|---|:--:|
| 43 | [API design](part-17-library-design/43-api-design.md) | 3 |
| 44 | [Header and library architecture](part-17-library-design/44-header-and-library-architecture.md) | 3 |
| 45 | [Design patterns revisited](part-17-library-design/45-design-patterns-revisited.md) | 3 |

### Part XVIII — C and Python interoperability

| Ch | Title | Level |
|:--:|---|:--:|
| 46 | [C compatibility](part-18-c-and-python-interop/46-c-compatibility.md) | 3 |
| 47 | [C++ and Python](part-18-c-and-python-interop/47-python-interop.md) | 3 |

### Part XIX — Qt and modern C++

| Ch | Title | Level |
|:--:|---|:--:|
| 48 | [Modern C++ in Qt](part-19-qt/48-modern-cpp-in-qt.md) | 3 |

### Part XX — Systems programming projects

| Project | Title |
|:--:|---|
| — | [Projects overview and grading rubric](part-20-projects/README.md) |
| 1 | [RAII resource library](part-20-projects/project-01-raii-library.md) |
| 2 | [Custom `Vector<T>`](part-20-projects/project-02-custom-vector.md) |
| 3 | [`SmallVector<T, N>`](part-20-projects/project-03-small-vector.md) |
| 4 | [Type-erased function](part-20-projects/project-04-type-erased-function.md) |
| 5 | [Memory arena, pool and PMR resource](part-20-projects/project-05-memory-arena.md) |
| 6 | [Thread pool](part-20-projects/project-06-thread-pool.md) |
| 7 | [Concurrent queue](part-20-projects/project-07-concurrent-queue.md) |
| 8 | [Coroutine `Task<T>`](part-20-projects/project-08-coroutine-task.md) |
| 9 | [`epoll` event loop](part-20-projects/project-09-event-loop.md) |
| 10 | [Async HTTP server](part-20-projects/project-10-async-http-server.md) |
| 11 | [Python extension](part-20-projects/project-11-python-extension.md) |
| ★ | [**Capstone: an asynchronous runtime library**](part-20-projects/capstone.md) |

### Appendices

| | |
|---|---|
| [Toolchain setup](docs/toolchain.md) | Compilers, sanitizers, `perf`, Compiler Explorer, an experiment workflow |
| [Reading list](docs/reading-list.md) | Standards, papers, books, talks, source code worth reading |
| [Glossary](docs/glossary.md) | Precise definitions of the terms the course uses |

---

## Suggested pacing

Roughly **30 weeks at 6–8 hours a week**. Go faster through chapters you already know; go slower through the Level 5 chapters.

| Weeks | Focus | Chapters | Projects |
|:--:|---|---|---|
| 1–3 | Mental model, object model, lifetime, RAII | 1–4 | 1 |
| 4–6 | Value categories, moves, forwarding | 5–7 | 2 |
| 7–10 | Templates, traits, concepts | 8–10 | — |
| 11–13 | Library design from the inside | 11–15 | 3 |
| 14–16 | Compile-time, polymorphism, type erasure | 16–21 | 4 |
| 17–19 | Errors, memory, UB | 22–28 | 5 |
| 20–23 | Concurrency and the memory model | 29–32 | 6, 7 |
| 24–26 | Coroutines and async I/O | 33–35 | 8, 9, 10 |
| 27–28 | Build, ABI, modules, performance | 36–42 | — |
| 29–30 | Library design, interop, Qt, capstone | 43–48 | 11, capstone |

> [!TIP]
> If you can only do one thing per chapter, do the **implementation exercise**. If you can do two, add the **assembly investigation**.

---

## Toolchain

The course was written and verified on **Ubuntu 24.04 with GCC 14.2** (C++23), with selected snippets also checked on **GCC 13.3** and **Clang 18**. Everything runs on Linux. See [docs/toolchain.md](docs/toolchain.md) for setup.

| Tool | Used for |
|---|---|
| `g++` ≥ 14, `clang++` ≥ 18 | Compilation; comparing two implementations |
| `objdump`, `nm`, `readelf`, `c++filt` | Reading object files and symbols |
| `gdb` / `lldb` | Inspecting objects and the stack |
| ASan, UBSan, TSan, Valgrind | Detecting undefined behavior |
| `perf`, Google Benchmark | Measuring, not guessing |
| CMake ≥ 3.28, Ninja | Builds |
| [Compiler Explorer](https://godbolt.org) | Quick assembly comparisons, newer compiler versions |

> [!NOTE]
> Features that need a newer compiler than GCC 14 (for example C++26 reflection in GCC 16) are shown as code you can paste into Compiler Explorer, and are marked as *not verified locally*.

---

## Verified code

Code blocks in the course are real. A block whose first line is a test header is compiled (and usually run) by [`tools/check_snippets.py`](tools/check_snippets.py):

```cpp
// @test run -std=c++23 -O2
#include <print>
int main() { std::println("this block is checked by the repository tooling"); }
```

Program output and assembly shown in the chapters were generated by running the code, not typed from memory:

```bash
python3 tools/check_snippets.py                      # verify every tagged snippet
python3 tools/check_snippets.py --update FILE.md     # refresh output/asm blocks
```

Output and assembly are tagged with the compiler version that produced them. Your numbers will differ; the *shape* of the results is what matters.

---

## Repository layout

```text
advanced-cpp/
├── README.md                  ← you are here
├── docs/                      toolchain, reading list, glossary
├── part-01-…/ … part-19-…/    the 48 chapters, grouped by part
├── part-20-projects/          11 projects + capstone
└── tools/                     snippet and link checkers
```

---

## Conventions

- `> [!NOTE]`, `> [!TIP]`, `> [!WARNING]`, `> [!CAUTION]` boxes mark asides, advice, and traps.
- **Verdict** lines are opinions, stated plainly.
- Answers to knowledge checks are folded away. Try first.
- Code uses `std::` explicitly; no `using namespace std`.

Start here → **[Chapter 1: The modern C++ philosophy](part-01-mental-model/01-modern-cpp-philosophy.md)**
