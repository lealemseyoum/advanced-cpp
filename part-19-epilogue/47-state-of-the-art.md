# Chapter 47 — Epilogue: The State of the Art (C++20, C++23, C++26)

> **Part XIX · Epilogue** &nbsp;|&nbsp; **Level 2** (language-mechanism understanding, applied as engineering judgement) &nbsp;|&nbsp; **≈ 3 hours**
> **Prerequisites:** none strictly; it refers back to most of the course &nbsp;|&nbsp; **Standards:** C++20 ⚖️, C++23 ⚖️, C++26 (technically complete per the committee; see §3) &nbsp;|&nbsp; **Tools:** `g++` 13.3, `g++-14` 14.2, `clang++` 18.1.3 (all with libstdc++), the web for everything else

[← Previous: Capstone](../part-18-projects/capstone.md) &nbsp;|&nbsp; [Course map](../README.md)

---

**In one sentence:** the question is no longer "does the standard have it?" but **"does every compiler *and* every standard library I ship on have it, at the version I am allowed to require?"**. This chapter sorts C++20/23/26 into four tiers (safe everywhere, safe above a version floor, wait, and not yet) and shows you how to re-measure the answer on your own toolchain.

**By the end of this chapter you can:**

- name which C++20 and C++23 features are safe to use today without caring which compiler is behind them, and which are not
- explain why "the standard has it" and "my toolchain has it" are different questions, using real failures from this course
- read a feature-test macro table without being fooled by it (there are three traps, with evidence)
- say what C++26 actually contains, which parts you can use now, and which only exist in one compiler
- re-run the measurement scripts on your own compilers and get a support matrix for *your* constraints

> [!NOTE]
> **Honesty about the data.** Two kinds of evidence are used and they are labelled:
> 🧪 **measured**: I compiled the snippet on this machine (GCC 13.3, GCC 14.2, Clang 18.1.3, all with **libstdc++**; scripts in `code/ch47/`).
> 🌐 **sourced**: taken from cppreference's compiler-support tables, the GCC 16.1 announcement, the libc++ status pages and press coverage, read in October 2026. I could not test **MSVC**, **libc++**, **GCC 15/16** or **Clang 19+** here. Those tables change monthly; treat sourced cells as "what the documentation claims on the day I read it".

---

## 1. Problem

A language standard is a promise from the committee. A *toolchain* is a promise from a compiler team, a standard-library team and a platform vendor, and these are on different schedules:

```text
 ISO C++ standard      ←  one document, one date
        ↓
 front-end (GCC / Clang / MSVC / EDG)         ← language features
 standard library (libstdc++ / libc++ / MSVC STL)   ← library features
        ↓
 your distro / SDK / corporate image          ← the version you may actually require
```

Each arrow can drop a feature. Concretely, from this course:

| What I expected | What the toolchain did | Evidence |
|---|---|---|
| `std::expected` is C++23, Clang supports C++23 | Clang 18 + libstdc++ 13 **has no `std::expected`** (libstdc++ gates it on `__cpp_concepts >= 202002L`; Clang 18 reports `201907L`) | 🧪 matrix below, Chapter 23 |
| Symmetric transfer makes coroutine chains O(1) stack | Only a *real tail call* at `-O2` without sanitizers on GCC; Clang does it at every level | 🧪 Project 8 |
| Modules are in C++20 | Language support yes; `import std;` not available on GCC 14.2 or Clang 18 here | 🧪 `__cpp_lib_modules` absent |
| `__cpp_lib_execution` is defined, so senders/receivers exist | That macro is the **C++17 parallel-algorithms** macro; `std::execution` (P2300) is absent | 🧪 §4, trap 2 |

Engineering consequence: **"use C++23" is not a requirement; "use these C++23 features, on these three toolchains, at these minimum versions" is.**

---

## 2. Historical context

| Era | Gap between "standardised" and "usable everywhere" | Example |
|---|---|---|
| C++98 → C++03 | Years (`export template` was implemented by one front-end and then removed) | Cautionary tale: standardising ahead of implementation |
| C++11 | ~3–5 years for full library and language support | `<regex>` correct and fast on only some platforms for a long time |
| C++17 | Language fast; **library** slow (`<filesystem>`, parallel algorithms) | Parallel algorithms needed TBB on GCC |
| C++20 | Concepts and ranges fast; **modules, coroutine library support, chrono tz** slow | Chapter 38 |
| C++23 | Language fast (deducing `this` by GCC 14 / Clang 18); library staggered across three vendors | `mdspan`, `flat_map`, `generator` |
| C++26 | Committee finished in **March 2026** 🌐; compilers are implementing the headline features *now*, mostly GCC first | §6 |

The pattern is stable: **language features land within about two years; library features land vendor by vendor over three to five; the build-system and tooling story (modules) takes longest.** Plan for that, and the surprises stop being surprises.

---

## 3. Modern solution: how to think about "state of the art"

Use a four-tier model. A feature belongs to the **lowest** tier that any toolchain you must support puts it in.

| Tier | Meaning | Rule |
|:--:|---|---|
| **1 · Safe everywhere** | Implemented, stable and unsurprising on GCC, Clang and MSVC with their own standard libraries, at versions from the last ~3 years | Use freely |
| **2 · Safe above a floor** | Fine, but has a minimum compiler/library version, a known trap, or a missing implementation on one vendor | Use after checking the floor and writing it down in your build |
| **3 · Wait** | Standardised, but incomplete, experimental or single-vendor | Don't depend on it in shipped code; prototype behind an abstraction |
| **4 · Not yet** | In C++26 or later, available only in a development compiler or reference implementation | Learn it; don't ship it |

**The C++26 status line** 🌐: the committee reported the C++26 draft as complete in March 2026 (Herb Sutter's trip report, summarised by [InfoQ, April 2026](https://www.infoq.com/news/2026/04/cpp-26-reflection-safety-async/)). I did **not** verify the date of formal ISO publication; for contractual purposes, cite the ISO document, not this chapter.

> [!NOTE]
> **Layer check.**
> | Layer | Question for this chapter |
> |---|---|
> | C++ standard | What does C++20/23/26 specify? |
> | Compiler front-end | Which language features compile, at which version? |
> | Standard library | Which headers/types exist in libstdc++ vs libc++ vs MSVC STL? |
> | ABI / platform | Can I even *choose* the compiler version (distro, SDK, ABI of dependencies)? |
> | Build system | Does CMake/Ninja/your CI understand the feature (modules)? |

---

## 4. Mental model: what to trust, what to measure

### The three traps of feature-test macros

Feature-test macros (`__cpp_*`, `__cpp_lib_*`, `<version>`) are the *intended* way to ask "is this here?". They are good, and they lie in three specific ways. All three are demonstrated by 🧪 measurements in this chapter's data.

**Trap 1: absent macro ≠ absent feature.** Clang 18.1.3 compiles *deducing this* correctly, but does not define `__cpp_explicit_this_parameter` (it appears in `results/matrix.md` as `—`).
**Trap 2: the right-looking macro is the wrong standard's macro.** `__cpp_lib_execution` is `201902L` on all three compilers. That is **C++17's** parallel-algorithm execution policies. `std::execution` the senders/receivers framework (P2300, C++26) has no such value yet, and the snippet that uses `std::execution::just` fails to compile everywhere I tried.
**Trap 3: a defined macro does not mean the feature is *complete*.** `__cpp_lib_chrono` is `201611L` on GCC 13.3 but `201907L` on GCC 14.2 and Clang 18, and cppreference marks libc++'s calendar/time-zone support as still partial in libc++ 19 🌐. The value says which *paper revision* is in, not that every corner works.

**The reliable test is behavioural:** compile a tiny program that uses the feature the way you will. That is what the second script in `code/ch47/` does.

### The portability triangle

```text
          language feature
          (front-end)
              /\
             /  \
            /    \
   library  ------  build / tooling
   (std lib)         (CMake, modules, sanitizers)
```

Concepts, `consteval`, `<=>`, deducing `this` and `#embed` are *left-corner* features: a new compiler version is enough. `std::print`, `std::generator`, `mdspan` and `flat_map` are *right-corner*: you also need the library version. Modules need *all three corners*, and that is why they are still the last to be usable.

---

## 5. Language rules: nothing new to learn here

This chapter introduces no new language rules. The relevant rules are in the chapters it points to; follow the links in each table row.

---

## 6. Implementation model: the measured matrix 🧪

### How it was produced

`code/ch47/snippets.py` holds 48 small, complete translation units, one per feature, written the way you would really use the feature. `compile_probe.py` compiles each with `-fsyntax-only` against the newest standard that compiler accepts:

| Column | Command shape |
|---|---|
| GCC 13.3 | `g++ -std=c++23 -fsyntax-only` |
| GCC 14.2 | `g++-14 -std=c++26 -fsyntax-only` |
| Clang 18.1 | `clang++ -std=c++2c -fsyntax-only` (libstdc++ 13) |

`-fsyntax-only` proves the feature *parses and type-checks*; it does **not** prove it links (e.g. `std::stacktrace` and `<print>` may need extra libraries or runtime support) or behaves correctly.

### Result

| Feature | Std | GCC 13.3 | GCC 14.2 | Clang 18.1 |
|---|:--:|:--:|:--:|:--:|
| concepts + requires | C++20 | ✅ | ✅ | ✅ |
| ranges pipeline | C++20 | ✅ | ✅ | ✅ |
| std::format | C++20 | ✅ | ✅ | ✅ |
| std::span | C++20 | ✅ | ✅ | ✅ |
| <=> and defaulted == | C++20 | ✅ | ✅ | ✅ |
| designated initializers | C++20 | ✅ | ✅ | ✅ |
| consteval / constinit | C++20 | ✅ | ✅ | ✅ |
| coroutines (language + <coroutine>) | C++20 | ✅ | ✅ | ✅ |
| jthread + stop_token | C++20 | ✅ | ✅ | ✅ |
| latch / barrier / semaphore | C++20 | ✅ | ✅ | ✅ |
| atomic wait/notify | C++20 | ✅ | ✅ | ✅ |
| std::source_location | C++20 | ✅ | ✅ | ✅ |
| std::bit_cast | C++20 | ✅ | ✅ | ✅ |
| constexpr std::vector / std::string | C++20 | ✅ | ✅ | ✅ |
| calendar (year_month_day) | C++20 | ✅ | ✅ | ✅ |
| chrono time zones (tzdb) | C++20 | ✅ | ✅ | ✅ |
| std::osyncstream | C++20 | ✅ | ✅ | ✅ |
| deducing this | C++23 | ❌ | ✅ | ✅ |
| if consteval | C++23 | ✅ | ✅ | ✅ |
| std::expected (+ monadic) | C++23 | ✅ | ✅ | ❌ |
| std::print / println | C++23 | ❌ | ✅ | ✅ |
| std::generator | C++23 | ❌ | ✅ | ❌ |
| std::mdspan | C++23 | ❌ | ❌ | ❌ |
| std::flat_map | C++23 | ❌ | ❌ | ❌ |
| ranges::to | C++23 | ❌ | ✅ | ✅ |
| views::zip / enumerate / chunk | C++23 | ✅ | ✅ | ✅ |
| std::move_only_function | C++23 | ✅ | ✅ | ✅ |
| std::stacktrace (compiles) | C++23 | ✅ | ✅ | ✅ |
| std::unreachable / byteswap / to_underlying | C++23 | ✅ | ✅ | ✅ |
| std::start_lifetime_as | C++23 | ❌ | ❌ | ❌ |
| formatting ranges (format of vector) | C++23 | ❌ | ❌ | ❌ |
| static operator() / auto(x) | C++23 | ✅ | ✅ | ✅ |
| import std;  (needs prebuilt std module) | C++23 | ❌ | ❌ | ❌ |
| pack indexing  Ts...[0] | C++26 | ❌ | ❌ | ❌ |
| placeholder  _  variable | C++26 | ❌ | ✅ | ✅ |
| static_assert(cond, user message) | C++26 | ❌ | ✅ | ✅ |
| #embed | C++26 | ❌ | ❌ | ❌ |
| std::inplace_vector | C++26 | ❌ | ❌ | ❌ |
| std::function_ref | C++26 | ❌ | ❌ | ❌ |
| std::copyable_function | C++26 | ❌ | ❌ | ❌ |
| saturating arithmetic (add_sat) | C++26 | ❌ | ✅ | ✅ |
| contracts  pre / post / contract_assert | C++26 | ❌ | ❌ | ❌ |
| static reflection  ^^T  [: :] | C++26 | ❌ | ❌ | ❌ |
| template for (expansion statements) | C++26 | ❌ | ❌ | ❌ |
| std::execution  (senders/receivers) | C++26 | ❌ | ❌ | ❌ |
| std::hive | C++26 | ❌ | ❌ | ❌ |
| std::text_encoding | C++26 | ❌ | ✅ | ✅ |
| trivially relocatable  (std::is_trivially_relocatable) | C++26 | ❌ | ❌ | ❌ |

**Totals:** GCC 13.3: 24 of 48 compile · GCC 14.2: 32 of 48 · Clang 18.1: 30 of 48.

How to read it:

- **C++20 is a solid floor.** All 17 C++20 rows pass on all three compilers, including coroutines, `format`, ranges, `jthread`, calendar/time-zone and constexpr `vector`/`string`. This is the first standard where I would say "just require it".
- **The C++23 language features are in good shape; the C++23 library is the patchwork.** `if consteval`, `static operator()`, `auto(x)` pass everywhere; `deducing this` needs GCC 14; `std::print` needs GCC 14; `ranges::to` needs GCC 14.
- **Nothing in C++26 passes on all three.** The four that pass on both GCC 14 and Clang 18 are the small ones (`_` placeholders, `static_assert` with a message, saturating arithmetic, `text_encoding`).
- **Failures that are not about the compiler version:** `mdspan`, `flat_map`, `start_lifetime_as`, `formatting ranges (format of vector)` and `import std;` fail on all three of *these* libstdc++ 13/14 installations; they are library-version gaps (GCC 15/16 per cppreference 🌐).

The raw 75-row *macro* table (for comparison and the traps above) is in [`code/ch47/results/matrix.md`](code/ch47/results/matrix.md).

---

## 7. Experiments

All run from `part-19-epilogue/code/ch47/`.

### Experiment 1 — reproduce the behavioural matrix

```bash
cd part-19-epilogue/code/ch47
python3 compile_probe.py -v      # -v prints the first error line for every failure
cat results/compile_matrix.md
```

Expected: the table above (if your compilers match mine). **Edit `CONFIGS` in `compile_probe.py`** to add your own compilers: `("Clang 19", "clang++-19", ["-std=c++2c"])`, `("GCC 15", "g++-15", ["-std=c++26"])`, and for libc++ add `-stdlib=libc++`. This is the most valuable exercise in the chapter.

### Experiment 2 — the macro table, and why it needs the compile table

```bash
bash run.sh                      # builds probe.cpp three ways, writes results/matrix.md
grep -E "explicit_this|lib_execution|lib_chrono" results/matrix.md
```

Check the three traps against §4 yourself. Then ask: for each macro that is `—` in the macro table, does the *compile* table agree?

### Experiment 3 — a version-floor guard you can leave in the code

```cpp
// guard.hpp: fail early, with a message that tells the next person what to do
#include <version>

#if !defined(__cpp_lib_expected)
#  error "This project needs std::expected (GCC >= 12 with -std=c++23, Clang >= 16 with libc++, MSVC >= 19.33). See docs/toolchain.md"
#endif

#if !defined(__cpp_lib_format)
#  error "std::format needed (GCC >= 13, libc++ >= 17, MSVC >= 19.29)"
#endif
```

*Not auto-verified.* Remember trap 1: guard on the library macro, then confirm with a behavioural compile test in CI, because a missing macro is not always a missing feature.

---

## 8. Assembly / runtime investigation

Not the point of this chapter. The one runtime-behaviour finding that *does* affect the tiers is from Project 8: **coroutine symmetric transfer is a guaranteed tail call only when the optimiser makes it one**, so `Task<T>` chains are safe from stack overflow at `-O2` on GCC 13.3/14.2 and on Clang 18 at every level. Code that relies on it should assert the optimisation level in CI, not just the compiler version.

---

## 9. Implementation exercise: build your own support matrix

**Goal:** produce a one-page, version-pinned support statement for your own project.

1. List the toolchains you must support (compiler + standard library + minimum version). Include whatever your Linux distro, your CI image and your customers' platforms ship.
2. Add each to `CONFIGS` in `compile_probe.py` (add `-stdlib=libc++` for libc++).
3. Add rows to `snippets.py` for the features *your* codebase wants; keep each snippet the way you'd use it in production, not a minimal "hello".
4. Run it in CI on every toolchain upgrade. A red cell is a feature you may not use yet.

*Deliverable:* a `docs/toolchain.md` section called "Language features we allow", generated from the matrix, with the decision per row: **use / use with floor / wait**.

---

## 10. Real-world example: the tiers, with versions and evidence

Evidence key: 🧪 measured here (libstdc++ only) · 🌐 sourced from [cppreference compiler-support tables](https://en.cppreference.com/w/cpp/compiler_support) (C++20, C++23, C++26 pages), GCC/libc++ pages and press coverage.

### Tier 1: safe everywhere

Use these without checking which compiler is behind them. Each row passed on GCC 13.3, GCC 14.2 **and** Clang 18 🧪, and cppreference lists MSVC support 🌐 (check the footnote on exact versions).

| Feature | Why it's safe | Why you'd use it | See |
|---|---|---|---|
| **Concepts** (core and library) | Complete in GCC 10, Clang 10, MSVC 19.30 🌐 | Replace SFINAE; real diagnostics | [Ch 10](../part-04-generic-programming/10-concepts.md) |
| **`<=>` + defaulted `==`** | Everywhere | Less comparison boilerplate | [Ch 1](../part-01-mental-model/01-modern-cpp-philosophy.md) |
| **Designated initializers** | Everywhere | Readable aggregates | — |
| **`consteval`, `constinit`** | Everywhere | Compile-time guarantees | [Ch 16](../part-07-compile-time/16-constexpr.md) |
| **constexpr `vector`/`string`** | Everywhere 🧪 | Real compile-time algorithms | [Ch 16](../part-07-compile-time/16-constexpr.md) |
| **Ranges core + views** (`filter`, `transform`, `take`…) | GCC 10, libc++ 15, MSVC 19.29 🌐 | Lazy pipelines | [Ch 14](../part-06-ranges/14-ranges.md) |
| **`views::zip`, `enumerate`, `chunk`** | passed on all three here 🧪 | Common loops, no index bugs | [Ch 14](../part-06-ranges/14-ranges.md) |
| **`std::span`** | Everywhere | Non-owning array view (watch lifetimes) | [Ch 12](../part-05-standard-library/12-views-and-non-owning-types.md) |
| **`std::format`** | GCC 13, libc++ 17, MSVC 19.29 🌐 | Type-safe formatting | — |
| **`std::jthread`, `stop_token`** | passed on all three 🧪 | RAII threads, cancellation | [Ch 29](../part-12-concurrency/29-threading.md) |
| **`latch`, `barrier`, `counting_semaphore`, atomic `wait/notify`** | passed on all three 🧪 | Cheaper sync primitives | [Ch 29](../part-12-concurrency/29-threading.md), [Ch 31](../part-12-concurrency/31-atomics.md) |
| **`std::source_location`, `bit_cast`, `osyncstream`** | passed on all three 🧪 | Logging, type punning without UB | [Ch 28](../part-11-undefined-behavior/28-undefined-behavior.md) |
| **`if consteval`, `static operator()`, `auto(x)`** | passed on all three 🧪 | Small language wins | — |
| **`std::unreachable`, `byteswap`, `to_underlying`, `move_only_function`, `stacktrace` (compiles)** | passed on all three 🧪 | Utility; `stacktrace` may need a link-time library | — |
| **Coroutines (language + `<coroutine>`)** | Passed everywhere 🧪; GCC 10, MSVC 19.28; cppreference still marks Clang as partial at 17 🌐 | Async machinery | [Ch 33](../part-13-coroutines/33-coroutines.md) |

> [!WARNING]
> **Coroutines are Tier 1 only for the *language*.** The standard library gives you no `Task<T>`; `std::generator` is Tier 2 (below). You write `Task` yourself (Chapter 34/Project 8) or take a library. The *tail-call guarantee* for symmetric transfer is also optimiser-dependent (§8).

**Verdict:** if your minimum is GCC 13 / Clang 16 / MSVC 19.3x, **C++20 is a no-regrets baseline** (modules excluded; see Tier 3).

### Tier 2: safe above a version floor

| Feature | Floor (🌐 cppreference minimums) | The catch | See |
|---|---|---|---|
| **Deducing `this`** | GCC 14, Clang 18, MSVC 19.43 | Old compilers reject the syntax; MSVC was *partial* from 19.32 | — |
| **`std::expected`** | GCC 12, libc++ 16, MSVC 19.33 | 🧪 **Clang + an older libstdc++ is a dead end** (Clang 18 + libstdc++ 13: absent). Pin the std-lib, not just the compiler | [Ch 23](../part-09-error-handling/23-expected.md) |
| **`std::print`/`println`** | GCC 14, libc++ 17 (partial)/18, MSVC 19.37 | Needs the newest std-lib on GCC; 🧪 absent on GCC 13.3 | — |
| **`ranges::to`** | GCC 14 (partial)/15, libc++ 17, MSVC 19.34 | 🧪 absent on GCC 13.3 | [Ch 14](../part-06-ranges/14-ranges.md) |
| **Formatting ranges** (`format("{}", vec)`) | GCC 15, libc++ 16, MSVC 19.37 (partial) | 🧪 fails on libstdc++ 13/14 | — |
| **Chrono calendars + time zones** | GCC 14 complete; **libc++ partial even at 19**; MSVC 19.29 | Time-zone database dependency; behaviour differs by platform | — |
| **`std::generator`** | GCC 14, MSVC 19.43; **no libc++** | 🧪 absent on Clang 18 + libstdc++ 13 and on GCC 13 | [Ch 33](../part-13-coroutines/33-coroutines.md) |
| **`_` placeholders, `static_assert` message** | GCC 14, Clang 18 | MSVC not listed 🌐 | — |
| **Pack indexing `Ts...[0]`** | GCC 15, Clang 19; MSVC not listed 🌐 | 🧪 fails on GCC 14.2 and Clang 18 | [Ch 8](../part-04-generic-programming/08-templates-deep-dive.md) |
| **`#embed`** | GCC 15, Clang 19 (starred) 🌐 | 🧪 fails on GCC 14.2/Clang 18; MSVC not listed | — |
| **Saturating arithmetic**, **`text_encoding`** | GCC 14, libc++ 18 (saturation) | C++26 but small and portable on recent toolchains 🧪 | — |

**Verdict:** the "floor" column is the requirement you should *write into the build*, not folklore. For a typical Linux-and-Windows product shipping in 2026, **C++23 language features at GCC 14 / Clang 18 / MSVC 19.4x are fine; C++23 library features need per-feature checks.**

### Tier 3: wait

| Feature | State 🌐 | Why wait |
|---|---|---|
| **Modules (`import mymod;`)** | Partial on all three per cppreference. GCC's own docs call modules support "still experimental" in GCC 16.1 and add a `--compile-std-module` option to build the std module | Dependency scanning, build-system and tooling support remain uneven; see [Ch 38](../part-14-compilation-and-linking/38-modules.md) |
| **`import std;`** | libc++: "early development state, not considered stable nor complete"; needs Clang ≥ 18.1.2 and CMake ≥ 3.30 for installed modules. GCC: cppreference lists 15; experimental flag. 🧪 absent on GCC 14.2 and Clang 18 here | Real benefit (compile time), real cost (toolchain pinning). Try it in a leaf project |
| **`std::mdspan`** | GCC 16 (cppreference 🌐), libc++ 17/18, MSVC 19.39; 🧪 absent on libstdc++ 13/14 | GCC 16 is not what most Linux users have yet. Great feature, narrow window |
| **`std::flat_map`/`flat_set`** | GCC 15, libc++ 20, MSVC 19.51 🌐; 🧪 absent here | Floor too high today for a portable requirement |
| **`std::start_lifetime_as`** | GCC 16, MSVC 19.51; Clang not listed 🌐 | The right answer to implicit-lifetime-object punning ([Ch 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md)), but you can't depend on it yet |

### Tier 4: C++26 (learn it; don't ship it)

| Feature | Status 🌐 | Notes |
|---|---|---|
| **Static reflection** (`^^T`, `[: :]`, `<meta>`) | Adopted into C++26. GCC 16: P2996R13 with `-std=c++26 -freflection`. cppreference lists no Clang or MSVC support | The biggest change since templates. Single compiler, behind a flag. See [Ch 18](../part-07-compile-time/18-reflection-landscape.md) |
| **Contracts** (`pre`, `post`, `contract_assert`) | Adopted; GCC 16 (P2900R14). Clang/MSVC not listed. Four evaluation semantics: ignore, observe, enforce, quick-enforce | Controversial in the committee ([DevClass, April 2026](https://www.devclass.com/development/2026/04/01/contracts-are-in-c26-despite-disquiet-over-their-value/5213555)). Do not build a design around them yet |
| **Expansion statements** (`template for`) | GCC 16 | Pairs with reflection |
| **Erroneous behaviour for uninitialised reads** | GCC 16 | See "safety you can use today" below |
| **`std::execution`** (senders/receivers, P2300) | Adopted. libc++'s C++2c status page did not mark it complete in my read. 🧪 absent on all three installed here | Use the reference implementation (stdexec) if you need the model now |
| **`std::inplace_vector`, `function_ref`, `copyable_function`, `hive`, `linalg`** | GCC 16 mentions `inplace_vector`; libc++ status page did not mark these complete in my read; 🧪 absent on all three installed | You can write `inplace_vector` yourself in an afternoon ([Project 3](../part-18-projects/project-03-small-vector.md)); `function_ref` likewise ([Project 4](../part-18-projects/project-04-type-erased-function.md)) |
| **Trivial relocatability** | 🧪 absent; I could not confirm any compiler support | Don't plan around it |

> [!NOTE]
> I did not independently verify the Clang 19–21, GCC 15 or MSVC 19.4x numbers; they are cppreference's, via one fetch each. If one matters to you, add that compiler to `CONFIGS` and run Experiment 1.

### Safety you can have today (without waiting for C++26)

C++26's headline safety work (🌐 reported by [InfoQ](https://www.infoq.com/news/2026/04/cpp-26-reflection-safety-async/)) is "uninitialised reads are no longer UB by default" and a hardened standard library. You don't have to wait for the standard to get most of that behaviour:

| Goal | Today's switch | Cost / catch |
|---|---|---|
| Zero-initialise automatic variables | `-ftrivial-auto-var-init=zero` (GCC ≥ 12, Clang) | Small run-time cost; hides logic bugs from MSan-style tools |
| Bounds checks in the standard library | `-D_GLIBCXX_ASSERTIONS` (libstdc++); libc++ hardening modes (libc++ ≥ 18); MSVC checked iterators | Measure the cost on your hot paths (Chapter 39) |
| Find the bugs instead of hiding them | ASan, UBSan, TSan in CI ([Ch 28](../part-11-undefined-behavior/28-undefined-behavior.md)) | CI time |

**Opinion:** the hardening flags are the most valuable "modern C++" feature a mature codebase can adopt this quarter, and none of them needs a language upgrade.

---

## 11. Failure modes

| Mistake | Why it hurts |
|---|---|
| **Choosing a standard by compiler version alone** | libstdc++/libc++/MSVC STL versions decide library features (🧪 `expected` on Clang 18) |
| **Trusting `__cpp_lib_*` blindly** | Three traps in §4 |
| **Requiring `-std=c++26` on a distro compiler** | `c++2c` on Clang 18 is a different, earlier subset than on Clang 21; the flag names a moving target |
| **Using `import std;` in a library others must build** | You've made your consumers pin the toolchain and CMake |
| **Adopting contracts or reflection as architecture** | Single-compiler, evolving semantics |
| **Rewriting working code for novelty** | New feature ≠ better code; each should remove a real problem (Chapter 1) |
| **Treating a table as final** | Support cells change with every release; re-measure |

---

## 12. Exercises

1. **(L1)** Run Experiment 1. Which three rows differ between GCC 13.3 and GCC 14.2 for C++23 *library* features? Which for *language* features?
2. **(L1)** Pick five features from Tier 1. For each, state the **minimum** compiler and library version you'd be willing to put in a README.
3. **(L2)** Add `-stdlib=libc++` as a config (if installed). Which rows change? Does `std::expected` now compile on Clang 18?
4. **(L2)** Write a behavioural test for `std::stacktrace` that also *links and runs*, and report what else GCC needs on this machine.
5. **(L3)** Reproduce trap 1 for a different feature of your choice: find a feature your compiler implements whose macro it doesn't define.
6. **(L3)** Write `inplace_vector<T,N>` (Project 3 gives you the pieces) and keep it behind `#if !defined(__cpp_lib_inplace_vector)`, so it vanishes when the standard one arrives.
7. **(L4)** Generate a Markdown "allowed features" table from the JSON in `results/compile_probe.json` plus a hand-written "decision" column.

---

## 13. Challenge

**Support policy for a real library.** You maintain a library used by (a) Ubuntu LTS builds with the distro GCC, (b) Windows with the latest MSVC, (c) macOS with Apple Clang. Choose the minimum standard, and a list of banned/allowed features, such that you never need a `#ifdef` on compiler identity. Defend each row with evidence from your own matrix (add the compilers you have). State which C++26 feature you'd *most* like to adopt, and what conditions would have to be true first.

---

## 14. Knowledge check

<details>
<summary>Questions (try them first)</summary>

1. Why can Clang 18 compile code that uses `std::expected` on one machine and fail on another with the same Clang version?
2. A header `#if`s on `__cpp_lib_execution >= 201902L` to enable senders/receivers. What's wrong?
3. Name a C++23 feature that is a *language* feature and one that is a *library* feature. Which is likely to arrive first on a new compiler release, and why?
4. Why is "C++20, except modules" a defensible project baseline?
5. What do `-ftrivial-auto-var-init=zero` and C++26's erroneous behaviour have in common, and how do they differ?
6. When is a behavioural compile test better than a feature-test macro, and when is the macro better?

</details>

<details>
<summary>Answers</summary>

1. The standard library is a separate component. `std::expected` in libstdc++ is gated on a concepts-support macro that Clang 18 reports at an older value, so the *library version and its gating conditions* decide, not the compiler version alone.
2. That macro is the C++17 parallel-algorithms macro (`std::execution::par` etc.). It says nothing about P2300.
3. Language: deducing `this`, `if consteval`. Library: `std::print`, `std::expected`. Language features depend on one component (the front-end); library features also depend on the std-lib team's schedule and on the compiler front-end supporting what the library uses.
4. All of C++20 except modules passes on current GCC/Clang/MSVC; modules are the one feature whose support depends on the build system and tooling as well, and they are still partial/experimental in places.
5. Both remove "uninitialised read is exploitable UB" as a default. The flag is a compiler-specific implementation that *defines* the value (zero); the C++26 rule specifies erroneous behaviour (still a bug, but with bounded, diagnosable consequences, and implementations may still diagnose it).
6. Behavioural tests catch what macros don't (trap 1, partial implementations) but cost a compile; macros are free in the preprocessor and let you write `#if` fallbacks in headers. Use macros for selection and behavioural tests in CI to validate the macros.

</details>

---

## What to do next

You've reached the end of the book. The best use of what you've learned:

1. Pick one program you maintain. Run the sanitizers and the hardening flags (§10) in CI this week.
2. Add `code/ch47` to your CI with your real toolchains; commit the matrix.
3. Re-run it every time you upgrade a compiler or standard library, and let the table, not the press releases, decide what you adopt.

### Sources

- [InfoQ, "C++26: Reflection, Memory Safety, Contracts, and a New Async Model"](https://www.infoq.com/news/2026/04/cpp-26-reflection-safety-async/) (April 2026)
- [DevClass, "Contracts are in C++26 despite disquiet over their value"](https://www.devclass.com/development/2026/04/01/contracts-are-in-c26-despite-disquiet-over-their-value/5213555)
- [cppreference: C++20 compiler support](https://en.cppreference.com/w/cpp/compiler_support/20), [C++23](https://en.cppreference.com/w/cpp/compiler_support/23), [C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
- [GCC 16.1 release note on isocpp.org](https://isocpp.org/blog/2026/04/gcc-16.1)
- [libc++ C++2c status](https://libcxx.llvm.org/Status/Cxx2c.html) and [libc++ Modules](https://libcxx.llvm.org/Modules.html)
- 🧪 Own measurements: `part-19-epilogue/code/ch47/` (GCC 13.3, GCC 14.2, Clang 18.1.3, all libstdc++)
