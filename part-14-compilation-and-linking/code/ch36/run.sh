#!/usr/bin/env bash
# Reproduces the shell experiments of Chapter 36. Run from this directory:  bash run.sh
set -u
CXX=${CXX:-g++-14}
tmp=$(mktemp -d); cp ./*.cpp ./*.hpp "$tmp"/; cd "$tmp"

echo "== E1: what a header costs (preprocessed lines)"
for h in cstdio iostream vector ranges format print; do
  echo "#include <$h>" > h.cpp; printf '%-9s %s\n' "$h" "$($CXX -std=c++23 -E h.cpp | wc -l)"; done

echo "== E2: object file anatomy"
$CXX -std=c++17 -O0 -c anatomy.cpp -o anatomy.o
nm -C anatomy.o | grep -vE "std::|__gnu|operator|_Vector|allocator"
echo "COMDAT groups: $(readelf -g anatomy.o | grep -c 'COMDAT group')"

echo "== E3: ODR violation, same inline function, two bodies"
$CXX -O0 -c tu_a.cpp tu_b.cpp odr_main.cpp
for order in "tu_a.o tu_b.o" "tu_b.o tu_a.o"; do $CXX odr_main.o $order -o odr && echo "link order: $order" && ./odr; done
$CXX -O2 -c tu_a.cpp tu_b.cpp odr_main.cpp && $CXX odr_main.o tu_a.o tu_b.o -o odr && echo "-O2:" && ./odr

echo "== E4: static vs shared, visibility"
$CXX -O1 -c -fPIC -DBUILD_LIB lib.cpp -o lib.o
$CXX -shared -o libfoo_all.so lib.o
$CXX -shared -fvisibility=hidden -DBUILD_LIB -fPIC lib.cpp -o libfoo_hidden.so
echo "exports (default):"; nm -D --defined-only libfoo_all.so | awk '{print $2,$3}'
echo "exports (hidden + API):"; nm -D --defined-only libfoo_hidden.so | awk '{print $2,$3}'

echo "== E5: static initialisation order across TUs"
$CXX -c init_a.cpp init_b.cpp init_main.cpp
$CXX init_main.o init_a.o init_b.o -o i1 && ./i1
$CXX init_main.o init_b.o init_a.o -o i2 && ./i2

echo "== E6: static-library link order"
$CXX -c ar_main.cpp ar_util.cpp && ar rcs libutil.a ar_util.o
$CXX -L. -lutil ar_main.o -o a1 2>&1 | head -3
$CXX ar_main.o -L. -lutil -o a2 && echo "object before lib: ok"

echo "== E7: extern template (5 TUs instantiating Box<std::string>)"
cat > et_tmpl.hpp <<'HDR'
#pragma once
#include <string>
#include <vector>
template <class T> struct Box {
    std::vector<T> v;
    void add(T x) { v.push_back(std::move(x)); }
    T sum() const { T s{}; for (auto& x : v) s = s + x; return s; }
};
#ifdef USE_EXTERN
extern template struct Box<std::string>;       // promise: instantiated in exactly one TU
#endif
HDR
for i in 1 2 3 4 5; do printf '#include "et_tmpl.hpp"\nint f%d() { Box<std::string> b; b.add("x"); return (int)b.sum().size(); }\n' $i > et_$i.cpp; done
printf '#include "et_tmpl.hpp"\ntemplate struct Box<std::string>;\n' > et_inst.cpp
for mode in implicit extern; do
  flags="-std=c++17 -O0"; [ $mode = extern ] && flags="$flags -DUSE_EXTERN"
  tot=0; for i in 1 2 3 4 5; do $CXX $flags -c et_$i.cpp -o et_$i.o; t=$(size et_$i.o | awk 'NR==2{print $1}'); tot=$((tot+t)); done
  echo "$mode: sum of .text in the 5 objects = $tot bytes"
done
