#!/usr/bin/env python3
"""Build a support matrix from feature-test macros. Run: ./run.sh  (compiles probe.cpp with each installed compiler)."""
import json, sys, pathlib
feat = json.load(open('features.json'))
files = {'GCC 13.3': 'results/gcc13.txt', 'GCC 14.2': 'results/gcc14.txt', 'Clang 18.1.3': 'results/clang18.txt'}
def load(p):
    d = {}
    for l in pathlib.Path(p).read_text().splitlines():
        k, v = l.split('='); d[k] = v
    return d
data = {n: load(p) for n, p in files.items()}
print('| Feature | Std | ' + ' | '.join(files) + ' |')
print('|---|:--:|' + ':--:|' * len(files))
for m, label, std, kind in feat:
    cells = [('`' + data[n][m] + '`') if data[n][m] != '-' else '—' for n in files]
    print(f'| {label} (`{m}`) | {std} | ' + ' | '.join(cells) + ' |')
