add_executable(lobkit_tests
  book/test_primitives.cpp
  book/test_orders.cpp
  book/test_modify.cpp
  book/test_replay_adapter.cpp
  book/test_replay_own_orders.cpp
  book/test_invariants.cpp
  book/test_determinism.cpp
  book/test_differential.cpp
  book/test_names.cpp)
target_include_directories(lobkit_tests PRIVATE support)
target_link_libraries(lobkit_tests PRIVATE lobkit_book lobkit_warnings GTest::gtest_main)

# Replaces global operator new, so it must not share a binary with other tests.
add_executable(lobkit_no_alloc_test book/test_no_alloc.cpp)
target_include_directories(lobkit_no_alloc_test PRIVATE support)
target_link_libraries(lobkit_no_alloc_test PRIVATE lobkit_book lobkit_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lobkit_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(lobkit_no_alloc_test DISCOVERY_MODE PRE_TEST)


# ---- fuzzing -------------------------------------------------------------
# fuzz_engine_regression replays the committed corpus (and any saved crash
# reproducers) through the same entry point as the libFuzzer target, so GCC
# builds and CI exercise it without libFuzzer. The libFuzzer binary is built
# only with -DLOBKIT_BUILD_FUZZ=ON under Clang (see the fuzz preset).
add_executable(fuzz_engine_regression
  ../fuzz/fuzz_engine.cpp ../fuzz/standalone_driver.cpp)
target_include_directories(fuzz_engine_regression PRIVATE support ../fuzz)
target_link_libraries(fuzz_engine_regression PRIVATE lobkit_book lobkit_warnings)
add_test(NAME fuzz_engine_corpus
  COMMAND fuzz_engine_regression
          ${CMAKE_SOURCE_DIR}/fuzz/corpus/engine ${CMAKE_SOURCE_DIR}/fuzz/regressions)
