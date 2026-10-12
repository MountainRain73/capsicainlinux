cmake_minimum_required(VERSION 3.16)
project(capsicain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

option(CAPSICAIN_DEBUG "Enable debug output in binary" OFF)
option(CAPSICAIN_TRACE "Enable trace output in binary" OFF)
option(CAPSICAIN_PROFILE "Enable profiling output in binary" OFF)

if(CAPSICAIN_DEBUG)
    add_definitions(-DCAPSICAIN_DEBUG)
endif()
if(CAPSICAIN_TRACE)
    add_definitions(-DCAPSICAIN_TRACE)
endif()
if(CAPSICAIN_PROFILE)
    add_definitions(-DCAPSICAIN_PROFILE)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DCAPSICAIN_DEBUG)
endif()

set(SOURCES
    src/capsicain.cpp
    src/configUtils.cpp
    src/modifiers.cpp
    src/scancodes.cpp
    src/utils.cpp
    src/linux_input.cpp
    src/pch.cpp
)

add_executable(capsicain ${SOURCES})

target_include_directories(capsicain PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(capsicain PRIVATE pthread)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(capsicain PRIVATE -s)
endif()

install(TARGETS capsicain RUNTIME DESTINATION bin)