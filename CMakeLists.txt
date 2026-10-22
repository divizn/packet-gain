cmake_minimum_required(VERSION 3.24)
project(pgain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PGAIN_SANITIZE "build with address and undefined behaviour sanitizers" OFF)
if(PGAIN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL 3 REQUIRED)

add_library(pgain_core src/wire.cpp src/dedup.cpp src/auth.cpp src/udp.cpp src/tunnel.cpp)
target_include_directories(pgain_core PUBLIC include)
target_link_libraries(pgain_core PUBLIC OpenSSL::Crypto)
target_compile_options(pgain_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pgain tools/main.cpp)
target_link_libraries(pgain PRIVATE pgain_core)
target_compile_options(pgain PRIVATE -Wall -Wextra -Wpedantic)

add_library(pgain_bench_support bench/impair.cpp)
target_include_directories(pgain_bench_support PUBLIC bench)
target_link_libraries(pgain_bench_support PUBLIC pgain_core)
target_compile_options(pgain_bench_support PRIVATE -Wall -Wextra -Wpedantic)

add_executable(pgain-bench bench/main.cpp)
target_link_libraries(pgain-bench PRIVATE pgain_bench_support)
target_compile_options(pgain-bench PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
