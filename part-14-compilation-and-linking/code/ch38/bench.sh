#!/usr/bin/env bash
# Experiment: 20 translation units that need <algorithm>,<iostream>,<map>,<ranges>,<string>,<vector>,
# compiled with textual includes vs with `import stdw;` (a hand-made mini "std" module).  Clang 18, -O0.
set -u
tmp=$(mktemp -d); cd "$tmp"
cat > stdw.cppm <<'SRC'
module;
#include <algorithm>
#include <iostream>
#include <map>
#include <ranges>
#include <string>
#include <vector>
export module stdw;
export namespace std {
    using std::vector; using std::string; using std::map; using std::cout; using std::endl;
    using std::sort; using std::to_string; using std::ostream;
}
SRC
body='{ std::vector<int> v{3,1,2}; std::sort(v.begin(), v.end()); std::map<std::string,int> m; m["a"]=v[0]; return (int)m.size() + (int)std::to_string(v[1]).size(); }'
for i in $(seq 1 20); do
  printf '#include <algorithm>\n#include <iostream>\n#include <map>\n#include <ranges>\n#include <string>\n#include <vector>\nint work_%d() %s\n' $i "$body" > hdr_$i.cpp
  printf 'import stdw;\nint work_%d() %s\n' $i "$body" > mod_$i.cpp
done
best() { local b=999999; for r in 1 2 3; do local s=$(date +%s%N); for i in $(seq 1 20); do "$@" $i; done; local e=$(date +%s%N); local t=$(( (e-s)/1000000 )); [ $t -lt $b ] && b=$t; done; echo $b; }
hdrc() { clang++-18 -std=c++20 -w -c hdr_$1.cpp -o hdr_$1.o 2>/dev/null; }
modc() { clang++-18 -std=c++20 -w -fmodule-file=stdw=stdw.pcm -c mod_$1.cpp -o mod_$1.o 2>/dev/null; }
echo "headers: 20 TUs = $(best hdrc) ms (best of 3)"
s=$(date +%s%N); clang++-18 -std=c++20 --precompile stdw.cppm -o stdw.pcm 2>/dev/null; e=$(date +%s%N)
echo "one-time module build: $(( (e-s)/1000000 )) ms, stdw.pcm = $(stat -c %s stdw.pcm) bytes"
echo "module : 20 TUs = $(best modc) ms (best of 3)"
