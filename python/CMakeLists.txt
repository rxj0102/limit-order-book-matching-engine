# Python extension lobkit._core. Only configured when LOBKIT_BUILD_PYTHON is ON, so the C++
# core never depends on Python.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

# pybind11: use an installed copy if there is one (pip install pybind11 provides a CMake
# directory), otherwise fetch a pinned release when LOBKIT_FETCH_DEPS is ON.
execute_process(COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
                OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_pybind11_dir)
  list(APPEND CMAKE_PREFIX_PATH "${_pybind11_dir}")
endif()
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
  if(NOT LOBKIT_FETCH_DEPS)
    message(FATAL_ERROR "pybind11 not found and LOBKIT_FETCH_DEPS is OFF")
  endif()
  include(FetchContent)
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG a2e59f0e7065404b44dfe92a28aca47ba1378dc4  # v2.13.6
  )
  FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(lobkit_core src/bindings.cpp)
set_target_properties(lobkit_core PROPERTIES
  OUTPUT_NAME _core
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python/lobkit)
target_link_libraries(lobkit_core PRIVATE lobkit_sim)
# Warnings are on for our code; pybind11 headers trip -Wold-style-cast and -Wconversion in macros,
# so the bindings use a smaller set and -Werror stays on for it.
target_compile_options(lobkit_core PRIVATE -Wall -Wextra -Wshadow)
if(LOBKIT_WERROR)
  target_compile_options(lobkit_core PRIVATE -Werror)
endif()

# Copy the pure Python package next to the compiled module so that
# PYTHONPATH=<build>/python imports lobkit.
file(GLOB _lobkit_py CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lobkit/*.py)
add_custom_target(lobkit_py_package ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/python/lobkit
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_lobkit_py} ${CMAKE_BINARY_DIR}/python/lobkit)
add_dependencies(lobkit_py_package lobkit_core)

if(LOBKIT_BUILD_TESTS)
  add_test(NAME python_pytest
    COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(python_pytest PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python;LOBKIT_SIM_RUN=$<TARGET_FILE:lobkit_sim_run>")
endif()
