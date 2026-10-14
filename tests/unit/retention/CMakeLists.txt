confide_add_unit_tests(retention_tests
  SOURCES
    scheduler_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::retention-static
)
