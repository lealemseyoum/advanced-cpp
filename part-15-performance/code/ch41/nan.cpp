#include <cmath>
#include <cstdio>
#include <limits>
int main(int argc, char**) {
    double x = argc > 5 ? 1.0 : std::numeric_limits<double>::quiet_NaN();
    std::printf("isnan(x) = %d,  x != x = %d\n", std::isnan(x), x != x);
}
