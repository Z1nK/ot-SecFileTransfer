set(LOCAL_EXE_NAME ftc)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE
  main.cpp
  dispatch.cpp
  local_files.cpp
  outgoing.cpp
  password.cpp
  reply.cpp
  session.cpp
  token_cache.cpp
  commands/auth.cpp
  commands/compose.cpp
  commands/get.cpp
  commands/query.cpp
  commands/send.cpp
)

# commands/ includes the headers next to main.cpp by their bare names.
target_include_directories(${LOCAL_EXE_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# net: the HTTP client (brings common along). common: ids, path checks,
# SHA-256. Boost.JSON for the replies, OpenSSL for OPENSSL_cleanse.
target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
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
