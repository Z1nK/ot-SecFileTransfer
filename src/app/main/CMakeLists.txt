set(LOCAL_EXE_NAME confide-server)

# main.cpp needs TransactionService and TransactionRepository, which are
# only declared so far; skip the server until they are implemented, so the
# rest of the project still builds.
set(TRANSACTION_DIR ${PROJECT_SOURCE_DIR}/src/lib/transaction)
if(NOT EXISTS ${TRANSACTION_DIR}/service.cpp OR NOT EXISTS ${TRANSACTION_DIR}/repository.cpp)
  message(STATUS "${LOCAL_EXE_NAME}: skipped until transaction/service.cpp and repository.cpp exist")
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
