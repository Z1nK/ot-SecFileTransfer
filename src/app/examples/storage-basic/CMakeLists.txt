set(LOCAL_EXE_NAME example-storage-basic)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE main.cpp)

target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::storage-static
)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
