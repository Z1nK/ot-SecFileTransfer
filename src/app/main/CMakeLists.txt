set(LOCAL_EXE_NAME example)
add_executable(${LOCAL_EXE_NAME})

target_sources(${LOCAL_EXE_NAME} PRIVATE main.cpp)

target_link_libraries(${LOCAL_EXE_NAME} PRIVATE
)

if (MSVC)
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE /W4)
else()
  target_compile_options(${LOCAL_EXE_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()


# Installation

install(TARGETS ${LOCAL_EXE_NAME} RUNTIME DESTINATION bin)
