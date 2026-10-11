#!/usr/bin/env bash
# Chapter 43 experiment 3: the run-time price of pImpl
set -e; cd "$(dirname "$0")"
g++ -O2 -std=c++20 bench.cpp counter.cpp -lbenchmark -lpthread -o bench
./bench --benchmark_min_time=0.3s | grep ^BM
g++ -O2 -std=c++20 -flto bench.cpp counter.cpp -lbenchmark -lpthread -o bench_lto
./bench_lto --benchmark_min_time=0.3s | grep ^BM
