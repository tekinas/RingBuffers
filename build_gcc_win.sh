mkdir gcc_win
cd gcc_win
cmake -DCMAKE_SH="CMAKE_SH-NOTFOUND" -DCMAKE_BUILD_TYPE=Release -G "CodeBlocks - MinGW Makefiles" ..
cmake --build . --parallel 8