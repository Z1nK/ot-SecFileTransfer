set(LOCAL_LIB_NAME net)

add_library(${LOCAL_LIB_NAME} STATIC
  http_client.cpp
  url.cpp
)

# HttpClient internals; private headers live next to them.
target_sources(${LOCAL_LIB_NAME} PRIVATE
  client/io.cpp
)

add_library(${PROJECT_NAMESPACE}::net-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# http_client.hpp uses Result, Sha256Digest and beast::http::verb.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::common-static
  Boost::headers
)

# Asio/Beast with TLS and Boost.JSON stay inside http_client.cpp.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  Boost::json
  OpenSSL::SSL
  Threads::Threads
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
