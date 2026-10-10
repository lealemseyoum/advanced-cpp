// translation unit B
#include <cstdio>
inline int answer() { return 2; }          // same name and signature, different body
template <class T> T twice(T x) { return x + x; }
int from_b() { return answer() + twice(10); }
void print_b() { std::printf("tu_b: answer()=%d\n", answer()); }
