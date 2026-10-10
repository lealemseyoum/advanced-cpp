export module shapes;                // primary module interface: the only thing clients can import
export import :geometry;             // re-export the partition's declarations

export namespace shapes {
    struct Circle { Point centre; double radius; };
    double area(const Circle& c);
    double perimeter(const Circle& c);
    bool contains(const Circle& c, Point p);
}
