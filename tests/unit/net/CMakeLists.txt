confide_add_unit_tests(net_tests
  SOURCES
    http_client_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::net-static
)
