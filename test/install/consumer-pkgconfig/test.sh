#!/bin/bash
set -e

echo "Testing static linking with pkg-config --static..."

echo "CFLAGS:"
pkg-config --cflags --static sylvan

echo "LIBS:"
pkg-config --libs --static sylvan

echo "Compiling with static link..."
cc main.c $(pkg-config --cflags --static sylvan) $(pkg-config --libs --static sylvan) -o test

echo "Inspecting binary..."
file test
if command -v otool &> /dev/null; then
    otool -L ./test
elif command -v ldd &> /dev/null; then
    ldd ./test
fi

echo "Running binary..."
./test
