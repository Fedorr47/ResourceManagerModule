add_executable(CoreEngineTests
  "TestTextureStorage.cpp")

target_link_libraries(CoreEngineTests
  PRIVATE
    CoreEngineModuleLib
    GTest::gtest_main
)

target_compile_features(CoreEngineTests PRIVATE cxx_std_23)
set_property(TARGET CoreEngineTests PROPERTY CXX_SCAN_FOR_MODULES ON)

include(GoogleTest)
gtest_discover_tests(CoreEngineTests)