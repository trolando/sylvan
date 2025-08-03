#!/bin/bash
set -e
mkdir -p build
cmake -S . -B build -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}"
cmake --build build
./build/test

