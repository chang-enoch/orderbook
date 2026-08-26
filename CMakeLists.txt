cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)

# C++20 for <bit>'s countl_zero / countr_zero, used by the bitmap scans.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ENABLE_SANITIZERS "Build with ASan + UBSan" OFF)
option(ENABLE_NATIVE "Tune for the host CPU" ON)

add_library(orderbook STATIC src/orderBook.cpp src/fastOrderBook.cpp)
target_include_directories(orderbook PUBLIC src)
target_compile_options(orderbook PRIVATE -Wall -Wextra)

# Frame pointers stay in even in Release so a profiler can walk the stack.
if(ENABLE_NATIVE)
    target_compile_options(orderbook PUBLIC
        $<$<CONFIG:Release>:-O3 -march=native -fno-omit-frame-pointer>)
else()
    target_compile_options(orderbook PUBLIC
        $<$<CONFIG:Release>:-O3 -fno-omit-frame-pointer>)
endif()

if(ENABLE_SANITIZERS)
    target_compile_options(orderbook PUBLIC -fsanitize=address,undefined -g)
    target_link_options(orderbook PUBLIC -fsanitize=address,undefined)
endif()

add_executable(orderbook_demo src/main.cpp)
target_link_libraries(orderbook_demo PRIVATE orderbook)

add_executable(orderbook_bench bench/benchmark.cpp)
target_include_directories(orderbook_bench PRIVATE bench)
target_compile_definitions(orderbook_bench PRIVATE ORDERBOOK_HAS_FAST)
target_link_libraries(orderbook_bench PRIVATE orderbook)

add_executable(orderbook_test tests/conformance.cpp)
target_include_directories(orderbook_test PRIVATE bench)
target_link_libraries(orderbook_test PRIVATE orderbook)

enable_testing()
add_test(NAME conformance COMMAND orderbook_test 200000)
