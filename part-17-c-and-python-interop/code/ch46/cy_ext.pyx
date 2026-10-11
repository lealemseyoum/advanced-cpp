# cython: language_level=3, boundscheck=False, wraparound=False
from libc.math cimport sin
def sum_memview(double[::1] a):
    cdef double t = 0
    cdef Py_ssize_t i
    for i in range(a.shape[0]):
        t += a[i]
    return t
def spin(long n):
    cdef double x = 0
    cdef long i
    with nogil:
        for i in range(1, n + 1):
            x += sin(i * 1e-3)
    return x
