cmake_minimum_required(VERSION 3.25)
project(protopt LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "protopt is intentionally Windows-only: this experiment targets Win32 + Direct3D 12.")
endif()

add_executable(protopt WIN32 main.cpp)
target_compile_features(protopt PRIVATE cxx_std_23)
target_compile_definitions(protopt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(protopt PRIVATE d3d12 dxgi d3dcompiler user32 kernel32)

if(MSVC)
  target_compile_options(protopt PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(protopt PRIVATE -Wall -Wextra -Wpedantic)
endif()
