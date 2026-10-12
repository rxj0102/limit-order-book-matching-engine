# libFuzzer targets. The instrumentation flags apply to this directory only, so
# the rest of the tree keeps its normal flags.
set(FUZZ_FLAGS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined
               -fno-omit-frame-pointer -g)

add_executable(fuzz_engine fuzz_engine.cpp)
target_include_directories(fuzz_engine PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_options(fuzz_engine PRIVATE ${FUZZ_FLAGS})
target_link_libraries(fuzz_engine PRIVATE lobkit_book lobkit_warnings)
target_link_options(fuzz_engine PRIVATE -fsanitize=fuzzer,address,undefined)

foreach(f lobster itch)
  add_executable(fuzz_${f} fuzz_${f}.cpp)
  target_compile_options(fuzz_${f} PRIVATE ${FUZZ_FLAGS})
  target_link_libraries(fuzz_${f} PRIVATE lobkit_md lobkit_warnings)
  target_link_options(fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
