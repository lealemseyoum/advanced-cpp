# Chapter 11 — Modern Containers

> **Part V · The modern standard library** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 9](../part-04-generic-programming/09-type-traits.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, replaced `operator new`, `-D_GLIBCXX_DEBUG`, ASan

[← Previous: Chapter 10](../part-04-generic-programming/10-concepts.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 12 — Views and non-owning types →](12-views-and-non-owning-types.md)

---

**In one sentence:** a standard container is a *contract about three things*: **where the elements live** (layout and allocation), **which operations invalidate what** (iterators, pointers, references), and **what it costs on real hardware**, and the Big-O table is the least useful of the three.

**By the end of this chapter you can:**

- draw the memory layout of `vector`, `array`, `deque`, `list`, `forward_list`, `map`/`set` and `unordered_map` and count their allocations
- state, from memory, which operations invalidate iterators for each container, and *why* the layout forces it
- predict and measure when a "worse" Big-O container (`vector`) beats a "better" one (`list`, `map`)
- choose a container by *access pattern and ownership*, not by habit
- use `std::array`, `std::span` and (C++23) `std::mdspan`/`std::flat_map` correctly, and know their compiler support

---

## 1. Problem

Every program stores sequences and associations of things. The questions that actually matter in production are not "what is the complexity of `insert`" but:

| Question | Why it matters |
|---|---|
| **How many allocations** does building 10⁶ elements cost? | `malloc` is 20–100 ns plus cache misses; it often dominates |
| **Are the elements contiguous?** | Contiguity gives prefetching, vectorization, `memcpy`, and `span`/C-API interop |
| **Does my pointer to an element survive an insert?** | Pointer stability decides whether you can hand references to other components |
| **What does a lookup touch in memory?** | One cache line or twelve pointer hops? |
| **Who owns the elements, and how much per-element overhead is there?** | A `map<int,int>` stores 8 useful bytes in a 40-byte node plus allocator overhead |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1994 | Stepanov and Lee's STL enters C++98: containers, iterators, algorithms, **complexity guarantees as part of the specification** |
| 1998 | `vector`, `deque`, `list`, `map`, `set`, `multimap`, `multiset` (red-black trees in all implementations) |
| 2005–2011 | `unordered_*` (TR1, C++11), `array`, `forward_list`: hash tables specified with **separate chaining** (the interface forces buckets of nodes), a decision still blamed for slow `unordered_map` |
| 2011 | Move semantics: `vector<string>` growth stops copying; `emplace` |
| 2017 | `std::pmr` containers, node handles (`extract`/`insert` for node-based containers), `try_emplace`, `string_view` |
| 2020 | `std::span`, `ssize`, `erase`/`erase_if`, `contains`, constexpr `vector`/`string` |
| 2023 | **`std::mdspan`**, **`flat_map` / `flat_set`** (sorted-vector adaptors), `std::generator` |
| 2026 | `std::inplace_vector` (fixed-capacity vector), `hive` (stable-pointer bucket container), `std::linalg`: adopted for C++26, mostly *not yet shipped* in GCC 14 |

The arc is a retreat from node-based containers toward **contiguous storage**, because hardware changed: memory latency stayed flat while CPU speed grew a hundredfold, so pointer chasing became the dominant cost. `flat_map`, `inplace_vector` and `mdspan` are the standard admitting it.

---

## 3. Modern solution

```cpp
std::vector<int>        v;                  // contiguous, growable: the default
std::array<int, 8>      a;                  // contiguous, fixed, no allocation
std::span<int>          s{v};               // non-owning view of contiguous elements
std::deque<int>         d;                  // chunked: stable references on push_back/push_front
std::list<int>          l;                  // node-based: stable everything, slow traversal
std::map<K, V>          m;                  // balanced tree: ordered, stable nodes
std::unordered_map<K,V> u;                  // hash table of nodes: O(1) average lookup, stable references
std::flat_map<K, V>     f;                  // C++23: two sorted vectors; fast lookup, slow insert
```

> **The default is `std::vector`.** Pick another container only after you can state, in one sentence, what requirement `vector` fails (pointer stability? O(1) front removal? ordered iteration with many inserts?), and have measured it.

---

## 4. Mental model

### Containers in memory

```text
 std::vector<int>      [ptr|size|cap]  ──►  [ 1 | 2 | 3 | 4 | _ | _ ]            ONE block, contiguous
 (24 bytes on stack)                         ◄── size ──►◄─ spare ─►

 std::array<int,4>     [ 1 | 2 | 3 | 4 ]                                         inside the object itself (no heap)

 std::deque<int>       map ─► [ptr][ptr][ptr][ptr]                               an array of pointers to fixed-size chunks
 (80 bytes)                     │    │    │    │
                                ▼    ▼    ▼    ▼
                              [128 ints][128 ints]...                            (libstdc++: 512-byte chunks)

 std::list<int>        head ⇄ [prev|next|1] ⇄ [prev|next|2] ⇄ ...               one heap node PER element
                                                                                  node = 16 + 4 (+4 pad) = 24 bytes

 std::map<int,int>     red-black tree: [color|parent|left|right|key|value]      one heap node per element, 40 bytes
                                                                                  + allocator header ⇒ 48 bytes real

 std::unordered_map    buckets ─► [ptr][ptr][ptr]...   each ► [next|key|value|(hash)] node chain
                                                                                  one node per element + the bucket array
```

### Three costs, in the order they usually matter

```text
 1. ALLOCATIONS         (malloc/free: tens of ns, and each node lands wherever the allocator likes)
 2. CACHE BEHAVIOUR     (64-byte lines; a pointer hop is a potential ~100 ns miss; contiguous reads are prefetched)
 3. Big-O               (matters only once N is large *and* 1 and 2 are equal)
```

Chapter 27 builds the cache model rigorously; this chapter measures its consequences.

### Invalidation follows from layout

If the elements are in **one contiguous block**, growing it may have to move them: every iterator, pointer and reference dies. If each element has **its own node**, nothing ever moves; only the erased element's handles die. *You can derive the whole invalidation table from the layout diagram.* (Experiment 3 does that.)

---

## 5. Language rules

### 5.1 What the standard specifies  `[container.requirements]`

For each container the standard specifies: the operations, their **complexity**, **iterator/reference invalidation**, **exception guarantees**, and **allocator behavior**. It deliberately does **not** specify the layout, **except** where an interface forces it:

| Layout fact | Guaranteed by the standard? | Source |
|---|---|---|
| `vector` elements are contiguous; `&v[0] + i == &v[i]` | **Yes** (since C++03 TC1; `contiguous_iterator` in C++20) | `[vector.overview]` |
| `array<T,N>` is an aggregate holding `T[N]` | Yes (no padding promised *between* elements; `sizeof` may exceed `N*sizeof(T)` only by padding at the end, which libraries don't add) | `[array.overview]` |
| `string` is contiguous and null-terminated via `c_str()` | Yes (since C++11) | `[string.require]` |
| `deque` is a chunked array | **No**: only "random access, O(1) insert at both ends, references stable on push_back/front" | the layout is **implementation-defined** |
| `map`/`set` are red-black trees | **No**: *"balanced"* is implied by the complexity bounds; all implementations use RB trees | |
| `unordered_*` use separate chaining (node per element) | **Effectively yes**: the bucket interface (`begin(n)`, `bucket_size`, `bucket_count`) and reference stability on rehash force it | `[unord.req]` |
| Growth factor of `vector` | **No**: libstdc++ ×2, libc++ ×2, MSVC ×1.5 | implementation |
| Small buffer for `string` | **No**: libstdc++/libc++ have SSO (16 / 22 bytes), but it is a quality-of-implementation property | |

### 5.2 Complexity guarantees: what Big-O hides

| Container | Index | Search | Insert/erase middle | push_back | Notes |
|---|---|---|---|---|---|
| `vector` | O(1) | O(n) / O(log n) sorted | O(n) *memmove* | **amortized** O(1) | Worst case O(n) at reallocation |
| `deque` | O(1) | O(n) | O(n) | O(1) (amortized for `map` array) | Also `push_front` |
| `list` | – | O(n) | **O(1) given an iterator** | O(1) | Finding the iterator is O(n) |
| `forward_list` | – | O(n) | O(1) *after* an iterator | – | 8 bytes per node |
| `map`/`set` | – | O(log n) | O(log n) | – | Ordered iteration |
| `unordered_*` | – | **O(1) average**, O(n) worst | O(1) average | – | Rehash: O(n) |
| `flat_map` (C++23) | – | O(log n) binary search | **O(n)** (shifts two vectors) | – | Iteration is a contiguous scan |

*Amortized* means: over a sequence of *n* operations the total cost is O(n); an individual `push_back` can be O(n). It's not a guarantee about **latency** (real-time code must `reserve`).

### 5.3 Iterator, pointer and reference invalidation  `[container.reqmts]`

Full table (the standard's wording condensed). **I** = iterators, **R** = pointers/references to elements.

| Container | Operation | Invalidated |
|---|---|---|
| `vector` | `push_back`/`emplace_back` / `insert` | **all** (I and R) if capacity changes; otherwise only at and after the insertion point (I), R from there |
| | `erase` | I and R **at and after** the erased element |
| | `reserve`, `shrink_to_fit`, `resize` growing, `assign` | all if reallocation happens |
| | `swap`/move | iterators and refs stay valid but now refer into the *other* container (`swap`) / the new owner (move) |
| `deque` | `push_back`/`push_front` | **all iterators**, but **references to elements stay valid** |
| | `insert`/`erase` in the middle | all iterators and references |
| | `erase` at the front or back | only the erased element (+ `end()`) |
| `list`, `forward_list` | any insert | **nothing** |
| | `erase` | only the erased element |
| `map`/`set` (+multi) | insert | **nothing** |
| | `erase` | only the erased element |
| `unordered_*` | insert **without** rehash | I: none; R: none |
| | insert **with** rehash | **all iterators**, but **references and pointers to elements stay valid** |
| | `erase` | only the erased element |
| `flat_map`/`flat_set` | any insert/erase | like `vector`: all if reallocation; otherwise at and after |
| `string` | like `vector`, **plus**: SSO means a *move or swap* can invalidate pointers into the *string's own buffer*! | |

The two surprising lines: **`deque::push_back` invalidates iterators but not references** (the chunk map may be reallocated, the chunks never move), and **`unordered_map` rehash invalidates iterators but not references**. These come straight out of the layouts in §4.

### 5.4 Allocators and `noexcept`

Every container takes an allocator (`vector<T, Alloc>`): [Chapter 26](../part-10-memory/26-allocators-and-memory-resources.md). Two rules matter now:

- move construction is `noexcept` for all standard containers (stealing the pointer); **move assignment** is `noexcept` only if the allocator propagates or is always equal (`allocator_traits::is_always_equal`). With `std::pmr` allocators it is *not* always-equal, which is why `pmr::vector` move assignment can allocate.
- `vector<T>::push_back` requires `T` to be `MoveInsertable`; growth uses `move_if_noexcept` (Chapter 6).

### Layer check

| Layer | What it decides |
|---|---|
| **Standard** | Operations, complexity, invalidation, exception safety, vector contiguity |
| **Compiler / library** | Layout of `deque`, `string` SSO size, growth factor, hash function and bucket count policy, node size |
| **ABI** | The layout of `std::string`/`list` is **ABI-frozen** in libstdc++ (the dual ABI: `_GLIBCXX_USE_CXX11_ABI`); libc++ uses an `__1` inline namespace |
| **OS** | `malloc` behaviour: glibc arenas, `mmap` threshold (128 KiB) for large `vector` blocks, page faults on first touch |
| **CPU** | Cache lines, prefetcher, TLB: contiguous wins (Chapter 27) |

---

## 6. Implementation model (libstdc++ 14)

| Container | `sizeof` | Node/chunk | Growth |
|---|---|---|---|
| `vector<T>` | 24 (begin, end, end-of-storage) | one block | capacity ×2 (exactly: `size + max(size, n)`) |
| `string` | 32 (ptr, size, union{capacity, 16-byte SSO buffer}) | SSO ≤ 15 chars | ×2 |
| `deque<T>` | 80 (map ptr, map size, two 4-pointer iterators) | 512-byte chunks (`512 / sizeof(T)` elements, min 1) | map array ×2 |
| `list<T>` | 24 (the sentinel node: prev, next, size) | `16 + sizeof(T)` rounded to 8 | none |
| `map<K,V>` | 48 (header node + size + comparator) | `32 + sizeof(pair<K,V>)` (color, parent, left, right) | none |
| `unordered_map<K,V>` | 56 (bucket ptr, count, before-begin node, element count, rehash policy, single bucket) | `8 (next) + sizeof(pair) [+ 8 cached hash if the hash is not "fast"]` | rehash to the next prime when load factor > 1.0 |

**malloc overhead.** glibc `malloc` rounds each request up so the *chunk* is a multiple of 16 bytes and includes an 8-byte header. A 24-byte list node becomes a **32-byte chunk**; a 40-byte `map<int,int>` node a **48-byte chunk**. For a `list<int>` the real overhead is 32 bytes for 4 bytes of payload: **8×**.

---

## 7. Experiments

### Experiment 1: Count allocations and bytes

We replace the global `operator new` with a counter (Chapter 24 covers replacing allocation functions) and ask each container to hold 10⁵ `int`s.

```cpp
// @test run -std=c++23 -O2
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <list>
#include <map>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

static long g_allocs = 0, g_bytes = 0;
void* operator new(std::size_t n)                      { ++g_allocs; g_bytes += static_cast<long>(n); return std::malloc(n); }
void  operator delete(void* p) noexcept                { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept   { std::free(p); }

template <class F> void count(const char* name, F f) {
    g_allocs = g_bytes = 0;
    f();
    std::printf("  %-32s allocs=%-7ld bytes=%ld\n", name, g_allocs, g_bytes);
}

int main() {
    std::puts("sizeof of the container objects themselves:");
    std::printf("  vector=%zu deque=%zu list=%zu map=%zu unordered_map=%zu set=%zu string=%zu array<int,4>=%zu\n",
        sizeof(std::vector<int>), sizeof(std::deque<int>), sizeof(std::list<int>), sizeof(std::map<int, int>),
        sizeof(std::unordered_map<int, int>), sizeof(std::set<int>), sizeof(std::string), sizeof(std::array<int, 4>));

    std::puts("vector capacity as it grows (push_back 1000 ints):");
    {
        std::vector<int> v; std::size_t last = 0;
        std::printf("  ");
        for (int i = 0; i < 1000; ++i) { v.push_back(i); if (v.capacity() != last) { last = v.capacity(); std::printf("%zu ", last); } }
        std::puts("");
    }

    std::puts("10^5 ints:");
    const int N = 100000;
    count("vector push_back",             [&] { std::vector<int> v;               for (int i = 0; i < N; ++i) v.push_back(i); });
    count("vector reserve + push_back",   [&] { std::vector<int> v; v.reserve(N); for (int i = 0; i < N; ++i) v.push_back(i); });
    count("deque push_back",              [&] { std::deque<int> v;                for (int i = 0; i < N; ++i) v.push_back(i); });
    count("list push_back",               [&] { std::list<int> v;                 for (int i = 0; i < N; ++i) v.push_back(i); });
    count("map<int,int>",                 [&] { std::map<int, int> v;             for (int i = 0; i < N; ++i) v[i] = i; });
    count("unordered_map<int,int>",       [&] { std::unordered_map<int, int> v;   for (int i = 0; i < N; ++i) v[i] = i; });
    count("unordered_map + reserve",      [&] { std::unordered_map<int, int> v; v.reserve(N); for (int i = 0; i < N; ++i) v[i] = i; });

    std::puts("deque chunking:");
    {
        std::deque<int> d;
        int* first = &d.emplace_back(); int n = 1;
        for (int i = 0; i < 400; ++i) { int* p = &d.emplace_back(); if (p != first + n) { std::printf("  first chunk holds %d ints (= 512 bytes / sizeof(int))\n", n); break; } ++n; }
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof of the container objects themselves:
  vector=24 deque=80 list=24 map=48 unordered_map=56 set=48 string=32 array<int,4>=16
vector capacity as it grows (push_back 1000 ints):
  1 2 4 8 16 32 64 128 256 512 1024 
10^5 ints:
  vector push_back                 allocs=18      bytes=1048572
  vector reserve + push_back       allocs=1       bytes=400000
  deque push_back                  allocs=790     bytes=420656
  list push_back                   allocs=100000  bytes=2400000
  map<int,int>                     allocs=100000  bytes=4000000
  unordered_map<int,int>           allocs=100014  bytes=4326480
  unordered_map + reserve          allocs=100001  bytes=2463176
deque chunking:
  first chunk holds 128 ints (= 512 bytes / sizeof(int))
```

Read the numbers:

- **`vector` growth** allocates 18 times for 10⁵ elements (1, 2, 4, …, 131072) and touches ~1 MB for 400 KB of data (**2.6×** the memory over its lifetime); with `reserve`, **1 allocation, 400 000 bytes**. *`reserve` is the cheapest optimization in C++ whenever you know the size.*
- **`list`**: 10⁵ allocations. 24-byte requests (the 2 400 000 bytes), which glibc rounds to **32-byte chunks** (real footprint ≈ 3.2 MB for 400 KB of data).
- **`map`**: 10⁵ allocations of 40 bytes (4 000 000 requested; 48-byte chunks in reality).
- **`unordered_map`**: ~10⁵ node allocations *plus* the bucket arrays reallocated on rehash (14 more allocations, and 1.8 MB more bytes than the reserved version, which allocates 24-byte nodes plus one bucket array once).
- **`deque`**: 790 allocations: 782 chunks of 512 bytes plus the map array's growth. About **one allocation per 128 elements**: cheap, and the elements are *almost* contiguous.

### Experiment 2: Where is the first element? Layout facts you can verify

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <string>
#include <vector>

int main() {
    // vector: elements are contiguous and the iterator is (in libstdc++) a thin pointer wrapper
    std::vector<int> v = {10, 20, 30, 40};
    std::printf("vector: &v[1]-&v[0] = %td ints (contiguous)\n", &v[1] - &v[0]);
    std::printf("vector: data()==&v[0]: %d, data() is %s the object (heap)\n",
                v.data() == &v[0], (void*)v.data() == (void*)&v ? "inside" : "outside");

    // array: the elements ARE the object
    std::array<int, 4> a = {1, 2, 3, 4};
    std::printf("array : data() == (void*)&a: %d  sizeof=%zu\n", (void*)a.data() == (void*)&a, sizeof a);

    // string SSO: a short string's characters live inside the string object itself
    std::string s_short = "short", s_long(40, 'x');
    auto inside = [](const std::string& s) { auto b = (const char*)&s; return (const char*)s.data() >= b && (const char*)s.data() < b + sizeof s; };
    std::printf("string: short data inside object: %d   long data inside object: %d\n", inside(s_short), inside(s_long));

    // list: consecutive nodes are NOT adjacent (whatever the allocator hands out)
    std::list<int> l = {1, 2, 3, 4};
    std::printf("list  : node addresses differ by:");
    const int* prev = nullptr;
    for (const int& x : l) { if (prev) std::printf(" %td", (const char*)&x - (const char*)prev); prev = &x; }
    std::puts(" bytes (fresh allocator: often adjacent, but never guaranteed)");

    // map: node address order is unrelated to key order
    std::map<int, int> m; for (int k : {5, 1, 4, 2, 3}) m[k] = k;
    std::printf("map   : keys in order, address deltas:");
    const void* pp = nullptr;
    for (auto& [k, val] : m) { if (pp) std::printf(" %td", (const char*)&val - (const char*)pp); pp = &val; }
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
vector: &v[1]-&v[0] = 1 ints (contiguous)
vector: data()==&v[0]: 1, data() is outside the object (heap)
array : data() == (void*)&a: 1  sizeof=16
string: short data inside object: 1   long data inside object: 0
list  : node addresses differ by: 32 32 32 bytes (fresh allocator: often adjacent, but never guaranteed)
map   : keys in order, address deltas: 96 48 -96 -96
```

The `list` nodes, allocated back-to-back on a fresh heap, are exactly 32 bytes apart (one malloc chunk each): *adjacent*. The `map` line shows something different: the nodes were allocated in insertion order (5, 1, 4, 2, 3) but we walk them in **key** order, so the address deltas jump around (96, 48, −96, −96). Sorted order and memory order are unrelated. A fresh heap flatters node containers; in a **long-running program** with a fragmented heap even the list would scatter. This is the most common way a `std::list` benchmark misleads: Experiment 4 uses shuffled allocation to model a realistic heap.

### Experiment 3: Invalidation, observed (without invoking UB)

Dereferencing an invalidated iterator is undefined behavior, so we *observe invalidation* by comparing **addresses**, which is legal, rather than reading through stale handles.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

int main() {
    {   // vector: the address of element 0 changes on reallocation → all pointers/iterators/references die
        std::vector<int> v = {1, 2, 3};
        const int* before = v.data();
        auto cap = v.capacity();
        while (v.capacity() == cap) v.push_back(0);
        std::printf("vector       : push_back past capacity moved the data:        %s\n", before != v.data() ? "YES (all handles invalid)" : "no");

        std::vector<int> w; w.reserve(100); w.push_back(1);
        const int* b2 = w.data(); w.push_back(2);
        std::printf("vector+reserve: push_back within capacity moved the data:     %s\n", b2 != w.data() ? "YES" : "no (handles to existing elements still valid)");
    }
    {   // deque: references to existing elements survive push_back (chunks never move)
        std::deque<int> d = {1, 2, 3};
        const int* before = &d.front();
        for (int i = 0; i < 100000; ++i) d.push_back(i);
        std::printf("deque        : &front() after 1e5 push_backs is unchanged:     %s\n", before == &d.front() ? "YES (references stable; iterators are not)" : "no");
    }
    {   // list: nothing moves, ever
        std::list<int> l = {1, 2, 3};
        const int* before = &l.front();
        for (int i = 0; i < 100000; ++i) l.push_back(i);
        std::printf("list         : &front() unchanged:                             %s\n", before == &l.front() ? "YES" : "no");
    }
    {   // map: same
        std::map<int, int> m = {{1, 1}};
        const int* before = &m.begin()->second;
        for (int i = 2; i < 100000; ++i) m[i] = i;
        std::printf("map          : element address unchanged:                      %s\n", before == &m.begin()->second ? "YES" : "no");
    }
    {   // unordered_map: rehash invalidates iterators but NOT references: elements are nodes
        std::unordered_map<int, int> u = {{1, 1}};
        const int* before = &u.begin()->second;
        auto buckets = u.bucket_count();
        for (int i = 2; i < 100000; ++i) u[i] = i;
        std::printf("unordered_map: rehashed (%zu → %zu buckets); element moved:      %s\n", buckets, u.bucket_count(), before != &u.find(1)->second ? "YES" : "no (references stable)");
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
vector       : push_back past capacity moved the data:        YES (all handles invalid)
vector+reserve: push_back within capacity moved the data:     no (handles to existing elements still valid)
deque        : &front() after 1e5 push_backs is unchanged:     YES (references stable; iterators are not)
list         : &front() unchanged:                             YES
map          : element address unchanged:                      YES
unordered_map: rehashed (13 → 172933 buckets); element moved:      no (references stable)
```

Now the same bug as a *real* bug, caught by a tool:

```cpp
// @test crash -std=c++23 -O0 -fsanitize=address -g err=heap-use-after-free
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3};
    int& first = v[0];                  // reference into the vector's buffer
    for (int i = 0; i < 100; ++i) v.push_back(i);   // reallocates: `first` dangles
    std::printf("%d\n", first);         // heap-use-after-free
}
```

ASan reports `heap-use-after-free` and shows **both** the free (inside `push_back`'s reallocation) and the read. In libstdc++ debug mode (`-D_GLIBCXX_DEBUG`) *iterator* (not reference) invalidation is detected with a clean message, at the cost of a different ABI. Use it in CI; never ship it.

### Experiment 4: The cache, measured: `vector` vs `list` vs `map` vs `unordered_map`

```cpp
// @test run -std=c++23 -O2
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <list>
#include <map>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

template <class F> double ms(F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    constexpr int N = 2'000'000;
    std::mt19937 rng(42);

    std::vector<int> vec(N);
    std::iota(vec.begin(), vec.end(), 0);

    // Build the list from a SHUFFLED allocation order so its nodes are scattered like a long-lived heap:
    std::vector<int> order(N); std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
    std::list<int> lst;
    for (int i : order) lst.push_back(i);                 // nodes are allocated in shuffled-value order…
    lst.sort();                                           // …sort() relinks them into value order WITHOUT moving them,
                                                          // so traversal now hops around memory like a long-lived list

    std::map<int, int> mp;                      for (int i = 0; i < N; ++i) mp[order[i]] = i;
    std::unordered_map<int, int> um;  um.reserve(N); for (int i = 0; i < N; ++i) um[order[i]] = i;

    long long s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    double t_vec = ms([&] { for (int x : vec) s1 += x; });
    double t_lst = ms([&] { for (int x : lst) s2 += x; });
    double t_map = ms([&] { for (auto& [k, v] : mp) s3 += k; });
    double t_um  = ms([&] { for (auto& [k, v] : um) s4 += k; });
    std::printf("full traversal, sum of %d ints  (checksums %lld %lld %lld %lld)\n", N, s1, s2, s3, s4);
    std::printf("  vector            %7.2f ms\n  list (scattered)  %7.2f ms   (%.1fx)\n  map               %7.2f ms   (%.1fx)\n  unordered_map     %7.2f ms   (%.1fx)\n",
                t_vec, t_lst, t_lst / t_vec, t_map, t_map / t_vec, t_um, t_um / t_vec);

    // random lookups
    std::vector<int> keys(1'000'000); for (auto& k : keys) k = int(rng() % N);
    std::vector<int> sorted = vec;                              // already sorted
    long long f1 = 0, f2 = 0, f3 = 0;
    double l_bin = ms([&] { for (int k : keys) f1 += std::binary_search(sorted.begin(), sorted.end(), k); });
    double l_map = ms([&] { for (int k : keys) f2 += mp.count(k); });
    double l_um  = ms([&] { for (int k : keys) f3 += um.count(k); });
    std::printf("1e6 random lookups in %d keys (hits %lld %lld %lld)\n", N, f1, f2, f3);
    std::printf("  sorted vector + binary_search %7.1f ms\n  map                           %7.1f ms\n  unordered_map                 %7.1f ms\n", l_bin, l_map, l_um);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
full traversal, sum of 2000000 ints  (checksums 1999999000000 1999999000000 1999999000000 1999999000000)
  vector               1.37 ms
  list (scattered)   305.59 ms   (223.6x)
  map                282.88 ms   (207.0x)
  unordered_map        8.73 ms   (6.4x)
1e6 random lookups in 2000000 keys (hits 1000000 1000000 1000000)
  sorted vector + binary_search   185.9 ms
  map                            1319.9 ms
  unordered_map                    40.9 ms
```

*How to read this.* The sequential traversal of a `vector` is **memory-bandwidth bound**: the prefetcher streams the cache lines. A scattered `list` pays a likely cache miss per element: two orders of magnitude slower in this run (over 200×), with identical Big-O. `unordered_map` is O(1) per lookup but one random cache miss per node (plus the bucket), and **a sorted `vector` with binary search is often competitive with, or faster than, `map` up to millions of keys** because its first probes stay in cache. These are indicative single runs on a shared machine; Chapter 40 explains how to benchmark rigorously. The ranking, not the exact figures, is the lesson.

> **Verdict.** Default to `vector`. Use a sorted `vector` (or C++23 `flat_map`) for read-mostly ordered lookup. Use `unordered_map` when you need many lookups and don't need ordering (and call `reserve`). Use `map` when you need *ordered iteration with frequent inserts and erases and stable references*. Use `list` almost never: only for splice-heavy algorithms and intrusive structures where you control allocation (Chapter 26). `deque` when you need stable references with push at both ends (a queue of large objects).

### Experiment 5: `std::array`, `std::span` and `mdspan`

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdio>
#include <numeric>
#include <span>
#include <vector>

double sum(std::span<const double> s) { return std::accumulate(s.begin(), s.end(), 0.0); }   // one function for all contiguous sources

int main() {
    std::array<double, 4> a = {1, 2, 3, 4};
    std::vector<double>   v = {10, 20, 30};
    double c_arr[2] = {100, 200};

    std::printf("sum(array)=%g sum(vector)=%g sum(C array)=%g sum(subspan)=%g\n",
                sum(a), sum(v), sum(c_arr), sum(std::span<const double>(v).subspan(1)));

    std::printf("sizeof(array<double,4>)=%zu  sizeof(span<double>)=%zu  sizeof(span<double,4>)=%zu (static extent: no size stored)\n",
                sizeof(a), sizeof(std::span<double>), sizeof(std::span<double, 4>));

    // static-extent conversion is checked at compile time
    std::span<double, 4> fixed{a};
    static_assert(decltype(fixed)::extent == 4);
    auto first2 = fixed.first<2>();                         // type span<double, 2>: no runtime check
    std::printf("first<2>: %g %g\n", first2[0], first2[1]);

    // a 2-D view over a flat buffer: what C++23 mdspan formalizes (not in GCC 14's libstdc++: see below)
    std::vector<int> grid(3 * 4);
    std::iota(grid.begin(), grid.end(), 0);
    auto at = [&](int r, int c) -> int& { return grid[std::size_t(r) * 4 + std::size_t(c)]; };
    std::printf("grid[2][1] = %d (row-major offset %d)\n", at(2, 1), 2 * 4 + 1);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum(array)=10 sum(vector)=60 sum(C array)=300 sum(subspan)=50
sizeof(array<double,4>)=32  sizeof(span<double>)=16  sizeof(span<double,4>)=8 (static extent: no size stored)
first<2>: 1 2
grid[2][1] = 9 (row-major offset 9)
```

`std::mdspan` formalizes the last lines (a non-owning multidimensional view with customizable *extents*, *layout* and *accessor*). It is standardized in C++23 but **libstdc++ ships it only from GCC 16**; libc++ from 18.

```cpp
// @test skip std::mdspan requires GCC 16 / libc++ 18; shown for the API shape
#include <mdspan>
#include <vector>

int main() {
    std::vector<int> buf(3 * 4);
    std::mdspan m(buf.data(), 3, 4);              // CTAD → mdspan<int, dextents<size_t,2>, layout_right>
    m[2, 1] = 9;                                  // C++23 multidimensional subscript operator
    // layout_left gives Fortran/column-major order; layout_stride arbitrary strides (and submdspan slices)
}
```

> **Compiler support (verified at writing, Oct 2026):** `std::mdspan` GCC 16, Clang/libc++ 18, MSVC 19.39. `std::flat_map` / `flat_set` GCC 15, libc++ 20, MSVC 19.34 (`<flat_map>`). `std::inplace_vector` is C++26 and ships in GCC 15+ / libc++ 20+. Re-check before relying on any: [cppreference compiler support](https://en.cppreference.com/w/cpp/compiler_support). The `@test skip` marker above is deliberate: GCC 14 cannot compile it.

---

## 8. Assembly / runtime investigation

Where does `push_back` spend its time? Compile the function with both a *fast path* and a *slow path*:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=append
#include <vector>
void append(std::vector<int>& v, int x) { v.push_back(x); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
append(std::vector<int, std::allocator<int> >&, int):
	push	r15
	push	r14
	push	r13
	push	r12
	mov	r12d, esi
	push	rbp
	push	rbx
	mov	rbx, rdi
	sub	rsp, 8
	mov	rax, QWORD PTR 8[rdi]
	cmp	rax, QWORD PTR 16[rdi]
	je	.L2
	mov	DWORD PTR [rax], esi
	add	rax, 4
	mov	QWORD PTR 8[rdi], rax
	add	rsp, 8
	pop	rbx
	pop	rbp
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	ret
.L2:
	movabs	rdx, 2305843009213693951
	mov	r15, QWORD PTR [rdi]
	sub	rax, r15
	mov	rbp, rax
	sar	rax, 2
	cmp	rax, rdx
	je	.L15
	test	rax, rax
	mov	edx, 1
	cmovne	rdx, rax
	add	rax, rdx
	jc	.L6
	movabs	rdx, 2305843009213693951
	cmp	rax, rdx
	cmova	rax, rdx
	lea	r13, 0[0+rax*4]
.L7:
	mov	rdi, r13
	call	operator new(unsigned long)@PLT
	mov	DWORD PTR [rax+rbp], r12d
	mov	r14, rax
	lea	r12, 4[rax+rbp]
	test	rbp, rbp
	jg	.L18
	test	r15, r15
	jne	.L19
.L10:
	mov	QWORD PTR [rbx], r14
	add	r14, r13
	mov	QWORD PTR 8[rbx], r12
	mov	QWORD PTR 16[rbx], r14
	add	rsp, 8
	pop	rbx
	pop	rbp
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	ret
.L18:
	mov	rsi, r15
	mov	rdx, rbp
	mov	rdi, rax
	call	memcpy@PLT
	mov	rsi, QWORD PTR 16[rbx]
... (truncated)
```

The **fast path** is five instructions: load `end`, compare with `end_of_storage`, store the value, bump `end`, write it back. Everything after `.L2` is the **growth path**: compute the new capacity (`max(size, 1) + size`, capped at `max_size()`), call `operator new`, store the new element, `memcpy` the old ones across, free the old block, and update the three pointers.

Two things are worth noticing, and neither is what the textbook says:

1. **GCC 14 inlined the growth path into `append`.** (It is the library's `_M_realloc_append`; before GCC 14 it was `_M_realloc_insert`, and it was a separate out-of-line call.) The result is a function that pushes **six callee-saved registers on entry even when the vector has room**, because the prologue is shared by both paths. For a tiny wrapper like this, that prologue costs more than the store. Whether a given build inlines the growth path depends on version and flags (`-O2` vs `-Os`, PGO); check with `-fno-inline-functions` or look at your own binary.
2. The growth path contains a **`memcpy` call**, not a loop of moves: `int` is trivially copyable, so the library took the `is_trivially_copyable` branch from Chapter 9. For `std::string` elements the same source would loop with a move per element (Chapter 6).

So the cost model is: ~1 ns on the fast path, a microsecond-scale reallocation at each doubling; and *in a hot loop it is the prologue and the reallocation check, not the store, that you are paying for*. This is the real argument for `reserve`, or for appending through a pointer/`span` after sizing, in inner loops.

```bash
valgrind --tool=massif ./a.out && ms_print massif.out.*      # heap profile over time: the sawtooth of vector growth
ltrace -e malloc+free ./a.out 2>&1 | head                    # allocation count without recompiling
perf stat -e cache-misses,cache-references,dTLB-load-misses ./a.out   # needs real PMU access (Chapter 41)
```

---

## 9. Implementation exercise

Implement **`FlatMap<K, V>`**, a sorted-`vector` associative container, and compare it to `std::map` and `std::unordered_map`:

1. storage: `std::vector<std::pair<K, V>>` (and then the **two-vector** layout: keys and values in separate arrays; why does it make `find` faster?)
2. `find`, `operator[]`, `insert`, `erase`, `contains`, `lower_bound` using `std::lower_bound`
3. `insert(first, last)` range overload that sorts once, merges, and de-duplicates (*O(n log n)*, not n× O(n))
4. a transparent comparator (`std::less<>`) so `find("literal")` doesn't build a `std::string`
5. benchmark for N = 10², 10³, 10⁴, 10⁵, 10⁶: build time, lookup time, iteration time, memory; find the crossover points
6. state which iterator/reference invalidation rules your container has, and which standard container's table matches

---

## 10. Real-world example

| System | Container choice and why |
|---|---|
| **Linux kernel** (C, not C++) | Intrusive lists and red-black trees: the node is *inside* the object. No allocation, no separate node. The C++ equivalent is Boost.Intrusive or hand-rolled (Chapter 26) |
| **LLVM** | `SmallVector<T, N>` (Project 3), `DenseMap` (open addressing), `StringMap`: LLVM avoids `std::map`/`std::unordered_map` almost entirely because they were measured slow |
| **Abseil / Folly** | `flat_hash_map` (SwissTable, open addressing, SIMD group probing): typically 2–3× faster than `std::unordered_map` and 2× less memory; the standard's *interface* (stable references) rules this out for `std::` |
| **Qt** | `QList` (Qt 6: contiguous like `vector`), `QMap` (a sorted tree), `QHash` (open-addressing-like in Qt 6): copy-on-write; Chapter 48 compares |
| **Games / HPC** | SoA arrays in `vector` (Chapter 27), `mdspan` for grids |

> **Opinion on `std::unordered_map`.** Its interface mandates node-based buckets, so it can never be as fast as modern open-addressing tables, and the standard cannot change that without breaking users who rely on stable references. If a profile shows hash-table lookup is hot, replace it with `absl::flat_hash_map`, `boost::unordered_flat_map` or `ankerl::unordered_dense`: not a micro-optimization, a structural one. Otherwise, keep it.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Holding a pointer/reference/iterator across `push_back`/`insert` on a `vector` | Heap-use-after-free; intermittent (only when capacity grows) | Use indices; `reserve` up front; ASan; `-D_GLIBCXX_DEBUG` |
| Erasing in a loop with a stale iterator | UB | `it = v.erase(it)`, or `std::erase_if(v, pred)` (C++20) |
| `vector<bool>` | Not a container of `bool` (bit-packed proxy); `auto& x = v[0]` fails | `vector<char>`, `std::bitset`, `std::vector<std::uint8_t>`, or a `deque<bool>` |
| `v.size() - 1` when empty | `size_t` wraps to 2⁶⁴−1 | Check emptiness; use `std::ssize` or signed indices intentionally |
| `std::vector<T> v(n)` then `push_back` | Appends *after* n default elements | `v.reserve(n)` for capacity, `v(n)` for size |
| Using `list` for "frequent inserts" | Slower than vector even with insertion in the middle for n ≲ 10⁴ | Measure; the O(n) `memmove` is faster than a cache miss per hop |
| Using `map` where order isn't needed | 3–10× slower than a hash table or a sorted vector | `unordered_map` or `flat_map` |
| `map[key]` on lookup | **Inserts** a default element when absent | `find`, `contains`, `at`, or `try_emplace` |
| Rehash invalidating iterators while inserting in a loop over the same `unordered_map` | UB | Collect first, insert after; or `reserve` |
| `unordered_map` with a poor hash (`x % 1024`) | Everything lands in a few buckets: O(n) lookup | Use a hash that spreads bits; `std::hash` for integers is *identity* in libstdc++ |
| `std::string` as a map key with `find("literal")` | Allocates a temporary `std::string` per lookup | `std::map<std::string, V, std::less<>>` for heterogeneous lookup; `string_view` keys carefully |
| Assuming the growth factor is 2 | Wrong on MSVC (1.5) | Do not depend on it; `reserve` |
| Moving a `std::string` or `small_vector` and keeping pointers into it | SSO/inline buffer *moves with the object* | Re-derive pointers after a move |
| `emplace_back` of an element built from a reference into the same vector | Reference may dangle during reallocation (the standard requires the library to handle `v.push_back(v[0])`; it does, with an extra move) | Fine for library calls; don't rely on it in your own container |

---

## 12. Exercises

1. **Count it.** Extend Experiment 1 to `std::set<std::string>` with 10⁵ short (≤15 char) and long (≥20 char) strings. Where do the extra allocations come from?
2. **Growth.** Write a `vector`-like class with a configurable growth factor. For factors 1.5, 2, and 4, measure total bytes allocated and number of `memcpy`s to reach 10⁷ elements. Why do some allocators prefer factors below the golden ratio (≈1.618)?
3. **Invalidation matrix.** For each of the 6 containers × 6 operations, predict "iterators / references valid?". Check by comparing addresses as in Experiment 3 (and ASan for the invalid ones).
4. **`deque` anatomy.** Using `std::deque<std::array<char, 100>>` and `std::deque<char>`, find the chunk size by observing address discontinuities. Derive the formula (`max(1, 512/sizeof(T))`) and confirm it for sizes 1, 100, 600.
5. **Shuffled `list`.** Reproduce Experiment 4 for N = 10³ … 10⁷ and plot (with `perf stat -e L1-dcache-load-misses,LLC-load-misses` if available) the traversal cost per element against N. Where is the cliff, and which cache level does it match?
6. **Hash quality.** For `unordered_map<int,int>`, insert keys `0, 1024, 2048, …`. Look at `bucket_count()` and `bucket_size(i)` distributions. Repeat with a custom hash that mixes bits (`x * 0x9E3779B97F4A7C15 >> 32`). What changes?
7. **Heterogeneous lookup.** Build a `std::map<std::string, int, std::less<>>` and `std::unordered_map<std::string, int, TransparentHash, std::equal_to<>>`. Count allocations for 10⁶ `find("literal")` calls versus the default.
8. **Node handles.** Use `map::extract` to move an element from one `std::map` to another and to rename its key *without* copying the value (`nh.key() = new_key`). Check that no allocation happens.
9. **`erase_if` cost.** Compare `v.erase(std::remove_if(...), v.end())`, `std::erase_if(v, pred)`, and rebuilding a new vector for deleting 50% of 10⁷ elements. Which is fastest, and what does `perf` say about the copies?

---

## 13. Challenge: an LRU cache with the right containers

Implement `LruCache<K, V>` with `get` and `put` in **O(1)** (average) and capacity eviction of the least recently used entry, in three ways:

1. `std::list<std::pair<K,V>>` + `std::unordered_map<K, list::iterator>` (the textbook; note which iterators stay valid and *why that design depends on `list`'s invalidation rules*)
2. an **index-linked** version: a `std::vector<Node>` with `prev`/`next` as 32-bit indices, a free list, and a hash map from `K` to index (the cache-friendly version; discuss what invalidation means now)
3. an **intrusive** version where the node *is* the hash-table entry

Benchmark all three with 10⁶ operations, Zipf-distributed keys, and capacities 10³, 10⁵, 10⁷. Report allocations (`operator new` counter), cache misses (`perf`), and throughput. Which design wins at which size, and why? What changes if values are 4 KB blobs?

---

## 14. Knowledge check

1. List the layout of `vector`, `deque`, `list`, `map` and `unordered_map` in one sentence each.
2. Why does `deque::push_back` invalidate iterators but not references? Why does `unordered_map` rehash do the same?
3. What does *amortized O(1)* promise about a single `push_back`?
4. When is `std::vector<int>::insert` in the middle faster than `std::list<int>::insert`, and why?
5. What is the per-element memory overhead of `list<int>`, `map<int,int>` and `vector<int>` on glibc/x86-64?
6. Why can `unordered_map` never be as fast as open addressing, *by interface*?
7. What does `reserve` change, and what does it *not* change?
8. Which `vector` operations never invalidate iterators/references to existing elements?
9. What does `std::erase_if` add over the erase-remove idiom?
10. Why is `vector<bool>` not a regular container?
11. What does `span<T, N>` (static extent) save over `span<T>`?
12. Name two containers whose layout the standard *does* specify and two where it doesn't.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `vector`: one contiguous heap block plus 3 pointers on the stack. `deque`: array of pointers to fixed-size chunks (512 B in libstdc++). `list`: doubly-linked, one heap node per element. `map`: red-black tree, one heap node per element. `unordered_map`: bucket array of pointers into singly-linked node chains, one heap node per element.
2. Iterators in a `deque` carry (chunk pointer, position) and a reference to the chunk *map*, which is reallocated when it needs to grow; the chunks and elements never move, so references survive. In `unordered_map`, a rehash re-links nodes into new buckets (iterators encode bucket traversal state), but the nodes (the elements) stay where they are.
3. Only that *n* operations cost O(n) in total. A single `push_back` can take O(n) (reallocation); it gives no latency bound.
4. Almost always, for contiguous small elements: the `memmove` of the tail is a sequential, prefetched copy (a few ns per KB); `list::insert` is O(1) once you have the iterator, but *finding* the position costs a cache miss per hop, and the allocation costs ~50 ns. The crossover is usually in the tens of thousands of elements, and often never.
5. `vector<int>`: 4 bytes (plus up to 2× slack from capacity). `list<int>`: 24-byte node → 32-byte malloc chunk = **32 bytes** (8×). `map<int,int>`: 40-byte node → 48-byte chunk = **48 bytes** per 8 bytes of payload (6×).
6. The standard requires bucket-level API (`bucket(k)`, `begin(n)`) and *reference stability across rehash*, which force separately-allocated nodes with chaining. Open addressing stores elements in the table itself and must move them on growth.
7. It changes **capacity** (guaranteeing no reallocation until size exceeds it, so handles stay valid until then). It does not change `size()`, and it never shrinks.
8. `reserve` when capacity is already enough, `push_back`/`emplace_back` within capacity (only `end()` is invalidated), `pop_back` (only the erased element and `end()`), and reading operations.
9. A single, harder-to-misuse call (`std::erase_if(c, pred)`) that works uniformly on every container (including `list` and `map`) and returns the number of erased elements.
10. Its `operator[]` returns a proxy (bit reference), `&v[0]` is not a `bool*`, and elements aren't addressable, so it fails the container requirements (`reference` is not `value_type&`). Prefer `vector<char>` or `bitset`.
11. The size is a compile-time constant: `sizeof(span<T, N>)` is one pointer, with no runtime length, and operations like `first<K>()` are checked at compile time.
12. Specified: `vector` (contiguous), `array` (aggregate of `T[N]`), and `string` (contiguous, null-terminated via `c_str`). Unspecified: `deque` (chunking), `map`/`set` (red-black tree), growth factor, SSO size.

</details>

---

[← Previous: Chapter 10](../part-04-generic-programming/10-concepts.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 12 — Views and non-owning types →](12-views-and-non-owning-types.md)
