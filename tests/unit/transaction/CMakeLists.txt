confide_add_unit_tests(transaction_tests
  SOURCES
    codec_test.cpp
    state_machine_test.cpp
    transaction_test.cpp
  LIBS
    ${PROJECT_NAMESPACE}::transaction-static
)
