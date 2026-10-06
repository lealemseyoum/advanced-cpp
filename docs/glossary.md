# Glossary

[← Back to the course map](../README.md)

Precise definitions of the terms this course uses. Where the standard has an official term, the stable section name is given in brackets, such as `[basic.life]`, so you can look it up at [eel.is/c++draft](https://eel.is/c++draft/).

---

## Objects, storage, lifetime

| Term | Definition |
|---|---|
| **Object** | A region of storage with a type, lifetime, and (usually) a value. Not the same as a class instance: an `int` is an object. `[intro.object]` |
| **Subobject** | A member subobject, base-class subobject, or array element of another object |
| **Storage duration** | How long storage is reserved: automatic, static, thread, or dynamic. `[basic.stc]` |
| **Lifetime** | The period during which an object may be used. Starts when initialization completes (storage obtained and any constructor finished); ends when destruction starts. Distinct from storage duration. `[basic.life]` |
| **Object representation** | The sequence of `sizeof(T)` bytes that make up an object of type `T` |
| **Value representation** | The bits that participate in the object's value. Padding bits are excluded |
| **Trivially copyable** | Copying the bytes is a valid way to copy the object. Allows `memcpy` between objects of the type |
| **Standard-layout** | Layout is predictable enough to interoperate with C: no virtual functions, uniform access control, restricted inheritance |
| **Implicit-lifetime type** | A type for which certain operations (`malloc`, `memcpy`, `std::start_lifetime_as`) implicitly create objects, such as arrays, aggregates, and types with a trivial constructor and destructor. `[basic.types.general]` |
| **Placement new** | `new (ptr) T(args)`: construct a `T` in already-provided storage |
| **Pointer provenance** | The idea that a pointer value carries information about which object it may access, beyond its address. `std::launder` exists because of it |
| **Dangling** | Refers to an object whose lifetime has ended or whose storage was released |

## Value categories and moves

| Term | Definition |
|---|---|
| **Expression** | A sequence of operators and operands. Value categories belong to *expressions*, not to variables or objects. `[basic.lval]` |
| **glvalue** | An expression whose evaluation determines the identity of an object or function |
| **prvalue** | An expression whose evaluation initializes an object or computes a value. Since C++17 it is *not* a temporary until materialized |
| **xvalue** | A glvalue denoting an object whose resources can be reused ("expiring") |
| **lvalue** | A glvalue that is not an xvalue |
| **rvalue** | A prvalue or an xvalue |
| **Temporary materialization** | The conversion of a prvalue into an xvalue by creating a temporary object |
| **Forwarding reference** | A parameter of type `T&&` where `T` is a template parameter of the function being deduced (and not `const`-qualified). Often informally called a "universal reference" |
| **Reference collapsing** | `T& &`, `T& &&`, `T&& &`  all become `T&`; only `T&& &&` becomes `T&&` |
| **Guaranteed copy elision** | Since C++17, initializing an object from a prvalue of the same type constructs the object directly in its final location. No copy or move exists to elide |
| **NRVO** | Named return value optimization: an *optional* optimization when returning a named local. Not guaranteed |
| **Moved-from state** | The "valid but unspecified" state of a standard-library object after a move. You may destroy it or assign to it; do not assume its value |

## Templates and compile time

| Term | Definition |
|---|---|
| **Instantiation** | Generating a specialization of a template from its definition and a set of arguments |
| **Dependent name** | A name whose meaning depends on a template parameter, so cannot be resolved until instantiation |
| **Two-phase lookup** | Non-dependent names are looked up at definition; dependent names at instantiation (via ADL for unqualified function calls) |
| **ADL** | Argument-dependent lookup: unqualified function names also search the namespaces of the arguments' types |
| **SFINAE** | Substitution failure is not an error: a failed substitution in an immediate context removes a candidate rather than failing the program |
| **Concept** | A named, composable constraint on template arguments, evaluated at compile time |
| **Constant expression** | An expression the compiler can (and sometimes must) evaluate at compile time |
| **`consteval`** | An *immediate function*: every call must produce a constant expression |
| **Customization point object (CPO)** | A function object that dispatches to user-provided overloads through ADL, with constraints and a fixed lookup behavior |
| **Reflection** | The ability of a program to inspect its own entities at compile time. Standardized in C++26 |

## Polymorphism and type erasure

| Term | Definition |
|---|---|
| **vptr / vtable** | Per-object pointer to a per-class table of virtual function addresses. An *implementation technique*; the standard mandates behavior, not the mechanism |
| **Devirtualization** | The compiler proving the dynamic type and replacing a virtual call with a direct call |
| **CRTP** | `struct D : Base<D>`: static polymorphism through a base class that knows its derived type |
| **Type erasure** | Hiding a concrete type behind a uniform interface while keeping value semantics. `std::function` and `std::any` are the standard examples |
| **Small-buffer optimization (SBO / SOO)** | Storing small objects inside the owner instead of allocating |

## Errors and exceptions

| Term | Definition |
|---|---|
| **Basic guarantee** | After an exception, invariants hold and nothing leaks, but state may have changed |
| **Strong guarantee** | After an exception, state is as if the operation never happened (commit-or-rollback) |
| **No-throw guarantee** | The operation never throws |
| **Stack unwinding** | Destroying automatic objects as control propagates up the call stack |
| **Table-driven ("zero-cost") exceptions** | Implementations where the non-throwing path has no runtime cost, and throwing is expensive, driven by tables like `.eh_frame` and LSDA |

## Memory and concurrency

| Term | Definition |
|---|---|
| **Memory location** | A scalar object or a maximal sequence of adjacent bit-fields. The unit of data-race reasoning. `[intro.memory]` |
| **Data race** | Two conflicting accesses to the same memory location from different threads, at least one non-atomic and a write, with no happens-before ordering. **Undefined behavior** |
| **Sequenced-before** | Intra-thread ordering of evaluations |
| **Synchronizes-with** | A release operation observed by an acquire operation on the same atomic |
| **Happens-before** | The transitive combination of sequenced-before and synchronizes-with (plus a few details). The ordering that makes data visible |
| **Release / acquire** | A release store publishes everything the thread did before it; an acquire load that reads that value sees it |
| **Sequential consistency** | A single total order of all `seq_cst` operations consistent with program order. The default |
| **Lock-free** | Some thread always makes progress in a finite number of steps, no matter what the others do |
| **Wait-free** | *Every* thread completes in a bounded number of its own steps |
| **ABA problem** | A CAS succeeds because a value changed from A to B and back to A, hiding intervening changes |
| **False sharing** | Unrelated variables on the same cache line cause cache-coherence traffic between cores |

## Compilation, linking, binary interface

| Term | Definition |
|---|---|
| **Translation unit** | A source file after preprocessing |
| **ODR** | One-definition rule: every entity may have only one definition in the program (with carefully specified exceptions such as inline functions and templates). Violations are ill-formed, no diagnostic required |
| **Linkage** | Whether a name denotes the same entity across translation units: none, internal, external, or module linkage |
| **Name mangling** | Encoding a function's scope, name and parameter types into one linker symbol |
| **ABI** | The binary contract: sizes, layouts, calling conventions, mangling, vtable layout. Distinct from API |
| **pImpl** | A class holding a pointer to a hidden implementation, so that the public class's layout never changes |
| **Module** | A C++20 unit of encapsulation compiled once into a binary interface, imported instead of textually included |
| **LTO** | Link-time optimization: whole-program optimization across translation units |
| **PGO** | Profile-guided optimization: using runtime profiles to guide inlining, layout and branch hints |

## Behavior classes

| Term | Definition |
|---|---|
| **Undefined behavior (UB)** | The standard imposes **no** requirements. The program is not "wrong at this line": the compiler may assume it never happens |
| **Unspecified behavior** | The implementation chooses among allowed behaviors and need not document which |
| **Implementation-defined behavior** | The implementation chooses and **must document** it |
| **Ill-formed, no diagnostic required (IFNDR)** | The program is invalid but the compiler is not obliged to tell you; behavior is effectively undefined |
| **Erroneous behavior** | New in C++26: well-defined but incorrect behavior (for example reading an uninitialized automatic variable) that implementations are encouraged to diagnose |
