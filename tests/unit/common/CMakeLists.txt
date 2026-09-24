confide_add_unit_tests(common_tests
  SOURCES
    error_test.cpp
    id_test.cpp
    time_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::common-static
)
