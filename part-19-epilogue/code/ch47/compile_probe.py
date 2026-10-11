#!/usr/bin/env python3
"""Compile each snippet with each installed compiler (-fsyntax-only, newest standard that compiler accepts)."""
import subprocess, shutil, sys, os, tempfile, json, pathlib
sys.path.insert(0, os.path.dirname(__file__))
from snippets import S

CONFIGS = [  # label, compiler, flags
    ("GCC 13.3", "g++", ["-std=c++23"]),
    ("GCC 14.2", "g++-14", ["-std=c++26"]),
    ("Clang 18.1", "clang++", ["-std=c++2c"]),
]
def modules_flags(label):  # `import std;` cannot be built here: record the honest reason instead of a bogus pass
    return None

res = {}
with tempfile.TemporaryDirectory() as d:
    for label, cxx, flags in CONFIGS:
        if not shutil.which(cxx): continue
        res[label] = {}
        for name, std, code in S:
            f = pathlib.Path(d) / "t.cpp"; f.write_text(code if name.startswith("import std") is False else code)
            extra = ["-fmodules-ts"] if name.startswith("import std") and cxx.startswith("g++") else []
            p = subprocess.run([cxx, *flags, *extra, "-fsyntax-only", "-Wno-attributes", str(f)], capture_output=True, text=True)
            ok = p.returncode == 0
            res[label][name] = ok
            if "-v" in sys.argv and not ok: print(f"[{label}] {name}: {p.stderr.splitlines()[0] if p.stderr else ''}")
json.dump(res, open("results/compile_probe.json", "w"), indent=1)

labels = [c[0] for c in CONFIGS if c[0] in res]
out = ["| Feature | Std | " + " | ".join(labels) + " |", "|---|:--:|" + ":--:|" * len(labels)]
for name, std, _ in S:
    out.append(f"| {name} | {std} | " + " | ".join("✅" if res[l][name] else "❌" for l in labels) + " |")
pathlib.Path("results/compile_matrix.md").write_text("\n".join(out) + "\n")
print("\n".join(out))
print()
for l in labels: print(f"{l}: {sum(res[l].values())} of {len(S)} compile")
