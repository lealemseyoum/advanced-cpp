module;
#include <cmath>
module shapes;                       // implementation unit: sees the interface, adds definitions, exports nothing

double distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

namespace {                          // internal linkage: invisible to every importer, as ever
constexpr double kPi = 3.14159265358979323846;
}

namespace shapes {
double area(const Circle& c)      { return kPi * c.radius * c.radius; }
double perimeter(const Circle& c) { return 2 * kPi * c.radius; }
bool contains(const Circle& c, Point p) { return distance(c.centre, p) <= c.radius; }
}
