add_executable(CoreEngineModuleTests
  "unit/InputTests/TestInputCore.cpp"
  "unit/InputTests/TestControllerBase.cpp"
  "unit/InputTests/TestCameraController.cpp"
 "unit/Math/TestMathUtils.cpp")

target_link_libraries(CoreEngineModuleTests
  PRIVATE
    CoreEngineModuleLib
    GTest::gtest_main
)

target_compile_features(CoreEngineModuleTests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(CoreEngineModuleTests)
