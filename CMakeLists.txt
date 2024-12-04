cmake_minimum_required(VERSION 3.20)
project(benchplot-tools LANGUAGES C CXX)

set(CHERISDK "" CACHE STRING "Path to the CHERI SDK directory, used to find LLVM")

set(CMAKE_SHARED_MODULE_PREFIX "")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CHERISDK STREQUAL "")
  message(FATAL_ERROR "Must set the CHERISDK CMake option")
endif ()

set(CMAKE_PREFIX_PATH "${CHERISDK}/sdk")

enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

find_package(LLVM REQUIRED CONFIG NO_CMAKE_SYSTEM_PATH)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs
  DebugInfoDWARF
  AllTargetsDescs
  AllTargetsInfos
  MC
  Object
  Support)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})

find_package(Qt6 REQUIRED COMPONENTS Core Sql)
qt_standard_project_setup()

add_library(dwarf_scraper_lib
  "flat_layout_scraper.cc"
  "scraper.cc"
  "storage.cc"
)
target_include_directories(dwarf_scraper_lib PRIVATE
  "${PROJECT_SOURCE_DIR}/../cheri-compressed-cap")
target_compile_options(dwarf_scraper_lib PRIVATE
  "-fno-rtti" "-Wno-deprecated-enum-enum-conversion" "-Werror")
target_include_directories(dwarf_scraper_lib PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(dwarf_scraper_lib PUBLIC ${LLVM_DEFINITIONS_LIST})
target_link_directories(dwarf_scraper_lib PUBLIC ${LLVM_LIBRARY_DIRS})
target_link_libraries(dwarf_scraper_lib PUBLIC ${llvm_libs})
target_link_libraries(dwarf_scraper_lib PUBLIC Qt6::Core Qt6::Sql)

qt_add_executable(dwarf_scraper
  "dwarf_scraper.cc"
)
target_compile_options(dwarf_scraper PRIVATE
  "-fno-rtti" "-Werror")
target_link_libraries(dwarf_scraper PRIVATE dwarf_scraper_lib)

# add_executable(test_subobject "test_subobject.cc")
# target_link_libraries(test_subobject dwarf_scraper_lib GTest::gtest_main)
# gtest_discover_tests(test_subobject PROPERTIES
#   ENVIRONMENT "ASSET_DIR=${PROJECT_SOURCE_DIR}/../tests/assets")

