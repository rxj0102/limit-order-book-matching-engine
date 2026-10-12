add_executable(lobkit_tests
  book/test_primitives.cpp
  book/test_orders.cpp
  book/test_modify.cpp
  book/test_replay_adapter.cpp
  book/test_replay_own_orders.cpp
  book/test_invariants.cpp
  book/test_determinism.cpp)
target_include_directories(lobkit_tests PRIVATE support)
target_link_libraries(lobkit_tests PRIVATE lobkit_book lobkit_warnings GTest::gtest_main)

# Replaces global operator new, so it must not share a binary with other tests.
add_executable(lobkit_no_alloc_test book/test_no_alloc.cpp)
target_include_directories(lobkit_no_alloc_test PRIVATE support)
target_link_libraries(lobkit_no_alloc_test PRIVATE lobkit_book lobkit_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lobkit_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(lobkit_no_alloc_test DISCOVERY_MODE PRE_TEST)

