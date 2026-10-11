// The same ideas with pybind11. Module `pb_ext`.
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
namespace py = pybind11;

// ---- 1. basic functions, exceptions, defaults, keyword args -------------------------------------
double divide(double a, double b) {
    if (b == 0) throw std::invalid_argument("division by zero");           // -> ValueError automatically
    return a / b;
}
struct BudgetExceeded : std::runtime_error { using std::runtime_error::runtime_error; };

double sum_list(const std::vector<double>& v) { double t = 0; for (double x : v) t += x; return t; }   // list -> vector copy

// ---- 2. buffer protocol / numpy: no copy -----------------------------------------------------------
double sum_array(py::array_t<double, py::array::c_style | py::array::forcecast> a) {
    auto r = a.unchecked<1>();                                             // bounds-check-free view of the buffer
    double t = 0; for (py::ssize_t i = 0; i < r.shape(0); ++i) t += r(i);
    return t;
}
void scale_inplace(py::array_t<double> a, double k) {                      // mutates the caller's array: shares memory
    auto r = a.mutable_unchecked<1>();
    for (py::ssize_t i = 0; i < r.shape(0); ++i) r(i) *= k;
}
// Any object that supports the buffer protocol (array.array, memoryview, bytes, numpy, mmap, ...)
double buffer_sum(py::buffer b) {
    py::buffer_info info = b.request();                                    // asks the exporter for pointer, format, shape, strides
    if (info.format != py::format_descriptor<double>::format() || info.ndim != 1)
        throw std::invalid_argument("expected a 1-D buffer of doubles, got format '" + info.format + "', ndim=" + std::to_string(info.ndim));
    const double* p = static_cast<const double*>(info.ptr);
    double t = 0; for (py::ssize_t i = 0; i < info.shape[0]; ++i) t += p[i * info.strides[0] / (py::ssize_t)sizeof(double)];
    return t;
}
std::uintptr_t data_address(py::array_t<double> a) { return reinterpret_cast<std::uintptr_t>(a.data()); }

// ---- 3. the GIL ---------------------------------------------------------------------------------------
double spin_locked(long n)   { double x = 0; for (long i = 1; i <= n; ++i) x += std::sin(i * 1e-3); return x; }          // holds the GIL
double spin_released(long n) {
    py::gil_scoped_release release;                                       // other Python threads may run now; NO Python API calls below
    double x = 0; for (long i = 1; i <= n; ++i) x += std::sin(i * 1e-3);
    return x;
}

// ---- 4. ownership and lifetimes -------------------------------------------------------------------
struct Item { std::string name; int qty; };
struct Inventory {
    std::vector<Item> items;
    Item& add(std::string n, int q) { items.push_back({std::move(n), q}); return items.back(); }   // reference into a vector!
    Item& at(size_t i) { return items.at(i); }
    size_t size() const { return items.size(); }
    ~Inventory() { ++destroyed; }
    static inline int destroyed = 0;
};

// ---- 5. callbacks into Python --------------------------------------------------------------------------
double integrate(const std::function<double(double)>& f, double a, double b, int n) {
    double h = (b - a) / n, s = 0;
    for (int i = 0; i < n; ++i) s += f(a + (i + 0.5) * h) * h;             // Python exceptions propagate through here as C++ exceptions
    return s;
}

PYBIND11_MODULE(pb_ext, m) {
    m.doc() = "pybind11 demo";
    static py::exception<BudgetExceeded> exc(m, "BudgetExceeded");         // new Python exception class
    py::register_exception_translator([](std::exception_ptr p) {
        try { if (p) std::rethrow_exception(p); } catch (const BudgetExceeded& e) { PyErr_SetString(exc.ptr(), e.what()); }
    });
    m.def("divide", &divide, py::arg("a"), py::arg("b") = 1.0, "a / b; ValueError if b == 0");
    m.def("over_budget", [](int spent) { if (spent > 100) throw BudgetExceeded("spent " + std::to_string(spent)); return spent; });
    m.def("sum_list", &sum_list);
    m.def("sum_array", &sum_array);
    m.def("scale_inplace", &scale_inplace);
    m.def("buffer_sum", &buffer_sum);
    m.def("data_address", &data_address);
    m.def("spin_locked", &spin_locked);
    m.def("spin_released", &spin_released);
    m.def("integrate", &integrate);

    py::class_<Item>(m, "Item")
        .def_readwrite("name", &Item::name).def_readwrite("qty", &Item::qty)
        .def("__repr__", [](const Item& i) { return "Item(" + i.name + ", " + std::to_string(i.qty) + ")"; });
    py::class_<Inventory>(m, "Inventory")
        .def(py::init<>())
        .def("add_default", &Inventory::add, py::arg("name"), py::arg("qty"))                                  // default policy for a returned lvalue reference: copy
        .def("add_ref", &Inventory::add, py::arg("name"), py::arg("qty"), py::return_value_policy::reference_internal)
        .def("at", &Inventory::at, py::return_value_policy::reference_internal)
        .def("__len__", &Inventory::size)
        .def_static("destroyed", [] { return Inventory::destroyed; });
}
