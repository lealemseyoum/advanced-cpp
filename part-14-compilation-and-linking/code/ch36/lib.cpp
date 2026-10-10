#include "lib.hpp"
int internal_fn(int x) { return x * 3; }
int public_fn(int x) { return internal_fn(x) + 1; }
