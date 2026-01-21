cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

# ------------------------------------------------------------
# Experimental: import std;
# ------------------------------------------------------------
option(ENABLE_IMPORT_STD "Enable CMake experimental support for `import std;`" ON)

if (ENABLE_IMPORT_STD AND NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "d0edc3af-4c50-42ea-a356-e2862fe7a444"
    CACHE STRING "" FORCE)
endif()

project(CoreEngineModule LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Modules
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
if (ENABLE_IMPORT_STD)
  # Enable `import std;` support for targets created after this line
  set(CMAKE_CXX_MODULE_STD 1)
endif()

option(USE_SUBMODULES "Prefer extern/* submodules if present" ON)
option(WITH_TESTS "Build tests" ON)

# ------------------------------------------------------------
# GLFW
# ------------------------------------------------------------
if (USE_SUBMODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/glfw/CMakeLists.txt")
  add_subdirectory(extern/glfw EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
  )
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
endif()

# Обычно таргет называется glfw
if (TARGET glfw AND NOT TARGET GLFW::glfw)
  add_library(GLFW::glfw ALIAS glfw)
endif()

# ------------------------------------------------------------
# GLM (header-only)
# ------------------------------------------------------------
if (USE_SUBMODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/glm/CMakeLists.txt")
  add_subdirectory(extern/glm EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
  )
  FetchContent_MakeAvailable(glm)
endif()

if (NOT TARGET glm::glm)
  add_library(glm::glm INTERFACE IMPORTED)
  if (DEFINED glm_SOURCE_DIR AND EXISTS "${glm_SOURCE_DIR}/glm/glm.hpp")
    set_target_properties(glm::glm PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${glm_SOURCE_DIR}"
    )
  else()
    message(FATAL_ERROR "glm::glm target not found and glm_SOURCE_DIR is unknown")
  endif()
endif()

# ------------------------------------------------------------
# OpenGL
# ------------------------------------------------------------
find_package(OpenGL REQUIRED)

set(OPENGL_TARGET "")
if (TARGET OpenGL::GL)
  set(OPENGL_TARGET OpenGL::GL)
elseif (TARGET OpenGL::OpenGL)
  set(OPENGL_TARGET OpenGL::OpenGL)
else()
  message(FATAL_ERROR "No suitable OpenGL target found (expected OpenGL::GL or OpenGL::OpenGL)")
endif()

# ------------------------------------------------------------
# GLEW
# ------------------------------------------------------------
set(GLEW_USE_STATIC_LIBS TRUE)
find_package(GLEW QUIET)

set(GLEW_HEADERS_DIR "")

if (GLEW_FOUND AND TARGET GLEW::GLEW)
else()
  FetchContent_Declare(glew_src
    URL https://github.com/nigels-com/glew/releases/download/glew-2.3.0/glew-2.3.0.tgz
    SOURCE_SUBDIR build/cmake
  )
  set(ONLY_LIBS ON  CACHE BOOL "" FORCE)
  set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glew_src)

  # Пробуем разные имена таргетов (на разных сборках GLEW они отличаются)
  if (TARGET glew_s AND NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS glew_s)
  elseif (TARGET glew AND NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS glew)
  elseif (TARGET libglew_static AND NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS libglew_static)
  elseif (TARGET libglew_shared AND NOT TARGET GLEW::GLEW)
    add_library(GLEW::GLEW ALIAS libglew_shared)
  endif()

  if (DEFINED glew_src_SOURCE_DIR AND EXISTS "${glew_src_SOURCE_DIR}/include/GL/glew.h")
    set(GLEW_HEADERS_DIR "${glew_src_SOURCE_DIR}/include")
  endif()
endif()

if (NOT TARGET GLEW::GLEW)
  message(FATAL_ERROR "GLEW target not available. Expected GLEW::GLEW from find_package or from source build.")
endif()

# ------------------------------------------------------------
# STB
# ------------------------------------------------------------
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
)
FetchContent_MakeAvailable(stb)

add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE "${stb_SOURCE_DIR}")

# ------------------------------------------------------------
# CoreEngineModuleLib
# ------------------------------------------------------------
add_library(CoreEngineModuleLib STATIC)
target_compile_features(CoreEngineModuleLib PUBLIC cxx_std_23)

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
        src/Render/ShaderSystem.cppm
        src/Render/Sync.cppm
        src/Render/ShaderFiles.cppm
        src/Render/FileSystem.cppm

        src/Render/OpenGL/OpenGLRHI.cppm
        src/Render/OpenGL/OpenGLCore.cppm
        src/Render/OpenGL/OpenGLRenderer.cppm

        src/Render/Decoders/TextureDecoderSTB.cppm

        src/Assets/ResourceManager.ixx
        src/Assets/ResourceManager_core.cppm
        src/Assets/ResourceManager_texture.cppm
  PRIVATE
    src/Timer/GameTimer_impl.cpp
)

if (GLEW_HEADERS_DIR)
  target_include_directories(CoreEngineModuleLib PUBLIC "${GLEW_HEADERS_DIR}")
endif()

target_link_libraries(CoreEngineModuleLib
  PUBLIC
    GLFW::glfw
    glm::glm
    GLEW::GLEW
    ${OPENGL_TARGET}
    stb_headers
)

target_compile_definitions(CoreEngineModuleLib
  PUBLIC
    $<$<TARGET_EXISTS:glew_s>:GLEW_STATIC>
    $<$<TARGET_EXISTS:libglew_static>:GLEW_STATIC>
)

# ------------------------------------------------------------
# App
# ------------------------------------------------------------
add_executable(app src/main.cpp)
add_custom_command(TARGET app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:app>/assets"
)
target_compile_features(app PRIVATE cxx_std_23)
target_link_libraries(app PRIVATE CoreEngineModuleLib)

# ------------------------------------------------------------
# Tests
# ------------------------------------------------------------
if (WITH_TESTS)
  enable_testing()

  # googletest: submodule или FetchContent
  if (USE_SUBMODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/googletest/CMakeLists.txt")
    add_subdirectory(extern/googletest EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    # MSVC runtime mismatch fix (часто нужно на Windows)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_subdirectory(tests)
endif()
