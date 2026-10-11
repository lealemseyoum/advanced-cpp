# Chapter 46 — C++ and Python: Extensions, Bindings and the GIL

> **Part XVII · C and Python interoperability** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 9 hours**
> **Prerequisites:** [Chapter 25 (smart pointers)](../part-10-memory/25-smart-pointers.md), [Chapter 29 (threading)](../part-12-concurrency/29-threading.md), [Chapter 37 (ABI)](../part-14-compilation-and-linking/37-abi.md), [Chapter 45 (C compatibility)](45-c-compatibility.md) &nbsp;|&nbsp; **Standards:** none for the Python side (CPython C API, PEP 3118 buffer protocol); C++17/20 for the glue &nbsp;|&nbsp; **Tools:** `python3.12` + headers, **pybind11 2.11.1**, NumPy 1.26, Cython 3.3, `g++-14`/`g++` 13.3

[← Previous: Chapter 45](45-c-compatibility.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 47 — Modern C++ in Qt →](../part-18-qt/47-modern-cpp-in-qt.md)

---

**In one sentence:** Python extensions are shared libraries that follow CPython's C API (reference-counted `PyObject*`, a global interpreter lock, a tagged ABI); pybind11 is a C++17 template layer that generates that C-API code for you, and the real engineering questions are **who owns each object, who holds the GIL, what is copied, and what crosses as an exception**.

**By the end of this chapter you can:**

- write a raw CPython extension and explain borrowed vs new references (and demonstrate a leak with `sys.getrefcount`)
- bind functions, classes, exceptions, callbacks and NumPy arrays with pybind11, and know which of them copy
- use the buffer protocol to share memory with Python **without copying**, and measure the difference
- release the GIL correctly, and show a 2× parallel speedup (and the mistakes that corrupt it)
- reason about ownership: return-value policies, `reference_internal`, keep-alive
- explain the ABI constraints on extension modules (interpreter version, compiler, standard library) and choose between hand-written C API, pybind11, Cython and `ctypes`

---

## 1. Problem

Python is the interface; C++ is the engine. The same facts that make Python productive make it slow at inner loops (dynamic dispatch, boxed numbers, the GIL), so the standard architecture is **Python orchestrates, native code computes**. The boundary is where the bugs and the performance cliffs live:

| Boundary issue | Symptom |
|---|---|
| **Object model mismatch** | Python objects are heap-allocated, reference-counted and dynamically typed; C++ objects are values with static types and RAII |
| **Lifetime** | A C++ pointer returned to Python can outlive the C++ object; a Python object stored in C++ can be freed under you |
| **Copying** | Converting a 10⁶-element `list` to `std::vector<double>` on every call costs more than doing the work (Experiment 3) |
| **Exceptions** | C++ exceptions must become Python exceptions and vice versa; neither may cross the other's frames raw (Chapter 45) |
| **Concurrency** | The GIL serialises Python bytecode; native code must release it to run in parallel, and must not touch Python objects while released |
| **ABI/versioning** | An extension built for Python 3.12 does not load in 3.13; mixing standard libraries or compilers can break exception and RTTI handling |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1991 | CPython 0.9 ships with a C API; extension modules are how Python gets `os`, `socket`, `re` |
| 1996–2003 | **SWIG** generates wrappers from C/C++ headers (still used for multi-language bindings) |
| 2002 | **Boost.Python**: C++ template metaprogramming binds classes (the model for later libraries) |
| 2007 | **Cython** (from Pyrex): Python-like language compiled to C/C++ with typed fast paths |
| 2006 | `ctypes` in the standard library (3rd-party earlier): call C functions in shared objects with no compiler |
| 2007/2008 | **PEP 3118** (buffer protocol, Python 3.0) standardises zero-copy sharing of array memory; NumPy builds on it |
| 2015 | **pybind11** (Wenzel Jakob): Boost.Python's design as a small header-only C++11 library without the Boost dependency |
| 2016 | **cffi** popularises ABI-mode and API-mode C bindings; **PEP 384** (limited API/stable ABI, `abi3`) |
| 2022 | **nanobind** (same author): smaller binaries and faster calls than pybind11, C++17 and newer |
| 2023–24 | CPython 3.12 per-interpreter GIL; **PEP 703** accepted: an optional *free-threaded* build (experimental in 3.13) makes "what does the GIL protect?" a live question 🟡 |
| 2025–26 | Extension authors target multiple Python versions via `abi3` where possible and scikit-build-core/CMake/Meson for builds |

---

## 3. Modern solution

Four ways to call native code, and when each is right:

| Approach | You write | Pros | Cons | Use when |
|---|---|---|---|---|
| **`ctypes` / `cffi`** | Python, against a C API (Chapter 45) | No compiler step on the Python side; works with any language's C ABI | Manual marshalling; no C++ types; crashes on mistakes; slow per call | Wrapping an existing C-ABI shared library quickly |
| **Raw C API** | C/C++ against `Python.h` | Zero dependencies; the most control; works with the **stable ABI** | Verbose; reference counting by hand; error-prone | Tiny modules; embedding; you need `abi3` |
| **pybind11 / nanobind** | C++ with templates | Concise; C++ types, classes, STL, NumPy, exceptions, callbacks handled; RAII for `PyObject` | Compile times; template-heavy errors; ABI-sensitive internals | **Binding a C++ library** (the default) |
| **Cython** | `.pyx` (Python with C types) | Great for numeric loops written in a Python dialect; type-checked | A third language and a build step; C++ support is less ergonomic for templates | Optimising Python code in place; thin wrappers |

The architecture to aim for (extends Chapter 45):

```text
   Python application
        │   import mymod
        ▼
   binding layer   (pybind11 module: m.def, py::class_)            ← THIN: conversion + policy only
        │
        ▼
   C++ library     (no Python.h includes, unit-tested on its own)  ← the real code
```

**The binding layer must be a leaf**: the C++ library must not include `pybind11/…` or `Python.h`. That keeps it testable, reusable from other languages, and keeps Python's lifetime rules out of the core.

---

## 4. Mental model

### Everything in Python is a `PyObject*` with a reference count

```text
   PyObject  { Py_ssize_t ob_refcnt;  PyTypeObject* ob_type; ... payload ... }
                     ▲
                     └── incremented by Py_INCREF, decremented by Py_DECREF; at zero the object is destroyed
```

The C-API discipline is an **ownership vocabulary**, the same one as Chapter 25 in different words:

| Term | Meaning | C++ analogue |
|---|---|---|
| **New reference** | You own one count; you must `Py_DECREF` it (or hand it on) | `unique_ptr` you hold |
| **Borrowed reference** | You do not own it; valid only while something else holds it | raw pointer / reference |
| **Stolen reference** | The callee takes your count | moved-from `unique_ptr` |

Forgetting a `Py_DECREF` is a leak; an extra one is a use-after-free. Experiment 1 shows the former with `sys.getrefcount`. pybind11's `py::object` is `unique_ptr` for `PyObject*`: constructor and destructor do the INCREF/DECREF, so you don't write them.

### The GIL: one thread runs Python bytecode at a time

```text
   Thread A:  [ Python code ][ native, GIL held ]............[ Python ]
   Thread B:  ..............[ waiting for the GIL............][ Python ]        ← default: native code serialises threads

   Thread A:  [ Python ][ native, GIL RELEASED ──────────────────][ Python ]
   Thread B:  ..........[ Python ][ native (also released) ──────][ Python ]    ← parallel, on separate cores
```

A native function that is called from Python **holds the GIL**. While it computes, no other Python thread runs. Releasing it (`py::gil_scoped_release`, `Py_BEGIN_ALLOW_THREADS`) lets others run, **but then your code must not touch any `PyObject`**: reading a `list` while released is a data race. The rule: *copy what you need into C++ values first, release, compute, reacquire, then build results.*

### Three ways to move an array

```text
   Python list of floats   ──convert──►  std::vector<double>     copies 1M elements + creates/destroys boxed objects
   NumPy array / buffer    ──────────►   pointer + shape + strides  NO copy: C++ sees Python's memory
   bytes / bytearray       ──────────►   pointer + length          NO copy (bytes is immutable!)
```

### Ownership across the boundary: the return-value policy decides

When C++ returns a pointer or reference, pybind11 must decide who owns the pointee. This is the part of binding code that most often produces crashes:

| Policy | Python gets | Danger |
|---|---|---|
| `copy` (default for returned lvalue references of copyable types) | an independent copy | None for correctness; costs a copy; mutations don't reach C++ |
| `move` | the moved-from value | none |
| `take_ownership` (default for returned raw pointers) | ownership; Python will `delete` it | Double-delete if C++ also owns it |
| `reference` | a non-owning view | **Dangling** if the C++ owner dies first |
| `reference_internal` | a non-owning view **plus** a keep-alive on the parent object | Safe against parent death; still dangles if the C++ container reallocates (Experiment 2) |

### Exceptions translate in both directions

```text
   C++ throw std::invalid_argument  ──pybind11 catch──►  PyErr_SetString(ValueError)  ──► Python except ValueError
   Python raise KeyError in callback ──error_already_set──► C++ exception  ──(propagates through your C++)──► back to Python as KeyError
```

---

## 5. Language rules

The rules here belong to CPython and pybind11, not to the C++ standard; I flag the C++ standard rules that matter:

| Rule | Source | Consequence |
|---|---|---|
| **Module init function** | CPython: `PyInit_<name>` (exported, C linkage) | The only symbol an extension must export; `-fvisibility=hidden` leaves exactly this (Experiment 6) |
| **`PyObject*` reference rules** | CPython docs ("reference counts") | Every API function documents "new / borrowed / stolen" |
| **Error indicator** | CPython | A function returning `NULL` (or `-1`) **must** have set an exception; returning a value with an exception set is a bug |
| **GIL** | CPython (≤ 3.12 always; 3.13 optional free-threaded build 🟡) | Hold it to call the C API; release it only around pure C++ work |
| **Buffer protocol** | PEP 3118 | `Py_buffer` view with pointer, format string (`"d"`, `"f"`…), shape, strides, readonly flag; must be released |
| **Stable ABI** | PEP 384 | `Py_LIMITED_API` restricts you to functions that stay binary-compatible across 3.x versions (`abi3` wheels); pybind11 does not use it |
| **Extension filename tag** | PEP 3149 / `importlib` | `name.cpython-312-x86_64-linux-gnu.so`: interpreter version, platform; the import system matches the suffix |
| **C++: exceptions across frames** ⚖️ | Chapter 45 | No C++ exception may escape a `PyCFunction`: the wrapper must catch and set a Python error (pybind11 does it) |
| **C++: static-init, RTTI** ⚖️ | Itanium ABI | `typeid`/`dynamic_cast` and exception matching across DSOs depend on a **single** definition of each type_info: hence pybind11's ABI-id check and `-fvisibility=hidden` |
| **C++: destructors on abnormal exit** ⚖️ | Standard | If Python is torn down at interpreter exit, C++ objects held by Python are destroyed *late*: don't rely on `atexit` order |

### Layer check

| Layer | Question |
|---|---|
| **C++ standard** | Lifetimes, exceptions, move semantics: the same as always; nothing about Python |
| **Compiler** | Which compiler/standard library built the extension? pybind11 encodes `gcc`, `libstdcpp`, `cxxabi`-version in its internals id (Experiment 6) |
| **Python/ABI** | CPython version (the `.cpython-312` tag), interpreter build flags (debug, free-threaded), whether the extension uses the stable ABI |
| **OS / linker** | Extensions are `dlopen`ed (by default `RTLD_LOCAL`); symbols come from the interpreter, not from a linked `libpython` (Experiment 6) |
| **CPU** | Memory layout of buffers (C vs Fortran order, strides), vectorisation, NUMA: same as any native code |

---

## 6. Implementation model

### What pybind11 generates

For `m.def("divide", &divide, py::arg("a"), py::arg("b") = 1.0)` it instantiates a **dispatcher**: a `PyCFunction` that (1) parses positional/keyword arguments into a tuple of `type_caster<T>` objects, (2) tries each registered overload in order, (3) calls your function inside a `try`, (4) converts the result with the return policy, (5) catches C++ exceptions and sets the Python error. Each `type_caster<T>` is a conversion rule: `double` ← `float`/`int`; `std::vector<T>` ← any sequence (copy); `std::function` ← any callable (wrapper that takes the GIL when called); `py::array_t<T>` ← buffer or convertible (copy if `forcecast` and the dtype differs).

### Where the cost goes (Experiment 3)

| Path | Cost per element | Why |
|---|---|---|
| `sum(list)` in Python | ~9 ns | Iterate, unbox `PyFloat`, add; C code in the interpreter |
| C API `sum_floats(list)` | ~9 ns | The same work, written by hand: no faster than the built-in |
| pybind11 `std::vector<double>` parameter | ~32 ns | Converts each element **and allocates the vector**, then sums |
| pybind11 `array_t<double>` (zero copy) | ~1.2 ns | A pointer and a tight loop |
| `numpy.sum` | ~0.27 ns | SIMD pairwise summation in C |

The lesson: **native code is fast only if the data is already in native layout.** The conversion boundary is usually the bottleneck, not the loop.

### Embedding a callback

`py::function` and `std::function<…>` parameters hold a reference to a Python callable; calling one from C++ needs the GIL (pybind11's wrapper acquires it with `gil_scoped_acquire`). Python exceptions in the callback become `py::error_already_set` C++ exceptions, which propagate through your C++ code (running destructors) and are converted back at the boundary (Experiment 2).

---

## 7. Experiments

Everything is in [`code/ch46/`](code/ch46/): `capi_ext.cpp`, `pb_ext.cpp`, `cy_ext.pyx`, three Python test scripts and `run.sh`. These are multi-file, interpreter-dependent experiments, so they are **not auto-verified** by the snippet checker; the outputs below are real runs of `bash run.sh`. **Python 3.12.3, pybind11 2.11.1, NumPy 1.26.4, Cython 3.3.0, GCC 13.3 / 14.2, 2-vCPU VM.**

### Experiment 1 ✅: The raw C API, reference counts, and a deliberate leak

`capi_ext.cpp` implements `sum_floats(iterable)`, `leaky_sum`, `first_item(list)`, and `divide(a, b)` against `Python.h`. The core:

```cpp
static PyObject* sum_floats(PyObject*, PyObject* args) {
    PyObject* seq;
    if (!PyArg_ParseTuple(args, "O", &seq)) return nullptr;           // TypeError already set
    PyObject* it = PyObject_GetIter(seq);                              // NEW reference: ours to release
    if (!it) return nullptr;
    double total = 0;
    while (PyObject* item = PyIter_Next(it)) {                         // NEW reference per item
        double v = PyFloat_AsDouble(item);
        Py_DECREF(item);                                               // forget this and every item leaks
        if (v == -1.0 && PyErr_Occurred()) { Py_DECREF(it); return nullptr; }
        total += v;
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return nullptr;
    return PyFloat_FromDouble(total);                                  // NEW reference handed to the caller
}
```

Built with `g++ -shared -fPIC $(python3.12-config --includes) capi_ext.cpp -o capi_ext.cpython-312-x86_64-linux-gnu.so`, tested by `test_capi.py`:

```text
sum_floats: 7.0
sum of generator: 12.0
TypeError: 'int' object is not iterable
TypeError: must be real number, not str
ValueError: division by zero
refcount after correct sum  : 0 (delta)
refcount after leaky sum    : 10 (delta)  <- one leaked reference per item
first_item: x  refcount delta: 0
```

Notes. (1) The `TypeError` messages come from CPython (`PyObject_GetIter`, `PyFloat_AsDouble`): the C API sets the exception and you merely return `NULL`. (2) **The leak is observable**: `sys.getrefcount(obj)` increases by 10 after `leaky_sum([obj]*10)`: those objects can now never be freed. (3) `first_item` shows the other half: `PyList_GetItem` returns a **borrowed** reference, so returning it requires `Py_INCREF`; with the INCREF the count returns to its starting value once the caller drops the result. This is exactly the discipline `py::object` automates.

### Experiment 2 ✅: pybind11: functions, exceptions, callbacks, classes

`pb_ext.cpp` binds the same ideas. A function with a default argument, a C++ exception that becomes `ValueError`, a *custom* exception registered with a translator, a Python callable called from C++, and a class whose methods return references:

```text
divide(6, 3): 2.0   default b: 5.0
ValueError: division by zero
BudgetExceeded: spent 500
TypeError: sum_list(): incompatible function arguments. The following argument types are supported:
divide(a: float, b: float = 1.0) -> float
integrate x^2 on [0,1]: 0.333333
Python exception propagated through C++: KeyError('callback failed')
```

- The auto-generated docstring `divide(a: float, b: float = 1.0) -> float` comes from the C++ signature.
- Passing a `str` where a `std::vector<double>` is expected yields pybind11's overload-mismatch `TypeError` listing the supported signatures. No code written.
- The `integrate(f, a, b, n)` C++ loop calls a Python lambda 1 000 times (`0.333333`). When the callback raises `KeyError`, the exception unwinds *through the C++ `for` loop* and re-emerges as the same `KeyError` in Python.

The class `Inventory` shows the **return-value policy** problem, the most important lesson of the binding layer:

```text
add_default returns a COPY; inventory item still: Item(bolt, 10)       <- we set it.qty = 99 on the copy; C++ never saw it
add_ref returns a reference; inventory sees: Item(nut, 7)               <- reference_internal: writes go through
Inventory destroyed after `del inv`? 0 (alive: `it` still references into it...)
it still valid: Item(nut, 7)
after deleting the item too -> destroyed count: 1
```

Policy `reference_internal` ties the lifetime of the returned `Item` reference to the parent `Inventory` object (keep-alive): after `del inv`, the C++ object was **not** destroyed, because the Python `Item` proxy still points into it; once `it` is deleted too, the destructor ran (counter = 1). That is the keep-alive mechanism working. **What it does not protect against**: `add_ref` returns a reference into a `std::vector<Item>`; a later `add` can reallocate the vector and leave the earlier Python proxy dangling. The parent is alive, but the element moved. (I did not run this failure; it is undefined behaviour, and the fix is a container with stable addresses such as `std::deque` or `std::vector<std::unique_ptr<Item>>`, or returning indices/copies.)

### Experiment 3 ✅: Where the time goes: list, buffer, NumPy

Sum 1 000 000 floats (best of 7):

```text
  python sum(list)                    8.89 ms
  C API sum_floats(list)              9.28 ms
  pybind11 sum_list(list)            34.48 ms   (list -> std::vector copy each call)
  pybind11 sum_array(ndarray)         1.23 ms   (zero copy)
  numpy arr.sum()                     0.27 ms
  python loop over ndarray           55.46 ms
```

Three things worth learning from the table, none of which is "C++ is fast":

1. **A hand-written C-API loop over a Python `list` is no faster than the built-in `sum`** (9.3 vs 8.9 ms). The cost is unboxing a million `PyFloat` objects, which native code cannot avoid.
2. **pybind11's convenient `std::vector<double>` parameter is 4× *slower* than pure Python's `sum`**: it builds a 8 MB vector on every call. Elegant signature, bad performance.
3. **Zero copy changes everything**: `array_t<double>` runs at 1.2 ms (7× faster than Python), but NumPy's own SIMD `sum` is a further 4.5× faster. A naive loop in C++ is not automatically better than a library that already does the job.

*The buffer protocol* (PEP 3118) lets any object that exports memory (NumPy arrays, `array.array`, `memoryview`, `mmap`, `bytes`) be read **without a copy**. `buffer_sum(py::buffer)` takes any of them and checks the format and shape:

```text
array.array('d'):                      6.5
memoryview of numpy slice (strided):  20.0       <- strides honoured, still no copy
numpy float64 2-step view:            20.0
ValueError: expected a 1-D buffer of doubles, got format 'f', ndim=1       <- array('f')
ValueError: expected a 1-D buffer of doubles, got format 'B', ndim=1       <- bytes
ValueError: expected a 1-D buffer of doubles, got format 'd', ndim=2       <- 2-D array
```

and the memory really is shared:

```text
sum_array: 10.0
same memory (no copy): True            <- data_address(a) == a.__array_interface__["data"][0]
after scale_inplace: [ 0. 10. 20. 30. 40.]     <- C++ mutated the caller's array
int32 array via forcecast (copy): 10.0
scale_inplace on an int array mutates a COPY: [0 1 2 3 4]    <- dtype mismatch: pybind11 converted a copy, the original is untouched
```

That last line is a classic trap: a function that *mutates in place* and accepts a type-converting `array_t` silently mutates a temporary when the dtype doesn't match. Use `py::array_t<double, py::array::c_style>` **without** `forcecast` for mutators (a mismatch then raises) or check `a.writeable()`/dtype explicitly.

### Experiment 4 ✅: The GIL: serial vs parallel, measured

Two Python threads each call a CPU-bound C++ function (`n = 2·10⁷` sine evaluations):

```text
CPU-bound C++ call:  one call = 195 ms
  2 threads, holding the GIL     :  438 ms   (ideal parallel = 195, serial = 389)
  2 threads, gil_scoped_release  :  205 ms
```

Holding the GIL the two calls ran **one after the other** (438 ms ≈ 2 × 195 plus some switching overhead); releasing it ran them in parallel on two cores (205 ms ≈ one call). The fix is a single line (`py::gil_scoped_release release;` at the top of the function), and it is legal here only because the function touches no Python objects after that line: its argument is a C++ `long` that was converted *before* release. (This machine has two vCPUs, so "2×" is the ceiling.) For a function that takes a `py::array_t`, the pattern is to take a raw pointer and length, release, compute, and *not* call `a.shape(0)` or any other Python-API method while released.

### Experiment 5 ✅: Cython: the same job from a different direction

`cy_ext.pyx` (15 lines) defines `sum_memview(double[::1] a)` and a `nogil` loop:

```text
cython memoryview sum: 1.22 ms     pybind11 sum_array: 1.22 ms
cython spin result matches C++: True
```

The same speed as the C++ version, because both compile to a scalar loop over a pointer. Cython generated 1.08 MB of C++ from 15 lines; the value is that the *source* looks like Python, not that the output is small. It is the natural choice when you are optimising a Python module in place; pybind11 is the natural choice when you are exposing an existing C++ library.

### Experiment 6 🧩: ABI: what the module is tied to

```text
pb_ext.cpython-312-x86_64-linux-gnu.so            <- interpreter, version, platform in the name
$ python3.13 -c "import pb_ext"                    ModuleNotFoundError: No module named 'pb_ext'      <- suffix doesn't match: not even tried
$ nm -D --defined-only pb_ext...so | grep ' T '   PyInit_pb_ext                                       <- the only exported function (-fvisibility=hidden)
$ ldd pb_ext...so | grep -E 'python|stdc'          libstdc++.so.6 only                                    <- NOT libpython: symbols come from the interpreter
$ strings pb_ext...so | grep pybind11_internals    __pybind11_internals_v5_gcc_libstdcpp_cxxabi1019__     (built with g++-14; the g++ 13.3 build says ...cxxabi1018__)
```

- **Version**: the import system looks only for files whose suffix matches the running interpreter, so a 3.12 build is invisible to 3.13. Wheels carry the same tags (`cp312-cp312-manylinux_2_28_x86_64`); `abi3` wheels (limited API) are the exception.
- **Exports**: with `-fvisibility=hidden` only `PyInit_*` is visible, which avoids symbol clashes between two extensions that bundle different pybind11 copies.
- **No libpython link**: the extension uses CPython functions provided by the process that loaded it. Linking `libpython` into an extension is a bug source (two interpreters in one process).
- **pybind11's internal ID** embeds the compiler family, standard library and C++ ABI version (`gcc`, `libstdcpp`, `cxxabi1019`); modules with different IDs do **not** share type registrations, so a `py::class_<Foo>` bound in one module cannot be passed to a function bound in another that was built with a different compiler/ABI. We saw the ID change between GCC 13.3 (`1018`) and GCC 14.2 (`1019`) on the same machine. For a plug-in-like set of binding modules, build them all with the same toolchain and pybind11 version.

---

## 8. Assembly / runtime investigation

```bash
# What did the module import and export?
nm -D --defined-only pb_ext*.so | c++filt           # PyInit_pb_ext only?
nm -D --undefined-only pb_ext*.so | grep -E ' Py|_Py' | head   # CPython API the module calls (resolved at load time)
readelf -d pb_ext*.so | grep NEEDED                 # libstdc++ yes, libpython no
ls -l capi_ext*.so pb_ext*.so cy_ext*.so            # 17 KB (C API) vs 314 KB (pybind11) vs Cython

# Interpreter-level views
python3.12 -X importtime -c 'import pb_ext'         # load time
python3.12 -c 'import sys; print(sys.getrefcount(obj))'    # reference counts (always +1 for the argument)
python3.12 -X dev -c 'import capi_ext; ...'         # dev mode: more checks, faulthandler enabled
PYTHONMALLOC=debug python3.12 test.py               # debug hooks on the allocators: catches some misuse

# Memory errors in an extension: run Python under ASan
LD_PRELOAD=$(g++ -print-file-name=libasan.so) ASAN_OPTIONS=detect_leaks=0 python3.12 test_pb.py
valgrind --suppressions=<cpython>/Misc/valgrind-python.supp python3.12 -X dev test.py

# GIL contention
py-spy dump --pid <pid> ; py-spy record -o out.svg --native -- python3.12 bench.py   # (not installed here)
python3.12 -c "import sys; print(sys.getswitchinterval())"   # 5 ms default: how often threads are asked to yield

# What does a dispatcher cost per call?
python3.12 -m timeit -s 'import pb_ext' 'pb_ext.divide(1.0, 2.0)'
python3.12 -m timeit -s 'import math' 'math.sqrt(2.0)'      # a built-in C function for comparison
```

---

## 9. Implementation exercise

**Build `fastmath`: a small module with three functions that cover the interop surface.** Requirements:

1. `moving_average(a: ndarray[float64], window: int) -> ndarray[float64]`: returns a **new** array computed without copying the input, validates dtype/contiguity/`window` (raising `ValueError`), and releases the GIL during the loop.
2. `class Histogram`: owns a `std::vector<uint64_t>` of bins; `add(array)` (GIL released), `counts() -> ndarray` that **shares** memory with the object (`reference_internal`/capsule keep-alive), `__len__`, `__iter__`.
3. `map_parallel(f: Callable[[float], float], a: ndarray, threads: int) -> ndarray`: runs the Python callable in C++ worker threads. Think: what does the GIL do to this? Compare with a pure-Python loop and a `numpy.vectorize`.
4. Tests (pytest): exceptions, dtype mismatches, a subclass-able class, `del` ordering, a thread-stress test; measure each against NumPy.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
namespace py = pybind11;

py::array_t<double> moving_average(py::array_t<double, py::array::c_style> a, int window) {
    if (a.ndim() != 1)            throw py::value_error("expected a 1-D array");
    if (window <= 0 || window > a.shape(0)) throw py::value_error("window out of range");
    const py::ssize_t n = a.shape(0), m = n - window + 1;
    py::array_t<double> out(m);
    const double* in = a.data();                       // raw pointers BEFORE releasing the GIL
    double* o = out.mutable_data();
    {
        py::gil_scoped_release release;                // no Python API in this scope
        double acc = 0; for (int i = 0; i < window; ++i) acc += in[i];
        o[0] = acc / window;
        for (py::ssize_t i = 1; i < m; ++i) { acc += in[i + window - 1] - in[i - 1]; o[i] = acc / window; }
    }
    return out;
}
```

For `Histogram::counts()` return an array that **wraps the vector's memory with a base object** (`py::array_t<uint64_t>(shape, strides, ptr, py::cast(this))`: the `base` argument keeps `this` alive). For `map_parallel`: calling a Python function from a C++ worker thread requires `py::gil_scoped_acquire`, which **serialises the calls**: the result is no faster than a Python loop, which is the lesson. If `f` is a NumPy ufunc or a C function, call it on whole arrays instead.

</details>

---

## 10. Real-world example

| Where | How Python and C++ meet |
|---|---|
| **NumPy / SciPy / pandas** | C (and Cython, now some C++) cores behind a Python API; the buffer protocol / `__array_interface__` is *the* interchange format |
| **PyTorch / TensorFlow / JAX** | A large C++ core (ATen, XLA) bound with pybind11 (PyTorch) and custom machinery; the Python layer builds graphs, the C++ layer runs them with the GIL released |
| **OpenCV, Open3D, Ceres, onnxruntime** | pybind11 (or generated) bindings over large C++ libraries; arrays pass as `cv::Mat` ↔ `ndarray` zero-copy views |
| **Qt for Python (PySide6), PyQt** | **Generated wrappers** (Shiboken, SIP) because Qt's API depends on `QObject` ownership, signals/slots and `moc` metadata that a template binder can't infer; ownership is expressed with parent/child (`QObject` tree) and keep-alive annotations, and the event loop and GIL interact (Qt callbacks into Python acquire the GIL) |
| **Polars, Pydantic-core, Ruff** | Rust with PyO3: the same shape (native core, thin binding, GIL release) in another language |
| **CPython itself** | The standard library's C accelerators (`_json`, `_heapq`, `_struct`) with the raw C API; many are being moved to the stable ABI |

> **Opinion.** **Treat the binding layer as a protocol, not as code you "just generate".** Decide per function: (1) what is copied (default: nothing large), (2) who owns returned references (default: copy; `reference_internal` only for containers with stable addresses), (3) whether it runs with the GIL released (default: yes for anything that takes more than ~50 µs and doesn't touch Python objects), (4) which exceptions map to which Python types. Write those four decisions in the binding file. For performance, remember the most important number in the chapter: **native code reading a Python list is no faster than Python.** If your inner loop iterates Python objects, you have not escaped Python; get the data into a buffer first. And never let the binding layer leak into the core: keep `pybind11` out of the headers of your C++ library.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Missing `Py_DECREF` / returning a borrowed reference without `Py_INCREF`** | Leak (Experiment 1) or crash later (refcount hits zero too early) | `py::object` / `py::handle`; check with `sys.getrefcount` in tests |
| **Returning `NULL` without setting an exception** | `SystemError: <built-in function f> returned NULL without setting an exception` | Set one (`PyErr_SetString`) or return a value |
| **Returning a value with an exception set** | `SystemError: ... returned a result with an exception set` | Clear it or return `NULL` |
| **Calling Python API with the GIL released** | Random crashes, corrupted refcounts, data races | Convert inputs before `gil_scoped_release`; release only around pure C++ (Experiment 4) |
| **Never releasing the GIL in long computations** | Python threads and event loops freeze; no parallel speed-up | `py::gil_scoped_release` |
| **Calling back into Python from a native thread without the GIL** | Crash | `py::gil_scoped_acquire` in the thread (and make sure the interpreter outlives the thread) |
| **Pointer-returning method with the wrong policy** | Double free (`take_ownership` on a pointer C++ also owns), or dangling reference (`reference`) | Return by value; `reference_internal`; smart pointers (`std::shared_ptr` holder) |
| **Reference into a container that reallocates** | Dangling proxy even with keep-alive (Experiment 2) | Stable-address containers, indices, copies |
| **Mutating function with `forcecast` array parameter** | Silently modifies a temporary copy (Experiment 3) | No `forcecast`; check dtype; `a.writeable()` |
| **`std::vector<T>` parameters for big data** | 4× slower than pure Python (Experiment 3) | `py::array_t`/`py::buffer`/`std::span` via the buffer protocol |
| **Holding a `Py_buffer` or raw pointer after the buffer owner dies** | Use-after-free | Keep the `py::buffer`/`array` object alive for the whole use |
| **C++ exception escaping a raw C-API function** | Interpreter aborts with `terminate` | Catch all; set Python errors (the `guarded` pattern, Chapter 45) |
| **Mixing compilers / standard libraries between binding modules** | Types not shared, odd `TypeError`s, cross-module exceptions fail | One toolchain; same pybind11 version; check the internals id |
| **Linking `libpython` into the extension** | Two interpreters; crashes in embedded scenarios | Don't link it; use `-undefined dynamic_lookup`-style (macOS) / leave unresolved (Linux) |
| **Wrong interpreter at build vs run time** | `ModuleNotFoundError` / `ImportError` (Experiment 6) | Build with the target's `python-config` / wheel tags |
| **Static destruction order at interpreter exit** | Crash on exit from C++ statics holding Python objects | Avoid global `py::object`s; `py::module_local`; leak deliberately or use `atexit` |
| **Subclassing a bound class in Python without a trampoline** | C++ virtual methods don't dispatch to Python overrides | `PYBIND11_OVERRIDE` trampoline class |
| **Free-threaded builds (3.13t) 🟡** | Races where the GIL used to protect you implicitly | Make C++ state thread-safe; audit module-level globals; check the extension's declared GIL-free support |

---

## 12. Exercises

1. **Leak hunt.** Add a bug of each kind (missing `DECREF`, extra `DECREF`, missing `INCREF` on a borrowed return) to `capi_ext.cpp`. Detect each with `sys.getrefcount`, `PYTHONMALLOC=debug`, and (for the extra DECREF) `-X dev`/ASan.
2. **Policies.** For `Inventory`, implement `at()` with every return-value policy. For each, write Python that is correct and Python that crashes or corrupts. Record which ones ASan catches.
3. **Zero copy both ways.** Return a NumPy array that *views* a `std::vector<double>` owned by a C++ object, with correct lifetime (use a capsule or the parent object as `base`). Prove no copy was made and that the vector outlives the array.
4. **The cost of a call.** Measure the per-call overhead of `pb_ext.divide`, `capi_ext.divide`, a Cython `cpdef`, `ctypes` calling a C `double div(double,double)`, and `math.sqrt`. Explain the ranking.
5. **GIL scaling.** Repeat Experiment 4 for 1, 2, 4, 8 threads on a machine with enough cores; plot speed-up with and without release. Add a Python-object-touching callback in the middle and see what happens.
6. **Exceptions both ways.** Define a C++ exception hierarchy and map each to a Python exception class hierarchy with `register_exception_translator`; call back into Python that raises, and check that C++ destructors run (RAII counters).
7. **Trampoline.** Bind an abstract C++ class with a virtual method, subclass it in Python, and call it from C++ (`PYBIND11_OVERRIDE_PURE`). What happens to the GIL, and what happens if the Python override raises?
8. **Package it.** Build a wheel (`pyproject.toml` with scikit-build-core or setuptools), `pip install` it into a fresh virtual environment, and inspect the wheel's tags (`cp312-cp312-linux_x86_64`); then try an `abi3` build using the limited API and compare sizes and supported interpreters.

---

## 13. Challenge: wrap the Chapter 45 library for Python three ways

Take the `wordcount` C++ library and its C shim from Chapter 45. Expose it to Python in **three** ways: (a) `ctypes` against the C shim; (b) pybind11 directly against the C++ class; (c) a raw C-API module against the C shim. For each, implement `add_text`, `top_k(k)`, and a callback-based `visit`. Measure: import time, per-call overhead, throughput on a 100 MB text file loaded as `bytes` (zero-copy vs copy), memory behaviour with a million `Counter` objects created and dropped (leak check), and exceptions from bad input. Then write the one-page comparison: lines of code, failure modes, build complexity, ABI constraints (which Python versions load which binary), and a recommendation for the case of "a C++ library with 200 functions and 20 classes".

---

## 14. Knowledge check

1. What is the difference between a new, borrowed and stolen reference? Give one C-API function for each.
2. Why was the hand-written C-API `sum_floats` over a `list` no faster than Python's built-in `sum`?
3. What does `py::gil_scoped_release` allow and forbid?
4. Why is a `std::vector<double>` parameter a performance trap, and what do you use instead?
5. What does `reference_internal` guarantee and what doesn't it protect against?
6. What is the buffer protocol and which three pieces of information does an exporter provide besides the pointer?
7. Why does a module built for Python 3.12 fail to load in 3.13, and what is `abi3`?
8. What does pybind11 do when a C++ function throws `std::invalid_argument`? What about a Python callback that raises?
9. Why should the C++ core library not include pybind11 headers?
10. When would you choose Cython over pybind11, and `ctypes`/`cffi` over both?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. New: you must release it (`PyList_New`, `PyObject_GetIter`, `PyFloat_FromDouble`). Borrowed: you don't own it (`PyList_GetItem`, `PyTuple_GetItem`). Stolen: the callee takes your reference (`PyList_SetItem`, `PyTuple_SetItem`).
2. The cost is dominated by iterating and unboxing 10⁶ heap-allocated `PyFloat` objects, which C code cannot avoid; the interpreter's `sum` is already C. Native code only wins when the data is in a native layout.
3. It lets other Python threads run while your C++ computes (parallelism); it forbids touching any `PyObject` or calling the Python C API, so inputs must be converted to C++ values first.
4. pybind11 builds the vector element by element (copy + allocation) on each call. Use `py::array_t<T>`, `py::buffer`, or a typed memoryview to read the caller's memory without copying.
5. It makes the returned object keep the parent alive, so the reference can't outlive its owner object. It does not protect against the *element* moving or being destroyed inside the parent (e.g. `vector` reallocation, erase).
6. The buffer protocol (PEP 3118) is how an object exposes raw memory to others. Besides the pointer: item format (`"d"`), shape (dimensions), strides (byte step per dimension); also item size and read-only flag.
7. The extension's file suffix (`.cpython-312-…so`) and its compiled-in C-API version are tied to the interpreter version; 3.13 doesn't search for 3.12-tagged files. `abi3` (PEP 384) is a subset of the C API stable across 3.x versions, producing one binary for multiple interpreters (tag `abi3`).
8. A registered translator maps it to a Python exception (`ValueError` for `invalid_argument`); a Python exception in a callback becomes `py::error_already_set`, propagates through C++ (destructors run) and is turned back into the same Python exception at the boundary.
9. So it stays testable and reusable from C++ and other languages, compiles faster, and doesn't couple the core to Python's lifetimes, GIL and ABI; the binding layer is a leaf.
10. Cython: optimising or typing Python code in place, numerical loops, thin wrappers where you don't have a C++ class library to expose. `ctypes`/`cffi`: wrapping an existing C-ABI library without a build step on the Python side, prototyping, or when you can't compile at install time.

</details>

---

[← Previous: Chapter 45](45-c-compatibility.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 47 — Modern C++ in Qt →](../part-18-qt/47-modern-cpp-in-qt.md)
