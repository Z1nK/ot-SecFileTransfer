include(GoogleTest)

# One test executable per library: <lib>_tests, built from <part>_test.cpp.
function(confide_add_unit_tests name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  if (MSVC)
    target_compile_options(${name} PRIVATE /W4)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -pedantic -Werror)
  endif()
  gtest_discover_tests(${name})
endfunction()

add_subdirectory(common)
add_subdirectory(logging)
