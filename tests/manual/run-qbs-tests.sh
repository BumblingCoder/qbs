#!/bin/bash

# Usage
# ./test.sh qbs debug release profile:x64 platform:clang

for i in *; do
    [ -d "$i" ] || continue
    cd "$i"
    "$@"
    cd ..
done
