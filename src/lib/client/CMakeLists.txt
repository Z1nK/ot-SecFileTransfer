set(LOCAL_LIB_NAME client)

# Client side of the protocol, shared by every front end (ftc, a GUI, ...):
# no terminal I/O and no command line parsing here.
add_library(${LOCAL_LIB_NAME} STATIC
  local_files.cpp
  outgoing.cpp
  reply.cpp
  token_cache.cpp
)

add_library(${PROJECT_NAMESPACE}::client-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

# Included as <client/...>, next to the server libraries' <common/...> etc.
target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# net: the HTTP client (brings common along). Boost.JSON is in reply.hpp
# and outgoing.hpp.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::net-static
  ${PROJECT_NAMESPACE}::common-static
  Boost::headers
  Boost::json
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
