/* A C API in front of a C++ implementation: opaque handle, status codes, callbacks, explicit ownership. */
#ifndef WORDCOUNT_H
#define WORDCOUNT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct wc_counter wc_counter;                  /* opaque: C sees no members, no layout */

typedef enum { WC_OK = 0, WC_ERR_NOMEM = 1, WC_ERR_ARG = 2, WC_ERR_CALLBACK = 3, WC_ERR_INTERNAL = 4 } wc_status;

typedef int (*wc_visit_fn)(const char* word, size_t count, void* user);   /* return non-zero to stop */

wc_status   wc_create(wc_counter** out);               /* *out is owned by the caller until wc_destroy */
void        wc_destroy(wc_counter*);                   /* accepts NULL */
wc_status   wc_add_text(wc_counter*, const char* text, size_t len);
wc_status   wc_visit(const wc_counter*, wc_visit_fn fn, void* user);
const char* wc_last_error(void);                       /* thread-local message of the last failure on this thread */
const char* wc_status_string(wc_status);

#ifdef __cplusplus
}
#endif
#endif
