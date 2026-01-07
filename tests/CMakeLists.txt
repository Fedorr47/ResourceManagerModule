add_executable(TextureManagerTests
  test_texture_basic.cpp
)

target_link_libraries(TextureManagerTests
  PRIVATE
    TextureManagerLib
    GTest::gtest_main
)

target_compile_features(TextureManagerTests PRIVATE cxx_std_23)
set_property(TARGET TextureManagerTests PROPERTY CXX_SCAN_FOR_MODULES ON)

include(GoogleTest)
gtest_discover_tests(TextureManagerTests)