import sys, capi_ext
x = [1.5, 2.5, 3.0]
print("sum_floats:", capi_ext.sum_floats(x))
print("sum of generator:", capi_ext.sum_floats(v * 2 for v in range(4)))
try: capi_ext.sum_floats(42)
except TypeError as e: print("TypeError:", e)
try: capi_ext.sum_floats(["a"])
except TypeError as e: print("TypeError:", e)
try: capi_ext.divide(1, 0)
except ValueError as e: print("ValueError:", e)
# Reference counting: a distinctive object, count references before/after
obj = 12345.678
before = sys.getrefcount(obj)
capi_ext.sum_floats([obj] * 10)
print("refcount after correct sum  :", sys.getrefcount(obj) - before, "(delta)")
capi_ext.leaky_sum([obj] * 10)
print("refcount after leaky sum    :", sys.getrefcount(obj) - before, "(delta)  <- one leaked reference per item")
lst = ["x", "y"]
print("first_item:", capi_ext.first_item(lst), " refcount delta:", end=" ")
r = sys.getrefcount(lst[0]); capi_ext.first_item(lst); print(sys.getrefcount(lst[0]) - r)
