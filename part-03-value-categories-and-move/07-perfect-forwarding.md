# Chapter 7 — Perfect Forwarding

> **Part III · Value categories and move semantics** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 5](05-value-categories.md), [Chapter 6](06-move-semantics.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`

[← Previous: Chapter 6](06-move-semantics.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 8 — Templates deep dive →](../part-04-generic-programming/08-templates-deep-dive.md)

---

**In one sentence:** perfect forwarding lets a generic wrapper pass its arguments to another function **exactly as it received them**, preserving both *value category* (lvalue/rvalue) and *constness*, with one template instead of 2ⁿ overloads.

**By the end of this chapter you can:**

- explain template deduction for `T&&`, and why it is *not* an rvalue reference there
- apply the reference-collapsing rules by hand
- write `std::forward` from memory and explain every token
- write wrappers, factories and constructors that forward correctly, and know when **not** to
- implement `std::invoke` (member pointers included) with correct return-type forwarding
- list the arguments that **cannot** be perfectly forwarded and why

---

## 1. Problem

You write a function that creates an object and needs to hand your arguments to its constructor:

```cpp
template <class T, class Arg>
std::unique_ptr<T> make(Arg arg) { return std::unique_ptr<T>(new T(arg)); }
```

What goes wrong?

| Signature | Problem |
|---|---|
| `make(Arg arg)` | Copies every argument. A move-only argument does not compile. |
| `make(const Arg& arg)` | Cannot move; **cannot bind a non-const lvalue reference parameter** of `T`'s constructor (`T(int&)`) |
| `make(Arg& arg)` | Rejects temporaries: `make<T>(5)` does not compile |
| both `const Arg&` and `Arg&` | Handles lvalues, still copies rvalues |
| **all combinations** | For *n* arguments you need **2ⁿ** overloads (`&`, `const&`, `&&` per parameter makes it 3ⁿ) |

The wrapper must be *transparent*: if the caller writes `make<T>(x)` it should behave as if they wrote `T(x)` directly; if they write `make<T>(std::move(x))`, as `T(std::move(x))`.

---

## 2. Historical context

| Era | Approach | Limits |
|---|---|---|
| C++98 | Hand-written overloads on `const T&` / `T&` (Boost.Bind and `make_pair` had up to 9 each, generated with preprocessor tricks) | Exponential; *could not* forward rvalues at all |
| C++98 | `boost::ref` / `reference_wrapper` to opt into reference semantics explicitly | Callers must remember |
| C++11 | **Forwarding references + `std::forward` + variadic templates** (N1385, N2951: *"the forwarding problem"*) | The solution; this chapter |
| C++14 | Generic lambdas: `[](auto&&... args) { return f(std::forward<decltype(args)>(args)...); }` | |
| C++20 | Pack init-captures `[...a = std::forward<A>(a)]`, `std::bind_front`, concepts to constrain forwarding constructors | Fixes the worst pitfalls |
| C++23 | `std::forward_like`, deducing `this` | Forward a *member* with the category of the owner |

The solution was not a new feature so much as an interaction of three: a **special deduction rule** for `T&&`, **reference collapsing**, and **`std::forward`**. All three are needed. Each looks arbitrary alone; together they solve the problem with no overhead.

---

## 3. Modern solution

```cpp
template <class T, class... Args>
std::unique_ptr<T> make(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
```

Three ingredients:

1. `Args&&` in a deduced context is a **forwarding reference**: it binds to *anything*, and `Args` records *what it bound to*.
2. **Reference collapsing**: `Args = X&` makes `Args&&` collapse to `X&`.
3. `std::forward<Args>(arg)` reconstructs the original category: an lvalue stays an lvalue, an rvalue becomes an xvalue.

---

## 4. Mental model

### A forwarding reference is "a reference that remembers what it bound to"

```text
    call                    deduced Args     parameter type (Args&&)    what the function sees
    ─────────────────────   ──────────────   ────────────────────────   ──────────────────────
    f(x)       x: int        int&             int& && → int&             an lvalue reference
    f(cx)      cx: const int const int&       const int&                 a const lvalue reference
    f(5)       prvalue       int              int&&                      an rvalue reference
    f(std::move(x))          int              int&&                      an rvalue reference
```

The *category of the argument is encoded in the deduced type*:

- **lvalue** argument of type `X` → `T = X&`
- **rvalue** argument of type `X` → `T = X`

`std::forward<T>(a)` then **decodes** it: it casts `a` to `T&&`. With `T = X&` that is `X& &&` = `X&` (an lvalue). With `T = X` it is `X&&` (an xvalue). Nothing else is needed.

### The mnemonic

> **`T&&` with `T` deduced is a *forwarding* reference. Everything else with `&&` is an *rvalue* reference.**
> A forwarding reference is forwarded (`std::forward`). An rvalue reference is moved (`std::move`).

Which is the single most important distinction in this chapter; Experiment 1 drills it.

### Forwarding vs moving

```cpp
void sink(std::string&& s) { store(std::move(s)); }                // rvalue ref → move: it IS an rvalue

template <class T>
void fwd(T&& t)             { store(std::forward<T>(t)); }         // forwarding ref → forward
```

If you `std::move` a forwarding reference you **steal from the caller's lvalue** (a silent bug). If you `std::forward` an rvalue reference, it works but misleads the reader.

---

## 5. Language rules

### 5.1 The deduction special case  `[temp.deduct.call]`

For a function template parameter of the form **`T&&`**, where `T` is a **template parameter of that function template** (not of the enclosing class!), and the call argument is an lvalue of type `U`, then `U&` is used in place of `U` for deduction. So `T` is deduced as `U&`.

> *Terminology.* The standard calls this a **forwarding reference** `[temp.deduct.call]/3`. Scott Meyers coined *universal reference* in 2012, before the standard had a name; both mean the same thing, and **forwarding reference** is the official term.

What is **not** a forwarding reference:

| Declaration | Why not |
|---|---|
| `void f(std::vector<T>&& v)` | `&&` applied to `vector<T>`, not to a bare `T` |
| `void f(const T&& t)` | `const`-qualified → pure rvalue reference |
| `template <class T> struct S { void f(T&& t); };` | `T` is the *class'* parameter, already fixed when `f` is called |
| `void f(int&& x)` | No deduction |
| `template <class T> void f(T&& t)` | ✅ This one *is* |
| `auto&& x = expr;` | ✅ `auto` deduction follows the same rule |
| `for (auto&& x : range)` | ✅ |
| `[](auto&& x) {}` | ✅ |
| `template <class... Ts> void f(Ts&&... ts)` | ✅ each element |

### 5.2 Reference collapsing  `[dcl.ref]/6`

Forming a reference to a reference is **ill-formed written directly**, but is allowed via a typedef, `decltype`, or template parameter, and then it *collapses*:

| `T` | `T&` | `T&&` |
|---|---|---|
| `X` | `X&` | `X&&` |
| `X&` | `X&` | `X&` |
| `X&&` | `X&` | `X&&` |

**An rvalue reference results only if both are rvalue references.** Any `&` involved wins:

```text
   & + &   → &
   & + &&  → &
   && + &  → &
   && + && → &&
```

### 5.3 `std::forward`  `[forward]`

```cpp
template <class T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {          // overload 1: lvalues
    return static_cast<T&&>(t);
}
template <class T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {         // overload 2: rvalues
    static_assert(!is_lvalue_reference_v<T>);                       // forwarding an rvalue as an lvalue is a bug
    return static_cast<T&&>(t);
}
```

Details that matter:

- The parameter is `remove_reference_t<T>&`, a **non-deduced context**: you **must write `T` explicitly**. `std::forward(x)` doesn't compile; that is deliberate, so the type argument cannot be deduced to the wrong thing.
- `static_cast<T&&>(t)`: with `T = X&` yields `X&`, with `T = X` yields `X&&`. Same as §4.
- Overload 2 exists so that `std::forward<T>(std::move(x))`-style calls on a genuine rvalue also work, and so the `static_assert` can reject forwarding an rvalue *as an lvalue*.
- **`std::move(x)` always yields an rvalue. `std::forward<T>(x)` yields an rvalue only if `T` is not an lvalue reference.** `std::forward` is a *conditional* `std::move`.

### 5.4 Variadic packs  `[temp.variadic]`

```cpp
template <class... Args>
void f(Args&&... args) {
    g(std::forward<Args>(args)...);       // pack expansion: g(forward<A0>(a0), forward<A1>(a1), …)
}
```

The pattern `std::forward<Args>(args)` is expanded in lock-step over both packs. Use `sizeof...(Args)` for the count.

### 5.5 What cannot be perfectly forwarded

A wrapper `template <class... A> void wrap(A&&... a) { target(std::forward<A>(a)...); }` fails (does not behave like `target(...)`) for these arguments:

| Argument | Why |
|---|---|
| **Braced-init-list** `{1, 2, 3}` | Not an expression; has no type, so `A` cannot be deduced. Workaround: explicit `std::initializer_list<int>` overload, or the caller writes `std::vector<int>{1,2,3}` |
| **`0` / `NULL` as a null pointer constant** | Deduces `int` / `long`; the *constant-ness* is lost, so it no longer converts to a pointer. Use `nullptr` |
| **`static const` integral data member** declared but never defined (pre-C++17, not `inline`) | Binding a reference ODR-uses it → link error. C++17 `inline` variables/`constexpr` members fix this |
| **Overloaded function name or function template name** | Cannot deduce which overload. Workaround: cast or `[](auto&&... a) { return f(std::forward<decltype(a)>(a)...); }` |
| **Bit-field** | A non-const reference cannot bind to a bit-field. Workaround: copy into a temporary |

### 5.6 Interaction with overloading: the greedy template

A forwarding-reference overload is an **exact match for everything**. That makes it *better* than a `const T&` overload in precisely the cases where you didn't want it. See Experiment 3, and the fix in §7.

### 5.7 C++23: `std::forward_like`

`std::forward_like<Owner>(member)` forwards `member` with the **constness and category of `Owner`**. It replaces the error-prone `std::forward<decltype(self)>(self).member` and works naturally with *deducing this* (`this Self&& self`).

```cpp
// @test skip C++23 forward_like: shown for the API shape; exercised in Chapter 8
struct Box {
    std::string s;
    template <class Self>
    auto&& get(this Self&& self) { return std::forward_like<Self>(self.s); }
};
```

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Forwarding-reference deduction, collapsing, `std::forward`, non-deducible arguments |
| **Compiler** | Instantiates one function per distinct `Args...`; inlines the wrapper chain |
| **ABI** | References are pointers; forwarding wrappers don't change how arguments are passed. **A by-value parameter of class type is passed by hidden pointer; a forwarded reference is a pointer to the caller's object** |
| **CPU** | A correctly forwarded call usually compiles to a direct jump/call to the target (Experiment 5) |

---

## 6. Implementation model

Every distinct combination of argument categories creates a **separate instantiation**:

```cpp
make<Foo>(a);             // Args = {A&}         → make<Foo, A&>
make<Foo>(std::move(a));  // Args = {A}          → make<Foo, A>
make<Foo>(ca);            // Args = {const A&}   → make<Foo, const A&>
```

Template instantiation cost is real: forwarding wrappers grow compile times and binary size (each instantiation is its own function, unless merged by the linker's identical-code folding). They're also **the main source of 100-line error messages** (before concepts, Chapter 10).

At `-O2` the wrapper is usually inlined away completely. `std::forward` and `std::move` are `constexpr` functions containing one `static_cast` and always inline: there is nothing to execute.

---

## 7. Experiments

### Experiment 1: What does `T` deduce to? What is `T&&`?

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <type_traits>
#include <utility>

template <class X>
constexpr const char* name() {
    using U = std::remove_reference_t<X>;
    constexpr bool c = std::is_const_v<U>;
    if constexpr (std::is_lvalue_reference_v<X>)  return c ? "const int&"  : "int&";
    else if constexpr (std::is_rvalue_reference_v<X>) return c ? "const int&&" : "int&&";
    else                                              return c ? "const int"   : "int";
}

template <class T> void fwd_ref(T&&)          { std::printf("  T = %-12s param = %s\n", name<T>(), name<T&&>()); }
template <class T> void by_const_rref(const T&&) { std::printf("  T = %-12s param = %s   (const T&&: NOT forwarding)\n", name<T>(), name<const T&&>()); }
template <class T> void by_value(T)           { std::printf("  T = %-12s (by value: decay, no references)\n", name<T>()); }

int  lval() { return 1; }
const int  clval() { return 1; }

int main() {
    int x = 1;
    const int cx = 2;
    int& rx = x;
    int&& rrx = 3;

    std::puts("forwarding reference  template<class T> void f(T&&):");
    fwd_ref(x);                 // lvalue
    fwd_ref(cx);                // const lvalue
    fwd_ref(rx);                // an lvalue of type int (the reference vanishes in an expression)
    fwd_ref(rrx);               // a NAME: lvalue!
    fwd_ref(5);                 // prvalue
    fwd_ref(lval());            // prvalue
    fwd_ref(std::move(x));      // xvalue
    fwd_ref(clval());           // const prvalue → int (top-level const of a prvalue of non-class type is dropped)
    std::puts("const T&&:  only rvalues bind");
    by_const_rref(5);
    by_const_rref(std::move(x));
    std::puts("by value:");
    by_value(x); by_value(cx); by_value(rx);
    (void)rrx;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
forwarding reference  template<class T> void f(T&&):
  T = int&         param = int&
  T = const int&   param = const int&
  T = int&         param = int&
  T = int&         param = int&
  T = int          param = int&&
  T = int          param = int&&
  T = int          param = int&&
  T = int          param = int&&
const T&&:  only rvalues bind
  T = int          param = const int&&   (const T&&: NOT forwarding)
  T = int          param = const int&&   (const T&&: NOT forwarding)
by value:
  T = int          (by value: decay, no references)
  T = int          (by value: decay, no references)
  T = int          (by value: decay, no references)
```

Remember the two surprises:

- `fwd_ref(rrx)` deduces `T = int&`: `rrx` is an **lvalue** (a name) even though it was declared `int&&`. That is why forwarding a name requires `std::forward`. It's the same observation as Chapter 5.
- `fwd_ref(rx)` deduces `int&` — *not* `int& &`; the reference-ness of the *declared* type of `rx` is irrelevant to the *expression* `rx`.

### Experiment 2: Wrapper behaviour, five ways

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <utility>

// The target records which overload of f it received.
void f(int&)        { std::puts("    f(int&)"); }
void f(const int&)  { std::puts("    f(const int&)"); }
void f(int&&)       { std::puts("    f(int&&)"); }
void f(const int&&) { std::puts("    f(const int&&)"); }

template <class T> void wrap_value(T t)             { f(t); }                         // by value
template <class T> void wrap_constref(const T& t)   { f(t); }                         // const&
template <class T> void wrap_noforward(T&& t)       { f(t); }                         // forwarding ref, forgot forward
template <class T> void wrap_move(T&& t)            { f(std::move(t)); }              // BUG: always moves
template <class T> void wrap_forward(T&& t)         { f(std::forward<T>(t)); }        // correct

int main() {
    int x = 1; const int cx = 2;
    std::puts("wrap_value (by value)");
    std::printf("  lvalue:       "); wrap_value(x);   std::printf("  const lvalue: "); wrap_value(cx);   std::printf("  rvalue:       "); wrap_value(3);
    std::puts("wrap_constref");
    std::printf("  lvalue:       "); wrap_constref(x); std::printf("  const lvalue: "); wrap_constref(cx); std::printf("  rvalue:       "); wrap_constref(3);
    std::puts("wrap_noforward (forgot std::forward)");
    std::printf("  lvalue:       "); wrap_noforward(x); std::printf("  const lvalue: "); wrap_noforward(cx); std::printf("  rvalue:       "); wrap_noforward(3);
    std::puts("wrap_move (std::move on a forwarding reference)");
    std::printf("  lvalue:       "); wrap_move(x);   std::printf("  const lvalue: "); wrap_move(cx);   std::printf("  rvalue:       "); wrap_move(3);
    std::puts("wrap_forward (correct)");
    std::printf("  lvalue:       "); wrap_forward(x); std::printf("  const lvalue: "); wrap_forward(cx); std::printf("  rvalue:       "); wrap_forward(3);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
wrap_value (by value)
  lvalue:           f(int&)
  const lvalue:     f(int&)
  rvalue:           f(int&)
wrap_constref
  lvalue:           f(const int&)
  const lvalue:     f(const int&)
  rvalue:           f(const int&)
wrap_noforward (forgot std::forward)
  lvalue:           f(int&)
  const lvalue:     f(const int&)
  rvalue:           f(int&)
wrap_move (std::move on a forwarding reference)
  lvalue:           f(int&&)
  const lvalue:     f(const int&&)
  rvalue:           f(int&&)
wrap_forward (correct)
  lvalue:           f(int&)
  const lvalue:     f(const int&)
  rvalue:           f(int&&)
```

`wrap_value` and `wrap_constref` lose information in the other direction: by value, even a `const` lvalue becomes a fresh non-const `int` (so `f(int&)` is picked); by `const&`, everything arrives as `const int&`. Only `wrap_forward` reproduces what the direct call `f(x)` / `f(cx)` / `f(3)` would do. `wrap_noforward` turns every rvalue into an lvalue (copies where moves were possible). `wrap_move` is worse: **it turns the caller's lvalue into an rvalue**: `wrap_move(x)` selects `f(int&&)`, and if `f` had been a sink that moves from its argument, the caller's `x` would be silently gutted.

### Experiment 3: The greedy forwarding constructor

A classic: a class that has *both* a copy constructor and a forwarding constructor.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

// BROKEN: the forwarding constructor is a better match for non-const lvalues than the copy constructor.
struct Person {
    std::string name;
    template <class T>
    explicit Person(T&& n) : name(std::forward<T>(n)) { std::puts("  Person(T&&) forwarding ctor"); }
    Person(const Person& o) : name(o.name)            { std::puts("  Person(const Person&) copy ctor"); }
    Person(Person&&) noexcept = default;
};

// FIXED (C++20): constrain the template so it can't hijack Person arguments.
struct Person2 {
    std::string name;
    template <class T>
        requires (!std::is_same_v<std::remove_cvref_t<T>, Person2>)
                 && std::is_constructible_v<std::string, T>
    explicit Person2(T&& n) : name(std::forward<T>(n)) { std::puts("  Person2(T&&) forwarding ctor"); }
    Person2(const Person2& o) : name(o.name)           { std::puts("  Person2(const Person2&) copy ctor"); }
    Person2(Person2&&) noexcept = default;
};

int main() {
    std::puts("--- broken ---");
    Person p1("Ada");                 // OK: T = const char(&)[4]
    const Person cp(p1.name);         // lvalue std::string
    Person p2(cp);                    // const lvalue Person: copy ctor wins (const& exact match, non-template)
    std::puts("copying a NON-const lvalue Person:");
    // Person p3(p1);                 // ERROR: picks the template with T = Person&,
    //                                //        then std::string(Person&) does not compile!
    std::puts("  (Person p3(p1) does not compile: see the commented line)");

    std::puts("--- fixed ---");
    Person2 q1("Ada");
    Person2 q2(q1);                   // non-const lvalue: copy ctor, now that the template is constrained
    Person2 q3(std::string("Grace"));
    (void)p2; (void)q2; (void)q3;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
--- broken ---
  Person(T&&) forwarding ctor
  Person(T&&) forwarding ctor
  Person(const Person&) copy ctor
copying a NON-const lvalue Person:
  (Person p3(p1) does not compile: see the commented line)
--- fixed ---
  Person2(T&&) forwarding ctor
  Person2(const Person2&) copy ctor
  Person2(T&&) forwarding ctor
```

Why `Person p3(p1)` fails: `p1` is a **non-const lvalue**. Candidates: the copy constructor wants `const Person&` (needs a qualification conversion on the reference), while the template deduces `T = Person&` and gets `Person&`: an **exact match**. Exact beats "adds const", so the template wins and then tries to build a `std::string` from a `Person`.

> **Rule.** A single-argument forwarding constructor must **always be constrained** so it never accepts the class itself (or classes derived from it). In C++20, use a `requires` clause. Before concepts you needed `std::enable_if_t<!std::is_base_of_v<Person, std::decay_t<T>>>`. This is the best single reason to *avoid* single-argument forwarding constructors unless they're needed: the by-value-then-move idiom (Chapter 6, §10) is simpler and has no such trap.

This is also exactly the bug that Scott Meyers' *Effective Modern C++* Item 26 warns about, and the reason `std::optional`, `std::any` and `std::function`'s converting constructors are all constrained.

### Experiment 4: Implement `std::invoke`

`std::invoke(f, args...)` calls *anything callable*, including pointers to member functions and data members, with the right forwarding. It is a forwarding exercise in its purest form.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace my {

namespace detail {
    template <class> struct is_refwrap : std::false_type {};
    template <class U> struct is_refwrap<std::reference_wrapper<U>> : std::true_type {};

    // Extract the class type C from a member pointer type M C::*
    template <class M, class C> C class_of(M C::*);
    template <class P> using class_of_t = decltype(class_of(std::declval<P>()));

    // Does T1 denote an object of (or derived from) class C (as opposed to a pointer or reference_wrapper)?
    template <class C, class T1>
    inline constexpr bool is_object_of_v = std::is_base_of_v<C, std::remove_cvref_t<T1>>;
}

template <class F, class... Args>
constexpr decltype(auto) invoke(F&& f, Args&&... args)
    noexcept(noexcept(std::invoke(std::forward<F>(f), std::forward<Args>(args)...)))   // borrow the spec for this teaching version
{
    using Fd = std::decay_t<F>;

    if constexpr (std::is_member_pointer_v<Fd>) {
        static_assert(sizeof...(Args) >= 1, "member pointer needs an object");
        using C = detail::class_of_t<Fd>;

        // Peel off the object argument (first) and keep the rest
        return [&]<class T1, class... Rest>(T1&& obj, Rest&&... rest) -> decltype(auto) {
            if constexpr (std::is_member_function_pointer_v<Fd>) {
                if constexpr (detail::is_object_of_v<C, T1>)
                    return (std::forward<T1>(obj).*f)(std::forward<Rest>(rest)...);   // obj.*f(args)
                else if constexpr (detail::is_refwrap<std::remove_cvref_t<T1>>::value)
                    return (obj.get().*f)(std::forward<Rest>(rest)...);               // reference_wrapper
                else
                    return ((*std::forward<T1>(obj)).*f)(std::forward<Rest>(rest)...);// (*ptr).*f(args)
            } else {                                       // pointer to data member
                static_assert(sizeof...(Rest) == 0, "data member pointer takes only the object");
                if constexpr (detail::is_object_of_v<C, T1>)
                    return std::forward<T1>(obj).*f;
                else if constexpr (detail::is_refwrap<std::remove_cvref_t<T1>>::value)
                    return obj.get().*f;
                else
                    return (*std::forward<T1>(obj)).*f;
            }
        }(std::forward<Args>(args)...);
    } else {
        return std::forward<F>(f)(std::forward<Args>(args)...);                       // ordinary callable
    }
}

} // namespace my

struct Widget {
    int id = 7;
    std::string name = "w";
    int  add(int a, int b) const { return id + a + b; }
    std::string& label()   & { return name; }
    std::string  label()   && { return std::move(name); }
};
int twice(int x) { return 2 * x; }
int& pick(int& a, int&) { return a; }

int main() {
    Widget w; const Widget cw;
    int a = 1, b = 2;

    std::printf("free function       : %d\n", my::invoke(twice, 21));
    std::printf("lambda              : %d\n", my::invoke([](int x, int y) { return x * y; }, 6, 7));
    std::printf("member function     : %d\n", my::invoke(&Widget::add, w, 1, 2));
    std::printf("... via const obj   : %d\n", my::invoke(&Widget::add, cw, 1, 2));
    std::printf("... via pointer     : %d\n", my::invoke(&Widget::add, &w, 1, 2));
    std::printf("... via ref_wrapper : %d\n", my::invoke(&Widget::add, std::ref(w), 1, 2));
    std::printf("data member         : %d\n", my::invoke(&Widget::id, w));
    std::printf("data member via ptr : %d\n", my::invoke(&Widget::id, &w));

    // The result category must be preserved:
    static_assert(std::is_same_v<decltype(my::invoke(pick, a, b)), int&>);               // lvalue ref passes through
    static_assert(std::is_same_v<decltype(my::invoke(&Widget::id, w)), int&>);           // data member of lvalue: int&
    static_assert(std::is_same_v<decltype(my::invoke(&Widget::id, std::move(w))), int&&>); // of rvalue: int&&
    static_assert(std::is_same_v<decltype(my::invoke(&Widget::id, cw)), const int&>);
    // &Widget::label names an OVERLOAD SET, which cannot be deduced (§5.5), so pick one explicitly:
    constexpr auto label_l = static_cast<std::string& (Widget::*)() &>(&Widget::label);
    constexpr auto label_r = static_cast<std::string  (Widget::*)() &&>(&Widget::label);
    static_assert(std::is_same_v<decltype(my::invoke(label_l, w)), std::string&>);
    static_assert(std::is_same_v<decltype(my::invoke(label_r, Widget{})), std::string>);

    my::invoke(pick, a, b) = 99;                  // assigning through the returned reference
    std::printf("assigned through invoke: a = %d\n", a);
    std::puts("static_asserts passed: result category and constness are forwarded exactly");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
free function       : 42
lambda              : 42
member function     : 10
... via const obj   : 10
... via pointer     : 10
... via ref_wrapper : 10
data member         : 7
data member via ptr : 7
assigned through invoke: a = 99
static_asserts passed: result category and constness are forwarded exactly
```

What to notice:

- **Two levels of forwarding.** `F&& f` forwards the callable; `Args&&... args` the arguments. For member pointers the first argument is the *object* and has to be forwarded too, because `&Widget::label` has `&` and `&&` overloads and the choice depends on the object's category (see the last two `static_assert`s). Note that `&Widget::label` could not be passed directly: it names an overload set, so the test selects an overload with a cast, which is the §5.5 limitation in action.
- **`decltype(auto)` is essential.** With `auto` the return type would decay: `pick` returns `int&`, but `auto` would copy and `invoke(pick, a, b) = 99` would not modify `a`. `decltype(auto)` returns *exactly what the callee returned*, which is the other half of "perfect": **perfect forwarding of arguments, and perfect forwarding of the result**.
- **`noexcept` is borrowed here** to keep the teaching version short; a real implementation writes the `is_nothrow_invocable` logic once in a trait.
- The `INVOKE` rules are the specification of `[func.require]`: seven cases in the standard: three for member functions, three for data members (object, `reference_wrapper`, pointer) and the ordinary callable, which are the branches above. It is **the** definition of "callable" in the library. `std::function`, `std::thread`, `std::async`, `std::bind`, all algorithms with a predicate, `std::apply`, `std::visit` and ranges projections call through it. So everything works with member pointers: `std::ranges::sort(people, {}, &Person::name)`.

### Experiment 5: The wrapper disappears

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=direct,wrapped
#include <string>
#include <utility>

void consume(std::string&&);
void consume(const std::string&);

template <class T>
void forward_to_consume(T&& t) { consume(std::forward<T>(t)); }

void direct(std::string&& s)                  { consume(std::move(s)); }
void wrapped(std::string&& s)                 { forward_to_consume(std::move(s)); }
void wrapped_lvalue(const std::string& s)     { forward_to_consume(s); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
direct(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&):
	jmp	consume(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&)@PLT

wrapped(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&):
	jmp	consume(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&)@PLT

wrapped_lvalue(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&):
	jmp	consume(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)@PLT
```

`direct` and `wrapped` are the **same code**: a tail `jmp` to `consume(std::string&&)`. The forwarding wrapper has no representation after inlining. `wrapped_lvalue` jumps to `consume(const std::string&)`. This is the whole point: **abstraction with no residual cost**, with the compiler proving at compile time which overload applies.

Compare the same at `-O0` and count the instructions of `std::forward` and `forward_to_consume`: each is a real function at `-O0` (a few instructions), and gone at `-O1` and above. This is one reason why debug builds of template-heavy code are 10–50× slower than release builds.

---

## 8. Assembly / runtime investigation

To see *which instantiations* the compiler created for your wrapper:

```bash
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o
nm -C prog.o | grep forward_to_consume
#   W void forward_to_consume<std::string>(std::string&&)
#   W void forward_to_consume<std::string const&>(std::string const&)
```

Each distinct `T` is a separate **weak symbol** (`W`: Chapter 36 explains why they do not clash across translation units). Count them in a real codebase's `-O0` object files to find the `make_*`/`emplace` instantiation explosion. Tools like `clang -ftime-trace` and `nm -S --size-sort -C` show you which are expensive.

To see deduced types **in a compile error**, use the "undefined template" trick:

```cpp
// @test fail -std=c++23 err=incomplete
template <class T> struct ShowType;                         // declared, never defined
template <class T> void f(T&&) { ShowType<T> x; }           // error names T
int main() { int i = 0; f(i); }
```

---

## 9. Implementation exercise

Implement the following, using only `<utility>` and `<new>`:

1. `my::forward<T>(t)` and `my::move(t)` (both overloads for `forward`).
2. `my::make_unique<T>(args...)` returning your `UniquePtr` from Chapter 6, with `static_assert` to reject array types.
3. `my::construct_at(T* p, Args&&...)`, equivalent to `::new (static_cast<void*>(p)) T(std::forward<Args>(args)...)`. Note that `std::construct_at` is `constexpr`, but placement `new` is not allowed there; consider why `std::construct_at` needs compiler support.
4. `my::emplace_back` for a minimal `StaticVec<T, N>`, backed by `alignas(T) unsigned char buf[N * sizeof(T)]`. Test with a type that has *only* a move constructor, and one with an `explicit` multi-argument constructor.

<details>
<summary><strong>Solution sketch for 1 and 3</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace my {

template <class T>
constexpr std::remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}

template <class T>
constexpr T&& forward(std::remove_reference_t<T>& t) noexcept { return static_cast<T&&>(t); }

template <class T>
constexpr T&& forward(std::remove_reference_t<T>&& t) noexcept {
    static_assert(!std::is_lvalue_reference_v<T>, "forwarding an rvalue as an lvalue");
    return static_cast<T&&>(t);
}

template <class T, class... Args>
T* construct_at(T* p, Args&&... args) {
    return ::new (static_cast<void*>(p)) T(my::forward<Args>(args)...);
}

} // namespace my

struct Probe {
    std::string tag;
    Probe(std::string t, int n) : tag(std::move(t)) { std::printf("  Probe(%s, %d)\n", tag.c_str(), n); }
    Probe(Probe&&) noexcept = default;
    ~Probe() = default;
};

int main() {
    alignas(Probe) unsigned char storage[sizeof(Probe)];
    std::string s = "named";
    Probe* p = my::construct_at(reinterpret_cast<Probe*>(storage), std::move(s), 42);
    std::printf("constructed: tag=%s; source string moved-from size=%zu\n", p->tag.c_str(), s.size());
    p->~Probe();

    static_assert(std::is_same_v<decltype(my::forward<int&>(std::declval<int&>())), int&>);
    static_assert(std::is_same_v<decltype(my::forward<int>(std::declval<int&>())), int&&>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  Probe(named, 42)
constructed: tag=named; source string moved-from size=0
```

</details>

---

## 10. Real-world example

Where forwarding is the load-bearing mechanism in the standard library:

| API | Forwarding does… |
|---|---|
| `std::make_unique`, `std::make_shared`, `std::make_optional` | forward arguments to `T`'s constructor |
| `vector::emplace_back`, `map::try_emplace`, `optional::emplace` | construct the element *in place* from the arguments (no temporary, no move) |
| `std::thread(f, args...)`, `std::async`, `std::jthread` | copy/move the callable and arguments into storage owned by the thread (`std::decay_t` on each), then invoke them with `std::invoke` |
| `std::function::operator()`, `std::move_only_function`, `std::bind_front` | forward call arguments to the stored callable |
| `std::apply`, `std::make_from_tuple`, `std::visit` | forward tuple elements / variant alternatives to the callee |
| `std::pair`/`tuple` converting constructors | forward each element (and are the textbook *constrained* forwarding constructors) |
| `std::ranges::views::transform(f)`, projections | `std::invoke` over forwarded elements |

**Note the asymmetry in `std::thread`.** `std::thread t(f, x)` **decays and copies** `x` into the thread, even when `f` takes `int&`: that is intentional, since passing a reference to a caller's local into another thread is a lifetime hazard. You opt in with `std::ref(x)`. Perfect forwarding of a *reference* across a thread boundary would be perfect forwarding of a dangling hazard. This is a design decision worth remembering when you design your own task-queue API (Project 6).

### Verdict: when *not* to use perfect forwarding

| Situation | Better |
|---|---|
| A function that **stores** one cheap-to-move argument | `void set(std::string s) { s_ = std::move(s); }` : by value, one overload, no template |
| A function that **only reads** its argument | `const T&`, or by value for small trivially copyable types, `std::string_view`, `std::span` |
| A *public* API where error messages and compile time matter | Concrete overloads, or constrain with a concept |
| A class constructor taking one argument | By value + move, or a constrained forwarding constructor (Experiment 3) |
| A hot path | Measure: instantiation per category = more code, may hurt the instruction cache |

Use forwarding for **wrappers, factories, `emplace`-style construction and callable adaptors**: where you genuinely don't know the target's signature. If you know it, spell it.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Forgetting `std::forward` | Rvalues become lvalues: copies instead of moves; move-only types fail to compile | `std::forward<T>(t)` |
| `std::move` on a forwarding reference | Caller's lvalues are **emptied** | `std::forward` |
| `std::forward<decltype(t)>(t)` mistaken for `forward<T>` on a non-forwarding param | Works for `auto&&` but is noise elsewhere | Use the template parameter directly |
| Forwarding the same argument **twice** | Second use sees a moved-from object | Forward **once**, at the last use; keep copies for earlier uses |
| Greedy forwarding constructor | Hijacks copy/move (Experiment 3); baffling errors | `requires` constraint excluding the class itself |
| `template <class T> struct S { void f(T&&); };` | Not forwarding; `S<int&>`'s `f` takes `int&`, but `S<int>`'s takes only rvalues | Make `f` a template: `template <class U> void f(U&&)` |
| Passing `{...}`, `0`, `NULL`, overloaded names, bit-fields through a wrapper | Deduction failure or wrong overload | `nullptr`; explicit types; lambda wrapper |
| `auto` return type in a forwarding wrapper | Decays: returned references are copied | `decltype(auto)` return, or `-> decltype(f(std::forward<Args>(args)...))` |
| Capturing forwarded arguments in a lambda by reference | Dangles when the lambda outlives the call | C++20 pack init-capture: `[...a = std::forward<Args>(args)]` |
| `std::forward` where `std::move` is meant (and vice versa) | Silent behaviour change | Mnemonic: *deduced `T&&` → forward; known rvalue-ref → move* |
| Assuming a forwarding function is "free" | Instantiation per category, long error messages, compile time | Constrain with a concept; prefer concrete overloads in stable APIs |
| Returning `std::forward<T>(t)` from a function returning by value | Moves from the caller's argument if it was an rvalue: usually right, but check the lifetime when returning `T&&` | Return by value; return `T&&` only for `forward`-like helpers |

---

## 12. Exercises

1. **Deduction drills.** Without compiling, give `T` and the parameter type for `f(T&&)`, `g(const T&)` and `h(T)` when called with: `int`, `const int`, `int&`, an array `int[3]`, a string literal `"ab"`, a function name, `std::move(x)`, `nullptr`. Then check with Experiment 1's `name<T>()` (extend it).
2. **Break it deliberately.** Take Experiment 2's `wrap_forward` and call it with `{1, 2}`, `0` (for a function taking `int*`), a bit-field and an overloaded function name. Record the error messages and learn to recognise each.
3. **Constrained `Any`-like constructor.** Write `class Holder { template <class T> Holder(T&& v); }` that stores a `std::decay_t<T>` in a `std::string`-or-`int` variant. Constrain it so that it never competes with copy/move, and so that an `std::vector<int>` argument gives a clear `requires` error rather than a deep template backtrace.
4. **Make `thread` safe.** Write `my::spawn(f, args...)` which decays-copies the arguments like `std::thread` does and invokes with `my::invoke`. Prove with a test that passing a local by `std::ref` is the *only* way to share it.
5. **`bind_front`.** Implement `my::bind_front(f, bound...)` returning a callable that stores `std::decay_t` copies of `f` and the bound arguments and forwards the call arguments after them. It must propagate `const`/`&`/`&&` qualification of the call operator correctly (hint: deducing `this`, or write four `operator()` overloads).
6. **Measure instantiations.** Compile a small project's translation units with `-ftime-trace` (Clang) and list the top 10 templates by instantiation time. How many are forwarding wrappers? Replace one with concrete overloads and measure the compile-time and binary-size difference.
7. **`forward_like`.** Using GCC 14 with `-std=c++23`, implement a getter for a member `std::string` in a class using deducing `this`, which returns `std::string&`, `const std::string&`, or `std::string&&` depending on the object's category. Check each case with `static_assert(std::is_same_v<…>)`.

---

## 13. Challenge: a generic `Retry` wrapper

Design `retry(n, f, args...)` that calls `std::invoke(f, args...)` up to *n* times until it does not throw, with these requirements:

- It must preserve the **return type exactly**, including references (`decltype(auto)`), and `void`.
- **Arguments must not be forwarded as rvalues on the first attempt** if a retry may need them again. Decide, and *document*, the semantics: the call is a retry, so the arguments must stay valid across attempts. Pass them as lvalues (or copies), and make move-only arguments a compile-time error with a clear message via a concept.
- `noexcept` must reflect whether `f` can throw.
- It must work with member function pointers and reference-wrapped arguments.
- Provide an overload taking a *backoff policy* object (callable `std::chrono::milliseconds(int attempt)`), selected by tag type or concept.

Write tests under ASan/UBSan, including a callable that returns `int&` (a reference into a static) and a move-only callable. Then answer: **why does perfect forwarding contradict "call repeatedly"?** (Forwarding says *consume once*; retrying says *use many times*. A wrapper that retries cannot be perfectly forwarding. This is a design lesson: perfect forwarding is for *single-use* calls.)

---

## 14. Knowledge check

1. What distinguishes a forwarding reference from an rvalue reference? Give three declarations of each.
2. For `template <class T> void f(T&&)`, what is `T` when called with an lvalue `int`, a `const int` lvalue, and the literal `5`?
3. State the four collapsing rules. Which single case yields an rvalue reference?
4. Why does `std::forward` take its parameter as `remove_reference_t<T>&` and require an explicit template argument?
5. Inside `void f(Widget&& w)`, `w` is passed to `g(Widget&&)`. What is required and why?
6. What is the bug in `template <class T> void put(T&& v) { store(std::move(v)); }`?
7. Why does `Person(T&&)` hijack `Person p2(p1)` for a non-const lvalue `p1`? Give two fixes.
8. List four argument kinds that cannot be perfectly forwarded.
9. Why must `invoke` return `decltype(auto)` instead of `auto`?
10. Why does `std::thread` decay-copy its arguments rather than forward them?
11. What is the difference between `std::forward<T>(x)` and `static_cast<T&&>(x)`? Is there one?
12. `template <class T> struct S { void f(T&&); };` — forwarding reference?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. A forwarding reference is `T&&` (or `auto&&`) where `T` is **deduced for this very function call**; it binds anything and `T` encodes the argument's category. An rvalue reference is any `&&` without deduction. *Forwarding:* `template <class T> void f(T&&)`, `auto&& x = e;`, `[](auto&&)`. *Rvalue:* `void f(std::string&&)`, `template <class T> void f(std::vector<T>&&)`, `template <class T> void f(const T&&)`, a member of a class template `void f(T&&)` with `T` the class parameter.
2. `T = int&`; `T = const int&`; `T = int`.
3. `& &→&`, `& &&→&`, `&& &→&`, `&& &&→&&`. Only two rvalue references yield an rvalue reference.
4. `remove_reference_t<T>` is a **non-deduced context**, so `T` cannot be inferred from the argument and **must** be given explicitly. This prevents `forward(x)` from silently deducing the wrong category: a name is always an lvalue, and deduction would always conclude "lvalue".
5. `g(std::move(w))`. `w` is a name, hence an lvalue; `Widget&&` cannot bind to it. It is a known rvalue reference (not deduced), so `std::move` is correct (not `forward`).
6. `std::move` on a forwarding reference: when called with an **lvalue**, it moves from the caller's object, leaving it moved-from. Use `std::forward<T>(v)`.
7. With a non-const lvalue, the template deduces `T = Person&` which is an exact match (`Person&`), but the copy constructor requires adding `const`; exact match is a better conversion than a qualification adjustment, so the template wins. Fixes: constrain the template to exclude `Person` (`requires !same_as<remove_cvref_t<T>, Person>`), or take the argument by value and avoid a forwarding constructor.
8. Braced-init-lists; `0`/`NULL` as null pointer constants; overloaded or template function names; bit-fields; (pre-C++17) `static const` integral members without a definition.
9. `auto` decays: references and cv-qualifiers on the result are dropped. `decltype(auto)` preserves exactly the callee's return type, so `int&` stays `int&` (and you can assign through it), and `T&&` returns stay `T&&`.
10. Forwarding a reference into another thread leaves the callee with a reference to the caller's object, which may be destroyed or raced on. Decay-copy makes the thread own its arguments; sharing requires the explicit `std::ref`.
11. For the cases `T = X` and `T = X&` the result is identical. `forward` adds the two-overload structure that rejects forwarding an rvalue as an lvalue (`static_assert`) and documents intent. In practice there's no semantic difference; use `forward`.
12. No. `T` is the *class* template parameter, already fixed when `f` is called: `S<std::string>::f` takes only `std::string&&` (an rvalue reference). For a forwarding reference, make `f` itself a template: `template <class U> void f(U&&)`.

</details>

---

[← Previous: Chapter 6](06-move-semantics.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 8 — Templates deep dive →](../part-04-generic-programming/08-templates-deep-dive.md)
