cmake_minimum_required(VERSION 3.25)
project(lobkit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOBKIT_BUILD_TESTS "Build tests" ON)
option(LOBKIT_WERROR "Treat warnings in project targets as errors" ON)
option(LOBKIT_BUILD_BENCH "Build benchmarks (fetches Google Benchmark if needed)" OFF)
set(LOBKIT_CACHE_LINE 128 CACHE STRING "Padding distance in bytes for cross-thread data (see docs/INFRA.md)")
option(LOBKIT_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(LOBKIT_FETCH_DEPS "Fetch missing dependencies with FetchContent (pinned SHAs)" ON)

# Warnings are attached to project targets only, so third party code fetched
# below is never compiled with -Werror.
add_library(lobkit_warnings INTERFACE)
target_compile_options(lobkit_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
  -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion)
# GCC reports -Wnull-dereference false positives inside gtest and in tests that
# dereference find_order() right after asserting non-null, so this one is only
# enabled for library code.
if(LOBKIT_WERROR)
  target_compile_options(lobkit_warnings INTERFACE -Werror)
endif()

add_library(lobkit_book STATIC src/book/names.cpp)
target_include_directories(lobkit_book PUBLIC include)
target_compile_features(lobkit_book PUBLIC cxx_std_20)
target_link_libraries(lobkit_book PRIVATE lobkit_warnings)
target_compile_options(lobkit_book PRIVATE -Wnull-dereference)

find_package(Threads REQUIRED)

# Market data parsers and the replay harness.
add_library(lobkit_md STATIC src/md/mapped_file.cpp src/md/names.cpp)
target_include_directories(lobkit_md PUBLIC include)
target_compile_features(lobkit_md PUBLIC cxx_std_20)
target_link_libraries(lobkit_md PUBLIC lobkit_book PRIVATE lobkit_warnings)

add_executable(lobkit_replay tools/lobkit_replay.cpp)
target_link_libraries(lobkit_replay PRIVATE lobkit_md lobkit_warnings)

# Header-only lock-free infrastructure (SPSC ring). No dependencies.
add_library(lobkit_infra INTERFACE)
target_include_directories(lobkit_infra INTERFACE include)
target_compile_features(lobkit_infra INTERFACE cxx_std_20)
target_compile_definitions(lobkit_infra INTERFACE LOBKIT_CACHE_LINE=${LOBKIT_CACHE_LINE})
target_link_libraries(lobkit_infra INTERFACE Threads::Threads)

if(LOBKIT_BUILD_TESTS)
  enable_testing()
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
endif()

if(LOBKIT_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(LOBKIT_BUILD_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LOBKIT_BUILD_FUZZ requires Clang (libFuzzer)")
  endif()
  add_subdirectory(fuzz)
endif()
