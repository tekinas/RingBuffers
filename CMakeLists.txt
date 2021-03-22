cmake_minimum_required(VERSION 3.16)

project(FunctionQueue)

set(CMAKE_CXX_STANDARD 20)

if (UNIX)
    link_libraries(pthread)
endif ()

add_executable(fq_test_call_only src/fq_test_call_only.cpp)
add_executable(fq_test_callNDelete src/fq_test_callNDelete.cpp)
add_executable(fq_test_1r_1w src/fq_test_1r_1w.cpp)
add_executable(fq_test_nr_nw_asio src/fq_test_nr_nw_asio.cpp)
add_executable(fq_test_nr_nw src/fq_test_nr_nw.cpp)
add_executable(fq_test_nr_1w src/fq_test_nr_1w.cpp)
add_executable(fq_test_1r_nw src/fq_test_1r_nw.cpp)
add_executable(fq_test_mr_mw src/fq_test_mr_mw.cpp)

if (WIN32)
    set(LIB_DIR "C:/Users/Aniket Bisht/Documents/Libraries/Windows")  # set the library directory for Windows ::::
endif (WIN32)

if (MINGW)
    set(LIB_DIR ${LIB_DIR}/mingw)
    set(LIBS ${LIB_DIR}/lib)
    include_directories(${LIB_DIR}/include)
endif ()

if (MSVC OR (WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")))
    set(LIB_DIR ${LIB_DIR}/msvc)
    set(COMMON_LIB_DIR ${LIB_DIR}/lib/common_libs)
    include_directories(${LIB_DIR}/include/common_libs)
endif ()

set(LIB_DIR ${LIB_DIR}/mingw)
set(LIBS ${LIB_DIR}/lib)
include_directories(${LIB_DIR}/include)
