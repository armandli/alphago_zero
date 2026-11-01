add_library(muzero INTERFACE)

target_include_directories(muzero INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(muzero INTERFACE util game agents "${TORCH_LIBRARIES}")
