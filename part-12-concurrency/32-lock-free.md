# Chapter 32 — Lock-Free Programming

> **Part XII · Concurrency** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 10 hours**
> **Prerequisites:** [Chapter 25](../part-10-memory/25-smart-pointers.md), [Chapter 30](30-memory-model.md), [Chapter 31](31-atomics.md) &nbsp;|&nbsp; **Standards:** C++11 atomics (the substrate); **C++26 `std::hazard_pointer` and `std::rcu_*` (P2530, P2545) 🟡 adopted, not in GCC 14.2 / libstdc++ 14**; Folly, Boost.Lockfree, libcds and the Linux kernel provide today's production versions &nbsp;|&nbsp; **Tools:** `g++-14`, ASan, TSan, `perf`

[← Previous: Chapter 31](31-atomics.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 33 — C++20 coroutines →](../part-13-coroutines/33-coroutines.md)

---

**In one sentence:** lock-free programming gives you progress guarantees that survive a stalled thread, but it replaces "who may touch this?" with the much harder question "**when may this memory be freed or reused?**", and the honest advice is to use a proven library, and to use a mutex unless you can *prove* you can't.

**By the end of this chapter you can:**

- define lock-free, wait-free and obstruction-free precisely, and say which of your structures have which
- reproduce the **ABA problem** deterministically and fix it with tagged pointers
- implement and use **hazard pointers** and **epoch-based reclamation**, and explain what each costs
- build a Treiber stack and a Michael–Scott queue that are memory-safe under ASan
- decide, with measurements and a risk assessment, when lock-free is justified, and when it is **not**

---

## 1. Problem

Locks have three failure modes that matter in some systems:

| Failure | Where it hurts |
|---|---|
| **Blocking**: a thread holding the lock is descheduled, killed, or page-faults, and everyone waits | Real-time and low-latency systems; tail latency |
| **Priority inversion / convoying** | Mixed-priority threads; kernels |
| **Not usable at all** | Signal handlers, interrupt context, audio callbacks, inside the allocator or a lock implementation |

Lock-free structures avoid these: if any thread stalls at any instruction, the others still complete their operations. But they trade a new problem for the old ones. **Memory reclamation**: in a lock-free structure, a thread can hold a pointer to a node *that another thread has just unlinked*. When may the node be deleted? If immediately, the first thread dereferences freed memory (use-after-free). If never, memory leaks without bound. If *reused* (a free list) while the first thread still holds the old pointer, **the ABA problem** corrupts the structure silently.

With a garbage collector (Java, Go, C#) the problem disappears, which is why Java's `ConcurrentLinkedQueue` is a dozen lines. In C++ it is the entire difficulty.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1986 | Treiber's stack (IBM): the first widely cited lock-free stack, built on CAS; documents the ABA problem and the tag-counter fix |
| 1991 | Herlihy: wait-free synchronisation hierarchy; CAS is universal |
| 1996 | **Michael & Scott queue**: the lock-free FIFO that underlies `java.util.concurrent` and countless C++ libraries |
| 2002–04 | Michael: **hazard pointers** (safe memory reclamation without a GC) |
| 2001 | Harris's lock-free linked list; Fraser's thesis (epoch-based reclamation, skip lists, STM) |
| 2002 | McKenney: **RCU** enters the Linux kernel (read-copy-update) |
| 2007–13 | Herlihy–Shavit *The Art of Multiprocessor Programming*; Vyukov's bounded MPMC queue (2010), widely adopted |
| 2014 | Folly `hazptr`; libcds; Boost.Lockfree 1.53 (`queue`, `stack`, `spsc_queue`) |
| 2020–23 | Proposals P2530 (`hazard_pointer`), P2545 (RCU), P2300 (senders/receivers) progress through WG21 |
| **2024–26** | `std::hazard_pointer`, `std::rcu_domain`/`rcu_retire` adopted for **C++26** 🟡; implementations: Folly (`folly::hazptr`), libunifex, userspace-rcu; **not shipped in libstdc++ 14** |

---

## 3. Modern solution

There is no "modern" language feature that makes lock-free easy; the modern solution is **a reclamation scheme packaged as a library**, and the guidance to avoid hand-rolling:

```cpp
// C++26 (🟡 adopted; GCC 14.2 lacks it) -- the shape of the API
std::hazard_pointer hp = std::make_hazard_pointer();
Node* n = hp.protect(head);          // load head and publish it as "in use"; safe to dereference until reset
...
n->retire();                         // (std::hazard_pointer_obj_base<Node>) deferred delete; safe even if others still protect it
```

Today, in a project: use **Boost.Lockfree** (`spsc_queue`, `queue`, `stack`), **Folly** (`MPMCQueue`, `hazptr`, `AtomicHashMap`), **libcds**, or **moodycamel::ConcurrentQueue** (excellent, header-only), and keep your own code to the SPSC ring buffer of Chapter 31. The rest of this chapter builds the mechanisms so you can read, review and debug those libraries.

---

## 4. Mental model

### Three progress levels

| | Guarantee | A stalled thread can… | Example |
|---|---|---|---|
| **Blocking** | none | block everybody | mutex-protected queue |
| **Obstruction-free** | a thread running *alone* finishes | be a blocker only while others interfere | simple optimistic schemes |
| **Lock-free** | system-wide progress: some operation completes in a bounded number of steps | stall without blocking others | Treiber stack, Michael–Scott queue |
| **Wait-free** | per-thread progress: *every* operation completes in a bounded number of steps | stall without blocking others; no starvation | SPSC ring, `fetch_add` counter, universal constructions (slow) |

Lock-free does **not** mean faster. It means "never blocked by someone else's misfortune". Wait-free structures are rare and costly.

### The ABA problem, in one picture

```text
   stack:  head → A → B → C                       Thread 1 starts pop: reads head = A, next = B   (gets descheduled)

   Thread 2:  pop A   (A removed, handed to caller)
              pop B   (B removed, handed to caller and freed)
              push A  (A recycled, now A → C)     head → A → C

   Thread 1 resumes:  CAS(head, expected A, new B)   ← head == A, so the CAS SUCCEEDS
                      head → B        B is freed memory; C has been lost.   The value is the same (A); the history is not.
```

CAS checks *"is the value still A?"*, not *"has anything happened since I read A?"*. Cures: **make A different each time** (tag/version counters), or **never let A be reused while someone might still hold it** (safe memory reclamation: hazard pointers, epochs, RCU, GC).

### The reclamation design space

```text
                        reader cost             writer/reclaimer cost        bound on unreclaimed memory     robust to a stalled reader?
   reference counting   RMW per access           —                            immediate                       yes (but contended RMW on every read)
   hazard pointers      store + fence + recheck  scan all hazard slots        O(threads × slots)              YES  (bounded)
   epoch-based (EBR)    ~1 relaxed store/enter   advance epoch, free 2 epochs back  unbounded if a reader stalls  NO   (one stalled reader blocks reclamation)
   RCU (quiescent state) ~zero (kernel: none)    grace period wait            unbounded                        NO
   GC                   —                        stop/trace                   —                               yes
```

The trade is always **reader speed vs bound on garbage vs robustness**. Hazard pointers are bounded and robust; EBR/RCU make reads nearly free but a stalled reader holds back all reclamation.

### A hazard pointer in two sentences

A thread **publishes** the address it is about to dereference into a globally visible slot, then **re-checks** that the address is still reachable. A thread that unlinks a node puts it on a private *retire list* and frees it only after **scanning all slots** and finding no thread has published that address.

### Epoch-based reclamation in two sentences

Time is divided into epochs. A thread **announces the current epoch** when it enters a critical region, and withdraws on exit. A node retired in epoch *e* can be freed once **every active thread has been seen in epoch ≥ e + 1** (so no thread that could have seen the node is still inside).

---

## 5. Language rules

The C++ rules this chapter leans on are those of Chapter 30; nothing new is added at the language level. The relevant guarantees and non-guarantees:

- **Atomics:** `std::atomic<T*>` is lock-free (Chapter 31); `atomic<struct {ptr, tag}>` of 16 bytes is *not* guaranteed lock-free (GCC reports false; Experiment 2 packs the tag into a 64-bit word instead).
- **Object lifetime:** accessing `n->next` after `delete n` is UB even if "the memory is still mapped": the allocator may have unmapped, recycled or poisoned it (ASan will flag it, Chapter 28). A lock-free algorithm that "reads a possibly-freed node and then validates" is **not** valid C++ unless the read itself is protected (by a hazard pointer or an epoch).
- **Memory orders:** every CAS that publishes a node needs `release`; every read that follows a pointer to a node needs `acquire` (or `consume` semantics in theory); the validation re-read in a hazard-pointer scheme needs `seq_cst` ordering between the hazard store and the re-read (store→load: the Store-Buffering pattern of Chapter 30 Experiment 1!).
- **Alignment and size:** to pack a tag into a pointer word you rely on implementation facts: x86-64 user pointers use ≤ 47 bits; AArch64 may use 48+ bits and top-byte tagging; `reinterpret_cast<uintptr_t>` round-trips are implementation-defined. Alternative: use indices into an array (Experiment 2) instead of pointers.
- **Destructors and static destruction:** lock-free structures with deferred reclamation must be torn down with all readers quiesced; a retire list that is never flushed at exit shows up as a (benign) leak in LSan unless you drain it.

> Library status. ⚖️ Standard in C++26 (adopted): `std::hazard_pointer`, `std::hazard_pointer_obj_base`, `std::rcu_domain`, `std::rcu_retire`, `std::rcu_synchronize`. 🟡 Compiler support: not in GCC 14.2 / libstdc++ 14. Use Folly or `userspace-rcu` today.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What's guaranteed? | Atomic semantics and orders; UB for accessing freed objects; C++26 adds standard reclamation facilities |
| **Compiler / library** | What exists? | Header-only primitives; no reclamation in libstdc++ 14; Folly/Boost/libcds/moodycamel for production |
| **ABI** | Pointer representation | x86-64 canonical 48-bit addresses (tag-in-high-bits works but is non-portable; LAM/UAI/TBI features may change this); `cmpxchg16b` (DWCAS) for 128-bit tagged pointers |
| **OS** | What helps reclamation? | `membarrier(2)` (expedited) lets a reclaimer force all threads through a memory barrier, which makes asymmetric hazard-pointer/RCU schemes (cheap reader, expensive reclaimer) possible; `sched_yield`/futex for backoff |
| **CPU** | What are the costs? | Every CAS = a locked instruction, ~20 cycles uncontended; the hazard-pointer store→load needs a full fence (`mfence`/`xchg`) unless `membarrier` is used; cache-line bouncing on shared head/tail pointers |

---

## 6. Implementation model

### What the retire/scan cycle costs

Hazard pointers: **reader** = `store(hp)`, a full fence (`seq_cst` store: `xchg`), then a re-read to validate. ≈ 25–30 cycles per protected pointer on x86. **Reclaimer** = scan all `T × K` slots (hundreds of ns for 64 threads), amortised by retiring in batches (threshold ≈ 2 × slots) so most `retire` calls are an `O(1)` push.

Epoch-based: **reader** = one relaxed/release store of the epoch on enter/exit (≈ 1–2 ns); **reclaimer** = occasional pass over the thread table; **hazard** = a thread that sits inside a critical region for a long time prevents epoch advance, so memory accumulates (this is why EBR needs short, bounded critical regions).

### Linearization points

An operation on a concurrent object is **linearizable** if it appears to take effect atomically at some instant (its *linearization point*) between its invocation and response. For the Treiber stack: `push` linearizes at the successful CAS on `head`; `pop` at its successful CAS (or at the load of `head` observing null for an empty stack). Stating the linearization point is how you argue correctness: if you can't, you don't have an algorithm yet.

### Why lock-free structures are usually *slower* at low contention

A mutex-protected `std::queue` push is: uncontended lock (CAS) + a few stores + unlock (store) ≈ 20 ns. A Michael–Scott push is: heap allocation of a node + two CASes + reclamation bookkeeping. The lock-free version is *more* work per operation; it wins only when contention or blocking would otherwise dominate (Experiment 6).

---

## 7. Experiments

### Experiment 1 ✅: The ABA problem, reproduced deterministically

No threads: we replay the interleaving above step by step, so the bug is guaranteed to appear. The stack uses indices into an array of nodes (a free-list allocator) so no undefined behaviour is involved: the *logic* corrupts, not the memory.

```cpp
// @test run -std=c++23 -O0
#include <atomic>
#include <cstdio>

struct Node { int value; int next; bool in_use; };            // next: index of the next node, -1 = none
Node pool[8];

struct Stack {
    std::atomic<int> head{-1};
    void push(int idx) { pool[idx].in_use = true; int h = head.load(); do { pool[idx].next = h; } while (!head.compare_exchange_weak(h, idx)); }
    int  pop() { int h = head.load(); while (h != -1 && !head.compare_exchange_weak(h, pool[h].next)) {} if (h != -1) pool[h].in_use = false; return h; }
};

void dump(const char* when, const Stack& s) {
    std::printf("%-44s head ->", when);
    for (int i = s.head.load(); i != -1; i = pool[i].next) std::printf(" %c%s", 'A' + i, pool[i].in_use ? "" : "(FREED!)");
    std::puts("");
}

int main() {
    Stack s;
    for (int i = 2; i >= 0; --i) { pool[i].value = i; s.push(i); }       // A=0, B=1, C=2  =>  head -> A -> B -> C
    dump("initial", s);

    // --- Thread 1 begins pop(): it has read head == A and A.next == B, then is "descheduled" ---
    int t1_head = s.head.load();                                          // A
    int t1_next = pool[t1_head].next;                                     // B
    std::printf("T1 read head=%c, next=%c, and is preempted\n", 'A' + t1_head, 'A' + t1_next);

    // --- Thread 2 runs: pop A, pop B (B is freed), push A back ---
    int a = s.pop();  int b = s.pop();  s.push(a);
    dump("after T2: pop A, pop B, push A", s);

    // --- Thread 1 resumes: CAS(head, A -> B). head is A again, so the CAS succeeds ---
    int expected = t1_head;
    bool ok = s.head.compare_exchange_strong(expected, t1_next);
    std::printf("T1 CAS(head: %c -> %c) %s\n", 'A' + t1_head, 'A' + t1_next, ok ? "SUCCEEDED (the ABA bug)" : "failed");
    dump("after T1 resumes", s);
    std::printf("node C was on the stack and is now unreachable: %s\n", (s.head.load() == 1) ? "LOST" : "still there");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
initial                                      head -> A B C
T1 read head=A, next=B, and is preempted
after T2: pop A, pop B, push A               head -> A C
T1 CAS(head: A -> B) SUCCEEDED (the ABA bug)
after T1 resumes                             head -> B(FREED!) C
node C was on the stack and is now unreachable: LOST
```

The final state has the head pointing at **B, which T2 popped and freed**: any later `pop` would hand out a node already owned by someone else, and `C` has vanished. Every individual CAS was correct; the algorithm assumed that “same address” meant “same node state”.

### Experiment 2 ✅: Fix 1: tagged pointers (version counters)

```cpp
// @test run -std=c++23 -O0
#include <atomic>
#include <cstdint>
#include <cstdio>

struct Node { int value; int next; };
Node pool[8];

// head = (tag << 32) | index.  Every successful modification increments the tag, so "A" is never the same "A" twice.
struct TaggedStack {
    static constexpr int kNone = -1;
    std::atomic<std::uint64_t> head{pack(0, kNone)};
    static std::uint64_t pack(std::uint32_t tag, int idx) { return (std::uint64_t(tag) << 32) | std::uint32_t(idx); }
    static std::uint32_t tag_of(std::uint64_t v) { return std::uint32_t(v >> 32); }
    static int idx_of(std::uint64_t v) { return int(std::uint32_t(v)); }

    void push(int idx) {
        std::uint64_t h = head.load();
        do { pool[idx].next = idx_of(h); } while (!head.compare_exchange_weak(h, pack(tag_of(h) + 1, idx)));
    }
    int pop() {
        std::uint64_t h = head.load();
        while (idx_of(h) != kNone && !head.compare_exchange_weak(h, pack(tag_of(h) + 1, pool[idx_of(h)].next))) {}
        return idx_of(h);
    }
};

int main() {
    TaggedStack s;
    for (int i = 2; i >= 0; --i) s.push(i);                                 // head -> A -> B -> C

    std::uint64_t t1_head = s.head.load();                                  // T1 reads (tag=3, A) and A.next == B
    int t1_next = pool[TaggedStack::idx_of(t1_head)].next;
    std::printf("T1 read head=(tag %u, %c), next=%c, and is preempted\n", TaggedStack::tag_of(t1_head), 'A' + TaggedStack::idx_of(t1_head), 'A' + t1_next);

    int a = s.pop(); s.pop(); s.push(a);                                    // T2: pop A, pop B, push A back
    std::uint64_t now = s.head.load();
    std::printf("after T2: head = (tag %u, %c): same index A, different tag\n", TaggedStack::tag_of(now), 'A' + TaggedStack::idx_of(now));

    std::uint64_t expected = t1_head;
    bool ok = s.head.compare_exchange_strong(expected, TaggedStack::pack(TaggedStack::tag_of(t1_head) + 1, t1_next));
    std::printf("T1 CAS %s -> the stack is intact\n", ok ? "SUCCEEDED (bug)" : "FAILED (correctly detected)");
    return ok ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
T1 read head=(tag 3, A), next=B, and is preempted
after T2: head = (tag 6, A): same index A, different tag
T1 CAS FAILED (correctly detected) -> the stack is intact
```

Tagging fixes ABA **but not use-after-free**: the stack above reads `pool[h].next` from a node that another thread might have freed (here it is an array, so no UB; with `new`/`delete` it is). Tags work for index-based pools and for freelists that are never returned to the OS. For `new`/`delete` nodes you need real reclamation. The tag must also be wide enough not to wrap during the lifetime of a stalled thread: 16 bits can wrap in microseconds under load; 32 bits last hours; 64-bit tag + 64-bit pointer needs `cmpxchg16b`.

### Experiment 3 ✅: Fix 2: hazard pointers: a Treiber stack that is memory-safe under ASan

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined link=-pthread timeout=180
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

// ---- A minimal hazard-pointer domain (educational: fixed thread table, one hazard slot per thread) ----
namespace hp {
constexpr int kMaxThreads = 8;
struct alignas(64) Slot { std::atomic<void*> ptr{nullptr}; };
inline std::array<Slot, kMaxThreads> g_slots;
inline std::atomic<int> g_next_id{0};
inline thread_local int t_id = g_next_id.fetch_add(1);

template <class T>
T* protect(const std::atomic<T*>& src) {
    T* p = src.load(std::memory_order_relaxed);
    for (;;) {
        g_slots[t_id].ptr.store(p, std::memory_order_seq_cst);          // publish: "I am about to dereference p"  (StoreLoad needs seq_cst!)
        T* again = src.load(std::memory_order_seq_cst);                 // validate: p is still reachable, hence not yet retired
        if (again == p) return p;                                       // from here on, nobody will free p
        p = again;
    }
}
inline void clear() { g_slots[t_id].ptr.store(nullptr, std::memory_order_release); }

struct Retired { void* p; void (*deleter)(void*); };
inline thread_local std::vector<Retired> t_retired;

inline void scan() {
    std::vector<void*> live;
    for (auto& s : g_slots) if (void* h = s.ptr.load(std::memory_order_seq_cst)) live.push_back(h);   // snapshot of every hazard
    std::sort(live.begin(), live.end());
    std::vector<Retired> keep;
    for (auto& r : t_retired) {
        if (std::binary_search(live.begin(), live.end(), r.p)) keep.push_back(r);     // still protected: try again later
        else r.deleter(r.p);                                                           // safe to free
    }
    t_retired.swap(keep);
}
template <class T> void retire(T* p) {
    t_retired.push_back({p, [](void* q) { delete static_cast<T*>(q); }});
    if (t_retired.size() >= 2 * kMaxThreads) scan();                                  // amortised: scan once per ~16 retirements
}
inline void drain() { scan(); }
}  // namespace hp

// ---- The Treiber stack, now with safe pop() ----
template <class T>
class Stack {
    struct Node { T value; Node* next; };
    std::atomic<Node*> head_{nullptr};
public:
    ~Stack() { for (Node* n = head_.load(); n;) { Node* nx = n->next; delete n; n = nx; } }
    void push(T v) {
        Node* n = new Node{std::move(v), head_.load(std::memory_order_relaxed)};
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    bool pop(T& out) {
        for (;;) {
            Node* n = hp::protect(head_);                                // n cannot be freed while our hazard slot names it
            if (!n) { hp::clear(); return false; }
            Node* next = n->next;                                        // safe to read: protected (this read was the use-after-free before)
            if (head_.compare_exchange_strong(n, next, std::memory_order_acquire, std::memory_order_relaxed)) {
                out = std::move(n->value);
                hp::clear();
                hp::retire(n);                                           // not delete: another thread may have this node in its hazard slot
                return true;
            }
        }
    }
};

int main() {
    Stack<long> st;
    constexpr int kThreads = 2, kOps = 100'000;
    std::atomic<long> pushed{0}, popped{0};
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&, t] {
                long local_push = 0, local_pop = 0, v;
                for (int i = 1; i <= kOps; ++i) {
                    st.push(t * kOps + i); local_push += t * kOps + i;
                    if (st.pop(v)) local_pop += v;
                }
                pushed += local_push; popped += local_pop;
                hp::drain();
            });
    }
    long v, rest = 0;
    while (st.pop(v)) rest += v;
    hp::drain();
    std::printf("pushed sum %ld, popped sum %ld (%s)\n", pushed.load(), popped.load() + rest, pushed.load() == popped.load() + rest ? "equal" : "MISMATCH");
    return pushed.load() == popped.load() + rest ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
pushed sum 20000100000, popped sum 20000100000 (equal)
```

This runs under **AddressSanitizer + UBSan** with two threads hammering push/pop, and it is clean. Replace `hp::retire(n)` with `delete n` and ASan reports `heap-use-after-free` within a fraction of a second (Exercise 2): `Node* next = n->next` reads a node another thread has just popped and freed. Three details deserve respect: (1) the **`seq_cst`** store of the hazard slot followed by a `seq_cst` re-read is the Store-Buffering pattern of Chapter 30; with `release`/`acquire` the scheme is broken on x86 too; (2) `retire` is amortised by batching; (3) this version has one slot per thread and a fixed thread table: real implementations (Folly, the C++26 design) use a registry, multiple slots, domains and thread-local caches.

### Experiment 4 ✅: Fix 3: epoch-based reclamation and a Michael–Scott queue

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined link=-pthread timeout=180
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// ---- Epoch-based reclamation (3-epoch scheme; educational, fixed thread table) ----
namespace ebr {
constexpr int kMaxThreads = 8;
constexpr std::uint64_t kInactive = ~std::uint64_t(0);
struct alignas(64) Rec { std::atomic<std::uint64_t> epoch{kInactive}; };
inline std::atomic<std::uint64_t> g_epoch{0};
inline std::array<Rec, kMaxThreads> g_recs;
inline std::atomic<int> g_next_id{0};
inline thread_local int t_id = g_next_id.fetch_add(1);

struct Retired { void* p; void (*del)(void*); };
inline thread_local std::vector<Retired> t_limbo[3];            // limbo[e % 3]: nodes retired while the global epoch was e
inline thread_local std::uint64_t t_limbo_epoch[3] = {};
inline thread_local unsigned t_ops = 0;

inline void try_advance() {
    std::uint64_t e = g_epoch.load(std::memory_order_acquire);
    for (auto& r : g_recs) {
        std::uint64_t le = r.epoch.load(std::memory_order_acquire);
        if (le != kInactive && le != e) return;                  // an active thread is still in an older epoch: cannot advance
    }
    g_epoch.compare_exchange_strong(e, e + 1, std::memory_order_acq_rel);
}
inline void flush_old(std::uint64_t e) {
    // nodes retired two or more epochs ago cannot be referenced by any active thread
    for (int i = 0; i < 3; ++i)
        if (t_limbo_epoch[i] + 2 <= e) { for (auto& r : t_limbo[i]) r.del(r.p); t_limbo[i].clear(); }
}
struct Guard {
    Guard()  { g_recs[t_id].epoch.store(g_epoch.load(std::memory_order_acquire), std::memory_order_seq_cst); }   // announce (StoreLoad again)
    ~Guard() { g_recs[t_id].epoch.store(kInactive, std::memory_order_release); }
    Guard(const Guard&) = delete;
};
template <class T> void retire(T* p) {
    std::uint64_t e = g_epoch.load(std::memory_order_acquire);
    auto& slot = t_limbo[e % 3];
    if (t_limbo_epoch[e % 3] != e) { flush_old(e); t_limbo_epoch[e % 3] = e; }   // reusing the bucket: older contents are safely free-able
    slot.push_back({p, [](void* q) { delete static_cast<T*>(q); }});
    if (++t_ops % 64 == 0) { try_advance(); flush_old(g_epoch.load(std::memory_order_acquire)); }
}
inline std::mutex g_orphan_mu;
inline std::vector<Retired> g_orphans;                           // limbo lists handed over by exiting threads
inline void orphan_my_limbo() {                                  // call at thread exit: we can't free yet (others may be inside a Guard)
    std::lock_guard lk(g_orphan_mu);
    for (int i = 0; i < 3; ++i) { for (auto& r : t_limbo[i]) g_orphans.push_back(r); t_limbo[i].clear(); }
}
inline void drain_all() {          // call only when no thread is inside a Guard (e.g. after joining everything)
    for (int i = 0; i < 3; ++i) { for (auto& r : t_limbo[i]) r.del(r.p); t_limbo[i].clear(); }
    std::lock_guard lk(g_orphan_mu);
    for (auto& r : g_orphans) r.del(r.p);
    g_orphans.clear();
}
}  // namespace ebr

// ---- Michael-Scott lock-free FIFO queue ----
template <class T>
class MsQueue {
    struct Node { T value; std::atomic<Node*> next{nullptr}; };
    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
public:
    MsQueue() { Node* dummy = new Node{}; head_.store(dummy); tail_.store(dummy); }
    ~MsQueue() { for (Node* n = head_.load(); n;) { Node* nx = n->next.load(); delete n; n = nx; } }

    void push(T v) {
        Node* n = new Node{}; n->value = std::move(v);
        ebr::Guard g;
        for (;;) {
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = t->next.load(std::memory_order_acquire);
            if (t != tail_.load(std::memory_order_acquire)) continue;                         // tail moved: re-read
            if (next) { tail_.compare_exchange_weak(t, next, std::memory_order_release, std::memory_order_relaxed); continue; }   // help a lagging tail
            Node* expected = nullptr;
            if (t->next.compare_exchange_weak(expected, n, std::memory_order_release, std::memory_order_relaxed)) {    // linearization point of push
                tail_.compare_exchange_strong(t, n, std::memory_order_release, std::memory_order_relaxed);             // swing tail (others may help)
                return;
            }
        }
    }
    bool pop(T& out) {
        ebr::Guard g;
        for (;;) {
            Node* h = head_.load(std::memory_order_acquire);
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = h->next.load(std::memory_order_acquire);
            if (h != head_.load(std::memory_order_acquire)) continue;
            if (!next) return false;                                                           // empty (linearization point)
            if (h == t) { tail_.compare_exchange_weak(t, next, std::memory_order_release, std::memory_order_relaxed); continue; }   // tail lagging: help
            T v = next->value;                                                                 // read BEFORE the CAS (another thread may free `next` right after)
            if (head_.compare_exchange_weak(h, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {   // linearization point of pop
                out = std::move(v);
                ebr::retire(h);                                                                // old dummy: deferred free (epoch protects other readers)
                return true;
            }
        }
    }
};

int main() {
    MsQueue<long> q;
    constexpr int kProducers = 2, kPerProducer = 100'000;
    std::atomic<long> consumed_sum{0}, consumed_count{0};
    std::atomic<bool> done{false};
    {
        std::vector<std::jthread> ts;
        for (int c = 0; c < 1; ++c)
            ts.emplace_back([&] {
                long v, s = 0, n = 0;
                for (;;) {
                    if (q.pop(v)) { s += v; ++n; }
                    else if (done.load(std::memory_order_acquire)) { if (!q.pop(v)) break; s += v; ++n; }
                }
                consumed_sum += s; consumed_count += n;
                ebr::orphan_my_limbo();
            });
        std::vector<std::jthread> ps;
        for (int p = 0; p < kProducers; ++p) ps.emplace_back([&, p] { for (int i = 1; i <= kPerProducer; ++i) q.push(p * kPerProducer + i); ebr::orphan_my_limbo(); });
        ps.clear();                                               // join producers
        done.store(true, std::memory_order_release);
    }
    long expected_sum = 0;
    for (int p = 0; p < kProducers; ++p) for (int i = 1; i <= kPerProducer; ++i) expected_sum += p * kPerProducer + i;
    std::printf("consumed %ld items (expected %d), sum %s\n", consumed_count.load(), kProducers * kPerProducer,
                consumed_sum.load() == expected_sum ? "matches" : "MISMATCH");
    ebr::drain_all();
    return (consumed_count == kProducers * kPerProducer && consumed_sum == expected_sum) ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
consumed 200000 items (expected 200000), sum matches
```

The queue is *lock-free*: a stalled thread cannot block the others. Observe the places where the proof lives: the linearization points are the successful CAS on `tail->next` (push) and the CAS on `head` (pop); helping a lagging tail is what makes it lock-free rather than blocking (a stalled producer between its two CASes cannot stop others); and `T v = next->value` is read **before** the head CAS because after it another thread may retire `next` (the epoch guard keeps it alive until we leave, but the *value* of a popped node is only ours once the CAS succeeds). Exiting threads hand their limbo lists to a global *orphan list* that is freed after everything is joined (without this, LeakSanitizer reports the retired nodes as leaks). The toy still bounds nothing if a thread stalls inside a guard; a real implementation (Folly, crossbeam-epoch) handles both.

### Experiment 5 🔧: Does lock-free actually win? Mutex queue vs SPSC ring

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=300
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// --- 1. mutex + std::queue
class MutexQueue {
    std::mutex m_; std::queue<long> q_;
public:
    void push(long v) { std::lock_guard lk(m_); q_.push(v); }
    bool pop(long& v) { std::lock_guard lk(m_); if (q_.empty()) return false; v = q_.front(); q_.pop(); return true; }
};
// --- 2. SPSC ring (Chapter 31)
class Spsc {
    static constexpr std::size_t N = 4096;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<long, N> buf_{};
public:
    bool try_push(long v) { auto t = tail_.load(std::memory_order_relaxed); if (t - head_.load(std::memory_order_acquire) == N) return false; buf_[t % N] = v; tail_.store(t + 1, std::memory_order_release); return true; }
    bool try_pop(long& v) { auto h = head_.load(std::memory_order_relaxed); if (h == tail_.load(std::memory_order_acquire)) return false; v = buf_[h % N]; head_.store(h + 1, std::memory_order_release); return true; }
};

template <class Push, class Pop>
double run(long items, Push push, Pop pop) {
    auto t0 = std::chrono::steady_clock::now();
    std::jthread prod([&] { for (long i = 1; i <= items; ++i) push(i); });
    long got = 0, sum = 0, v;
    while (got < items) if (pop(v)) { ++got; sum += v; }
    prod.join();
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / items;
    if (sum != items * (items + 1) / 2) std::puts("  !!! lost or duplicated items");
    return ns;
}

int main() {
    constexpr long N = 3'000'000;
    std::printf("1 producer + 1 consumer, %ld items, ns per item (lower is better)\n", N);
    { MutexQueue q; std::printf("  mutex + std::queue      %7.1f\n", run(N, [&](long v) { q.push(v); }, [&](long& v) { return q.pop(v); })); }
    { Spsc q;       std::printf("  SPSC ring (wait-free)   %7.1f\n", run(N, [&](long v) { while (!q.try_push(v)) {} }, [&](long& v) { return q.try_pop(v); })); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1 producer + 1 consumer, 3000000 items, ns per item (lower is better)
  mutex + std::queue        128.2
  SPSC ring (wait-free)      21.5
```

This is the honest comparison you should run before adopting anything lock-free. On this 2-vCPU VM the specialised SPSC ring costs about 21 ns per item and the mutex queue about 128 ns: **roughly 6x**, because with a producer and a consumer hammering the same lock the mutex is permanently contended (cache-line ping-pong plus futex sleeps and wake-ups). That gap is real but it applies to the *specialised* structure: an SPSC ring has one writer per index, no CAS and no reclamation. A general-purpose lock-free MPMC queue such as Michael–Scott pays for a heap allocation, two CASes and reclamation per element, so it does **not** inherit that win; benchmark it yourself (Exercise 4) before assuming it beats a mutex queue. The case for general lock-free queues is **tail latency, robustness against a descheduled lock holder, and high-core-count contention**, not average throughput on a small machine.

---

## 8. Assembly / runtime investigation

```bash
# (1) Reproduce use-after-free / ABA-induced corruption under tools
g++-14 -std=c++23 -O1 -g -fsanitize=address,undefined -pthread prog.cpp && ./a.out
g++-14 -std=c++23 -O1 -g -fsanitize=thread -pthread prog.cpp && ./a.out        # understands acquire/release; reports missing ordering

# (2) Systematic concurrency testing: explore interleavings instead of hoping the scheduler hits them
#     CDSChecker / GenMC / Relacy / Loom (Rust): model-check small configurations exhaustively
genmc --c11 -- -I. queue_test.c

# (3) Stress with deliberate delays: inject sched_yield()/random sleeps at every atomic op to widen race windows
#     (a macro such as  #define YIELD() if (rand()%64==0) sched_yield()  at each CAS/load)

# (4) Hardware: contended CAS cost and line transfers
perf stat -e cycles,instructions,cache-misses ./prog
perf c2c record ./prog && perf c2c report --stdio

# (5) Memory growth of a reclamation scheme under a stalled reader (EBR): watch RSS while one thread sleeps inside a guard
/usr/bin/time -v ./prog 2>&1 | grep Maximum
valgrind --tool=massif ./prog

# (6) membarrier-based asymmetric reclamation (kernel >= 4.14)
#     syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0)  -- readers skip the fence; reclaimer pays
```

---

## 9. Implementation exercise

Build, test and benchmark in this order, each with a written correctness argument:

1. **Treiber stack + hazard pointers** (Experiment 3), extended to *K* slots per thread, a dynamic registry (`thread_local` registration with recycling), and a global retire list for exited threads.
2. **Michael–Scott queue + hazard pointers** (two slots: `head` and `next`) to compare with the EBR version.
3. **Vyukov bounded MPMC queue** (Chapter 31 challenge) with no allocation and no reclamation: the right answer when a bounded buffer is acceptable.
4. **RCU-style read-mostly map**: an immutable `std::unordered_map` behind `atomic<shared_ptr<const Map>>`; writers copy-modify-swap; compare readers’ throughput with a `shared_mutex` map.
5. A **stress + model-check harness**: random operations on 2–4 threads, ASan + TSan, sleep injection, and a linearizability checker (record `{invoke, response, op, result}` histories; search for a legal sequential ordering).

<details>
<summary><strong>Solution sketch: the linearizability checker idea (brute force)</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <deque>
#include <vector>

// A history entry: operation, argument/result, and its [invoke, response] real-time interval.
struct Op { bool is_push; int value; int invoke, response; };

// Brute-force: is there a total order of the ops, consistent with real-time precedence (a.response < b.invoke => a before b),
// that is a legal sequential FIFO queue execution?
bool linearizable(const std::vector<Op>& h) {
    std::vector<int> order(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) order[i] = int(i);
    do {
        bool ok = true;
        for (std::size_t i = 0; i < order.size() && ok; ++i)
            for (std::size_t j = i + 1; j < order.size() && ok; ++j)
                if (h[order[j]].response < h[order[i]].invoke) ok = false;     // order[j] finished before order[i] started, yet is placed after it
        if (!ok) continue;
        std::deque<int> q;
        for (int idx : order) {
            const Op& o = h[idx];
            if (o.is_push) q.push_back(o.value);
            else { if (q.empty() || q.front() != o.value) { ok = false; break; } q.pop_front(); }
        }
        if (ok) return true;
    } while (std::next_permutation(order.begin(), order.end()));
    return false;
}

int main() {
    // Two overlapping pushes (1 and 2) then pops that return 2 then 1: legal, because the pushes overlapped.
    std::vector<Op> good = {{true, 1, 0, 5}, {true, 2, 1, 4}, {false, 2, 6, 7}, {false, 1, 8, 9}};
    // Non-overlapping push(1) then push(2), yet pops return 2 then 1: NOT legal for a FIFO queue.
    std::vector<Op> bad  = {{true, 1, 0, 1}, {true, 2, 2, 3}, {false, 2, 4, 5}, {false, 1, 6, 7}};
    std::printf("overlapping pushes, pops 2,1  -> linearizable: %d\n", linearizable(good));
    std::printf("sequential pushes,  pops 2,1  -> linearizable: %d\n", linearizable(bad));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
overlapping pushes, pops 2,1  -> linearizable: 1
sequential pushes,  pops 2,1  -> linearizable: 0
```

Linearizability checking is NP-hard in general but practical for short histories (Wing & Gong; Knossos, Lincheck, `porcupine`). Run thousands of 8–12-operation randomized histories against your queue and any failure is a bug with a witness.

</details>

---

## 10. Real-world example

| Where | Lock-free technique in use |
|---|---|
| **Linux kernel** | RCU everywhere (routing tables, dcache, process lists); lock-free `llist` (a CAS-based singly linked list), `kfifo` (SPSC ring), per-CPU data |
| **Java `java.util.concurrent`** | `ConcurrentLinkedQueue` is Michael–Scott with GC; `ConcurrentHashMap` mixes CAS and fine-grained locks; `LongAdder` is a sharded counter |
| **Folly** | `hazptr` (hazard pointers), `MPMCQueue` (Vyukov-style bounded), `AtomicUnorderedMap`, `ConcurrentHashMap` using hazard pointers |
| **moodycamel::ConcurrentQueue** | Per-producer sub-queues plus lock-free consumers; widely used for its speed |
| **Rust: crossbeam** | `crossbeam-epoch` (EBR), `crossbeam-queue`, `crossbeam-deque` (Chase–Lev work-stealing) |
| **Work-stealing schedulers** | Chase–Lev deque in Cilk, TBB, Go's runtime, Tokio, rayon: the owner pushes/pops at one end (cheap), thieves CAS at the other |
| **Allocators** | tcmalloc / jemalloc / mimalloc: lock-free central free lists and per-thread caches; the allocator cannot use a lock-based design while allocating its own nodes |
| **Logging / tracing systems** | Per-thread SPSC ring buffers drained by a background thread (no producer-side locks, bounded latency) |
| **Qt** | `QAtomicPointer` and lock-free paths in `QObject`'s deferred deletion, `QSemaphore` fast paths, `QReadWriteLock`; the event queue itself is lock-protected for simplicity (Chapter 48) |
| **Audio / real-time** | Lock-free SPSC buffers between the audio thread and the UI thread; the audio callback may not take a lock, allocate or make a syscall |

> **Opinion.** Lock-free is a *last resort* with a precise justification, not a mark of sophistication. My rule: **do not write lock-free code in application software**. Use a mutex. If a profile shows a lock is the problem, first shrink the critical section, shard the lock, or restructure to avoid sharing. If you genuinely need non-blocking progress (a real-time thread, a signal handler, a latency SLO that a descheduled lock holder would violate), then **use a vetted library** (Boost.Lockfree `spsc_queue`, Folly, moodycamel, libcds), or implement the one structure for which the proof is short: the **SPSC ring buffer**. Hand-rolled Treiber stacks and Michael–Scott queues belong in libraries maintained by people who model-check them. If you do write one: write the linearization point of every operation in a comment; state the memory order justification for every atomic; pick **one** reclamation scheme and know its failure mode (EBR + a stalled reader = unbounded memory; hazard pointers = a fence per protected pointer); test with ASan, TSan, interleaving injection and a model checker; and benchmark against the mutex version on *your* hardware with *your* contention, because — as Experiment 5 shows — it often loses.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **ABA** in pointer CAS with node reuse | Silent structure corruption, lost elements | Tagged pointers, hazard pointers, epochs, or never reuse nodes during the operation |
| `delete` of a node another thread may still read | Use-after-free (ASan), rare crashes | Safe memory reclamation (hazard pointers/EBR/RCU/ref-count) |
| Reading `next->value` after the CAS that unlinks `next` | Data race / UAF | Read before the CAS, or protect `next` too |
| Hazard store with `release` / validation with `acquire` | Missing StoreLoad fence: the reclaimer can miss the hazard. x86 reorders store→load too, so this is broken there as well | `seq_cst` store + `seq_cst` re-read (or `membarrier`) |
| EBR with long critical sections or a blocked thread inside a guard | Memory growth without bound | Keep guards short; never block inside; or use hazard pointers |
| Tag too narrow | Wrap-around ABA after a long stall | 32+ bits, DWCAS, or reclamation instead |
| Relying on a 16-byte `atomic` being lock-free | Hidden mutex (and not signal-safe) | `is_always_lock_free` check; pack into 64 bits; indices |
| `relaxed` where `release`/`acquire` needed | Works on x86, fails on ARM | Chapter 30 discipline; TSan; ARM testing |
| Unbounded retries in a CAS loop under contention | Livelock-like behaviour, latency spikes | Backoff, bounded retries then fall back, combining (flat combining) |
| Memory allocation inside a "lock-free" operation | The allocator may block (global lock) | Pre-allocate nodes (pool), bounded structure, or a lock-free allocator |
| False sharing on `head`/`tail` | Scaling collapse | `alignas(64)`; separate cache lines |
| Treating lock-free as "wait-free" | A thread can starve | Wait-free algorithms or fairness mechanisms (helping, tickets) |
| Testing only with ordinary stress tests | Rare interleavings never exercised | Interleaving injection, model checkers (GenMC, CDSChecker), linearizability checking |
| Accepting a benchmark that ignores contention | Wrong conclusion about benefits | Benchmark at realistic thread counts, with and without a stalled thread |
| Hand-rolling instead of using a library | Latent bugs, no maintenance | Boost.Lockfree, Folly, moodycamel, libcds |

---

## 12. Exercises

1. **ABA by hand.** Take Experiment 1 and construct two more ABA scenarios: one for a queue (head/tail), one for a lock-free linked list with a deleted-mark. Draw the state before/after.
2. **Break it.** In Experiment 3, replace `hp::retire(n)` with `delete n` and run with 2–4 threads under ASan. Capture the report and explain every frame. Then restore `retire` but weaken the hazard publication to `release`/`acquire` and try to provoke a failure with sleeps injected.
3. **Memory bound.** Instrument Experiment 3 to report the maximum size of the retired list per thread, and Experiment 4 to report limbo size. Make one thread stall inside a guard / holding a hazard pointer and show how each scheme behaves.
4. **Honest benchmark.** Benchmark `MutexQueue`, `MsQueue(EBR)`, `Spsc`, and Boost.Lockfree `queue` for 1P1C, 2P1C, 4P4C (on a bigger machine) at throughput **and** p99/p99.9 latency. Add a "noisy neighbour" thread that pins and spins. Summarise when each wins.
5. **Chase–Lev deque.** Implement the work-stealing deque (owner `push/pop` at the bottom, thieves `steal` at the top), with the C++11 memory orders from Lê et al. (2013), "Correct and Efficient Work-Stealing for Weak Memory Models". Stress-test and model-check it.
6. **RCU.** Implement a userspace RCU (epoch-based) `read_lock/read_unlock/synchronize_rcu` and a lock-free-readers linked list with `rcu_retire`; compare with `shared_mutex` readers at 1..N cores.
7. **Flat combining.** Implement a combining queue: threads publish requests in per-thread slots, one thread (the combiner) executes all pending requests under a lock. Compare with MS queue and mutex queue under high contention; explain why it can win.
8. **Reading.** Read the Michael–Scott paper (1996), Michael's hazard pointer paper (2004), and one production implementation (Folly's `hazptr` or crossbeam-epoch). Write a one-page summary of the differences from our educational versions.

---

## 13. Challenge: a lock-free hash map, or the decision not to build one

Task A (build): implement a **lock-free insert/lookup/erase hash table** (open addressing with CAS on slots, or split-ordered lists), with hazard pointers or epochs for reclamation and tombstones for erase. Provide the linearization point of each operation, a TSan/ASan stress test, a model-check of a 2-thread/3-key scenario, and a benchmark against (a) `std::unordered_map` + `mutex`, (b) a sharded `unordered_map` with 64 locks, (c) `absl::flat_hash_map` behind a `shared_mutex`, (d) `folly::ConcurrentHashMap` if available.

Task B (decide): write a one-page design memo, for a team that wants "a lock-free map because it's faster", that uses *your measurements* to answer: at what read/write ratio, key count, thread count and latency target does each option win? What is the maintenance cost? What would you ship? The grading criterion for the whole challenge is the quality of Task B's argument.

---

## 14. Knowledge check

1. Define lock-free and wait-free. Give a structure that is wait-free and one that is lock-free but not wait-free.
2. What exactly is the ABA problem, and why doesn't `compare_exchange` detect it?
3. Name three ways to prevent ABA, and say which of them also prevents use-after-free.
4. Why can't you simply `delete` a node after unlinking it in a lock-free stack?
5. How do hazard pointers work? Why is a `seq_cst` fence (StoreLoad) required between publishing the hazard and re-validating?
6. How does epoch-based reclamation decide a node is safe to free? What happens if one thread stays inside a critical region forever?
7. Compare hazard pointers and EBR on reader overhead, bound on garbage, and robustness to stalled threads.
8. What is a linearization point? State it for push and pop in the Treiber stack.
9. Why can a lock-free MS queue be slower than a mutex queue? When is it faster?
10. What does `T v = next->value;` before the CAS in the Michael–Scott `pop` protect against?
11. Why is the SPSC ring buffer wait-free, and why is that the easiest lock-free structure to get right?
12. State your policy on writing lock-free code in an application, and the checklist you would require before accepting it in review.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Lock-free: whenever threads run, some operation completes in a bounded number of steps (the system as a whole progresses). Wait-free: every operation completes in a bounded number of its own steps. Wait-free: SPSC ring buffer, `fetch_add` counter. Lock-free but not wait-free: Treiber stack / CAS-loop structures (a thread can retry forever while others succeed).
2. A thread reads value A, other threads change it to B and back to A (possibly freeing/reusing the node), and the first thread's CAS succeeds because the value is A, even though the structure's state changed. CAS compares only the value, not the history.
3. Tag/version counters in the CAS word; hazard pointers (or other safe reclamation: epochs, RCU, GC) so a node held by a thread cannot be reused; not reusing nodes (a monotone pool). Only the reclamation schemes also prevent use-after-free; tags prevent ABA only.
4. Another thread may have read the pointer before the unlink and still be about to dereference it, so deleting causes use-after-free; or the memory may be reused (ABA).
5. A reader stores the pointer in a globally visible hazard slot, then re-reads the source to confirm the node is still reachable; a reclaimer scans all slots before freeing a retired node. The StoreLoad fence guarantees the hazard store is visible to the reclaimer *before* the reader's re-read; otherwise both could miss each other's action (the store-buffering pattern).
6. A node retired in epoch e is freed when the global epoch has advanced by two (so every active thread has been seen in a later epoch). A thread stuck in a critical region prevents the epoch from advancing, so retired memory accumulates without bound.
7. Reader overhead: EBR ≈ one store per enter/exit; HP = store + full fence + re-check per protected pointer. Garbage bound: HP bounded (≈ threads × slots); EBR unbounded if a thread stalls. Robustness: HP tolerates stalled threads; EBR does not.
8. The instant an operation appears to take effect atomically. Treiber push: the successful CAS on `head`. Pop: the successful CAS on `head` (or the load observing `nullptr` for an empty stack).
9. It performs more work per operation (a heap allocation, two CASes, reclamation) than an uncontended mutex push/pop; a good mutex is cheap at low contention. It wins under heavy contention, when a lock holder may be descheduled, and on tail latency and real-time constraints.
10. The popped sentinel `next` can be retired and freed by another thread right after our head CAS; reading its value first (while protected by the epoch/hazard) avoids touching it after it may be reclaimed; the value is ours once the CAS succeeds.
11. Each index has exactly one writer, so plain release/acquire stores/loads suffice; there are no CAS retries, no ABA and no reclamation (the buffer is fixed). The proof is short (two happens-before edges).
12. Don't, unless required by real-time/non-blocking constraints; use a vetted library. If hand-written: written linearization points and memory-order justifications, one named reclamation scheme with its failure mode, ASan/TSan/stress with injected delays and a model-checker run, a linearizability test, and benchmarks showing a win over the mutex version on target hardware.

</details>

---

[← Previous: Chapter 31](31-atomics.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 33 — C++20 coroutines →](../part-13-coroutines/33-coroutines.md)
