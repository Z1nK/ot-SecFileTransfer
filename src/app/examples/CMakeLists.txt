add_subdirectory(error-prop)
add_subdirectory(config-read)
add_subdirectory(storage-basic)
add_subdirectory(logging-demo)

if(WITH_LDAP)
  add_subdirectory(ldapauth-demo)
endif()
