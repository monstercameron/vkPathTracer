cmake_minimum_required(VERSION 3.25)

project(vkPathTracer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PT_ENABLE_VULKAN "Enable Vulkan renderer support" OFF)
option(PT_ENABLE_D3D12 "Enable D3D12 renderer support" OFF)
option(PT_ENABLE_METAL "Enable Metal renderer support" OFF)
option(PT_ENABLE_WEBGPU "Enable WebGPU renderer support" OFF)
option(PT_ENABLE_OPENGL_EXPERIMENTAL "Enable experimental OpenGL compute fallback" OFF)
option(PT_ENABLE_CPU_RAYTRACER "Enable CPU ray tracer" OFF)
option(PT_ENABLE_CPU_SIMD "Enable CPU SIMD dispatch path" OFF)
option(PT_ENABLE_AVX "Enable AVX codepath" OFF)
option(PT_ENABLE_AVX2 "Enable AVX2 codepath" OFF)
option(PT_ENABLE_AVX512 "Enable AVX-512 codepath" OFF)
option(PT_ENABLE_NEON "Enable ARM NEON codepath" OFF)
option(PT_ENABLE_SVE "Enable ARM SVE codepath" OFF)
option(PT_ENABLE_JOLT "Enable Jolt integration" OFF)
option(PT_ENABLE_LUA "Enable Lua integration" OFF)
option(PT_ENABLE_EDITOR "Enable editor module" OFF)
option(PT_ENABLE_BENCHMARK "Enable benchmark module" OFF)
option(PT_ENABLE_PROFILING "Enable profiling hooks" OFF)
option(PT_ENABLE_SANITIZERS "Enable sanitizer instrumentation" OFF)
option(PT_STRICT_DETERMINISM "Enable strict deterministic mode" OFF)
set(PT_SANITIZER "disabled" CACHE STRING "Sanitizer flavor")
set_property(CACHE PT_SANITIZER PROPERTY STRINGS disabled address thread)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(_pt_all_features
  PT_ENABLE_VULKAN
  PT_ENABLE_D3D12
  PT_ENABLE_METAL
  PT_ENABLE_WEBGPU
  PT_ENABLE_OPENGL_EXPERIMENTAL
  PT_ENABLE_CPU_RAYTRACER
  PT_ENABLE_CPU_SIMD
  PT_ENABLE_AVX
  PT_ENABLE_AVX2
  PT_ENABLE_AVX512
  PT_ENABLE_NEON
  PT_ENABLE_SVE
  PT_ENABLE_JOLT
  PT_ENABLE_LUA
  PT_ENABLE_EDITOR
  PT_ENABLE_BENCHMARK
  PT_ENABLE_PROFILING
  PT_ENABLE_SANITIZERS
  PT_STRICT_DETERMINISM
)

set(PT_ACTIVE_FEATURE_FLAGS "")
set(PT_DISABLED_FEATURE_FLAGS "")
foreach(feature IN LISTS _pt_all_features)
  if(${feature})
    list(APPEND PT_ACTIVE_FEATURE_FLAGS "${feature}")
  else()
    list(APPEND PT_DISABLED_FEATURE_FLAGS "${feature}")
  endif()
endforeach()

list(JOIN PT_ACTIVE_FEATURE_FLAGS "," PT_ACTIVE_FEATURE_FLAGS_CSV)
list(JOIN PT_DISABLED_FEATURE_FLAGS "," PT_DISABLED_FEATURE_FLAGS_CSV)

if(PT_ACTIVE_FEATURE_FLAGS_CSV STREQUAL "")
  set(PT_ACTIVE_FEATURE_FLAGS_CSV "(none)")
endif()
if(PT_DISABLED_FEATURE_FLAGS_CSV STREQUAL "")
  set(PT_DISABLED_FEATURE_FLAGS_CSV "(none)")
endif()

if(PT_ENABLE_SANITIZERS)
  set(PT_ENABLE_SANITIZERS_VALUE true)
else()
  set(PT_ENABLE_SANITIZERS_VALUE false)
endif()
if(PT_STRICT_DETERMINISM)
  set(PT_STRICT_DETERMINISM_VALUE true)
else()
  set(PT_STRICT_DETERMINISM_VALUE false)
endif()

set(PT_SIMD_COMPILE_OPTIONS "${CMAKE_CXX_FLAGS}")
set(PT_BACKEND_COMPILE_OPTIONS "")
if(PT_ENABLE_SANITIZERS AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
  if(PT_SANITIZER STREQUAL "address")
    set(PT_SIMD_COMPILE_OPTIONS "${CMAKE_CXX_FLAGS} -fsanitize=address")
    set(PT_BACKEND_COMPILE_OPTIONS "-fsanitize=address")
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
  elseif(PT_SANITIZER STREQUAL "thread")
    set(PT_SIMD_COMPILE_OPTIONS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    set(PT_BACKEND_COMPILE_OPTIONS "-fsanitize=thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  endif()
endif()

find_program(_pt_git NAMES git)
if(_pt_git)
  execute_process(
    COMMAND ${_pt_git} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PT_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(NOT PT_GIT_HASH)
  set(PT_GIT_HASH "unknown")
endif()

string(TIMESTAMP PT_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)

if(CMAKE_SYSTEM_NAME)
  set(PT_TARGET_OS "${CMAKE_SYSTEM_NAME}")
else()
  set(PT_TARGET_OS "unknown")
endif()

if(CMAKE_SYSTEM_PROCESSOR)
  set(PT_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
else()
  set(PT_TARGET_ARCH "unknown")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(PT_BUILD_TYPE "unknown")
else()
  set(PT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/BuildInfo.h.in"
  "${CMAKE_BINARY_DIR}/generated/build_info.generated.h"
  @ONLY
)

add_executable(ptapp
  src/app/main.cpp
  src/platform/HeadlessPlatform.cpp
  src/scene/Scene.cpp
  src/pathtracer/PathTracer.cpp
  src/render/interface/RenderContracts.cpp
  src/render/backends/BackendFactory.cpp
  src/render/backends/FrameGraph.cpp
  src/render/backends/NullBackend.cpp
  src/render/backends/VulkanBackend.cpp
)

target_include_directories(ptapp PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
  target_compile_definitions(ptapp PRIVATE ${feature})
endforeach()

target_compile_features(ptapp PRIVATE cxx_std_23)

add_executable(ptbench
  src/benchmark/ptbench.cpp
  src/benchmark/BenchmarkSchema.cpp
  src/pathtracer/PathTracer.cpp
  src/scene/Scene.cpp
  src/render/interface/RenderContracts.cpp
  src/render/backends/BackendFactory.cpp
  src/render/backends/FrameGraph.cpp
  src/render/backends/NullBackend.cpp
  src/render/backends/VulkanBackend.cpp
)

target_include_directories(ptbench PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
  target_compile_definitions(ptbench PRIVATE ${feature})
endforeach()

target_compile_features(ptbench PRIVATE cxx_std_23)

add_custom_target(hello
  DEPENDS ptapp
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ptapp
  COMMENT "Build hello executable (ptapp)"
)
