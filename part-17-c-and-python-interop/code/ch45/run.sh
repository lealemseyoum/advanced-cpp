#!/usr/bin/env bash
# Chapter 45 experiments. Needs libsqlite3-dev, gcc, g++ >= 13.
set -e; cd "$(dirname "$0")"
echo "== 1: RAII wrapper over the SQLite C API"
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined demo.cpp -lsqlite3 -o demo && ./demo
echo "== 2: exceptions through C frames (0 = qsort, 1 = throw through sqlite3_exec, 2 = polite abort)"
g++ -std=c++23 -g -fsanitize=address throw_through_c.cpp -lsqlite3 -o ttc_asan
for m in 0 1 2; do echo "-- mode $m"; ./ttc_asan $m 2>&1 | grep -E "caught|sqlite3_close|SUMMARY" | cut -c1-120; done
echo "== 3: a C API in front of C++, used from pure C"
( cd shim
  g++ -std=c++23 -Wall -Wextra -fPIC -shared wordcount.cpp -o libwordcount.so
  gcc -std=c11 -Wall -Wextra -Werror client.c -L. -lwordcount -Wl,-rpath,. -o client_c && ./client_c
  nm -D --defined-only libwordcount.so | grep ' T '
  ldd libwordcount.so | grep -E 'stdc|libc.so' )
echo "== 4: the same file as C and as C++"
gcc -std=c11 -Wall c_vs_cpp.c -o cc && ./cc
g++ -std=c++23 -x c++ -Wall c_vs_cpp.c -o cx && ./cx
