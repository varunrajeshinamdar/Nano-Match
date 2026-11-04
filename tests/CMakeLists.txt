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
    book_invariants_test.cpp
    book_test.cpp
    log_io_test.cpp
    protocol_test.cpp
    types_test.cpp
)
target_link_libraries(nanomatch_tests
    PRIVATE nanomatch_baseline nanomatch_io nanomatch_warnings GTest::gtest_main)

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
