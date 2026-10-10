#!/usr/bin/env bash
# Chapter 41 experiments (GCC 14.2, Clang 18; pass CXX=... to change)
set -e
echo "== 1: optimisation levels (4 kernels, n=4096, best of 9)"
for O in -O0 -O1 -O2 -O3 -Ofast "-O3 -march=native" "-O2 -ftree-vectorize"; do
  g++-14 $O -std=c++20 levels.cpp -o lv; printf "gcc   %-22s" "$O"; ./lv | cut -c1-75; done
for O in -O1 -O2 -O3 -Ofast; do
  clang++-18 $O -std=c++20 levels.cpp -o lvc; printf "clang %-22s" "$O"; ./lvc | cut -c1-75; done
echo "== 2: LTO (cross-TU inlining)"
g++-14 -O2 lto_main.cpp lto_lib.cpp -o lto_off; g++-14 -O2 -flto lto_main.cpp lto_lib.cpp -o lto_on
for i in 1 2 3; do ./lto_off; ./lto_on; done
echo "== 3: devirtualization, see the assembly"
g++-14 -std=c++20 -O2 -fno-stack-protector -S -masm=intel -o - dv.cpp | grep -A12 '^_Z8via_rect'
echo "== 4: PGO"
rm -f *.gcda
g++-14 -O2 pgo.cpp -o pgo_base
g++-14 -O2 -fprofile-generate pgo.cpp -o pgo_x && ./pgo_x > /dev/null
g++-14 -O2 -fprofile-use -fprofile-correction pgo.cpp -o pgo_x
for i in 1 2 3; do ./pgo_base; ./pgo_x; done
echo "== 5: -Ofast and NaN"
g++-14 -O2 nan.cpp -o n2 && ./n2; g++-14 -Ofast nan.cpp -o nf && ./nf
