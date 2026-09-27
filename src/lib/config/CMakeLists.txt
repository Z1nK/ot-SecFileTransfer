set(LOCAL_LIB_NAME config)

add_library(${LOCAL_LIB_NAME} STATIC
  config.cpp
  parser.cpp
)

add_library(${PROJECT_NAMESPACE}::config-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# config.hpp includes common/error.hpp and logging/tech_log.hpp; toml11 is
# only used inside config.cpp.
target_link_libraries(${LOCAL_LIB_NAME}
  PUBLIC
    ${PROJECT_NAMESPACE}::common-static
    ${PROJECT_NAMESPACE}::logging-static
  PRIVATE
    toml11::toml11
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()