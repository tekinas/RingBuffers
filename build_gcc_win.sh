mkdir gcc_win
cd gcc_win
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_1r_1w.cpp" -o g_fq_test_1r_1w.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_1r_nw.cpp" -o g_fq_test_1r_nw.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_nr_1w.cpp" -o g_fq_test_nr_1w.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_nr_nw.cpp" -o g_fq_test_nr_nw.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_mr_mw.cpp" -o g_fq_test_mr_mw.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\mingw\include" "..\src\fq_test_nr_nw_asio.cpp" -lws2_32 -o g_fq_test_nr_nw_asio.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\mingw\include" "..\src\fq_test_call_only.cpp" -o g_fq_test_call_only.exe
g++ --std=c++2a -Ofast -fno-exceptions -fno-rtti -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\mingw\include" "..\src\fq_test_callNDelete.cpp" -o g_fq_test_callNDelete.exe