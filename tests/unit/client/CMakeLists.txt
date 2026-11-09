confide_add_unit_tests(client_tests
  SOURCES
    recent_recipients_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::client-static
)
