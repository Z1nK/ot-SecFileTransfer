confide_add_unit_tests(logging_tests
  SOURCES
    tech_log_test.cpp
    file_sink_test.cpp
    console_sink_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::logging-static
)
