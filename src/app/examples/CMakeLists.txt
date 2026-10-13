add_subdirectory(error-prop)
add_subdirectory(config-read)

if(WITH_LDAP)
  add_subdirectory(ldapauth-demo)
endif()
