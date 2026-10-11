#!/usr/bin/env bash
# Build the extension with the interpreter's own flags and run the tests and benchmark.
set -euo pipefail
cd "$(dirname "$0")"
PY=${PY:-python3.12}
EXT=$($PY-config --extension-suffix)
g++ -std=c++20 -O2 -Wall -Wextra -fvisibility=hidden -shared -fPIC \
    $($PY -m pybind11 --includes) fastmath.cpp -o "fastmath$EXT"
$PY test_fastmath.py
$PY bench.py
