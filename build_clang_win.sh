mkdir clang_win
cd clang_win
clang++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_1r_1w.cpp" -o c_fq_test_1r_1w.exe
clang++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_1r_nw.cpp" -o c_fq_test_1r_nw.exe
clang++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_nr_1w.cpp" -o c_fq_test_nr_1w.exe
clang++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_nr_nw.cpp" -o c_fq_test_nr_nw.exe
clang++ --std=c++2a -Ofast -fno-exceptions -fno-rtti "..\src\fq_test_mr_mw.cpp" -o c_fq_test_mr_mw.exe
clang++ --std=c++2a -Ofast -fno-exceptions -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\msvc\include\common_libs" "..\src\fq_test_call_only.cpp" -o c_fq_test_call_only.exe
clang++ --std=c++2a -Ofast -fno-exceptions -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\msvc\include\common_libs" "..\src\fq_test_callNDelete.cpp" -o c_fq_test_callNDelete.exe
clang++ --std=c++2a -Ofast -fno-exceptions -D_SILENCE_CLANG_COROUTINE_MESSAGE -DBOOST_ERROR_CODE_HEADER_ONLY -DBOOST_DATE_TIME_NO_LIB -DBOOST_REGEX_NO_LIB -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\msvc\include\boost_libs\clang_boost" -I"C:\Users\Aniket Bisht\Documents\Libraries\Windows\msvc\include\common_libs" "..\src\fq_test_nr_nw_asio.cpp" -lws2_32 -o c_fq_test_nr_nw_asio.exe