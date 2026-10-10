# Chapter 36 — The C++ Compilation Model

> **Part XIV · Compilation and Linking** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 9 hours**
> **Prerequisites:** [Chapter 8 (templates)](../part-04-generic-programming/), [Chapter 19 (virtual functions)](../part-08-polymorphism/), [Chapter 28 (UB)](../part-11-undefined-behavior/28-undefined-behavior.md) &nbsp;|&nbsp; **Standards:** translation phases and the ODR are ⚖️ in every standard; `inline` variables C++17 ⚖️; **object files, symbols, shared libraries and the linker are not C++ at all** (ELF/Itanium ABI/GNU ld, 🧩 🔧) &nbsp;|&nbsp; **Tools:** `g++-14`, `nm`, `readelf`, `objdump`, `ar`, `ldd`

[← Previous: Chapter 35](../part-13-coroutines/35-coroutines-and-networking.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 37 — ABI →](37-abi.md)

---

**In one sentence:** C++ is compiled one **translation unit** at a time into an object file full of named symbols, a *linker* glues those files together by name, and the **One Definition Rule** is the contract that makes this safe, a contract that nothing checks for you.

**By the end of this chapter you can:**

- trace a `.cpp` file through preprocessing, compilation, assembly and linking, and inspect what each stage produces
- read `nm`/`readelf` output: defined, undefined, weak and local symbols, and COMDAT groups
- state the ODR precisely, and **reproduce a violation that silently changes program behaviour**
- explain `inline`, `static`, anonymous namespaces and `extern template` as *linkage* tools, not just optimisation hints
- choose between static and shared libraries and control symbol visibility, link order and static-initialisation order

> **Why this chapter exists.** Most "mysterious C++ bugs" at scale are not language bugs: they are *build-model* bugs: an ODR violation, a symbol resolved from the wrong library, an initialisation order that changed when someone reordered the link line. You cannot reason about them without a mental model of what the toolchain does.

---

## 1. Problem

C++ inherited C's separate-compilation model, designed in the 1970s to fit a compiler into 64 KB of memory: compile each file independently, resolve cross-file references later by name. Everything awkward about C++ builds follows from three facts:

1. **The compiler sees one file at a time.** It knows only what the headers it includes tell it. A declaration `int helper(int);` is a *promise* that some other file defines it.
2. **Headers are textually pasted.** `#include <iostream>` inserts tens of thousands of lines into *every* file that mentions it (Experiment 1), compiled again each time.
3. **The linker knows names and addresses, not types.** It matches the string `_Z6helperi` to a definition. If two files disagree about what `helper` or `struct S` *is*, nobody notices: the program is ill-formed, **no diagnostic required**, and usually "works" until it doesn't.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1970s | C separate compilation: `cc -c`, `ld`, `ar`; headers as copy-paste interfaces |
| 1983–85 | Stroustrup's cfront compiles C++ to C; the linker needs unique names, so **name mangling** is born (types are encoded into symbols to support overloading) |
| 1990s | Every vendor invents its own mangling and object layout; C++ objects from different compilers are mutually unusable |
| 1998 | C++98 standardises the ODR and `export template` (separately compiled templates; implemented by only one compiler and removed in C++11) |
| 1999–2001 | Itanium C++ ABI becomes the *de facto* standard on Linux/macOS (GCC 3.x, later Clang); Microsoft keeps its own |
| 2004 | LTO appears in GCC (4.5 stable, 2010); `-fvisibility` (GCC 4.0) lets libraries stop exporting everything |
| 2011–17 | `extern template` (C++11), inline variables (C++17) fix template/constant duplication; unity builds and precompiled headers fight compile times |
| 2020 | **C++20 modules**: a different translation model (Chapter 38) |
| 2024–26 | Compilers/build systems (CMake ≥ 3.28, Ninja, Clang 17+, GCC 14, MSVC) support modules enough for projects willing to adopt them 🔧 |

---

## 3. Modern solution

There is no new mechanism in the C++17/20 *toolchain*; the "modern" answer is a set of tools and disciplines for living with the model:

| Technique | What it does | Where |
|---|---|---|
| `inline` (functions *and* variables, C++17) | Allow identical definitions in many TUs: the linker keeps one | §5 |
| Anonymous namespaces / `static` | Give a name **internal linkage**: private to this TU | §5 |
| `extern template` + explicit instantiation | Instantiate a template once instead of in every TU | Experiment 7 |
| `-fvisibility=hidden` + export macros | Export only the public API of a shared library | Experiment 4 |
| LTO (`-flto`), `-Wodr` | Let the compiler/linker see across TUs; detect some ODR violations | §8 |
| Precompiled headers, unity builds, **modules** | Attack the cost of re-parsing headers | Chapter 38 |
| Sanitizers, `-Wl,--no-undefined`, `ld --trace-symbol` | Find link-time surprises | §8 |

---

## 4. Mental model

### The pipeline

```text
  hello.cpp ──► PREPROCESSOR ──► translation unit (TU) ──► COMPILER ──► assembly ──► ASSEMBLER ──► hello.o ──┐
   + headers      (#include,        one big text file       parse, type-check,     (.s)           object file  │
                   #define, #if)    (hello.ii)              optimise, codegen                     (ELF)        │
                                                                                                               ▼
  other.cpp ──► ... ──► other.o ───────────────────────────────────────────────────────────────►  LINKER  ──► executable
  libfoo.a  (archive of .o files)  ─────────────────────────────────────────────────────────►  (ld, lld,    or shared library
  libbar.so (shared library)       ─────────────────────────────────────────────────────────►   mold)       (.so)
                                                                                                               │
                                                                                         at run time:  DYNAMIC LOADER (ld.so) resolves .so symbols
```

| Stage | Input → output | What can go wrong here |
|---|---|---|
| Preprocess | source → TU text | macro collisions, include order dependence, huge TUs |
| Compile | TU → assembly/object | syntax and type errors, template errors, **UB** (Chapter 28) |
| Assemble | assembly → ELF `.o` | (rare) |
| Link | `.o`/`.a`/`.so` → executable | **undefined reference**, **multiple definition**, wrong link order, ODR violations (silent) |
| Load | executable → process | missing `.so`, symbol interposition, static-init order |

### A TU is the unit of compilation

The compiler never sees "the program". It sees **one translation unit**: a `.cpp` file after `#include` expansion. Anything declared but not defined in the TU becomes an **undefined symbol** in the object file, left for the linker. Anything defined becomes a **defined symbol**, with one of a few *binding* kinds:

| `nm` letter | Meaning | C++ source |
|---|---|---|
| `T` / `t` | defined in `.text`, global / local | an ordinary function / a `static` or anonymous-namespace function |
| `D` / `d` | defined in `.data`, global / local | `int global_counter = 5;` / `static int file_local = 7;` |
| `B` / `b` | `.bss` (zero-initialised) | `int zero;` |
| `R` / `r` | read-only data | `const` data, string literals |
| `U` | **undefined**: needed from elsewhere | a declaration that was *used* |
| `W` / `V` | **weak** definition (function / object) | `inline` functions, template instantiations, inline variables: many TUs may define them; the linker keeps one |

### The ODR in one sentence

> **Each non-inline function and variable has exactly one definition in the whole program; entities that may appear in many TUs (classes, templates, inline functions and variables) must have *token-for-token identical definitions* with the same meaning in each.**

"Exactly one definition" violations are caught by the linker (*multiple definition*). "Identical definitions" violations are **not detected at all** in the general case: that is where the dragons are (Experiment 3).

### Linkage

```text
   external linkage   the name denotes the same entity in all TUs        int f();   extern int x;   class members, templates ...
   internal linkage   the name is private to this TU                      static int x;   namespace { ... }   const int x = 1;  (namespace scope, C++)
   no linkage         local entities                                       automatic variables, local classes
   module linkage     visible within a named module only (C++20)          export module m;  (without `export`)  -- Chapter 38
```

---

## 5. Language rules

| Topic | Rule (⚖️ standard unless marked) |
|---|---|
| **Translation phases** | (1–4) character mapping, line splicing, tokenisation, **preprocessing** (directives, macro expansion, `#include` recursion); (5–6) literal processing; (7) **compilation**: syntax/semantic analysis, template instantiation; (8) **instantiation** of templates not yet instantiated; (9) **linking**: resolve external references. The standard describes *what* each does, not that they are separate programs |
| **Declaration vs definition** | A declaration introduces a name. A definition also provides the entity (body, storage, initialiser). `extern int x;` declares; `int x;` defines. A class definition is a *type definition*, not a symbol |
| **ODR** ([basic.def.odr]) | Exactly one definition of each used non-inline function/variable in the program. A class, enum, inline function, inline variable or template may be defined in several TUs **iff** each definition consists of the same token sequence, name lookup finds the same entities, and the same overload resolution results. Violation: UB, **no diagnostic required** |
| **`inline`** | Despite the name, today it means “**may be defined in more than one TU**” (identical definitions) and “a definition must be visible in every TU that odr-uses it”. Inlining as an optimisation is the compiler's own decision. Functions defined in a class body, `constexpr` functions, and (C++17) `constexpr` static data members are implicitly inline |
| **Inline variables** (C++17) | `inline int g = 0;` in a header: one object program-wide. Replaces the `extern` + one-definition dance and the `static`-in-header copy-per-TU trap |
| **Internal linkage** | `static` at namespace scope, anonymous namespaces, and (C++) `const`/`constexpr` namespace-scope variables without `extern`. Each TU gets its own entity: a `static` function in a header is **copied into every TU** and a `static int` in a header gives every TU **its own variable** |
| **`extern "C"`** | Disables name mangling and uses C linkage: one symbol per name, no overloading. The boundary to C and most plug-in ABIs (Chapter 45) |
| **Templates** | Definitions must be visible where instantiated (hence in headers). An implicit instantiation is emitted in **every TU that uses it** as weak/COMDAT code; the linker discards duplicates. `extern template class X<int>;` suppresses implicit instantiation of non-inline members in this TU, and `template class X<int>;` in one TU forces the instantiation |
| **Static initialisation order** | Within one TU, namespace-scope variables initialise in order of definition. **Across TUs the order is unspecified** (the “static initialisation order fiasco”). `constinit` (C++20) and constant initialisation avoid the problem; function-local statics are initialised on first use, thread-safely |
| **No diagnostic required (NDR)** | Many ODR violations, and e.g. a missing definition of an inline function, are NDR: the standard permits compilers to accept the program. Treat NDR as “silent miscompilation” |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is guaranteed? | Phases, ODR, linkage rules, initialisation-order rules; **nothing** about object files, `.so`, archives or symbols |
| **Compiler** | What does GCC/Clang do? | Emits ELF objects with sections per function (`-ffunction-sections`), COMDAT groups for inline/template entities, Itanium-mangled names; `-fvisibility` controls export |
| **ABI** (Itanium) | What names/layouts? | Mangling scheme, vtable and RTTI symbols, guard variables for function-local statics, COMDAT rules (Chapter 37) |
| **OS / loader** | What happens at start-up? | `ld.so` maps shared libraries, resolves symbols lazily or eagerly through GOT/PLT, runs `.init_array` constructors (static initialisers) in dependency order across libraries |
| **CPU** | Cost? | Calls into a shared library go through the PLT: one extra indirect jump; page faults when first touching a library's pages |

---

## 6. Implementation model

### What an object file contains

An ELF relocatable file is: a set of **sections** (`.text` code, `.data`/`.bss`/`.rodata` data, `.eh_frame`/`.gcc_except_table` for unwinding, `.symtab`/`.strtab` symbols, `.rela.*` relocations), a **symbol table** (name, section, value, binding, visibility), and **relocations** (“at this offset, patch in the address of symbol `helper` once you know it”). The compiler leaves holes where it called functions in other TUs; the linker fills them.

### COMDAT: how the linker keeps one copy of an inline function

For each inline function or template instantiation, GCC/Clang put the code in its **own section** (e.g. `.text._Z5twiceIiET_S0_`) belonging to a **COMDAT group** named by the symbol. When many objects contain the same group, the linker keeps the first and discards the rest. This is the mechanism behind “defined in many TUs, kept once”, and also behind the ODR hazard: **it keeps the first it sees, not the best one** (Experiment 3).

### Archives and the link-order rule

A static library `libfoo.a` is an `ar` archive of `.o` files plus an index. The linker processes inputs **left to right** and pulls a member out of an archive **only if it currently resolves an outstanding undefined symbol**. Therefore: objects that *use* symbols must come **before** the libraries that define them (Experiment 6). Circular dependencies between archives need `--start-group … --end-group` or a re-listed library.

### Shared libraries

A shared object is position-independent code (`-fPIC`) loaded at a runtime-chosen address. Calls to other shared objects go through the **PLT** (a small stub) and a **GOT** entry filled by the dynamic loader. By default every exported symbol is also *interposable*: a symbol defined earlier in the search order (e.g. in the executable or a `LD_PRELOAD`ed library) wins, which is why a "local" call inside a library to its own exported function may still be indirect (`-fno-semantic-interposition`, `-Bsymbolic`, or hidden visibility remove this).

### Cost of a header

Every `#include` is textually replaced, so its cost is paid **once per TU that includes it**. It is not free (Experiment 1): on this machine `<iostream>` alone is ~69 000 lines after preprocessing and takes ~1 second to parse, in each of the hundreds of TUs of a large project.

---

## 7. Experiments

All the multi-file experiments use sources in [`code/ch36/`](code/ch36/); `bash run.sh` in that directory reproduces every shell result below. Their outputs are pasted from real runs on this machine (GCC 14.2, binutils 2.42) and are **not auto-verified** by the snippet checker, which handles single files.

### Experiment 1 🔧: What does a header cost?

```bash
for h in cstdio iostream vector ranges format print; do
  echo "#include <$h>" > h.cpp
  printf '%-9s %6s lines   ' $h "$(g++-14 -std=c++23 -E h.cpp | wc -l)"
  /usr/bin/time -f '%e s (parse only)' g++-14 -std=c++23 -fsyntax-only h.cpp
done
```

```text
header      preprocessed lines     -fsyntax-only (best of 3)
<cstdio>            1 079                  21 ms
<vector>           27 941                 245 ms
<ranges>           53 330                 561 ms
<iostream>         68 977               1 023 ms
<format>           65 890               1 032 ms
<print>            66 000               1 050 ms
```

A one-line `main` that includes `<iostream>` becomes a **69 000-line** translation unit; the 255-line assembly in `hello.s` is the tiny tail. The numbers are noisy on a shared VM (compare the ratios, not the digits), but the lesson is stable: **modern libstdc++ headers are expensive**, `<ranges>`, `<format>` and `<print>` roughly as much as `<iostream>`, and every TU pays. This is the cost that precompiled headers, forward declarations, `pImpl`, and modules (Chapter 38) attack.

### Experiment 2 ✅: A name that mangles: types and functions

The compiler encodes the **types** of a function into its symbol so overloads are distinct symbols. A runnable look at the Itanium encoding, with the runtime's own demangler:

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <string>
#include <typeinfo>
#include <vector>

namespace geo { struct Point { int x, y; }; template <class T> struct Box { T v; }; }

template <class T> void show(const char* what) {
    const char* mangled = typeid(T).name();
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::printf("%-18s mangled: %-34s demangled: %s\n", what, mangled, status == 0 ? demangled : "?");
    std::free(demangled);
}

int main() {
    show<int>("int");
    show<geo::Point>("geo::Point");
    show<geo::Box<int>>("geo::Box<int>");
    show<std::vector<int>>("vector<int>");
    show<int(*)(double, char)>("int(*)(double,char)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
int                mangled: i                                  demangled: int
geo::Point         mangled: N3geo5PointE                       demangled: geo::Point
geo::Box<int>      mangled: N3geo3BoxIiEE                      demangled: geo::Box<int>
vector<int>        mangled: St6vectorIiSaIiEE                  demangled: std::vector<int, std::allocator<int> >
int(*)(double,char) mangled: PFidcE                             demangled: int (*)(double, char)
```

(`typeid` names are the mangling of the *type* alone, e.g. `N3geo5PointE` = nested-name, 3 chars `geo`, 5 chars `Point`; a function symbol is `_Z` + the mangled name + the parameter types.) Now the functions, from the real object file in Experiment 2b below: `_Z9public_fni` is `public_fn(int)`, `_Z11internal_fni` is `internal_fn(int)`. The linker sees only those strings. Rename `helper(int)` to `helper(long)` in one file and you do not get a type error: you get `undefined reference to helper(int)` at link time, because the symbol *name* changed.

### Experiment 2b 🧩: Anatomy of an object file

[`anatomy.cpp`](code/ch36/anatomy.cpp) defines some things, declares others and uses a template:

```cpp
int global_counter = 5;                  // defined: external linkage, data
static int file_local = 7;               // internal linkage
extern int defined_elsewhere;            // declaration only
int helper(int);                         // declaration only
template <class T> T twice(T x) { return x + x; }
inline int inl(int x) { return x + 1; }
int api(int x) { /* uses all of the above and std::vector<int> */ }
```

```bash
g++-14 -std=c++17 -O0 -c anatomy.cpp -o anatomy.o
nm -C anatomy.o | grep -vE "std::|__gnu|operator|_Vector|allocator"
readelf -g anatomy.o | grep -c 'COMDAT group'
```

```text
0000000000000000 T api(int)                  <- defined, global code
0000000000000000 D global_counter            <- defined, global data
0000000000000004 d file_local                <- lower case: LOCAL binding (static)
0000000000000000 W inl(int)                  <- W: weak. Inline function, kept once by the linker
0000000000000000 W int twice<int>(int)       <- W: template instantiation
                 U helper(int)               <- U: undefined, the linker must find it
                 U defined_elsewhere         <- U: ditto (data)
                 U printf
                 U memmove
COMDAT groups: 39
```

Everything you declared and used but did not define is a `U` in this object; everything you defined is `T`/`D`; things allowed to appear in many TUs are `W`. The filtered-out lines are the 39 COMDAT groups: **`std::vector<int>`'s member functions are instantiated into this object** (each in its own `.text._ZNSt6vectorIiSaIiEE…` section) because the TU used them. Every other TU that uses `vector<int>` carries its own copies; the linker discards all but one.

### Experiment 3 ✅: The ODR violation that changes behaviour

Two TUs define `inline int answer()` with different bodies: [`tu_a.cpp`](code/ch36/tu_a.cpp) returns 1 and [`tu_b.cpp`](code/ch36/tu_b.cpp) returns 2. Each prints what *it* thinks `answer()` is. The compiler of each file is happy: each TU is a perfectly valid program on its own.

```bash
g++-14 -O0 -c tu_a.cpp tu_b.cpp odr_main.cpp
g++-14 odr_main.o tu_a.o tu_b.o -o odr && ./odr
g++-14 odr_main.o tu_b.o tu_a.o -o odr && ./odr
g++-14 -O2 -c tu_a.cpp tu_b.cpp odr_main.cpp && g++-14 odr_main.o tu_a.o tu_b.o -o odr && ./odr
```

```text
link order: tu_a.o tu_b.o      -O0
tu_a: answer()=1
tu_b: answer()=1               <- tu_b "sees" tu_a's definition

link order: tu_b.o tu_a.o      -O0
tu_a: answer()=2               <- reorder the link line: the program's behaviour changes
tu_b: answer()=2

-O2 (any order)
tu_a: answer()=1
tu_b: answer()=2               <- at -O2 each TU inlined its own body, so it now "works"
```

**Same source, three different behaviours, no warning, no error.** At `-O0` the linker's COMDAT rule keeps the first `answer()` it sees and both TUs call it; with optimisation each call is inlined from the local body, so the bug *disappears*, and reappears the day someone builds a Debug configuration, enables LTO, or reorders a link line. This is the textbook shape of an ODR bug: unreproducible across build modes.

The compiler *can* sometimes catch the type version of the mistake. With LTO, `-Wodr` compares class definitions across TUs:

```bash
cat wodr_a.cpp   # struct S { int a; };            int use_a(S* s) { return s->a; }
cat wodr_b.cpp   # struct S { long a; long b; };   ...
g++-14 -O1 -flto -Wodr wodr_a.cpp wodr_b.cpp -o wodr
```

```text
wodr_a.cpp:1:8: warning: type 'struct S' violates the C++ One Definition Rule [-Wodr]
wodr_b.cpp:1:8: note: a different type is defined in another translation unit
wodr_a.cpp:1:16: note: the first difference of corresponding definitions is field 'a'
wodr_b.cpp:1:17: note: a field of same name but different type is defined in another translation unit
```

`-Wodr` finds mismatched **types** under LTO; it does **not** compare the *bodies* of inline functions (our `answer()` above went undetected). Practical defence: never define the same name differently, put shared definitions in **one header**, give TU-private helpers internal linkage (anonymous namespace), and build one CI configuration with `-flto -Wodr`.

### Experiment 4 🧩: Static vs shared libraries and symbol visibility

[`lib.cpp`](code/ch36/lib.cpp) has a public function and an internal helper; `lib.hpp` marks the public one with an `API` macro that expands to `__attribute__((visibility("default")))` when building the library.

```bash
g++-14 -O1 -c -fPIC -DBUILD_LIB lib.cpp -o lib.o
g++-14 -shared -o libfoo_all.so lib.o                                     # default: everything exported
g++-14 -shared -fvisibility=hidden -DBUILD_LIB -fPIC lib.cpp -o libfoo_hidden.so
nm -D --defined-only libfoo_all.so      | awk '{print $2,$3}'
nm -D --defined-only libfoo_hidden.so   | awk '{print $2,$3}'
```

```text
default visibility:             -fvisibility=hidden + API macro:
T _Z11internal_fni              T _Z9public_fni
T _Z9public_fni
```

By default **every** non-static function is part of the library's dynamic interface, including `internal_fn`, which is now an accidental ABI promise and a candidate for interposition. Hidden-by-default shrinks the exported surface to what you opted into. Linking an application against both flavours:

```bash
g++-14 -O1 app.cpp -L. -l:libfoo.a -o app_static && ./app_static        # prints 13
g++-14 -O1 app.cpp -L. -l:libfoo_hidden.so -Wl,-rpath,'$ORIGIN' -o app_shared && ./app_shared      # prints 13
readelf -d app_shared | grep -E "NEEDED|RUNPATH"
objdump -d --no-show-raw-insn app_shared | grep "call.*public_fn"
```

```text
 0x0000000000000001 (NEEDED)             Shared library: [libfoo_hidden.so]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN]
    1176:	call   1060 <_Z9public_fni@plt>        <- the call goes through the PLT stub
```

| | Static (`.a`) | Shared (`.so`) |
|---|---|---|
| Linked | at build time; only the needed members are copied in | at load time by `ld.so`; found via RUNPATH/`LD_LIBRARY_PATH`/cache |
| Updates | rebuild and relink everything | replace the `.so` (if ABI-compatible, Chapter 37) |
| Calls | direct | via PLT/GOT (one indirect jump) unless bound locally |
| Disk/memory | duplicated in every executable | shared between processes |
| Risks | ODR across archives, bigger binaries | **ABI breaks**, symbol interposition, missing library at run time, versioning |
| C++ specifics | template/inline copies fine | the same inline function may exist in the executable *and* the `.so`; function-local statics and singletons can be duplicated (two "singletons") unless visibility is right |

> **Opinion.** For an application: link your own code **statically** (one build graph, whole-program optimisation, fewer deployment failures) and the system C/C++ runtime dynamically. For a *plug-in or ABI boundary*: shared library with a **small, hidden-by-default, C-style API** (Chapter 45). Do not ship a C++ class hierarchy across a `.so` boundary you do not control.

### Experiment 5 🧩: Static-initialisation order across TUs

[`init_a.cpp`](code/ch36/init_a.cpp) defines `int A = make_a();` and [`init_b.cpp`](code/ch36/init_b.cpp) defines `int B = make_b();`, where `make_b` *reads* `A`.

```bash
g++-14 -c init_a.cpp init_b.cpp init_main.cpp
g++-14 init_main.o init_a.o init_b.o -o i1 && ./i1
g++-14 init_main.o init_b.o init_a.o -o i2 && ./i2
```

```text
link order a,b:                                         link order b,a:
  init_a: constructing A                                  init_b: constructing B, A is 0 at this moment
  init_b: constructing B, A is 1 at this moment           init_a: constructing A
main: A=1 B=2                                           main: A=1 B=1                <- B silently differs
```

The standard says the order across TUs is **unspecified**; the GNU linker happens to run initialisers in link order, so swapping two objects on the command line changes the program's output. Reading an uninitialised (zero-initialised) object is not even UB here, which makes it harder to notice. Fixes, in order of preference:

```cpp
inline constexpr int kLimit = 10;                    // constant initialisation: no dynamic init at all
constinit int counter = compute_at_compile_time();   // C++20: error if it is not constant-initialised
int& instance() { static int x = make(); return x; } // function-local static: initialised on first use, thread-safe (Meyers singleton)
```

### Experiment 6 🧩: Link order with static libraries

```bash
g++-14 -c ar_main.cpp ar_util.cpp && ar rcs libutil.a ar_util.o
g++-14 -L. -lutil ar_main.o -o a1          # library first
g++-14 ar_main.o -L. -lutil -o a2          # object first
```

```text
/usr/bin/ld: ar_main.o: in function `main':
ar_main.cpp:(.text+0x9): undefined reference to `util()'      <- library listed too early: nothing needed it yet
collect2: error: ld returned 1 exit status
object before lib: ok
```

The linker scans left to right and only extracts archive members that satisfy symbols it is *already looking for*. With `-lutil` first there were no outstanding references, so no member was pulled. (Shared libraries are *not* order-sensitive in the same way by default, but `--as-needed`, which is the default on many distributions, makes them order-sensitive too.)

### Experiment 7 🔧: Instantiate a template once: `extern template`

Five TUs each use `Box<std::string>` (a small class template, [`run.sh`](code/ch36/run.sh) E7). In the first build every TU implicitly instantiates its members; in the second the header says `extern template struct Box<std::string>;` and one TU (`et_inst.cpp`) has the explicit instantiation `template struct Box<std::string>;`.

```text
implicit instantiation : sum of .text in the 5 objects = 42605 bytes
extern template        : sum of .text in the 5 objects = 15065 bytes     (-65%, at -O0)
```

At `-O0` each user TU carries its own weak copies of `vector<string>` and `Box<string>` members (all later de-duplicated by the linker, which is why the *final* executable is the same size). `extern template` removes the duplicate **compile and assembly work** and shrinks the objects, which is the real win: it speeds up the build, not the program. Two caveats: members defined inside the class body are *implicitly inline*, and an explicit instantiation **declaration** does not stop the compiler from instantiating inline functions for inlining purposes (which is why some `Box<>` symbols remain in the user objects); and with optimisation on, the compiler inlines most of these tiny members anyway.

---

## 8. Assembly / runtime investigation

```bash
# 1. Every stage, saved
g++-14 -E hello.cpp -o hello.ii          # preprocess:  68 980 lines for an iostream hello world
g++-14 -S hello.cpp -o hello.s           # compile:     255 lines of assembly
g++-14 -c hello.cpp -o hello.o           # assemble:    ELF relocatable
g++-14 hello.o -o hello                  # link
g++-14 -v hello.cpp 2>&1 | grep -E "cc1plus|collect2|/as|/ld"      # the actual sub-programs the driver runs
g++-14 -save-temps -c hello.cpp          # keep .ii/.s/.o without asking

# 2. Symbols and sections
nm -C obj.o                  # defined (T D B R W) vs undefined (U);   nm -D lib.so for the dynamic table
nm -C --undefined-only obj.o # what this object needs from the rest of the world
readelf -S -W obj.o          # sections;  readelf -g obj.o : COMDAT groups;  readelf -r obj.o : relocations
objdump -dr -C obj.o         # disassembly *with relocations*: the holes the linker will fill
c++filt _ZNSt6vectorIiSaIiEE9push_backERKi      # demangle by hand

# 3. Who defines this symbol? Why did the linker pick that one?
g++-14 main.o a.o b.o -Wl,--trace-symbol=_Z6answerv       # lists every file that references/defines it
g++-14 main.o a.o b.o -Wl,-Map=out.map                    # full link map: which input contributed which section
g++-14 -Wl,--cref ...                                      # cross-reference table of symbols and defining files

# 4. Shrinking: remove unused functions
g++-14 -ffunction-sections -fdata-sections -c x.cpp && g++-14 x.o -Wl,--gc-sections -o x

# 5. Whole-program view
g++-14 -O2 -flto -Wodr -fno-fat-lto-objects *.cpp -o app   # LTO: inline across TUs, detect type ODR violations

# 6. Run-time linking
ldd ./app                    # which .so files are found and where
LD_DEBUG=libs ./app 2>&1 | head -30          # library search/loading trace
LD_DEBUG=bindings ./app 2>&1 | grep public_fn # which definition each symbol was bound to
readelf -d ./app | grep -E "NEEDED|RPATH|RUNPATH|SONAME"
```

---

## 9. Implementation exercise

**Build a tiny linker-eyes tool.** Write a C++ program that reads an ELF64 relocatable file (use `<elf.h>` structures, no libraries) and prints: (1) section names and sizes, (2) the symbol table with demangled names, binding (local/global/weak) and whether each symbol is defined or undefined, (3) for every undefined symbol, the list of relocations that reference it. Run it on `anatomy.o` and check your output against `nm -C` and `readelf -r`. Then add a `--collisions a.o b.o ...` mode that reports every **strong symbol defined in more than one object** (what the linker would reject) and every **weak symbol defined with different section sizes** (a likely ODR violation).

<details>
<summary><strong>Solution sketch: the core of the symbol reader</strong></summary>

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Educational: ELF64, little-endian, no validation beyond bounds checks. Reads THIS program's own executable.
int main() {
    std::ifstream f("/proc/self/exe", std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
    auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data.data());
    if (data.size() < sizeof *eh || std::string(reinterpret_cast<const char*>(eh->e_ident), 4) != "\x7f""ELF") { std::puts("not ELF"); return 1; }

    auto* sh = reinterpret_cast<const Elf64_Shdr*>(data.data() + eh->e_shoff);
    const char* shstr = data.data() + sh[eh->e_shstrndx].sh_offset;

    int defined_global = 0, defined_weak = 0, undefined = 0, local = 0, shown = 0;
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        auto* syms = reinterpret_cast<const Elf64_Sym*>(data.data() + sh[i].sh_offset);
        std::size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        const char* strtab = data.data() + sh[sh[i].sh_link].sh_offset;
        for (std::size_t k = 1; k < n; ++k) {
            const Elf64_Sym& s = syms[k];
            if (ELF64_ST_TYPE(s.st_info) != STT_FUNC && ELF64_ST_TYPE(s.st_info) != STT_OBJECT) continue;
            unsigned bind = ELF64_ST_BIND(s.st_info);
            bool def = s.st_shndx != SHN_UNDEF;
            (!def ? undefined : bind == STB_WEAK ? defined_weak : bind == STB_LOCAL ? local : defined_global)++;
            if (def && bind == STB_WEAK && shown < 3 && strtab[s.st_name] == '_') {                                   // show the first few weak symbols
                int st = 0; char* d = abi::__cxa_demangle(strtab + s.st_name, nullptr, nullptr, &st);
                std::printf("  weak: %.90s\n", st == 0 ? d : strtab + s.st_name);
                std::free(d); ++shown;
            }
        }
        std::printf("sections: %d   symbols: %d global-defined, %d weak-defined, %d local, %d undefined\n", eh->e_shnum, defined_global, defined_weak, local, undefined);
        (void)shstr;
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  weak: void std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >::_M_c
  weak: std::vector<char, std::allocator<char> >::~vector()
  weak: std::istreambuf_iterator<char, std::char_traits<char> >::istreambuf_iterator(std::istream&
sections: 41   symbols: 10 global-defined, 8 weak-defined, 13 local, 50 undefined
```

This reads the executable it is running as. Run it on a `-c` object file instead (open `argv[1]`) and you get the `nm` picture; the collision mode is a loop over several objects with a `std::unordered_map<std::string, ...>` keyed by symbol name.

</details>

---

## 10. Real-world example

| Where | Build-model issue |
|---|---|
| **Linux distributions** | `libstdc++.so` is shared by thousands of programs; a new GCC must keep the library ABI-compatible (`GLIBCXX_3.4.x` symbol versions) or every binary breaks (Chapter 37) |
| **Large C++ projects (Chromium, LLVM, Firefox)** | Compile-time engineering: unity builds, precompiled headers, forward declarations, `extern template` for hot templates, component builds (`.so` in debug, static in release), and `-fvisibility=hidden` everywhere |
| **Plug-in systems** | Hidden-by-default plus a tiny `extern "C"` entry point; two plug-ins each statically linking different versions of the same library can crash through ODR/duplicate singletons |
| **Embedded** | `-ffunction-sections -Wl,--gc-sections`, no exceptions/RTTI, linker scripts control placement; static-initialisation order matters at boot |
| **Games / engines** | Hot-reload DLLs/`.so` files; static objects with constructors in a reloaded library are the classic source of ODR-style crashes |
| **Qt** | `moc` generates additional TUs; Qt libraries are shared with a long-term binary-compatibility policy; `Q_DECL_EXPORT`/`Q_DECL_IMPORT` are the export macros of Experiment 4; **"Q_OBJECT in a header, forgot to re-run moc"** produces `undefined reference to vtable for X` (Chapter 47) |
| **Python extensions** | A `.so` loaded into a process that already contains another copy of a C++ runtime: symbol interposition and duplicate singletons (Chapter 46) |

> **Opinion.** Three rules prevent most build-model bugs. (1) **One definition, one header**: never copy a definition between files; put shared types in a header that every user includes. (2) **Everything private is `static`/anonymous-namespace, everything exported is explicit.** Default-hidden visibility turns accidental API into link errors you can see. (3) **Keep one CI job with `-flto -Wodr` and sanitizers** and one with a different link order. ODR bugs are invisible to normal testing, because the symptom changes with the build. And a heresy worth defending: for a mid-sized application, a *unity* (single-TU) release build is a legitimate way to get whole-program optimisation and fast clean builds, as long as your headers don't depend on each other's macros.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **ODR: same name, different definitions** (class layout, inline function, `#define` that changes a struct) | Memory corruption, wrong behaviour that depends on build flags/link order (Experiment 3) | One header; `-flto -Wodr`; avoid macros that change layouts; anonymous namespace for TU-local helpers |
| **Function/variable defined in a header without `inline`** | `multiple definition of 'x'` at link time when two TUs include it | `inline`, or move to a `.cpp`, or `static`/`constexpr` (knowing it duplicates) |
| **`static` variable in a header** | Every TU gets its **own** copy; "global" counter doesn't count globally; code bloat | `inline` variable (C++17) |
| **Declared but not defined** (`undefined reference`) | Link error; often a missing `.cpp` in the build, a mismatch between declaration and definition, or a template member defined in a `.cpp` | Check `nm -C | grep` for the mangled name; define templates in headers or explicitly instantiate |
| **Wrong link order with archives** | `undefined reference` though the library is on the line (Experiment 6) | Objects first, then libraries, dependents before dependencies; `--start-group` for cycles |
| **Static-init order across TUs** | Zero/garbage values at start-up that vanish when files are reordered (Experiment 5) | `constinit`, function-local statics, no cross-TU dependencies in constructors |
| **Singleton duplicated across shared libraries** | Two "global" instances; state not shared | Control visibility; define in one library; avoid header-only singletons across `.so` boundaries |
| **Missing/incorrect export macros** (Windows) or default visibility (Linux) | Huge accidental ABI; symbol clashes | `-fvisibility=hidden` + `API` macro |
| **Mixing libstdc++ and libc++** or `_GLIBCXX_USE_CXX11_ABI=0/1` | Undefined references to `std::__cxx11::basic_string`, or silent layout mismatch | One standard library and ABI setting per link (Chapter 37) |
| **`-fPIC` omitted for a static lib linked into a `.so`** | `relocation R_X86_64_32 against … can not be used when making a shared object` | Build with `-fPIC` (`CMAKE_POSITION_INDEPENDENT_CODE`) |
| **Header not self-contained / include-order dependence** | Compiles only when another header happens to come first | Include what you use; compile each header alone in CI |
| **Too many includes in headers** | Slow builds (Experiment 1) | Forward declarations, `pImpl`, include-what-you-use, PCH/modules |
| **Reading a symbol's definition from the wrong library** (interposition, `LD_PRELOAD`, older `.so` first in path) | "Works on my machine" | `LD_DEBUG=bindings`, `ldd`, `readelf -d`, RUNPATH discipline |
| **Assuming `inline` means "will be inlined"** | Surprise code size / no speed-up; or the reverse | `inline` is a linkage hint; use `[[gnu::always_inline]]`/`noinline` only after profiling |

---

## 12. Exercises

1. **Stages.** For a 10-line program using `<vector>`, save every stage with `-save-temps`. Report the sizes in lines/bytes, find the user code in the `.ii` file, and the `main` function in the `.s`.
2. **Symbols.** For each of the following, predict the `nm` letter and linkage, then check: `const int a = 1;` / `int b;` / `static int c;` / `inline int d = 2;` / `constexpr int e = 3;` / an anonymous-namespace function / a template instantiation / a virtual function's vtable.
3. **Break it three ways.** Reproduce (a) `multiple definition` (non-inline function in a header), (b) a *silent* ODR violation of a class layout across two TUs (find which compiler flag or tool detects it), (c) the wrong-link-order failure, and write down the one-line diagnosis for each.
4. **COMDAT archaeology.** Compile three TUs that all instantiate `std::map<std::string,int>::operator[]`. Use `nm`, `readelf -g`, `-Wl,--trace-symbol` and `-Wl,-Map` to show that the executable contains one copy and which object it came from.
5. **Visibility.** Build a small shared library with 20 functions, 5 public. Measure `nm -D | wc -l` and the library size with default vs hidden visibility, with and without `-fno-semantic-interposition`; look at the disassembly of an internal call in each.
6. **Header diet.** Take a real header from a project of yours, measure its parse cost (`-ftime-report`), then reduce it with forward declarations and `pImpl`. Report the saving across all TUs that include it.
7. **Init order.** Write a three-TU example where link order changes the result, then fix it three ways (`constinit`, function-local static, explicit `init()` called from `main`) and discuss destruction order at exit.
8. **Read.** In the C++ standard [basic.def.odr], list the conditions for “identical definitions”. Find one that surprises you and write a program that violates it.

---

## 13. Challenge: a build-model audit tool

Write a script (Python or C++) that, given a CMake build directory (`compile_commands.json` + object files), reports: (1) the heaviest headers by *preprocessed lines × number of TUs including them*; (2) symbols **defined as strong in more than one object** and weak symbols whose **section sizes differ between objects** (candidate ODR violations); (3) `static` or anonymous-namespace objects defined in headers (copies per TU: list the total bytes duplicated); (4) the exported dynamic symbols of each shared library that are not declared in any public header (accidental API). Run it on a real project (LLVM, a Qt app, or your own) and write up the three most valuable findings.

---

## 14. Knowledge check

1. List the stages from `.cpp` to a running process, with the artefact each produces.
2. What is a translation unit? Why can't the compiler see across them (without LTO)?
3. State the ODR in a sentence. Which parts are diagnosed, which are not, and what does "no diagnostic required" permit?
4. What does `inline` mean in modern C++? Why is it correct for a function defined in a header?
5. Why does Experiment 3 give different answers at `-O0` and `-O2`, and under different link orders?
6. What does a `W` symbol in `nm` output mean? How does the linker choose among duplicates, and why is that dangerous?
7. A `static int counter;` in a header is incremented in two `.cpp` files. What do they see? What is the fix?
8. Why must objects precede static libraries on the link line? What does `--start-group` do?
9. What does `-fvisibility=hidden` change, and why is the default bad for libraries?
10. Why is the static-initialisation order across TUs unspecified, and how do `constinit` and function-local statics avoid the problem?
11. What does `extern template` save, and what does it *not* change about the final executable?
12. What can `-flto -Wodr` detect that the normal compiler cannot, and what can it not detect?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Source → (preprocessor) translation unit `.ii` → (compiler) assembly `.s` → (assembler) object `.o` → (linker, with archives/shared libs) executable or `.so` → (dynamic loader) process image with shared libraries mapped and initialisers run.
2. One `.cpp` after `#include` expansion is compiled in isolation; the compiler has no access to other TUs' source. Without LTO it cannot inline or check consistency across them; the linker only sees symbols.
3. Each used non-inline function/variable has exactly one definition in the program, and entities that may be defined in many TUs (classes, templates, inline functions/variables) must have identical definitions with the same meaning. "Multiple/missing definition" of strong symbols is caught by the linker; differing definitions of inline/class entities generally are not; NDR means the compiler need not diagnose, and the program is ill-formed (UB in practice).
4. "May be defined in more than one TU, with identical definitions"; the definition must be visible in every TU using it. A header function needs this so every includer may define it without a multiple-definition error. (Whether it is actually inlined is the optimiser's decision.)
5. The two `answer()` definitions violate the ODR (UB, NDR). At `-O0` calls go to the one COMDAT copy the linker kept (the first in link order); at `-O2` each TU inlines its own visible body before the linker is involved, so the inconsistency doesn't show.
6. Weak definition: allowed to appear in many objects (inline functions, template instantiations, inline variables). The linker keeps the first one it encounters and drops the rest, without comparing them, so inconsistent definitions are silently resolved by link order.
7. Each TU has its own `counter` (internal linkage), so each sees only its own increments. Make it `inline int counter;` (C++17) or define it once in a `.cpp` with an `extern` declaration in the header.
8. The linker processes input left to right and extracts an archive member only to satisfy an already-undefined symbol; a library listed before its users contributes nothing. `--start-group a.a b.a --end-group` rescans the group repeatedly to resolve circular references.
9. It makes all symbols hidden unless explicitly marked `visibility("default")`, shrinking the dynamic symbol table, avoiding accidental ABI promises, interposition and symbol clashes between libraries, and enabling better optimisation of internal calls. Default visibility exports everything.
10. The standard doesn't order dynamic initialisation across TUs (the compiler/linker/loader decide; GNU ld uses link order). `constinit`/constant initialisation happens at compile time (no order), and a function-local static is initialised on first use (thread-safe), so dependencies are satisfied on demand.
11. It avoids re-instantiating (compiling and assembling) the template's non-inline members in every TU, speeding up the build and shrinking objects. The final linked executable is the same either way, since the linker de-duplicates identical weak copies anyway.
12. It compares type definitions across TUs (class layout, fields, base classes) and reports mismatches; it does not compare inline function bodies, macros-dependent differences that don't alter types, or non-LTO builds; it is also not exhaustive.

</details>

---

[← Previous: Chapter 35](../part-13-coroutines/35-coroutines-and-networking.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 37 — ABI →](37-abi.md)
