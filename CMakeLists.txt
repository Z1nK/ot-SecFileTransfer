cmake_minimum_required(VERSION 3.20)

# Release builds get the version from the git tag: CI turns tag vX.Y.Z into
# -DAPP_VERSION=X.Y.Z. Local builds keep the 0.0.0 default.
set(APP_VERSION "0.7.8" CACHE STRING "Application version X.Y.Z, set by CI from the git tag")
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
option(WITH_EXAMPLES "Whether to build the example-* programs" ON)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake/modules)
set(CMAKE_CONFIG_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake/configs)
include(configuration)

add_subdirectory(src)

if(WITH_GOOGLE_TEST)
  enable_testing()
  add_subdirectory(tests)
endif()

# Server files outside bin/. The config dir is absolute because packages put
# it in /etc whatever the prefix; for a local `cmake --install` pass e.g.
# -DCONFIDE_CONFIG_DIR=etc/confide.
set(CONFIDE_CONFIG_DIR "/etc/confide" CACHE STRING "Where the server config is installed")
install(FILES config/server.example.toml
        DESTINATION ${CONFIDE_CONFIG_DIR} RENAME server.toml COMPONENT server)
install(FILES packaging/confide-server.service
        DESTINATION lib/systemd/system COMPONENT server)

# Desktop integration of ftc (integrations/): the GNOME Files menu items and
# the dialogs they start. A package of its own (confide-client-nautilus), so
# confide-client stays the bare ftc binary on servers and headless VMs.
if(UNIX AND NOT APPLE)
  install(PROGRAMS integrations/nautilus/ftc-send-dialog
                   integrations/nautilus/ftc-get-dialog
          DESTINATION bin COMPONENT nautilus)
  install(FILES integrations/nautilus/ftc_nautilus.py
          DESTINATION share/nautilus-python/extensions COMPONENT nautilus)
endif()

# -----------------------------------------------------------------------------
# Packaging: one .deb/.rpm per install() COMPONENT
#   server -> confide         (confide-server)
#   utils  -> confide-utils   (confide-passwd)
#   client -> confide-client  (ftc)
#   nautilus -> confide-client-nautilus  (GNOME Files menu items; Linux only)
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
if(UNIX AND NOT APPLE)
  list(APPEND CPACK_COMPONENTS_ALL nautilus)
endif()
set(CPACK_DEB_COMPONENT_INSTALL ON)

set(CPACK_DEBIAN_SERVER_PACKAGE_NAME confide)
set(CPACK_DEBIAN_UTILS_PACKAGE_NAME confide-utils)
set(CPACK_DEBIAN_CLIENT_PACKAGE_NAME confide-client)
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_NAME confide-client-nautilus)

set(CPACK_COMPONENT_SERVER_DESCRIPTION "Confide secure file transfer server")
set(CPACK_COMPONENT_UTILS_DESCRIPTION "Confide administration tools (confide-passwd)")
set(CPACK_COMPONENT_CLIENT_DESCRIPTION "Confide command line client (ftc)")
set(CPACK_COMPONENT_NAUTILUS_DESCRIPTION "GNOME Files (Nautilus) menu items for ftc: Send with ftc, Get with ftc")

# confide-client-nautilus: shell scripts and a Python file, the same for every
# architecture. Needs the ftc of the same version and zenity for the dialogs;
# nautilus-python (top-level menu items; without it the dialogs work as
# Nautilus scripts) and notify-send are weak dependencies, as nautilus-python
# is not in every RHEL repository. confide-client up to 0.7.6 had these files
# itself: Replaces lets dpkg move them over on upgrade. No Breaks, which would
# make a local build (default version 0.7.6) uninstallable.
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_ARCHITECTURE all)
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_DEPENDS "confide-client (= ${PROJECT_VERSION}), zenity")
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_RECOMMENDS "python3-nautilus, libnotify-bin")
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_REPLACES "confide-client (<< 0.7.7)")
set(CPACK_DEBIAN_NAUTILUS_PACKAGE_SHLIBDEPS OFF)

# <name>_<version>_<arch>.deb, and Depends: filled from the linked shared
# libraries (needs dpkg-shlibdeps).
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# confide: server.toml is a conffile (kept on upgrade if edited), the scripts
# add the confide user and handle the service.
set(CPACK_DEBIAN_SERVER_PACKAGE_CONTROL_EXTRA
  ${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/conffiles
  ${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst
  ${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/prerm
  ${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

# RPM (EL8/EL9 CI jobs run `cpack -G RPM`): same components,
# <name>-<version>-1.el<N>.<arch>.rpm, Requires: from the linked shared libraries.
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_SERVER_PACKAGE_NAME confide)
set(CPACK_RPM_UTILS_PACKAGE_NAME confide-utils)
set(CPACK_RPM_CLIENT_PACKAGE_NAME confide-client)
set(CPACK_RPM_NAUTILUS_PACKAGE_NAME confide-client-nautilus)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
# confide-client-nautilus: as for the .deb. RPM resolves the file move from
# an old confide-client by itself, as both are upgraded in one transaction.
set(CPACK_RPM_NAUTILUS_PACKAGE_ARCHITECTURE noarch)
set(CPACK_RPM_NAUTILUS_PACKAGE_REQUIRES "confide-client = ${PROJECT_VERSION}, zenity")
set(CPACK_RPM_NAUTILUS_PACKAGE_RECOMMENDS "nautilus-python, libnotify")
# ftc_nautilus.py is loaded by Nautilus, not a Python module: don't byte-compile
# it into __pycache__ files that the generated file list doesn't have.
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _python_bytecompile_extra 0")

# confide: same as the .deb. The confide user is made in %pre so that the
# files below can belong to its group.
set(CPACK_RPM_SERVER_PACKAGE_REQUIRES_PRE "shadow-utils")
set(CPACK_RPM_SERVER_PRE_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/pre.sh)
set(CPACK_RPM_SERVER_POST_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/post.sh)
set(CPACK_RPM_SERVER_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preun.sh)
set(CPACK_RPM_SERVER_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun.sh)
set(CPACK_RPM_SERVER_USER_FILELIST
  "%dir %attr(0750,root,confide) /etc/confide"
  "%config(noreplace) %attr(0640,root,confide) /etc/confide/server.toml")
# Owned by systemd, not by us.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/lib/systemd /usr/lib/systemd/system)

include(CPack)
