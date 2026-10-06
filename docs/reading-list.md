# Reading List

[← Back to the course map](../README.md)

The course tries to send you to *primary sources*, because secondary explanations of C++ go stale quickly. This is the short list worth keeping open.

---

## The language itself

| Source | Use it for |
|---|---|
| [**C++ working draft** (eel.is/c++draft)](https://eel.is/c++draft/) | The standard's text, searchable and linkable by stable section name such as `basic.life` or `expr.prop` |
| [**cppreference.com**](https://en.cppreference.com/) | Readable reference. Check the *compiler support* tables and "since C++NN" markers |
| [**WG21 papers**](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/) | The *why*: motivation sections of proposals explain design decisions better than any book |
| [**isocpp.org**](https://isocpp.org/) | News, status, FAQ |
| [**Herb Sutter's trip reports**](https://herbsutter.com/) | Authoritative summaries of each standards meeting |

Papers that are worth reading *in the original*:

| Paper | Topic | Chapter |
|---|---|---|
| N4861 / the C++20 draft, `[intro.races]`, `[atomics]` | Memory model | 30–31 |
| P0135 | Guaranteed copy elision | 6 |
| P0593 | Implicit object creation | 2–3 |
| P0847 | Deducing `this` | 20 |
| P0912 | Coroutines (the merge of the Coroutines TS) | 33–34 |
| P2300 | `std::execution` (senders/receivers) | 35 |
| P2996 | Static reflection | 18 |
| P2900 | Contracts | 22, 43 |
| P2795, P3471 | Erroneous behaviour, hardened library | 28 |
| P0443 → P2300 | The executors story | 29, 35 |

## Binary interfaces and the toolchain

| Source | Use it for |
|---|---|
| [**Itanium C++ ABI**](https://itanium-cxx-abi.github.io/cxx-abi/abi.html) | Object layout, vtables, name mangling, exception tables. What GCC and Clang follow on Linux |
| [**System V AMD64 psABI**](https://gitlab.com/x86-psABIs/x86-64-ABI) | Calling convention, register usage, stack layout |
| [**GCC manual**](https://gcc.gnu.org/onlinedocs/) and [**GCC's C++ status pages**](https://gcc.gnu.org/projects/cxx-status.html) | Flags, extensions, which standard features are implemented |
| [**Clang documentation**](https://clang.llvm.org/docs/) and [**C++ status page**](https://clang.llvm.org/cxx_status.html) | Same, plus sanitizer and modules docs |
| [**libstdc++ docs and source**](https://gcc.gnu.org/onlinedocs/libstdc++/) | How `vector`, `shared_ptr`, `function` are really built |
| [**libc++ source**](https://github.com/llvm/llvm-project/tree/main/libcxx) | The other real implementation |
| [**Linux man pages**](https://man7.org/linux/man-pages/) | `epoll(7)`, `mmap(2)`, `futex(2)`, `perf_event_open(2)` |

## Books

| Book | Why |
|---|---|
| *C++ Templates: The Complete Guide*, 2nd ed. (Vandevoorde, Josuttis, Gregor) | Templates in the depth chapter 8–10 point to |
| *C++20: The Complete Guide* (Josuttis) | Concepts, ranges, coroutines, modules in one place |
| *C++ Concurrency in Action*, 2nd ed. (Anthony Williams) | The memory model and lock-free chapters, from someone who implemented `std::thread` |
| *Effective Modern C++* (Scott Meyers) | Still the best explanation of deduction and move pitfalls, though it predates C++20 |
| *The C++ Standard Library*, 2nd ed. (Josuttis) | Library reference with design commentary |
| *Inside the C++ Object Model* (Lippman) | Dated in places, but the right questions about layout |
| *Rust Atomics and Locks* (Mara Bos) | Unusually clear treatment of memory ordering; the model is the same as C++'s |
| *The Art of Multiprocessor Programming* (Herlihy, Shavit) | Theory behind lock-free algorithms |
| *What Every Programmer Should Know About Memory* (Drepper) | Caches, TLB, NUMA. Free PDF |
| *Systems Performance*, 2nd ed. (Brendan Gregg) | Profiling methodology |
| *The Linux Programming Interface* (Kerrisk) | The OS side of sockets, `epoll`, signals |
| *Linkers and Loaders* (Levine) | Chapter 36–37 background |
| *Large-Scale C++ Volume I* (Lakos) | Physical design and dependency management; opinionated, influential |

## Talks and articles

| Source | Topic |
|---|---|
| Sean Parent, *Inheritance Is the Base Class of Evil* and *Better Code: Runtime Polymorphism* | Value semantics, type erasure |
| Louis Dionne, *Runtime Polymorphism: Back to the Basics* (CppCon 2017) | Type erasure design |
| Lewis Baker, *C++ Coroutines: Understanding operator co_await* and related posts | The best coroutine machinery explanations |
| Herb Sutter, *atomic<> Weapons* (C++ and Beyond 2012) | Memory model |
| Jeff Preshing, *Preshing on Programming* | Acquire/release, lock-free, hardware ordering |
| Hans Boehm's papers and talks | Origin of the C++ memory model |
| Chandler Carruth, *Efficiency with Algorithms, Performance with Data Structures* (CppCon 2014); *Tuning C++* | Performance engineering |
| Matt Godbolt, *What Has My Compiler Done for Me Lately?* | Reading assembly |
| Jason Turner, *C++ Weekly* | Short experiments in the spirit of this course |
| Louis Brandy, *Curiously Recurring C++ Bugs at Facebook* | Failure modes |
| Titus Winters, *Software Engineering at Google* (the book) and talks on API design | Library evolution, Hyrum's Law |

## Code worth reading

Reading a real implementation once is worth ten blog posts. In order of difficulty:

1. `libstdc++`: `<bits/unique_ptr.h>`, `<bits/shared_ptr_base.h>`, `<bits/stl_vector.h>`
2. `libstdc++`: `<optional>`, `<variant>`, `<bits/std_function.h>`
3. `libc++`: `<__memory/compressed_pair.h>`, `<__functional/function.h>`
4. `libstdc++`: `<coroutine>`, `<generator>`
5. [Folly](https://github.com/facebook/folly): `small_vector`, `Function`
6. [Abseil](https://github.com/abseil/abseil-cpp): `flat_hash_map`, `InlinedVector`
7. [liburing](https://github.com/axboe/liburing) and [`libuv`](https://github.com/libuv/libuv) for event-loop designs
8. [cppcoro](https://github.com/lewissbaker/cppcoro) for coroutine task design

## Habits that pay off

- When a chapter claims something about the standard, **look up the wording** in the draft. Note the stable section name such as `[basic.life]`.
- When it claims something about the compiler, **reproduce it** on both GCC and Clang.
- When it claims something about performance, **measure it**, on your own machine, with your own flags.
