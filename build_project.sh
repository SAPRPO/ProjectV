#!/bin/bash

echo $(pwd)

BUILD_TYPE="$1"

if [ -z "$BUILD_TYPE" ]; then
    BUILD_TYPE="Debug"
    echo "No build type specified, defaulting to 'Debug'"
fi

if [ "$BUILD_TYPE" != "Debug" ] && [ "$BUILD_TYPE" != "Release" ]; then
    echo "Error: Invalid build type '$BUILD_TYPE'. Use 'Debug' or 'Release'."
    exit 1
fi

echo "Build type: $BUILD_TYPE"

if [ -d "build" ]; then
    echo "Removing directory 'build'"
    rm -rf build
    echo "Directory 'build' removed"
else
    echo "Build folder will be created"
fi


cmake -S . -B build -DCMAKE_BUILD_TYPE=$BUILD_TYPE

echo "Building project:"
cmake --build build -j 