set(LOCAL_LIB_NAME ldapclient)

add_library(${LOCAL_LIB_NAME} STATIC
  connection.cpp
  ldapAuth.cpp
)

add_library(${PROJECT_NAMESPACE}::ldapclient-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# Consumers can guard LDAP-only code with #ifdef CONFIDE_WITH_LDAP.
target_compile_definitions(${LOCAL_LIB_NAME} PUBLIC CONFIDE_WITH_LDAP)

# libldap stays inside connection.cpp; ldapAuth.hpp only forward-declares it.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  PkgConfig::OPENLDAP
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
