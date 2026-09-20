if(NOT DEFINED PROGRAM OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "PROGRAM and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/data")
set(existing_output "${WORK_DIR}/existing.csv")
file(WRITE "${existing_output}" "sentinel\n")

set(common_arguments
    --workload=engine_writer_hot_path_profile
    --iterations=2
    --warmup=1
    --engine-group-size=2
    --engine-group-delay-us=0
    --engine-producer-lanes=2
    --wal-prepare-workers=2
    --wal-parallel-prepare-min-commands=2
    --writer-phase-profile=on
    --writer-thread-diagnostics=on
    --data-dir=${WORK_DIR}/data)

execute_process(
  COMMAND "${PROGRAM}" ${common_arguments}
          --writer-tail-telemetry-output=${existing_output}
  RESULT_VARIABLE existing_result
  OUTPUT_VARIABLE existing_stdout
  ERROR_VARIABLE existing_stderr
)
if(existing_result EQUAL 0)
  message(FATAL_ERROR "existing telemetry output was unexpectedly accepted\n${existing_stdout}")
endif()
string(FIND "${existing_stdout}\n${existing_stderr}"
       "error_code=writer_tail_telemetry_output_unavailable" existing_error)
if(existing_error EQUAL -1)
  message(FATAL_ERROR "existing output error was not reported\n${existing_stdout}\n${existing_stderr}")
endif()
file(READ "${existing_output}" existing_contents)
if(NOT existing_contents STREQUAL "sentinel\n")
  message(FATAL_ERROR "existing telemetry output was modified")
endif()

set(dangling_output "${WORK_DIR}/dangling.csv")
file(CREATE_LINK "${WORK_DIR}/missing-target.csv" "${dangling_output}"
     SYMBOLIC RESULT link_result)
if(NOT link_result STREQUAL "0")
  message(FATAL_ERROR "failed to create dangling telemetry output: ${link_result}")
endif()
execute_process(
  COMMAND "${PROGRAM}" ${common_arguments}
          --writer-tail-telemetry-output=${dangling_output}
  RESULT_VARIABLE dangling_result
  OUTPUT_VARIABLE dangling_stdout
  ERROR_VARIABLE dangling_stderr
)
if(dangling_result EQUAL 0)
  message(FATAL_ERROR "dangling telemetry output was unexpectedly accepted\n${dangling_stdout}")
endif()
string(FIND "${dangling_stdout}\n${dangling_stderr}"
       "error_code=writer_tail_telemetry_output_unavailable" dangling_error)
if(dangling_error EQUAL -1)
  message(FATAL_ERROR "dangling output error was not reported\n${dangling_stdout}\n${dangling_stderr}")
endif()

set(missing_parent_output "${WORK_DIR}/missing-parent/tail.csv")
execute_process(
  COMMAND "${PROGRAM}" ${common_arguments}
          --writer-tail-telemetry-output=${missing_parent_output}
  RESULT_VARIABLE parent_result
  OUTPUT_VARIABLE parent_stdout
  ERROR_VARIABLE parent_stderr
)
if(parent_result EQUAL 0)
  message(FATAL_ERROR "missing telemetry parent was unexpectedly accepted\n${parent_stdout}")
endif()
string(FIND "${parent_stdout}\n${parent_stderr}"
       "error_code=writer_tail_telemetry_output_unavailable" parent_error)
if(parent_error EQUAL -1)
  message(FATAL_ERROR "missing parent error was not reported\n${parent_stdout}\n${parent_stderr}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
