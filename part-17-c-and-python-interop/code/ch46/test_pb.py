import gc, pb_ext, numpy as np
print("divide(6, 3):", pb_ext.divide(6, 3), "  default b:", pb_ext.divide(5))
try: pb_ext.divide(1, 0)
except ValueError as e: print("ValueError:", e)
try: pb_ext.over_budget(500)
except pb_ext.BudgetExceeded as e: print("BudgetExceeded:", e)
try: pb_ext.sum_list("abc")
except TypeError as e: print("TypeError:", str(e).splitlines()[0])
print(pb_ext.divide.__doc__.splitlines()[0])

a = np.arange(5, dtype=np.float64)
print("sum_array:", pb_ext.sum_array(a))
print("same memory (no copy):", pb_ext.data_address(a) == a.__array_interface__["data"][0])
pb_ext.scale_inplace(a, 10.0); print("after scale_inplace:", a)
b = np.arange(5, dtype=np.int32)                      # wrong dtype
print("int32 array via forcecast (copy):", pb_ext.sum_array(b))
print("scale_inplace on an int array mutates a COPY:", end=" ")
pb_ext.scale_inplace(b, 10.0); print(b)

print("integrate x^2 on [0,1]:", round(pb_ext.integrate(lambda x: x * x, 0, 1, 1000), 6))
def bad(x):
    if x > 0.5: raise KeyError("callback failed")
    return x
try: pb_ext.integrate(bad, 0, 1, 10)
except KeyError as e: print("Python exception propagated through C++:", repr(e))

inv = pb_ext.Inventory()
it_copy = inv.add_default("bolt", 10)
it_copy.qty = 99
print("add_default returns a COPY; inventory item still:", inv.at(0))
it = inv.add_ref("nut", 5)
it.qty = 7
print("add_ref returns a reference; inventory sees:", inv.at(1))
del inv; gc.collect()
print("Inventory destroyed after `del inv`?", pb_ext.Inventory.destroyed(), "(it is alive: `it` still references into it, reference_internal keeps the parent alive)")
print("it still valid:", it)
del it; gc.collect()
print("after deleting the item too -> destroyed count:", pb_ext.Inventory.destroyed())
