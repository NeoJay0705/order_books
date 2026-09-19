if(NOT DEFINED PROGRAM OR NOT DEFINED OUTPUT_PATH)
  message(FATAL_ERROR "PROGRAM and OUTPUT_PATH are required")
endif()

file(REMOVE "${OUTPUT_PATH}")
execute_process(
  COMMAND "${PROGRAM}"
          --workload=engine_durable_single_instrument
          --engine-tail-telemetry-output=${OUTPUT_PATH}
          --wal-prepare-workers=2
          --wal-parallel-prepare-min-commands=2
          --engine-group-size=4096
          --engine-group-delay-us=1000
          --engine-producer-lanes=2
          --iterations=64
          --warmup=2
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)

if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "telemetry benchmark failed: ${RESULT}\nstdout:\n${STDOUT}\nstderr:\n${STDERR}")
endif()

if(NOT STDOUT MATCHES "tail_telemetry=on")
  message(FATAL_ERROR "telemetry summary missing\nstdout:\n${STDOUT}")
endif()
if(NOT STDOUT MATCHES "measured_sync_count=[1-9][0-9]*")
  message(FATAL_ERROR "measured sync count missing\nstdout:\n${STDOUT}")
endif()
foreach(PERCENTILE IN ITEMS p50 p99 max)
  if(NOT STDOUT MATCHES "measured_sync_${PERCENTILE}_us=[0-9]+")
    message(FATAL_ERROR "measured sync ${PERCENTILE} missing\nstdout:\n${STDOUT}")
  endif()
endforeach()
if(NOT STDOUT MATCHES "measured_sync_p99.9_us=[0-9]+")
  message(FATAL_ERROR "measured sync p99.9 missing\nstdout:\n${STDOUT}")
endif()
if(NOT STDOUT MATCHES "telemetry_dropped_samples=0")
  message(FATAL_ERROR "telemetry sample drop reported\nstdout:\n${STDOUT}")
endif()

if(NOT EXISTS "${OUTPUT_PATH}")
  message(FATAL_ERROR "telemetry artifact was not created")
endif()

file(STRINGS "${OUTPUT_PATH}" ARTIFACT_ROWS)
list(LENGTH ARTIFACT_ROWS ARTIFACT_ROW_COUNT)
if(ARTIFACT_ROW_COUNT LESS 1)
  message(FATAL_ERROR "telemetry artifact is empty")
endif()
list(GET ARTIFACT_ROWS 0 HEADER)
if(NOT HEADER STREQUAL "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns")
  message(FATAL_ERROR "telemetry artifact header is invalid")
endif()
list(REMOVE_AT ARTIFACT_ROWS 0)

string(REGEX MATCH "measured_sync_count=([0-9]+)" SUMMARY_SYNC_MATCH "${STDOUT}")
set(SUMMARY_SYNC_COUNT "${CMAKE_MATCH_1}")
string(REGEX MATCH "measured_group_sample_count=([0-9]+)" SUMMARY_GROUP_MATCH "${STDOUT}")
set(SUMMARY_GROUP_COUNT "${CMAKE_MATCH_1}")
string(REGEX MATCH "measured_group_sample_commands=([0-9]+)" SUMMARY_COMMAND_MATCH "${STDOUT}")
set(SUMMARY_GROUP_COMMANDS "${CMAKE_MATCH_1}")
if(NOT SUMMARY_SYNC_MATCH OR NOT SUMMARY_GROUP_MATCH OR NOT SUMMARY_COMMAND_MATCH)
  message(FATAL_ERROR "telemetry aggregate summary is incomplete\nstdout:\n${STDOUT}")
endif()

set(SYNC_COUNT 0)
set(GROUP_COUNT 0)
set(GROUP_COMMANDS 0)
set(STATE_COUNT 0)
foreach(ROW IN LISTS ARTIFACT_ROWS)
  if(ROW MATCHES "^sync,measured,[0-9]+,([0-9]+),,,,$")
    math(EXPR SYNC_COUNT "${SYNC_COUNT} + 1")
  elseif(ROW MATCHES "^group_commands,measured,[0-9]+,([0-9]+),,,,$")
    math(EXPR GROUP_COUNT "${GROUP_COUNT} + 1")
    math(EXPR GROUP_COMMANDS "${GROUP_COMMANDS} + ${CMAKE_MATCH_1}")
  elseif(ROW MATCHES "^state,(measured|drain),[0-9]+,,[0-9]+,[0-9]+,[0-9]+,[0-9]+$")
    math(EXPR STATE_COUNT "${STATE_COUNT} + 1")
  else()
    message(FATAL_ERROR "malformed telemetry row: '${ROW}'")
  endif()
endforeach()

if(NOT SYNC_COUNT EQUAL SUMMARY_SYNC_COUNT)
  message(FATAL_ERROR "sync row count ${SYNC_COUNT} differs from summary ${SUMMARY_SYNC_COUNT}")
endif()
if(NOT GROUP_COUNT EQUAL SUMMARY_GROUP_COUNT)
  message(FATAL_ERROR "group row count ${GROUP_COUNT} differs from summary ${SUMMARY_GROUP_COUNT}")
endif()
if(NOT GROUP_COMMANDS EQUAL SUMMARY_GROUP_COMMANDS)
  message(FATAL_ERROR "group command sum ${GROUP_COMMANDS} differs from summary ${SUMMARY_GROUP_COMMANDS}")
endif()
if(STATE_COUNT LESS 1)
  message(FATAL_ERROR "state telemetry row is missing")
endif()

file(REMOVE "${OUTPUT_PATH}")
