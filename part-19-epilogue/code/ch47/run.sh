#!/usr/bin/env bash
# Compile probe.cpp with every compiler found, at the newest standard mode it accepts, and print the support matrix.
set -u
cd "$(dirname "$0")"
mkdir -p results
probe() {  # name, compiler, flags...
  local name=$1; shift
  if command -v "$1" >/dev/null 2>&1; then
    "$@" probe.cpp -o "results/probe_$name" 2>"results/$name.err" && "results/probe_$name" >"results/$name.txt" \
      && echo "$name: $("$1" --version | head -1)  [$(grep -vc '=-$' "results/$name.txt") of $(wc -l <"results/$name.txt") macros defined]"
  fi
}
probe gcc13  g++     -std=c++23     # GCC 13 has no -std=c++26 / c++2c
probe gcc14  g++-14  -std=c++26
probe clang18 clang++ -std=c++2c
python3 matrix.py | tee results/matrix.md >/dev/null
echo "matrix written to results/matrix.md"
