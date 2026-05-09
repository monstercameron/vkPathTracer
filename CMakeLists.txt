cmake_minimum_required(VERSION 3.25)

project(vkPathTracer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PT_ENABLE_VULKAN "Enable Vulkan renderer support" OFF)
set(VULKAN_SDK "" CACHE PATH "Path to the Vulkan SDK root")
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
option(PT_ENABLE_LUA_VENDOR "Download and build Lua when system Lua 5.4 is unavailable" ON)
option(PT_ENABLE_AUDIO "Enable engine-owned game audio support" OFF)
option(PT_ENABLE_MINIAUDIO "Enable the miniaudio audio backend when PT_ENABLE_AUDIO=ON" ON)
option(PT_ENABLE_EDITOR "Enable editor module" OFF)
option(PT_ENABLE_BENCHMARK "Enable benchmark module" OFF)
option(PT_ENABLE_PROFILING "Enable profiling hooks" OFF)
option(PT_ENABLE_SANITIZERS "Enable sanitizer instrumentation" OFF)
option(PT_STRICT_DETERMINISM "Enable strict deterministic mode" OFF)
option(PT_ENABLE_QT "Enable Qt desktop platform/editor shell" OFF)
option(PT_ENABLE_QT_EDITOR "Enable Qt editor widgets" OFF)
option(PT_ENABLE_QT_RHI "Enable experimental Qt QRhi viewport" OFF)
option(PT_ENABLE_RAW_DESKTOP "Enable raw native desktop platform" ON)
set(PT_WINDEPLOYQT_EXECUTABLE "" CACHE FILEPATH "Path to windeployqt for Windows Qt runtime deployment")
set(PT_JOLT_GIT_TAG "v5.5.0" CACHE STRING "Jolt Physics git tag used when PT_ENABLE_JOLT=ON")
set(PT_MINIAUDIO_GIT_TAG "0.11.25" CACHE STRING "miniaudio git tag used when PT_ENABLE_AUDIO=ON")
set(PT_AUDIO_BACKEND "auto" CACHE STRING "Default audio backend: auto|miniaudio|noop")
set_property(CACHE PT_AUDIO_BACKEND PROPERTY STRINGS auto miniaudio noop)
set(PT_SANITIZER "disabled" CACHE STRING "Sanitizer flavor")
set_property(CACHE PT_SANITIZER PROPERTY STRINGS disabled address thread)

if(PT_ENABLE_AVX OR PT_ENABLE_AVX2 OR PT_ENABLE_AVX512 OR PT_ENABLE_NEON OR PT_ENABLE_SVE)
  set(PT_ENABLE_CPU_SIMD ON)
endif()

if(PT_ENABLE_QT_EDITOR AND NOT PT_ENABLE_QT)
  message(FATAL_ERROR "PT_ENABLE_QT_EDITOR=ON requires PT_ENABLE_QT=ON.")
endif()

if(PT_ENABLE_QT_RHI AND NOT PT_ENABLE_QT)
  message(FATAL_ERROR "PT_ENABLE_QT_RHI=ON requires PT_ENABLE_QT=ON.")
endif()

if(NOT PT_ENABLE_AUDIO)
  set(PT_ENABLE_MINIAUDIO OFF)
endif()

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
  PT_ENABLE_AUDIO
  PT_ENABLE_EDITOR
  PT_ENABLE_BENCHMARK
  PT_ENABLE_PROFILING
  PT_ENABLE_SANITIZERS
  PT_STRICT_DETERMINISM
  PT_ENABLE_QT
  PT_ENABLE_QT_EDITOR
  PT_ENABLE_QT_RHI
  PT_ENABLE_RAW_DESKTOP
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

set(PT_QT_ENABLED_VALUE false)
set(PT_QT_EDITOR_ENABLED_VALUE false)
set(PT_QT_RHI_ENABLED_VALUE false)
set(PT_QT_VERSION "disabled")
set(PT_PLATFORM_SHELLS headless)

if(PT_ENABLE_RAW_DESKTOP)
  list(APPEND PT_PLATFORM_SHELLS raw)
endif()

if(PT_ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
  qt_standard_project_setup()
  set(PT_QT_ENABLED_VALUE true)
  set(PT_QT_VERSION "${Qt6Core_VERSION}")
  if(NOT PT_QT_VERSION)
    set(PT_QT_VERSION "${Qt6_VERSION}")
  endif()
  if(NOT PT_QT_VERSION)
    set(PT_QT_VERSION "unknown")
  endif()
  list(APPEND PT_PLATFORM_SHELLS qt)
  message(STATUS "Qt support enabled: ${PT_QT_VERSION}")
  if(WIN32)
    if(NOT PT_WINDEPLOYQT_EXECUTABLE AND DEFINED WINDEPLOYQT_EXECUTABLE)
      set(PT_WINDEPLOYQT_EXECUTABLE "${WINDEPLOYQT_EXECUTABLE}" CACHE FILEPATH "Path to windeployqt for Windows Qt runtime deployment" FORCE)
    endif()
    if(NOT PT_WINDEPLOYQT_EXECUTABLE)
      set(_pt_qt_bin_dir "")
      if(TARGET Qt6::qmake)
        get_target_property(_pt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
        if(_pt_qmake_executable)
          get_filename_component(_pt_qt_bin_dir "${_pt_qmake_executable}" DIRECTORY)
        endif()
      endif()
      find_program(PT_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt.exe
        HINTS "${_pt_qt_bin_dir}"
        DOC "Path to windeployqt for Windows Qt runtime deployment")
    endif()
    if(PT_WINDEPLOYQT_EXECUTABLE)
      message(STATUS "windeployqt: ${PT_WINDEPLOYQT_EXECUTABLE}")
    else()
      message(WARNING "windeployqt not found; Windows Qt runtime DLLs will not be deployed beside executables.")
    endif()
  endif()
else()
  message(STATUS "Qt support disabled (PT_ENABLE_QT=OFF)")
endif()

function(pt_deploy_qt_runtime target_name)
  if(WIN32 AND PT_ENABLE_QT AND PT_WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND "${PT_WINDEPLOYQT_EXECUTABLE}"
              $<IF:$<CONFIG:Debug>,--debug,--release>
              --no-translations
              --no-compiler-runtime
              --include-plugins qoffscreen
              "$<TARGET_FILE:${target_name}>"
      COMMENT "Deploying Qt runtime for ${target_name}"
      VERBATIM)
  endif()
endfunction()

if(PT_ENABLE_QT_EDITOR)
  set(PT_QT_EDITOR_ENABLED_VALUE true)
endif()

if(PT_ENABLE_QT_RHI)
  set(PT_QT_RHI_ENABLED_VALUE true)
endif()

list(JOIN PT_PLATFORM_SHELLS "," PT_PLATFORM_SHELLS_CSV)

# ---- Jolt Physics -----------------------------------------------------------
set(PT_JOLT_TARGET "")
if(PT_ENABLE_JOLT)
  include(FetchContent)

  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "Disable Jolt warning-as-error policy for vendored builds" FORCE)
  set(ENABLE_INSTALL OFF CACHE BOOL "Disable Jolt install rules for vendored builds" FORCE)
  set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "Preserve parent project compiler flags for Jolt" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Disable Jolt LTO unless the parent enables it explicitly" FORCE)
  set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "Use the same dynamic MSVC runtime as vkPathTracer" FORCE)
  set(CROSS_PLATFORM_DETERMINISTIC ${PT_STRICT_DETERMINISM} CACHE BOOL "Match vkPathTracer strict deterministic physics policy" FORCE)
  set(USE_AVX ${PT_ENABLE_AVX} CACHE BOOL "Match vkPathTracer AVX policy for Jolt" FORCE)
  set(USE_AVX2 ${PT_ENABLE_AVX2} CACHE BOOL "Match vkPathTracer AVX2 policy for Jolt" FORCE)
  set(USE_AVX512 ${PT_ENABLE_AVX512} CACHE BOOL "Match vkPathTracer AVX-512 policy for Jolt" FORCE)

  FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG ${PT_JOLT_GIT_TAG}
    SOURCE_SUBDIR Build
  )
  FetchContent_MakeAvailable(JoltPhysics)

  if(TARGET Jolt)
    set(PT_JOLT_TARGET Jolt)
    message(STATUS "Jolt Physics enabled: ${PT_JOLT_GIT_TAG}")
  else()
    message(FATAL_ERROR "PT_ENABLE_JOLT=ON but vendored Jolt target was not created")
  endif()
endif()

# ---- Lua --------------------------------------------------------------------
set(PT_LUA_TARGET "")
if(PT_ENABLE_LUA)
  find_package(Lua 5.4 QUIET)
  if(TARGET Lua::Lua)
    set(PT_LUA_TARGET Lua::Lua)
  elseif(LUA_FOUND)
    add_library(pt_lua_imported UNKNOWN IMPORTED)
    set_target_properties(pt_lua_imported PROPERTIES
      IMPORTED_LOCATION "${LUA_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
    )
    set(PT_LUA_TARGET pt_lua_imported)
  elseif(PT_ENABLE_LUA_VENDOR)
    include(FetchContent)
    FetchContent_Declare(lua_vendor
      URL https://www.lua.org/ftp/lua-5.4.6.tar.gz
      URL_HASH SHA256=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88
    )
    FetchContent_MakeAvailable(lua_vendor)

    set(_pt_lua_src_dir "${lua_vendor_SOURCE_DIR}/src")
    set(_pt_lua_vendor_sources
      "${_pt_lua_src_dir}/lapi.c"
      "${_pt_lua_src_dir}/lauxlib.c"
      "${_pt_lua_src_dir}/lbaselib.c"
      "${_pt_lua_src_dir}/lcode.c"
      "${_pt_lua_src_dir}/lcorolib.c"
      "${_pt_lua_src_dir}/lctype.c"
      "${_pt_lua_src_dir}/ldblib.c"
      "${_pt_lua_src_dir}/ldebug.c"
      "${_pt_lua_src_dir}/ldo.c"
      "${_pt_lua_src_dir}/ldump.c"
      "${_pt_lua_src_dir}/lfunc.c"
      "${_pt_lua_src_dir}/lgc.c"
      "${_pt_lua_src_dir}/linit.c"
      "${_pt_lua_src_dir}/liolib.c"
      "${_pt_lua_src_dir}/llex.c"
      "${_pt_lua_src_dir}/lmathlib.c"
      "${_pt_lua_src_dir}/lmem.c"
      "${_pt_lua_src_dir}/loadlib.c"
      "${_pt_lua_src_dir}/lobject.c"
      "${_pt_lua_src_dir}/lopcodes.c"
      "${_pt_lua_src_dir}/loslib.c"
      "${_pt_lua_src_dir}/lparser.c"
      "${_pt_lua_src_dir}/lstate.c"
      "${_pt_lua_src_dir}/lstring.c"
      "${_pt_lua_src_dir}/lstrlib.c"
      "${_pt_lua_src_dir}/ltable.c"
      "${_pt_lua_src_dir}/ltablib.c"
      "${_pt_lua_src_dir}/ltm.c"
      "${_pt_lua_src_dir}/lundump.c"
      "${_pt_lua_src_dir}/lutf8lib.c"
      "${_pt_lua_src_dir}/lvm.c"
      "${_pt_lua_src_dir}/lzio.c"
    )
    add_library(pt_lua_vendor STATIC ${_pt_lua_vendor_sources})
    target_include_directories(pt_lua_vendor PUBLIC "${_pt_lua_src_dir}")
    target_compile_definitions(pt_lua_vendor PUBLIC LUA_COMPAT_5_3)
    if(WIN32)
      target_compile_definitions(pt_lua_vendor PRIVATE LUA_USE_WINDOWS)
    endif()
    if(MSVC)
      target_compile_options(pt_lua_vendor PRIVATE /W0)
    else()
      target_compile_options(pt_lua_vendor PRIVATE -w)
    endif()
    set_target_properties(pt_lua_vendor PROPERTIES
      AUTOMOC OFF
      AUTOUIC OFF
      AUTORCC OFF
    )
    set(PT_LUA_TARGET pt_lua_vendor)
    set(LUA_VERSION_STRING "5.4.6 (vendored)")
  else()
    message(FATAL_ERROR "PT_ENABLE_LUA=ON requires Lua 5.4. Install Lua or enable PT_ENABLE_LUA_VENDOR.")
  endif()
  message(STATUS "Lua enabled: ${LUA_VERSION_STRING}")
endif()

# ---- Audio ------------------------------------------------------------------
set(PT_MINIAUDIO_TARGET "")
string(TOLOWER "${PT_AUDIO_BACKEND}" PT_AUDIO_BACKEND_NORMALIZED)
if(NOT PT_AUDIO_BACKEND_NORMALIZED STREQUAL "auto" AND
   NOT PT_AUDIO_BACKEND_NORMALIZED STREQUAL "miniaudio" AND
   NOT PT_AUDIO_BACKEND_NORMALIZED STREQUAL "noop")
  message(FATAL_ERROR "PT_AUDIO_BACKEND must be one of: auto, miniaudio, noop.")
endif()

if(PT_ENABLE_AUDIO)
  if(PT_AUDIO_BACKEND_NORMALIZED STREQUAL "miniaudio" AND NOT PT_ENABLE_MINIAUDIO)
    message(FATAL_ERROR "PT_AUDIO_BACKEND=miniaudio requires PT_ENABLE_MINIAUDIO=ON.")
  endif()

  if(PT_ENABLE_MINIAUDIO AND NOT PT_AUDIO_BACKEND_NORMALIZED STREQUAL "noop")
    include(FetchContent)
    FetchContent_Declare(miniaudio_vendor
      GIT_REPOSITORY https://github.com/mackron/miniaudio.git
      GIT_TAG ${PT_MINIAUDIO_GIT_TAG}
      SOURCE_SUBDIR cmake-unused
    )
    FetchContent_MakeAvailable(miniaudio_vendor)

    add_library(pt_miniaudio STATIC
      "${miniaudio_vendor_SOURCE_DIR}/miniaudio.c"
    )
    target_include_directories(pt_miniaudio PUBLIC
      "${miniaudio_vendor_SOURCE_DIR}"
    )
    if(MSVC)
      target_compile_options(pt_miniaudio PRIVATE /W0)
    else()
      target_compile_options(pt_miniaudio PRIVATE -w)
    endif()
    if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
      target_link_libraries(pt_miniaudio PUBLIC dl pthread m)
    endif()
    set_target_properties(pt_miniaudio PROPERTIES
      AUTOMOC OFF
      AUTOUIC OFF
      AUTORCC OFF
    )
    set(PT_MINIAUDIO_TARGET pt_miniaudio)
  endif()

  message(STATUS "Audio enabled: backend=${PT_AUDIO_BACKEND_NORMALIZED}, miniaudio=${PT_ENABLE_MINIAUDIO}, tag=${PT_MINIAUDIO_GIT_TAG}")
else()
  message(STATUS "Audio disabled")
endif()

# ---- Vulkan SDK --------------------------------------------------------------
if(PT_ENABLE_VULKAN)
  # Allow the user to point at a specific SDK via VULKAN_SDK env variable.
  if(VULKAN_SDK)
    set(Vulkan_SDK_DIR "${VULKAN_SDK}")
    list(APPEND CMAKE_PREFIX_PATH "${Vulkan_SDK_DIR}")
  elseif(DEFINED ENV{VULKAN_SDK})
    set(Vulkan_SDK_DIR "$ENV{VULKAN_SDK}")
    list(APPEND CMAKE_PREFIX_PATH "${Vulkan_SDK_DIR}")
  endif()
  find_package(Vulkan REQUIRED)
  message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}, lib: ${Vulkan_LIBRARIES}")

  # Locate glslc for shader compilation
  find_program(GLSLC glslc
    HINTS "${Vulkan_SDK_DIR}/Bin" "$ENV{VULKAN_SDK}/Bin"
          "${Vulkan_INCLUDE_DIRS}/../Bin"
    REQUIRED)
  message(STATUS "glslc: ${GLSLC}")

  # Compile pathtrace.comp → pathtrace.spv into <binary>/shaders/
  set(PT_SHADER_SRC "${CMAKE_SOURCE_DIR}/src/shaders/gpu/pathtrace.comp")
  set(PT_PUSH_CONSTANT_SCHEMA "${CMAKE_SOURCE_DIR}/src/gpu/PathTracePushConstants.inc")
  set(PT_SHADER_SPV "${CMAKE_BINARY_DIR}/bin/shaders/pathtrace.spv")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/shaders")
  add_custom_command(
    OUTPUT  "${PT_SHADER_SPV}"
    COMMAND "${GLSLC}" -fshader-stage=comp "-I${CMAKE_SOURCE_DIR}/src" "${PT_SHADER_SRC}" -o "${PT_SHADER_SPV}"
    DEPENDS "${PT_SHADER_SRC}" "${PT_PUSH_CONSTANT_SCHEMA}"
    COMMENT "Compiling pathtrace.comp → pathtrace.spv"
    VERBATIM)
  add_custom_target(pt_shaders ALL DEPENDS "${PT_SHADER_SPV}")
endif()

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/BuildInfo.h.in"
  "${CMAKE_BINARY_DIR}/generated/build_info.generated.h"
  @ONLY
)

set(PT_CPU_SOURCES
  # Core/runtime services
  src/core/Assert.cpp
  src/core/Config.cpp
  # Observability stack (Track A, SYSTEM.md Phase 0)
  src/core/Observability.cpp
  src/core/cli/Flags.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/core/metrics/UiInputLatency.cpp
  src/core/pacing/FramePacer.cpp
  src/core/repl/Repl.cpp
  src/core/trace/Trace.cpp
  src/diagnostics/CrashHooks.cpp
  src/diagnostics/CrashRecorder.cpp
  src/diagnostics/StatusFile.cpp
  src/jobs/JobSystem.cpp

  # CPU execution
  src/cpu/BvhTriangleIntersector.cpp
  src/cpu/CpuFeatures.cpp
  src/cpu/ParallelBvhBuilder.cpp
  src/cpu/TiledCpuPathTracer.cpp

  # Editor model data shared by CLI, desktop, and Qt shells
  src/app/DoctorChecks.cpp
  src/app/UiValidation.cpp
  src/app/UiValidationAssets.cpp
  src/app/UiValidationBenchmarkStatus.cpp
  src/app/UiValidationCameraQt.cpp
  src/app/UiValidationReleaseGate.cpp
  src/app/UiValidationSceneTree.cpp
  src/editor/QtPanelModels.cpp
  src/editor/QtPanelModelsSecondary.cpp
  src/editor/UiModels.cpp
  src/editor/UiModelsBenchmark.cpp
  src/editor/UiModelsCommands.cpp
  src/editor/UiModelsEventLog.cpp
  src/editor/UiModelsMenu.cpp
  src/editor/UiModelsPanels.cpp
  src/editor/UiModelsReleaseGates.cpp
  src/editor/UiModelsSerialization.cpp
  src/editor/UiModelsState.cpp

  # Scene, scripting, materials, and physics
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/AnimationSkinning.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/audio/AudioSystem.cpp
  src/materials/MaterialDescriptors.cpp
  src/materials/MaterialInterface.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/scene/FrameLifecycle.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneSnapshot.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SnapshotRing.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/scripting/ScriptRuntime.cpp
  src/scripting/ScriptRuntimeBindings.cpp

  # Path tracer core
  src/pathtracer/CpuBvhAccelerator.cpp
  src/pathtracer/FilmBuffer.cpp
  src/pathtracer/ImageIo.cpp
  src/pathtracer/PathTracer.cpp
  src/pathtracer/ScalarCpuPathTracer.cpp
  src/pathtracer/ScalarCpuPathTracer.Intersect.cpp
  src/pathtracer/ScalarCpuPathTracerJobs.cpp
  src/pathtracer/ScalarCpuPathTracer.Render.cpp
  src/pathtracer/SceneConversion.cpp
  src/pathtracer/SceneLayoutManifest.cpp

  # Platform selection
  src/platform/HeadlessPlatform.cpp
  src/platform/PlatformFactory.cpp

  # Shader and render orchestration
  src/shaders/ShaderFamilyManifest.cpp
  src/shaders/SdfShaderInventory.cpp
  src/render/DebugViews.cpp
  src/render/FrameHandoff.cpp
  src/render/HistoryTransition.cpp
  src/render/MultiGpuAccumulation.cpp
  src/render/RenderCoordinator.cpp
  src/render/TileScheduler.cpp
  src/render/interface/RenderContracts.cpp
  src/render/backends/BackendFactory.cpp
  src/render/backends/D3D12BackendPlanner.cpp
  src/render/backends/D3D12BackendSmoke.cpp
  src/render/backends/FrameGraph.cpp
  src/render/backends/NullBackend.cpp
  src/render/backends/D3D12Backend.cpp
  src/render/backends/VulkanBackend.cpp
)

if(PT_ENABLE_RAW_DESKTOP)
  list(APPEND PT_CPU_SOURCES src/platform/DesktopPlatform.cpp)
endif()

if(PT_ENABLE_AVX2)
  set(PT_ENABLE_CPU_SIMD ON)
  list(APPEND PT_CPU_SOURCES
    src/cpu/Avx2PathTracer.cpp
    src/cpu/BvhTriangleIntersectorAvx2.cpp)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/cpu/Avx2PathTracer.cpp src/cpu/BvhTriangleIntersectorAvx2.cpp src/cpu/CpuFeatures.cpp
      PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
  elseif(MSVC)
    set_source_files_properties(src/cpu/Avx2PathTracer.cpp src/cpu/BvhTriangleIntersectorAvx2.cpp src/cpu/CpuFeatures.cpp
      PROPERTIES COMPILE_FLAGS "/arch:AVX2")
  endif()
endif()

if(PT_ENABLE_VULKAN)
  list(APPEND PT_CPU_SOURCES src/gpu/VulkanGpuPathTracer.cpp)
endif()

if(PT_ENABLE_D3D12)
  list(APPEND PT_CPU_SOURCES
    src/gpu/D3D12GpuPathTracer.Compute.cpp
    src/gpu/D3D12GpuPathTracer.Device.cpp
    src/gpu/D3D12GpuPathTracer.Dxr.cpp
    src/gpu/D3D12GpuPathTracer.DxrPipeline.cpp
    src/gpu/D3D12GpuPathTracer.Film.cpp
    src/gpu/D3D12GpuPathTracer.cpp
    src/gpu/D3D12GpuPathTracer.Scene.cpp
    src/gpu/D3D12GpuPathTracer.SceneBvh.cpp
    src/gpu/D3D12GpuPathTracer.Utils.cpp
  )
endif()

if(PT_ENABLE_QT)
  list(APPEND PT_CPU_SOURCES
    src/app/AppQtSceneDocumentActions.cpp
    src/app/QtDockPanels.cpp
    src/app/QtDockPanelsAssets.cpp
    src/app/QtDockPanelsPlatform.cpp
    src/app/QtDockPanelsRender.cpp
    src/app/QtDockPanelsRayDevices.cpp
    src/app/QtDockPanelsParsing.cpp
    src/app/QtDockPanelsScene.cpp
    src/app/QtDockPanelsShared.cpp
  )
  list(APPEND PT_CPU_SOURCES
    src/app/ViewportInteraction.cpp
    src/app/ViewportMath.cpp
    src/app/ViewportPicking.cpp
    src/app/ViewportFpsPhysics.cpp
    src/app/ViewportOverlay.cpp
    src/app/ViewportGizmo.cpp
    src/app/ViewportPhysicsGate.cpp
  )

  add_library(pt_platform_qt STATIC
    src/platform/qt/QtPlatform.cpp
    src/platform/qt/QtPlatformAdapters.cpp
    src/platform/qt/QtPlatformDockTree.cpp
    src/platform/qt/QtPlatformEvents.cpp
    src/platform/qt/QtPlatformLifecycle.cpp
    src/platform/qt/QtPlatformStyle.cpp
  )

  target_include_directories(pt_platform_qt PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
  )

  target_link_libraries(pt_platform_qt PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
  )

  target_compile_definitions(pt_platform_qt PUBLIC
    VKPT_HAS_QT=1
  )

  if(PT_ENABLE_QT_EDITOR)
    target_compile_definitions(pt_platform_qt PRIVATE
      VKPT_QT_EDITOR=1
    )
  endif()

  target_compile_features(pt_platform_qt PUBLIC cxx_std_23)
endif()

# ---- ptapp ------------------------------------------------------------------
add_executable(ptapp
  src/app/main.cpp
  src/app/AppBenchmarkActions.cpp
  src/app/AppEditorWorldSupport.cpp
  src/app/AppOptions.cpp
  src/app/AppRuntime.cpp
  src/app/AppRuntimeSupport.cpp
  src/cpu/BvhTriangleIntersector.cpp
  src/sim/SimWorker.cpp
  ${PT_CPU_SOURCES}
)

if(WIN32 AND PT_ENABLE_QT)
  # Qt GUI builds should not spawn a separate console window by default.
  # Use ptapp --console or --terminal when interactive stdout/stderr is wanted.
  set_target_properties(ptapp PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(ptapp PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

if(PT_ENABLE_VULKAN)
  target_include_directories(ptapp PRIVATE ${Vulkan_INCLUDE_DIRS})
  target_link_libraries(ptapp PRIVATE ${Vulkan_LIBRARIES})
  add_dependencies(ptapp pt_shaders)
  target_compile_definitions(ptapp PRIVATE
    PT_SHADER_SPV_PATH=\"$<TARGET_FILE_DIR:ptapp>/shaders/pathtrace.spv\")
endif()

if(PT_ENABLE_D3D12)
  target_link_libraries(ptapp PRIVATE d3d12 dxgi d3dcompiler windowscodecs ole32)
  target_compile_definitions(ptapp PRIVATE
    PT_SHADER_HLSL_PATH=\"${CMAKE_SOURCE_DIR}/src/shaders/gpu/pathtrace_cs.hlsl\")
endif()

if(PT_ENABLE_QT)
  target_link_libraries(ptapp PRIVATE pt_platform_qt)
  if(PT_ENABLE_QT_EDITOR)
    target_compile_definitions(ptapp PRIVATE
      VKPT_QT_EDITOR=1
    )
  endif()
endif()

if(PT_ENABLE_JOLT)
  target_link_libraries(ptapp PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(ptapp PRIVATE
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

if(PT_ENABLE_LUA)
  target_link_libraries(ptapp PRIVATE ${PT_LUA_TARGET})
endif()

if(PT_ENABLE_AUDIO)
  target_compile_definitions(ptapp PRIVATE
    PT_AUDIO_BACKEND_STRING="${PT_AUDIO_BACKEND_NORMALIZED}")
  if(PT_MINIAUDIO_TARGET)
    target_link_libraries(ptapp PRIVATE ${PT_MINIAUDIO_TARGET})
    target_compile_definitions(ptapp PRIVATE PT_ENABLE_MINIAUDIO)
  endif()
endif()

foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
  target_compile_definitions(ptapp PRIVATE ${feature})
endforeach()

target_compile_features(ptapp PRIVATE cxx_std_23)

pt_deploy_qt_runtime(ptapp)

# ---- ptbench ----------------------------------------------------------------
add_executable(ptbench
  src/benchmark/ptbench.cpp
  src/benchmark/BenchmarkCapture.cpp
  src/benchmark/BenchmarkCli.cpp
  src/benchmark/BenchmarkExperiments.cpp
  src/benchmark/BenchmarkImages.cpp
  src/benchmark/BenchmarkInspectionCommands.cpp
  src/benchmark/BenchmarkRunArtifacts.cpp
  src/benchmark/BenchmarkRunOptions.cpp
  src/benchmark/BenchmarkRuntime.cpp
  src/benchmark/BenchmarkRuntimeCommands.cpp
  src/benchmark/BenchmarkSweeps.cpp
  src/benchmark/BenchmarkTraceProfiler.cpp
  src/benchmark/BenchmarkSchema.cpp
  src/benchmark/BenchmarkSchemaCapabilities.cpp
  src/cpu/BvhTriangleIntersector.cpp
  ${PT_CPU_SOURCES}
)

target_include_directories(ptbench PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

if(PT_ENABLE_VULKAN)
  target_include_directories(ptbench PRIVATE ${Vulkan_INCLUDE_DIRS})
  target_link_libraries(ptbench PRIVATE ${Vulkan_LIBRARIES})
  add_dependencies(ptbench pt_shaders)
  target_compile_definitions(ptbench PRIVATE
    PT_SHADER_SPV_PATH=\"$<TARGET_FILE_DIR:ptbench>/shaders/pathtrace.spv\")
endif()

if(PT_ENABLE_D3D12)
  target_link_libraries(ptbench PRIVATE d3d12 dxgi d3dcompiler dxguid)
  set(PT_WINPIX_EVENT_RUNTIME_INCLUDE_DIR "" CACHE PATH
    "Optional path containing WinPixEventRuntime pix3.h")
  set(_pt_winpix_event_runtime_include "")
  foreach(_pt_winpix_candidate
      "${PT_WINPIX_EVENT_RUNTIME_INCLUDE_DIR}"
      "${CMAKE_SOURCE_DIR}/artifacts/pix_probe/winpixeventruntime/pkg/Include/WinPixEventRuntime")
    if(EXISTS "${_pt_winpix_candidate}/pix3.h")
      set(_pt_winpix_event_runtime_include "${_pt_winpix_candidate}")
      break()
    endif()
  endforeach()
  if(_pt_winpix_event_runtime_include)
    target_include_directories(ptbench PRIVATE "${_pt_winpix_event_runtime_include}")
    target_compile_definitions(ptbench PRIVATE PT_HAS_WINPIX_EVENT_RUNTIME=1 USE_PIX=1)
  endif()
  target_compile_definitions(ptbench PRIVATE
    PT_SHADER_HLSL_PATH=\"${CMAKE_SOURCE_DIR}/src/shaders/gpu/pathtrace_cs.hlsl\")
endif()

if(PT_ENABLE_QT)
  target_link_libraries(ptbench PRIVATE pt_platform_qt)
endif()

if(PT_ENABLE_JOLT)
  target_link_libraries(ptbench PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(ptbench PRIVATE
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

if(PT_ENABLE_LUA)
  target_link_libraries(ptbench PRIVATE ${PT_LUA_TARGET})
endif()

if(PT_ENABLE_AUDIO)
  if(PT_MINIAUDIO_TARGET)
    target_link_libraries(ptbench PRIVATE ${PT_MINIAUDIO_TARGET})
    target_compile_definitions(ptbench PRIVATE PT_ENABLE_MINIAUDIO)
  endif()
endif()

foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
  target_compile_definitions(ptbench PRIVATE ${feature})
endforeach()

target_compile_features(ptbench PRIVATE cxx_std_23)

pt_deploy_qt_runtime(ptbench)

# ---- ptdoctor ---------------------------------------------------------------
add_executable(ptdoctor
  src/app/ptdoctor.cpp
)

target_compile_features(ptdoctor PRIVATE cxx_std_23)

# ---- focused smoke targets ---------------------------------------------------
# Track A — observability stack regression test (SYSTEM.md Phase 0.6).
# Self-contained: only depends on the new src/core/{sync,log,metrics,trace,
# health,cli,repl}/ modules and the Observability bootstrap.
add_executable(pt_observability_smoke EXCLUDE_FROM_ALL
  tests/observability_smoke.cpp
  src/core/Observability.cpp
  src/core/cli/Flags.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/core/metrics/UiInputLatency.cpp
  src/core/repl/Repl.cpp
  src/core/trace/Trace.cpp
  src/diagnostics/CrashRecorder.cpp
  src/scene/Json.cpp
)

target_include_directories(pt_observability_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

target_compile_features(pt_observability_smoke PRIVATE cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(pt_observability_smoke PRIVATE Threads::Threads)

# Frame pacing primitive smoke (SYSTEM.md acceptance gate :1729 / :1736).
add_executable(pt_frame_pacer_smoke EXCLUDE_FROM_ALL
  tests/frame_pacer_smoke.cpp
  src/core/pacing/FramePacer.cpp
)

target_include_directories(pt_frame_pacer_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_frame_pacer_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_frame_pacer_smoke PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(pt_frame_pacer_smoke PRIVATE winmm)
endif()

# Standalone diagnostic that exercises SleepUntilHighResolution against the
# naive sleep_for path so the dev can see whether HIGH_RESOLUTION_TIMER is
# actually being granted on this host.
add_executable(pt_frame_pacer_diag EXCLUDE_FROM_ALL
  tests/frame_pacer_diag.cpp
  src/core/pacing/FramePacer.cpp
)
target_include_directories(pt_frame_pacer_diag PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)
target_compile_features(pt_frame_pacer_diag PRIVATE cxx_std_23)
target_link_libraries(pt_frame_pacer_diag PRIVATE Threads::Threads)

add_executable(pt_job_health_smoke EXCLUDE_FROM_ALL
  tests/job_health_smoke.cpp
  src/core/log/Log.cpp
  src/core/health/Health.cpp
  src/core/metrics/Metrics.cpp
  src/jobs/JobSystem.cpp
)

target_include_directories(pt_job_health_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_job_health_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_job_health_smoke PRIVATE Threads::Threads)

add_executable(pt_determinism_propagation_smoke EXCLUDE_FROM_ALL
  tests/determinism_propagation_smoke.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/jobs/JobSystem.cpp
)

target_include_directories(pt_determinism_propagation_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_determinism_propagation_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_determinism_propagation_smoke PRIVATE Threads::Threads)

add_executable(pt_platform_contract_smoke EXCLUDE_FROM_ALL
  tests/platform_contract_smoke.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/platform/HeadlessPlatform.cpp
  src/platform/PlatformFactory.cpp
)

target_include_directories(pt_platform_contract_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_platform_contract_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_platform_contract_smoke PRIVATE Threads::Threads)

add_executable(pt_vulkan_backend_snapshot_smoke EXCLUDE_FROM_ALL
  tests/vulkan_backend_snapshot_smoke.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/core/metrics/UiInputLatency.cpp
  src/render/backends/FrameGraph.cpp
  src/render/backends/VulkanBackend.cpp
  src/render/interface/RenderContracts.cpp
  src/scene/SnapshotRing.cpp
)

target_include_directories(pt_vulkan_backend_snapshot_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_vulkan_backend_snapshot_smoke PRIVATE cxx_std_23)

if(PT_ENABLE_VULKAN)
  add_executable(pt_vulkan_gpu_submission_contract EXCLUDE_FROM_ALL
    tests/vulkan_gpu_path_tracer_submission_contract.cpp
    src/gpu/VulkanGpuPathTracer.cpp
    src/pathtracer/FilmBuffer.cpp
    src/pathtracer/PathTracer.cpp
  )

  target_include_directories(pt_vulkan_gpu_submission_contract PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    ${Vulkan_INCLUDE_DIRS}
  )

  target_link_libraries(pt_vulkan_gpu_submission_contract PRIVATE
    ${Vulkan_LIBRARIES}
  )

  target_compile_definitions(pt_vulkan_gpu_submission_contract PRIVATE
    PT_ENABLE_VULKAN
  )

  target_compile_features(pt_vulkan_gpu_submission_contract PRIVATE cxx_std_23)
endif()

add_executable(pt_scripting_smoke EXCLUDE_FROM_ALL
  tests/scripting_runtime_smoke.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/audio/AudioSystem.cpp
  src/cpu/BvhTriangleIntersector.cpp
  src/cpu/CpuFeatures.cpp
  src/cpu/ParallelBvhBuilder.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/core/metrics/UiInputLatency.cpp
  src/jobs/JobSystem.cpp
  src/pathtracer/CpuBvhAccelerator.cpp
  src/pathtracer/SceneConversion.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneSnapshot.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/SnapshotRing.cpp
  src/scene/WorldCommandBuffer.cpp
  src/scripting/ScriptRuntime.cpp
  src/scripting/ScriptRuntimeBindings.cpp
)

target_include_directories(pt_scripting_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_scripting_smoke PRIVATE cxx_std_23)

if(PT_ENABLE_LUA)
  target_link_libraries(pt_scripting_smoke PRIVATE ${PT_LUA_TARGET})
  target_compile_definitions(pt_scripting_smoke PRIVATE PT_ENABLE_LUA)
endif()

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_scripting_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_scripting_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

if(PT_ENABLE_AUDIO)
  if(PT_MINIAUDIO_TARGET)
    target_link_libraries(pt_scripting_smoke PRIVATE ${PT_MINIAUDIO_TARGET})
    target_compile_definitions(pt_scripting_smoke PRIVATE PT_ENABLE_MINIAUDIO)
  endif()
  target_compile_definitions(pt_scripting_smoke PRIVATE PT_ENABLE_AUDIO)
endif()

add_executable(pt_physics_contract_smoke EXCLUDE_FROM_ALL
  tests/physics_contract_smoke.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
)

target_include_directories(pt_physics_contract_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_physics_contract_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_physics_contract_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_physics_contract_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_physics_contract_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

add_executable(pt_script_dispatch_contract_smoke EXCLUDE_FROM_ALL
  tests/script_dispatch_contract_smoke.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/jobs/JobSystem.cpp
  src/scripting/ScriptRuntime.cpp
  src/scripting/ScriptRuntimeBindings.cpp
)

target_include_directories(pt_script_dispatch_contract_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_script_dispatch_contract_smoke PRIVATE cxx_std_23)

if(PT_ENABLE_LUA)
  target_link_libraries(pt_script_dispatch_contract_smoke PRIVATE ${PT_LUA_TARGET})
  target_compile_definitions(pt_script_dispatch_contract_smoke PRIVATE PT_ENABLE_LUA)
endif()

add_executable(pt_editor_override_tracker_smoke EXCLUDE_FROM_ALL
  tests/editor_override_tracker_smoke.cpp
)

target_include_directories(pt_editor_override_tracker_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_editor_override_tracker_smoke PRIVATE cxx_std_23)

foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
  target_compile_definitions(pt_editor_override_tracker_smoke PRIVATE ${feature})
endforeach()

# SimWorker channel/lifecycle smoke (Step 1 of the SimWorker rollout).
add_executable(pt_sim_worker_smoke EXCLUDE_FROM_ALL
  tests/sim_worker_smoke.cpp
  src/sim/SimWorker.cpp
)

target_include_directories(pt_sim_worker_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_sim_worker_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_sim_worker_smoke PRIVATE Threads::Threads)

add_executable(pt_scene_contract_smoke EXCLUDE_FROM_ALL
  tests/scene_contract_smoke.cpp
  src/core/log/Log.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
)

target_include_directories(pt_scene_contract_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_scene_contract_smoke PRIVATE cxx_std_23)

add_executable(pt_skeleton_import_smoke EXCLUDE_FROM_ALL
  tests/skeleton_import_smoke.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/pathtracer/SceneConversion.cpp
)

target_include_directories(pt_skeleton_import_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_skeleton_import_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_skeleton_import_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_skeleton_import_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_skeleton_import_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

# Phase 2 RAG01-04: ragdoll bridge smoke. Mirrors pt_skeleton_import_smoke's
# source list (asset loader pulls in glTF + Skeleton) and adds the Ragdoll
# bridge plus Jolt link. The smoke gracefully no-ops with a SKIP message
# when built without PT_ENABLE_JOLT.
add_executable(pt_ragdoll_smoke EXCLUDE_FROM_ALL
  tests/ragdoll_smoke.cpp
  src/animation/AnimationAuthority.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/pathtracer/SceneConversion.cpp
)

target_include_directories(pt_ragdoll_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_ragdoll_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_ragdoll_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_ragdoll_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_ragdoll_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

# Phase 3 ANI02/03/06: animation clip + sampler smoke. Mirrors the asset
# loader source list of pt_skeleton_import_smoke and pt_ragdoll_smoke (the
# glTF loader pulls clips out of the same asset path) and adds the new
# animation translation units.
add_executable(pt_animation_sampler_smoke EXCLUDE_FROM_ALL
  tests/animation_sampler_smoke.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/pathtracer/SceneConversion.cpp
)

target_include_directories(pt_animation_sampler_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_animation_sampler_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_animation_sampler_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_animation_sampler_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_animation_sampler_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

# Phase 4 SKN01-03 / ANI04-05: skinning pixel-hash smoke. Mirrors the asset
# loader source list of pt_animation_sampler_smoke and adds the new
# AnimationSkinning translation unit. Validates per-vertex JOINTS_0/WEIGHTS_0
# import + bind-pose skinning matrix identity property + deterministic
# hash over the skinned vertex stream.
add_executable(pt_skinning_pixel_hash_smoke EXCLUDE_FROM_ALL
  tests/skinning_pixel_hash_smoke.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/AnimationSkinning.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/pathtracer/SceneConversion.cpp
)

target_include_directories(pt_skinning_pixel_hash_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_skinning_pixel_hash_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_skinning_pixel_hash_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_skinning_pixel_hash_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_skinning_pixel_hash_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

# Phase 5 Part B: hero hit demo smoke. Drives the full
# script -> animation -> ragdoll authority arbitration loop end-to-end against
# a synthesized SceneWorld. Validates hero_hit_demo.json parses, the script
# fires enable_ragdoll() near t=2s, the ragdoll seeds from the current
# animation pose, takes the requested impulse, and lands cleanly on the
# floor (no NaNs).
add_executable(pt_hero_hit_demo_smoke EXCLUDE_FROM_ALL
  tests/hero_hit_demo_smoke.cpp
  src/animation/AnimationAuthority.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/jobs/JobSystem.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/pathtracer/SceneConversion.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
  src/scripting/ScriptRuntime.cpp
  src/scripting/ScriptRuntimeBindings.cpp
)

target_include_directories(pt_hero_hit_demo_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_hero_hit_demo_smoke PRIVATE cxx_std_23)
target_link_libraries(pt_hero_hit_demo_smoke PRIVATE Threads::Threads)

if(PT_ENABLE_LUA)
  target_link_libraries(pt_hero_hit_demo_smoke PRIVATE ${PT_LUA_TARGET})
  target_compile_definitions(pt_hero_hit_demo_smoke PRIVATE PT_ENABLE_LUA)
endif()

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_hero_hit_demo_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_hero_hit_demo_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

add_executable(pt_scene_script_bootstrap_smoke EXCLUDE_FROM_ALL
  tests/scene_script_bootstrap_smoke.cpp
  src/assets/AssetImporters.cpp
  src/assets/AssetImportersTexture.cpp
  src/assets/SceneAssetLoader.cpp
  src/assets/SceneAssetLoaderSupport.cpp
  src/assets/SceneAssetObjLoader.cpp
  src/assets/SceneAssetGltfLoader.cpp
  src/core/health/Health.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/pathtracer/SceneConversion.cpp
  src/animation/AnimationClip.cpp
  src/animation/AnimationSampler.cpp
  src/animation/Skeleton.cpp
  src/physics/PhysicsWorld.cpp
  src/physics/Ragdoll.cpp
  src/scene/Json.cpp
  src/scene/Scene.cpp
  src/scene/SceneDocumentExport.cpp
  src/scene/SceneDocumentLoad.cpp
  src/scene/SceneScriptBootstrap.cpp
  src/scene/SceneWorld.cpp
  src/scene/SceneWorldExtraction.cpp
  src/scene/WorldCommandBuffer.cpp
)

target_include_directories(pt_scene_script_bootstrap_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_scene_script_bootstrap_smoke PRIVATE cxx_std_23)

if(PT_ENABLE_JOLT)
  target_link_libraries(pt_scene_script_bootstrap_smoke PRIVATE ${PT_JOLT_TARGET})
  target_compile_definitions(pt_scene_script_bootstrap_smoke PRIVATE
    PT_ENABLE_JOLT
    PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
endif()

add_executable(pt_snapshot_bus_smoke EXCLUDE_FROM_ALL
  tests/snapshot_bus_smoke.cpp
  src/cpu/BvhTriangleIntersector.cpp
  src/cpu/CpuFeatures.cpp
  src/cpu/ParallelBvhBuilder.cpp
  src/cpu/TiledCpuPathTracer.cpp
  src/core/log/Log.cpp
  src/core/metrics/Metrics.cpp
  src/core/metrics/UiInputLatency.cpp
  src/diagnostics/CrashRecorder.cpp
  src/jobs/JobSystem.cpp
  src/pathtracer/CpuBvhAccelerator.cpp
  src/pathtracer/FilmBuffer.cpp
  src/pathtracer/PathTracer.cpp
  src/pathtracer/ScalarCpuPathTracer.cpp
  src/pathtracer/ScalarCpuPathTracer.Intersect.cpp
  src/pathtracer/ScalarCpuPathTracerJobs.cpp
  src/pathtracer/ScalarCpuPathTracer.Render.cpp
  src/render/FrameHandoff.cpp
  src/render/HistoryTransition.cpp
  src/render/RenderCoordinator.cpp
  src/render/TileScheduler.cpp
  src/render/backends/FrameGraph.cpp
  src/scene/SceneSnapshot.cpp
  src/scene/SnapshotRing.cpp
)

target_include_directories(pt_snapshot_bus_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

target_compile_features(pt_snapshot_bus_smoke PRIVATE cxx_std_23)

add_executable(pt_multi_gpu_accumulation_smoke EXCLUDE_FROM_ALL
  tests/multi_gpu_accumulation_smoke.cpp
  src/pathtracer/FilmBuffer.cpp
  src/render/FrameHandoff.cpp
  src/render/MultiGpuAccumulation.cpp
  src/render/TileScheduler.cpp
)

target_include_directories(pt_multi_gpu_accumulation_smoke PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_features(pt_multi_gpu_accumulation_smoke PRIVATE cxx_std_23)

# Drag-throughput smoke for the D3D12 path tracer. Built only when D3D12 is
# enabled because the test is a thin wrapper around D3D12GpuPathTracer; the
# bug it measures only manifests on that backend in the editor.
if(PT_ENABLE_D3D12)
  add_executable(pt_drag_throughput_smoke EXCLUDE_FROM_ALL
    tests/transform_drag_throughput.cpp
    ${PT_CPU_SOURCES}
  )
  target_include_directories(pt_drag_throughput_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
  )
  target_compile_features(pt_drag_throughput_smoke PRIVATE cxx_std_23)
  target_link_libraries(pt_drag_throughput_smoke PRIVATE d3d12 dxgi d3dcompiler dxguid)
  target_compile_definitions(pt_drag_throughput_smoke PRIVATE
    PT_SHADER_HLSL_PATH=\"${CMAKE_SOURCE_DIR}/src/shaders/gpu/pathtrace_cs.hlsl\")
  if(PT_ENABLE_QT)
    target_link_libraries(pt_drag_throughput_smoke PRIVATE pt_platform_qt)
    if(PT_ENABLE_QT_EDITOR)
      target_compile_definitions(pt_drag_throughput_smoke PRIVATE
        VKPT_QT_EDITOR=1
      )
    endif()
  endif()
  if(PT_ENABLE_VULKAN)
    target_include_directories(pt_drag_throughput_smoke PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(pt_drag_throughput_smoke PRIVATE ${Vulkan_LIBRARIES})
  endif()
  if(PT_ENABLE_JOLT)
    target_link_libraries(pt_drag_throughput_smoke PRIVATE ${PT_JOLT_TARGET})
    target_compile_definitions(pt_drag_throughput_smoke PRIVATE
      PT_ENABLE_JOLT
      PT_JOLT_GIT_TAG_STRING="${PT_JOLT_GIT_TAG}")
  endif()
  if(PT_ENABLE_LUA)
    target_link_libraries(pt_drag_throughput_smoke PRIVATE ${PT_LUA_TARGET})
    target_compile_definitions(pt_drag_throughput_smoke PRIVATE PT_ENABLE_LUA)
  endif()
  if(PT_ENABLE_AUDIO)
    if(PT_MINIAUDIO_TARGET)
      target_link_libraries(pt_drag_throughput_smoke PRIVATE ${PT_MINIAUDIO_TARGET})
      target_compile_definitions(pt_drag_throughput_smoke PRIVATE PT_ENABLE_MINIAUDIO)
    endif()
    target_compile_definitions(pt_drag_throughput_smoke PRIVATE PT_ENABLE_AUDIO)
  endif()
  foreach(feature IN LISTS PT_ACTIVE_FEATURE_FLAGS)
    target_compile_definitions(pt_drag_throughput_smoke PRIVATE ${feature})
  endforeach()
endif()

add_custom_target(hello
  DEPENDS ptapp
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ptapp
  COMMENT "Build hello executable (ptapp)"
)
