#!/usr/bin/env bash
# Reproduces the small module experiments of Chapter 38.  Needs g++-14 and clang++-18.  bash run.sh
set -u
tmp=$(mktemp -d); cd "$tmp"
cat > greet.cppm <<'SRC'
module;
#include <string>
export module greet;
export namespace greet { std::string hello(const std::string& who); }
std::string helper(const std::string& s) { return "[" + s + "]"; }   // not exported
std::string greet::hello(const std::string& who) { return helper("hello, " + who); }
SRC
cat > main.cpp <<'SRC'
import greet;
#include <iostream>
int main() { std::cout << greet::hello("modules") << '\n'; }
SRC
cat > main_inc_first.cpp <<'SRC'
#include <iostream>
import greet;
int main() { std::cout << greet::hello("modules") << '\n'; }
SRC
cat > bad.cpp <<'SRC'
import greet;
int main() { auto s = helper("x"); }
SRC

echo "== Clang 18"
clang++-18 -std=c++20 --precompile greet.cppm -o greet.pcm && clang++-18 -std=c++20 -c greet.pcm -o greet_c.o
clang++-18 -std=c++20 -fmodule-file=greet=greet.pcm main.cpp greet_c.o -o app_clang && ./app_clang
echo "BMI size: $(stat -c %s greet.pcm) bytes"
clang++-18 -std=c++20 -fmodule-file=greet=greet.pcm -c bad.cpp 2>&1 | grep error | head -2

echo "== GCC 14   (-x c++ because .cppm is not a recognised extension)"
g++-14 -std=c++20 -fmodules-ts -x c++ -c greet.cppm -o greet_g.o && ls gcm.cache
echo "-- import first, #include after:";  g++-14 -std=c++20 -fmodules-ts main.cpp greet_g.o -o app_gcc 2>&1 | grep -E "error" | head -2
echo "-- #include first, import after:";  g++-14 -std=c++20 -fmodules-ts main_inc_first.cpp greet_g.o -o app_gcc && ./app_gcc
g++-14 -std=c++20 -fmodules-ts -c bad.cpp 2>&1 | grep error | head -2
