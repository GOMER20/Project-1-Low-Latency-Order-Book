cmake_minimum_required(VERSION 3.20)

project(lob_engine
  VERSION 0.1.0
  DESCRIPTION "Ultra-low-latency limit order book matching engine"
  LANGUAGES CXX)

# --- Language standard -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Build type --------------------------------------------------------------
# Latency numbers from an unoptimised build are meaningless, so default to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# --- Options -----------------------------------------------------------------
option(LOB_BUILD_TESTS        "Build the GoogleTest unit tests"            ON)
option(LOB_BUILD_BENCHMARKS   "Build the Google Benchmark suite"           ON)
option(LOB_NATIVE_ARCH        "Tune for the host CPU (-march=native)"      ON)
option(LOB_ENABLE_LTO         "Link-time optimisation in Release builds"   ON)
option(LOB_ENABLE_SANITIZERS  "Build with ASan + UBSan (use with Debug)"   OFF)
option(LOB_ENABLE_TSAN        "Build with ThreadSanitizer (use with Debug)" OFF)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"          OFF)

# --- Flags shared by every first-party target --------------------------------
# Kept on an INTERFACE target so they never leak into third-party dependencies.
add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wcast-align -Wdouble-promotion
  $<$<BOOL:${LOB_WARNINGS_AS_ERRORS}>:-Werror>)

if(LOB_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native LOB_HAS_MARCH_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    target_compile_options(lob_flags INTERFACE -march=native)
  else()
    # AArch64 toolchains spell it -mcpu.
    check_cxx_compiler_flag(-mcpu=native LOB_HAS_MCPU_NATIVE)
    if(LOB_HAS_MCPU_NATIVE)
      target_compile_options(lob_flags INTERFACE -mcpu=native)
    endif()
  endif()
endif()

if(LOB_ENABLE_SANITIZERS)
  target_compile_options(lob_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
endif()

# ThreadSanitizer checks the SPSC ring's memory ordering. It cannot share a
# binary with AddressSanitizer.
if(LOB_ENABLE_TSAN)
  if(LOB_ENABLE_SANITIZERS)
    message(FATAL_ERROR "LOB_ENABLE_TSAN and LOB_ENABLE_SANITIZERS cannot be combined")
  endif()
  target_compile_options(lob_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(lob_flags INTERFACE -fsanitize=thread)
endif()

if(LOB_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LOB_IPO_SUPPORTED OUTPUT LOB_IPO_MESSAGE)
  if(NOT LOB_IPO_SUPPORTED)
    message(STATUS "LTO disabled: ${LOB_IPO_MESSAGE}")
  endif()
endif()

# --- Core library (header-only) ----------------------------------------------
# Header-only so the whole hot path is visible to the optimiser in one TU.
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)
target_link_libraries(lob INTERFACE lob_flags)

function(lob_add_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lob::lob Threads::Threads)
  if(LOB_ENABLE_LTO AND LOB_IPO_SUPPORTED)
    set_property(TARGET ${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  endif()
endfunction()

# --- Dependencies ------------------------------------------------------------
# Prefer system packages (apt/brew); fall back to a pinned source download.
include(FetchContent)
find_package(Threads REQUIRED)

# --- Tests -------------------------------------------------------------------
if(LOB_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GTest not found on system - fetching v1.17.0")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.17.0
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  include(GoogleTest)

  lob_add_executable(lob_tests
    tests/toolchain_test.cpp
    tests/order_pool_test.cpp
    tests/price_level_test.cpp
    tests/level_bitmap_test.cpp
    tests/order_book_test.cpp
    tests/matching_test.cpp
    tests/edge_case_test.cpp
    tests/model_test.cpp
    tests/spsc_ring_test.cpp
    tests/matching_engine_test.cpp
    tests/order_type_test.cpp
    tests/thread_affinity_test.cpp
    tests/multi_symbol_test.cpp
    tests/sharded_engine_test.cpp
    tests/shard_assignment_test.cpp
    tests/symbol_move_test.cpp)
  target_link_libraries(lob_tests PRIVATE GTest::gtest_main)
  # Listing the tests runs the binary at build time. The default limit of 5
  # seconds is too tight for a sanitizer build on a busy machine.
  # The per-test limit turns a hang into a failure instead of a stalled run.
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)

  # This test replaces global operator new to count allocations. Sanitizer
  # runtimes replace operator new too, and the two cannot share a binary, so
  # the test gets an executable of its own and is left out of sanitizer builds.
  if(NOT LOB_ENABLE_SANITIZERS AND NOT LOB_ENABLE_TSAN)
    lob_add_executable(lob_allocation_test
      tests/allocation_test.cpp)
    target_link_libraries(lob_allocation_test PRIVATE GTest::gtest_main)
    gtest_discover_tests(lob_allocation_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
  endif()
endif()

# --- Benchmarks --------------------------------------------------------------
if(LOB_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG QUIET)
  if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found on system - fetching v1.9.4")
    set(BENCHMARK_ENABLE_TESTING       OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS   OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL       OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR        OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG        v1.9.4
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(benchmark)
  endif()

  lob_add_executable(lob_bench
    bench/latency_bench.cpp
    bench/matching_bench.cpp
    bench/order_book_bench.cpp
    bench/price_level_bench.cpp
    bench/order_pool_bench.cpp
    bench/spsc_ring_bench.cpp
    bench/engine_bench.cpp
    bench/sharded_engine_bench.cpp)
  target_link_libraries(lob_bench PRIVATE benchmark::benchmark_main)
endif()
