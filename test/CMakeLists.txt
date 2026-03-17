cmake_minimum_required(VERSION 3.16)
project(duke3d_matrix_tests)
set(CMAKE_CXX_STANDARD 17)

add_executable(test_hub75 hub75_matrix/test_color.cpp)
target_include_directories(test_hub75 PRIVATE mock)
