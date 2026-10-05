set(LOCAL_LIB_NAME transaction)

add_library(${LOCAL_LIB_NAME} STATIC
  codec.cpp
  repository.cpp
  service.cpp
  state_machine.cpp
  transaction.cpp
)

add_library(${PROJECT_NAMESPACE}::transaction-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# service.hpp exposes FileStore, Upload/Download and BusinessLog, and the
# model uses common's id, path, digest and time types.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::common-static
  ${PROJECT_NAMESPACE}::storage-static
  ${PROJECT_NAMESPACE}::logging-static
)

# Boost.JSON stays inside codec.cpp; no public header includes it.
target_link_libraries(${LOCAL_LIB_NAME} PRIVATE
  Boost::json
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
