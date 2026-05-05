cmake_minimum_required(VERSION 3.20)

project(pathtracer_optimization_graph LANGUAGES CXX)

add_executable(ptopt_math_microbench
  math_microbench.cpp
)

target_compile_features(ptopt_math_microbench PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(ptopt_math_microbench PRIVATE /O2 /permissive-)
else()
  target_compile_options(ptopt_math_microbench PRIVATE -O3 -march=native)
endif()

