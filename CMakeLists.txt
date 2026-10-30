cmake_minimum_required(VERSION 3.16)
project(SVGFixer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# A single self-contained exe: static C/C++ runtime.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 7z decoding: LZMA SDK (public domain), see third_party/lzma/README.md
file(GLOB LZMA_SOURCES CONFIGURE_DEPENDS third_party/lzma/*.c)
add_library(lzma7z STATIC ${LZMA_SOURCES})
target_include_directories(lzma7z PUBLIC third_party/lzma)
target_compile_definitions(lzma7z PUBLIC Z7_PPMD_SUPPORT Z7_EXTRACT_ONLY)

add_library(svgfix_core STATIC
  src/archive7z.cpp
  src/pathdata.cpp
  src/report.cpp
  src/svgfix.cpp
  src/util.cpp
  src/xml.cpp)
target_include_directories(svgfix_core PUBLIC src)
target_link_libraries(svgfix_core PUBLIC lzma7z)
target_compile_definitions(svgfix_core PUBLIC SVGFIX_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(svgfix_core PUBLIC /utf-8 /W4 /permissive-)
  target_compile_definitions(svgfix_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(svgfix_core PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_link_libraries(svgfix_core PUBLIC shell32)
endif()
if(MINGW)
  # C99 printf/strtod instead of msvcrt's, for identical number formatting everywhere
  target_compile_definitions(svgfix_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

set(APP_SOURCES src/main.cpp)
if(WIN32)
  list(APPEND APP_SOURCES src/app.rc)
endif()
add_executable(SVG-Fixer ${APP_SOURCES})
target_link_libraries(SVG-Fixer PRIVATE svgfix_core)
if(MINGW)
  target_link_options(SVG-Fixer PRIVATE -municode -static -s)
elseif(MSVC)
  target_link_options(SVG-Fixer PRIVATE /MANIFEST:NO)  # app.rc embeds app.manifest
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(svgfix_tests tests/tests.cpp)
  target_link_libraries(svgfix_tests PRIVATE svgfix_core)
  if(MINGW)
    target_link_options(svgfix_tests PRIVATE -static)
  endif()
  add_test(NAME svgfix_tests COMMAND svgfix_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
endif()
