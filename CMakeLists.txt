cmake_minimum_required(VERSION 3.16)
project(ArborDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(-Wall -Wextra)

# Sanitizers: "auto" = AddressSanitizer+UBSan for Debug builds, none otherwise. ThreadSanitizer cannot be
# combined with AddressSanitizer, so it is a separate build: -DARBOR_SANITIZER=thread
set(ARBOR_SANITIZER "auto" CACHE STRING "auto | address | thread | none")
if(ARBOR_SANITIZER STREQUAL "auto")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ARBOR_SANITIZER "address")
  else()
    set(ARBOR_SANITIZER "none")
  endif()
endif()
if(ARBOR_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(ARBOR_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()
message(STATUS "ArborDB: build type '${CMAKE_BUILD_TYPE}', sanitizer '${ARBOR_SANITIZER}'")

# Core library; later milestones add their sources here.
file(GLOB_RECURSE ARBOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER ARBOR_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
add_library(arbordb_core STATIC ${ARBOR_SOURCES})
target_include_directories(arbordb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(arbordb_core PUBLIC Threads::Threads)

# Tests: one gtest executable per tests/test_*.cpp.
enable_testing()
find_package(GTest REQUIRED)
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE arbordb_core GTest::gtest_main)
  target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
  target_compile_definitions(${test_name} PRIVATE ARBOR_SQL_DIR="${CMAKE_SOURCE_DIR}/tests/sql")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Helper executables (crash-test driver, stress test, ...): one per tools/*.cpp.
file(GLOB TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/*.cpp)
foreach(tool_src ${TOOL_SOURCES})
  get_filename_component(tool_name ${tool_src} NAME_WE)
  add_executable(${tool_name} ${tool_src})
  target_link_libraries(${tool_name} PRIVATE arbordb_core)
  target_include_directories(${tool_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
endforeach()

add_test(NAME crash_recovery
         COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/crash_recovery_test.sh $<TARGET_FILE:crash_driver>)
set_tests_properties(crash_recovery PROPERTIES ENVIRONMENT "ARBORDB=$<TARGET_FILE:arbordb>")

# The interactive SQL shell.
add_executable(arbordb src/main.cpp)
target_link_libraries(arbordb PRIVATE arbordb_core)

# Per-layer microbenchmarks (run from a Release build): ./bin/arbordb_bench [--quick] [buffer|btree|heap|txn|sql]
add_executable(arbordb_bench bench/bench.cpp)
target_link_libraries(arbordb_bench PRIVATE arbordb_core)
