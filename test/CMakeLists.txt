include(GoogleTest)

# The project headers define non-inline free functions, so each test file
# is its own executable (linking several into one would duplicate symbols).
set(ALPHAGO_TESTS
  go_scoring
  move_parsing
  self_play
  zero_mcgs_agent
  train
  json_io
  go_board
)

foreach(name ${ALPHAGO_TESTS})
  add_executable(${name}_test ${name}_test.cpp)
  set_property(TARGET ${name}_test PROPERTY CXX_STANDARD 26)
  # tests rely on asserts (e.g. illegal stone placement) staying enabled
  target_compile_options(${name}_test PRIVATE -UNDEBUG)
  target_link_libraries(${name}_test
    util rgen distributions pytorch_util game encoders
    experience json_io models agents selfplay
    "${TORCH_LIBRARIES}"
    GTest::gtest_main)
  target_compile_definitions(${name}_test PRIVATE
    ALPHAGO_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
  gtest_discover_tests(${name}_test DISCOVERY_TIMEOUT 60)
endforeach()
