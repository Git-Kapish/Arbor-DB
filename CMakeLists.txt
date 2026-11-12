cmake_minimum_required(VERSION 3.16)
project(ArborDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(-Wall -Wextra)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

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
