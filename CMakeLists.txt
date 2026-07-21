cmake_minimum_required(VERSION 3.20)

project(grimfang LANGUAGES CXX)

set(GRIMFANG_NET_BIN "${CMAKE_CURRENT_SOURCE_DIR}/tools/training/grimfang-net-003.bin")
set(GRIMFANG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GRIMFANG_EMBEDDED_NET_CPP "${GRIMFANG_GENERATED_DIR}/grimfang_net_embedded.cpp")

# Generate the embedded network with CMake itself so the build has no host-shell
# dependency. Reconfigure automatically if the selected network changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GRIMFANG_NET_BIN}")
file(READ "${GRIMFANG_NET_BIN}" GRIMFANG_NET_HEX HEX)
string(TOUPPER "${GRIMFANG_NET_HEX}" GRIMFANG_NET_HEX)
string(REGEX REPLACE
  "([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])([0-9A-F][0-9A-F])"
  "    0x\\1, 0x\\2, 0x\\3, 0x\\4, 0x\\5, 0x\\6, 0x\\7, 0x\\8,\n"
  GRIMFANG_NET_INITIALIZER
  "${GRIMFANG_NET_HEX}")
file(MAKE_DIRECTORY "${GRIMFANG_GENERATED_DIR}")
file(WRITE "${GRIMFANG_EMBEDDED_NET_CPP}"
  "#include <cstddef>\n"
  "\n"
  "namespace embedded_net {\n"
  "\n"
  "alignas(64) extern const unsigned char g_grimfang_net[] = {\n"
  "${GRIMFANG_NET_INITIALIZER}"
  "};\n"
  "\n"
  "extern const std::size_t g_grimfang_net_size = sizeof(g_grimfang_net);\n"
  "\n"
  "} // namespace embedded_net\n")
unset(GRIMFANG_NET_HEX)
unset(GRIMFANG_NET_INITIALIZER)

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
option(GRIMFANG_TESTS "Build the test suite"                        ON)

# ---------------------------------------------------------------------------
# Warning / optimization flags (per-compiler, zero-warning goal)
# ---------------------------------------------------------------------------
add_library(grimfang_flags INTERFACE)

if(MSVC)
  target_compile_options(grimfang_flags INTERFACE /W4 /permissive- /EHsc)
  target_compile_options(grimfang_flags INTERFACE
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:RelWithDebInfo>:/O2>)
  # AVX2 for the NNUE SIMD inference/accumulator paths (all configs: the Debug
  # test build exercises the SIMD path under NNUE_INCREMENTAL_CHECK). Defines
  # __AVX2__ so the compile-time guard in nnue.cpp selects the vectorized path.
  target_compile_options(grimfang_flags INTERFACE /arch:AVX2)
else()
  target_compile_options(grimfang_flags INTERFACE -Wall -Wextra -Wshadow)
  target_compile_options(grimfang_flags INTERFACE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2>)
  target_compile_options(grimfang_flags INTERFACE -mavx2)
endif()

if(USE_PEXT)
  target_compile_definitions(grimfang_flags INTERFACE USE_PEXT=1)
  if(NOT MSVC)
    target_compile_options(grimfang_flags INTERFACE -mbmi2)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library (every module except main) so tests can reuse it
# ---------------------------------------------------------------------------
add_library(grimfang_core STATIC
  src/bitboard.cpp
  src/attacks.cpp
  src/zobrist.cpp
  src/position.cpp
  src/movegen.cpp
  src/perft.cpp
  src/eval.cpp
  src/nnue.cpp
  src/tt.cpp
  src/search.cpp
  src/timeman.cpp
  src/uci.cpp
  src/datagen.cpp
  ${GRIMFANG_EMBEDDED_NET_CPP})

find_package(Threads REQUIRED)

target_include_directories(grimfang_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(grimfang_core PUBLIC grimfang_flags Threads::Threads)
target_compile_definitions(grimfang_core PRIVATE $<$<CONFIG:Debug>:NNUE_INCREMENTAL_CHECK=1>)

# ---------------------------------------------------------------------------
# Engine executable
# ---------------------------------------------------------------------------
add_executable(grimfang src/main.cpp)
target_link_libraries(grimfang PRIVATE grimfang_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GRIMFANG_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
