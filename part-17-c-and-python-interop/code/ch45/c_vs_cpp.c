/* The same file, compiled as C11 and as C++23 */
#include <stdio.h>
#include <stdbool.h>
#ifdef __cplusplus
#define LANG "C++"
#else
#define LANG "C"
#endif
enum Color { RED, GREEN };
struct S { char c; };
int main(void) {
    const int n = 4;
    printf("%-3s sizeof('a')=%zu  sizeof(enum Color)=%zu  sizeof(bool)=%zu  sizeof(1==1)=%zu  sizeof(struct S)=%zu\n",
           LANG, sizeof('a'), sizeof(enum Color), sizeof(bool), sizeof(1 == 1), sizeof(struct S));
#ifdef __cplusplus
    int arr[n]; (void)arr; printf("%-3s int arr[n] with const int n: constant expression, fixed array\n", LANG);
#else
    int arr[n]; (void)arr; printf("%-3s int arr[n] with const int n: variable-length array (VLA)\n", LANG);
#endif
    return 0;
}
