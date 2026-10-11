#include <stdlib.h>
int main(void) {
    int* p = malloc(4 * sizeof(int));    /* fine in C, an error in C++: void* does not convert implicitly */
    free(p);
    return 0;
}
