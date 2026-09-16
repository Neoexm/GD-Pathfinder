cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(GDPathfinder VERSION 1.0.0)

# __FILE__ otherwise bakes the absolute source path, and so the name of the machine it
# was built on, into the binary. Directory scope so it also covers the dependencies
# compiled inside the build tree, which is where all of them came from.
if (MSVC)
    add_compile_options("/d1trimfile:${CMAKE_SOURCE_DIR}/" "/d1trimfile:${CMAKE_SOURCE_DIR}\\")
endif()

# Add all source files inside src (recursively)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Set up the mod binary
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Set up dependencies, resources, and link Geode.
setup_geode_mod(${PROJECT_NAME})
# Keep the absolute build path out of the shipped binary. Without this MSVC embeds the
# full local path to the pdb, which means the machine it was built on.
if (MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE "/PDBALTPATH:%_PDB%")
endif()

