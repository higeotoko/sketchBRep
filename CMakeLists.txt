cmake_minimum_required(VERSION 3.21)
project(SketchBRep VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SBR_BUILD_APP   "Build the Qt desktop application" ON)
option(SBR_BUILD_TESTS "Build automated tests" ON)
option(SBR_BUILD_TOOLS "Build CLI verification tools" ON)
option(SBR_WITH_OPENNURBS "Rhino .3dm import/export (downloads openNURBS)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# openNURBS is added before our own compile options so that its sources
# build with the flags its authors use.
if(SBR_WITH_OPENNURBS)
  enable_language(C)  # its bundled zlib
  include(FetchContent)
  FetchContent_Declare(opennurbs
    URL https://github.com/mcneel/opennurbs/archive/eb92af3ba1806b0a34a99aba0d3bda83e3d46083.tar.gz)
  FetchContent_GetProperties(opennurbs)
  if(NOT opennurbs_POPULATED)
    FetchContent_Populate(opennurbs)
    add_subdirectory(${opennurbs_SOURCE_DIR} ${opennurbs_BINARY_DIR} EXCLUDE_FROM_ALL)
    # Its zlib calls back into openNURBS for memory (zcalloc/zcfree): declare
    # the cycle so static linking repeats the libraries in order.
    target_link_libraries(zlib INTERFACE opennurbsStatic)
  endif()
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /bigobj)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WNT)
else()
  add_compile_options(-Wall -Wextra)
  # Checked standard containers (e.g. back() on an empty vector traps
  # instead of reading garbage), so such mistakes fail the tests.
  add_compile_definitions(_GLIBCXX_ASSERTIONS)
endif()

find_package(OpenCASCADE REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_subdirectory(src/core)

if(SBR_BUILD_TOOLS)
  add_subdirectory(tools/phase0)
  add_subdirectory(tools/cli)
  add_subdirectory(tools/bench)
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
