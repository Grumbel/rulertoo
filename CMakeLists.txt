# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

# Source of truth: top-level VERSION (e.g. "0.2.1" or "0.3.0-dev"), or
# -DPROJECT_VERSION_FULL=... from packaging (Nix may append .<revCount>+g<rev>).
if(NOT DEFINED PROJECT_VERSION_FULL)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_FULL)
  string(STRIP "${PROJECT_VERSION_FULL}" PROJECT_VERSION_FULL)
endif()
# project() needs a numeric version; strip -dev / +git / similar suffixes.
string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" PROJECT_VERSION_NUMERIC "${PROJECT_VERSION_FULL}")

project(rulertoo VERSION ${PROJECT_VERSION_NUMERIC} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Nixpkgs / generic builders inject these -D flags; reference them so CMake
# does not warn about unused CLI variables on a CXX-only, no-test project.
set(_rulertoo_cli_ack
  "${BUILD_TESTING}${CMAKE_C_COMPILER}${CMAKE_EXPORT_NO_PACKAGE_REGISTRY}")
unset(_rulertoo_cli_ack)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)

# libX11/xcb Requires.private pull in xcb and xau — make their .pc files
# visible so pkg-config does not emit "Package xcb/xau was not found".
pkg_check_modules(XCB xcb)
pkg_check_modules(XAU xau)
pkg_check_modules(XRANDR REQUIRED xrandr)
pkg_check_modules(XRENDER REQUIRED xrender)

add_executable(rulertoo src/main.cpp)
target_include_directories(rulertoo PRIVATE
  ${X11_INCLUDE_DIR}
  ${XRANDR_INCLUDE_DIRS}
  ${XRENDER_INCLUDE_DIRS}
  ${XCB_INCLUDE_DIRS}
  ${XAU_INCLUDE_DIRS}
)
target_link_libraries(rulertoo PRIVATE
  ${X11_LIBRARIES}
  ${XRANDR_LIBRARIES}
  ${XRENDER_LIBRARIES}
)
target_compile_options(rulertoo PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(rulertoo PRIVATE
  RULERTOO_VERSION="${PROJECT_VERSION_FULL}"
)

install(TARGETS rulertoo
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES README.md LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# Desktop integration
install(FILES data/org.grumbel.rulertoo.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES data/icons/hicolor/scalable/apps/rulertoo.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)
install(FILES data/metainfo/org.grumbel.rulertoo.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
)

install(FILES man/rulertoo.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
