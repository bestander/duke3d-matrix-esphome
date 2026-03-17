cmake_minimum_required(VERSION 3.16)
project(duke3d_matrix_tests)
set(CMAKE_CXX_STANDARD 17)

add_executable(test_hub75 hub75_matrix/test_color.cpp)
target_include_directories(test_hub75 PRIVATE mock)

add_executable(test_sd_card sd_card/test_grp_detection.cpp)

add_executable(test_hud
    hud/test_hud_layout.cpp
    ${CMAKE_SOURCE_DIR}/../components/hud/hud.cpp
)
target_include_directories(test_hud PRIVATE
    mock
    ${CMAKE_SOURCE_DIR}/../components/hud
)
target_compile_definitions(test_hud PRIVATE DUKE3D_HOST_TEST)
