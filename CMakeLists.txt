cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

# Gate MUST be set before project()/enable_language(CXX)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
  "d0edc3af-4c50-42ea-a356-e2862fe7a444")

project(CoreEngineModule LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable `import std;` support for targets created after this line
set(CMAKE_CXX_MODULE_STD 1) # initializes target property CXX_MODULE_STD :contentReference[oaicite:3]{index=3}

option(WITH_TESTS "Build tests" ON)

add_library(CoreEngineModuleLib STATIC)

target_compile_features(CoreEngineModuleLib PUBLIC cxx_std_23)

set_property(TARGET CoreEngineModuleLib PROPERTY CXX_SCAN_FOR_MODULES ON) # :contentReference[oaicite:4]{index=4}

target_sources(CoreEngineModuleLib
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES
      BASE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
      FILES
        src/Core.ixx

        src/Timer/GameTimer.ixx
        src/Timer/GameTimer.cppm

        src/Render/Render.ixx
        src/Render/RenderCore.cppm
        src/Render/RHI.cppm
        src/Render/RenderGraph.cppm
        src/Render/Bindless.cppm
        src/Render/GpuMemory.cppm
        src/Render/Renderer.cppm
        src/Render/SceneBridge.cppm

        src/Assets/ResourceManager.ixx
        src/Assets/ResourceManager_core.cppm
        src/Assets/ResourceManager_texture.cppm

  PRIVATE
    src/Timer/GameTimer_impl.cpp
)

add_executable(app
  src/main.cpp
)

target_compile_features(app PRIVATE cxx_std_23)
set_property(TARGET app PROPERTY CXX_SCAN_FOR_MODULES ON) # :contentReference[oaicite:5]{index=5}

target_link_libraries(app PRIVATE CoreEngineModuleLib)

if (WITH_TESTS)
  enable_testing()
  add_subdirectory(extern/googletest)
  add_subdirectory(tests)
endif()
