# Project 7 — Concurrent Queue

[← Project 6](project-06-thread-pool.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 8 — Coroutine Task →](project-08-coroutine-task.md)

> **Level 5 · ≈ 10 hours · Builds on:** Chapters 30 (memory model), 31 (atomics), 32 (lock-free), 27 (false sharing), and Project 6
> **Reference solution:** [`code/p07/queues.hpp`](code/p07/queues.hpp), [`test.cpp`](code/p07/test.cpp), [`bench.cpp`](code/p07/bench.cpp)

## Goal

Implement four queues, in increasing order of difficulty, and compare them on correctness, complexity **and** speed:

| Queue | Producers / consumers | Bounded | Blocking | Technique |
|---|:--:|:--:|:--:|---|
| `MutexQueue<T>` | many / many | no | pop blocks | mutex + condition variable |
| `BoundedQueue<T>` | many / many | yes | push and pop block | two condition variables, `close()` |
| `SpscRing<T>` | **one** / **one** | yes | no (`try_*`) | lock-free ring, acquire/release |
| `MpmcRing<T>` | many / many | yes | no (`try_*`) | lock-free, per-cell sequence numbers |

The point is not that lock-free wins. In this project it often **does not**, and you should be able to explain why.

## Specification

1. **No lost items, no duplicated items, FIFO per producer.** Every test checks the count *and* the sum of all values.
2. `BoundedQueue::push` blocks while full and returns `false` if the queue was `close()`d; `pop` returns `nullopt` only when the queue is closed **and drained** (so consumers see every item, then stop).
3. The lock-free queues are **non-blocking `try_push`/`try_pop`**. They must work with non-trivial `T` (`std::string`, an instrumented `Probe`) and destroy any leftover elements in their destructors.
4. Every `memory_order` argument has a **comment** saying what it synchronises with. If you cannot write the comment, use `seq_cst` and say so.
5. Producer-owned and consumer-owned variables live on **different cache lines**.
6. Tests run under **ASan + UBSan** and **TSan**; the concurrent tests are repeated at least five times.

## Milestones

1. `MutexQueue`. Get the test harness right first (`mpmc_check`: P producers push disjoint ranges, C consumers pop, verify count and sum).
2. `BoundedQueue` with `close()`. Prove that a closed queue wakes *blocked* producers and consumers.
3. `SpscRing` with plain (non-atomic) cached copies of the other side's index.
4. `MpmcRing` (Vyukov): per-cell sequence numbers, ticket CAS.
5. Benchmark all four, with and without cache-line padding.

## How the lock-free queues work

### SPSC ring

```text
   producer owns head_ (writes it)          consumer owns tail_ (writes it)
   ┌───┬───┬───┬───┬───┬───┬───┬───┐
   │ . │ X │ X │ X │ . │ . │ . │ . │         X = live element
   └───┴───┴───┴───┴───┴───┴───┴───┘
         ▲tail_         ▲head_
```

Each index has exactly one writer, so no read-modify-write is needed anywhere:

- Producer: write the element, then `head_.store(next, release)`.
- Consumer: `head_.load(acquire)`; if it sees the new `head_`, the element write **happens-before** its read.
- Symmetrically, the consumer's `tail_.store(release)` tells the producer that the slot may be overwritten (the producer's `tail_.load(acquire)`).
- **Cached indices:** each side keeps a plain copy of the other side's index and refreshes it only when the queue *looks* full/empty. That cuts cross-core traffic from one cache-line transfer per operation to roughly one per lap.

### MPMC ring (sequence-number design)

```text
  cell.seq == pos        → empty: producer holding ticket pos may write
  cell.seq == pos + 1    → full:  consumer holding ticket pos may read
  consumer stores seq = pos + capacity → empty for the next lap
```

A producer CASes `enq_` to claim ticket `pos`, writes into `cells_[pos & mask]`, then release-stores `seq = pos + 1`. Consumers do the same with `deq_`. The cell's sequence number is the only synchronisation between the producer that filled it and the consumer that empties it. **ABA** (the CAS sees the same index value after a full lap) is harmless here because `seq` grows by `capacity` each lap, so a stale view is always detected by the `diff` test; the cells are never freed, so there is no reclamation problem. That is a property of *this* design. It does not carry over to linked lock-free queues (Michael-Scott), which need hazard pointers or epochs (Chapter 32).

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| `memory_order_relaxed` on the publishing store | Passes on x86 (stores are not reordered with other stores), **fails on ARM**. TSan reports it on x86 too. This is why you run TSan |
| Reading the element before the acquire load of the index | Torn / stale values; TSan race |
| Producer and consumer indices on one cache line | Correct but 2–3× slower (measured below) |
| Not destroying leftovers | `Probe::live != 0` |
| `close()` without `notify_all` | Hang at shutdown |
| Wait predicate without `|| closed_` | Hang at shutdown |
| Notifying only one waiter on `close()` | One consumer exits, the rest sleep forever |
| Capacity not a power of two with `& mask` | Items overwritten or dropped; the sum test catches it |

## Think about (and write answers)

1. For each `memory_order` in `SpscRing`, name the **other** operation it synchronises with. Replace each with `relaxed` in turn: which test fails, on which architecture, and why might it *still* pass on your machine?
2. Why does the SPSC ring keep one slot empty? What would you change to use all `N` slots? (Hint: a separate count, or monotonically increasing indices masked on access.)
3. In `MpmcRing`, why is the CAS on `enq_` allowed to be `relaxed`? What orders the *data* write in the cell?
4. A consumer that claimed a ticket and is then descheduled blocks all later consumers of that cell (and eventually the producers at the same cell). Is this queue **lock-free**? **Wait-free**? What does the term mean for a queue that can be blocked by one stalled thread? (Chapter 32.)
5. When would you choose `BoundedQueue` over `MutexQueue`? (Hint: back-pressure and memory.)
6. The consumers in the benchmark **spin**. On a 2-core machine with 4 threads, what does a spinning consumer do to a producer that was descheduled? What does `yield` fix, and what does it not?
7. When should you **not** use a lock-free queue? (Write down at least three situations.)

## Measurements

`-O2`, GCC 13.3, **2-vCPU VM**, 4 M items, best of 5. Million items per second:

```text
queue                                  1P / 1C    2P / 2C
MutexQueue (mutex + deque)                 6.7        7.5
BoundedQueue (blocking, cap 1024)          3.4        4.2
SpscRing (lock-free, cap 1024)             9.6        n/a
MpmcRing (lock-free, cap 1024)             7.7        6.5
```

Read these numbers carefully:

- **The lock-free queues are not dramatically faster.** On this VM the best case (SPSC) is 1.4× the mutex queue. Production SPSC rings reach tens of millions of items per second on bare metal with pinned cores; a 2-vCPU virtual machine, where the two "cores" may be hyper-threads of one physical core or time-sliced by the hypervisor, hides most of that. **Do not extrapolate to your hardware: run it there.**
- **`BoundedQueue` is slower because it is blocking.** Its consumers sleep and are woken by the producer (system calls and context switches), while the other three are polled with `try_pop` in the benchmark. The numbers compare *behaviours*, not just data structures: a sleeping consumer costs no CPU, a spinning one burns a core.
- **With 4 threads on 2 cores**, the 2P/2C column measures the scheduler as much as the queue.
- The mutex queue is **simple, unbounded, and never loses to anything by an order of magnitude here.** That is the honest engineering answer for most programs.

### The cost of false sharing (padding removed)

The same benchmark with `alignas(kCacheLine)` deleted from the index members, run twice:

```text
                      padded       unpadded
SpscRing 1P/1C        8.6-9.7     7.0-7.8    M items/s
MpmcRing 1P/1C        8.3         4.9-5.0
MpmcRing 2P/2C        6.2-7.3     2.0-2.8
```

Putting `enq_` and `deq_` (and, in the SPSC ring, `head_`/`tail_`) on one cache line makes every operation invalidate the other side's copy of the line. The largest effect is where two sides hammer adjacent variables: the unpadded `MpmcRing` under 2P/2C was **2.5-3× slower**. That is false sharing, measured.

## Stretch goals

- **Batch operations** (`try_push_n`, `try_pop_n`) for the SPSC ring. Measure the effect on throughput; explain it in terms of cache-line transfers.
- **A blocking adapter** for the MPMC ring using `std::atomic::wait`/`notify_one` (C++20) or a `futex`. What do you lose?
- **A linked unbounded MPSC queue** (Vyukov intrusive MPSC) and the reason it is simpler than MPMC.
- **Run on ARM** (or emulate weaker ordering with `-fsanitize=thread` and `relaxed` mistakes) to see a broken queue fail.
- **Linearizability check:** record operation start/end timestamps in the test and write a small checker that verifies the history is linearizable for a FIFO queue.
- **Latency percentiles:** measure p50/p99/p99.9 push-to-pop latency instead of throughput. Which queue has the worst tail, and why?

## Reference solution notes

- **`kCacheLine` is a constant (64), not `std::hardware_destructive_interference_size`.** The latter is not stable across compilers and flags (GCC warns when you use it in a header, because it is an ABI hazard) and is also not the best value on every CPU (Apple silicon is 128; some Intel CPUs prefetch adjacent lines in pairs).
- **`Slot` and `Cell` store raw bytes** and construct/destroy objects explicitly, as in Project 3.
- **`std::launder` is used** here (unlike in Project 3's inline pointer) because by the time we call `ptr()` there **is** a live `T` at that address, created by placement new.
- **The destructors drain** remaining elements; make sure yours does, or leaks show up only for non-trivial `T`.

Run it:

```bash
cd code/p07
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan -pthread && ./test_asan
g++ -std=c++23 -Wall -Wextra -g -fsanitize=thread test.cpp -o test_tsan -pthread && ./test_tsan
g++ -std=c++23 -O2 bench.cpp -o bench -pthread && ./bench
```

```text
all tests passed        (ASan+UBSan, TSan x5)
```

## What I learned (write it)

> Under what conditions would you replace a mutex-protected queue with a lock-free one in a real system, and what evidence would you require first?
