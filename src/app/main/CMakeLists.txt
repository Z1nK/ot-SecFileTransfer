set(LOCAL_EXE_NAME confide-server)

# main.cpp wires in the auth module; skip the server until it exists, so the
# rest of the project still configures and builds.
if(NOT TARGET ${PROJECT_NAMESPACE}::auth-static)
  message(STATUS "${LOCAL_EXE_NAME}: skipped, ${PROJECT_NAMESPACE}::auth-static not found")
  return()
endif()

add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE main.cpp)

# api brings transaction, storage, config, logging and common along.
target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::api-static
  ${PROJECT_NAMESPACE}::auth-static
)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()


# Installation

install(TARGETS ${LOCAL_EXE_NAME} RUNTIME DESTINATION bin)
