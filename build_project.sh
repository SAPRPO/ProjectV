#!/bin/bash

echo $(pwd)
if [ -d "build" ]; then
    echo "Removing directory 'build'"
    rm -rf build
    echo "Directory 'build' removed"
else
    echo "Build folder will be created"
fi

echo "Configure project. Configuration $1"
cmake -S . -B build -DCMAKE_BUILD_TYPE=$1

echo "Building project:"
cmake --build build -j 