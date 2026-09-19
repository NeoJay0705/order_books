if(NOT DEFINED PROGRAM OR NOT DEFINED OUTPUT_PATH)
  message(FATAL_ERROR "PROGRAM and OUTPUT_PATH are required")
endif()

set(state_sampling on)
if(DEFINED STATE_SAMPLING)
  set(state_sampling "${STATE_SAMPLING}")
endif()
if(NOT state_sampling STREQUAL "on" AND NOT state_sampling STREQUAL "off")
  message(FATAL_ERROR "STATE_SAMPLING must be on or off")
endif()

file(REMOVE "${OUTPUT_PATH}")
set(state_sampling_argument "--engine-tail-state-sampling=${state_sampling}")
execute_process(
  COMMAND "${PROGRAM}"
          --workload=engine_durable_single_instrument
          --engine-tail-telemetry-output=${OUTPUT_PATH}
          "${state_sampling_argument}"
          --wal-prepare-workers=2
          --wal-parallel-prepare-min-commands=2
          --engine-group-size=4096
          --engine-group-delay-us=1000
          --engine-producer-lanes=2
          --iterations=4096
          --warmup=64
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
foreach(CLOCK_FIELD IN ITEMS
        tail_clock_start_realtime_epoch_ns
        tail_clock_end_realtime_epoch_ns
        tail_clock_end_steady_elapsed_ns)
  if(NOT STDOUT MATCHES "${CLOCK_FIELD}=[1-9][0-9]*")
    message(FATAL_ERROR "telemetry clock field ${CLOCK_FIELD} must be non-zero\nstdout:\n${STDOUT}")
  endif()
endforeach()
foreach(CLOCK_FIELD IN ITEMS
        tail_clock_start_uncertainty_ns
        tail_clock_end_uncertainty_ns)
  if(NOT STDOUT MATCHES "${CLOCK_FIELD}=[0-9]+")
    message(FATAL_ERROR "telemetry clock field ${CLOCK_FIELD} missing\nstdout:\n${STDOUT}")
  endif()
endforeach()
if(NOT STDOUT MATCHES "tail_state_sampling=${state_sampling}")
  message(FATAL_ERROR "state sampling mode missing\nstdout:\n${STDOUT}")
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
if(state_sampling STREQUAL "on")
  foreach(DRAIN_FIELD IN ITEMS events_first bytes_first age_ns_first events_last bytes_last age_ns_last)
    if(NOT STDOUT MATCHES "drain_publisher_lag_${DRAIN_FIELD}=[0-9]+")
      message(FATAL_ERROR "drain ${DRAIN_FIELD} missing\nstdout:\n${STDOUT}")
    endif()
  endforeach()
  string(REGEX MATCH "drain_state_sample_count=([0-9]+)"
         DRAIN_COUNT_MATCH "${STDOUT}")
  set(SUMMARY_DRAIN_COUNT "${CMAKE_MATCH_1}")
  if(NOT DRAIN_COUNT_MATCH)
    message(FATAL_ERROR "drain state sample count missing\nstdout:\n${STDOUT}")
  endif()
  if(SUMMARY_DRAIN_COUNT LESS 2)
    message(FATAL_ERROR "drain boundary samples missing\nstdout:\n${STDOUT}")
  endif()
else()
  string(REGEX MATCH "drain_state_sample_count=([0-9]+)"
         DRAIN_COUNT_MATCH "${STDOUT}")
  set(SUMMARY_DRAIN_COUNT "${CMAKE_MATCH_1}")
  if(NOT DRAIN_COUNT_MATCH OR NOT SUMMARY_DRAIN_COUNT EQUAL 0)
    message(FATAL_ERROR "collector-only drain state count is invalid\nstdout:\n${STDOUT}")
  endif()
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
set(DRAIN_COUNT 0)
foreach(ROW IN LISTS ARTIFACT_ROWS)
  if(ROW MATCHES "^sync,measured,[0-9]+,([0-9]+),,,,$")
    math(EXPR SYNC_COUNT "${SYNC_COUNT} + 1")
  elseif(ROW MATCHES "^group_commands,measured,[0-9]+,([0-9]+),,,,$")
    math(EXPR GROUP_COUNT "${GROUP_COUNT} + 1")
    math(EXPR GROUP_COMMANDS "${GROUP_COMMANDS} + ${CMAKE_MATCH_1}")
  elseif(ROW MATCHES "^state,measured,[0-9]+,,[0-9]+,[0-9]+,[0-9]+,[0-9]+$")
    math(EXPR STATE_COUNT "${STATE_COUNT} + 1")
  elseif(ROW MATCHES "^state,drain,([0-9]+),,([0-9]+),([0-9]+),([0-9]+),([0-9]+)$")
    if(DRAIN_COUNT EQUAL 0)
      set(CSV_DRAIN_EVENTS_FIRST "${CMAKE_MATCH_3}")
      set(CSV_DRAIN_BYTES_FIRST "${CMAKE_MATCH_4}")
      set(CSV_DRAIN_AGE_FIRST "${CMAKE_MATCH_5}")
    endif()
    set(CSV_DRAIN_EVENTS_LAST "${CMAKE_MATCH_3}")
    set(CSV_DRAIN_BYTES_LAST "${CMAKE_MATCH_4}")
    set(CSV_DRAIN_AGE_LAST "${CMAKE_MATCH_5}")
    math(EXPR DRAIN_COUNT "${DRAIN_COUNT} + 1")
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
if(NOT DRAIN_COUNT EQUAL SUMMARY_DRAIN_COUNT)
  message(FATAL_ERROR "drain row count ${DRAIN_COUNT} differs from summary ${SUMMARY_DRAIN_COUNT}")
endif()
if(state_sampling STREQUAL "on")
  if(STATE_COUNT LESS 3 OR DRAIN_COUNT LESS 2)
    message(FATAL_ERROR "measured and drain state telemetry rows are missing")
  endif()
  string(REGEX MATCH "drain_publisher_lag_events_first=([0-9]+)" _SUMMARY_EVENTS_FIRST "${STDOUT}")
  set(SUMMARY_EVENTS_FIRST "${CMAKE_MATCH_1}")
  string(REGEX MATCH "drain_publisher_lag_bytes_first=([0-9]+)" _SUMMARY_BYTES_FIRST "${STDOUT}")
  set(SUMMARY_BYTES_FIRST "${CMAKE_MATCH_1}")
  string(REGEX MATCH "drain_publisher_lag_age_ns_first=([0-9]+)" _SUMMARY_AGE_FIRST "${STDOUT}")
  set(SUMMARY_AGE_FIRST "${CMAKE_MATCH_1}")
  string(REGEX MATCH "drain_publisher_lag_events_last=([0-9]+)" _SUMMARY_EVENTS_LAST "${STDOUT}")
  set(SUMMARY_EVENTS_LAST "${CMAKE_MATCH_1}")
  string(REGEX MATCH "drain_publisher_lag_bytes_last=([0-9]+)" _SUMMARY_BYTES_LAST "${STDOUT}")
  set(SUMMARY_BYTES_LAST "${CMAKE_MATCH_1}")
  string(REGEX MATCH "drain_publisher_lag_age_ns_last=([0-9]+)" _SUMMARY_AGE_LAST "${STDOUT}")
  set(SUMMARY_AGE_LAST "${CMAKE_MATCH_1}")
  if(NOT _SUMMARY_EVENTS_FIRST OR NOT _SUMMARY_BYTES_FIRST OR NOT _SUMMARY_AGE_FIRST OR
     NOT _SUMMARY_EVENTS_LAST OR NOT _SUMMARY_BYTES_LAST OR NOT _SUMMARY_AGE_LAST)
    message(FATAL_ERROR "summary drain boundaries are invalid\nstdout:\n${STDOUT}")
  endif()
  if(NOT "${SUMMARY_EVENTS_FIRST}" STREQUAL "${CSV_DRAIN_EVENTS_FIRST}" OR
     NOT "${SUMMARY_BYTES_FIRST}" STREQUAL "${CSV_DRAIN_BYTES_FIRST}" OR
     NOT "${SUMMARY_AGE_FIRST}" STREQUAL "${CSV_DRAIN_AGE_FIRST}" OR
     NOT "${SUMMARY_EVENTS_LAST}" STREQUAL "${CSV_DRAIN_EVENTS_LAST}" OR
     NOT "${SUMMARY_BYTES_LAST}" STREQUAL "${CSV_DRAIN_BYTES_LAST}" OR
     NOT "${SUMMARY_AGE_LAST}" STREQUAL "${CSV_DRAIN_AGE_LAST}")
    message(FATAL_ERROR "summary drain boundaries differ from CSV")
  endif()
endif()
if(state_sampling STREQUAL "off" AND NOT STATE_COUNT EQUAL 0)
  message(FATAL_ERROR "collector-only telemetry unexpectedly contains state rows")
endif()

file(REMOVE "${OUTPUT_PATH}")
