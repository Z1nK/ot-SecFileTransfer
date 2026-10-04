confide_add_unit_tests(auth_tests
  SOURCES
    authenticator_test.cpp
    base64_test.cpp
    password_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::auth-static
)
