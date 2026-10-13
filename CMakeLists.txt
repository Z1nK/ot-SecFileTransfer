cmake_minimum_required(VERSION 3.20)

# Release builds get the version from the git tag: CI turns tag vX.Y.Z into
# -DAPP_VERSION=X.Y.Z. Local builds keep the 0.0.0 default.
set(APP_VERSION "0.1.0" CACHE STRING "Application version X.Y.Z, set by CI from the git tag")
if(NOT APP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "APP_VERSION '${APP_VERSION}' is not X.Y.Z")
endif()

project(SecFileTransfer VERSION ${APP_VERSION})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_NAMESPACE confide)

option(WITH_GOOGLE_TEST "Whether to build Google test" ON)
option(WITH_LDAP "Whether to build LDAP authentication support (needs OpenLDAP)" ON)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake/modules)
set(CMAKE_CONFIG_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake/configs)
include(configuration)

add_subdirectory(src)

if(WITH_GOOGLE_TEST)
  enable_testing()
  add_subdirectory(tests)
endif()

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT example@example.com)
include(CPack)
