set(LOCAL_LIB_NAME common)

# Header-only for now; switch back to STATIC once it gets .cpp files.
add_library(${LOCAL_LIB_NAME} INTERFACE)

add_library(${PROJECT_NAMESPACE}::common-static ALIAS ${LOCAL_LIB_NAME})

target_include_directories(${LOCAL_LIB_NAME} INTERFACE
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )
