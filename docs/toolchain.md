# Toolchain Setup

> **Goal:** a Linux workstation where you can compile an experiment, read its assembly, run it under three sanitizers and profile it, all in under a minute.

[← Back to the course map](../README.md)

---

## 1. What you need

| Tool | Why | Minimum |
|---|---|---|
| GCC (`g++`) | Primary compiler; best C++26 coverage | 14 (13 works for most of the course) |
| Clang (`clang++`) | Second opinion; different diagnostics, different optimizer | 18 |
| `libstdc++` / `libc++` | The two standard libraries you will compare | shipped with the compilers |
| GNU binutils | `objdump`, `nm`, `readelf`, `c++filt`, `ld` | any recent |
| `gdb`, `lldb` | Object and stack inspection | any recent |
| Valgrind | A second UB detector, no recompilation | 3.20+ |
| `perf` | Sampling profiler, hardware counters | matches your kernel |
| CMake + Ninja | Builds | CMake ≥ 3.28 |
| Google Benchmark, GoogleTest | Benchmarks and tests | recent |
| Python 3 + dev headers, pybind11 | Chapter 46 and Project 11 | 3.10+ |
| Qt 6 | Chapter 47 | 6.5+ |

## 2. Installing on Ubuntu 24.04

```bash
sudo apt update
sudo apt install -y build-essential g++-14 clang-18 lld-18 lldb-18 libclang-rt-18-dev \
    cmake ninja-build gdb valgrind binutils \
    libbenchmark-dev libgtest-dev libgmock-dev \
    python3-dev pybind11-dev python3-pybind11 \
    linux-tools-common linux-tools-generic
```

Ubuntu 24.04 ships GCC 13 as `g++` and offers GCC 14 as `g++-14`. Make the newer one easy to reach:

```bash
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 140
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 180
```

Qt 6 for chapter 48:

```bash
sudo apt install -y qt6-base-dev qt6-base-dev-tools
```

## 3. The experiment workflow

Almost every experiment in this course is a single `.cpp` file. Put these helpers in your `~/.bashrc` and the friction disappears:

```bash
# cpp  file.cpp [extra flags]    compile with strict warnings and run
cpp() {
  local src=$1; shift
  g++-14 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -g "$@" "$src" -o /tmp/exp.out \
    && /tmp/exp.out
}

# cppc file.cpp [extra flags]    same, with clang
cppc() {
  local src=$1; shift
  clang++-18 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -g "$@" "$src" -o /tmp/expc.out \
    && /tmp/expc.out
}

# asm   file.cpp [flags]         readable assembly, demangled, Intel syntax, noise removed
asm() {
  local src=$1; shift
  g++-14 -std=c++23 -O2 -S -masm=intel -fno-asynchronous-unwind-tables -fno-ident \
         -fcf-protection=none "$@" "$src" -o - | c++filt | grep -v '^\s*\.' 
}

# san   file.cpp                 ASan + UBSan build and run
san() {
  local src=$1; shift
  g++-14 -std=c++23 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "$@" "$src" -o /tmp/san.out \
    && /tmp/san.out
}

# tsan  file.cpp                 ThreadSanitizer build and run (cannot be combined with ASan)
tsan() {
  local src=$1; shift
  g++-14 -std=c++23 -g -O1 -fsanitize=thread "$@" "$src" -o /tmp/tsan.out -pthread \
    && /tmp/tsan.out
}
```

Typical session:

```bash
cpp experiment.cpp            # does it behave the way I think?
cpp experiment.cpp -O2        # does it still?
asm experiment.cpp | less     # what did the compiler do with it?
san experiment.cpp            # is it hiding undefined behavior?
```

## 4. A warning set worth having

```text
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wnon-virtual-dtor -Woverloaded-virtual -Wold-style-cast -Wcast-align
-Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
```

GCC adds `-Wuseless-cast -Wduplicated-cond -Wlogical-op`. Clang's `-Weverything` is a useful *audit*, though not a daily flag.

> [!TIP]
> Treat warnings as hypotheses. Several chapters show a warning that is a symptom of undefined behavior, and a few where it is a false positive. Learn to tell the difference rather than silencing everything.

## 5. Sanitizers in one table

| Sanitizer | Flag | Catches | Cost | Can combine with |
|---|---|---|---|---|
| AddressSanitizer | `-fsanitize=address` | out-of-bounds, use-after-free, double free, leaks, stack-use-after-scope | ~2× time, ~2–3× memory | UBSan |
| UndefinedBehaviorSanitizer | `-fsanitize=undefined` | signed overflow, bad shifts, null deref, misaligned access, bad `bool`/enum, `vptr` | small | ASan, TSan |
| ThreadSanitizer | `-fsanitize=thread` | data races, some lock-order inversions | ~5–15× time | UBSan only |
| MemorySanitizer (Clang) | `-fsanitize=memory` | reads of uninitialized memory | ~3× | needs an instrumented stdlib |
| Valgrind memcheck | `valgrind ./a.out` | leaks, invalid reads/writes, uninitialized reads | ~20–50× | nothing to rebuild |

Useful environment variables:

```bash
export ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=1:strict_string_checks=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS=second_deadlock_stack=1
```

## 6. `perf`

`perf` needs permission to read hardware counters:

```bash
sudo sysctl kernel.perf_event_paranoid=1     # until reboot
sudo sysctl kernel.kptr_restrict=0
```

```bash
perf stat -d ./a.out                          # counters: cycles, IPC, cache and branch misses
perf record -g --call-graph dwarf ./a.out     # sample with call stacks
perf report --no-children                     # interactive report
perf annotate --stdio -s function_name        # hot instructions inside one function
```

For flame graphs, clone [Brendan Gregg's FlameGraph scripts](https://github.com/brendangregg/FlameGraph) and pipe `perf script` through `stackcollapse-perf.pl | flamegraph.pl`.

> [!NOTE]
> Containers and VMs often do not expose hardware counters. If `perf stat` shows `<not supported>` for cycles, use a physical machine or a VM with PMU passthrough for chapters 27 and 41. Everything else still works.

## 7. Compiler Explorer

[godbolt.org](https://godbolt.org) is the fastest way to compare compilers and versions. Useful tricks:

- Add a second compiler pane and use **diff view**.
- Use `-O2 -std=c++23` and tick **Compile to binary object** to see what really survives linking.
- Use the **Opt Remarks** and **Optimization output** panes to ask *why* something did not inline or vectorize.
- Use the newest GCC trunk or a P2996 Clang fork to try reflection before your local compiler supports it.
- Share short links to reproduce your findings. The links include the compiler version.

## 8. Reading object files

```bash
g++-14 -std=c++23 -O2 -c foo.cpp -o foo.o

nm -C foo.o                      # symbols, demangled   (T = defined text, U = undefined, W = weak)
nm -C --defined-only foo.o
objdump -dC -M intel foo.o       # disassembly, demangled, Intel syntax
objdump -dC -M intel -r foo.o    # ...with relocations, so you can see what the linker must patch
readelf -S foo.o                 # sections: .text, .rodata, .data, .bss, .eh_frame, .gcc_except_table
readelf -sW foo.o                # symbol table with binding and visibility
readelf --dyn-syms libfoo.so     # what a shared library exports
c++filt _ZNSt6vectorIiSaIiEE9push_backERKi
```

## 9. Which compiler for which chapter

| Chapters | Use | Why |
|---|---|---|
| Most of the course | GCC 14 | `<print>`, `<expected>`, `<generator>`, deducing `this`, ranges |
| Comparison experiments | Clang 18 + libstdc++ | Different optimizer and diagnostics, same library |
| Chapter 12, 14 | GCC 14, newer if available | C++23 views (`zip`, `chunk`, `slide`, `to`) |
| Chapter 11 | GCC 16 or Compiler Explorer | `std::mdspan` ships in libstdc++ with GCC 16 |
| Chapter 18 | GCC 16 (`-freflection`) or a P2996-capable Clang fork on Compiler Explorer | Reflection is new; check release notes |
| Chapter 38 | Clang 18+ or GCC 14+ with CMake ≥ 3.28 and Ninja | Modules need build-system support |

> [!WARNING]
> Compiler support changes monthly. Whenever a chapter shows a version number, treat it as a snapshot and re-check [cppreference's compiler support page](https://en.cppreference.com/w/cpp/compiler_support).

## 10. A scratch CMake project

For the larger exercises and projects, this template gives you sanitizers as one switch:

```cmake
cmake_minimum_required(VERSION 3.28)
project(scratch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

option(SANITIZE "Build with ASan+UBSan" OFF)
if(SANITIZE)
  target_compile_options(project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(project_options INTERFACE -fsanitize=address,undefined)
endif()

add_executable(exp exp.cpp)
target_link_libraries(exp PRIVATE project_options)
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZE=ON
cmake --build build && ./build/exp
```

Keep it simple: a plain CMake build like this is all the course needs.
