// The raw CPython C API: what pybind11 generates for you. Module `capi_ext`.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdexcept>

// sum_floats(seq) -> float.   Shows: argument parsing, borrowed vs new references, error propagation.
static PyObject* sum_floats(PyObject* /*self*/, PyObject* args) {
    PyObject* seq;
    if (!PyArg_ParseTuple(args, "O", &seq)) return nullptr;              // sets TypeError, we return NULL
    PyObject* it = PyObject_GetIter(seq);                                  // NEW reference: we own it
    if (!it) return nullptr;
    double total = 0;
    while (PyObject* item = PyIter_Next(it)) {                             // NEW reference per item
        double v = PyFloat_AsDouble(item);
        Py_DECREF(item);                                                   // forget this line and every item leaks
        if (v == -1.0 && PyErr_Occurred()) { Py_DECREF(it); return nullptr; }
        total += v;
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return nullptr;                                  // iteration itself failed
    return PyFloat_FromDouble(total);                                      // NEW reference handed to the caller
}

// leaky_sum: identical, but the Py_DECREF(item) is "forgotten"
static PyObject* leaky_sum(PyObject*, PyObject* args) {
    PyObject* seq; if (!PyArg_ParseTuple(args, "O", &seq)) return nullptr;
    PyObject* it = PyObject_GetIter(seq); if (!it) return nullptr;
    double total = 0;
    while (PyObject* item = PyIter_Next(it)) { total += PyFloat_AsDouble(item); /* missing Py_DECREF(item) */ }
    Py_DECREF(it);
    return PyFloat_FromDouble(total);
}

// first_item(seq): demonstrates borrowed references (PyList_GetItem returns one) and INCREF when returning it
static PyObject* first_item(PyObject*, PyObject* args) {
    PyObject* list; if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) return nullptr;
    PyObject* item = PyList_GetItem(list, 0);                              // BORROWED: valid while the list holds it
    if (!item) return nullptr;
    Py_INCREF(item);                                                       // we return it, so the caller needs its own reference
    return item;
}

// divide(a, b): C++ exception -> Python exception, by hand (pybind11 does this for every function)
static PyObject* divide(PyObject*, PyObject* args) {
    double a, b; if (!PyArg_ParseTuple(args, "dd", &a, &b)) return nullptr;
    try {
        if (b == 0) throw std::invalid_argument("division by zero");
        return PyFloat_FromDouble(a / b);
    } catch (const std::invalid_argument& e) { PyErr_SetString(PyExc_ValueError, e.what()); }
      catch (const std::exception& e)        { PyErr_SetString(PyExc_RuntimeError, e.what()); }
    return nullptr;
}

static PyMethodDef methods[] = {
    {"sum_floats", sum_floats, METH_VARARGS, "Sum an iterable of numbers."},
    {"leaky_sum",  leaky_sum,  METH_VARARGS, "Like sum_floats but leaks a reference per item."},
    {"first_item", first_item, METH_VARARGS, "First item of a list (borrowed reference demo)."},
    {"divide",     divide,     METH_VARARGS, "a / b, raising ValueError on zero."},
    {nullptr, nullptr, 0, nullptr}};
static PyModuleDef moduledef = {PyModuleDef_HEAD_INIT, "capi_ext", "Raw C API demo", -1, methods, nullptr, nullptr, nullptr, nullptr};
PyMODINIT_FUNC PyInit_capi_ext(void) { return PyModule_Create(&moduledef); }
