#!/usr/bin/env bash

mkdir -p ../build

# -pedantic
Flags="-Wall -Wextra -std=c++11 -Wno-write-strings -fno-rtti -fno-exceptions"
g++ -g main.cpp -o ../build/editor $Flags
