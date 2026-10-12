find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  if(NOT LOBKIT_FETCH_DEPS)
    message(FATAL_ERROR "Google Benchmark not found and LOBKIT_FETCH_DEPS is OFF")
  endif()
  include(FetchContent)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        c58e6d0710581e3a08d65c349664128a8d9a2461  # v1.9.1
    GIT_SHALLOW    FALSE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

foreach(t bench_queues bench_queue_latency bench_false_sharing bench_interleaved)
  add_executable(${t} ${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${t} PRIVATE lobkit_infra lobkit_warnings Threads::Threads)
endforeach()
target_link_libraries(bench_queues PRIVATE benchmark::benchmark)

string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
# Book engine benchmarks and the std::map baseline. Include tests/support for the shared workload generator.
add_executable(bench_book bench_book.cpp)
target_include_directories(bench_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/support)
target_link_libraries(bench_book PRIVATE lobkit_md lobkit_warnings benchmark::benchmark Threads::Threads)
target_compile_definitions(bench_book PRIVATE LOBKIT_BENCH_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER}} build=${CMAKE_BUILD_TYPE} ${LOBKIT_BENCH_EXTRA_FLAGS}")
