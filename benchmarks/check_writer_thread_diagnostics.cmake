if(NOT DEFINED PROGRAM OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "PROGRAM and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/data")
execute_process(
  COMMAND "${PROGRAM}"
          --workload=engine_writer_hot_path_profile
          --iterations=4
          --warmup=1
          --engine-group-size=2
          --engine-group-delay-us=0
          --engine-producer-lanes=2
          --wal-prepare-workers=2
          --wal-parallel-prepare-min-commands=2
          --writer-phase-profile=on
          --writer-profile-sample-every=1
          --writer-thread-diagnostics=on
          --data-dir=${WORK_DIR}/data
          --writer-tail-telemetry-output=${WORK_DIR}/writer-tail.csv
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "writer diagnostics smoke failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
endif()
if(NOT EXISTS "${WORK_DIR}/writer-tail.csv")
  message(FATAL_ERROR "writer diagnostics smoke did not create telemetry CSV")
endif()
file(READ "${WORK_DIR}/writer-tail.csv" csv)
string(FIND "${csv}" "record_type,phase,elapsed_us,value" header_position)
if(header_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics telemetry CSV header is missing")
endif()
string(FIND "${csv}" "sync,measured," sync_position)
if(sync_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics telemetry CSV has no sync records")
endif()
string(FIND "${csv}" "group_commands,measured," group_commands_position)
if(group_commands_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics telemetry CSV has no group command records")
endif()
string(FIND "${output}" "role=ob-wr-1" writer_position)
if(writer_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics output has no writer role\n${output}")
endif()
string(FIND "${output}" "writer_tail_telemetry=on" tail_position)
if(tail_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics output has no tail summary\n${output}")
endif()
string(FIND "${output}" "migrations_per_million=" migration_position)
if(migration_position EQUAL -1)
  message(FATAL_ERROR "writer diagnostics output has no migration normalization\n${output}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
