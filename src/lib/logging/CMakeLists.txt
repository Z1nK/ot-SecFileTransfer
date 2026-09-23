set(LOCAL_LIB_NAME logging)

find_package(Threads REQUIRED)

add_library(${LOCAL_LIB_NAME} STATIC
  tech_log.cpp
)

add_library(${PROJECT_NAMESPACE}::logging-static ALIAS ${LOCAL_LIB_NAME})

set_property(TARGET ${LOCAL_LIB_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${LOCAL_LIB_NAME} PUBLIC
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
  )

# std::thread is part of the public header (TechLog member), so consumers
# need the thread library as well.
target_link_libraries(${LOCAL_LIB_NAME} PUBLIC
  Threads::Threads
)

if (MSVC)
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_LIB_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
