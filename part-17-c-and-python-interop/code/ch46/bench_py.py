import time, threading, statistics as st
import numpy as np, capi_ext, pb_ext

def best(f, n=7):
    ts = []
    for _ in range(n):
        t = time.perf_counter(); f(); ts.append(time.perf_counter() - t)
    return min(ts) * 1e3

N = 1_000_000
lst = [float(i) for i in range(N)]
arr = np.array(lst)
print(f"sum of {N:,} floats (best of 7, ms):")
print(f"  python sum(list)                {best(lambda: sum(lst)):8.2f}")
print(f"  C API sum_floats(list)          {best(lambda: capi_ext.sum_floats(lst)):8.2f}")
print(f"  pybind11 sum_list(list)         {best(lambda: pb_ext.sum_list(lst)):8.2f}   (list -> std::vector copy each call)")
print(f"  pybind11 sum_array(ndarray)     {best(lambda: pb_ext.sum_array(arr)):8.2f}   (zero copy)")
print(f"  numpy arr.sum()                 {best(lambda: arr.sum()):8.2f}")
print(f"  python loop over ndarray        {best(lambda: sum(arr), 3):8.2f}")

def threaded(fn, n, threads=2):
    ts = [threading.Thread(target=fn, args=(n,)) for _ in range(threads)]
    t = time.perf_counter(); [x.start() for x in ts]; [x.join() for x in ts]
    return (time.perf_counter() - t) * 1e3
n = 20_000_000
single = best(lambda: pb_ext.spin_locked(n), 3)
print(f"\nCPU-bound C++ call, n={n:,}:  one call = {single:.0f} ms")
for name, fn in (("holding the GIL", pb_ext.spin_locked), ("gil_scoped_release", pb_ext.spin_released)):
    t2 = min(threaded(fn, n, 2) for _ in range(3))
    print(f"  2 threads, {name:20s}: {t2:6.0f} ms   (ideal parallel = {single:.0f}, serial = {2*single:.0f})")
