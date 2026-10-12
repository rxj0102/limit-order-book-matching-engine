# System packages are preferred. Otherwise fetch at a pinned commit so builds
# are reproducible. Offline builds: install libgtest-dev or pass
# -DLOBKIT_FETCH_DEPS=OFF together with a system GoogleTest.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  if(NOT LOBKIT_FETCH_DEPS)
    message(FATAL_ERROR "GoogleTest not found and LOBKIT_FETCH_DEPS is OFF")
  endif()
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59  # v1.15.2
    GIT_SHALLOW    FALSE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
