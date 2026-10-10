#include <cstdio>
import shapes;

int main() {
    shapes::Circle c{{0, 0}, 2.0};
    std::printf("area %.3f, perimeter %.3f\n", shapes::area(c), shapes::perimeter(c));
    std::printf("contains (1,1): %s, contains (3,0): %s\n", shapes::contains(c, {1, 1}) ? "yes" : "no", shapes::contains(c, {3, 0}) ? "yes" : "no");
    std::printf("distance((0,0),(3,4)) = %.1f\n", distance(Point{0, 0}, Point{3, 4}));
}
