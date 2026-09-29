# Hardware Lifecycle - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the examples, the benchmarks and the tests each carry
# exactly the options they need and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(HardwareLifecycle
        VERSION 1.0.0
        DESCRIPTION "Canonical physical hardware lifecycle runtime for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build hardware_lifecycle as a shared library instead of a static one" OFF)

option(HL_BUILD_TESTS "Build the Hardware Lifecycle test suite" ON)
option(HL_BUILD_TOOLS "Build the Hardware Lifecycle command line tools" ON)
option(HL_BUILD_EXAMPLES "Build the Hardware Lifecycle example programs" ON)
option(HL_BUILD_BENCHMARKS "Build the Hardware Lifecycle benchmarks" ON)
option(HL_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(HL_SANITIZERS "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# A shared build puts the library and every executable in one directory, so the
# runtime loader finds the library next to the program that uses it. A static
# build keeps the default layout.
if(BUILD_SHARED_LIBS AND NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(HL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/HardwareLifecycle"
    CACHE STRING "Install destination of the Hardware Lifecycle CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it becomes part of the installed package.
function(hl_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    # Two documented suppressions, and nothing else hides a defect:
    #   C4820 implicit structure padding. Domain types are never memcpy'd to a
    #         durable format - every byte is encoded field by field - so padding
    #         is irrelevant here and only noise.
    #   C4251 standard library member without a dll interface. Value types cross
    #         the shared library boundary through the shared C runtime; it only
    #         fires for BUILD_SHARED_LIBS builds.
    target_compile_options(${target} PRIVATE /wd4820)
    if(BUILD_SHARED_LIBS)
      target_compile_options(${target} PRIVATE /wd4251)
    endif()
    if(HL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(HL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(HL_SANITIZERS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# Adds one executable and applies the project build policy to it.
function(hl_add_executable target)
  add_executable(${target} ${ARGN})
  hl_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(HL_PUBLIC_HEADERS
  include/hardware_lifecycle/compatibility.hpp
  include/hardware_lifecycle/diff.hpp
  include/hardware_lifecycle/digest.hpp
  include/hardware_lifecycle/errors.hpp
  include/hardware_lifecycle/export.hpp
  include/hardware_lifecycle/health.hpp
  include/hardware_lifecycle/history.hpp
  include/hardware_lifecycle/ids.hpp
  include/hardware_lifecycle/lifecycle.hpp
  include/hardware_lifecycle/limits.hpp
  include/hardware_lifecycle/model.hpp
  include/hardware_lifecycle/persistence.hpp
  include/hardware_lifecycle/provenance.hpp
  include/hardware_lifecycle/registry.hpp
  include/hardware_lifecycle/replacement.hpp
  include/hardware_lifecycle/requests.hpp
  include/hardware_lifecycle/result.hpp
  include/hardware_lifecycle/runtime.hpp
  include/hardware_lifecycle/text.hpp
  include/hardware_lifecycle/version.hpp
  include/hardware_lifecycle/hardware_lifecycle.hpp)

set(HL_SOURCES
  src/compatibility.cpp
  src/crc32.cpp
  src/diff.cpp
  src/digest.cpp
  src/errors.cpp
  src/export.cpp
  src/fs_platform.cpp
  src/health.cpp
  src/history.cpp
  src/ids.cpp
  src/json.cpp
  src/lifecycle.cpp
  src/limits.cpp
  src/model.cpp
  src/persistence.cpp
  src/provenance.cpp
  src/registry.cpp
  src/replacement.cpp
  src/requests.cpp
  src/runtime.cpp
  src/serialization.cpp
  src/store_files.cpp
  src/text.cpp
  src/transition_table.cpp
  src/version.cpp)

set(HL_PRIVATE_HEADERS
  src/codec.hpp
  src/fs_platform.hpp
  src/json.hpp
  src/serialization.hpp
  src/store_files.hpp)

add_library(hardware_lifecycle ${HL_SOURCES} ${HL_PUBLIC_HEADERS} ${HL_PRIVATE_HEADERS})
add_library(HardwareLifecycle::hardware_lifecycle ALIAS hardware_lifecycle)

target_include_directories(hardware_lifecycle
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set_target_properties(hardware_lifecycle PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

hl_configure_target(hardware_lifecycle)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(hardware_lifecycle
    PUBLIC HARDWARE_LIFECYCLE_SHARED
    PRIVATE HARDWARE_LIFECYCLE_BUILDING)
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests and benchmarks
# ---------------------------------------------------------------------------

if(HL_BUILD_TOOLS)
  hl_add_executable(hwlifecycle tools/hwlifecycle.cpp)
  target_link_libraries(hwlifecycle PRIVATE HardwareLifecycle::hardware_lifecycle)
  # The command line tool renders --json output with the project's own strict
  # codec, which is an internal header and is not installed.
  target_include_directories(hwlifecycle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

  if(HL_BUILD_TESTS)
    # Process-level proof harnesses. They are real programs that a test starts
    # as separate operating-system processes to hold writer authority, to be
    # terminated at a chosen durable step, and to race for authority. They are
    # not installed.
    hl_add_executable(hl_crash_child tools/crash_child.cpp)
    target_link_libraries(hl_crash_child PRIVATE HardwareLifecycle::hardware_lifecycle)
    hl_add_executable(hl_lock_child tools/lock_child.cpp)
    target_link_libraries(hl_lock_child PRIVATE HardwareLifecycle::hardware_lifecycle)
  endif()
endif()

if(HL_BUILD_EXAMPLES)
  foreach(example IN ITEMS
      example_lifecycle_walkthrough
      example_durable_journal
      example_replacement_lineage)
    hl_add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE HardwareLifecycle::hardware_lifecycle)
  endforeach()
endif()

if(HL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HL_BUILD_BENCHMARKS)
  hl_add_executable(bench_hardware_lifecycle benchmarks/bench_hardware_lifecycle.cpp)
  target_link_libraries(bench_hardware_lifecycle PRIVATE HardwareLifecycle::hardware_lifecycle)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS hardware_lifecycle
        EXPORT HardwareLifecycleTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/hardware_lifecycle
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

if(HL_BUILD_TOOLS)
  install(TARGETS hwlifecycle RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT HardwareLifecycleTargets
        FILE HardwareLifecycleTargets.cmake
        NAMESPACE HardwareLifecycle::
        DESTINATION ${HL_INSTALL_CMAKEDIR})

configure_package_config_file(
  cmake/HardwareLifecycleConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/HardwareLifecycleConfig.cmake"
  INSTALL_DESTINATION ${HL_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/HardwareLifecycleConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/HardwareLifecycleConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/HardwareLifecycleConfigVersion.cmake"
        DESTINATION ${HL_INSTALL_CMAKEDIR})

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "Hardware Lifecycle ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  shared library      : ${BUILD_SHARED_LIBS}")
message(STATUS "  tests               : ${HL_BUILD_TESTS}")
message(STATUS "  tools               : ${HL_BUILD_TOOLS}")
message(STATUS "  examples            : ${HL_BUILD_EXAMPLES}")
message(STATUS "  benchmarks          : ${HL_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors  : ${HL_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${HL_SANITIZERS}")
