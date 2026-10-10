#!/usr/bin/env bash
# Reproduces the shell experiments of Chapter 37. Run from this directory:  bash run.sh
set -u
X=${CXX:-g++-14}
tmp=$(mktemp -d); cp -r ./* "$tmp"/; cd "$tmp"
build() { $X -O1 -fPIC -shared -I"$2" $3 "$1" -o "$4"; }

echo "== A. data layout: insert a field, do not recompile the client"
build config.cpp v1 "" libcfg_v1.so; build config.cpp v2 "" libcfg_v2.so
$X -O1 -Iv1 client_config.cpp -L. -l:libcfg_v1.so -Wl,-rpath,'$ORIGIN' -o cfg_client
./cfg_client; cp libcfg_v2.so libcfg_v1.so; echo "-- library replaced by v2:"; ./cfg_client

echo "== B. virtual function inserted before existing ones"
build shape.cpp v1 "" libshape_v1.so; build shape.cpp v2 -DV2 libshape_v2.so
$X -O1 -Iv1 client_shape.cpp -L. -l:libshape_v1.so -Wl,-rpath,'$ORIGIN' -o shape_client
./shape_client; cp libshape_v2.so libshape_v1.so; echo "-- library replaced by v2:"; ./shape_client

echo "== C. pImpl survives"
$X -O1 -fPIC -shared -Iv1 widget_v1.cpp -o libwidget.so
$X -O1 -Iv1 client_widget.cpp -L. -lwidget -Wl,-rpath,'$ORIGIN' -o widget_client
./widget_client
$X -O1 -fPIC -shared -Iv2 widget_v2.cpp -o libwidget.so; echo "-- library replaced by v2 (private layout changed):"; ./widget_client

echo "== D. the dual std::string ABI"
$X -std=c++17 -c dual.cpp -o dual_new.o; $X -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=0 -c dual.cpp -o dual_old.o
nm dual_new.o | grep greet; nm dual_old.o | grep greet
$X -std=c++17 -c dual_main.cpp -o dual_main_new.o
$X dual_main_new.o dual_old.o -o dual_bad 2>&1 | head -3

echo "== E. libstdc++ symbol versioning"
L=/usr/lib/x86_64-linux-gnu/libstdc++.so.6
echo "GLIBCXX versions: $(objdump -T $L | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | wc -l); newest: $(objdump -T $L | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | tail -1)"
printf '#include <iostream>\n#include <string>\nint main(){std::string s="x";std::cout<<s;}\n' > req.cpp
$X -std=c++17 req.cpp -o req && objdump -T req | grep -oE 'GLIBCXX_[0-9.]+|CXXABI_[0-9.]+|GLIBC_[0-9.]+' | sort -uV | tr '\n' ' '; echo
