cmake_minimum_required(VERSION 3.11)
include(ExternalProject)

project(emacs-ghostty-vt C)

set(GHOSTTY_SRC "${CMAKE_SOURCE_DIR}/vendor/ghostty")
set(GHOSTTY_OUT "${GHOSTTY_SRC}/zig-out")

find_program(ZIG zig REQUIRED)

ExternalProject_Add(libghostty-vt
  SOURCE_DIR "${GHOSTTY_SRC}"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ${ZIG} build lib-vt
  BUILD_IN_SOURCE ON
  INSTALL_COMMAND "")

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
  BUILD_RPATH "${GHOSTTY_OUT}/lib"
  )

add_dependencies(ghostty-vt-module libghostty-vt)
target_include_directories(ghostty-vt-module PRIVATE "${GHOSTTY_SRC}/include")
target_link_libraries(ghostty-vt-module PRIVATE "${GHOSTTY_OUT}/lib/libghostty-vt.so")


add_custom_target(run
  COMMAND emacs -Q -L ${CMAKE_SOURCE_DIR} --eval "(require 'ghostty-vt)" --eval "(ghostty-vt)"
  DEPENDS ghostty-vt-module)
