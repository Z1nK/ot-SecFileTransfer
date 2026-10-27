# End-to-end tests: real confide-server processes driven with ftc and curl.
# Shell scripts, so Unix only; they need openssl (test certificate) and curl.
if(WIN32 OR NOT TARGET confide-server)
  return()
endif()

find_program(BASH_PROGRAM bash)
find_program(OPENSSL_PROGRAM openssl)
find_program(CURL_PROGRAM curl)
if(NOT BASH_PROGRAM OR NOT OPENSSL_PROGRAM OR NOT CURL_PROGRAM)
  message(STATUS "integration tests skipped: need bash, openssl and curl")
  return()
endif()

add_test(NAME integration.two_sites
  COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/two_sites.sh $<TARGET_FILE_DIR:confide-server>)
set_tests_properties(integration.two_sites PROPERTIES
  LABELS integration
  TIMEOUT 180)

# Admin interface with LDAP users: needs a server built WITH_LDAP and the
# directory of docker/docker-compose.yml. Skipped (exit 77) unless
# CONFIDE_LDAP_TEST_URI is set when ctest runs:
#   CONFIDE_LDAP_TEST_URI=ldap://localhost:1389 ctest -L ldap
if(WITH_LDAP)
  add_test(NAME integration.ldap_admin
    COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/ldap_admin.sh $<TARGET_FILE_DIR:confide-server>)
  set_tests_properties(integration.ldap_admin PROPERTIES
    LABELS "integration;ldap"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)
endif()
