# Part XVIII — Systems Programming Projects

[← Course map](../README.md)

Eleven projects and a capstone. Each one is something you **build yourself**, test hard, and then compare against a reference solution. They are the point of the course: the chapters give you the model, the projects make it yours.

## How to work

1. **Read the project page, not the solution.** Each page gives the goal, the spec, milestones, and the tests your code must pass.
2. **Write your own tests first** (at least the failure cases listed). Then run them under sanitizers.
3. **Only then open the reference solution** in [`code/`](code/) and compare design decisions, not lines.
4. **Write the "what I learned" paragraph** at the end of each project. If you cannot explain a decision, you did not make it.

All reference solutions compile warning-free with `g++ -std=c++23 -Wall -Wextra` (GCC 13.3, and 14.2 where noted) and pass under ASan+UBSan; concurrent ones also pass under TSan. Projects that use `std::expected` (1 and the capstone) need GCC: Clang 18 with libstdc++ 13 does not provide it. Project 8's deep-recursion tests behave differently by compiler and optimisation level, and the page says exactly how. Numbers in the pages are from a 2-vCPU Linux VM and are indicative, not benchmarks of your hardware.

## The projects

| # | Project | Builds on chapters | Level | Time |
|:--:|---|---|:--:|:--:|
| 1 | [RAII resource library](project-01-raii-library.md) | 3, 4, 6, 22, 23 | 2 | 4 h |
| 2 | [Custom `Vector<T>`](project-02-custom-vector.md) | 2, 3, 6, 11, 22, 24 | 3 | 8 h |
| 3 | [`SmallVector<T, N>`](project-03-small-vector.md) | 2, 3, 27 | 3 | 6 h |
| 4 | [Type-erased function](project-04-type-erased-function.md) | 7, 21 | 3 | 6 h |
| 5 | [Memory arena, pool, PMR resource](project-05-memory-arena.md) | 24, 26, 27 | 4 | 8 h |
| 6 | [Thread pool](project-06-thread-pool.md) | 29, 21 | 3 | 6 h |
| 7 | [Concurrent queue](project-07-concurrent-queue.md) | 30, 31, 32 | 5 | 10 h |
| 8 | [Coroutine `Task<T>`](project-08-coroutine-task.md) | 33, 34 | 4 | 8 h |
| 9 | [`epoll` event loop](project-09-event-loop.md) | 33, 35 | 4 | 8 h |
| 10 | [Async HTTP server](project-10-async-http-server.md) | 34, 35 | 4 | 12 h |
| 11 | [Python extension](project-11-python-extension.md) | 45, 46 | 3 | 6 h |
| ★ | [Capstone: an asynchronous runtime library](capstone.md) | all | 5 | 30 h |

## Grading rubric (use it on yourself)

Score each project 0–3 on each row. **Below 2 on correctness or ownership means the project is not done.**

| Criterion | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| **Correctness** | fails spec tests | passes happy path | passes all spec tests | also passes your own adversarial tests |
| **Ownership and lifetime** | leaks or double frees | leaks only on error paths | clean under ASan, rules documented | every invariant stated and tested |
| **Exception / error safety** | not considered | basic guarantee by luck | stated guarantees, tested with throwing types | strong guarantee where cheap, `noexcept` justified |
| **API design** | surprising | usable | hard to misuse | misuse fails to compile |
| **Sanitizers** | not run | ASan only | ASan + UBSan (+ TSan if concurrent) clean | also run with throwing/instrumented types |
| **Measurement** | none | one number | before/after with variance | explains the number from the hardware or assembly |
| **Explanation** | none | what the code does | why each decision | what you would change and what it would cost |

## Rules of the game

- No library that does the job (`std::vector` inside your `Vector`, `std::function` inside your `Function`). Standard utilities that are *not* the thing being built (`std::allocator_traits`, `std::exchange`, `<atomic>`) are fine.
- Every raw owning pointer needs a comment naming its owner.
- Every `noexcept` and every `memory_order` needs a one-line justification.
- If a test can only fail "sometimes", run it 1000 times under TSan before you believe it.

## Layout of the reference code

```text
code/
  p01/ raii.hpp, test.cpp
  p02/ vector.hpp, test.cpp, bench.cpp
  p03/ small_vector.hpp, test.cpp, bench.cpp
  p04/ function.hpp, test.cpp, bench.cpp
  p05/ arena.hpp, test.cpp, bench.cpp
  p06/ thread_pool.hpp, test.cpp, bench.cpp, tsan.supp
  p07/ queues.hpp, test.cpp, bench.cpp
  p08/ task.hpp, test.cpp, bench.cpp
  p09/ event_loop.hpp, test.cpp, bench.cpp
  p10/ http.hpp, server_main.cpp, test.cpp, loadgen.cpp
  p11/ fastmath.cpp, test_fastmath.py, bench.py, run.sh
  capstone/ include/aio/*.hpp, tests/, bench/, examples/, Makefile
```
