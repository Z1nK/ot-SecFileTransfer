set(LOCAL_EXE_NAME example-config-read)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE main.cpp)

target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::config-static
)

# Put the test config next to the executable so the example runs from bin/.
# Depends on the .toml itself, so editing it re-copies without a relink.
set(TEST_CONFIG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_config.toml)
set(TEST_CONFIG_DST ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_config.toml)
add_custom_command(
  OUTPUT ${TEST_CONFIG_DST}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEST_CONFIG_SRC} ${TEST_CONFIG_DST}
  DEPENDS ${TEST_CONFIG_SRC}
  COMMENT "Copying test_config.toml to bin"
)
add_custom_target(${LOCAL_EXE_NAME}-config ALL DEPENDS ${TEST_CONFIG_DST})
add_dependencies(${LOCAL_EXE_NAME} ${LOCAL_EXE_NAME}-config)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
