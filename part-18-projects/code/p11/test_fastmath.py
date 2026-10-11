import gc, threading, time, unittest
import numpy as np
import fastmath as fm


class MovingAverage(unittest.TestCase):
    def test_matches_numpy(self):
        a = np.random.default_rng(1).normal(size=1000)
        for w in (1, 2, 7, 1000):
            ref = np.convolve(a, np.ones(w) / w, mode="valid")
            np.testing.assert_allclose(fm.moving_average(a, w), ref, rtol=1e-9, atol=1e-9)

    def test_other_inputs_are_converted(self):
        self.assertEqual(list(fm.moving_average([1, 2, 3, 4], 2)), [1.5, 2.5, 3.5])              # list
        self.assertEqual(list(fm.moving_average(np.arange(4, dtype=np.int32), 2)), [0.5, 1.5, 2.5])
        self.assertEqual(list(fm.moving_average(np.arange(8.0)[::2], 2)), [1.0, 3.0, 5.0])        # non-contiguous view

    def test_errors(self):
        with self.assertRaises(ValueError): fm.moving_average(np.zeros(3), 0)
        with self.assertRaises(ValueError): fm.moving_average(np.zeros(3), 4)
        with self.assertRaises(ValueError): fm.moving_average(np.zeros((2, 2)), 1)
        with self.assertRaises(TypeError):  fm.moving_average("not an array", 1)

    def test_runs_in_parallel_because_gil_is_released(self):
        a = np.random.default_rng(2).normal(size=20_000_000)
        t0 = time.perf_counter(); fm.moving_average(a, 5); one = time.perf_counter() - t0
        out = []
        ts = [threading.Thread(target=lambda: out.append(fm.moving_average(a, 5))) for _ in range(2)]
        t0 = time.perf_counter(); [t.start() for t in ts]; [t.join() for t in ts]; two = time.perf_counter() - t0
        print(f"  moving_average 20M: one call {one*1000:.0f} ms, two threads {two*1000:.0f} ms (serial would be ~{2*one*1000:.0f})")
        self.assertLess(two, 1.7 * one)                                                            # parallel, with generous slack


class ScaleInplace(unittest.TestCase):
    def test_mutates_caller_array(self):
        a = np.arange(5.0); fm.scale_inplace(a, 10.0)
        self.assertEqual(list(a), [0, 10, 20, 30, 40])

    def test_wrong_dtype_is_rejected_not_silently_copied(self):
        for bad in (np.arange(5), np.arange(5, dtype=np.float32), np.arange(10.0)[::2]):          # int64, float32, strided
            with self.assertRaises(TypeError): fm.scale_inplace(bad, 2.0)
        self.assertEqual(list(bad), list(np.arange(10.0)[::2]))                                    # untouched

    def test_readonly(self):
        a = np.arange(3.0); a.setflags(write=False)
        with self.assertRaises((ValueError, TypeError)): fm.scale_inplace(a, 2.0)


class HistogramTests(unittest.TestCase):
    def test_counts_and_outside(self):
        h = fm.Histogram(4, 0.0, 4.0)
        h.add(np.array([0.5, 1.5, 1.6, 3.9, 4.0, -1.0]))
        self.assertEqual(list(h.counts()), [1, 2, 0, 1]); self.assertEqual(h.outside, 2); self.assertEqual(len(h), 4)

    def test_counts_is_a_live_view_not_a_copy(self):
        h = fm.Histogram(2, 0.0, 2.0); c = h.counts()
        h.add(np.array([0.5, 1.5, 1.5]))
        self.assertEqual(list(c), [1, 2])                                                         # the earlier array saw the update
        self.assertFalse(c.flags.owndata)                                                         # it does not own its memory

    def test_view_keeps_histogram_alive(self):
        h = fm.Histogram(3, 0.0, 3.0); h.add(np.array([0.1, 1.1, 2.1])); c = h.counts()
        del h; gc.collect()
        self.assertEqual(list(c), [1, 1, 1])                                                      # no dangling pointer: base object kept it alive

    def test_validation_error_is_a_value_error(self):
        with self.assertRaises(fm.HistogramError) as cm: fm.Histogram(0, 0.0, 1.0)
        self.assertIsInstance(cm.exception, ValueError)
        with self.assertRaises(ValueError): fm.Histogram(3, 5.0, 1.0)

    def test_concurrent_add_from_threads_is_correct(self):
        h = fm.Histogram(10, 0.0, 10.0); data = np.random.default_rng(3).uniform(0, 10, 200_000)
        ts = [threading.Thread(target=lambda: [h.add(data) for _ in range(20)]) for _ in range(4)]
        [t.start() for t in ts]; [t.join() for t in ts]
        self.assertEqual(int(h.counts().sum()), 4 * 20 * len(data))                                # no lost updates with the internal mutex


class MapPy(unittest.TestCase):
    def test_callback_and_exception_propagation(self):
        self.assertEqual(list(fm.map_py(lambda x: x * 2, np.array([1.0, 2.0]))), [2.0, 4.0])
        def boom(x): raise KeyError("callback failed")
        with self.assertRaises(KeyError): fm.map_py(boom, np.array([1.0]))
        # A manual .cast<double>() failure is pybind11's cast_error -> RuntimeError, NOT TypeError. Only *argument*
        # conversion (the overload resolution path) produces TypeError. Decide which one your API should expose.
        with self.assertRaises(RuntimeError): fm.map_py(lambda x: "str", np.array([1.0]))
        with self.assertRaises(TypeError): fm.map_py(lambda x: x, "not an array")


if __name__ == "__main__":
    unittest.main(verbosity=2)
