
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

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Sql)
qt_standard_project_setup()

add_library(dwarf_scraper_lib
  "global_sym_scraper.cc"
  "flat_layout_scraper.cc"
  "scraper.cc"
  "storage.cc"
)
target_include_directories(dwarf_scraper_lib PRIVATE
  "${PROJECT_SOURCE_DIR}/third-party/cheri-compressed-cap")
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


