export module shapes:geometry;       // partition interface: part of module `shapes`, not importable outside it

export struct Point { double x, y; };
export double distance(Point a, Point b);
