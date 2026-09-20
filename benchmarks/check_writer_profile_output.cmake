if(NOT DEFINED PROGRAM OR NOT DEFINED REQUIRED_REGEXES)
  message(FATAL_ERROR "PROGRAM and REQUIRED_REGEXES are required")
endif()

set(command "${PROGRAM}")
foreach(index RANGE 1 8)
  if(DEFINED CLI_ARGUMENT_${index})
    list(APPEND command "${CLI_ARGUMENT_${index}}")
  endif()
endforeach()

execute_process(
  COMMAND ${command}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "expected success, got '${result}'\n${stdout}\n${stderr}")
endif()

foreach(regex IN LISTS REQUIRED_REGEXES)
  if(NOT stdout MATCHES "${regex}")
    message(FATAL_ERROR "required output '${regex}' was not found\n${stdout}\n${stderr}")
  endif()
endforeach()

if(DEFINED FORBIDDEN_REGEXES)
  foreach(regex IN LISTS FORBIDDEN_REGEXES)
    if(stdout MATCHES "${regex}")
      message(FATAL_ERROR "forbidden output '${regex}' was found\n${stdout}\n${stderr}")
    endif()
  endforeach()
endif()
