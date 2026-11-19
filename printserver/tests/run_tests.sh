#!/bin/sh
# Host unit tests (no hardware needed)
cd "$(dirname "$0")"
g++ -std=c++17 -I. -Iarduino_shim -o test_pwg test_pwg.cpp && ./test_pwg
