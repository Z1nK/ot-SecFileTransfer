set(LOCAL_EXE_NAME ftc)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE
  token_cache.cpp
  main.cpp
)

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
