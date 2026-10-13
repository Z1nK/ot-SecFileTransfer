set(AUTH_TEST_SOURCES
  authenticator_test.cpp
  local_directory_test.cpp
  password_test.cpp
)
# The LDAP directory is tested when it is built, the stub otherwise.
if(WITH_LDAP)
  list(APPEND AUTH_TEST_SOURCES ldap_directory_test.cpp)
else()
  list(APPEND AUTH_TEST_SOURCES ldap_disabled_test.cpp)
endif()

confide_add_unit_tests(auth_tests
  SOURCES
    ${AUTH_TEST_SOURCES}
  LIBS
    ${PROJECT_NAMESPACE}::auth-static
)
