
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

find_package(SQLite3 REQUIRED)

add_library(dwarf_scraper_lib
  "storage.cc"
  "scraper.cc"
  "struct_layout_scraper.cc"
  "log.cc")
target_include_directories(dwarf_scraper_lib PRIVATE
  "${PROJECT_SOURCE_DIR}/cheri-compressed-cap")
target_compile_options(dwarf_scraper_lib PRIVATE
  "-fno-rtti" "-Wno-deprecated-enum-enum-conversion" "-Werror")
target_include_directories(dwarf_scraper_lib PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(dwarf_scraper_lib PUBLIC ${LLVM_DEFINITIONS_LIST})
target_link_directories(dwarf_scraper_lib PUBLIC ${LLVM_LIBRARY_DIRS})
target_link_libraries(dwarf_scraper_lib ${llvm_libs} SQLite::SQLite3)

add_executable(dwarf_scraper
  "dwarf_scraper.cc"
)
target_compile_options(dwarf_scraper PRIVATE
  "-fno-rtti" "-Werror")
target_link_libraries(dwarf_scraper dwarf_scraper_lib)

add_executable(test_subobject "test_subobject.cc")
target_link_libraries(test_subobject dwarf_scraper_lib GTest::gtest_main)
gtest_discover_tests(test_subobject PROPERTIES
  ENVIRONMENT "ASSET_DIR=${PROJECT_SOURCE_DIR}/../tests/assets")

