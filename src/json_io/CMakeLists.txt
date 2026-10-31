add_library(json_io INTERFACE)

target_include_directories(json_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(json_io INTERFACE simdjson util pytorch_util)
