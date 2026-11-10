set(LOCAL_EXE_NAME ftc)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE
  main.cpp
  dispatch.cpp
  password.cpp
  session.cpp
  commands/admin.cpp
  commands/auth.cpp
  commands/compose.cpp
  commands/get.cpp
  commands/query.cpp
  commands/send.cpp
)

# Terminal (hidden password, isatty) and OS account: one file per OS.
if (WIN32)
  target_sources(${LOCAL_EXE_NAME} PRIVATE platform_win.cpp)
else()
  target_sources(${LOCAL_EXE_NAME} PRIVATE platform_posix.cpp)
endif()

# commands/ includes the headers next to main.cpp by their bare names.
target_include_directories(${LOCAL_EXE_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# client: token cache, local files, replies, outgoing transactions (brings
# net and common along). Boost.JSON for the replies, OpenSSL for
# OPENSSL_cleanse.
target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::client-static
  ${PROJECT_NAMESPACE}::net-static
  ${PROJECT_NAMESPACE}::common-static
  Boost::headers
  Boost::json
  Boost::program_options
  OpenSSL::SSL
  Threads::Threads
)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()


# Installation

install(TARGETS ${LOCAL_EXE_NAME} RUNTIME DESTINATION bin COMPONENT client)
