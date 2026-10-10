// translation unit A
#include <cstdio>
inline int answer() { return 1; }          // ODR violation: a different definition lives in tu_b.cpp
template <class T> T twice(T x) { return x + x; }
int from_a() { return answer() + twice(10); }
void print_a() { std::printf("tu_a: answer()=%d\n", answer()); }
