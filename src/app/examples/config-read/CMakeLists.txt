set(LOCAL_EXE_NAME example-config-read)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE main.cpp)

target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
  ${PROJECT_NAMESPACE}::config-static
)

# Put the test configs (and the users/groups files the local one points at)
# next to the executable so the example runs from bin/. Each output depends
# on its source, so editing one re-copies it without a relink.
set(TEST_CONFIG_FILES test_config.toml test_config_ldap.toml users groups)
set(TEST_CONFIG_DSTS)
foreach(f IN LISTS TEST_CONFIG_FILES)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/${f})
  set(dst ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${f})
  add_custom_command(
    OUTPUT ${dst}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} ${dst}
    DEPENDS ${src}
    COMMENT "Copying ${f} to bin"
  )
  list(APPEND TEST_CONFIG_DSTS ${dst})
endforeach()
add_custom_target(${LOCAL_EXE_NAME}-config ALL DEPENDS ${TEST_CONFIG_DSTS})
add_dependencies(${LOCAL_EXE_NAME} ${LOCAL_EXE_NAME}-config)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
