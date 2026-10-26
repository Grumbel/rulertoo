# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(rulertoo VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XRANDR REQUIRED xrandr)
pkg_check_modules(XEXT REQUIRED xext)
pkg_check_modules(XRENDER REQUIRED xrender)

add_executable(rulertoo src/main.cpp)
target_include_directories(rulertoo PRIVATE
  ${X11_INCLUDE_DIR}
  ${XRANDR_INCLUDE_DIRS}
  ${XEXT_INCLUDE_DIRS}
  ${XRENDER_INCLUDE_DIRS}
)
target_link_libraries(rulertoo PRIVATE
  ${X11_LIBRARIES}
  ${XRANDR_LIBRARIES}
  ${XEXT_LIBRARIES}
  ${XRENDER_LIBRARIES}
)
target_compile_options(rulertoo PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS rulertoo RUNTIME DESTINATION bin)
