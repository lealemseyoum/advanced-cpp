# Project 11 — Python Extension

[← Project 10](project-10-async-http-server.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Capstone →](capstone.md)

> **Level 3 · ≈ 6 hours · Builds on:** Chapters 45 (C compatibility) and 46 (C++ and Python)
> **Reference solution:** [`code/p11/fastmath.cpp`](code/p11/fastmath.cpp), [`test_fastmath.py`](code/p11/test_fastmath.py), [`bench.py`](code/p11/bench.py), [`run.sh`](code/p11/run.sh)

## Goal

```text
Python → C++ binding → C++ core → native computation
```

Build a small extension module, `fastmath`, that a Python programmer can use without knowing it is C++, and that does not crash, copy needlessly, freeze other threads, or hand out dangling memory. The module is deliberately small; every function exists to force one decision about **ownership, copying, the GIL or exceptions**.

## Specification

Module `fastmath` (built with pybind11; the build is one `g++` command in [`run.sh`](code/p11/run.sh)):

| Name | Behaviour | Decision it forces |
|---|---|---|
| `moving_average(a, window)` | Trailing-window mean of a 1-D float array; returns a **new** array. Accepts lists, `int32` arrays, non-contiguous slices. Releases the GIL. `ValueError` for `window` out of range or non-1-D input. | Conversion vs copying; when it is safe to release the GIL |
| `scale_inplace(a, k)` | Multiplies a `float64`, C-contiguous array **in place**. Any other dtype or layout is a `TypeError`, **not** a silent copy. Read-only arrays raise. | `noconvert`: a mutator that works on a temporary copy is a bug |
| `Histogram(bins, lo, hi)` | Owns its counts. `add(values)` (GIL released, internally synchronised), `counts()` returns a NumPy array that **views** the histogram's memory, `outside`, `len()`. Invalid arguments raise `HistogramError`, a subclass of `ValueError`. | Ownership, keep-alive, thread-safety once the GIL is gone, exception translation |
| `map_py(f, a)` | Calls a Python function for every element from C++. | What the GIL does to "native speed" |

Rules:

1. **The C++ core (`namespace core`) contains no Python or pybind11 types.** It can be unit-tested, and reused, without an interpreter.
2. **No raw `PyObject*`.** All reference counting is done by pybind11 objects.
3. **Exceptions:** `core::HistogramError` is registered with a translator; other `std::` exceptions use pybind11's default mapping.
4. **A view must keep its owner alive.** `h.counts()` must stay valid after `del h`.
5. Tests are `unittest` (no third-party test runner needed), including a thread test and a lifetime test.

## Milestones

1. Build the minimal module (one function) with the exact flags in `run.sh`; `import` it.
2. `moving_average` with correct validation; compare with `np.convolve` on random data.
3. Release the GIL; prove two threads run in parallel.
4. `scale_inplace` with `noconvert`. Write the tests that show the silent-copy bug first, with a deliberately wrong version.
5. `Histogram`, with the view and its keep-alive; the lifetime test.
6. Exception translation.
7. `map_py`; benchmark; read the numbers.

## What the tests prove

```text
$ bash run.sh      # Python 3.12.3, pybind11 2.11.1, NumPy 1.26.4, GCC 13.3
test_concurrent_add_from_threads_is_correct ... ok
test_counts_and_outside ... ok
test_counts_is_a_live_view_not_a_copy ... ok
test_validation_error_is_a_value_error ... ok
test_view_keeps_histogram_alive ... ok
test_callback_and_exception_propagation ... ok
test_errors / test_matches_numpy / test_other_inputs_are_converted ... ok
test_runs_in_parallel_because_gil_is_released ...
  moving_average 20M: one call 79 ms, two threads 78 ms (serial would be ~157)
test_mutates_caller_array / test_readonly / test_wrong_dtype_is_rejected_not_silently_copied ... ok
Ran 13 tests ... OK
```

**Two threads, each calling `moving_average` on 20 million doubles, took the same time as one call (78 vs 79 ms).** That is parallelism, which requires two things: the GIL released *and* the raw pointers extracted *before* the release.

### Mutation test: what the `std::mutex` in `Histogram::add` is for

`add` runs with the GIL released, so two Python threads can be inside it at the same time, and the GIL no longer protects the histogram. I deleted the single `lock_guard` line and re-ran the thread test (4 threads × 20 calls × 200 000 values = 16 000 000 values expected):

```text
AssertionError: 12490917 != 16000000
AssertionError: 12350427 != 16000000
AssertionError: 14943291 != 16000000
```

About a quarter of the increments were lost, every time. **Releasing the GIL transfers the thread-safety responsibility from the interpreter to you.** Without the release, the unlocked code would have been correct by accident, which is also why this bug survives code review.

### A surprise worth keeping

My first test expected `TypeError` when the Python callback in `map_py` returned a string. The result was `RuntimeError: Unable to cast Python instance of type <class 'str'> to C++ type 'double'`. pybind11 raises **`TypeError` for *argument* conversion failures** (the overload-resolution path) but **`cast_error` → `RuntimeError` for a manual `.cast<T>()`**. The test now documents both. Decide which error your API should expose, and translate deliberately.

## Measurements

Python 3.12.3, NumPy 1.26.4, `-O2`, best of 5 (3 for the pure-Python loop), 2-vCPU VM:

```text
moving_average, 1M doubles, window 16
  pure Python loop         185.0 ms
  np.convolve               20.2 ms
  cumsum trick (numpy)      17.8 ms
  fastmath (C++)             1.55 ms

map a Python function over 100k elements
  [f(x) for x in list]       5.97 ms
  np.vectorize              13.9  ms
  fm.map_py                 10.0  ms
  numpy  x*2                 0.050 ms
```

- **The C++ version is 12× faster than `np.convolve` and 120× faster than pure Python**, because it is a single pass over contiguous memory with a running sum (`O(n)`), whereas `np.convolve` does `O(n × window)` work. Part of the win is the *algorithm*, not the language: the NumPy "cumsum trick" is also `O(n)` and is within 11× of the C++. Always compare against the best Python solution, not the naive one.
- **`map_py` is slower than a list comprehension.** Calling Python from C++ per element pays the interpreter's call cost *plus* conversion in both directions, with the GIL held. Native code cannot make a Python callback fast. If the per-element work is a Python function, keep the loop in Python; if it must be fast, implement the operation natively or express it in NumPy (0.05 ms here).
- **The binding itself is cheap, the boundary is not:** 1.55 ms for 1M elements is dominated by the loop, not by the call. A call that processes ten elements would be dominated by the dispatcher (see Chapter 46, measurements).

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| `scale_inplace` accepts `int64` and converts | The caller's array is untouched, the function "succeeds". Caught by `test_wrong_dtype_is_rejected_not_silently_copied` |
| GIL released but `a.shape(0)` or `a.data()` called afterwards | Races on the array object; crashes under thread stress |
| `counts()` returns `py::array_t(…, ptr)` with no base object | NumPy copies, or you get a dangling pointer after `del h` (`test_view_keeps_histogram_alive`) |
| `counts()` returns a copy | `test_counts_is_a_live_view_not_a_copy` fails; also costs `O(bins)` per call |
| No lock in `add` | Lost updates (measured above) |
| Exception type not registered | Python sees `RuntimeError` instead of your `ValueError` subclass |
| `forcecast` on a mutator | Silent copy (see above) |
| Built with another Python than the one you run | `ModuleNotFoundError` / `ImportError` (the filename encodes `cpython-312`) |

## Think about (and write answers)

1. `counts()` returns a view of memory that `add` may modify concurrently from another thread. What guarantees does NumPy give about reading it? What would a `snapshot()` method return, and what does it cost?
2. `Histogram::add` locks one mutex around the whole loop. What does this do to scaling when 8 threads call `add` on the same object? What would you do instead (per-thread local histograms merged at the end, atomic counters, sharded locks)?
3. What would happen if `counts()` returned `py::array_t<…>(shape, strides, ptr)` and you then called `h.reset()` that reallocated `counts_` (it never does here)? Why is "stable address" a **class invariant** of this design?
4. Why is `py::array_t<double, py::array::c_style | py::array::forcecast>` right for `moving_average` and wrong for `scale_inplace`?
5. The module is built with `-fvisibility=hidden`. What would go wrong when two extensions each embed their own pybind11 and export their symbols? (Chapter 46.)
6. Python 3.13 offers a free-threaded build (🟡 experimental). Which functions in this module would be unsafe to call concurrently without the GIL, and what would you change? (Be specific about `map_py`.)
7. How would you package this for `pip install`? What goes in `pyproject.toml`, and what tags does the wheel need?

## Stretch goals

- **A `pyproject.toml`** with `scikit-build-core` or `setuptools` + `pybind11`; build a wheel, install it into a clean virtual environment, and import it.
- **Type stubs** (`fastmath.pyi`) generated with `pybind11-stubgen`; type-check a caller with `mypy`.
- **`buffer_protocol`:** expose `Histogram` itself as a buffer (`py::buffer_protocol()` + `def_buffer`) so `np.asarray(h)` works without `counts()`.
- **`__getstate__`/`__setstate__`** for pickling, with `py::pickle`.
- **A trampoline class:** bind an abstract C++ interface (`Reducer`) so Python subclasses can override a virtual method called from C++ (`PYBIND11_OVERRIDE_PURE`), and see what happens when the override raises.
- **Free-threaded build** (Python 3.13t 🟡): declare the module GIL-free (`py::mod_gil_not_used()`) and run the thread stress test; fix what breaks.
- **The same module with the raw C API or with Cython** and compare lines of code, speed, and failure modes.
- **Fuzz the boundary:** call every function with random dtypes, shapes, empty arrays, `NaN`s, huge values, and views with negative strides.

## Reference solution notes

- **`py::gil_scoped_release` is in a nested block** in `moving_average` so that the return value (`out`, a Python object) is built *after* the GIL is re-acquired.
- **`counts()` takes `py::object self`**, not `Histogram&`, precisely so the view can name `self` as its base. That one change is the whole keep-alive mechanism.
- **`py::arg("a").noconvert()`** makes pybind11 skip its implicit conversions for that argument, so a wrong dtype fails overload resolution with a `TypeError` instead of copying.
- **`HistogramError`** is registered as a subclass of `ValueError`, so `except ValueError` callers keep working while callers who care can catch the specific class.
- **Left out:** pickling, stubs, packaging, free-threading, and cancellation of long computations (`PyErr_CheckSignals` inside a long loop, so Ctrl-C works).

Run it:

```bash
cd code/p11
bash run.sh                  # builds fastmath.cpython-312-x86_64-linux-gnu.so, runs tests and bench
PY=python3.12 bash run.sh    # choose the interpreter explicitly: the extension is tied to it
```

## What I learned (write it)

> Which of the four decisions (copy, ownership, GIL, exception type) did you get wrong the first time, and which test told you?
