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

add_library(pgain_core src/wire.cpp src/dedup.cpp)
target_include_directories(pgain_core PUBLIC include)
target_compile_options(pgain_core PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
