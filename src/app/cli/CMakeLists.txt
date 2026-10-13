set(LOCAL_EXE_NAME ftc)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE
  http_client.cpp
  main.cpp
)

# common: ids, path checks, SHA-256; auth: base64 for Basic credentials.
# Beast/Asio are header-only (Boost::headers), TLS needs OpenSSL::SSL.
target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::common-static
  ${PROJECT_NAMESPACE}::auth-static
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

install(TARGETS ${LOCAL_EXE_NAME} RUNTIME DESTINATION bin)
