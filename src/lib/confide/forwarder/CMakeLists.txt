set(LOCAL_LIB_NAME forwarder)

add_library(${LOCAL_LIB_NAME} STATIC
  forwarder.cpp
)

add_library(${PROJECT_NAMESPACE}::forwarder-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# forwarder.hpp holds a TransactionService, a FileStore, a TechLog and
# net::HttpClient; logging brings Threads along.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::transaction-static
  ${PROJECT_NAMESPACE}::storage-static
  ${PROJECT_NAMESPACE}::logging-static
  ${PROJECT_NAMESPACE}::net-static
)

# api only for encode_receive(), so the push body is the one the peer's
# decode_receive() reads; Boost.JSON for the peer's answer.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::api-static
  Boost::json
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
