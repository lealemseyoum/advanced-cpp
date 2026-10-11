#!/usr/bin/env bash
# Chapter 43, experiment 1: what does #include <X> cost a translation unit? (preprocessed lines, parse time)
set -e
cd "$(mktemp -d)"
for h in cstdio cstdint string vector memory map unordered_map algorithm functional iostream sstream regex ranges format chrono thread filesystem print; do
  printf '#include <%s>\nint main(){}\n' $h > h.cpp
  l=$(g++ -std=c++23 -E h.cpp | grep -vc '^#')
  s=$(date +%s.%N); g++ -std=c++23 -fsyntax-only h.cpp; e=$(date +%s.%N)
  printf "%-14s %7d lines  %5.0f ms\n" $h $l "$(echo "($e - $s)*1000" | bc -l)"
done
