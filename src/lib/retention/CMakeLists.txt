set(LOCAL_LIB_NAME retention)

add_library(${LOCAL_LIB_NAME} STATIC
  scheduler.cpp
)

add_library(${PROJECT_NAMESPACE}::retention-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# scheduler.hpp holds a TransactionService and a TechLog reference;
# logging brings Threads along.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  ${PROJECT_NAMESPACE}::transaction-static
  ${PROJECT_NAMESPACE}::logging-static
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
