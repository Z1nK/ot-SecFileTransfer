cmake_minimum_required(VERSION 3.20)

# Release builds get the version from the git tag: CI turns tag vX.Y.Z into
# -DAPP_VERSION=X.Y.Z. Local builds keep the 0.0.0 default.
set(APP_VERSION "0.1.1" CACHE STRING "Application version X.Y.Z, set by CI from the git tag")
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

# -----------------------------------------------------------------------------
# Packaging: one .deb/.rpm per install() COMPONENT
#   server -> confide         (confide-server)
#   utils  -> confide-utils   (confide-passwd)
#   client -> confide-client  (ftc)
# -----------------------------------------------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME confide)
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT example@example.com)

# Only these components are packaged; anything else installed (e.g. by
# FetchContent'ed googletest) is left out.
set(CPACK_COMPONENTS_ALL server utils client)
set(CPACK_DEB_COMPONENT_INSTALL ON)

set(CPACK_DEBIAN_SERVER_PACKAGE_NAME confide)
set(CPACK_DEBIAN_UTILS_PACKAGE_NAME confide-utils)
set(CPACK_DEBIAN_CLIENT_PACKAGE_NAME confide-client)

set(CPACK_COMPONENT_SERVER_DESCRIPTION "Confide secure file transfer server")
set(CPACK_COMPONENT_UTILS_DESCRIPTION "Confide administration tools (confide-passwd)")
set(CPACK_COMPONENT_CLIENT_DESCRIPTION "Confide command line client (ftc)")

# <name>_<version>_<arch>.deb, and Depends: filled from the linked shared
# libraries (needs dpkg-shlibdeps).
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# RPM (EL8 CI job runs `cpack -G RPM`): same three components,
# <name>-<version>-1.el8.<arch>.rpm, Requires: from the linked shared libraries.
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_SERVER_PACKAGE_NAME confide)
set(CPACK_RPM_UTILS_PACKAGE_NAME confide-utils)
set(CPACK_RPM_CLIENT_PACKAGE_NAME confide-client)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)

include(CPack)
