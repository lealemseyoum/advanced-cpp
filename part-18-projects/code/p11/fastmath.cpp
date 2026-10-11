// Project 11 reference solution: a small pybind11 module showing zero-copy arrays, GIL release,
// ownership/keep-alive and exception translation.
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace py = pybind11;

// ---- 1. pure C++ core: no Python.h, no pybind11 here ------------------------------------------------
namespace core {

inline void moving_average(const double* in, std::size_t n, std::size_t window, double* out) {
    double acc = 0;
    for (std::size_t i = 0; i < window; ++i) acc += in[i];
    out[0] = acc / double(window);
    for (std::size_t i = 1; i + window <= n; ++i) {
        acc += in[i + window - 1] - in[i - 1];
        out[i] = acc / double(window);
    }
}

class HistogramError : public std::runtime_error { using runtime_error::runtime_error; };

class Histogram {
public:
    Histogram(std::size_t bins, double lo, double hi) : lo_(lo), hi_(hi), counts_(bins, 0) {
        if (bins == 0) throw HistogramError("bins must be positive");
        if (!(hi > lo)) throw HistogramError("hi must be greater than lo");
    }
    void add(const double* v, std::size_t n) {
        std::lock_guard lk(m_);                        // add() runs with the GIL released: guard our own state
        const double scale = double(counts_.size()) / (hi_ - lo_);
        for (std::size_t i = 0; i < n; ++i) {
            if (!(v[i] >= lo_ && v[i] < hi_)) { ++outside_; continue; }
            ++counts_[std::size_t((v[i] - lo_) * scale)];
        }
    }
    std::size_t bins() const noexcept { return counts_.size(); }
    std::uint64_t* data() noexcept { return counts_.data(); }
    std::uint64_t outside() const { std::lock_guard lk(m_); return outside_; }
private:
    double lo_, hi_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t outside_ = 0;
    mutable std::mutex m_;
};

}  // namespace core

// ---- 2. the binding layer: conversion, ownership, GIL, exceptions ------------------------------------------
py::array_t<double> moving_average(py::array_t<double, py::array::c_style | py::array::forcecast> a, std::size_t window) {
    if (a.ndim() != 1) throw py::value_error("moving_average: expected a 1-D array");
    const std::size_t n = std::size_t(a.shape(0));
    if (window == 0 || window > n) throw py::value_error("moving_average: window must be in [1, len(a)]");
    py::array_t<double> out(py::ssize_t(n - window + 1));
    const double* in = a.data();                       // raw pointers are taken BEFORE the GIL is released
    double* o = out.mutable_data();
    {
        py::gil_scoped_release release;                // no Python API calls in this scope
        core::moving_average(in, n, window, o);
    }
    return out;
}

// In-place: must not silently operate on a converted copy, so the argument is noconvert() in the binding.
void scale_inplace(py::array_t<double, py::array::c_style>& a, double k) {
    if (!a.writeable()) throw py::value_error("scale_inplace: array is read-only");
    double* p = a.mutable_data(); const py::ssize_t n = a.size();
    py::gil_scoped_release release;
    for (py::ssize_t i = 0; i < n; ++i) p[i] *= k;
}

// Calls a Python function for each element from C++. The GIL must be HELD: this is not faster than Python.
py::array_t<double> map_py(const py::function& f, py::array_t<double, py::array::c_style | py::array::forcecast> a) {
    py::array_t<double> out(a.size());
    const double* in = a.data(); double* o = out.mutable_data();
    for (py::ssize_t i = 0; i < a.size(); ++i) o[i] = f(in[i]).cast<double>();
    return out;
}

PYBIND11_MODULE(fastmath, m) {
    m.doc() = "Teaching module for Project 11";
    static py::exception<core::HistogramError> exc(m, "HistogramError", PyExc_ValueError);   // subclass of ValueError
    py::register_exception_translator([](std::exception_ptr p) {
        try { if (p) std::rethrow_exception(p); }
        catch (const core::HistogramError& e) { exc(e.what()); }
    });

    m.def("moving_average", &moving_average, py::arg("a"), py::arg("window"),
          "Trailing-window mean of a 1-D float array. Releases the GIL while computing.");
    m.def("scale_inplace", &scale_inplace, py::arg("a").noconvert(), py::arg("k"),
          "Multiply a float64 C-contiguous array by k, in place. Raises TypeError for any other dtype/layout.");
    m.def("map_py", &map_py, py::arg("f"), py::arg("a"));

    py::class_<core::Histogram>(m, "Histogram")
        .def(py::init<std::size_t, double, double>(), py::arg("bins"), py::arg("lo"), py::arg("hi"))
        .def("add", [](core::Histogram& h, py::array_t<double, py::array::c_style | py::array::forcecast> a) {
            const double* p = a.data(); const std::size_t n = std::size_t(a.size());
            py::gil_scoped_release release;
            h.add(p, n);
        }, py::arg("values"))
        // A NumPy array that VIEWS the histogram's memory; `self` is the array's base object, which keeps the
        // Histogram alive for as long as the view exists (the keep-alive pattern).
        .def("counts", [](py::object self) {
            auto& h = self.cast<core::Histogram&>();
            return py::array_t<std::uint64_t>({py::ssize_t(h.bins())}, {py::ssize_t(sizeof(std::uint64_t))}, h.data(), self);
        })
        .def_property_readonly("outside", &core::Histogram::outside)
        .def("__len__", &core::Histogram::bins);
}
