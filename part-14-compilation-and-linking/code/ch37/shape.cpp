#include "shape.hpp"
Shape::~Shape() {}
#ifdef V2
const char* Shape::name() const { return "shape"; }
#endif
int Shape::sides() const { return 4; }
int Shape::corners() const { return 40; }
Shape* make_shape() { return new Shape; }
