#!/usr/bin/env bash
# Chapter 43, experiment 2: private members in headers cost every includer
set -e
cd "$(dirname "$0")"
cat > client.tpl <<'T'
#include "WIDGET_HDR"
bool client_N(const std::string& s) { Widget w("a+b"); return w.matches(s); }
T
for kind in plain pimpl; do
  for i in $(seq 1 20); do sed "s/WIDGET_HDR/widget_$kind.hpp/; s/client_N/client_$i/" client.tpl > c_${kind}_$i.cpp; done
done
for kind in plain pimpl; do
  s=$(date +%s.%N)
  for i in $(seq 1 20); do g++ -std=c++20 -O1 -c c_${kind}_$i.cpp -o /dev/null; done
  e=$(date +%s.%N); printf "%s: 20 clients compile in %.1f s\n" $kind "$(echo "$e - $s" | bc)"
  n=0; for i in $(seq 1 20); do g++ -std=c++20 -M c_${kind}_$i.cpp | grep -q "/regex " && n=$((n+1)); done
  echo "  $n / 20 clients depend on <regex>"
done
rm -f c_*.cpp client.tpl
