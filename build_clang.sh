#!/usr/bin/bash
ls
mkdir clang_build
cd clang_build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build . --parallel $(nproc)