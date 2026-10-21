cmake_minimum_required(VERSION 3.21)
project(SketchBRep VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SBR_BUILD_APP   "Build the Qt desktop application" ON)
option(SBR_BUILD_TESTS "Build automated tests" ON)
option(SBR_BUILD_TOOLS "Build CLI verification tools" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /bigobj)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WNT)
else()
  add_compile_options(-Wall -Wextra)
endif()

find_package(OpenCASCADE REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_subdirectory(src/core)

if(SBR_BUILD_TOOLS)
  add_subdirectory(tools/phase0)
  add_subdirectory(tools/cli)
  if(SBR_BUILD_APP)
    add_subdirectory(tools/mcp)  # talks to the app over Qt's local socket
  endif()
endif()

if(SBR_BUILD_APP)
  add_subdirectory(src/app)
endif()

if(SBR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
