# Chapter 5 — Value Categories

> **Part III · Value categories and move semantics** &nbsp;|&nbsp; **Level 2** (language mechanisms) &nbsp;|&nbsp; **≈ 4 hours**
> **Prerequisites:** [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, `decltype`

[← Previous: Chapter 4](../part-02-object-model-and-lifetime/04-raii.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 6 — Move semantics →](06-move-semantics.md)

---

**In one sentence:** every *expression* in C++ has a **value category** that answers two questions (*does it refer to an object that has identity?* and *may its resources be taken?*), and overload resolution, reference binding, copy elision and move semantics are all driven by the answers.

**By the end of this chapter you can:**

- classify any expression as lvalue, xvalue or prvalue **by following a procedure**, not by memorizing a list
- explain why a *named* rvalue reference is an lvalue, and why `std::move(x)` is an xvalue
- predict which overload and which reference binds, for every category and cv-qualification
- use `decltype` as a **category oracle** to check your own reasoning
- explain temporary materialization and why C++17 introduced it
- use *ref-qualified member functions* to make an API do the right thing for temporaries

---

## 1. Problem

Before C++11 you had a simple vocabulary: an **lvalue** was something you could assign to (left side of `=`), an **rvalue** was everything else. That was enough for C.

It stopped being enough the moment C++ wanted to answer a new question:

> *"This object is about to go away. May I steal its guts instead of copying them?"*

Consider:

```cpp
std::string a = "a long string that lives on the heap, probably";
std::string b = a;                    // a is still needed afterwards: must copy
std::string c = make_string();        // the returned temporary is going away: could steal
std::string d = std::move(a);         // a is *named*, yet we declare we are done with it: steal
```

The compiler must distinguish these three situations *syntactically*, without knowing what you plan to do next. The old "left/right of assignment" idea cannot: `a` and `std::move(a)` both name the same object, and `std::move(a)` cannot be assigned to.

C++11 needed a vocabulary with **two independent properties**:

1. Does the expression **have identity**? (Is there a particular object I can refer to again?)
2. Can its resources be **moved from**? (Is the object *expiring*?)

---

## 2. Historical context

| Era | Vocabulary | Why it changed |
|---|---|---|
| **C** | *lvalue* = "locator value", something with an address. *rvalue* = everything else | Fine for C |
| **C++98/03** | Same, plus: a non-`const` reference binds only lvalues; `const T&` binds anything | Copying was the only way to pass temporaries around |
| **C++11** | Three **primary** categories: **lvalue, xvalue, prvalue**; two **mixed**: glvalue, rvalue | Move semantics needed "has identity but expiring" (the *xvalue*) |
| **C++17** | A *prvalue* is redefined as "an expression that **initializes** an object", not "a temporary object". New conversion: **temporary materialization** | Guaranteed copy elision: `T x = f();` constructs `x` in place, with no temporary |

The 2010 taxonomy proposal (N3055, Hinnant, Miller, Stroustrup, ...) is worth reading for one reason: it was the first time the committee derived the categories from two properties instead of listing them. The result is a small, exact system.

---

## 3. Modern solution: the taxonomy

Two properties, three combinations in use:

|  | **can be moved from** (`m`) | **cannot be moved from** |
|---|:-:|:-:|
| **has identity** (`i`) | **xvalue** | **lvalue** |
| **no identity** | **prvalue** | *(does not exist)* |

```text
                    expression
                   /          \
              glvalue          rvalue
             (has identity)   (can be moved from)
             /        \       /        \
        lvalue        xvalue         prvalue
```

| Category | In words | Example |
|---|---|---|
| **lvalue** | An object you can refer to again, which you did **not** say you are done with | `x`, `*p`, `a[i]`, `s.m` |
| **xvalue** ("expiring") | An object with identity whose resources you **may** steal | `std::move(x)`, `static_cast<T&&>(x)`, `f()` where `f` returns `T&&` |
| **prvalue** ("pure rvalue") | Not an object yet: a **recipe for a value** or for initializing an object | `42`, `x + 1`, `T{}`, `f()` where `f` returns `T` |
| **glvalue** | *lvalue or xvalue*: refers to an object | |
| **rvalue** | *xvalue or prvalue*: can be moved from | |

Two things to notice immediately:

1. An **xvalue is a glvalue and an rvalue** at once. It is the hinge of the whole design.
2. **Value category belongs to an *expression*, not to an object or a variable.** The same object can be named by an lvalue expression (`x`) and an xvalue expression (`std::move(x)`).

---

## 4. Mental model

### A decision procedure

To classify an expression, ask in this order:

```text
 1. Does it refer to an existing object or function with a name/identity?
    (can you apply & to it, or is it clearly "that particular thing"?)
      │
      ├── NO ──► PRVALUE.   It computes a value or initializes an object.
      │
      └── YES ─► it is a GLVALUE. Now:
                 2. Is it "expiring"?  i.e. is it
                      · the result of std::move / static_cast<T&&>(…)
                      · a call to a function returning T&&
                      · a member (or element) of an xvalue
                      · a member of a temporary (since C++17)
                      │
                      ├── YES ─► XVALUE
                      └── NO ──► LVALUE
```

### Four sentences to keep

> **A name is an lvalue.** Always. Whatever its declared type is, including `T&&`.
> **A call is an lvalue if it returns `T&`, an xvalue if it returns `T&&`, and a prvalue if it returns `T`.**
> **`std::move` does not move. It *casts* an lvalue into an xvalue**, so that something *else* may choose to move.
> **A prvalue is not an object.** It becomes one (a temporary) only when something needs one: **temporary materialization**.

### Three pictures

```text
  lvalue         ┌────────────┐         "that thing over there (still needed)"
  x ───────────► │  object    │ ◄──┐
                 └────────────┘    │
  xvalue                           │    "that same thing, but I'm finished with it"
  std::move(x) ────────────────────┘
                                        
  prvalue        42       x + 1       T{}        "a recipe: no place yet"
                  │         │          │
                  └─────────┴──────────┴──► (materialized into a temporary object only if needed)
```

### Taking the address

A practical heuristic for *lvalue*: **if you can write `&expr`, it is an lvalue** (with caveats for bit-fields and `register`-era corners). `&x` works, `&42` and `&std::move(x)` do not. An xvalue has identity but the language forbids taking its address on purpose: it is an object you are not supposed to refer to again.

---

## 5. Language rules

### 5.1 The definitions  `[basic.lval]`

- A **glvalue** is an expression whose evaluation determines the **identity** of an object or function.
- A **prvalue** is an expression whose evaluation **initializes an object** or computes the value of an operand of an operator, as specified by the context in which it appears.
- An **xvalue** is a glvalue that denotes an object whose **resources can be reused**.
- An **lvalue** is a glvalue that is not an xvalue.
- An **rvalue** is a prvalue or an xvalue.

### 5.2 Which expressions are which  `[expr.prop]`

| Expression | Category | Note |
|---|---|---|
| Name of variable, function, data member, enumerator-as-object; `this` is a prvalue though | **lvalue** | Even if its type is `T&&`: `void f(T&& r)` → `r` is an lvalue |
| String literal (`"abc"`) | **lvalue** | Of type `const char[4]` |
| Other literals (`42`, `true`, `nullptr`, `3.14`) | **prvalue** | |
| `a + b`, `-a`, `!a`, `a == b`, `&a` | **prvalue** | Built-in arithmetic and relational operators |
| `a++`, `a--` | **prvalue** | Returns the *old* value |
| `++a`, `--a`, `a = b`, `a += b` | **lvalue** | Built-in; return `a` itself |
| `*p`, `a[i]` (built-in), `p->m`, `a.m` (a lvalue) | **lvalue** | |
| `a, b` (built-in comma) | category of `b` | |
| Function call returning `T&` | **lvalue** | |
| Function call returning `T&&` | **xvalue** | |
| Function call returning `T` (including `T(args)`, `T{args}`) | **prvalue** | |
| `static_cast<T&&>(e)`, `std::move(e)`, `std::forward<T>(e)` for non-ref `T` | **xvalue** | |
| `static_cast<T&>(e)`, `std::forward<T&>(e)` | **lvalue** | |
| `static_cast<T>(e)`, `(T)e` for non-reference `T` | **prvalue** | |
| `a.m` where `a` is an xvalue, or where `a` is a prvalue (materialized) | **xvalue** | C++17; `make().m` is an xvalue |
| `a[i]` where `a` is an array xvalue | **xvalue** | |
| Lambda expression | **prvalue** | |
| `c ? e1 : e2` | see §5.3 | The subtle one |
| `sizeof(…)`, `alignof(…)`, `noexcept(…)` | **prvalue** | |

### 5.3 The conditional operator

`c ? e1 : e2` is an **lvalue** only if `e1` and `e2` are **both lvalues of the same type** (after cv-adjustment). Otherwise it is a **prvalue**, and the chosen operand is *copied/converted* into the result:

```cpp
cond ? x : y        // both lvalue int → lvalue (so you can write: (cond ? x : y) = 5;)
cond ? x : 1        // lvalue and prvalue → prvalue (x is copied)
cond ? x : Tr("z")  // lvalue and prvalue → prvalue: choosing x COPIES it
```

This catches people when `x` is a large object: a harmless-looking `cond ? big : T{}` copies `big`.

### 5.4 Binding rules  `[dcl.init.ref]`

Which references can bind to which categories:

| Reference type | lvalue | xvalue | prvalue |
|---|:-:|:-:|:-:|
| `T&` | ✅ | ❌ | ❌ |
| `const T&` | ✅ | ✅ | ✅ (lifetime-extends the temporary) |
| `T&&` | ❌ | ✅ | ✅ (materialized) |
| `const T&&` | ❌ | ✅ | ✅ |

And **overload resolution prefers the narrowest binding**: for an rvalue argument, `f(T&&)` beats `f(const T&)`; for an lvalue, `f(T&)` beats `f(const T&)`; a `const` lvalue can only use `f(const T&)`. This is how a single call-site spelling `v.push_back(x)` selects *copy* for an lvalue and *move* for a temporary: two overloads, selected by category.

### 5.5 Temporary materialization  `[conv.rval]` (C++17)

> A **prvalue is converted to an xvalue** by *temporary materialization* (a temporary object is created and initialized), when a reference is bound to it, when a member is accessed on it, and in a few other contexts.

This is why `make().m` is an xvalue: `make()` is a prvalue; accessing `.m` forces materialization of a temporary `S`, and `.m` of an xvalue is an xvalue.

Why the extra concept? Because **before C++17 a prvalue *was* a temporary** and `T x = f();` semantically created a temporary, then copy-constructed `x` from it (the copy then being elidable). Since C++17, `f()` is an *initialization recipe* that is **handed the destination**. No temporary exists, so there is nothing to copy or move, so guaranteed copy elision is not an optimization but a *consequence of the definition*. Chapter 6 builds on this.

### 5.6 `decltype` and the categories  `[dcl.type.decltype]`

For an expression `e` that is not an unparenthesized id-expression or class member access:

| Category of `e` | `decltype(e)` |
|---|---|
| prvalue of type `T` | `T` |
| lvalue of type `T` | `T&` |
| xvalue of type `T` | `T&&` |

And `decltype((x))` for a *variable* `x` (note the **extra parentheses**) treats `(x)` as an expression: `T&`. Meanwhile `decltype(x)` without parentheses gives the **declared type** of `x`. This difference is precisely what lets us build an **oracle** in Experiment 1.

### 5.7 Implicit moves on `return`  `[class.copy.elision]`, `[stmt.return]`

When a `return` names a local variable (or parameter), the compiler first tries to treat the name as an **xvalue**, so that the move constructor is selected. The rules widened in C++20 (P1825) and C++23 (P2266: even `T&&` parameters now move). Chapter 6 covers when this helps and when `return std::move(x);` actively hurts.

### Layer check

| Layer | What it decides |
|---|---|
| **Standard** | The categories; all the rules above; reference binding; overload resolution |
| **Compiler** | Nothing observable: categories exist only in the front end |
| **ABI** | `T&` and `T&&` are both *pointers*; the distinction disappears in the calling convention (Experiment 6) |
| **OS / CPU** | Nothing. Value categories have **no runtime representation** |

---

## 6. Implementation model

Value categories are a **purely compile-time, front-end** concept. Once overload resolution and initialization have used them, they are gone:

```text
   lvalue ref  T&   ─┐
                     ├──►  a pointer to the object   (one register, e.g. rdi)
   rvalue ref  T&&  ─┘

   prvalue of scalar type, passed by value  ──►  the value itself, in a register
   prvalue materialized because a reference needs an address
                                            ──►  a stack slot, at -O0; usually *optimized away* at -O2
```

So **`std::move` generates no code**. It changes the type of an expression from `T&` to `T&&`, and *that* changes which overload is selected. The overload that `T&&` selects may then do something clever. There is no "move instruction".

---

## 7. Experiments

### Experiment 1: A value-category oracle

You will rarely need to guess again: `decltype((expr))` tells the compiler's answer. This is the single most useful tool in the chapter, so build it once and keep it.

```cpp
// @test run -std=c++23 -O0 -Wno-unused-value
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

// decltype((e)) is T for a prvalue, T& for an lvalue, T&& for an xvalue.
template <class T>
constexpr const char* category_of() {
    if constexpr (std::is_lvalue_reference_v<T>)      return "lvalue";
    else if constexpr (std::is_rvalue_reference_v<T>) return "xvalue";
    else                                              return "prvalue";
}
#define VC(expr)   category_of<decltype((expr))>()
#define SHOW(expr) std::printf("  %-26s %s\n", #expr, VC(expr))

struct S { int m = 0; int method() const { return 1; } S& self() { return *this; } };
int   val();      // returns by value
int&  lref();     // returns an lvalue reference
int&& rref();     // returns an rvalue reference
S     make();

int main() {
    int x = 1, y = 2, a[3] = {};
    int* p = &x;
    int&& named_rr = 5;
    S s;
    const int cx = 4;
    bool cond = true;

    std::puts("names and literals");
    SHOW(x); SHOW((x)); SHOW(cx); SHOW(named_rr); SHOW(42); SHOW("text"); SHOW(nullptr); SHOW(val);
    std::puts("operators");
    SHOW(x + y); SHOW(-x); SHOW(x++); SHOW(++x); SHOW(x = 5); SHOW(x += 1);
    SHOW(&x); SHOW(*p); SHOW(a[1]); SHOW((x, y)); SHOW(!cond);
    std::puts("function calls");
    SHOW(val()); SHOW(lref()); SHOW(rref()); SHOW(make()); SHOW(S{}); SHOW(s.method()); SHOW(s.self());
    std::puts("casts");
    SHOW(std::move(x)); SHOW(static_cast<int&&>(x)); SHOW(static_cast<int>(x)); SHOW(static_cast<int&>(x));
    SHOW(std::forward<int>(x)); SHOW(std::forward<int&>(x));
    std::puts("members");
    SHOW(s.m); SHOW(make().m); SHOW(std::move(s).m); SHOW(S{}.m); SHOW(std::move(a)[1]);
    std::puts("conditional");
    SHOW(cond ? x : y); SHOW(cond ? x : 1); SHOW(cond ? 1 : 2); SHOW(cond ? x : cx);
    std::puts("misc");
    SHOW([] {}); SHOW(sizeof(x)); SHOW(static_cast<void>(x));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
names and literals
  x                          lvalue
  (x)                        lvalue
  cx                         lvalue
  named_rr                   lvalue
  42                         prvalue
  "text"                     lvalue
  nullptr                    prvalue
  val                        lvalue
operators
  x + y                      prvalue
  -x                         prvalue
  x++                        prvalue
  ++x                        lvalue
  x = 5                      lvalue
  x += 1                     lvalue
  &x                         prvalue
  *p                         lvalue
  a[1]                       lvalue
  (x, y)                     lvalue
  !cond                      prvalue
function calls
  val()                      prvalue
  lref()                     lvalue
  rref()                     xvalue
  make()                     prvalue
  S{}                        prvalue
  s.method()                 prvalue
  s.self()                   lvalue
casts
  std::move(x)               xvalue
  static_cast<int&&>(x)      xvalue
  static_cast<int>(x)        prvalue
  static_cast<int&>(x)       lvalue
  std::forward<int>(x)       xvalue
  std::forward<int&>(x)      lvalue
members
  s.m                        lvalue
  make().m                   xvalue
  std::move(s).m             xvalue
  S{}.m                      xvalue
  std::move(a)[1]            xvalue
conditional
  cond ? x : y               lvalue
  cond ? x : 1               prvalue
  cond ? 1 : 2               prvalue
  cond ? x : cx              lvalue
misc
  [] {}                      prvalue
  sizeof(x)                  prvalue
  static_cast<void>(x)       prvalue
```

GCC and Clang agree on every line. Pick out the ones that trip people:

- **`named_rr` is an lvalue**, although it is *declared* `int&&`. Declared type and expression category are different things. This single fact explains why `std::move` and `std::forward` exist.
- **`"text"` is an lvalue** (of type `const char[5]`), unlike every other literal.
- **`x++` is a prvalue; `++x` is an lvalue.** Post-increment returns a copy of the old value, and pre-increment returns the object itself. For class types this is why `++it` is usually cheaper than `it++`.
- **`(x, y)` is an lvalue**: the comma operator yields its right operand *as is*.
- **`make().m` and `S{}.m` are xvalues**: members of a materialized temporary.
- **`cond ? x : 1` is a prvalue**, but `cond ? x : y` is an lvalue.
- **`std::forward<int>(x)` is an xvalue, `std::forward<int&>(x)` an lvalue.** `std::forward<T>` preserves the *category* that `T` encodes ([Chapter 7](07-perfect-forwarding.md)).

### Experiment 2: Overload resolution by category

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <utility>

struct W {};
const char* f(W&)        { return "f(W&)"; }
const char* f(const W&)  { return "f(const W&)"; }
const char* f(W&&)       { return "f(W&&)"; }
const char* f(const W&&) { return "f(const W&&)"; }

const char* g(const W&)  { return "g(const W&)"; }       // the only overload is const&
const char* h(W&)        { return "h(W&)"; }
const char* h(W&&)       { return "h(W&&)"; }

const W make_const() { return W{}; }                     // a const prvalue (rare: see §11)

const char* from_rr(W&& r)       { return f(r); }                // r is a NAME: an lvalue
const char* from_rr_moved(W&& r) { return f(std::move(r)); }

#define ROW(expr) std::printf("  %-26s -> %s\n", #expr, expr)

int main() {
    W w;
    const W cw{};
    std::puts("overload set {W&, const W&, W&&, const W&&}");
    ROW(f(w)); ROW(f(cw)); ROW(f(W{})); ROW(f(std::move(w))); ROW(f(std::move(cw))); ROW(f(make_const()));
    std::puts("only g(const W&): everything binds");
    ROW(g(w)); ROW(g(W{})); ROW(g(std::move(w)));
    std::puts("h(W&) and h(W&&) only");
    ROW(h(w)); ROW(h(W{}));
    std::puts("inside a function taking W&& r");
    ROW(from_rr(W{})); ROW(from_rr_moved(W{}));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
overload set {W&, const W&, W&&, const W&&}
  f(w)                       -> f(W&)
  f(cw)                      -> f(const W&)
  f(W{})                     -> f(W&&)
  f(std::move(w))            -> f(W&&)
  f(std::move(cw))           -> f(const W&&)
  f(make_const())            -> f(const W&&)
only g(const W&): everything binds
  g(w)                       -> g(const W&)
  g(W{})                     -> g(const W&)
  g(std::move(w))            -> g(const W&)
h(W&) and h(W&&) only
  h(w)                       -> h(W&)
  h(W{})                     -> h(W&&)
inside a function taking W&& r
  from_rr(W{})               -> f(W&)
  from_rr_moved(W{})         -> f(W&&)
```

The last pair is the whole point of `std::move` and `std::forward` in two lines. `from_rr` receives an rvalue but, **inside**, `r` is a *name*, so `f(r)` selects `f(W&)`. To pass it on *as an rvalue* you must say so with a cast. That cast is `std::move(r)`. No other mechanism could be both safe and explicit: if a named rvalue reference were *automatically* an rvalue, a second use of `r` after the first would silently use a moved-from object.

### Experiment 3: `decltype(x)` vs `decltype((x))`, `auto` vs `auto&&`

```cpp
// @test run -std=c++23 -O0 -Wno-unused-variable
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

template <class T>
std::string describe() {                       // shows only const-ness and reference-ness of int
    std::string s = std::is_const_v<std::remove_reference_t<T>> ? "const int" : "int";
    if (std::is_lvalue_reference_v<T>) s += "&";
    if (std::is_rvalue_reference_v<T>) s += "&&";
    return s;
}
#define SHOW(label, ...) std::printf("  %-34s %s\n", label, describe<__VA_ARGS__>().c_str())

int val() { return 0; }

int main() {
    int x = 1;
    const int cx = 2;

    SHOW("decltype(x)",                        decltype(x));
    SHOW("decltype((x))",                      decltype((x)));
    SHOW("decltype(std::move(x))",             decltype(std::move(x)));
    SHOW("decltype(val())",                    decltype(val()));

    auto a = x;                                SHOW("auto a = x",               decltype(a));
    auto&& b = x;                              SHOW("auto&& b = x",             decltype(b));
    auto&& c = cx;                             SHOW("auto&& c = cx",            decltype(c));
    auto&& d = std::move(x);                   SHOW("auto&& d = std::move(x)",  decltype(d));
    auto&& e = 5;                              SHOW("auto&& e = 5",             decltype(e));
    auto&& f = val();                          SHOW("auto&& f = val()",         decltype(f));
    decltype(auto) g = x;                      SHOW("decltype(auto) g = x",     decltype(g));
    decltype(auto) h = (x);                    SHOW("decltype(auto) h = (x)",   decltype(h));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  decltype(x)                        int
  decltype((x))                      int&
  decltype(std::move(x))             int&&
  decltype(val())                    int
  auto a = x                         int
  auto&& b = x                       int&
  auto&& c = cx                      const int&
  auto&& d = std::move(x)            int&&
  auto&& e = 5                       int&&
  auto&& f = val()                   int&&
  decltype(auto) g = x               int
  decltype(auto) h = (x)             int&
```

`auto&&` (and a template parameter `T&&`) is a **forwarding reference**: it deduces `int&` for an lvalue and `int&&` for an rvalue, which is the mechanism of [Chapter 7](07-perfect-forwarding.md). The last two lines are the classic trap: `decltype(auto) h = (x);` makes a **reference** because of the parentheses, which is a footgun in `decltype(auto)` return types (`return (x);` returns `int&`, and dangles if `x` is local).

### Experiment 4: Ref-qualified member functions: an API that reacts to category

The category of the **object** on which a member function is called can select the overload, via a *ref-qualifier* (`&`, `const &`, `&&`):

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <utility>
#include <vector>

struct Elem {
    static inline int copies = 0;
    Elem() = default;
    Elem(const Elem&) { ++copies; }
    Elem(Elem&&) noexcept {}
};

class Buffer {
    std::vector<Elem> data_;
public:
    Buffer() : data_(3) {}

    // Called on an lvalue: the buffer is still needed, so COPY out.
    std::vector<Elem> take() const& { return data_; }

    // Called on an rvalue: the buffer is about to die, so STEAL.
    std::vector<Elem> take() && { return std::move(data_); }
};

Buffer make_buffer() { return Buffer{}; }

int main() {
    Buffer b;

    Elem::copies = 0;
    auto v1 = b.take();                               // lvalue
    std::printf("b.take()              element copies: %d\n", Elem::copies);

    Elem::copies = 0;
    auto v2 = make_buffer().take();                   // prvalue → materialized → xvalue
    std::printf("make_buffer().take()  element copies: %d\n", Elem::copies);

    Elem::copies = 0;
    auto v3 = std::move(b).take();                    // explicit xvalue
    std::printf("std::move(b).take()   element copies: %d\n", Elem::copies);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
b.take()              element copies: 3
make_buffer().take()  element copies: 0
std::move(b).take()   element copies: 0
```

The same *spelling* (`.take()`) does the cheap thing when the object is expiring, and the safe thing when it is not. This is the standard library's own idiom: `std::optional::value() &` returns `T&` while `value() &&` returns `T&&`; `std::stringstream::str() &&` (C++20) moves the buffer out; range adaptors treat rvalue ranges as owned.

Ref-qualifiers can also **forbid** a dangerous call outright. This is the cleanest fix for the dangling-view bug of [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md):

```cpp
// @test fail -std=c++23 err=deleted
#include <string>
#include <string_view>

class Person {
    std::string name_ = "Ada Lovelace";
public:
    std::string_view name() const& { return name_; }   // fine: *this outlives the view
    std::string_view name() && = delete;                // a view into a temporary would dangle
};

Person make();

int main() {
    Person p;
    std::string_view ok = p.name();
    std::string_view bad = make().name();               // error: use of deleted function
    (void)ok; (void)bad;
}
```

### Experiment 5: The same code for `T&` and `T&&` (assembly)

```cpp
// @test asm -std=c++23 -O0 -fno-stack-protector filter=take_lref,take_rref,call_prvalue
void take_lref(int& r)  { r += 1; }
void take_rref(int&& r) { r += 1; }

int call_prvalue() {
    int r = 41;
    take_rref(r + 1 - 1);        // a prvalue bound to int&&: needs a temporary to point at
    return r;
}
```

```asm
; asm (gcc 14.2.0, -O0, x86-64, Intel syntax)
take_lref(int&):
	push	rbp
	mov	rbp, rsp
	mov	QWORD PTR -8[rbp], rdi
	mov	rax, QWORD PTR -8[rbp]
	mov	eax, DWORD PTR [rax]
	lea	edx, 1[rax]
	mov	rax, QWORD PTR -8[rbp]
	mov	DWORD PTR [rax], edx
	nop
	pop	rbp
	ret

take_rref(int&&):
	push	rbp
	mov	rbp, rsp
	mov	QWORD PTR -8[rbp], rdi
	mov	rax, QWORD PTR -8[rbp]
	mov	eax, DWORD PTR [rax]
	lea	edx, 1[rax]
	mov	rax, QWORD PTR -8[rbp]
	mov	DWORD PTR [rax], edx
	nop
	pop	rbp
	ret

call_prvalue():
	push	rbp
	mov	rbp, rsp
	sub	rsp, 16
	mov	DWORD PTR -4[rbp], 41
	mov	eax, DWORD PTR -4[rbp]
	mov	DWORD PTR -8[rbp], eax
	lea	rax, -8[rbp]
	mov	rdi, rax
	call	take_rref(int&&)
	mov	eax, DWORD PTR -4[rbp]
	leave
	ret
```

`take_lref` and `take_rref` are **instruction-for-instruction identical**: each receives a pointer in `rdi` and adds 1 through it. `&` versus `&&` was a compile-time overload-selection device and left no trace. In `call_prvalue` the prvalue `r + 1 - 1` was **materialized**: the compiler computed it into a stack slot (`-8[rbp]`) and passed its address, because an rvalue reference needs something to point at. Now optimized:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=take_lref,take_rref,call_prvalue
void take_lref(int& r)  { r += 1; }
void take_rref(int&& r) { r += 1; }

int call_prvalue() {
    int r = 41;
    take_rref(r + 1 - 1);
    return r;
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
take_lref(int&):
	add	DWORD PTR [rdi], 1
	ret

take_rref(int&&):
	add	DWORD PTR [rdi], 1
	ret

call_prvalue():
	mov	eax, 41
	ret
```

With optimization the temporary is gone, the function is inlined, and `call_prvalue` returns the constant `41`. (Look: `take_rref(r + 1 - 1)` incremented a *temporary*, not `r`; the optimizer proved it and folded the whole function to `mov eax, 41`.) Same lesson as Chapter 1: **language-level abstractions are erased, and only meaning survives.**

---

## 8. Assembly / runtime investigation

The assembly in Experiment 5 *is* the investigation. To go further, dump the **front end's** view, which is where categories live:

```bash
# GCC: the GIMPLE the front end hands to the middle end (references already as pointers)
g++-14 -std=c++23 -fdump-tree-gimple -c prog.cpp && less prog.cpp.*gimple

# Clang: the AST, with value categories annotated on every expression
clang++-18 -std=c++23 -Xclang -ast-dump -fsyntax-only prog.cpp | less
```

In Clang's AST dump every expression node ends in `lvalue`, `xvalue` or `prvalue`, and you can see `MaterializeTemporaryExpr` nodes where §5.5 says they should be, and `ImplicitCastExpr <NoOp>` nodes for the `static_cast<T&&>` hidden inside `std::move`. Use it whenever the oracle disagrees with your intuition.

---

## 9. Implementation exercise: predict, then check with your own oracle

1. Re-create the oracle (macro `VC` over `decltype((e))`) without looking at Experiment 1.
2. **Before running**, write down the category of each of these 24 expressions, using only the decision procedure in §4.
3. Add a `static_assert` for each of your predictions, using a version of the oracle that is `constexpr` and returns an `enum class Cat { lvalue, xvalue, prvalue }`. Every wrong guess becomes a compile error. Count them.

```cpp
// @test skip exercise: predict each category, then assert it with your own oracle
int  x = 0, y = 0, arr[2] = {};  int* p = &x;  int&& rr = 1;  const int c = 2;
struct P { int m; int get(); int& ref(); };  P pv();  P& pl();  P&& px();  P obj;
// 1  x               9   pl()           17  px().m
// 2  rr              10  px()           18  pv().get()
// 3  c               11  pv().m         19  obj.ref()
// 4  x + y           12  pl().m         20  (obj).m
// 5  *p              13  std::move(rr)  21  true ? x : y
// 6  arr[1]          14  x = y          22  true ? x : 3
// 7  ++x             15  x++            23  -x
// 8  pv()            16  &x             24  nullptr
```

<details>
<summary><strong>Answers</strong> (try first)</summary>

| # | Category | # | Category | # | Category |
|---|---|---|---|---|---|
| 1 | lvalue | 9 | lvalue | 17 | xvalue |
| 2 | lvalue (a name) | 10 | xvalue | 18 | prvalue |
| 3 | lvalue | 11 | xvalue | 19 | lvalue |
| 4 | prvalue | 12 | lvalue | 20 | lvalue |
| 5 | lvalue | 13 | xvalue | 21 | lvalue |
| 6 | lvalue | 14 | lvalue | 22 | prvalue |
| 7 | lvalue | 15 | prvalue | 23 | prvalue |
| 8 | prvalue | 16 | prvalue | 24 | prvalue |

Verification program (tested):

```cpp
// @test run -std=c++23 -O0
#include <utility>

enum class Cat { lvalue, xvalue, prvalue };

template <class T>
constexpr Cat category_of() {
    if constexpr (std::is_lvalue_reference_v<T>)      return Cat::lvalue;
    else if constexpr (std::is_rvalue_reference_v<T>) return Cat::xvalue;
    else                                              return Cat::prvalue;
}
#define CAT(e) category_of<decltype((e))>()

int  x = 0, y = 0, arr[2] = {};
int* p = &x;
int&& rr = 1;
const int c = 2;
struct P { int m; int get(); int& ref(); };
P pv();  P& pl();  P&& px();
P obj;

static_assert(CAT(x) == Cat::lvalue);          static_assert(CAT(rr) == Cat::lvalue);
static_assert(CAT(c) == Cat::lvalue);          static_assert(CAT(x + y) == Cat::prvalue);
static_assert(CAT(*p) == Cat::lvalue);         static_assert(CAT(arr[1]) == Cat::lvalue);
static_assert(CAT(++x) == Cat::lvalue);        static_assert(CAT(pv()) == Cat::prvalue);
static_assert(CAT(pl()) == Cat::lvalue);       static_assert(CAT(px()) == Cat::xvalue);
static_assert(CAT(pv().m) == Cat::xvalue);     static_assert(CAT(pl().m) == Cat::lvalue);
static_assert(CAT(std::move(rr)) == Cat::xvalue);
static_assert(CAT(x = y) == Cat::lvalue);      static_assert(CAT(x++) == Cat::prvalue);
static_assert(CAT(&x) == Cat::prvalue);        static_assert(CAT(px().m) == Cat::xvalue);
static_assert(CAT(pv().get()) == Cat::prvalue);static_assert(CAT(obj.ref()) == Cat::lvalue);
static_assert(CAT((obj).m) == Cat::lvalue);    static_assert(CAT(true ? x : y) == Cat::lvalue);
static_assert(CAT(true ? x : 3) == Cat::prvalue);
static_assert(CAT(-x) == Cat::prvalue);        static_assert(CAT(nullptr) == Cat::prvalue);

int main() {}
```

</details>

---

## 10. Real-world example: category-aware APIs in the standard library

| API | Lvalue behaviour | Rvalue behaviour | Why |
|---|---|---|---|
| `vector::push_back(const T&)` / `(T&&)` | copy into the vector | move into the vector | The classic pair |
| `optional::value() &` / `&&` | returns `T&` | returns `T&&` | `std::move(opt).value()` is a *move out* |
| `unique_ptr::operator*` | `T&` | (same) | Not category-sensitive: ownership is not transferred by dereference |
| `std::stringstream::str() &` / `&&` (C++20) | copy of the buffer | **moves** the buffer out | Avoids a copy of the whole string |
| `std::get<I>(tuple&)` / `(tuple&&)` | `T&` | `T&&` | Structured forwarding of tuple elements |
| `std::views::all(r)` | a **view** of `r` | an **owning_view** (C++20) | A range adaptor must not dangle on a temporary |
| `operator+(std::string&&, const std::string&)` | allocates a new string | **reuses** the left operand's buffer | `a + b + c` allocates once, not twice |

The last row is the quiet payoff of the whole system: `"a" + s1 + s2 + s3` evaluates left to right, and each intermediate result is a *prvalue*, so each `+` finds a moveable buffer and appends instead of allocating. You write a natural expression and get the efficient algorithm, because the category of each sub-expression selected the right overload.

This also tells you how to design your own API: **offer `&&` overloads where the object is an expiring resource owner; `= delete` the `&&` overload where returning a view into `*this` would dangle**.

---

## 11. Failure modes

| Mistake | What goes wrong | Fix |
|---|---|---|
| Assuming a `T&&` parameter is an rvalue **inside** the function | It is a *name*, hence an lvalue; `f(r)` **copies** | `f(std::move(r))` for sink parameters; `std::forward<T>(r)` for forwarding references |
| Using `r` **after** `std::move(r)` | Moved-from value ([Chapter 6](06-move-semantics.md)) | Treat `std::move(x)` as the *last use* of `x` |
| `std::move(const_obj)` | Yields `const T&&`; binds to the **copy** constructor `T(const T&)`, so it silently copies | Don't move from `const`; make `static_assert`s in generic code |
| `return std::move(local);` | Defeats NRVO: forces a move where the compiler would have constructed in place (GCC: `-Wpessimizing-move`) | `return local;` |
| `T&& f()` returning a reference to a local or a member | Dangling reference | Return by value |
| `auto&& x = f().member;` or `for (auto&& x : f().items())` | Possibly dangling (Chapter 3) | Name the object first |
| `const T f()` (a `const` return by value) | Blocks moves from the result: `const T&&` selects the copy constructor | Return `T`, not `const T` |
| `decltype(auto) f() { return (x); }` | Returns `T&`, usually to a local | Return `x`, or use `auto` |
| Believing "rvalue" means "temporary object" | An **xvalue** is not a temporary, it is any object you may steal from | Use the precise term |
| `std::move` on a trivially copyable type | A pure no-op: same bytes copied | Harmless, but noise |
| `cond ? big : T{}` | The `big` operand is **copied** (the result is a prvalue) | Use `if`/`else`, or `cond ? std::move(big) : T{}` if moving is intended |
| Forgetting the ref-qualifier makes *all* overloads of that name ref-qualified | Mixing qualified and unqualified overloads is ill-formed | Qualify all overloads of a name, or none |

---

## 12. Exercises

1. **Extend the oracle.** Add rows to Experiment 1 for: a bit-field member, a pointer-to-member (`s.*pm`), a function name, `typeid(x)`, `co_await`-free `this`, a `reinterpret_cast<int&>`, and `std::as_const(x)`. Predict first.
2. **Overload matrix.** For every combination of {lvalue, const lvalue, prvalue, const prvalue, xvalue, const xvalue} × {`f(T&)`, `f(const T&)`, `f(T&&)`, `f(const T&&)`, `f(T)`}, build a table of which candidate wins when only *some* of the overloads are present. Which combinations are **ambiguous**?
3. **Ref-qualified fluent API.** Write a `QueryBuilder` whose `where(...)` returns `QueryBuilder&` when called on an lvalue (chain without copying) and `QueryBuilder&&` when called on an rvalue (so `QueryBuilder().where(a).where(b)` never copies). Count copies with an instrumented member.
4. **Delete the dangling overload.** Take any class with an accessor returning `string_view`/`span`/pointer into `*this`. Add the `&& = delete` overload and show the rejected call. Then find a legitimate use that it blocks, and decide whether the trade is right.
5. **Read the AST.** Use `clang++ -Xclang -ast-dump` on `T x = make();`, `const T& r = make();` and `T y = make().member;`. Mark every `MaterializeTemporaryExpr`. Which of the three has *no* materialization, and why?
6. **Pre-C++17.** Compile Experiment 4's `make_buffer().take()` with `-std=c++14 -fno-elide-constructors`. What changes in the number of moves, and what does that tell you about prvalues before and after C++17?

---

## 13. Challenge: predict the copies and moves

The program below has 17 statements. For each one, predict the **sequence of events** (`construct`, `COPY`, `move`, `COPY-assign`, `move-assign`) it prints. Only then open the output.

```cpp
// @test run -std=c++23 -O0 -Wno-pessimizing-move -Wno-unused-but-set-variable
#include <cstdio>
#include <utility>

struct Tr {
    const char* n;
    explicit Tr(const char* name) : n(name) { std::puts("      construct"); }
    Tr(const Tr& o) : n(o.n)         { std::puts("      COPY"); }
    Tr(Tr&& o) noexcept : n(o.n)     { std::puts("      move"); }
    Tr& operator=(const Tr&)         { std::puts("      COPY-assign"); return *this; }
    Tr& operator=(Tr&&) noexcept     { std::puts("      move-assign"); return *this; }
};

Tr make()                     { return Tr("make"); }
Tr pass(Tr t)                 { return t; }
const Tr& id(const Tr& t)     { return t; }
void sink(Tr)                 {}
Tr pick(bool c, Tr& a, Tr& b) { return c ? a : b; }
Tr pick_rv(bool c)            { return c ? Tr("x") : Tr("y"); }
Tr local_named()              { Tr t("local"); return t; }
Tr local_moved()              { Tr t("local"); return std::move(t); }

#define STEP(n, code) std::printf("%2d  %s\n", n, #code); code

int main() {
    bool cond = true;
    STEP(1,  Tr a("a"););
    STEP(2,  Tr b = a;);
    STEP(3,  Tr c = std::move(a););
    STEP(4,  Tr d = make(););
    STEP(5,  Tr e = pass(d););
    STEP(6,  Tr f = pass(make()););
    STEP(7,  sink(std::move(e)););
    STEP(8,  sink(Tr("tmp")););
    STEP(9,  Tr g = id(f););
    STEP(10, Tr h = pick(cond, b, c););
    STEP(11, Tr i = pick_rv(cond););
    STEP(12, Tr j = local_named(););
    STEP(13, Tr k = local_moved(););
    STEP(14, b = c;);
    STEP(15, b = Tr("rhs"););
    STEP(16, Tr l = cond ? b : c;);
    STEP(17, Tr m = (cond ? b : Tr("z")););
}
```

<details>
<summary><strong>Output</strong> (predict first; the reasons follow below)</summary>

```text
# output (gcc 14.2.0, x86-64 Linux)
 1  Tr a("a");
      construct
 2  Tr b = a;
      COPY
 3  Tr c = std::move(a);
      move
 4  Tr d = make();
      construct
 5  Tr e = pass(d);
      COPY
      move
 6  Tr f = pass(make());
      construct
      move
 7  sink(std::move(e));
      move
 8  sink(Tr("tmp"));
      construct
 9  Tr g = id(f);
      COPY
10  Tr h = pick(cond, b, c);
      COPY
11  Tr i = pick_rv(cond);
      construct
12  Tr j = local_named();
      construct
13  Tr k = local_moved();
      construct
      move
14  b = c;
      COPY-assign
15  b = Tr("rhs");
      construct
      move-assign
16  Tr l = cond ? b : c;
      COPY
17  Tr m = (cond ? b : Tr("z"));
      COPY
```

**Why**, step by step:

| # | Events | Reason |
|:-:|---|---|
| 1 | construct | Direct initialization |
| 2 | COPY | `a` is an lvalue |
| 3 | move | `std::move(a)` is an xvalue → `Tr(Tr&&)` |
| 4 | construct | `make()` is a prvalue; **guaranteed elision**: `d` is initialized in place. (Inside `make`, `Tr("make")` is a prvalue returned directly.) |
| 5 | COPY, move | The parameter `t` is copy-initialized from lvalue `d` (COPY). `return t;` implicitly moves the parameter (move) into the result object |
| 6 | construct, move | `make()` initializes the parameter in place (construct). `return t;` moves it into `f` |
| 7 | move | The parameter is initialized from an xvalue |
| 8 | construct | Prvalue initializes the parameter directly |
| 9 | COPY | `id` returns an lvalue reference; initializing `g` from it copies |
| 10 | COPY | `c ? a : b`, with `a`, `b` references, is an **lvalue**: `return` of a non-local lvalue copies. (Not implicitly moved: `a` and `b` are *references*, not local objects.) |
| 11 | construct | Both branches are prvalues: the chosen one initializes the result **directly** |
| 12 | construct | **NRVO**: the named local `t` is constructed directly in the return slot. *Optional* by the standard; GCC and Clang both do it even at `-O0` |
| 13 | construct, move | `return std::move(t);` yields an xvalue, so NRVO is **not** permitted: the object is constructed locally, then moved. A pessimization ([Chapter 6](06-move-semantics.md)) |
| 14 | COPY-assign | Lvalue right-hand side |
| 15 | construct, move-assign | The prvalue is materialized into a temporary, then move-assigned |
| 16 | COPY | Both operands are lvalues, so the conditional is an **lvalue**; initializing `l` copies |
| 17 | COPY | `b` is an lvalue, `Tr("z")` a prvalue ⇒ the conditional is a **prvalue**; the chosen operand `b` is copied to initialize it (the unchosen `Tr("z")` is never evaluated) |

</details>

Now extend it: add a member function `Tr clone() const & { return *this; }` and `Tr clone() && { return std::move(*this); }`, and add six statements of your own that exercise them. Predict, then check.

---

## 14. Knowledge check

1. What two properties define the three primary value categories? Which combination does not exist, and why?
2. Is `std::move(x)` an lvalue, xvalue or prvalue? What does `std::move` *do* at runtime?
3. `void f(std::string&& s) { g(s); }`: which `g` overload is called for `g(const std::string&)` / `g(std::string&&)`? Why?
4. Why is `make().m` an xvalue and `make()` a prvalue? Which C++ version made this precise, and what concept explains it?
5. `T x = f();` before and after C++17: describe what is constructed, and whether the move constructor must exist.
6. What are the categories of `x++`, `++x`, and `(x, y)`?
7. For `const std::string s; std::string t = std::move(s);`: what is called, and why?
8. `cond ? x : 1`: category? Consequence if `x` is a large object?
9. What is the difference between `decltype(x)` and `decltype((x))` for a variable `x`?
10. Give one API-design use for each of `&`, `&&` and `= delete` ref-qualified overloads.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. *Has identity* and *can be moved from*. lvalue = identity, not movable; xvalue = identity, movable; prvalue = no identity, movable. "No identity and not movable" doesn't exist: if an expression has no identity (just a value), nothing would stop you from using its resources, so it is trivially movable.
2. An **xvalue**. At runtime it does **nothing**: it is `static_cast<remove_reference_t<T>&&>(x)`, a compile-time type change. Any "move" is performed by whatever overload (`T(T&&)`, `operator=(T&&)`, `push_back(T&&)`, …) the xvalue now selects.
3. `g(const std::string&)` is chosen if only that overload exists, and with both overloads `g(std::string&&)` is **not** chosen: `s` is a *name*, so an lvalue, and `g(std::string&&)` cannot bind it. If both exist, the `const&` one wins (or the non-const `&` one if present). To pass it on as an rvalue write `g(std::move(s))`.
4. `make()` is a prvalue (a recipe). Accessing `.m` forces **temporary materialization**, creating a temporary `S`; `.m` of that xvalue (the materialized temporary) is an xvalue. Since **C++17**, because prvalues no longer *are* temporaries.
5. Before: `f()` creates a temporary; `x` is initialized from it by the move/copy constructor (which may be elided, but **must be accessible**, i.e. not deleted). After: `f()` initializes `x` directly; no temporary, no move or copy, so the type does **not** need a move/copy constructor at all (e.g. `std::mutex` can be returned by value from a factory).
6. `x++`: prvalue (old value). `++x`: lvalue (`x` itself). `(x, y)`: the category of `y`.
7. The copy constructor `string(const string&)`. `std::move(s)` has type `const std::string&&`; `string(string&&)` cannot bind a `const` rvalue (it would need to modify the source), but `string(const string&)` can bind it.
8. A **prvalue** (one operand is an lvalue, the other a prvalue). The lvalue operand is *copied* into the result. For a large `x` this is a hidden copy.
9. `decltype(x)` is the **declared type** of `x` (e.g. `int`); `decltype((x))` treats `(x)` as an expression: an lvalue, so `int&`.
10. `&`/`const&`: a getter that borrows (`const std::vector<T>& items() const&`). `&&`: a "take" that moves a member out of an expiring object (`std::vector<T> take() &&`). `&& = delete`: forbids a *view-returning* accessor on temporaries (`string_view name() && = delete;`), preventing dangling.

</details>

---

[← Previous: Chapter 4](../part-02-object-model-and-lifetime/04-raii.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 6 — Move semantics →](06-move-semantics.md)
