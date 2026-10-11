#!/usr/bin/env bash
# Chapter 44 experiment 1 and 2: dispatch mechanisms and strategy injection (GCC and Clang)
set -e; cd "$(dirname "$0")"
for cxx in g++ clang++-18; do echo "== $cxx"; $cxx -O2 -std=c++20 dispatch.cpp -lbenchmark -lpthread -o dispatch_bin; ./dispatch_bin --benchmark_min_time=0.3s | grep ^BM; done
