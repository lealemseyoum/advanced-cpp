# Chapter 43 — Header and Library Architecture

> **Part XVI · Modern library design** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 7 hours**
> **Prerequisites:** [Chapter 36 (compilation model)](../part-14-compilation-and-linking/36-compilation-model.md), [Chapter 37 (ABI)](../part-14-compilation-and-linking/37-abi.md), [Chapter 38 (modules)](../part-14-compilation-and-linking/38-modules.md), [Chapter 42 (API design)](42-api-design.md) &nbsp;|&nbsp; **Standards:** incomplete types, ODR, `inline` variables ⚖️; `#pragma once` 🔧 &nbsp;|&nbsp; **Tools:** `g++-14`, `clang-tidy-18` (`misc-include-cleaner`), `g++ -M/-H`, Google Benchmark

[← Previous: Chapter 42](42-api-design.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 44 — Design patterns revisited →](44-design-patterns-revisited.md)

---

**In one sentence:** a header is not just a declaration file but a **dependency you impose on every includer**, so library architecture is the discipline of deciding what each header exposes, what it forces its users to compile, and which parts of the implementation are allowed to change without rebuilding or relinking anyone.

**By the end of this chapter you can:**

- separate a library's *public interface*, *private implementation* and *internal-but-shared* parts physically (directories, namespaces, visibility) as well as logically
- measure what a header costs its includers, and cut that cost with forward declarations, pImpl, explicit instantiation and header hygiene
- write headers that are self-contained, minimal and ODR-safe, and verify that with tools
- choose among header-only, compiled, and hybrid library shapes for a given project
- state the run-time price of pImpl with numbers, and know when it is not worth paying

---

## 1. Problem

Two facts about C++ builds, both from Chapter 36:

1. **`#include` is textual copying.** Every translation unit that includes a header re-parses it and everything *it* includes.
2. **A change to a header recompiles every file that includes it, transitively.** The build's dependency graph is the include graph.

Consequences you can measure (Experiments 1 and 2): a private `std::regex` member in a class makes **every includer of that class** pay for 91 000 lines of `<regex>`, and one such member made 20 trivial client files take **54 s instead of 9 s** to compile. The same mechanism makes a one-line edit in a "core" header trigger a rebuild of half the project.

The architectural problem is therefore not tidiness; it is **controlling the blast radius of change and of compile cost**:

| Concern | Question |
|---|---|
| **Compile-time dependencies** | What does including this header drag in? |
| **Change propagation** | If I edit this file, what must rebuild? |
| **Encapsulation** | What can users depend on, and what is private? |
| **Binary compatibility** | If this is a shared library, which edits break existing binaries? (Chapter 37) |
| **ODR safety** | Can two TUs see two different meanings of the same name? (Chapter 36) |
| **Usability** | Can a user include one header and have it work? |

---

## 2. Historical context

| Era | Practice |
|---|---|
| 1970s | Headers invented to share declarations; include guards (`#ifndef FOO_H`) appear to survive double inclusion |
| 1990s | Large C++ systems hit multi-hour builds; **John Lakos, *Large-Scale C++ Software Design* (1996)** defines physical design: levelisation, include-only-what-you-need, "insulation" (the origin of pImpl and protocol classes) |
| 1998 | *Cheshire Cat* / **pImpl** (Meyers, Sutter *Exceptional C++*) popularised; PCHs as a band-aid |
| 2000s | Template libraries (Boost, STL) push implementation into headers: *header-only* becomes a distribution strategy and a compile-time problem |
| 2005 | `extern template` (C++11) and explicit instantiation recover some control; `#pragma once` becomes ubiquitous (non-standard but universal) |
| 2010s | Unity builds, precompiled headers, include-what-you-use (IWYU), ccache, distributed builds as workarounds; Chromium and LLVM adopt strict layering and `-Wunused-include` style tooling |
| 2020 | **Modules** (C++20) offer a language-level fix (Chapter 38), adopted slowly |
| 2023–26 | clang-tidy `misc-include-cleaner`, `-ftime-trace` as standard tooling; header-only libraries remain popular for ease of distribution |

---

## 3. Modern solution

A modern layout and a set of rules:

```text
mylib/
├── include/mylib/            PUBLIC headers: the only ones installed and documented
│   ├── mylib.hpp               umbrella: includes the stable public pieces (optional, costs users compile time)
│   ├── config.hpp              small, dependency-free
│   ├── client.hpp              the main class (pImpl or minimal private members)
│   └── fwd.hpp                 forward declarations of public types
├── src/                      PRIVATE implementation
│   ├── client.cpp
│   ├── detail/                 internal headers shared among .cpp files, NOT installed
│   │   └── protocol.hpp
│   └── ...
├── tests/ benchmarks/ examples/
└── CMakeLists.txt            exposes ONE target: `mylib::mylib` (usage requirements: include/, C++ standard)
```

The rules, in priority order:

1. **Every public header is self-contained**: compiles on its own, includes exactly what it uses (Experiment 5).
2. **A header includes the minimum**: forward-declare (`class Foo;`) when only pointers/references/`unique_ptr` *parameters* are needed; include when you need size (members by value, base classes, inline function bodies).
3. **Private members that drag in heavy headers go behind pImpl** (or a protocol class) unless profiling says the indirection matters (Experiment 3).
4. **Names live in a namespace**; helpers live in `namespace mylib::detail` (convention: *not* part of the API, not documented, may change anytime).
5. **Visibility is explicit**: `-fvisibility=hidden` + an export macro (Chapter 36) for shared libraries; internal symbols never leak.
6. **Header-only is a choice with a price**: choose it for small, template-heavy or trivially distributable code, not by default.
7. **Measure**: `-ftime-trace`, `-H`, `clang-tidy misc-include-cleaner`, and the number of translation units that a header edit rebuilds.

---

## 4. Mental model

### Physical design: the include graph is the architecture

```text
   logical design ("Client uses Protocol uses Socket")        physical design (what #includes what)
   ──────────────────────────────────────────────────────    ──────────────────────────────────────
         Client                                                  client.hpp ──► protocol.hpp ──► <regex>, <map>, "socket.hpp"
           │                                                          ▲
        Protocol                                                       └── 40 files include client.hpp
           │                                                     edit protocol.hpp → rebuild 40 + files
        Socket
```

The logical design looks tidy; the physical design reveals that **any user of `Client` indirectly compiles `<regex>` and `socket.hpp`**. Architecture work is reshaping the physical graph so that stable, widely-included headers are small and depend on little ("depend towards stability").

### What a header must contain: three tiers

```text
   always OK in a header                 OK, with eyes open                      do not put in a header
   ─────────────────────────             ───────────────────────                ─────────────────────────
   declarations                          inline functions, small                non-inline function definitions
   type definitions                      templates (definition required)         non-inline variable definitions
   constexpr / inline variables          constants that need a type's size       `using namespace ...`
   forward declarations                  macros (prefixed!)                      anonymous namespaces / static
   concept definitions                                                           non-trivial static data (init order)
```

### Include only what you need: the three relationships

| Relationship | Needs a *complete* type? | Header may |
|---|---|---|
| `Foo*`, `Foo&`, function declaration `void f(Foo)`, `void f(Foo&)` | **No** (declaration of a function taking `Foo` by value is OK; the *definition* or call needs the complete type) | `class Foo;` (forward declaration) |
| `Foo` member by value, base class, inline function that uses `Foo`'s members | **Yes** | `#include "foo.hpp"` |
| `std::unique_ptr<Foo>` member | Complete type **where the destructor/constructors are instantiated**, not in the class definition | Forward declare + declare special members in the header, define them in the `.cpp` (Experiment 4) |
| `std::vector<Foo>` / `std::map<K,Foo>` member | Allowed to be incomplete in C++17+ only for `vector`, `list`, `forward_list`; **not** for others | Check the standard before relying on it |

### The cost model of a header

```text
   cost(header) = parse time of its text   +  instantiation of what it uses
                = (lines × tokens)         +  (templates, inline functions it forces into every TU)
   cost(project) = Σ over TUs of Σ over includes of cost(header)          ← the part modules attack
   rebuild(edit) = number of TUs that transitively include the edited file ← the part pImpl/forward declarations attack
```

### Three library shapes

| Shape | You ship | Good for | Costs |
|---|---|---|---|
| **Header-only** | `.hpp` files (maybe `inline`/templates) | Small utilities, template libraries (ranges-v3, fmt in header-only mode, Eigen), easy distribution | Every user recompiles it; no insulation; can bloat; ODR risks |
| **Compiled (static/shared)** | headers + `.a`/`.so` | Large implementations, stable ABI (shared), proprietary code | Build/link configuration; ABI issues (Chapter 37) |
| **Hybrid** | headers for templates and thin wrappers + compiled core | The practical default: `std` itself, Boost, Qt | Two places to look; need careful `extern template` and visibility |

---

## 5. Language rules

| Rule | Text | Consequence |
|---|---|---|
| **One Definition Rule** ⚖️ [basic.def.odr] | A class, inline function, template or constant may be defined in multiple TUs only if the definitions are token-for-token identical with identical meaning | Header content must not depend on macros or include order (Chapter 36) |
| **`inline` functions & variables** ⚖️ (C++17 for variables) | May be defined in every TU, with one address across the program | Constants and global objects in headers: `inline constexpr int kMax = 64;`, `inline Logger g_log;` |
| **`static` / anonymous namespace in a header** ⚖️ | Each TU gets its own copy (internal linkage) | Duplicated data, distinct addresses (`&kTable` differs per TU), code bloat; use `inline` instead |
| **Incomplete types** ⚖️ [basic.types] | A declared-but-undefined class may be used for pointers/references and in function declarations; `sizeof`, member access, `delete` of its pointer require completeness | The basis of forward declarations and pImpl |
| **`delete` of incomplete type** ⚖️ [expr.delete] | Undefined behaviour if the class has a non-trivial destructor (unless diagnosed) | A `std::unique_ptr<Impl>` with the destructor defined where `Impl` is incomplete is ill-formed (Experiment 4) |
| **Declaring things in `std`** ⚖️ [namespace.std] | Adding declarations (including forward declarations of `std::string`, `std::vector`) to `std` is **undefined behaviour** | Never write `namespace std { class string; }`; include `<iosfwd>`/`<string>` etc. The `<iosfwd>` header exists for exactly this reason |
| **Header guards vs `#pragma once`** | Guards: ⚖️ plain preprocessor. `#pragma once`: 🔧 universal but not standard (symlinks/copies can confuse file identity) | Either works in practice; guards are portable, pragma is simpler; do not mix names carelessly |
| **Templates & definitions** ⚖️ | The definition must be visible at the point of instantiation | Template definitions live in headers (or an `.inl`/`.tpp` included by it) unless explicit instantiation is used |
| **`extern template`** ⚖️ (C++11) | Suppresses implicit instantiation in this TU; one TU supplies the explicit instantiation definition | Cuts compile time and code duplication for common instantiations (Chapter 36) |
| **`export` on templates** ⚖️ (removed in C++11) | Separate compilation of templates was dropped | Don't expect it; modules are the C++20 route |
| **Module vs header** ⚖️ C++20 | A module interface replaces both | See Chapter 38 |

### Layer check

| Layer | What it controls |
|---|---|
| **C++ standard** | ODR, incomplete types, `inline`, what you may declare in `std` |
| **Compiler** | How long parsing takes; `-Winvalid-pch`, `-fvisibility`, `-H`, `-ftime-trace`, `-M` |
| **ABI** | Class layout and exported symbols: what pImpl and visibility protect (Chapter 37) |
| **Build system** | The dependency graph the compiler's `-M` output builds; incremental rebuild granularity |
| **OS / linker** | Shared-library symbol visibility, `-Wl,--no-undefined`, lazy binding; irrelevant for pure header-only |

---

## 6. Implementation model

### How the build discovers include dependencies

```bash
g++ -M foo.cpp           # Makefile fragment: foo.o: foo.cpp foo.hpp bar.hpp /usr/include/c++/14/... (all, transitively)
g++ -MM foo.cpp          # same, but omits system headers
g++ -H -fsyntax-only foo.cpp   # a tree of the includes, indented by depth, to stderr
```

Make and Ninja store these lists and rebuild an object when **any** file in its list changes. This is why a private member that pulls `<regex>` into a header is visible in the dependency list of every client (Experiment 2: 20/20 clients list `/regex`).

### What pImpl compiles to

```text
   class Widget { ...; std::unique_ptr<Impl> impl_; };            struct Widget::Impl { ...; std::regex re; };   // widget.cpp only
   ┌────────────┐
   │ Widget     │  sizeof = 8     ──────────►  ┌───────────────┐
   │  impl_ ────┼─────────────────────────────►│ Impl (heap)   │   one extra allocation; one extra indirection per call
   └────────────┘                              │  std::regex   │
        header: only <memory>                  └───────────────┘
```

Costs: **one heap allocation per object**; **one pointer dereference per member access**; **calls are non-inlinable across TUs** unless LTO sees through them (Experiment 3 shows LTO did not remove it for a heap-allocated `Impl`); **special members must be defined out of line** (Experiment 4). Benefits: header independence (compile time), ABI stability (Chapter 37), freedom to change private state without rebuilding clients.

### Explicit instantiation (reducing template cost)

```cpp
// widget.hpp
template <class T> class Buffer { /* ... lots of code ... */ };
extern template class Buffer<int>;            // "someone else instantiates this: don't do it here"
extern template class Buffer<std::string>;

// widget.cpp
template class Buffer<int>;                    // the one definition
template class Buffer<std::string>;
```

The compiler skips implicit instantiation of `Buffer<int>` in every TU but still inlines what it can see; the linker keeps one copy. It trades *some* inlining opportunity (functions not marked `inline` and instantiated elsewhere aren't visible for inlining unless defined in the header) for build time.

### What an umbrella header costs

An umbrella header (`#include <mylib/mylib.hpp>`) is convenient and makes **every user pay for the whole library**. The standard library avoids this with granular headers (`<vector>`, `<string>`); Boost and Qt offer both. Offer an umbrella *and* granular headers; use granular ones inside your own library.

---

## 7. Experiments

All code is in [`code/ch43/`](code/ch43/) (`header_cost.sh`, `pimpl/run.sh`, `pimpl_cost/run.sh`). **GCC 14.2, 2-vCPU VM.** The numbers show shape, not precision.

### Experiment 1 🔧: What does `#include <X>` cost?

Preprocessed lines (`g++ -E`) and parse time (`-fsyntax-only`, `-std=c++23`) for a translation unit containing nothing but the include:

| Header | Lines after preprocessing | Parse time |
|---|---:|---:|
| `<cstdint>` | 293 | 14 ms |
| `<cstdio>` | 934 | 18 ms |
| `<vector>` | 27 291 | 187 ms |
| `<map>` | 26 454 | 165 ms |
| `<unordered_map>` | 29 037 | 188 ms |
| `<string>` | 32 446 | 272 ms |
| `<algorithm>` | 31 469 | 211 ms |
| `<functional>` | 37 501 | 257 ms |
| `<ranges>` | 51 749 | 441 ms |
| `<format>` | 63 869 | 746 ms |
| `<print>` | 63 962 | 910 ms |
| `<iostream>` | 66 832 | 837 ms |
| `<sstream>` | 67 955 | 831 ms |
| `<thread>` | 75 466 | 1 002 ms |
| `<memory>` | 76 281 | 941 ms |
| `<filesystem>` | 77 806 | 1 059 ms |
| `<chrono>` | 86 590 | 1 139 ms |
| `<regex>` | 91 125 | 1 021 ms |

Three observations. (1) **Cost scales with size**, roughly 80-150 lines per millisecond here (small headers sit lower), so the table doubles as a rough estimate for anything unlisted. (2) **C++23 mode makes the old standard headers heavier**: `<memory>` (76 K lines) and `<chrono>` (87 K) are larger than `<regex>`'s *reputation*, because they pull in ranges, concepts, `<format>` and calendar machinery. (3) A project of 500 files that each include `<iostream>` pays about 0.84 s × 500 ≈ **7 minutes of CPU** just to read that one header (spread across cores, not eliminated). That is the cost modules (Chapter 38) and header hygiene attack.

> **Opinion.** **`<iostream>` in a header is a bug.** Including it to get `std::ostream&` in a declaration pulls 66 000 lines into every includer; use `<iosfwd>` (a few dozen lines) in the header and `<iostream>` in the `.cpp`. Likewise `<chrono>`, `<regex>`, `<filesystem>`, `<thread>`: reach for the forward header or push the use into the implementation.

### Experiment 2 ✅: A private member in a header taxes every includer

`widget_plain.hpp` has a private `std::regex` member; `widget_pimpl.hpp` hides it behind `unique_ptr<Impl>`, defined in `widget_pimpl.cpp`. Twenty client files each create a `Widget` and call `matches`. `-O1`, sequential:

```text
plain: 20 clients compile in 54.0 s      20 / 20 clients depend on <regex>
pimpl: 20 clients compile in  9.3 s       0 / 20 clients depend on <regex>
```

A **5.8× difference** in compile time, and a structural difference you can read from the dependency list: with the plain header every client's `-M` output lists `<regex>`, so editing *anything* `<regex>` includes (it won't happen, but your own heavy headers will) rebuilds all 20; with pImpl none does. (Each plain client also has to compile `std::regex`'s constructor, destructor and `regex_search` templates, which most likely explains why 2.7 s per file exceeds the ~1 s bare-include cost of Experiment 1; I did not break the time down with `-ftime-report`. Template-heavy private members plausibly tax you twice: parse and instantiate.)

### Experiment 3 ✅: What pImpl costs at run time

`pimpl_cost/`: a counter with an `add` and a `value`, either with inline members and the data in the class (`CounterPlain`) or behind pImpl with out-of-line members (`CounterPimpl`). `-O2`, with and without `-flto`:

```text
                            no LTO       -flto
BM_plain_add               0.36-0.41 ns   0.36 ns        inlined into an increment of a register/stack slot
BM_pimpl_add                2.26 ns       2.27 ns        an actual call + pointer chase
BM_plain_make               0.33 ns       0.35 ns        construction is free (trivial)
BM_pimpl_make              14.1 ns       15.1 ns         one malloc + one free
```

Per operation pImpl costs ~2 ns per call and ~14 ns per object (allocation). LTO **did not remove** the cost: the allocation of `Impl` and the accesses through the pointer survive. Read that against what you are buying: a call that does real work (a regex match, a socket write, a database lookup) makes 2 ns invisible; a call that does one addition makes it 6×.

> [!WARNING]
> The installed `libbenchmark` here prints `Library was built as DEBUG`. Its own loop overhead is a fraction of a nanosecond, and the *ratios* above (≈6×, ≈40×) are far larger than that, but treat the digits for sub-nanosecond results accordingly (Chapter 39).

> **Opinion.** **pImpl is for classes whose methods do real work, are called thousands rather than billions of times, or sit on an ABI/compile-time boundary.** Do not pImpl a `Vec3`, an iterator, a small value type, or anything created and destroyed in an inner loop. For those, keep data in the class and fix the compile-time dependencies another way (forward declarations, moving heavy members into a base/parent class, or accepting a heavier header).

### Experiment 4 ✅: pImpl's classic compile error

```cpp
// @test fail -std=c++20 err=incomplete
#include <memory>
class Widget {
public:
    Widget();                              // declared, defined elsewhere
    // ~Widget() is implicitly defined HERE (inline), where Impl is still incomplete
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int main() { Widget w; }
```

GCC: `error: invalid application of 'sizeof' to incomplete type 'Widget::Impl'`, raised from inside `std::default_delete`. The class's implicitly-generated destructor is inline and instantiated where `Impl` is incomplete. Fix: **declare the destructor, move operations (and any copy operations you want) in the class and `= default` them in the `.cpp`** where `Impl` is complete. This is the full list of members that must move out of line: destructor, move constructor, move assignment (and copy operations if you provide them, via a deep copy).

### Experiment 5 ✅: A header that isn't self-contained

```cpp
// @test fail -std=c++20 err=string
// h1.hpp as written by someone whose .cpp always included <string> and <vector> first
struct Config { std::string name; std::vector<int> values; };
```

Compiled alone: `'string' in namespace 'std' does not name a type`. In the author's own `.cpp` the includes happened to precede it; in a client that includes `h1.hpp` first, it breaks (and in a third client it works by accident). **The test is mechanical**: compile each public header as its own translation unit, as part of CI:

```bash
for h in include/mylib/*.hpp; do
  echo "#include \"$h\"" | g++ -std=c++20 -Iinclude -fsyntax-only -Wall -Wextra -x c++ - || echo "NOT SELF-CONTAINED: $h"
done
```

The inverse problem (including more than you use) is detected by `clang-tidy`'s `misc-include-cleaner`:

```text
$ clang-tidy-18 -checks='-*,misc-include-cleaner' ic.cpp -- -std=c++20
ic.cpp:3:1: warning: included header map is not used directly [misc-include-cleaner]
```

(It also reports *missing* direct includes, i.e. things you use via transitive includes. Both directions matter: unused includes slow builds; indirect includes break when someone cleans a header.)

---

## 8. Assembly / runtime investigation

```bash
# 1. What does this TU actually include? (a tree; the deepest, widest branches are the cost)
g++-14 -std=c++20 -H -fsyntax-only foo.cpp 2>&1 | grep -c '^\.'            # count of included files
g++-14 -std=c++20 -H -fsyntax-only foo.cpp 2>&1 | grep '^\.\. ' | head      # direct children only

# 2. What does it cost? (Clang: a per-file timeline; open in chrome://tracing or Perfetto)
clang++-18 -std=c++20 -ftime-trace -c foo.cpp && ls foo.json
g++-14 -std=c++20 -ftime-report -c foo.cpp 2>&1 | head -20                  # GCC's phase summary

# 3. Who depends on this header?  (the real blast radius of an edit)
grep -rl 'client.hpp' src include | wc -l                                    # direct includers
for f in $(ls src/*.cpp); do g++ -std=c++20 -M -Iinclude $f | grep -q client.hpp && echo $f; done | wc -l   # transitive

# 4. Public ABI surface of a shared library
nm -D --defined-only libmylib.so | c++filt | grep -v 'detail' | less         # exported symbols; anything unexpected leaked
objdump -T libmylib.so | wc -l                                                # count; compare before/after -fvisibility=hidden

# 5. Is the pImpl really out of line?  (look for Widget::~Widget in widget.o and NOT in client objects)
nm -C c_pimpl_1.o | grep Widget
nm -C widget_pimpl.o | grep -E 'Widget::(~Widget|Widget)'
```

---

## 9. Implementation exercise

**Refactor a heavy header.** Take the `Widget` of Experiment 2 (or your own class with a heavy private member) and:

1. Measure the compile time of 20 clients and the number of dependents on the heavy header.
2. Apply, in order, and re-measure after each: (a) forward-declare what the *header* needs only by reference; (b) move implementations of non-trivial inline methods to the `.cpp`; (c) move the heavy private member behind pImpl; (d) replace `<iostream>`/`<sstream>` in headers by `<iosfwd>`.
3. Add a CI script that compiles every header alone.
4. Add a rule that fails CI if a header in `include/` includes anything from `src/`.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
// widget.hpp: after
#pragma once
#include <iosfwd>          // std::ostream only by reference
#include <memory>
#include <string>

class Widget {
public:
    explicit Widget(std::string pattern);
    ~Widget();                                   // out of line: Impl is incomplete here
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;

    [[nodiscard]] bool matches(const std::string& s) const;
    friend std::ostream& operator<<(std::ostream&, const Widget&);   // declaration only; needs just <iosfwd>
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

```cpp
// widget.cpp
#include "widget.hpp"
#include <ostream>
#include <regex>
struct Widget::Impl { std::regex re; std::string source; };
Widget::Widget(std::string p) : impl_(new Impl{std::regex(p), p}) {}
Widget::~Widget() = default;
Widget::Widget(Widget&&) noexcept = default;
Widget& Widget::operator=(Widget&&) noexcept = default;
bool Widget::matches(const std::string& s) const { return std::regex_search(s, impl_->re); }
std::ostream& operator<<(std::ostream& os, const Widget& w) { return os << "Widget(" << w.impl_->source << ')'; }
```

For the "no `src/` from `include/`" check: `grep -rn '#include "\(\.\./\)*src/' include && exit 1`.

</details>

---

## 10. Real-world example

| Where | Architecture lesson |
|---|---|
| **The C++ standard library** | Granular headers (`<iosfwd>`, `<cstdint>`) exist to keep declarations cheap; `<bits/…>` internal headers; `std::__detail` namespaces. Even so, C++23 headers have grown heavy (Experiment 1) |
| **Boost** | Both extremes: header-only (`Boost.Hana`) and compiled (`Boost.Filesystem` historically); modular "Boost.X" layout; `detail` namespaces and `BOOST_*_HPP` guards |
| **{fmt}** | Header-only mode (`FMT_HEADER_ONLY`) **and** compiled mode (`libfmt.a`) from the same sources; compiled mode cuts user compile times substantially |
| **LLVM / Clang** | Rigorous layering (`include/llvm/Support/…` public, `lib/…` private), IWYU-style discipline, forward-declare-first style, no `<iostream>` (their own `raw_ostream`) |
| **Chromium** | `//base` dependency rules enforced by tooling; IWYU in CI; forward declarations preferred; component builds |
| **Qt** | The canonical pImpl: `QObject` has a `d_ptr` (`QObjectPrivate`), public headers expose only a pointer, `Q_DECLARE_PRIVATE`/`Q_D` macros wrap the accessor, and the private headers (`*_p.h`) are not installed. The reason: **binary compatibility across Qt 5.x/6.x minor releases**, i.e. a deliberate trade of ~one pointer chase per call for ABI freedom. Qt Creator's header-shape (`*_p.h`, `qt*global.h`, `QT_BEGIN_NAMESPACE`) is physical design as policy |
| **Eigen / ranges-v3** | Header-only because the types are templates over user types; the cost is borne by users' compile times |

> **Opinion.** **Default to a compiled core with small, dependency-light public headers, and add header-only pieces where templates force it.** "Header-only" is a distribution convenience, not an architecture; it moves the library's build cost into every user's build and removes the insulation. The exception is a *small*, *template-generic*, *stable* library, where the convenience and inlining beat the cost. And **measure the include cost of your own public headers the way Experiment 1 did**: it is the number your users feel.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Header not self-contained** | Compiles only if included after others (Experiment 5) | Compile each header alone in CI; include what you use |
| **Including more than needed** | Slow builds, large rebuild fan-out | Forward declarations; `<iosfwd>`; `misc-include-cleaner` |
| **Relying on transitive includes** | Breaks when a dependency stops including it | Include directly everything you use |
| **`using namespace` in a header** | Name collisions in every includer | Never; qualify names |
| **Non-inline function/variable definitions in a header** | `multiple definition of` at link time | `inline`, or move to a `.cpp` |
| **`static` or anonymous namespace in a header** | Per-TU copies; surprises with addresses and static state | `inline constexpr`/`inline` variables |
| **Macros without prefix/guards** | Collisions (`min`, `max`, `ERROR`) | `MYLIB_` prefix; prefer `constexpr`/functions |
| **Forward-declaring `std` types** | UB, breaks with other standard libraries | Include `<iosfwd>`/the real header |
| **Missing out-of-line special members with pImpl** | `invalid application of sizeof to incomplete type` (Experiment 4) | Declare and `= default` in the `.cpp` |
| **pImpl on a tiny hot type** | 6× slower calls, extra allocation (Experiment 3) | Don't; solve the compile-time problem differently |
| **Umbrella header used inside the library** | Everything rebuilds on every edit | Granular includes internally |
| **Circular includes** | Incomplete types in surprising places | Forward declare; split into lower-level header; levelise |
| **Public header includes private header** | Private details become public | Check with a script; physical layering |
| **Leaking symbols from a shared library** | ABI surface larger than intended | `-fvisibility=hidden` + export macros (Chapter 36); `nm -D` audit |
| **Templates with definitions in `.cpp`** | Link errors for new instantiations | Define in the header; or explicit instantiation for a closed set |
| **Header-only library with global state** | One definition per TU without `inline` | `inline` variables or function-local statics |
| **Include guards copy-pasted** | Two headers share a guard name; one silently disappears | `#pragma once` or generated guards |
| **Order-dependent macros between headers** | Behaviour changes with include order | Remove the macro dependency; configuration header included first, once |

---

## 12. Exercises

1. **Build the table.** Reproduce Experiment 1 for your standard library. Add three headers from your own project. Compute lines per ms.
2. **Blast radius.** For a project of your choice, compute, for each header, the number of TUs that transitively include it. Sort; the top five are your architecture problems.
3. **pImpl or not.** For five classes in a codebase, decide pImpl yes/no using three numbers: heaviest private include (Experiment 1), call frequency (profile), ABI exposure. Defend each decision.
4. **Forward-declaration audit.** Use `clang-tidy` `misc-include-cleaner` and your own scripts to turn 20 includes into forward declarations in the headers of a real project. Measure the build-time saving.
5. **Explicit instantiation.** Take a template-heavy class used with five types in 40 TUs. Add `extern template` and one explicit instantiation TU. Measure compile time and object size (`size *.o`).
6. **Header-only vs compiled.** Build the same small library (e.g. a JSON writer) both ways; measure client compile time for 20 TUs and run-time speed. At what size/usage does each win?
7. **Visibility audit.** Build a shared library with and without `-fvisibility=hidden`; count exported symbols (`nm -D`), and measure load time (`LD_DEBUG=statistics`).
8. **CI gate.** Write the CI script that (a) compiles every public header alone, (b) rejects `using namespace` in headers, (c) rejects includes of `src/` from `include/`, (d) checks no header includes `<iostream>`.

---

## 13. Challenge: untangle a include graph

Take (or synthesise, with a generator script) a codebase of 200 files whose include graph has a "hairball": a `common.hpp` included by everything, which includes `<regex>`, `<iostream>`, `<filesystem>` and a dozen project headers. Measure: full build time, incremental rebuild after touching `common.hpp`, after touching a leaf `.cpp`, and `-ftime-trace` totals for `Source` (parsing). Then restructure it **without changing behaviour**: split `common.hpp` by responsibility, forward-declare, pImpl the three heaviest classes, add `extern template` for the two hottest instantiations, and enforce the layering with a CI script. Report the before/after numbers for each step, and the run-time effect (benchmark) of each pImpl you introduced. Finally, estimate what modules (Chapter 38) would add on top.

---

## 14. Knowledge check

1. Why is the include graph the "real" architecture of a C++ project?
2. In which cases may a header use only a forward declaration?
3. Why does `std::unique_ptr<Impl>` need out-of-line special members, and which ones?
4. What does pImpl buy and what does it cost (numbers from Experiments 2 and 3)?
5. Why is forward-declaring `std::string` undefined behaviour, and what does the standard offer instead?
6. What happens to a `static const int` or anonymous-namespace object defined in a header?
7. What does `extern template` do, and what are its trade-offs?
8. How do you test that a header is self-contained?
9. When is header-only the right choice?
10. How does Qt use pImpl, and what is the trade-off it chose?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Because rebuild fan-out and compile cost are determined by which files include which; a clean logical design can have a tangled, expensive physical one.
2. When the header only declares pointers, references, or function signatures (not definitions that use the members), and doesn't use the type by value, as base, or in inline code needing its size.
3. The destructor (and moves, copies) are implicitly inline and instantiate `default_delete<Impl>`, requiring a complete `Impl`. Declare destructor and move operations (and copy if provided) in the class, and `= default` them in the `.cpp`.
4. Buys compile-time independence (54.0 s vs 9.3 s for 20 clients, 0/20 vs 20/20 dependents on `<regex>`) and ABI freedom. Costs ~2 ns per call and ~14 ns per object (allocation) here, uninlined even with LTO.
5. The standard forbids adding declarations to `std`; implementations may declare these types differently (templates with extra parameters, inline namespaces). Use `<iosfwd>` for streams, or include the real header.
6. Each translation unit gets its own copy with internal linkage: duplicated storage, different addresses, per-TU static state. Use `inline` variables.
7. It suppresses implicit instantiation in the TU, with one explicit instantiation TU supplying the code: less compile time and code duplication, but fewer inlining opportunities and a closed set of instantiations.
8. Compile each header as the only content of a translation unit (`echo '#include "x.hpp"' | g++ -fsyntax-only -x c++ -`) in CI.
9. Small, template-generic, stable libraries where distribution simplicity and cross-TU inlining matter more than the cost to users' builds.
10. `QObject` and most classes keep their data in a `d_ptr` to a private class (`*_p.h`), so public headers don't change when internals do; Qt accepts an extra allocation and indirection per object for binary compatibility across releases.

</details>

---

[← Previous: Chapter 42](42-api-design.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 44 — Design patterns revisited →](44-design-patterns-revisited.md)
