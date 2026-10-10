#include "config.hpp"
#include <cstdio>
int main() {
    Config c = make_config();
    std::printf("client sees width=%d height=%d   library area()=%d\n", c.width, c.height, area(c));
}
