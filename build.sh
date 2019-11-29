#!/bin/sh
echo "CDT and CMake must be correctly installed!"
mkdir build
cd build && cmake .. && make
find -name "*.wasm"|xargs sha256sum
