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
    protocol_test.cpp
    types_test.cpp
)
target_link_libraries(nanomatch_tests PRIVATE nanomatch nanomatch_warnings GTest::gtest_main)

gtest_discover_tests(nanomatch_tests)
