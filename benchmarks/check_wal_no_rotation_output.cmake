if(NOT DEFINED PROGRAM)
  message(FATAL_ERROR "PROGRAM is required")
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
  message(FATAL_ERROR "no-rotation smoke failed: ${result}\n${stdout}\n${stderr}")
endif()

if(NOT stdout MATCHES "(^|[\n ])wal_append_no_rotation([ \n])")
  message(FATAL_ERROR "no-rotation output name is missing\n${stdout}")
endif()
foreach(field IN ITEMS measured_rusage_valid measured_io_valid measured_meminfo_valid
                     measured_user_seconds measured_system_seconds
                     measured_voluntary_context_switches measured_involuntary_context_switches
                     measured_syscw measured_wchar measured_write_bytes
                     measured_cancelled_write_bytes measured_wal_write_calls
                     measured_wal_sync_calls
                     measured_dirty_bytes_before measured_dirty_bytes_after
                     measured_writeback_bytes_before measured_writeback_bytes_after)
  string(REGEX MATCHALL "(^|[ \n])${field}=[^ \n]+" matches "${stdout}")
  list(LENGTH matches count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "field ${field} must occur once, got ${count}\n${stdout}")
  endif()
  if(stdout MATCHES "(^|[ \n])${field}=na([ \n])")
    message(FATAL_ERROR "field ${field} cannot be na\n${stdout}")
  endif()
endforeach()
foreach(field IN ITEMS measured_rusage_valid measured_io_valid measured_meminfo_valid)
  if(NOT stdout MATCHES "(^|[ \n])${field}=true([ \n])")
    message(FATAL_ERROR "field ${field} must be true\n${stdout}")
  endif()
endforeach()
foreach(field IN ITEMS measured_segment_rotations rotation_triggered replay_verified
                     wal_byte_plan_verified)
  if(NOT stdout MATCHES "(^|[ \n])${field}=(0|false|true)([ \n])")
    message(FATAL_ERROR "field ${field} is missing\n${stdout}")
  endif()
endforeach()
if(NOT stdout MATCHES "(^|[ \n])measured_segment_rotations=0([ \n])" OR
   NOT stdout MATCHES "(^|[ \n])rotation_triggered=false([ \n])" OR
   NOT stdout MATCHES "(^|[ \n])replay_verified=true([ \n])" OR
   NOT stdout MATCHES "(^|[ \n])wal_byte_plan_verified=true([ \n])")
  message(FATAL_ERROR "no-rotation semantic gate failed\n${stdout}")
endif()

string(REGEX MATCH "(^|[ \n])wal_bytes_delta=([0-9]+)" _actual "${stdout}")
set(actual "${CMAKE_MATCH_2}")
string(REGEX MATCH "(^|[ \n])planned_wal_bytes_delta=([0-9]+)" _planned "${stdout}")
set(planned "${CMAKE_MATCH_2}")
if(NOT _actual OR NOT _planned OR NOT actual STREQUAL planned)
  message(FATAL_ERROR "WAL byte plan does not match actual bytes\n${stdout}")
endif()

string(REGEX MATCH "(^|[ \n])measured_syscw=([0-9]+)" _syscw "${stdout}")
set(syscw "${CMAKE_MATCH_2}")
string(REGEX MATCH "(^|[ \n])measured_wal_write_calls=([0-9]+)" _wal_writes "${stdout}")
set(wal_writes "${CMAKE_MATCH_2}")
if(NOT _syscw OR NOT _wal_writes OR NOT "${syscw}" STREQUAL "${wal_writes}")
  message(FATAL_ERROR "measured WAL write calls do not match measured syscw\n${stdout}")
endif()
