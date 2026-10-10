# Chapter 38 — C++20 Modules

> **Part XIV · Compilation and Linking** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 36 (compilation model)](36-compilation-model.md), [Chapter 37 (ABI)](37-abi.md) &nbsp;|&nbsp; **Standards:** named modules, partitions, header units, GMF/private fragment: C++20 ⚖️; `import std;` and `import std.compat;`: C++23 ⚖️ (🟡 **not available in GCC 14.2 or Clang 18**: needs GCC 15 / Clang 18+ with libc++'s module or MSVC) &nbsp;|&nbsp; **Tools:** `g++-14` (`-fmodules-ts`), `clang++-18` (`--precompile`), CMake 3.28 + Ninja 1.11, `clang-scan-deps`

[← Previous: Chapter 37](37-abi.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 39 — Benchmarking →](../part-15-performance/39-benchmarking.md)

---

**In one sentence:** a module is a translation unit that is compiled *once* into a binary description of its interface (a BMI), which importers load instead of re-parsing text, giving you real encapsulation, macro isolation, and (measurably) faster builds, in exchange for a build system that must now understand the *order* in which files compile.

**By the end of this chapter you can:**

- write a module interface, an implementation unit, a partition and a private fragment, and say what each is for
- explain the differences from headers: no macro leakage, no include-order dependence, reachability vs visibility, module linkage
- build a multi-module project with CMake + Ninja on both Clang 18 and GCC 14, and read the dependency graph
- **measure** the build-time effect and state honestly where modules stand today (what works, what is broken, what is missing)
- decide whether *your* project should adopt modules now

> **Status in one table (checked on this machine, October 2026).**
>
> | Feature | GCC 14.2 | Clang 18.1 | Standard |
> |---|---|---|---|
> | `export module m;`, `import m;`, partitions, impl. units | ✅ `-fmodules-ts` (`-std=c++20`) | ✅ `--precompile` | C++20 ⚖️ |
> | Header units (`import <vector>;`) | ✅ works; needs the header unit built | 🔧 supported but needs explicit setup | C++20 ⚖️ |
> | `import std;` | ❌ (GCC 15+) | ❌ (needs libc++ + a build step) | C++23 ⚖️ |
> | CMake 3.28 `FILE_SET CXX_MODULES` + Ninja | ✅ | ✅ | not standard |
> | Include order `import m;` then `#include <…>` | ⚠️ **fails** in our test | ✅ | n/a |
> | Incremental rebuild minimal | ⚠️ coarse (Experiment 5) | ✅ implementation edits only | n/a |

---

## 1. Problem

Chapter 36 listed the costs of the header model. Collected in one place:

| Problem with `#include` | Concrete consequence |
|---|---|
| **Textual paste, repeated per TU** | `<iostream>` is 69 000 lines, parsed in every TU (Chapter 36, Experiment 1): O(files × headers) build time |
| **Macros leak both ways** | A header's `#define min(a,b)` breaks `std::min` in your code; your `#define DEBUG` changes a header's meaning. Behaviour depends on *include order* |
| **No encapsulation** | Everything in a header is visible to every includer: private helpers, implementation detail types, `using namespace` directives |
| **ODR violations are unchecked** | Two TUs can disagree about a header's meaning (macro-dependent differences); the linker cannot tell (Chapter 36) |
| **Headers must be self-sufficient and order-independent**, and mostly aren't | “Include what you use” discipline; fragile transitive includes |
| **Separate declaration and definition** | Duplicated signatures, template definitions forced into headers |
| **Cannot express “this is the public API”** | Visibility and ABI discipline (Chapters 36–37) must be bolted on with macros and `detail` namespaces |

A macro leak, the simplest case. (The famous example is Windows' `min`/`max` macros breaking `std::min`; libstdc++ defends itself by `#undef`-ing them, so on Linux we use a blunter macro. Either way the point is the same: the preprocessor rewrites code in a header that never asked for it.)

```cpp
// @test fail -std=c++23 err=error
#define vector 42                       // an old C header's "helpful" macro
#include <vector>                       // ...rewrites `class vector` inside the standard library

int main() {}
```

The error is reported inside the standard library (the wording differs between GCC and Clang, but it is always inside `<vector>` or at the macro). With modules, macros do not cross the `import` boundary at all (Experiment 3).

---

## 2. Historical context

| Year | Event |
|---|---|
| 1970s–2000s | `#include` copy-paste; header guards; precompiled headers (`.pch`/`.gch`) as an optimisation, not a fix |
| 2012 | Clang implements **modules for C and Objective-C/C++ via module maps** (Apple, to speed up system frameworks) |
| 2014 | **Modules TS** begins (Gabriel Dos Reis, Microsoft); a competing Clang-style proposal |
| 2017–19 | Both converge in the merged **C++20 modules** design: named modules, `export`, `import`, partitions, header units |
| 2019 | MSVC first to ship a usable implementation (`/experimental:module`); C++20 finalised with modules |
| 2021 | GCC 11 ships `-fmodules-ts`; Clang 16 stabilises named modules |
| 2022 | **P1689** defines a JSON dependency-scan format so build systems can discover module dependencies; CMake adopts it |
| 2023 | **CMake 3.28** (Nov) declares C++ modules support for Ninja + Clang/GCC/MSVC; C++23 adds `import std;` (P2465) |
| 2024 | GCC 14 (May) is the first with a usable (still partial) modules implementation; libstdc++'s `std` module is not yet shipped |
| **2025–26** | GCC 15 ships `import std`; MSVC and Clang+libc++ have it; large codebases begin trials; the ecosystem (packages, IDEs, static analysers) is still catching up 🔧 |

---

## 3. Modern solution

```cpp
// greet.cppm -- the primary module interface unit
module;                                   // global module fragment: preprocessor-only area for legacy #includes
#include <string>
export module greet;                      // module declaration: this TU defines module `greet`

export namespace greet {                  // `export` makes a declaration visible to importers
    std::string hello(const std::string& who);
}

std::string helper(const std::string& s)  // not exported: unreachable by name from outside
{ return "[" + s + "]"; }

std::string greet::hello(const std::string& who) { return helper("hello, " + who); }
```

```cpp
// main.cpp
#include <iostream>        // legacy headers still work (textual); order matters on GCC 14 (§11)
import greet;              // load greet's BMI: no text is pasted, no macros arrive

int main() { std::cout << greet::hello("modules") << '\n'; }
```

The vocabulary, all of it:

| Construct | Meaning |
|---|---|
| `export module m;` | Starts the **primary module interface unit** of module `m` |
| `export` (on declarations, namespaces, `export { … }`, `export import`) | Marks what importers can use. Everything else is internal to the module |
| `import m;` | Make the exported declarations of `m` available; must come before use; does **not** import macros |
| `module m;` | Starts a **module implementation unit** of `m`: sees the whole interface and the module's non-exported entities; exports nothing |
| `module;` | Begins the **global module fragment**: only preprocessor directives (`#include`, `#define`) are allowed before `export module`; the place to include legacy headers |
| `module :private;` | **Private module fragment** (single-file module): everything after it is not part of the interface, so changing it need not rebuild importers |
| `export module m:part;` / `module m:part;` | **Partition** of `m`: an interface (`export module m:part;`) or implementation (`module m:part;`) piece, importable only within `m` (`import :part;`); re-exported with `export import :part;` |
| `import <header>;` | **Header unit**: a header compiled as a module. Macros it defines *do* flow to the importer (it behaves like a well-behaved `#include`) |
| `import std;` | C++23: the entire standard library as one module (🟡 not on our compilers) |

---

## 4. Mental model

### The two-step build

```text
  HEADERS:      a.cpp ──preprocess (paste all headers, again)──► compile ──► a.o
                b.cpp ──preprocess (paste all headers, AGAIN)──► compile ──► b.o        (cost ∝ #TUs × header size)

  MODULES:      greet.cppm ──compile once──► greet.pcm / greet.gcm   (BMI: binary module interface)  +  greet.o
                                                      │
                a.cpp ── `import greet;` ── LOAD the BMI (a serialised AST, no re-parse) ──► a.o
                b.cpp ── `import greet;` ── LOAD the BMI ──────────────────────────────────► b.o
```

A **BMI** (binary module interface; Clang `.pcm`, GCC `.gcm`) is the compiler's own serialisation of the module's exported (and *reachable*) declarations: types, function signatures, template definitions, constants. Importing it is a deserialisation, not a parse. It is **not** portable: it is tied to the compiler version, flags (`-std=`, `-D`, `-O` sometimes), and target, and is not an ABI-stable artefact. You never ship BMIs; you ship source, and rebuild them.

### The build graph now has *order*

With headers, all TUs compile independently and in parallel. With modules, `main.cpp` cannot compile until `greet`'s BMI exists:

```text
   shapes_geometry.cppm ──► shapes.cppm ──► main.cpp ──► (link)
        (partition)        (primary iface)      ▲
                                  └──────► shapes_impl.cpp
```

The build system must therefore **scan** each source *before* compiling (to learn `export module X` and `import Y`), derive the order, and schedule. That is the P1689 dependency-scanning protocol, and the reason Ninja (with `dyndep`) and CMake ≥ 3.28 are effectively required.

### Visibility vs reachability

An importer can *name* only what is exported (visibility), but may be able to *use* types that are not exported if they appear in an exported signature (reachability): if `export Circle f();` returns a non-exported type `Impl`, the caller can call `f()`, store the result with `auto`, and use its members, but cannot write the name `Impl`.

### Module linkage

A non-exported name has **module linkage**: visible across all units of the same module, invisible outside. It is stricter than internal linkage (usable across files of the module) and finer than external linkage (not part of the module's API). The module's name becomes part of the **mangled name** of everything attached to it (Experiment 7); this is the ODR defence: two modules can each define a `helper` without a clash.

---

## 5. Language rules

| Topic | Rule (⚖️ C++20/23) |
|---|---|
| **Module declaration** | `export module name;` appears at most once per TU, after an optional global module fragment (`module;` … preprocessor directives). A module has exactly one primary interface unit. Module names are dot-separated identifiers (`app.core`); the dots mean nothing to the language |
| **Export** | Only declarations marked `export` (directly, via an `export` namespace/block, or `export import`) are visible. You cannot export an entity with internal linkage; you cannot export a name that is not declared in the module interface |
| **Import** | `import name;` must appear before any declaration (in the module purview, directly after the module declaration; in an ordinary TU, at namespace scope, order matters). Imports are transitive only if re-exported (`export import`) |
| **Macros** | Macros are **never** exported from a named module and do not leak in. Header units are the exception: `import <x>;` makes that header's macros visible |
| **Global module fragment** | May only contain preprocessing directives; used to `#include` legacy headers. Declarations from it are attached to the *global module*, so they keep their usual mangled names (and may be merged with the same declarations imported elsewhere) |
| **Private module fragment** | `module :private;` ends the interface; later declarations are not reachable by importers; only allowed in single-file modules |
| **Partitions** | `export module m:p;` (interface partition) or `module m:p;` (implementation partition). Imported inside `m` as `import :p;`. Importers outside `m` cannot import a partition; the primary interface must `export import :p;` to expose it |
| **Implementation unit** | `module m;` (no `export`) implicitly imports the primary interface and sees non-exported declarations; for definitions you'd rather keep out of the interface |
| **Linkage** | Names declared in a module and not exported have *module linkage* (C++20): shared by all units of the module, invisible elsewhere. `static`/anonymous-namespace names keep internal linkage |
| **ODR across modules** | Different modules may each define a class with the same name and different content; they are different entities because the module name is part of the identity ([module.unit]/[basic.link]). Within one module the usual ODR applies |
| **Templates** | Template definitions are exported in the BMI (as AST); importers instantiate them. No need to put definitions in a header, and no ODR hazard from macro-dependent differences |
| **Cyclic imports** | Forbidden: the module dependency graph must be acyclic |
| **`import std;`** (C++23) | Makes all `std::` (and, via `std.compat`, global C library) names available. Does not provide macros (e.g. `assert`, `errno`); use `#include <cassert>` for those |
| **Language-neutral** | The standard says nothing about BMI format, file names, build commands, or how to find a module; those are implementation matters (🔧) |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is defined? | Module syntax, visibility, linkage, partitions, header units, `import std;` semantics. **Not** BMI format, discovery, or file extensions |
| **Compiler** | What does it produce? | GCC: `.gcm` in `gcm.cache/` (via `-fmodules-ts`, with `-fmodule-mapper`); Clang: `.pcm` (via `--precompile`, `-fmodule-file=name=path`); MSVC: `.ifc`. **BMIs are compiler- and flag-specific** |
| **ABI** | Does mangling change? | Yes: entities attached to a named module carry the module name: `_ZN5greetW5greet5helloE…` (Itanium extension: `W<name>` marks the module). Each module also gets an **initializer** symbol `_ZGIW5greet` |
| **Build system** | What is new? | Needs to scan sources for module deps (P1689), order compilation, and manage BMIs per compiler/flag set. CMake ≥ 3.28 + Ninja ≥ 1.11 do this |
| **OS / CPU** | Runtime effect? | None by itself (modules are a compile-time construct); the object code is the same as with headers. Can improve inlining and avoid ODR duplication only indirectly |

---

## 6. Implementation model

### What a BMI is

Clang's `module-file-info` shows the structure of the file produced by the `greet` module (Experiment 1):

```text
Information for module file 'greet.pcm':
  Module format: raw
  ====== C++20 Module structure ======
  Interface Unit 'greet' is the Primary Module at index #2
   Sub Modules:
    Global Module Fragment '<global>' is at index #1
  Generated by this Clang: (1ubuntu1)
  Module name: greet
  Language options: C99: No  C11: No  C17: No ...        <- compiled-under flags are recorded; mismatch = refusal to load
```

The file is 3.8 MB for a module whose entire source is a dozen lines: the global module fragment pulled in `<string>`, so the BMI contains the serialised AST of what `<string>` declared and the module needed. (This is why the BMI of the mini-`std` module in Experiment 4 is 7.1 MB.) Loading is lazy: only the declarations an importer actually uses are deserialised.

### The module object file and its initializer

A module compiled to code (`greet.o`) contains the definitions (`greet::hello`, `helper`) *and* an **initializer function** `_ZGIW5greet` that runs the module's namespace-scope dynamic initialisation and imports'. Every program using a module must link its object file: forget it and you get `undefined reference to 'initializer for module stdw'`.

### Dependency scanning (P1689)

The build tool runs a scanner (`clang-scan-deps` or `g++ -fdeps-format=p1689r5`) that preprocesses each TU just enough to report what it *provides* and *requires*:

```json
{ "rules": [ { "primary-output": "main.o", "requires": [ { "logical-name": "greet" } ] } ], "version": 1 }
```

CMake turns these into a Ninja **dynamic dependency** (`dyndep`) file, so the build order is discovered at build time rather than configure time (the `Scanning … for CXX dependencies` steps in Experiment 5).

### What the compiler does differently

- **Names are looked up in the BMI** instead of in pasted text; macro state of the importer is irrelevant to the imported code (so the same BMI works for every importer).
- **Templates are stored as AST**, not text; instantiation happens in the importing TU. Compile-time cost for heavy template libraries shifts from “re-parse” to “instantiate”.
- **Inlining** of exported inline/templated functions still works across importers (their bodies are in the BMI).
- **Duplicate-definition handling** (merging identical declarations coming via the GMF from different paths) is complex and a source of compiler bugs: it is the reason for GCC 14's include-order failure (§11).

---

## 7. Experiments

Everything here is reproducible from [`code/ch38/`](code/ch38/): `bash run.sh` (small modules), `bash bench.sh` (build-time), and the CMake project in `cmake_demo/`. Outputs are real runs on this machine (GCC 14.2, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1) and are **not auto-verified** by the single-file snippet checker (except the macro-leak snippet in §1).

### Experiment 1 🔧: One module, two compilers

The `greet` module from §3, compiled both ways:

```bash
# Clang: precompile to a BMI (.pcm), compile the BMI to an object, build the importer
clang++-18 -std=c++20 --precompile greet.cppm -o greet.pcm
clang++-18 -std=c++20 -c greet.pcm -o greet_c.o
clang++-18 -std=c++20 -fmodule-file=greet=greet.pcm main.cpp greet_c.o -o app_clang && ./app_clang

# GCC: -fmodules-ts; the BMI goes to gcm.cache/greet.gcm automatically; -x c++ because .cppm is not a known extension
g++-14 -std=c++20 -fmodules-ts -x c++ -c greet.cppm -o greet_g.o
g++-14 -std=c++20 -fmodules-ts main_inc_first.cpp greet_g.o -o app_gcc && ./app_gcc
```

```text
[hello, modules]            <- Clang, BMI greet.pcm = 3 798 492 bytes
[hello, modules]            <- GCC,   BMI gcm.cache/greet.gcm
```

Both give the same output. Note what the build needed that headers don't: an explicit *producer* step per module, a file naming convention (`-fmodule-file=greet=greet.pcm` for Clang's explicit-path model; GCC's cache directory and module mapper), and the module's own object file at link time.

### Experiment 2 ✅: Encapsulation is enforced

`bad.cpp` tries to use the non-exported `helper` from the module:

```cpp
import greet;
int main() { auto s = helper("x"); }
```

```text
Clang: bad.cpp:2:23: error: declaration of 'helper' must be imported from module 'greet' before it is required
GCC:   bad.cpp:2:23: error: 'helper' was not declared in this scope
```

With a header the equivalent mistake is impossible to prevent: if `helper` were declared in `greet.hpp`, every includer could call it (and many would, then depend on it). The module boundary is a compiler-enforced API line. (The Clang message is notable: the compiler *knows* the declaration exists in the module but is not visible, and says so.)

### Experiment 3 🔧: Macros do not cross a module boundary

```cpp
// mac.cppm
export module mac;
#define SECRET 42
export int get() { return SECRET; }

// mac_main.cpp
import mac;
#ifdef SECRET
#error "macro leaked"
#endif
int main() { return get() == 42 ? 0 : 1; }
```

```text
g++-14 -std=c++20 -fmodules-ts -x c++ -c mac.cppm -o mac.o && g++-14 -std=c++20 -fmodules-ts mac_main.cpp mac.o -o mac_app && ./mac_app
macro stayed inside; program ok
```

`SECRET` is defined and used inside the module, and the importer neither sees the macro nor can it affect the module's code. This is the end of “include order” bugs for module-to-module dependencies. (Header units are the deliberate exception; they exist to *migrate* legacy headers.)

### Experiment 4 🔧: Does it actually build faster?

20 translation units, each needing `<algorithm> <iostream> <map> <ranges> <string> <vector>`, compiled with Clang 18 at `-O0`. The “module” variant uses a **hand-made mini-`std` module** (`stdw.cppm`: includes those headers in the global module fragment and `export`s a handful of names with `using` declarations), which is what `import std;` does in miniature, since the real thing is not available here.

```bash
bash bench.sh
```

```text
headers: 20 TUs = 24 728 ms (best of 3)
one-time module build: 1 169 ms, stdw.pcm = 7 120 168 bytes
module : 20 TUs = 5 066 ms (best of 3)
                      →  about 4.9x faster for the 20 consumers; ~4x including the one-time build
```

Per TU: roughly 1.2 s for headers vs 0.25 s for the import. And the generated objects are the same size (125 136 vs 125 600 bytes), so **the machine code is unchanged**: modules are a *compile-time* feature. Caveats you should hold in mind:

- The test is deliberately header-heavy and template-light; real projects spend much of their time instantiating their *own* templates and optimising, which modules don't change. Reports from large projects range from ~10 % to several-fold improvement depending on how header-dominated the build was.
- It is a single machine, `-O0`, one compiler. Treat the **ratio** as indicative, not the digits.
- The one-time cost (here 1.2 s) is paid once per BMI per configuration, and again whenever the module's interface or its dependencies change.

### Experiment 5 🔧: A multi-file module project with CMake, on both compilers

[`cmake_demo/`](code/ch38/cmake_demo/) is a small library `shapes` with a **primary interface** (`shapes.cppm`), an **interface partition** (`shapes_geometry.cppm`, re-exported with `export import :geometry;`), an **implementation unit** (`shapes_impl.cpp`, `module shapes;`) and an executable that `import shapes;`.

```cmake
cmake_minimum_required(VERSION 3.28)
project(modules_demo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shapes)
target_sources(shapes
  PUBLIC  FILE_SET modules TYPE CXX_MODULES FILES shapes.cppm shapes_geometry.cppm
  PRIVATE shapes_impl.cpp)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE shapes)
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 \
      -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/usr/bin/clang-scan-deps-18        # or: -DCMAKE_CXX_COMPILER=g++-14
cmake --build build && build/app
```

```text
[1/12] Scanning .../shapes_geometry.cppm for CXX dependencies
[2/12] Scanning .../shapes.cppm for CXX dependencies
[3/12] Scanning .../main.cpp for CXX dependencies
[4/12] Scanning .../shapes_impl.cpp for CXX dependencies
[5/12] Generating CXX dyndep file CMakeFiles/shapes.dir/CXX.dd
[6/12] Generating CXX dyndep file CMakeFiles/app.dir/CXX.dd
[7/12] Building CXX object CMakeFiles/shapes.dir/shapes_geometry.cppm.o      <- partition first
[8/12] Building CXX object CMakeFiles/shapes.dir/shapes.cppm.o               <- then the primary interface
[9/12] Building CXX object CMakeFiles/app.dir/main.cpp.o
[10/12] Building CXX object CMakeFiles/shapes.dir/shapes_impl.cpp.o
[11/12] Linking CXX static library libshapes.a
[12/12] Linking CXX executable app
area 12.566, perimeter 12.566
contains (1,1): yes, contains (3,0): no
distance((0,0),(3,4)) = 5.0
```

The GCC build prints exactly the same steps and output. The six scan and dyndep steps are what is new compared to a header build: this is where the compile **order** comes from. (Note `main.cpp` calls `distance(Point{…}, Point{…})` unqualified: found by ADL, through the re-exported partition.)

**Incremental rebuilds.** Touch one file at a time and see what the build system recompiles (best case = only what depends on the change):

| Edit | Clang 18 rebuilds | GCC 14.2 rebuilds |
|---|---|---|
| `shapes_impl.cpp` (implementation unit, touch only) | `shapes_impl` only ✅ | **all four units** ⚠️ |
| `shapes.cppm` (a comment-only change to the interface) | interface, `main`, `shapes_impl` (partition untouched) | all four units |
| `main.cpp` | `main` only | `main` only |

Two lessons. First, with headers an edit to a header always rebuilds every includer; with modules, Clang's BMI for `shapes` is regenerated when its interface text changes, which rebuilds importers even for a *comment-only* edit: there is **no early cut-off** (rebuild only if the BMI's content hash changed), a known open improvement. Second, the quality of incremental behaviour **depends on the compiler/build-system pairing** and is clearly weaker on GCC 14.2 + CMake 3.28 today: GCC rebuilt everything after a touch of the implementation unit. If you adopt modules, measure *your* incremental build, not just the clean build.

### Experiment 6 🟡: `import std;`

```cpp
import std;
int main() { std::println("hi"); }
```

```text
g++-14 -std=c++23 -fmodules-ts istd.cpp -o istd
std: error: failed to read compiled module: No such file or directory          <- GCC 14: no std module shipped
clang++-18 -std=c++23 -fmodules-ts istd.cpp                                      <- Clang: flag unknown; std module needs libc++ and a manual build step
```

The standard (C++23) provides `import std;`, which would remove the 69 000-line `<iostream>` tax for everyone at once and is the single biggest practical benefit of modules, but it requires the implementation to ship (or the build system to build) a BMI of the entire standard library. As of these compilers it is **not available**; GCC 15, libc++ (Clang), and MSVC provide it. Until then, the honest comparison for a project using GCC 14 / Clang 18 + libstdc++ is “your own modules, plus legacy `#include`s for the standard library”, and the mini-`std` of Experiment 4 is the closest approximation.

### Experiment 7 🧩: What modules do to symbols

```bash
nm greet_c.o | grep -E ' [TtWw] ' | awk '{print $3}' | grep -v 'St\|Gnu\|__'    # (filtered: the module's own symbols)
```

```text
_ZGIW5greet                                                                       <- module initializer ("GI" + W + name)
_ZN5greetW5greet5helloERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE      <- greet::hello, attached to module greet ("W5greet")
_ZW5greet6helperERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE            <- helper(...), NOT exported, but module-attached
```

(GCC demangles the same names as `greet::hello@greet(...)` and `helper@greet(...)`.) Two things: the non-exported `helper` is still a real symbol in the object file, but attached to module `greet` in its mangling, so another module's `helper` cannot collide with it (ODR protection); and the module initializer is a symbol the linker needs, which is why the module's own object file must be linked.

---

## 8. Assembly / runtime investigation

```bash
# 1. Inspect a BMI
clang++-18 -std=c++20 -fmodule-file=greet=greet.pcm -module-file-info greet.pcm       # structure, language options, deps
strings greet.pcm | head                                                              # (it is a serialised AST, mostly opaque)
ls -l gcm.cache/ ; g++-14 -fmodules-ts -std=c++20 -fdump-lang-module -x c++ -c greet.cppm    # GCC: dump the module's CMI contents

# 2. What do the scanners report?
clang-scan-deps-18 -format=p1689 -- clang++-18 -std=c++20 -c main.cpp -fmodule-file=greet=greet.pcm
g++-14 -std=c++20 -fmodules-ts -fdeps-format=p1689r5 -fdeps-file=main.ddi -fdeps-target=main.o -M -MF main.d -c main.cpp

# 3. See the build order Ninja derived
ninja -C build -t graph | dot -Tsvg > graph.svg           # module BMIs appear as extra edges
ninja -C build -t commands app | head                      # the real compile commands, incl. -fmodule-file / -fmodule-mapper flags
cat build/CMakeFiles/app.dir/CXX.dd                        # the dyndep file: discovered deps

# 4. Symbols and mangling
nm -C greet.o | grep -E 'initializer|@greet'               # GCC demangling shows `name@module`
nm greet_c.o | grep -E '_ZGIW|_ZW|W[0-9]+'                 # raw: initializer and module-attached names

# 5. Build-time accounting
clang++-18 -ftime-trace -c a.cpp && ls a.json              # Chrome-tracing view: how much of the time is "Source" (parsing headers)
g++-14 -ftime-report -c a.cpp                              # GCC's per-phase report
ninja -C build -d stats

# 6. Correctness: mismatch between BMI and importer flags
clang++-18 -std=c++23 -fmodule-file=greet=greet.pcm -c main.cpp     # compiled with different -std than the BMI: "module file was compiled with different options"
```

---

## 9. Implementation exercise

Take a small header-based library of your own (about 500 lines: a few classes plus a template or two) and convert it step by step:

1. **Wrap it**: a module whose global module fragment `#include`s the old headers and re-exports a curated set of names. Measure the compile time of the consumers.
2. **Convert it properly**: move declarations into `export module` interface(s) and definitions into an implementation unit; keep internal helpers non-exported.
3. **Split into a partition** (e.g. `:detail`, `:io`), re-export what's public.
4. **Add a private module fragment** for a single-file variant, and show that editing code below `module :private;` does not rebuild importers (Clang: check which objects Ninja recompiles).
5. Build with CMake + Ninja on **both** compilers, record what breaks, and write down every workaround.

<details>
<summary><strong>Solution sketch: header wrapper, then a proper split (the smallest possible example)</strong></summary>

```cpp
// counter.cppm  --  a single-file module using the private module fragment
export module counter;

export class Counter {
public:
    void add(int n);
    int value() const;
private:
    int total_ = 0;                  // layout is part of the interface (importers allocate Counter)
};

module :private;                      // ---- everything below is NOT part of the interface ----

void Counter::add(int n) { total_ += n; }          // changing these bodies does not change the BMI's interface
int  Counter::value() const { return total_; }
```

```cpp
// main.cpp
import counter;
#include <cstdio>
int main() { Counter c; c.add(3); c.add(4); std::printf("%d\n", c.value()); }
```

```text
clang++-18 -std=c++20 --precompile counter.cppm -o counter.pcm
clang++-18 -std=c++20 -c counter.pcm -o counter.o
clang++-18 -std=c++20 -fmodule-file=counter=counter.pcm main.cpp counter.o -o app && ./app
7
```

The private fragment keeps definitions out of the interface the compiler writes into the BMI, so you get the **single-file convenience of a header-only library with the rebuild behaviour of a header/source pair**. (Note that `Counter`'s data members *are* in the interface: exactly as with headers, adding a member changes every importer's view of the class's layout; the pImpl idiom of Chapter 37 is still how you decouple layout.)

</details>

---

## 10. Real-world example

| Where | State of modules (October 2026) |
|---|---|
| **MSVC / Visual Studio** | Most mature: `import std;` shipped in VS 2022 17.5+; Microsoft use modules internally and publish guidance |
| **Clang + libc++** | `import std;` available with a build step; module maps for system headers on macOS; Apple's frameworks use Clang modules (the older, non-standard variant) |
| **GCC 14/15 + libstdc++** | Named modules work (this chapter); `import std` arrives in GCC 15; header units and mixed `#include`/`import` still have rough edges (§11) |
| **CMake** | 3.28+ first-class (`FILE_SET CXX_MODULES`, scanning, Ninja/VS generators); Makefile generators don't support it |
| **Other build systems** | Bazel (experimental), Meson (limited), build2 (early, excellent), `xmake` (works) |
| **Libraries** | Some libraries ship module interfaces alongside headers: `fmt` (`import fmt;`), `{fmt}` and parts of Boost under way; most projects still rely on `#include` for dependencies |
| **IDE tooling** | clangd and IntelliSense support is improving but a common source of friction (BMI ownership, flag matching) |
| **Static analysis, sanitiser, coverage tools** | Mostly work (they run after codegen), but several source-level tools lag |
| **Large codebases** | Adoption is mostly experimental or confined to new greenfield code; the big costs are build-system migration and the toolchain-version requirement |

> **Opinion.** **Modules are the right long-term design and not yet a default recommendation for a production codebase, with one exception.** If you are starting a *new* project, control your toolchain (CMake ≥ 3.28, Ninja, Clang ≥ 17 or a recent MSVC, GCC ≥ 15), and your code is mostly your own, adopt modules now: the encapsulation and macro isolation are real wins, and `import std;` is a free build-time gain. If you maintain a large existing codebase, **do not convert it**: the benefit is mostly build time, which you can get more cheaply from precompiled headers, include hygiene (forward declarations, pImpl, `-ftime-trace`), `ccache`, a faster linker, and unity builds. And the failure mode to avoid: **half-adopting**: some modules, some headers, wrapped legacy includes in every GMF, two compilers with different behaviours: that is where modules cost you more than they give. Pilot on a leaf library first, measure, and keep the headers building until you are sure.

---

## 11. Failure modes

| Mistake / limitation | Symptom | Fix |
|---|---|---|
| **GCC 14: `import m;` followed by `#include <standard header>`** where `m`'s global module fragment includes the same header | `error: redefinition of 'void* operator new(std::size_t, void*)'` …, in `<new>`, `<type_traits>` (reproduced on GCC 14.2) | `#include` the standard headers **before** the `import` lines (works, Experiment 1); or use header units/`import std` once available; or Clang |
| **Forgetting the module's object file at link** | `undefined reference to 'initializer for module X'` | Link the module's `.o`; in CMake, let the target do it |
| **BMI built with different flags than the importer** (`-std`, `-D`, `-O`, `-fno-exceptions`) | `module file … was compiled with different options` / silent miscompile on some compilers | Same flags for the whole target; let CMake manage BMIs |
| **Shipping or committing BMIs** | Breaks on any compiler upgrade; not portable | Never; build from source |
| **Cyclic imports / partition cycles** | Build system error “cycle” or scanning failure | Introduce a lower-level module; partitions can't be cyclic either |
| **Expecting macros to cross the boundary** | `X` undefined in an importer | Provide constants/`constexpr`/inline functions; header units only for legacy headers; or `#include` the macro header separately |
| **Using non-exported entities** | `declaration … must be imported` / `not declared` (Experiment 2) | `export` them deliberately, or keep them internal |
| **Putting `#include`s in the module purview** (after `export module`) | A huge mess: the header's declarations get attached to the module and mangle with its name (ODR/ABI fallout) | Includes belong in the **global module fragment** (`module;` … before `export module`) |
| **Makefile generators / build systems without scanning** | “Module not found”, wrong order | Ninja + CMake ≥ 3.28, or a build system that supports P1689 |
| **Mixing compilers or versions across targets** | BMI of one compiler unreadable by another | Rebuild per compiler; never share BMIs |
| **No early cut-off** | A comment-only interface edit rebuilds all importers (Experiment 5) | Keep interfaces small and stable; use implementation units and private fragments; pImpl |
| **GCC incremental over-rebuilding** (Experiment 5) | Everything rebuilds after touching one implementation unit | Track GCC releases; measure; prefer Clang for module-heavy trees for now |
| **IDE/clangd confusion** | False errors in the editor while the build is fine | Generate `compile_commands.json` with module flags; keep BMIs reachable |
| **Using `module` / `import` / `export` as identifiers** | They are contextual keywords; breakage in old code | Rename |
| **Expecting a speedup from `-O` heavy or template-heavy builds** | Builds are the same speed (Experiment 4's caveats) | Profile with `-ftime-trace`; modules only help the parsing part |
| **Assuming ABI changes** | None: object code is the same; but module-attached names are mangled differently (Experiment 7) | Don't mix the same entity as a module-attached and a global-module symbol in a binary interface |

---

## 12. Exercises

1. **Reproduce the benchmark.** Run `bench.sh`; vary the header set (add `<regex>`, `<thread>`); plot per-TU time vs number of includes for headers and the mini-module.
2. **Real `import std`.** If you have GCC 15, libc++-enabled Clang, or MSVC, build the std module and re-run the benchmark replacing `stdw` with `std`. Record the BMI size and per-TU time.
3. **Partition design.** Split `shapes` into `shapes:geometry`, `shapes:algorithms`, `shapes:io`; use `export import` selectively; check which partitions an importer can see; verify partitions can't be imported from outside.
4. **GMF experiments.** Make the GCC 14 include-order failure of §11 smaller: find the minimal module + importer that triggers it, and test if putting `import <new>;`/header units, or a `-fmodule-header` build, avoids it.
5. **Private fragment.** Demonstrate with Ninja's `-d explain` that editing code after `module :private;` does not rebuild importers on Clang, but editing the exported declarations does; compare GCC.
6. **ODR across modules.** Define `struct S { int a; }` in module `m1` and `struct S { long a, b; }` in module `m2`; import both in one TU (via qualified names) and link; show it is well-formed and the mangled names differ (`nm`). Repeat with headers and `-flto -Wodr` from Chapter 36.
7. **Header units.** Build `<iostream>` as a header unit with GCC (`-x c++-system-header iostream`), then `import <iostream>;` in a program; look at `gcm.cache/` and measure the compile time of a trivial TU against `#include <iostream>`.
8. **Convert a leaf.** Pick one leaf library in a real project, convert it to a module with CMake, and write a one-page report: lines changed, build-time effect (clean + incremental), tooling friction, and a recommendation.

---

## 13. Challenge: a module migration plan

You maintain a 300-file C++20 application (CMake, GCC and Clang in CI). Produce a written **migration plan** with data: (1) profile the current build (`-ftime-trace` aggregated over all TUs: what fraction is header parsing, template instantiation, codegen, link?); (2) estimate the best case from modules using the mini-`std` experiment extrapolated to your header mix; (3) compare against the cheaper alternatives (PCH, `ccache`, unity builds, IWYU, `mold`) with measured numbers for each; (4) define the pilot (which library, which compilers, success criteria, rollback), the toolchain requirements, and the CI changes; (5) list the risks from §11 that apply and the mitigations. The grading criterion is whether the plan would convince a skeptical tech lead *including the case for not migrating yet*.

---

## 14. Knowledge check

1. Name four problems of the header model that modules solve.
2. What is a BMI? Why should you never ship one?
3. What are the roles of the primary interface unit, an implementation unit, a partition and the private module fragment?
4. Where do legacy `#include`s go in a module file, and why not after `export module`?
5. Do macros defined in a module reach its importers? Do macros in a header unit reach its importers? Why the difference?
6. What is the difference between visibility and reachability?
7. What is module linkage, and how does it appear in a mangled name?
8. Why does CMake need a scanning step, and what does the Ninja `dyndep` file contain?
9. What did Experiment 4 measure and what *didn't* change? Why are the object files the same size?
10. What is `import std;` and why does it matter more for build speed than your own modules?
11. In which ways are GCC 14.2 and Clang 18 different for modules on this machine (list three)?
12. Would you convert a mature 10-year-old codebase to modules today? State the criteria that would change your answer.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Repeated re-parsing of text per TU (build time); macro leakage in both directions; include-order dependence; no encapsulation (everything in a header is visible); unchecked ODR violations; duplicated declarations/definitions.
2. A binary module interface: the compiler's serialised AST of the module's exported and reachable declarations (Clang `.pcm`, GCC `.gcm`). It encodes the compiler version and flags and is not portable or stable; always rebuild from source.
3. Primary interface: the single `export module m;` unit that declares what `m` exports. Implementation unit (`module m;`): definitions and private helpers, imports the interface automatically, exports nothing. Partition: a named piece of the module (interface or implementation) importable only within it; re-exported via `export import :p;`. Private fragment: the tail of a single-file interface whose declarations are not part of the interface.
4. In the global module fragment: `module;` followed by `#include`s, then `export module m;`. After the module declaration the declarations become attached to the module (module purview), changing their linkage and mangling and making duplicate-declaration merging with other TUs fail or break ODR.
5. No: macros defined in a named module are never exported. Header units do export their macros, since they exist to stand in for `#include` of existing headers.
6. Visibility: the name can be found by lookup in the importer. Reachability: the entity's definition can be used (members, types in signatures) even if its name isn't visible, e.g. a non-exported return type reached through an exported function.
7. Entities declared in a module and not exported are visible only to units of that module. Their mangled names include the module name (Itanium `W<len>name`, e.g. `_ZW5greet6helper…`), so same-named entities in different modules don't collide.
8. The build must know which files provide and require which modules before it can order compilation; the scanner extracts this (P1689 JSON) and CMake writes it into a Ninja dyndep file, which lists, for each object, the modules it needs/provides and the BMI files it outputs.
9. Compile time of 20 TUs with textual includes vs `import` of a mini `std` module (~4.9x faster per consumer). The generated machine code didn't change, because modules affect only how declarations reach the compiler; the same functions are emitted.
10. `import std;` imports the whole standard library as one pre-built module, sparing every TU from re-parsing tens of thousands of lines of `<iostream>`, `<vector>`, `<ranges>` etc.; since nearly every TU includes several std headers, the saving is global, while your own headers are usually much smaller.
11. (a) Include order: GCC fails with `import m;` then `#include <…>`, Clang does not. (b) Incremental rebuilds: GCC rebuilt all four units after touching an implementation unit; Clang rebuilt one. (c) Setup: Clang needs explicit `--precompile`/`-fmodule-file` and a scan-deps tool; GCC uses `-fmodules-ts` and a cache directory; GCC doesn't recognise `.cppm` as C++ without `-x c++`. (Also: header units work out of the box on GCC and need more setup on Clang.)
12. Probably not. Criteria that would change it: a build time dominated by header parsing (measured with `-ftime-trace`) that PCH/IWYU/ccache cannot fix; a toolchain you control (CMake ≥ 3.28, Ninja, recent Clang/MSVC/GCC 15) with `import std`; a codebase with severe macro/ODR problems that modules would enforce away; and leaf libraries where a pilot succeeded.

</details>

---

[← Previous: Chapter 37](37-abi.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 39 — Benchmarking →](../part-15-performance/39-benchmarking.md)
