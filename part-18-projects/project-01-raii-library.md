# Project 1 — RAII Resource Library

[← Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 2 — Vector →](project-02-custom-vector.md)

> **Level 2 · ≈ 4 hours · Builds on:** Chapters 3 (lifetime), 4 (RAII), 6 (moves), 22–23 (errors)
> **Reference solution:** [`code/p01/raii.hpp`](code/p01/raii.hpp), tests in [`code/p01/test.cpp`](code/p01/test.cpp)

## Goal

Build a small library of owning handle types for operating-system resources, with correct move semantics, no leaks on any path, and errors reported without throwing from destructors.

You will have written, by the end, the pattern that every other project in this part relies on: **a value that owns something, can be moved, and cleans up exactly once.**

## Specification

Implement these in a header `raii.hpp`, namespace `raii`:

| Type | Owns | Must support |
|---|---|---|
| `Unique<Traits>` | any handle with an "invalid" value | move-only, `get`, `release`, `reset`, `swap`, `explicit operator bool` |
| `Fd` | a POSIX file descriptor | built from `Unique`; `close` never throws |
| `File` | a `FILE*` | built from `Unique` |
| `Socket` | an `Fd` | `pair()` (a connected `socketpair`), `send`, `recv`, `shutdown_write` |
| `ScopeExit<F>` | a callable | runs on scope exit, `dismiss()` |
| `Guarded<T, Mutex>` | a `T` and the mutex that protects it | you cannot reach the `T` without holding the lock |

Rules:

1. **Open functions return `std::expected<Handle, std::error_code>`**. They must not throw and must not leak if a later step fails.
2. **Moved-from handles are valid and empty.** Destroying or assigning them is safe.
3. **Self-move-assignment is safe.**
4. **`recv`/`send` retry on `EINTR`** and report every other error as `std::error_code`.
5. **`Fd`'s destructor does not retry `close` on `EINTR`.** (On Linux the descriptor is released even when `close` is interrupted; retrying can close a descriptor that another thread just opened.) Write down this decision in a comment.
6. `Guarded<T>` offers `lock()` (returns a proxy that holds the lock) and `with(f)`.

## Milestones

1. `Unique<Traits>` with `FdTraits`. Test: open `/dev/null`, move it twice, count open descriptors via `/proc/self/fd` before and after.
2. `File` and `open_file`. Test the error path by opening a path that does not exist.
3. `Socket::pair`, `send`, `recv`; test that `shutdown_write` makes the peer's `recv` return an empty string (EOF).
4. `ScopeExit`; test it runs during stack unwinding and does not run after `dismiss()`.
5. `Guarded`; four threads, 100 000 increments each, under TSan.

## Tests your solution must pass

- **Descriptor accounting:** the number of entries in `/proc/self/fd` is the same before and after every test block (this catches leaks and double closes).
- **Error path:** `open_fd("/definitely/not/here")` yields `std::errc::no_such_file_or_directory`.
- **Move semantics:** `a` is empty after `Fd b = std::move(a)`.
- **Scope guard:** runs during exception propagation.
- **ASan + UBSan** clean, **TSan** clean for `Guarded`.

## Think about (and write answers)

1. Why is `Unique` a template on a *traits* type instead of taking a deleter function pointer as `std::unique_ptr` does? What does it cost, and what does it buy? (Hint: `sizeof`.)
2. `release()` is `[[nodiscard]]`. Why?
3. What should `~Socket` do if the peer has unsent data? Is `shutdown` part of destruction?
4. `Guarded::lock()` returns a proxy that holds the lock for the whole *statement*. What bug does that prevent, and what new one does it allow (hint: two `lock()` calls in one expression)?
5. Why can't a destructor report a `close` failure? What are your options? (Log, `terminate`, provide an explicit `close()` that returns `expected`.)

## Stretch goals

- Add `Fd::dup()` returning `expected<Fd, error_code>`; add `O_CLOEXEC` everywhere and explain the race it avoids in multi-threaded programs that `fork`.
- A `TempFile` that unlinks on destruction, with `dismiss()`/`keep()`.
- A `Guarded` variant for `std::shared_mutex` with `read()` and `write()` accessors.
- Make `Unique` support handle types where the invalid value is not representable by `==` comparison (e.g. a struct); use `std::optional`.

## Reference solution notes

Open [`code/p01/raii.hpp`](code/p01/raii.hpp) only after you have a working version. Compare:

- **Traits vs deleter member:** the traits approach keeps `sizeof(Fd) == sizeof(int)` because the close function is a compile-time constant. A deleter stored in the object would double it.
- **`reset` order:** the new handle is stored *before* the old one is closed, so an exception or reentrancy during `close` cannot see a half-updated object.
- **`Guarded::Access`** is a lock plus a pointer; the proxy lives until the end of the full expression, so `*counter.lock() += 1` is one critical section.

Run it:

```bash
cd code/p01
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan && ./test_asan
g++ -std=c++23 -Wall -Wextra -fsanitize=thread test.cpp -o test_tsan -pthread && ./test_tsan
```

Output of the reference solution on this VM (GCC 13.3):

```text
all tests passed        (ASan + UBSan)
all tests passed        (TSan)
```

## What I learned (write it)

> One paragraph: which decision in this project would you defend in a code review, and which one are you least sure about?
