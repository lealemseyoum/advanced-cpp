#include <cstdio>
extern int A;
int make_b() { std::printf("  init_b: constructing B, A is %d at this moment\n", A); return A + 1; }
int B = make_b();
