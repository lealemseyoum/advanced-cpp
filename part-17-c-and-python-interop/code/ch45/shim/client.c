/* Pure C11 client: compiled with gcc -std=c11, no C++ headers */
#include <stdio.h>
#include <string.h>
#include "wordcount.h"

static int print_word(const char* word, size_t count, void* user) {
    int* budget = (int*)user;
    printf("  %-6s %zu\n", word, count);
    return --*budget == 0;                       /* stop after N words */
}

int main(void) {
    wc_counter* c = NULL;
    if (wc_create(&c) != WC_OK) return 1;
    const char* text = "the cat and the hat and the bat";
    wc_status st = wc_add_text(c, text, strlen(text));
    printf("add_text: %s\n", wc_status_string(st));
    int budget = 3;
    st = wc_visit(c, print_word, &budget);
    printf("visit: %s\n", wc_status_string(st));
    st = wc_add_text(NULL, "x", 1);
    printf("bad arg: %s (%s)\n", wc_status_string(st), wc_last_error());
    wc_destroy(c);
    wc_destroy(NULL);
    return 0;
}
