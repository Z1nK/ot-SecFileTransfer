confide_add_unit_tests(client_tests
  SOURCES
    recent_recipients_test.cpp
    settings_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::client-static
)

# settings_test checks the system config path ftc was built with.
target_compile_definitions(client_tests PRIVATE
  FTC_SYSTEM_CONFIG_FOR_TEST="${FTC_SYSTEM_CONFIG}")
