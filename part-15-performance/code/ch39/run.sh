#!/usr/bin/env bash
# Chapter 39 experiments (Google Benchmark 1.8.3: apt install libbenchmark-dev)
set -e
g++ -O2 -std=c++20 dce.cpp -lbenchmark -lpthread -o dce
g++ -O2 -std=c++20 realistic.cpp -lbenchmark -lpthread -o realistic
g++ -O2 -std=c++20 -fno-tree-vectorize -fno-if-conversion -fno-if-conversion2 realistic.cpp -lbenchmark -lpthread -o realistic_nb
g++ -O2 -std=c++20 harness.cpp -o harness
echo "== 1: DCE / constant folding";   ./dce --benchmark_min_time=0.2s | grep ^BM
echo "== 3a: branchy, default -O2 (compiler emits cmov)";      ./realistic --benchmark_filter=Branchy --benchmark_min_time=0.2s | grep ^BM
echo "== 3b: branchy, branches forced";                        ./realistic_nb --benchmark_filter=Branchy --benchmark_min_time=0.2s | grep ^BM
echo "== 4: working-set sweep";        ./realistic --benchmark_filter=Chase --benchmark_min_time=0.2s | grep ^BM
echo "== 5: variance";  ./dce --benchmark_filter=Escaped --benchmark_repetitions=10 --benchmark_report_aggregates_only=true | grep ^BM
echo "== 6: hand harness"; ./harness
