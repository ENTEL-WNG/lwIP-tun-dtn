#!/bin/bash

find src include -type f \( -name "*.c" -o -name "*.h" \) | xargs clang-format -i