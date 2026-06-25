include(FetchContent)

# Catch2 v3 - the only third-party dependency in Stage 1.
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(stockwolf_tests
  perft_tests.cpp
  eval_tests.cpp
  search_tests.cpp
  uci_tests.cpp)
target_link_libraries(stockwolf_tests PRIVATE stockwolf_core Catch2::Catch2WithMain)

# The UCI conformance test drives the real engine binary, so build it first and
# pass its path in.
add_dependencies(stockwolf_tests stockwolf)

# Pass the path to the bundled EPD suite to the tests.
target_compile_definitions(stockwolf_tests PRIVATE
  PERFT_EPD_PATH="${CMAKE_CURRENT_SOURCE_DIR}/perft_suite.epd"
  STOCKWOLF_ENGINE="$<TARGET_FILE:stockwolf>")

# Register two ctest entries via Catch2 tag specs so CI can run only the fast
# gate. The slow entry is also labelled "slow" for `ctest -L/-LE slow`.
add_test(NAME stockwolf_fast COMMAND stockwolf_tests "~[slow]")
add_test(NAME stockwolf_slow COMMAND stockwolf_tests "[slow]")
set_tests_properties(stockwolf_slow PROPERTIES LABELS "slow")
