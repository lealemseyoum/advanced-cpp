#!/usr/bin/env bash
# Chapter 40 experiments. Needs: perf (linux-tools-generic), valgrind, and (optional) FlameGraph:
#   git clone --depth 1 https://github.com/brendangregg/FlameGraph.git
# This VM has no hardware PMU, so we sample with the software clock: -e cpu-clock
set -e
for v in 1 2 3; do g++ -O2 -g -fno-omit-frame-pointer -std=c++20 -DV=$v app.cpp -o app$v; done
echo "== times (3 runs each)";  for v in 1 2 3; do for i in 1 2 3; do ./app$v; done; done

echo "== perf: self time, frame-pointer call graph"
perf record -e cpu-clock -F 5000 --call-graph fp -o v1.data ./app1 2000000
perf report -i v1.data --no-children --stdio -g none --percent-limit 3 | grep -v '^#' | grep -v '^$'
echo "== perf: inclusive time"
perf report -i v1.data --children --stdio -g none --percent-limit 4 | grep -v '^#' | grep -v '^$' | head -12
echo "== perf: who calls memcmp? fp loses the immediate caller of leaf functions, dwarf recovers it"
perf report -i v1.data --no-children --stdio -G -S __memcmp_evex_movbe | grep -v '^#' | head -8
perf record -e cpu-clock -F 2000 --call-graph dwarf,16384 -o v1d.data ./app1 500000
perf report -i v1d.data --no-children --stdio -G -S __memcmp_evex_movbe | grep -v '^#' | cut -c1-110 | head -10

echo "== flame graph (needs FlameGraph/ next to this script)"
if [ -d FlameGraph ]; then
  perf script -i v1.data | FlameGraph/stackcollapse-perf.pl > v1.folded
  FlameGraph/flamegraph.pl --title "app v1 (cpu-clock, fp)" v1.folded > v1.svg
fi

echo "== valgrind dhat: allocation counts"
for v in 1 2 3; do valgrind --tool=dhat --dhat-out-file=/dev/null ./app$v 200000 2>&1 | grep -E 'Total:'; done
echo "== valgrind callgrind: instruction counts"
for v in 1 3; do valgrind --tool=callgrind --callgrind-out-file=cg$v.out ./app$v 200000 2>&1 | grep 'refs'; done

echo "== layout: cachegrind (simulated D1/LL) and timings"
g++ -O2 -g -std=c++20 layout.cpp -o layout
valgrind --tool=cachegrind --cache-sim=yes --cachegrind-out-file=cgl.out ./layout 2>&1 | grep -E 'D1  misses|LLd misses'
cg_annotate cgl.out | grep -E 'sum_aos|sum_soa' | head
g++ -O2 -std=c++20 layout_time.cpp -o layout_time && ./layout_time
