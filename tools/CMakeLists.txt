add_library(nanomatch_tool_support INTERFACE)
target_include_directories(nanomatch_tool_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(nm_replay nm_replay.cpp)
target_link_libraries(nm_replay
    PRIVATE nanomatch_io nanomatch_baseline nanomatch_tool_support nanomatch_warnings Threads::Threads)

add_executable(nm_gen nm_gen.cpp)
target_link_libraries(nm_gen
    PRIVATE nanomatch_io nanomatch_baseline nanomatch_tool_support nanomatch_warnings)
