#pragma once
#ifdef BUILD_LIB
#  define API __attribute__((visibility("default")))
#else
#  define API
#endif
API int public_fn(int x);
int internal_fn(int x);      // not meant to be exported
