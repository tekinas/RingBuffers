#!/usr/bin/bash
ls
mkdir gcc_build
cd gcc_build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build . --parallel $(nproc)

