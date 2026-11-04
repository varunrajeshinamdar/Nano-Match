add_library(nanomatch_bench_support INTERFACE)
target_include_directories(nanomatch_bench_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_book bench_book.cpp)
target_link_libraries(bench_book
    PRIVATE nanomatch_io nanomatch_baseline nanomatch_bench_support nanomatch_tool_support
            nanomatch_warnings)
