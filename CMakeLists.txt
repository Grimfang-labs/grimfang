cmake_minimum_required(VERSION 3.20)

project(stockwolf LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global C++ settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(USE_PEXT       "Use BMI2 PEXT instruction for magic indexing" OFF)
option(STOCKWOLF_TESTS "Build the test suite"                        ON)

# ---------------------------------------------------------------------------
# Warning / optimization flags (per-compiler, zero-warning goal)
# ---------------------------------------------------------------------------
add_library(stockwolf_flags INTERFACE)

if(MSVC)
  target_compile_options(stockwolf_flags INTERFACE /W4 /permissive- /EHsc)
  target_compile_options(stockwolf_flags INTERFACE
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:RelWithDebInfo>:/O2>)
else()
  target_compile_options(stockwolf_flags INTERFACE -Wall -Wextra -Wshadow)
  target_compile_options(stockwolf_flags INTERFACE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2>)
endif()

if(USE_PEXT)
  target_compile_definitions(stockwolf_flags INTERFACE USE_PEXT=1)
  if(NOT MSVC)
    target_compile_options(stockwolf_flags INTERFACE -mbmi2)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library (every module except main) so tests can reuse it
# ---------------------------------------------------------------------------
add_library(stockwolf_core STATIC
  src/bitboard.cpp
  src/attacks.cpp
  src/zobrist.cpp
  src/position.cpp
  src/movegen.cpp
  src/perft.cpp
  src/eval.cpp
  src/search.cpp)

target_include_directories(stockwolf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(stockwolf_core PUBLIC stockwolf_flags)

# ---------------------------------------------------------------------------
# Engine executable
# ---------------------------------------------------------------------------
add_executable(stockwolf src/main.cpp)
target_link_libraries(stockwolf PRIVATE stockwolf_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(STOCKWOLF_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
