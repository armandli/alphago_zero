#Zero9 Small
add_executable(zero_9small_selfplay zero_9small_selfplay.cpp)

target_link_libraries(zero_9small_selfplay "${TORCH_LIBRARIES}")
set_property(TARGET zero_9small_selfplay PROPERTY CXX_STANDARD 26)

if (MSVC)
  file(GLOB TORCH_DLLS "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  add_custom_command(TARGET zero_9small_selfplay
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${TORCH_DLLS}
    $<TARGET_FILE_DIR:zero_9small_selfplay>)
endif(MSVC)

target_link_libraries(zero_9small_selfplay rapidjson)
target_link_libraries(zero_9small_selfplay util)
target_link_libraries(zero_9small_selfplay rgen)
target_link_libraries(zero_9small_selfplay distributions)
target_link_libraries(zero_9small_selfplay pytorch_util)
target_link_libraries(zero_9small_selfplay game)
target_link_libraries(zero_9small_selfplay encoders)
target_link_libraries(zero_9small_selfplay experience)
target_link_libraries(zero_9small_selfplay models)
target_link_libraries(zero_9small_selfplay agents)
target_link_libraries(zero_9small_selfplay CLI11::CLI11)

#vs_zero_9small
add_executable(vs_zero_9small vs_zero_9small.cpp)

target_link_libraries(vs_zero_9small "${TORCH_LIBRARIES}")
set_property(TARGET vs_zero_9small PROPERTY CXX_STANDARD 26)

if (MSVC)
  file(GLOB TORCH_DLLS "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  add_custom_command(TARGET vs_zero_9small
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${TORCH_DLLS}
    $<TARGET_FILE_DIR:vs_zero_9small>)
endif(MSVC)

target_link_libraries(vs_zero_9small rapidjson)
target_link_libraries(vs_zero_9small util)
target_link_libraries(vs_zero_9small rgen)
target_link_libraries(vs_zero_9small distributions)
target_link_libraries(vs_zero_9small pytorch_util)
target_link_libraries(vs_zero_9small game)
target_link_libraries(vs_zero_9small encoders)
target_link_libraries(vs_zero_9small experience)
target_link_libraries(vs_zero_9small models)
target_link_libraries(vs_zero_9small agents)
target_link_libraries(vs_zero_9small CLI11::CLI11)

#Zero13 Small
add_executable(zero_13small_selfplay zero_9small_selfplay.cpp)

target_link_libraries(zero_13small_selfplay "${TORCH_LIBRARIES}")
set_property(TARGET zero_13small_selfplay PROPERTY CXX_STANDARD 26)

if (MSVC)
  file(GLOB TORCH_DLLS "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  add_custom_command(TARGET zero_13small_selfplay
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${TORCH_DLLS}
    $<TARGET_FILE_DIR:zero_13small_selfplay>)
endif(MSVC)

target_link_libraries(zero_13small_selfplay rapidjson)
target_link_libraries(zero_13small_selfplay util)
target_link_libraries(zero_13small_selfplay rgen)
target_link_libraries(zero_13small_selfplay distributions)
target_link_libraries(zero_13small_selfplay pytorch_util)
target_link_libraries(zero_13small_selfplay game)
target_link_libraries(zero_13small_selfplay encoders)
target_link_libraries(zero_13small_selfplay experience)
target_link_libraries(zero_13small_selfplay models)
target_link_libraries(zero_13small_selfplay agents)
target_link_libraries(zero_13small_selfplay CLI11::CLI11)

#vs_zero_13small
add_executable(vs_zero_13small vs_zero_9small.cpp)

target_link_libraries(vs_zero_13small "${TORCH_LIBRARIES}")
set_property(TARGET vs_zero_13small PROPERTY CXX_STANDARD 26)

if (MSVC)
  file(GLOB TORCH_DLLS "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  add_custom_command(TARGET vs_zero_13small
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${TORCH_DLLS}
    $<TARGET_FILE_DIR:vs_zero_13small>)
endif(MSVC)

target_link_libraries(vs_zero_13small rapidjson)
target_link_libraries(vs_zero_13small util)
target_link_libraries(vs_zero_13small rgen)
target_link_libraries(vs_zero_13small distributions)
target_link_libraries(vs_zero_13small pytorch_util)
target_link_libraries(vs_zero_13small game)
target_link_libraries(vs_zero_13small encoders)
target_link_libraries(vs_zero_13small experience)
target_link_libraries(vs_zero_13small models)
target_link_libraries(vs_zero_13small agents)
target_link_libraries(vs_zero_13small CLI11::CLI11)
