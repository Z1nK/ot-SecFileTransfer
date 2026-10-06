set(LOCAL_LIB_NAME api)

add_library(${LOCAL_LIB_NAME} STATIC
  handlers.cpp
  http.cpp
  json.cpp
  router.cpp
  server.cpp
)

add_library(${PROJECT_NAMESPACE}::api-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# Public headers use Beast/Asio (header-only, Boost::headers), the
# asio::ssl::context (OpenSSL::SSL), TransactionService, TechLog and Config.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::transaction-static
  ${PROJECT_NAMESPACE}::config-static
  ${PROJECT_NAMESPACE}::logging-static
  Boost::headers
  OpenSSL::SSL
  Threads::Threads
)

# Boost.JSON stays inside http.cpp and json.cpp; no public header includes it.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  Boost::json
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
