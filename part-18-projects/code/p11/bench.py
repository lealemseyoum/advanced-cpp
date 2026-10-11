import time, numpy as np, fastmath as fm
def best(f, r=5):
    b = 1e9
    for _ in range(r):
        t = time.perf_counter(); f(); b = min(b, time.perf_counter() - t)
    return b * 1000
a = np.random.default_rng(0).normal(size=1_000_000); w = 16
def py_loop():
    out = []; acc = sum(a[:w].tolist()); out.append(acc / w); l = a.tolist()
    for i in range(1, len(l) - w + 1): acc += l[i + w - 1] - l[i - 1]; out.append(acc / w)
    return out
print(f"moving_average, 1M doubles, window {w}")
print(f"  pure Python loop      {best(py_loop, 3):8.1f} ms")
print(f"  np.convolve           {best(lambda: np.convolve(a, np.ones(w)/w, mode='valid')):8.2f} ms")
print(f"  cumsum trick (numpy)  {best(lambda: (np.cumsum(np.insert(a,0,0))[w:] - np.cumsum(np.insert(a,0,0))[:-w]) / w):8.2f} ms")
print(f"  fastmath (C++)        {best(lambda: fm.moving_average(a, w)):8.2f} ms")
x = a[:100_000]
print("map a Python function over 100k elements")
print(f"  [f(x) for x in list]  {best(lambda: [v*2 for v in x.tolist()]):8.2f} ms")
print(f"  np.vectorize          {best(lambda: np.vectorize(lambda v: v*2)(x)):8.2f} ms")
print(f"  fm.map_py             {best(lambda: fm.map_py(lambda v: v*2, x)):8.2f} ms")
print(f"  numpy  x*2            {best(lambda: x*2):8.3f} ms")
