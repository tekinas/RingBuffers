cmake_minimum_required(VERSION 3.16)

project(FunctionQueue)

set(CMAKE_CXX_STANDARD 20)

if (UNIX)
    set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} "-fno-rtti -fno-exceptions")
    link_libraries(pthread)
elseif (WIN32)
    set(LIB_DIR "C:/Users/Aniket Bisht/Documents/Libraries/Windows")  # set the library directory for Windows ::::
    link_libraries(ws2_32)
endif ()

if (MINGW)
    set(LIB_DIR ${LIB_DIR}/mingw)
    set(LIBS ${LIB_DIR}/lib)

    set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} "-fno-rtti -fno-exceptions")
    include_directories(${LIB_DIR}/include)
elseif (MSVC OR (WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")))
    add_compile_definitions(_USE_MATH_DEFINES _WIN32_WINDOWS=0x0601)

    set(LIB_DIR ${LIB_DIR}/msvc)
    set(COMMON_LIB_DIR ${LIB_DIR}/lib/common_libs)
    include_directories(${LIB_DIR}/include/common_libs)
    add_compile_definitions(_SILENCE_CLANG_COROUTINE_MESSAGE BOOST_ERROR_CODE_HEADER_ONLY BOOST_DATE_TIME_NO_LIB BOOST_REGEX_NO_LIB)

    if (MSVC)
        include_directories(${LIB_DIR}/include/boost_libs/msvc_boost)
        link_directories(${LIB_DIR}/lib/boost_libs/msvc_boost)
    elseif ((WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")))
        set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} "-fno-exceptions")
        include_directories(${LIB_DIR}/include/boost_libs/clang_boost)
        link_directories(${LIB_DIR}/lib/boost_libs/clang_boost)
    endif ()
endif ()

add_executable(fq_test_call_vector src/tests/fq_test_call_vector.cpp)
add_executable(fq_test_call_queue src/tests/fq_test_call_queue.cpp)
add_executable(fq_test_1r_1w src/tests/fq_test_1r_1w.cpp)
add_executable(fq_test_nr_nw_asio src/tests/fq_test_nr_nw_asio.cpp)
add_executable(fq_test_nr_nw src/tests/fq_test_nr_nw.cpp)
add_executable(fq_test_nr_1w src/tests/fq_test_nr_1w.cpp)
add_executable(fq_test_1r_nw src/tests/fq_test_1r_nw.cpp)
add_executable(fq_test_mr_mw src/tests/fq_test_mr_mw.cpp)
add_executable(dq_test_1r_1w src/tests/dq_test_1r_1w.cpp)
add_executable(oq_test_1r_1w src/tests/oq_test_1r_1w.cpp)
add_executable(oq_test_nr_1w src/tests/oq_test_nr_1w.cpp)
