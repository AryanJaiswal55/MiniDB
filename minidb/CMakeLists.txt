cmake_minimum_required(VERSION 3.16)
project(minidb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# e.g. -DMINIDB_SANITIZE=address,undefined
set(MINIDB_SANITIZE "" CACHE STRING "Sanitizers to enable")

add_library(minidb_lib src/pager.cpp src/buffer_pool.cpp src/bplustree.cpp)
target_include_directories(minidb_lib PUBLIC include)
target_compile_options(minidb_lib PUBLIC -Wall -Wextra -Wpedantic)
if(MINIDB_SANITIZE)
  target_compile_options(minidb_lib PUBLIC -fsanitize=${MINIDB_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(minidb_lib PUBLIC -fsanitize=${MINIDB_SANITIZE})
endif()

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_lib)

add_executable(minidb_tests tests/test_main.cpp)
target_link_libraries(minidb_tests PRIVATE minidb_lib)

add_executable(minidb_bench bench/bench.cpp)
target_link_libraries(minidb_bench PRIVATE minidb_lib)

enable_testing()
add_test(NAME unit COMMAND minidb_tests)
