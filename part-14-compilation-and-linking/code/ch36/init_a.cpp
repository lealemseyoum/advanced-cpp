#include <cstdio>
int make_a() { std::puts("  init_a: constructing A"); return 1; }
int A = make_a();
