struct P { int x, y, z; };
struct P a = { .z = 3, .x = 1 };           /* C99: any order; C++20: must be in declaration order */
int main(void) { return a.x; }
