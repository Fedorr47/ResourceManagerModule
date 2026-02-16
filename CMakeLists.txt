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

# Modules dyn-deps (needed for correct build order with Ninja/MSVC)
set(CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP 1)

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
# --- Backend switch (cache) ---
# ------------------------------------------------------------
set(CORE_RENDER_BACKEND "GL" CACHE STRING "Render backend: GL or DX12")
set_property(CACHE CORE_RENDER_BACKEND PROPERTY STRINGS "GL" "DX12")

if (CORE_RENDER_BACKEND STREQUAL "DX12" AND NOT WIN32)
  message(FATAL_ERROR "DX12 backend is Windows-only")
endif()

if (CORE_RENDER_BACKEND STREQUAL "GL")
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

if (TARGET glfw AND NOT TARGET GLFW::glfw)
  add_library(GLFW::glfw ALIAS glfw)
endif()


endif()

if (CORE_RENDER_BACKEND STREQUAL "GL")
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


endif()

# ------------------------------------------------------------
# OpenGL (only needed for GL backend)
# ------------------------------------------------------------
set(OPENGL_TARGET "")
if (CORE_RENDER_BACKEND STREQUAL "GL")
  find_package(OpenGL REQUIRED)
  if (TARGET OpenGL::GL)
    set(OPENGL_TARGET OpenGL::GL)
  elseif (TARGET OpenGL::OpenGL)
    set(OPENGL_TARGET OpenGL::OpenGL)
  else()
    message(FATAL_ERROR "No suitable OpenGL target found (expected OpenGL::GL or OpenGL::OpenGL)")
  endif()
endif()

if (CORE_RENDER_BACKEND STREQUAL "GL")
# ------------------------------------------------------------
# GLEW (build/find ALWAYS, link only for GL)
# ------------------------------------------------------------
set(GLEW_USE_STATIC_LIBS TRUE)
find_package(GLEW QUIET)

set(GLEW_HEADERS_DIR "")

if (GLEW_FOUND AND TARGET GLEW::GLEW)
  # ok
else()
  FetchContent_Declare(glew_src
    URL https://github.com/nigels-com/glew/releases/download/glew-2.3.0/glew-2.3.0.tgz
    SOURCE_SUBDIR build/cmake
  )
  set(ONLY_LIBS ON  CACHE BOOL "" FORCE)
  set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glew_src)

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

# Если ты в GL бекенде — GLEW обязан быть.
if (CORE_RENDER_BACKEND STREQUAL "GL" AND NOT TARGET GLEW::GLEW)
  message(FATAL_ERROR "GLEW target not available for GL backend.")
endif()


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
# ImGui (DX12 + Win32 only)
# ------------------------------------------------------------
if (WIN32 AND CORE_RENDER_BACKEND STREQUAL "DX12")
  set(IMGUI_DIR "")

  if (USE_SUBMODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui.cpp")
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui")
  else()
    FetchContent_Declare(imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG        v1.91.0
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
  endif()

  add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
  )

  target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
  )

  # Also allow imgui.cpp to see a project-level imconfig.h (if you have one in the repo root),
  # so that both the library and the app compile with the same config and struct layouts.
  target_include_directories(imgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

  # Make sure the application picks the same ImGui headers that the imgui target was built with.
  # Otherwise IMGUI_CHECKVERSION() can assert on mismatched struct layout.
  set(IMGUI_INCLUDE_DIRS "${IMGUI_DIR}" "${IMGUI_DIR}/backends")

  # IMPORTANT: Do NOT set IMGUI_DISABLE_OBSOLETE_FUNCTIONS only for the imgui target.
  # That macro can change public struct layouts (ImGuiIO, etc.) and will trip IMGUI_CHECKVERSION()
  # if the application includes imgui.h without the same macro.
  # Keep defaults. If you ever need it, set it PUBLIC so it propagates to app.

  # Backend needs D3D12/DXGI. (dxguid for IID definitions)
  target_link_libraries(imgui PUBLIC d3d12 dxgi dxguid)
endif()

# ------------------------------------------------------------
# CoreEngineModuleLib
# ------------------------------------------------------------
add_library(CoreEngineModuleLib STATIC)
target_compile_features(CoreEngineModuleLib PUBLIC cxx_std_23)

target_compile_definitions(CoreEngineModuleLib
  PUBLIC
    $<$<STREQUAL:${CORE_RENDER_BACKEND},DX12>:CORE_USE_DX12=1>
    $<$<STREQUAL:${CORE_RENDER_BACKEND},GL>:CORE_USE_GL=1>
)

# If you vendor d3dx12.h in extern/, allow includes like "d3dx12.h" or "extern/d3dx12.h"
target_include_directories(CoreEngineModuleLib
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

set(COMMON_MODULES
  Core/Core.ixx

  Core/Math/MathUtils.cppm

  Input/Input.cppm
  Input/InputCore.cppm
  Input/ControllerBase.cppm
  Input/Win32Input.cppm

  Timer/GameTimer.ixx
  Timer/GameTimer.cppm

  Render/Render.ixx
  Render/RenderCore.cppm
  Render/RHI.cppm
  Render/RenderGraph.cppm
  Render/Debug/DebugDraw.cppm
  Render/Bindless.cppm
  Render/GpuMemory.cppm
  Render/RendererSettings.cppm
  Render/Renderer.cppm
  
  Render/Sync.cppm
  Render/FileSystem.cppm

  Render/Shader/ShaderFiles.cppm
  Render/Shader/ShaderSystem.cppm

  Render/Scene/SceneBridge.cppm
  Render/Scene/Scene.cppm
  Render/Scene/Level.cppm

  Render/Scene/Picking.cppm

  Render/Scene/CameraController.cppm

  Render/Model/ObjLoader.cppm
  Render/Model/Mesh/Mesh.cppm

  Render/Decoders/TextureDecoderSTB.cppm

  Assets/ResourceManager.ixx
  Assets/ResourceManager_core.cppm
  Assets/ResourceManager_texture.cppm
  Assets/ResourceManager_mesh.cppm
  Assets/AssetManager.cppm
)

set(GL_MODULES
  Render/OpenGL/RenderGL.ixx
  Render/OpenGL/OpenGLCore.cppm
  Render/OpenGL/OpenGLRHI.cppm
  Render/OpenGL/OpenGLRenderer.cppm
  Render/OpenGL/OpenGLRenderer.cppm
)

set(DX12_MODULES
  Render/DirectX12/RenderDX12.ixx
  Render/DirectX12/DirectX12Core.cppm
  Render/DirectX12/DirectX12RHI.cppm
  Render/DirectX12/DirectX12Renderer.cppm
  Render/DirectX12/DebugDrawRendererDX12.cppm
  Render/DirectX12/DirectX12TextureUploader.cppm
  Render/ImGui/ImGuiDebugUI.cppm
)

set(CORE_MODULES ${COMMON_MODULES})
if (CORE_RENDER_BACKEND STREQUAL "DX12")
  list(APPEND CORE_MODULES ${DX12_MODULES})
else()
  list(APPEND CORE_MODULES ${GL_MODULES})
endif()
list(REMOVE_DUPLICATES CORE_MODULES)

set(CORE_MODULES_ABS "")
foreach(f IN LISTS CORE_MODULES)
  list(APPEND CORE_MODULES_ABS "${SRC_DIR}/${f}")
endforeach()

target_sources(CoreEngineModuleLib
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES
      FILES ${CORE_MODULES_ABS}
)

# Common deps
target_link_libraries(CoreEngineModuleLib
  PUBLIC
    stb_headers
)

# GL-only deps (link AFTER CoreEngineModuleLib exists)
if (CORE_RENDER_BACKEND STREQUAL "GL")
  if (GLEW_HEADERS_DIR)
    target_include_directories(CoreEngineModuleLib PUBLIC "${GLEW_HEADERS_DIR}")
  endif()

  target_link_libraries(CoreEngineModuleLib PUBLIC GLFW::glfw glm::glm GLEW::GLEW ${OPENGL_TARGET})

  target_compile_definitions(CoreEngineModuleLib
    PUBLIC
      $<$<TARGET_EXISTS:glew_s>:GLEW_STATIC>
      $<$<TARGET_EXISTS:libglew_static>:GLEW_STATIC>
  )
endif()

# DX12-only deps
if (WIN32 AND CORE_RENDER_BACKEND STREQUAL "DX12")
  target_include_directories(CoreEngineModuleLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/extern")
  target_link_libraries(CoreEngineModuleLib PUBLIC d3d12 dxgi dxguid)
  if (TARGET imgui)
    target_link_libraries(CoreEngineModuleLib PUBLIC imgui)
  endif()
endif()

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

if (CORE_RENDER_BACKEND STREQUAL "DX12")
  # Put ImGui include dirs BEFORE project/extern include dirs to avoid picking up a stray imgui.h from elsewhere.
  target_include_directories(app SYSTEM BEFORE PRIVATE ${IMGUI_INCLUDE_DIRS})
endif()

if (WIN32)
  find_library(D3DCOMPILER_LIB NAMES d3dcompiler_47 d3dcompiler REQUIRED)
  target_link_libraries(CoreEngineModuleLib PRIVATE ${D3DCOMPILER_LIB})
endif()

# ------------------------------------------------------------
# Tests
# ------------------------------------------------------------
if (WITH_TESTS)
  enable_testing()

  if (USE_SUBMODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/googletest/CMakeLists.txt")
    add_subdirectory(extern/googletest EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_subdirectory(tests)
endif()
