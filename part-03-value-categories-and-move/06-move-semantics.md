# Chapter 6 — Move Semantics

> **Part III · Value categories and move semantics** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 4](../part-02-object-model-and-lifetime/04-raii.md), [Chapter 5](05-value-categories.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, `-S`, `-fno-elide-constructors`

[← Previous: Chapter 5](05-value-categories.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 7 — Perfect forwarding →](07-perfect-forwarding.md)

---

**In one sentence:** move semantics lets a type distinguish *"copy this, I still need the original"* from *"take this, I am finished with it"*, so that **ownership of a resource can be transferred in O(1)** instead of duplicated.

**By the end of this chapter you can:**

- write a correct move constructor, move assignment and the rest of the rule of five, and know when to write none of them
- explain exactly what `std::move`, `std::exchange` and `std::swap` do and cost
- state what you may and may not do with a moved-from object, for your own types and the standard's
- explain why `noexcept` on a move constructor changes what `std::vector` does when it grows
- decide between *implicit move*, *NRVO*, *guaranteed elision* and an explicit `std::move`, and say why `return std::move(x);` is usually a bug
- read assembly to see what a move really costs

---

## 1. Problem

A type that owns a resource (heap memory, a file descriptor, a lock) is expensive or *impossible* to copy. Yet C++ value semantics wants to pass such types around by value, return them from functions and store them in containers.

```cpp
std::vector<std::string> make_names();            // returns thousands of strings
auto names = make_names();                        // before C++11: copy every string. Or…
void make_names(std::vector<std::string>& out);   // …the "out parameter" workaround
```

Three costs appear:

1. **Performance**: copying heap data that is about to be destroyed anyway.
2. **Expressiveness**: out-parameters, `swap` tricks and pointer returns distort interfaces.
3. **Impossibility**: some resources *cannot* be copied (a file descriptor, a `std::mutex`, a unique owner). Pre-C++11 there was no way to return one by value or put one in a container.

---

## 2. Historical context

| Pre-C++11 workaround | Limitation |
|---|---|
| Return a pointer (`new T`) | Ownership is invisible; leaks; allocation for every call |
| Out-parameter `void f(T& out)` | Awkward call sites; cannot chain; requires a default-constructed `T` |
| `std::auto_ptr` | Its *copy constructor moved*. `auto_ptr<T> b = a;` silently emptied `a`. Unsafe in containers and algorithms; **removed in C++17** |
| `swap`-based tricks (`vector<T>().swap(v)`), copy-on-write strings | Manual, not composable, COW cost atomics |
| Return-value optimization (RVO) | Compiler optimization, *not guaranteed* before C++17, and only for returns, not for passing into containers |

`auto_ptr` is the cautionary tale: the committee needed *transfer* semantics, but the language had no way to say "this source is expiring", so the copy operations were hijacked. Rvalue references (N1377, 2002 → C++11) solved the real problem: **make "expiring" part of the type system**, so overload resolution selects the right operation automatically (that is [Chapter 5](05-value-categories.md)).

---

## 3. Modern solution

Two new special member functions, plus the library machinery around them:

```cpp
T(T&& other) noexcept;              // move constructor
T& operator=(T&& other) noexcept;   // move assignment
```

- `std::move(x)`: *cast* to an xvalue ("I am finished with `x`")
- `std::exchange(obj, new_value)`: *replace and return the old value*, the idiom for writing move operations
- `std::move_if_noexcept`, `std::move_iterator`: move when it is safe / move in algorithms
- **Implicit move on `return`**, **guaranteed copy elision (C++17)**, **NRVO**: the compiler avoids the work entirely when it can
- **Rule of zero / rule of five**: how to organize the special members

---

## 4. Mental model

### A move is a *transfer of ownership* implemented as a cheap copy plus a reset of the source

```text
 before:    src ──► [ ptr ─────────────┐ ]            dst  [ ptr = null ]
                                       ▼
                            heap block (the resource)

 move:      dst = take(src)
            1. dst.ptr = src.ptr      (copy the handle: a few words)
            2. src.ptr = nullptr      (so src's destructor will not free the block)

 after:     src [ ptr = null ]        dst ──► [ ptr ─────┐ ]
                                                          ▼
                                               the same heap block
```

A move is **not** magic and does **not** do anything for types without a heap resource. Moving an `int`, a `std::array<int, 1000>` or a `struct Point {double x, y;}` is exactly a copy. Moving only wins when the type holds a *handle* to something bigger.

### The contract in one box

> After `T b = std::move(a);`, **`a` must be valid but unspecified**: it can be destroyed, assigned to, and (for most standard types) otherwise used with no preconditions. You must not *depend* on its value. The same holds for *your* types: **a moved-from object must remain safely destructible and assignable.**

*Valid but unspecified* is the standard's phrase for the library types (`[lib.types.movedfrom]`). A handful of types give **stronger** guarantees: a moved-from `unique_ptr` and `shared_ptr` is **empty (null)**. A moved-from `std::string` or `vector` is *in practice* empty on every mainstream implementation, but the **standard does not promise it**: treat it as valid but unspecified.

### A habit that prevents most bugs

> `std::move(x)` means **"`x` is dead to me."** Write it only at the **last use** of `x`, and do not read `x` afterward unless you first assign it a new value.

### Why `noexcept` matters

Containers promise the **strong exception guarantee** for `push_back`: if reallocation fails halfway, the old contents must be intact. If your move constructor can throw, then a partially moved vector cannot be rolled back (the moved-from originals are already damaged). So `std::vector` uses **`std::move_if_noexcept`**: *if the move constructor is `noexcept` it moves, otherwise it **copies*** (when the type is copyable). A move constructor that is not `noexcept` therefore silently turns a growth of N elements from N cheap moves into N expensive copies. Experiment 2 measures this.

---

## 5. Language rules

### 5.1 When do you get which special members?  `[class.copy.ctor]`, `[class.copy.assign]`

The compiler implicitly declares each special member under rules that depend on what *you* declared:

| You declared… | Copy ctor | Copy assign | Move ctor | Move assign | Dtor |
|---|---|---|---|---|---|
| nothing | ✅ generated | ✅ | ✅ | ✅ | ✅ |
| a **destructor** | ✅ (deprecated) | ✅ (deprecated) | ❌ *not declared* | ❌ | yours |
| a **copy ctor** or **copy assign** | yours / ✅ (deprecated) | ✅ / yours | ❌ not declared | ❌ | ✅ |
| a **move ctor** or **move assign** | 🚫 `= delete`d implicitly | 🚫 deleted | yours | ❌ / ✅ | ✅ |

Read the table's **Move** columns as: *as soon as you write a destructor or a copy operation, the compiler stops generating moves, and "move" requests fall back to copies.* Silent. That is the most common way to accidentally make a type slow.

### 5.2 The three rules

| Rule | Statement | Verdict |
|---|---|---|
| **Rule of zero** | A class that does not directly own a resource declares **none** of the five special members. Compose from members that already manage themselves (`vector`, `unique_ptr`, `string`). | **Default to this.** 90% of classes. |
| **Rule of five** | If you declare **any** of destructor, copy ctor, copy assign, move ctor, move assign, you almost certainly need to consider **all five** (declare, `= default` or `= delete` each). | For classes that own a resource directly: *wrappers*, not business types. |
| **Rule of three** (C++98) | Dtor + copy ctor + copy assign | Obsolete as stated; the rule of five replaces it |

> **Opinion.** If you find yourself writing a destructor in a *business* class, stop: wrap the resource in a small RAII type (Chapter 4) and let the business class go back to the rule of zero. Hand-written special members belong in a handful of low-level types per codebase.

### 5.3 What `std::move` is  `[forward]`

```cpp
template <class T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}
```

That is the whole function. It generates **no code**; it is a cast.

### 5.4 Moved-from state of the standard types

| Type | After being moved from | Guaranteed by the standard? |
|---|---|---|
| `unique_ptr`, `shared_ptr`, `weak_ptr` | empty / null | **Yes** |
| `optional<T>` | **still engaged** (its contained `T` is moved-from) | Yes: moving does not disengage it |
| `variant` | still holds the same alternative, moved-from | Yes |
| `string`, `vector`, `map`, … | valid but unspecified (empty in practice) | **No**: don't rely on emptiness; call `.clear()` if you need it |
| `thread`, `jthread`, `unique_lock`, `fstream` | not joinable / unlocked / closed | Yes |
| built-in types, trivially copyable types | unchanged (a move *is* a copy) | Yes |
| `array<T,N>` | each element moved-from | Yes |

### 5.5 Implicit move on return  `[class.copy.elision]`, `[stmt.return]`

In a `return` statement whose operand is the **name of a local object or parameter**, overload resolution is first done as if the name were an **rvalue**. If that finds a move constructor (it need not be perfect; this was widened by P1825 in C++20 and again by P2266 in C++23, which also covers `T&&` parameters), the object is **moved**; otherwise it is copied.

```cpp
std::string f() { std::string s = "..."; return s; }     // implicit move (or NRVO)
std::string g() { std::string s = "..."; return std::move(s); }   // WORSE: see below
```

### 5.6 Copy elision, NRVO, guaranteed elision

| Mechanism | Applies to | Guaranteed? | What it means |
|---|---|---|---|
| **Guaranteed elision** (C++17) | Initializing an object from a **prvalue of the same type**: `T x = T();`, `T x = f();`, `return T(...)` | **Yes**, mandatory | No temporary exists at all. The destination object is initialized directly. The copy/move constructor need not even exist |
| **NRVO** (named return value optimization) | `return local;` where `local` is a named automatic object of the return type | **No**, permitted not required | The local *is* the return object: it is constructed straight into the caller's storage |
| **Implicit move** | `return local;` when NRVO is not applied | Yes (the *rule*) | The return is treated as an xvalue → move constructor |
| Elision of a **throw**/catch copy, coroutine parameter copies | niche | permitted | |

Observable side effects (the copy/move constructor printing something) are allowed to disappear. That is the one place where the standard *explicitly* lets an optimization change observable behaviour.

> [!IMPORTANT]
> The *shape* of a function decides whether NRVO is possible: if different `return` statements return **different** named locals, or the returned name might be a parameter or a member, most compilers cannot construct all of them in the return slot, and fall back to the implicit move. Both are cheap if your type has a good move constructor, so **a good move constructor is the safety net under every elision.**

### 5.7 `noexcept` and the move operations

- A move operation that can't throw should say so: `noexcept`. The compiler-generated one is `noexcept` iff all members' are.
- `std::is_nothrow_move_constructible_v<T>` is what `move_if_noexcept` and `std::vector` consult.
- Move **assignment** being `noexcept` additionally enables more efficient container operations (e.g. `vector::erase`/`insert` shifting).
- **Destructors** are implicitly `noexcept`.

### 5.8 Self-move-assignment

`a = std::move(a);` is valid, if rare (it appears via `std::swap`-like generic code, `std::ranges::remove`, `x = std::move(arr[i])` with aliasing indices). The standard says the result for library types is *valid but unspecified*; **your own `operator=(T&&)` must not corrupt the object**, so don't blindly `delete[] data_; data_ = other.data_; other.data_ = nullptr;`. See Experiment 5.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Move constructors/assignments, `std::move`, valid-but-unspecified, `noexcept` interactions, elision rules |
| **Compiler** | Whether NRVO is applied (it is *optional*); inlining of the moves; whether a moved-from member is zeroed (dead-store elimination can drop it) |
| **ABI** | Itanium: a **non-trivially-copyable** type is passed and returned **in memory** (hidden pointer), not in registers, which is why *elision and NRVO are cheap*: the return slot is already the caller's storage |
| **CPU** | A move of `string` is a handful of loads/stores; a copy involves `malloc` and `memcpy` |

---

## 6. Implementation model

### What the compiler generates for `std::string s2 = std::move(s1);` (libstdc++)

libstdc++'s `std::string` has a **16-byte SSO buffer** (`_M_local_buf`). A move must check which mode it is in:

```text
 long (heap) mode:  steal the pointer and capacity; leave the source empty with its SSO buffer
 short (SSO) mode:  the bytes live *inside* the object: they must be COPIED (up to 16 bytes), then the source length set to 0
```

So a "move" of a short string is *not* O(1) pointer theft. It's a small memcpy. The standard doesn't care; it's an implementation detail. Experiment 4 shows both paths in assembly.

### What the Itanium ABI does for returning a class by value

If a class has a **non-trivial copy constructor or destructor**, the Itanium ABI says it is returned via a **hidden first pointer argument** (`sret`) to caller-allocated storage. The callee constructs the result *in place*. That is what makes **NRVO nearly free**: `Tr t("local"); return t;` constructs `t` at `*sret` in the first place. Under the same ABI, `return std::move(t)` has `t` as a separate local: it must then be moved into `*sret`.

---

## 7. Experiments

### Experiment 1: Counting what really happens

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <utility>
#include <vector>

struct Loud {
    int id;
    explicit Loud(int i) : id(i)            { std::printf("  ctor(%d)\n", id); }
    Loud(const Loud& o) : id(o.id)          { std::printf("  COPY(%d)\n", id); }
    Loud(Loud&& o) noexcept : id(o.id)      { std::printf("  move(%d)\n", id); o.id = -1; }
    ~Loud()                                 { std::printf("  dtor(%d)\n", id); }
};

Loud by_value()      { return Loud(1); }               // guaranteed elision
Loud nrvo()          { Loud l(2); return l; }          // NRVO (optional) / else implicit move
Loud pessimized()    { Loud l(3); return std::move(l); }

int main() {
    std::puts("[guaranteed elision]");   { Loud a = by_value(); }
    std::puts("[NRVO]");                 { Loud a = nrvo(); }
    std::puts("[return std::move(local)]"); { Loud a = pessimized(); }
    std::puts("[vector push_back]");
    {
        std::vector<Loud> v;
        v.reserve(4);
        Loud x(4);
        v.push_back(x);                  // copy: lvalue
        v.push_back(std::move(x));       // move: xvalue
        v.push_back(Loud(5));            // move: prvalue materialized
        v.emplace_back(6);               // construct in place: no copy, no move
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
[guaranteed elision]
  ctor(1)
  dtor(1)
[NRVO]
  ctor(2)
  dtor(2)
[return std::move(local)]
  ctor(3)
  move(3)
  dtor(-1)
  dtor(3)
[vector push_back]
  ctor(4)
  COPY(4)
  move(4)
  ctor(5)
  move(5)
  dtor(-1)
  ctor(6)
  dtor(-1)
  dtor(4)
  dtor(4)
  dtor(5)
  dtor(6)
```

Reading it:

- `by_value()`: **one `ctor`, zero copies, zero moves**. A language guarantee since C++17.
- `nrvo()`: the same result, but only because both GCC and Clang *choose* to apply NRVO (even at `-O0`). The standard does not require it.
- `pessimized()`: a `move` and an extra `dtor`. `return std::move(l)` makes the operand an xvalue *expression*, which is not a plain name, so **NRVO is not permitted**; you pay for a move and a destructor that `return l;` would have avoided. Clang and GCC both warn: `-Wpessimizing-move` (in `-Wall`).
- `emplace_back(6)` constructs the element directly in the vector's storage: no temporary, no move. That is perfect forwarding ([Chapter 7](07-perfect-forwarding.md)).

### Experiment 2: `noexcept` and `vector` growth

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

struct A {                                   // move constructor is noexcept
    A() = default;
    A(const A&)     { ++copies; }
    A(A&&) noexcept { ++moves; }
    static inline int copies = 0, moves = 0;
};
struct B {                                   // identical, but the move can "throw"
    B() = default;
    B(const B&)     { ++copies; }
    B(B&&)          { ++moves; }
    static inline int copies = 0, moves = 0;
};
struct C {                                   // move-only and not noexcept
    C() = default;
    C(C&&) { ++moves; }
    static inline int moves = 0;
};

template <class T>
void grow(const char* name) {
    std::vector<T> v;
    for (int i = 0; i < 100; ++i) v.emplace_back();     // 7 reallocations: 1→2→4→…→128
    std::printf("  %-28s copies=%-4d moves=%d\n", name, T::copies, T::moves);
}

int main() {
    std::puts("growing a vector to 100 elements:");
    grow<A>("A: noexcept move");
    grow<B>("B: move NOT noexcept");

    std::vector<C> vc;
    for (int i = 0; i < 100; ++i) vc.emplace_back();
    std::printf("  %-28s copies=n/a  moves=%d\n", "C: move-only, not noexcept", C::moves);

    std::printf("is_nothrow_move_constructible: A=%d B=%d C=%d\n",
                std::is_nothrow_move_constructible_v<A>,
                std::is_nothrow_move_constructible_v<B>,
                std::is_nothrow_move_constructible_v<C>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
growing a vector to 100 elements:
  A: noexcept move             copies=0    moves=127
  B: move NOT noexcept         copies=127  moves=0
  C: move-only, not noexcept   copies=n/a  moves=127
is_nothrow_move_constructible: A=1 B=0 C=0
```

`A` moves every element on every growth. `B`, **identical except for the missing `noexcept`**, gets **copied** every time: copies instead of moves, for no visible reason. `C` has no copy constructor, so the library has *no* safe alternative and moves anyway, giving up the strong guarantee.

> [!WARNING]
> **A missing `noexcept` on a move constructor is a performance bug that no test will catch.** For a type that contains a `std::string`, the difference is thousands of `malloc` calls. In code review: every user-written move constructor and move assignment should be `noexcept`, and a `static_assert(std::is_nothrow_move_constructible_v<T>)` next to the class definition is a cheap insurance policy.

### Experiment 3: Moved-from objects

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

int main() {
    std::string long_s(40, 'x');          // too long for SSO → heap
    std::string short_s = "short";        // fits in the SSO buffer
    std::string a = std::move(long_s);
    std::string b = std::move(short_s);
    std::printf("string  long  moved-from: size=%zu\n", long_s.size());
    std::printf("string  short moved-from: size=%zu\n", short_s.size());

    std::vector<int> v = {1, 2, 3};
    std::vector<int> w = std::move(v);
    std::printf("vector  moved-from: size=%zu (empty() = %d)\n", v.size(), v.empty());

    auto p = std::make_unique<int>(7);
    auto q = std::move(p);
    std::printf("unique_ptr moved-from == nullptr: %d  (GUARANTEED)\n", p == nullptr);

    std::optional<std::string> o = "text";
    auto o2 = std::move(o);
    std::printf("optional moved-from: has_value=%d (still engaged!), value size=%zu\n",
                o.has_value(), o->size());

    int i = 5;
    int j = std::move(i);
    std::printf("int moved-from: %d (a move IS a copy)\n", i);

    // the safe re-use pattern: re-assign (or clear) before reading again
    long_s = "reused";
    std::printf("after assignment: '%s'\n", long_s.c_str());
    (void)a; (void)b; (void)w; (void)q; (void)o2; (void)j;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
string  long  moved-from: size=0
string  short moved-from: size=0
vector  moved-from: size=0 (empty() = 1)
unique_ptr moved-from == nullptr: 1  (GUARANTEED)
optional moved-from: has_value=1 (still engaged!), value size=0
int moved-from: 5 (a move IS a copy)
after assignment: 'reused'
```

On libstdc++ the strings and vector all report size 0, **but this is an implementation property**. Programs that branch on it are non-portable (the standard says *valid but unspecified*). The optional line surprises people: **moving from an `optional` does not make it empty**; only the *contained value* is moved-from.

### Experiment 4: What a move costs: assembly

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=move_vec,move_ptr,copy_vec
#include <memory>
#include <vector>

struct V { std::vector<int> v; };
struct P { std::unique_ptr<int> p; };

V move_vec(V&& other)       { return V(std::move(other)); }
P move_ptr(P&& other)       { return P(std::move(other)); }
V copy_vec(const V& other)  { return V(other); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
move_vec(V&&):
	movdqu	xmm0, XMMWORD PTR [rsi]
	mov	rdx, QWORD PTR 16[rsi]
	mov	rax, rdi
	mov	QWORD PTR 16[rsi], 0
	movups	XMMWORD PTR [rdi], xmm0
	pxor	xmm0, xmm0
	mov	QWORD PTR 16[rdi], rdx
	movups	XMMWORD PTR [rsi], xmm0
	ret

move_ptr(P&&):
	mov	rdx, QWORD PTR [rsi]
	mov	rax, rdi
	mov	QWORD PTR [rdi], rdx
	mov	QWORD PTR [rsi], 0
	ret

copy_vec(V const&):
	push	r12
	pxor	xmm0, xmm0
	mov	r12, rsi
	push	rbp
	push	rbx
	mov	rbp, QWORD PTR 8[rsi]
	mov	rbx, rdi
	sub	rbp, QWORD PTR [rsi]
	mov	QWORD PTR 16[rdi], 0
	movups	XMMWORD PTR [rdi], xmm0
	je	.L9
	movabs	rax, 9223372036854775804
	cmp	rax, rbp
	jb	.L11
	mov	rdi, rbp
	call	operator new(unsigned long)@PLT
	mov	rcx, rax
.L5:
	movq	xmm0, rcx
	add	rbp, rcx
	punpcklqdq	xmm0, xmm0
	mov	QWORD PTR 16[rbx], rbp
	movups	XMMWORD PTR [rbx], xmm0
	mov	rsi, QWORD PTR [r12]
	mov	rbp, QWORD PTR 8[r12]
	sub	rbp, rsi
	cmp	rbp, 4
	jle	.L7
	mov	rdi, rcx
	mov	rdx, rbp
	call	memmove@PLT
	mov	rcx, rax
.L8:
	add	rcx, rbp
	mov	rax, rbx
	mov	QWORD PTR 8[rbx], rcx
	pop	rbx
	pop	rbp
	pop	r12
	ret
.L9:
	xor	ecx, ecx
	jmp	.L5
.L7:
	jne	.L8
	mov	eax, DWORD PTR [rsi]
	mov	DWORD PTR [rcx], eax
	jmp	.L8
.L11:
	call	std::__throw_bad_array_new_length()@PLT
```

- `move_ptr`: a load, a store of `0` into the source, a store into the result: **three instructions**.
- `move_vec`: three loads, three stores into the result, three zero stores into the source: **about a dozen instructions, no call, no branch**. This is the whole cost of moving a million-element vector.
- `copy_vec`: allocation (`operator new`), `memmove`, size arithmetic, **and** an exception landing pad. Look at how much more code it is, then remember it also runs `malloc` at runtime.

The zero-stores into the *source* are what make the moved-from object safe to destroy. If the source is a dead temporary that is never read again, the optimizer *removes* them (dead-store elimination). That is one reason moving from a temporary is cheaper than moving from `std::move(named)`.

Now what happens to a `std::string`: the SSO branch from §6:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=string_move
#include <string>

std::string string_move(std::string&& s) { return std::string(std::move(s)); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
string_move(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&):
	lea	rcx, 16[rdi]
	mov	rax, rsi
	lea	rsi, 16[rsi]
	mov	QWORD PTR [rdi], rcx
	mov	rdx, QWORD PTR -16[rsi]
	cmp	rdx, rsi
	je	.L18
	mov	QWORD PTR [rdi], rdx
	mov	rdx, QWORD PTR 16[rax]
	mov	QWORD PTR 16[rdi], rdx
.L16:
	mov	rdx, QWORD PTR 8[rax]
.L9:
	mov	QWORD PTR 8[rdi], rdx
	mov	QWORD PTR [rax], rsi
	mov	QWORD PTR 8[rax], 0
	mov	BYTE PTR 16[rax], 0
	mov	rax, rdi
	ret
.L18:
	mov	rdx, QWORD PTR 8[rax]
	lea	r8, 1[rdx]
	cmp	r8d, 8
	jnb	.L3
	test	r8b, 4
	jne	.L19
	test	r8d, r8d
	je	.L9
	movzx	edx, BYTE PTR 16[rax]
	mov	BYTE PTR 16[rdi], dl
	test	r8b, 2
	je	.L16
	mov	r8d, r8d
	movzx	edx, WORD PTR -2[rsi+r8]
	mov	WORD PTR -2[rcx+r8], dx
	mov	rdx, QWORD PTR 8[rax]
	jmp	.L9
.L3:
	mov	rdx, QWORD PTR 16[rax]
	mov	r10, rsi
	mov	QWORD PTR 16[rdi], rdx
	mov	edx, r8d
	mov	r9, QWORD PTR -8[rsi+rdx]
	mov	QWORD PTR -8[rcx+rdx], r9
	lea	rdx, 24[rdi]
	and	rdx, -8
	sub	rcx, rdx
	add	r8d, ecx
	sub	r10, rcx
	and	r8d, -8
	cmp	r8d, 8
	jb	.L16
	and	r8d, -8
	xor	ecx, ecx
.L7:
	mov	r9d, ecx
	add	ecx, 8
	mov	r11, QWORD PTR [r10+r9]
	mov	QWORD PTR [rdx+r9], r11
	cmp	ecx, r8d
	jb	.L7
	jmp	.L16
.L19:
	mov	edx, DWORD PTR 16[rax]
	mov	r8d, r8d
	mov	DWORD PTR 16[rdi], edx
	mov	edx, DWORD PTR -4[rsi+r8]
	mov	DWORD PTR -4[rcx+r8], edx
	mov	rdx, QWORD PTR 8[rax]
... (truncated)
```

Look for the **compare of the data pointer against the address of the local buffer**: that is `_M_is_local()`. If equal, the 16 inline bytes are copied; if not, the pointer is stolen. The compiler cannot avoid the branch because it does not know which mode the string is in.

### Experiment 5: A hand-written resource type, with three subtle bugs avoided

```cpp
// @test run -std=c++23 -O0 -fsanitize=address,undefined
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>

class IntBuf {
    int*        data_ = nullptr;
    std::size_t size_ = 0;
public:
    IntBuf() = default;
    explicit IntBuf(std::size_t n) : data_(n ? new int[n]() : nullptr), size_(n) {}

    ~IntBuf() { delete[] data_; }

    // copy: allocate first, then commit (strong guarantee)
    IntBuf(const IntBuf& o) : data_(o.size_ ? new int[o.size_] : nullptr), size_(o.size_) {
        std::copy_n(o.data_, size_, data_);
    }
    IntBuf& operator=(const IntBuf& o) {
        if (this != &o) { IntBuf tmp(o); swap(tmp); }       // copy-and-swap, strong guarantee
        return *this;
    }

    // move: noexcept, and the source is left valid and empty
    IntBuf(IntBuf&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

    IntBuf& operator=(IntBuf&& o) noexcept {
        if (this != &o) {                                   // guard: a = std::move(a)
            delete[] data_;
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    void swap(IntBuf& o) noexcept { std::swap(data_, o.data_); std::swap(size_, o.size_); }
    friend void swap(IntBuf& a, IntBuf& b) noexcept { a.swap(b); }

    std::size_t size() const { return size_; }
    int&        operator[](std::size_t i) { return data_[i]; }
};

static_assert(std::is_nothrow_move_constructible_v<IntBuf>);
static_assert(std::is_nothrow_move_assignable_v<IntBuf>);

int main() {
    IntBuf a(4); a[0] = 42;
    IntBuf b = std::move(a);                       // move construct
    std::printf("a.size()=%zu b.size()=%zu b[0]=%d\n", a.size(), b.size(), b[0]);

    IntBuf c(2);
    c = std::move(b);                              // move assign: frees c's old block
    std::printf("b.size()=%zu c.size()=%zu\n", b.size(), c.size());

    c = std::move(c);                              // self-move: must not corrupt
    std::printf("after self-move: c.size()=%zu c[0]=%d\n", c.size(), c[0]);

    IntBuf d = c;                                  // copy
    d[0] = 7;
    std::printf("copy is independent: c[0]=%d d[0]=%d\n", c[0], d[0]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a.size()=0 b.size()=4 b[0]=42
b.size()=0 c.size()=4
after self-move: c.size()=4 c[0]=42
copy is independent: c[0]=42 d[0]=7
```

Three bugs this version avoids, each of which appears in real code:

1. **Forgetting to null the source.** `data_ = o.data_;` alone gives a *double free*: both destructors call `delete[]`. `std::exchange(o.data_, nullptr)` does read-and-reset in one expression.
2. **Self-move.** Without the `this != &o` guard, `delete[] data_; data_ = o.data_` frees the block and then reads from the freed object.
3. **Throwing moves.** All of the moves are `noexcept`, and asserted so.

> **Verdict.** Write a type like this *once*, as a tiny single-purpose owner (it is `std::unique_ptr<int[]>` plus a size), and put the business logic in a rule-of-zero class that *contains* it. In real code, you'd use `std::vector<int>`.

### Experiment 6: The silent move disabler

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <utility>

struct Cat {
    static inline int copies = 0, moves = 0;
    Cat() = default;
    Cat(const Cat&) { ++copies; }
    Cat(Cat&&) noexcept { ++moves; }
};

struct Rule0 { Cat c; };                                      // nothing declared

struct HasDtor {                                              // user-declared destructor:
    Cat c;                                                    // implicit move is NOT declared
    ~HasDtor() {}
};

struct HasDtorFixed {                                         // restore the moves explicitly
    Cat c;
    ~HasDtorFixed() {}
    HasDtorFixed() = default;
    HasDtorFixed(const HasDtorFixed&) = default;
    HasDtorFixed(HasDtorFixed&&) = default;
};

template <class T>
void test(const char* name) {
    Cat::copies = Cat::moves = 0;
    T a;
    T b = std::move(a);
    std::printf("  %-14s copies=%d moves=%d\n", name, Cat::copies, Cat::moves);
}

int main() {
    test<Rule0>("Rule0");
    test<HasDtor>("HasDtor");
    test<HasDtorFixed>("HasDtorFixed");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  Rule0          copies=0 moves=1
  HasDtor        copies=1 moves=0
  HasDtorFixed   copies=0 moves=1
```

`HasDtor` asked to move and **was copied**. There is no diagnostic by default (`clang-tidy`'s `performance-noexcept-move-constructor` and `-Wdeprecated-copy-with-dtor` catch parts of this). If you must declare a destructor (say, `virtual ~Base() = default;`), **restate the moves** or ask yourself why the class isn't rule-of-zero.

---

## 8. Assembly / runtime investigation

The three tools for investigating move behaviour on your own types:

```bash
# 1. Disable elision to see what the language semantically does (C++14 and earlier semantics):
g++-14 -std=c++17 -fno-elide-constructors prog.cpp   # NOTE: C++17 guaranteed elision still applies
g++-14 -std=c++14 -fno-elide-constructors prog.cpp   # full pre-17 behaviour: temporaries, extra moves

# 2. Catch the self-inflicted wounds:
g++-14 -Wall -Wextra -Wpessimizing-move -Wredundant-move -Wdeprecated-copy-with-dtor prog.cpp

# 3. Look for calls the optimizer could not remove:
g++-14 -O2 -S -masm=intel prog.cpp -o - | c++filt | grep -E 'call|_Znwm|memcpy|memmove'
```

A useful check in a build: a test that counts allocations during a hot path (replace global `operator new` with a counter, or use `ltrace -e malloc` / `valgrind --tool=massif`) and fails if a "move" allocates.

---

## 9. Implementation exercise

Implement `UniquePtr<T>` (single-object, with a default deleter only), complete with:

- a constructor from `T*`, destructor, `get()`, `release()`, `reset(T* = nullptr)`, `operator*`, `operator->`, `explicit operator bool`
- move constructor and move assignment (`noexcept`), **copy deleted**
- a converting move constructor from `UniquePtr<U>` where `U*` converts to `T*` (use `requires std::convertible_to<U*, T*>`)
- a `make_unique`-like factory

Then answer: *why is the copy deleted rather than just absent?* (Because of §5.1: declaring a move makes copy implicitly deleted, but being explicit improves diagnostics.)

Test with the instrumentation from Experiment 1.

<details>
<summary><strong>Solution</strong></summary>

```cpp
// @test run -std=c++23 -O0 -fsanitize=address,undefined
#include <concepts>
#include <cstdio>
#include <utility>

template <class T>
class UniquePtr {
    T* p_ = nullptr;
    template <class U> friend class UniquePtr;
public:
    constexpr UniquePtr() noexcept = default;
    constexpr explicit UniquePtr(T* p) noexcept : p_(p) {}
    ~UniquePtr() { delete p_; }

    UniquePtr(const UniquePtr&)            = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    constexpr UniquePtr(UniquePtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

    template <class U> requires std::convertible_to<U*, T*>
    constexpr UniquePtr(UniquePtr<U>&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

    UniquePtr& operator=(UniquePtr&& o) noexcept {
        reset(std::exchange(o.p_, nullptr));       // reset handles self-move: see note
        return *this;
    }

    constexpr T*   get() const noexcept          { return p_; }
    constexpr T*   release() noexcept            { return std::exchange(p_, nullptr); }
    void reset(T* p = nullptr) noexcept          { delete std::exchange(p_, p); }
    constexpr T&   operator*()  const noexcept   { return *p_; }
    constexpr T*   operator->() const noexcept   { return p_; }
    constexpr explicit operator bool() const noexcept { return p_ != nullptr; }
};

template <class T, class... Args>
UniquePtr<T> make_unique_ptr(Args&&... args) {
    return UniquePtr<T>(new T(std::forward<Args>(args)...));
}

struct Base { virtual ~Base() { std::puts("  ~Base"); } };
struct Derived : Base { ~Derived() override { std::puts("  ~Derived"); } };

int main() {
    auto a = make_unique_ptr<int>(5);
    auto b = std::move(a);
    std::printf("a=%d b=%d *b=%d\n", bool(a), bool(b), *b);

    UniquePtr<Base> base = make_unique_ptr<Derived>();   // converting move
    std::puts("end of main:");
    b = std::move(b);                                    // self-move is safe here
    std::printf("b after self-move: %d\n", bool(b));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a=0 b=1 *b=5
end of main:
b after self-move: 1
  ~Derived
  ~Base
```

*Self-move in `operator=`*: when `o` is `*this`, `std::exchange(o.p_, nullptr)` returns the old pointer and leaves `p_` null. `reset(old)` then runs `delete std::exchange(p_, old)`: it deletes the *null* and stores `old` back. The object is **unchanged**, so self-move is safe *by construction*, with no branch. Compare with Experiment 5's explicit guard.

</details>

---

## 10. Real-world example

| Where | What move semantics buys |
|---|---|
| **`std::vector<std::string>` growth** | Reallocation moves strings (3 pointer-sized words each) instead of copying heap data |
| **Returning big objects from factories** | `std::vector<Particle> load(...)`: no out-parameter needed |
| **`std::unique_ptr` in containers**, factories returning `unique_ptr<Base>` | Ownership transfers explicitly, visible in the type |
| **`std::thread`, `std::jthread`, `std::future`, `std::promise`** | Move-only: a thread handle *cannot* be copied. Impossible without move semantics |
| **Task queues** | `std::move_only_function` (C++23) and moving tasks into a queue instead of copying their captures |
| **Qt** | `QString`/`QVector` use *implicit sharing* (copy-on-write, reference-counted) to make copies cheap; since Qt 6 they also have move constructors. Moves are cheaper still: no atomic reference-count traffic |

A concrete rule for APIs (cross-reference [Chapter 43](../part-17-library-design/43-api-design.md)):

| Parameter you *store* | Signature | Cost |
|---|---|---|
| a cheap-to-move type (`string`, `vector`) | **by value, then `std::move` into the member** | 1 move for rvalues, 1 copy + 1 move for lvalues. Simple. **Default choice.** |
| the same, hot path | `const T&` + `T&&` overloads | 1 copy or 1 move; twice the code |
| arbitrary types / constructors | forwarding reference ([Chapter 7](07-perfect-forwarding.md)) | optimal, but a template |
| a type that is cheap to copy (`int`, `string_view`, `span`) | by value | no move needed |

```cpp
class Widget {
    std::string name_;
public:
    explicit Widget(std::string name) : name_(std::move(name)) {}   // the idiom
};
```

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `return std::move(local);` | Defeats NRVO, extra move + dtor; `-Wpessimizing-move` | `return local;` |
| Move constructor not `noexcept` | `vector` growth *copies* | `noexcept`; `static_assert(is_nothrow_move_constructible_v<T>)` |
| `std::move(x)` then use `x` | Reads a moved-from value; may be empty, may be anything | Move at last use; re-assign before reuse |
| `std::move` on a `const` object | Silently *copies* (`const T&&` binds to the copy constructor) | Don't; `-Wpessimizing-move`/clang-tidy `performance-move-const-arg` |
| `std::move` of a function's *return value* `f(std::move(g()))` | Redundant: already a prvalue | Remove |
| Declaring a destructor, losing the moves | Silent copies (Experiment 6) | Rule of zero; or restate `= default` the moves |
| `const` members or reference members in a class | Move assignment (and copy assignment) is implicitly deleted; moves of `const` members **copy** | Make them non-`const`; use pointers/`unique_ptr` |
| Move constructor that does not reset the source | Double free; two owners | `std::exchange` |
| Self-move-assignment corrupting state | Use-after-free | Guard, or write it so it is naturally idempotent |
| Moving in a loop: `for (auto& x : v) sink(std::move(x));` then reading `v` | Moved-from elements | Clear the container, or don't read |
| Thinking `std::move` is the only way to "move" | `emplace_back`, `std::forward`, elision often better | Prefer in-place construction |
| `std::move(*this)` in a `&` member function | Surprising: invalidates the object the caller still holds | Use `&&`-qualified overload ([Chapter 5](05-value-categories.md)) |
| Moving a type with a *self-referencing* member (pointer into itself, SSO) naïvely | After the memberwise move the pointer points into the *old* object | Write the move to fix up the pointer, or delete it. `std::string` does this |
| Passing `const std::string` by value into a sink | Can only copy | Take `std::string` by value (non-`const`) |

> **Opinion on `std::move(x)` "everywhere".** The cargo-cult advice is to sprinkle `std::move` on everything to be "efficient." Nine times in ten it is a no-op (trivial types), a pessimization (`return std::move(local)`) or a bug (moved-from read). Write `std::move` where a **named object is consumed for the last time**: passing it to a sink parameter, storing it into a member or container, returning a *member* or a *parameter by reference*. Elsewhere, do not.

---

## 12. Exercises

1. **Instrument the container.** Using `Loud` from Experiment 1, count constructions, copies and moves for `vector::push_back`, `emplace_back`, `insert` in the middle, `erase`, and `sort` of 5 elements. Where does `noexcept` matter?
2. **Break it on purpose.** Remove `noexcept` from the move constructor in Experiment 5's `IntBuf` and put 100 of them into a `vector`. How many allocations does growth now cost? (Count with a replaced `operator new`.)
3. **`swap` vs `move`.** Implement `swap(T&, T&)` three ways: `T tmp = std::move(a); a = std::move(b); b = std::move(tmp);`, memberwise `std::swap`, and copy-and-swap assignment. For a `string`-like type with SSO, which is cheapest? Look at the assembly.
4. **Elision map.** Write ten small functions that return in different ways (two named locals with a branch, a parameter, a member, a ternary of two prvalues, a `static` local, a structured binding, ...). Predict, then use `-fno-elide-constructors` (with `-std=c++14`) and normal compilation to determine in which NRVO is applied on GCC and Clang. What does your table say about relying on NRVO?
5. **A moved-from contract.** Take a class of your own that holds a `std::string` and a cached `int` derived from it. Write the move constructor so that the moved-from object is **in a consistent state** (the cache is not stale relative to the empty string). Is "consistent" the same as "empty"?
6. **`std::exchange` internals.** Implement `exchange(T& obj, U&& new_value)` including the `noexcept` specification, with `std::is_nothrow_move_constructible_v<T>` and `std::is_nothrow_assignable_v<T&, U>`.
7. **Move-only function parameter.** Write a function taking `std::unique_ptr<Widget>` by value, one taking `unique_ptr&&`, and one taking `unique_ptr&`. Show call sites for each, and discuss which one communicates *"I take ownership"* best, which one allows the caller to keep it on failure, and the pitfalls of each.

---

## 13. Challenge: a move-aware `SmallString` with SSO

Implement `SmallString` that stores up to 15 characters inline and otherwise on the heap, with:

- default/copy/move constructors, copy/move assignment, destructor, `c_str()`, `size()`
- a **`noexcept`** move constructor that handles both modes correctly (hint: the inline mode must copy the bytes; the heap mode steals the pointer; **the data pointer, if you store one, must be fixed up**)
- the moved-from object left as an **empty string in inline mode**
- self-move safety
- a test with ASan/UBSan that exercises all mode transitions: inline→inline, heap→inline, inline→heap, heap→heap, for both construction and assignment (8 cases), plus self-assignment

Then compare the speed of moving 10 million `SmallString`s of length 8 and of length 40 against `std::string`. Explain the result using what you learned in Experiment 4.

*(This is the core of Project 3 / Chapter 11 and a warm-up for `SmallVector<T,N>`.)*

---

## 14. Knowledge check

1. What does `std::move(x)` do at runtime? What does it do at compile time?
2. Why did C++98 need `auto_ptr`'s unsafe copying, and what language feature replaced the need?
3. After `std::string b = std::move(a);` what can you rely on about `a`? What can you rely on after `unique_ptr q = std::move(p);`?
4. Why does `std::vector<T>::push_back` use `std::move_if_noexcept` during reallocation, and what is the consequence of a throwing move?
5. A class declares only a destructor. How is `T b = std::move(a);` executed, and what is the diagnostic?
6. `std::string f() { std::string s; return std::move(s); }`: what is wrong, and what does the compiler do?
7. Why is returning a `const T` by value harmful?
8. What does *guaranteed copy elision* guarantee that NRVO does not?
9. Why is moving a short `std::string` not an O(1) pointer theft?
10. In a constructor `Widget(std::string s) : name_(std::move(s))`, count the copies and moves for lvalue and rvalue arguments.
11. When is a self-move-assignment legitimate, and what must your implementation guarantee?
12. A class has a `const std::string` member. Which special members are affected, and what happens to `std::move(obj)`?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. At runtime: **nothing**, no instructions. At compile time: it casts its argument to an rvalue reference (xvalue), so a move constructor/assignment is *selected* by overload resolution.
2. C++98 had no way to distinguish a copy from a transfer, so `auto_ptr`'s copy constructor *moved*, which was unsafe in generic code (containers, algorithms copy elements assuming the source is unchanged). **Rvalue references** (C++11) let a type have a separate move operation; `unique_ptr` replaced `auto_ptr`.
3. For `string`: only that it is **valid but unspecified**: destroyable and assignable. (Implementations leave it empty; the standard doesn't promise it.) For `unique_ptr`: it is **null**, guaranteed.
4. To preserve the **strong exception guarantee** of the growth: if a move throws mid-way, the old elements are already partly moved-from and cannot be restored. If the move is `noexcept` it is safe to use; if not and the type is copyable, `vector` copies instead; if not copyable it moves and loses the guarantee.
5. The implicit move is **not declared**, so overload resolution finds the copy constructor (`const T&` binds an xvalue). The object is **copied**, silently. Some compilers/linters warn (`-Wdeprecated-copy-with-dtor`, clang-tidy).
6. `return std::move(s)` makes the operand an xvalue *expression*, not a plain name, so **NRVO is disabled** and `s` is constructed locally, then moved into the return slot, then destroyed. Plain `return s;` constructs `s` in the return slot. `-Wpessimizing-move`.
7. `const T&&` cannot bind to `T(T&&)`; the move constructor is not viable, so the result is **copied** wherever it is used to initialize something.
8. Guaranteed elision is **mandatory** and applies to initialization from a **prvalue**: no temporary exists, and the type need not even be movable. NRVO is optional, applies to *named* locals, and the type must still have an accessible copy/move constructor.
9. libstdc++ stores up to 15 characters **inside** the object. Moving such a string must **copy those bytes**; only a heap-mode string can have its pointer stolen. It is a small `memcpy`: still much cheaper than an allocation, but not O(1) pointer theft.
10. Lvalue: 1 copy (into the parameter) + 1 move (into the member). Rvalue: 1 move + 1 move. (A prvalue argument initializes the parameter directly: 0 + 1.)
11. It arises in generic code (`x = std::move(y)` where `x` and `y` alias). The object must remain **valid** (not necessarily unchanged); don't free then read the same resource. Guard with `this != &o`, or write it idempotently.
12. Copy assignment and move assignment are implicitly **deleted** (can't assign a `const` member). The move *constructor* still exists, but the `const` member is **copied**, not moved, since `const std::string&&` binds the copy constructor.

</details>

---

[← Previous: Chapter 5](05-value-categories.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 7 — Perfect forwarding →](07-perfect-forwarding.md)
