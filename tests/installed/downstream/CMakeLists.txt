# An independent consumer of the installed Hardware Lifecycle package.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately not part of the Hardware Lifecycle build: it is
# configured and built on its own against an installed prefix, which is what
# proves that the exported package, the installed headers and the installed
# library are usable by a downstream consumer.

cmake_minimum_required(VERSION 3.20)

project(HardwareLifecycleDownstreamConsumer
        VERSION 1.0.0
        DESCRIPTION "Out of tree consumer of the installed Hardware Lifecycle package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(HardwareLifecycle CONFIG REQUIRED)

add_executable(hl_downstream_consumer main.cpp)
target_link_libraries(hl_downstream_consumer PRIVATE HardwareLifecycle::hardware_lifecycle)
