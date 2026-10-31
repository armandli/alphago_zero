add_library(models INTERFACE)

target_include_directories(models INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(models INTERFACE json_io)
