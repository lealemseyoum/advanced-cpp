#!/usr/bin/env bash
# Chapter 46 experiments. Needs: python3.12 + headers (python3.12-dev), pybind11 (python3-pybind11), numpy, Cython (pip), g++ >= 13.
set -e; cd "$(dirname "$0")"
PY=python3.12
SUF=$($PY -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))")
INC="$($PY-config --includes)"
PB="-I$($PY -c 'import pybind11;print(pybind11.get_include())')"
echo "== build"
g++ -std=c++20 -O2 -Wall -Wextra -shared -fPIC $INC capi_ext.cpp -o capi_ext$SUF
g++ -std=c++20 -O2 -Wall -Wextra -shared -fPIC -fvisibility=hidden $INC $PB pb_ext.cpp -o pb_ext$SUF
$PY -m cython -3 --cplus cy_ext.pyx -o cy_ext.cpp
g++ -O2 -shared -fPIC -fvisibility=hidden $INC cy_ext.cpp -o cy_ext$SUF
echo "== 1: raw C API";        $PY test_capi.py
echo "== 2: pybind11";         $PY test_pb.py
echo "== 3: speed and the GIL"; $PY bench_py.py
echo "== 4: ABI tags";          $PY - <<'P'
import pb_ext; print(pb_ext.__file__.rsplit('/', 1)[-1])
P
nm -D --defined-only pb_ext$SUF | grep ' T '
strings pb_ext$SUF | grep -m1 pybind11_internals
python3.13 -c "import pb_ext" 2>&1 | tail -1 || true
