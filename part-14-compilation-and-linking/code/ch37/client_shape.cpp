#include "shape.hpp"
#include <cstdio>
int main() {
    Shape* s = make_shape();
    std::printf("client calls sides()   -> %d\n", s->sides());
    std::printf("client calls corners() -> %d\n", s->corners());
    delete s;
}
