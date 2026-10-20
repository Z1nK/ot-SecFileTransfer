confide_add_unit_tests(forwarder_tests
  SOURCES
    forwarder_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::forwarder-static
    ${PROJECT_NAMESPACE}::api-static
)
