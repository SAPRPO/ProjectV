#!/bin/bash

TESTS="$1"


if [ -z "$TESTS" ]; then
    echo "Error: no argument provided."
    echo "Usage: $0 all        # run all tests"
    echo "       $0 <test_name> # run specific test(s) by regex"
    exit 1
fi

if [ "$1" == "all" ]; then
    echo "Running all tests..."
    ctest --test-dir build --output-on-failure
else
    echo "Running tests matching: $TESTS"
    ctest --test-dir build -R "$TESTS" --output-on-failure
fi