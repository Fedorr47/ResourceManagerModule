cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

# Gate MUST be set before project()/enable_language(CXX)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
  "d0edc3af-4c50-42ea-a356-e2862fe7a444")

project(CoreEngineModule LANGUAGES CXX)
message(STATUS "CMAKE_CXX_COMPILER_IMPORT_STD='${CMAKE_CXX_COMPILER_IMPORT_STD}'")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tell CMake we want `import std;` (initializes target property CXX_MODULE_STD)
set(CMAKE_CXX_MODULE_STD 1)

# Optional: verify toolchain actually supports it
if (NOT "23" IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
  message(WARNING "Toolchain doesn't report C++23 `import std;` support. Falling back to #include may be needed.")
endif()

option(WITH_TESTS "Build tests" ON)

if (WITH_TESTS)
  enable_testing() 
  add_subdirectory(extern/googletest)
  add_subdirectory(tests)
endif()

add_library(CoreEngineModuleLib STATIC)

target_sources(CoreEngineModuleLib
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES
      BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
      FILES
        src/Core.ixx
        src/ResourceManager.ixx
        src/ResourceManager_core.cppm
        src/ResourceManager_texture.cppm
)

set_property(TARGET CoreEngineModuleLib PROPERTY CXX_SCAN_FOR_MODULES ON)
target_compile_features(CoreEngineModuleLib PUBLIC cxx_std_23)

add_executable(app src/main.cpp)
set_property(TARGET app PROPERTY CXX_SCAN_FOR_MODULES ON)
target_compile_features(app PRIVATE cxx_std_23)
target_link_libraries(app PRIVATE CoreEngineModuleLib)

target_link_libraries(app PRIVATE CoreEngineModuleLib)