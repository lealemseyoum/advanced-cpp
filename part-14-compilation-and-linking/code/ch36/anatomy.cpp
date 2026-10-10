#include <cstdio>
#include <vector>

int global_counter = 5;                  // defined here: external linkage, data
static int file_local = 7;               // internal linkage: not visible to other TUs
extern int defined_elsewhere;            // declaration only
int helper(int);                         // declaration only

template <class T> T twice(T x) { return x + x; }       // implicitly instantiated below
inline int inl(int x) { return x + 1; }

int api(int x) {
    std::vector<int> v{1, 2, 3};
    std::printf("%d\n", file_local + defined_elsewhere);
    return helper(x) + twice(x) + inl(x) + static_cast<int>(v.size());
}
