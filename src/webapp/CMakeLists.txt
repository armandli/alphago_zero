#Zero9 Small TUI
add_executable(zero_9small_tui zero_9small_tui.cpp)

target_link_libraries(zero_9small_tui "${TORCH_LIBRARIES}")
set_property(TARGET zero_9small_tui PROPERTY CXX_STANDARD 26)

if (MSVC)
  file(GLOB TORCH_DLLS "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  add_custom_command(TARGET zero_9small_tui
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${TORCH_DLLS}
    $<TARGET_FILE_DIR:zero_9small_tui>)
endif(MSVC)

target_link_libraries(zero_9small_tui rapidjson)
target_link_libraries(zero_9small_tui util)
target_link_libraries(zero_9small_tui rgen)
target_link_libraries(zero_9small_tui distributions)
target_link_libraries(zero_9small_tui pytorch_util)
target_link_libraries(zero_9small_tui game)
target_link_libraries(zero_9small_tui encoders)
target_link_libraries(zero_9small_tui experience)
target_link_libraries(zero_9small_tui models)
target_link_libraries(zero_9small_tui agents)
target_link_libraries(zero_9small_tui CLI11::CLI11)
target_link_libraries(zero_9small_tui ftxui::screen)
target_link_libraries(zero_9small_tui ftxui::dom)
target_link_libraries(zero_9small_tui ftxui::component)
