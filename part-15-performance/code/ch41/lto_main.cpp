#include <chrono>
#include <cstdio>
int weight(int);                                           // declaration only: the body is invisible here
int main() {
    long total = 0;
    auto t = std::chrono::steady_clock::now();
    for (int r = 0; r < 200; ++r)
        for (int i = 0; i < 1'000'000; ++i) total += weight(i);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    std::printf("total=%ld  %.0f ms\n", total, ms);
}
