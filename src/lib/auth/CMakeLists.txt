set(LOCAL_LIB_NAME auth)

add_library(${LOCAL_LIB_NAME} STATIC
  authenticator.cpp
  base64.cpp
  local_directory.cpp
  password.cpp
  user_directory.cpp
)

# One of two make_ldap_directory definitions, so no #ifdef is needed: the
# real one over ldapclient, or a stub that rejects provider = "ldap".
if(WITH_LDAP)
  target_sources(${LOCAL_LIB_NAME} PRIVATE ldap_directory.cpp)
  target_link_libraries(${LOCAL_LIB_NAME} PRIVATE ${PROJECT_NAMESPACE}::ldapclient-static)
else()
  target_sources(${LOCAL_LIB_NAME} PRIVATE ldap_directory_disabled.cpp)
endif()

add_library(${PROJECT_NAMESPACE}::auth-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# Authenticator and directories are built from Config (which brings
# logging along); digests use common's Sha256.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::config-static
  ${PROJECT_NAMESPACE}::common-static
)

# PBKDF2, RAND_bytes and CRYPTO_memcmp.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  OpenSSL::Crypto
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
