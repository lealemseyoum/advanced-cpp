// A skewed workload: a tiny bytecode VM where 94% of instructions are ADD/INC and the rest are rare
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <stdexcept>

enum Op : unsigned char { ADD, INC, DEC, MUL, XOR, DIV, THROW_IF_ZERO, NOP };

[[gnu::noinline]] long run(const std::vector<Op>& code, const std::vector<int>& arg) {
    long acc = 1;
    for (size_t i = 0; i < code.size(); ++i) {
        switch (code[i]) {
            case ADD: acc += arg[i]; break;
            case INC: ++acc; break;
            case DEC: --acc; break;
            case MUL: acc *= (arg[i] | 1); break;
            case XOR: acc ^= arg[i]; break;
            case DIV: acc /= (arg[i] | 1); break;
            case THROW_IF_ZERO: if (acc == 0) throw std::runtime_error("zero"); break;
            case NOP: break;
        }
    }
    return acc;
}

int main() {
    std::mt19937 rng(3);
    const size_t n = 1 << 22;
    std::vector<Op> code(n); std::vector<int> arg(n);
    for (size_t i = 0; i < n; ++i) {
        unsigned r = rng() % 100;
        code[i] = r < 55 ? ADD : r < 94 ? INC : r < 96 ? XOR : r < 97 ? DEC : r < 98 ? MUL : r < 99 ? DIV : NOP;
        arg[i] = static_cast<int>(rng());
    }
    long total = 0;
    auto t = std::chrono::steady_clock::now();
    for (int r = 0; r < 40; ++r) total += run(code, arg);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    std::printf("%ld  %.0f ms\n", total, ms);
}
