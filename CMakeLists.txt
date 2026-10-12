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

if(LOBKIT_BUILD_TESTS)
  enable_testing()
  include(cmake/Dependencies.cmake)
  add_subdirectory(tests)
endif()
