#!/usr/bin/env bash
# Chapter 42 experiment 5: what does passing a shared_ptr cost? (apt install libbenchmark-dev)
set -e
g++ -O2 -std=c++20 shared_ptr_cost.cpp -lbenchmark -lpthread -o shared_ptr_cost
./shared_ptr_cost --benchmark_min_time=0.3s | grep ^BM
