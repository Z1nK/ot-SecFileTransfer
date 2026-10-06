confide_add_unit_tests(storage_tests
  SOURCES
    storage_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::storage-static
)
