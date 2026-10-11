#include "thread_pool.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); }
static double work(int i) { double x = i; for (int k = 0; k < 200000; ++k) x = std::sin(x) + 1.0; return x; }
int main() {
    {   // overhead: tasks/second for empty tasks
        edu::ThreadPool pool(2);
        const int N = 200000; std::vector<std::future<int>> fs; fs.reserve(N);
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) fs.push_back(pool.submit([i] { return i; }));
        long s = 0; for (auto& f : fs) s += f.get();
        double t = ms(t0);
        std::printf("empty tasks: %d in %.1f ms  =  %.2f us/task  (sum %ld)\n", N, t, t * 1000 / N, s);
    }
    for (unsigned threads : {1u, 2u, 4u}) {   // scaling of CPU-bound tasks (this VM has 2 vCPUs)
        edu::ThreadPool pool(threads);
        const int N = 64; std::vector<std::future<double>> fs;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) fs.push_back(pool.submit(work, i));
        double s = 0; for (auto& f : fs) s += f.get();
        std::printf("64 CPU tasks, %u worker(s): %7.1f ms   (checksum %.3f)\n", threads, ms(t0), s);
    }
}
