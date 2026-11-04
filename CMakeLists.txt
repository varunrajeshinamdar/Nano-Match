cmake_minimum_required(VERSION 3.25)

project(NanoMatch VERSION 0.1.0 DESCRIPTION "Limit order book and matching engine" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NANOMATCH_BUILD_TESTS "Build the test suite" ON)

# Warnings live on an interface target so they apply to project code only, not to dependencies.
add_library(nanomatch_warnings INTERFACE)
target_compile_options(nanomatch_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)

add_library(nanomatch INTERFACE)
target_include_directories(nanomatch INTERFACE include)
target_compile_features(nanomatch INTERFACE cxx_std_20)

if(NANOMATCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
