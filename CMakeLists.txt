cmake_minimum_required(VERSION 3.11)
project(emacs-ghostty-vt C)

set(GHOSTTY_PREFIX "/home/dick/ghostty/zig-out" CACHE PATH "ghostty zig-out prefix")

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${GHOSTTY_PREFIX}/share/pkgconfig")
pkg_check_modules(LIBGHOSTTY_VT REQUIRED IMPORTED_TARGET libghostty-vt)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, defaulting to RelWithDebInfo")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type (default RelWithDebInfo)" FORCE)
endif()

add_library(ghostty-vt-module MODULE ghostty-vt-module.c elisp.c)
set_target_properties(ghostty-vt-module PROPERTIES
  C_STANDARD 99
  COMPILE_WARNING_AS_ERROR ON
  C_VISIBILITY_PRESET "hidden"
  POSITION_INDEPENDENT_CODE ON
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
  BUILD_RPATH "${GHOSTTY_PREFIX}/lib"
  )

target_link_libraries(ghostty-vt-module PRIVATE PkgConfig::LIBGHOSTTY_VT)

add_custom_target(run
  COMMAND emacs -Q -L ${CMAKE_SOURCE_DIR} --eval "(require 'ghostty-vt)" --eval "(ghostty-vt)"
  DEPENDS ghostty-vt-module)
