if(NOT DEFINED PROGRAM OR NOT DEFINED CLI_ARGUMENT OR NOT DEFINED EXPECTED_ERROR)
  message(FATAL_ERROR "PROGRAM, CLI_ARGUMENT and EXPECTED_ERROR are required")
endif()

execute_process(
  COMMAND "${PROGRAM}" --workload=engine_durable_single_instrument "${CLI_ARGUMENT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT result EQUAL 2)
  message(FATAL_ERROR "expected exit code 2, got '${result}'\n${stdout}\n${stderr}")
endif()

string(FIND "${stdout}\n${stderr}" "${EXPECTED_ERROR}" error_offset)
if(error_offset EQUAL -1)
  message(FATAL_ERROR "expected error '${EXPECTED_ERROR}' was not reported\n${stdout}\n${stderr}")
endif()
