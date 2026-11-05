include(FetchContent)
include(GoogleTest)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SYSTEM
)
FetchContent_MakeAvailable(googletest)

add_executable(nanomatch_tests
    bench_support_test.cpp
    book_invariants_test.cpp
    book_test.cpp
    differential_test.cpp
    fast_components_test.cpp
    log_io_test.cpp
    protocol_test.cpp
    types_test.cpp
    workload_test.cpp
)
target_link_libraries(nanomatch_tests
    PRIVATE nanomatch_baseline nanomatch_bench_support nanomatch_io nanomatch_tool_support
            nanomatch_warnings GTest::gtest_main)

gtest_discover_tests(nanomatch_tests)

# The expected output comes from tests/data/make_spec_example.py, an independent encoding of the worked
# example in the specification, so this checks encoding, matching and hashing end to end.
add_test(NAME replay_spec_example
    COMMAND nm_replay --in ${CMAKE_CURRENT_SOURCE_DIR}/data/spec_example.nmc
            --min-price 1 --max-price 1000 --capacity 16)
set_tests_properties(replay_spec_example PROPERTIES
    PASS_REGULAR_EXPRESSION "commands=2 events=5 hash=0x189e422178bdb0f6")

add_test(NAME replay_missing_file
    COMMAND nm_replay --in does_not_exist.nmc --min-price 1 --max-price 1000 --capacity 16)
set_tests_properties(replay_missing_file PROPERTIES WILL_FAIL TRUE)

add_test(NAME replay_bad_usage COMMAND nm_replay --in)
set_tests_properties(replay_bad_usage PROPERTIES WILL_FAIL TRUE)

# The tools working together on a small generated log. The tests check that the pipeline runs, and the
# values in the bench output are meaningless at this size.
add_test(NAME pipeline_generate
    COMMAND nm_gen --profile realistic --seed 1 --commands 20000 --out pipeline.nmc)
set_tests_properties(pipeline_generate PROPERTIES FIXTURES_SETUP pipeline_log)

add_test(NAME pipeline_replay
    COMMAND nm_replay --in pipeline.nmc --min-price 1 --max-price 1048576 --capacity 20000)
set_tests_properties(pipeline_replay PROPERTIES
    FIXTURES_REQUIRED pipeline_log PASS_REGULAR_EXPRESSION "commands=20000 events=[0-9]+ hash=0x")

foreach(mode untimed timed)
    add_test(NAME pipeline_bench_${mode}
        COMMAND bench_book --log pipeline.nmc --profile realistic --book baseline
                --mode ${mode} --warmup 5000 --cpu none)
    set_tests_properties(pipeline_bench_${mode} PROPERTIES
        FIXTURES_REQUIRED pipeline_log PASS_REGULAR_EXPRESSION "\"mode\": \"${mode}\"")
endforeach()

add_test(NAME workload_unknown_profile
    COMMAND nm_gen --profile nonsense --seed 1 --commands 1 --out unused.nmc)
set_tests_properties(workload_unknown_profile PROPERTIES WILL_FAIL TRUE)
