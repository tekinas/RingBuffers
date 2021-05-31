mkdir clang_win
cd clang_win
cmake -DCMAKE_SH="CMAKE_SH-NOTFOUND" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G "CodeBlocks - MinGW Makefiles" ..
cmake --build . --parallel 8