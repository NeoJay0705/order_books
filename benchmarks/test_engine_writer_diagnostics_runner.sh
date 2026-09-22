#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RUNNER="$SCRIPT_DIR/run_engine_writer_diagnostics.sh"
REAL_GIT=$(command -v git)
REPO_ROOT=$(git rev-parse --show-toplevel)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/engine-writer-runner-test-XXXXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT

assert_status() {
  local expected=$1
  local actual=$2
  [[ "$actual" == "$expected" ]] || {
    printf 'expected exit status %s, got %s\n' "$expected" "$actual" >&2
    return 1
  }
}

write_fake_commands() {
  local fake_bin=$1
  local fake_state=$2
  mkdir -p "$fake_bin" "$fake_state"
  printf '%s\n' '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    'name=$(basename "$0")' \
    'scenario=${ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO:-normal}' \
    'state=${FAKE_STATE:?}' \
    'real_git=${REAL_GIT:?}' \
    'log="$state/invocations.log"' \
    'case "$name" in' \
    '  git)' \
    '    if [[ "${1:-}" == status ]]; then' \
    '      "$real_git" "$@"' \
    '      if [[ "$scenario" == identity_change && -f "$state/identity-marker" ]]; then printf " M fake-identity-change\\n"; fi' \
    '    else exec "$real_git" "$@"; fi' \
    '    ;;' \
    '  taskset)' \
    '    if [[ "${1:-}" == -c ]]; then shift 2; fi' \
    '    exec "$@"' \
    '    ;;' \
    '  timeout)' \
    '    shift' \
    '    if [[ "$scenario" == benchmark_timeout && "${1:-}" == *order_books_benchmark ]]; then exit 124; fi' \
    '    exec "$@"' \
    '    ;;' \
    '  mpstat)' \
    '    if [[ " $* " == *" -o JSON "* ]]; then printf "%s\\n" '\''{"sysstat":{"hosts":[{"statistics":[{"cpu-load":[{"cpu":2,"idle":95,"iowait":0}]}]}]}}'\''; exit 0; fi' \
    '    printf "mpstat monitor\\n"; if [[ "$scenario" == observer_early_exit ]]; then exit 7; fi' \
    '    trap '\''exit 0'\'' TERM INT; while true; do read -r -t 1 _ || true; done' \
    '    ;;' \
    '  iostat)' \
    '    if [[ " $* " == *" -o JSON "* ]]; then printf "%s\\n" '\''{"sysstat":{"hosts":[{"statistics":[{"disk":[{"disk_device":"vdb","util":0,"aqu-sz":0}]}]}]}}'\''; exit 0; fi' \
    '    printf "iostat monitor\\n"; if [[ "$scenario" == observer_early_exit ]]; then exit 7; fi' \
    '    trap '\''exit 0'\'' TERM INT; while true; do read -r -t 1 _ || true; done' \
    '    ;;' \
    '  findmnt)' \
    '    if [[ "$*" == *"-no SOURCE"* ]]; then printf "/dev/testwal\\n"; else printf "/dev/testwal / testfs rw\\n"; fi' \
    '    ;;' \
    '  lsblk)' \
    '    printf "vdb\\n"' \
    '    ;;' \
    '  lscpu)' \
    '    printf "Architecture: x86_64\\nCPU(s): 8\\n"' \
    '    ;;' \
    '  jq)' \
    '    filter=${2:-}; if [[ "$scenario" == preflight_failure && "$filter" == *idle* ]]; then printf "50\\n"' \
    '    elif [[ "$filter" == *idle* ]]; then printf "95\\n"; elif [[ "$filter" == *iowait* ]]; then printf "0\\n"' \
    '    elif [[ "$filter" == *util* || "$filter" == *aqu-sz* ]]; then printf "0\\n"; else printf "0\\n"; fi' \
    '    ;;' \
    '  perf)' \
    '    sub=${1:-}; shift' \
    '    if [[ "$sub" == stat ]]; then' \
    '      if [[ "$scenario" == perf_unsupported ]]; then printf "<not supported>\\n" >&2; exit 0; fi' \
    '      output=""; events=""; while (($#)); do case "$1" in -o) output=$2; shift 2 ;; -e) events=$2; shift 2 ;; --) shift; break ;; *) shift ;; esac; done' \
    '      expected_events="task-clock,cycles,instructions,cache-misses,context-switches,cpu-migrations,page-faults"; [[ "$events" == "$expected_events" ]] || exit 91' \
    '      printf "%s\\n" "perf-stat events=$events" >>"$state/perf.log"; [[ -z "$output" ]] || printf "0,task-clock,1,1,1\\n" >"$output"; exec "$@"' \
    '    elif [[ "$sub" == record ]]; then' \
    '      [[ "$scenario" != perf_record_failure && "$scenario" != perf_call_graph_failure ]] || exit 1; output=""; while (($#)); do case "$1" in -o) output=$2; shift 2 ;; --) shift; break ;; *) shift ;; esac; done' \
    '      printf "fake perf data\\n" >"$output"; exec "$@"' \
    '    elif [[ "$sub" == report ]]; then printf "fake perf report\\n"; else exit 1; fi' \
    '    ;;' \
    '  fio)' \
    '    output=""; for arg in "$@"; do [[ "$arg" == --output=* ]] && output=${arg#*=}; done; [[ -z "$output" ]] || printf "{}\\n" >"$output"' \
    '    ;;' \
    '  order_books_benchmark)' \
    '    count_file="$state/benchmark-count"; count=0; [[ -s "$count_file" ]] && count=$(<"$count_file"); count=$((count + 1)); printf "%s\\n" "$count" >"$count_file"' \
    '    spin_iterations=20000; for ((spin=0; spin<spin_iterations; ++spin)); do :; done' \
    '    printf "%s label=%s %s\\n" "$name" "${RUNNER_CASE_LABEL:-unknown}" "$*" >>"$log"; [[ "$scenario" != benchmark_nonzero || "$count" != 1 ]] || exit 42' \
    '    iterations=10; workload=""; group=256; pipeline_batch=2; pipeline_scenario=new_crossing_pair; wal_sync=none; phase_profile=off; no_rotation_epoch=0; rotation_diagnostic=""; tail_output=""; for arg in "$@"; do case "$arg" in --iterations=*) iterations=${arg#*=} ;; --workload=*) workload=${arg#*=} ;; --engine-group-size=*) group=${arg#*=} ;; --pipeline-batch-size=*) pipeline_batch=${arg#*=} ;; --pipeline-command-scenario=*) pipeline_scenario=${arg#*=} ;; --wal-group-size=*) group=${arg#*=} ;; --wal-sync=*) wal_sync=${arg#*=} ;; --wal-phase-profile=*) phase_profile=${arg#*=} ;; --wal-no-rotation-epoch-commands=*) no_rotation_epoch=${arg#*=} ;; --wal-rotation-diagnostic=*) rotation_diagnostic=${arg#*=} ;; --engine-tail-telemetry-output=*) tail_output=${arg#*=} ;; esac; done' \
    '    : "${RUNNER_CASE_LABEL:?}"' \
    '    [[ "$scenario" != identity_change ]] || : >"$state/identity-marker"; commands=$((iterations * 2)); elapsed=42; [[ "$RUNNER_CASE_LABEL" != calibration-* ]] || elapsed=1; rps=100000; if [[ "$scenario" == component_profile_bias_2pct ]]; then rps=1e+06; elif [[ "$scenario" == component_rounding_probe ]]; then if [[ "$workload" == engine_pipeline_ceiling ]]; then rps=100001; else rps=81921; fi; elif [[ "$scenario" == component_append_cv_rejected && "$RUNNER_CASE_LABEL" == append-r*-b1 ]]; then case "$RUNNER_CASE_LABEL" in *-r2-*) rps=120000 ;; *) rps=100000 ;; esac; fi' \
    '    if [[ "$workload" == engine_pipeline_ceiling ]]; then' \
    '      commands=$((iterations * pipeline_batch)); trades=0; events=$commands; active_orders=0; [[ "$pipeline_scenario" != new_crossing_pair ]] || { trades=$((commands / 2)); events=$((commands * 2)); }' \
    '      [[ "$pipeline_scenario" != amend_quantity && "$pipeline_scenario" != replace_order ]] || active_orders=1' \
    '      printf "engine_pipeline_ceiling stage=state_machine scenario=%s measured_commands=%s commands_per_second=%s average_ns_per_command=10 elapsed_ms=%s trades=%s events=%s active_orders=%s active_levels=%s last_engine_seq=%s completion_boundary=state_apply_return latency_scope=run_average correctness_verified=true\n" "$pipeline_scenario" "$commands" "$rps" "$elapsed" "$trades" "$events" "$active_orders" "$active_orders" "$commands"' \
    '    elif [[ "$workload" == wal_write_ceiling ]]; then' \
    '      exec 3>&1; wal_output_file="$state/wal-output-$$"; exec >"$wal_output_file"' \
    '      output_name=wal_write_ceiling; [[ "$no_rotation_epoch" == 0 ]] || output_name=wal_append_no_rotation; [[ -z "$rotation_diagnostic" ]] || output_name=wal_rotation_diagnostic' \
    '      commands=$((iterations * group)); output_commands=$commands; reported_groups=$iterations; service_rps=$rps; if [[ "$phase_profile" == on && "$scenario" == component_profile_bias ]]; then service_rps=104000; elif [[ "$phase_profile" == on && "$scenario" == component_profile_bias_2pct ]]; then service_rps=1.02e+06; fi; sync_samples=0; [[ "$wal_sync" != per_group ]] || sync_samples=$iterations; [[ "$scenario" != component_sync_counter_mismatch || "$wal_sync" != per_group || "$RUNNER_CASE_LABEL" != fsync-r1-b1 ]] || sync_samples=$((iterations - 1)); [[ "$scenario" != component_sync_counter_overflow || "$wal_sync" != per_group || "$RUNNER_CASE_LABEL" != fsync-r1-b1 ]] || sync_samples=$((iterations + 1)); profiled_groups=$iterations; profiled_commands=$commands; profiled_data_write_calls=$iterations; [[ "$scenario" != component_profile_counter_mismatch ]] || profiled_commands=$((commands - 1)); [[ "$scenario" != component_profile_groups_mismatch || "$phase_profile" != on ]] || profiled_groups=$((iterations - 1)); [[ "$scenario" != component_group_counter_mismatch || "$RUNNER_CASE_LABEL" != append-r1-b1 ]] || reported_groups=$((iterations - 1)); [[ "$scenario" != component_malformed_counter || "$RUNNER_CASE_LABEL" != append-r1-b1 ]] || output_commands=nan; profile_fields=""; [[ "$phase_profile" != on || "$scenario" == component_profile_missing_field ]] || profile_fields=" profiled_groups=$profiled_groups profiled_commands=$profiled_commands profiled_data_write_calls=$profiled_data_write_calls wal_prepare_group_total_us=1 wal_plan_copy_group_total_us=1 wal_write_group_total_us=1 wal_publish_group_total_us=1 wal_rotation_group_total_us=1 rotation_sync_total_us=1 rotation_header_write_total_us=1 rotation_header_sync_total_us=1 rotation_directory_sync_total_us=1"' \
    '      rotation_scope=rotation_inclusive; [[ "$no_rotation_epoch" == 0 ]] || rotation_scope=no_rotation; rotation_count=0; [[ "$rotation_diagnostic" != trigger ]] || rotation_count=1; [[ "$scenario" != component_rotation_control_nonzero || "$rotation_diagnostic" != control ]] || rotation_count=1; [[ "$scenario" != component_rotation_trigger_zero || "$rotation_diagnostic" != trigger ]] || rotation_count=0; [[ "$scenario" != component_rotation_trigger_multiple || "$rotation_diagnostic" != trigger ]] || rotation_count=2; rotation_triggered=false; [[ "$rotation_count" != 0 ]] && rotation_triggered=true; measured_syscw=$reported_groups; [[ "$rotation_count" == 0 ]] || measured_syscw=1; planned_bytes=$((commands * 100)); [[ "$rotation_diagnostic" != trigger ]] || planned_bytes=$((planned_bytes + 22)); actual_bytes=$planned_bytes; [[ "$scenario" != component_byte_plan_mismatch ]] || actual_bytes=$((actual_bytes + 1)); diagnostic_case=$rotation_diagnostic; [[ "$scenario" != component_rotation_case_mismatch ]] || { [[ "$diagnostic_case" == control ]] && diagnostic_case=trigger || diagnostic_case=control; }; diagnostic_after_id=1; diagnostic_after_offset=122; [[ "$scenario" != component_rotation_position_mismatch ]] || diagnostic_after_offset=123; [[ "$scenario" != component_wrong_no_rotation_name || "$no_rotation_epoch" == 0 ]] || output_name=wal_write_ceiling; [[ "$scenario" != component_rotation_multiple_groups || -z "$rotation_diagnostic" ]] || { reported_groups=2; profiled_groups=2; }; printf "%s case=%s sync_mode=%s completion_boundary=%s groups=%s group_size=%s commands=%s commands_per_second=%s workload_wall_commands_per_second=%s service_commands_per_second=%s target_commands_per_second=1000000 target_attainment_percent=1 workload_wall_target_attainment_percent=1 service_target_attainment_percent=1 wal_mib_per_second=1 workload_wall_wal_mib_per_second=1 service_wal_mib_per_second=1 average_wal_bytes_per_command=100 latency_sample_stride=1 append_latency_sample_count=%s append_group_p50_us=1 append_group_p99_us=1 append_group_p99.9_us=1 append_group_max_us=1 sync_samples=%s sync_latency_sample_count=%s sync_p50_us=1 sync_p99_us=1 sync_p99.9_us=1 sync_max_us=1 group_total_latency_sample_count=%s group_total_p50_us=1 group_total_p99_us=1 group_total_p99.9_us=1 group_total_max_us=1 elapsed_ms=%s workload_wall_elapsed_ms=%s service_elapsed_ms=%s wal_bytes_delta=%s planned_wal_bytes_delta=%s wal_byte_plan_verified=true segment_count=1 measured_segment_rotations=%s rotation_triggered=%s rotation_scope=%s segment_id_before=1 segment_id_after=%s target_sequence=1 segment_offset_before=22 segment_offset_after=%s segment_header_bytes=22 frame_bytes_per_command=100 measured_rusage_valid=true measured_io_valid=true measured_meminfo_valid=true measured_user_seconds=1 measured_system_seconds=1 measured_voluntary_context_switches=1 measured_involuntary_context_switches=1 measured_syscw=%s measured_wchar=1 measured_write_bytes=1 measured_cancelled_write_bytes=0 measured_dirty_bytes_before=1 measured_dirty_bytes_after=2 measured_writeback_bytes_before=3 measured_writeback_bytes_after=4 measured_wal_write_calls=%s measured_wal_sync_calls=%s replay_verified=true phase_profile=%s%s\n" "$output_name" "$diagnostic_case" "$wal_sync" "$([[ "$wal_sync" == none ]] && printf append_batch_return || printf group_fsync)" "$reported_groups" "$group" "$output_commands" "$rps" "$rps" "$service_rps" "$reported_groups" "$sync_samples" "$sync_samples" "$sync_samples" "$elapsed" "$elapsed" "$elapsed" "$actual_bytes" "$planned_bytes" "$rotation_count" "$rotation_triggered" "$rotation_scope" "$diagnostic_after_id" "$diagnostic_after_offset" "$measured_syscw" "$measured_syscw" "$sync_samples" "$phase_profile" "$profile_fields"' \
    '      exec >&3; if [[ "$RUNNER_CASE_LABEL" == append-r1-b1 ]]; then case "$scenario" in component_resource_missing) sed -i "s/ measured_io_valid=true//" "$wal_output_file" ;; component_resource_duplicate) sed -i "s/ measured_io_valid=true/ measured_io_valid=true measured_io_valid=true/" "$wal_output_file" ;; component_resource_na) sed -i "s/ measured_io_valid=true/ measured_io_valid=na/" "$wal_output_file" ;; component_write_call_mismatch) sed -i "s/ measured_wal_write_calls=[0-9][0-9]*/ measured_wal_write_calls=0/" "$wal_output_file" ;; esac; fi; cat "$wal_output_file"; rm -f "$wal_output_file"' \
    '    elif [[ "$workload" == engine_durable_single_instrument ]]; then' \
    '      [[ "$scenario" != command_mismatch || "$RUNNER_CASE_LABEL" != engine-calibration-* ]] || commands=$((commands + 1))' \
    '      [[ "$scenario" != duration_short || "$RUNNER_CASE_LABEL" != engine-scan-* ]] || elapsed=1' \
    '      if [[ "$scenario" == cv_over && "$RUNNER_CASE_LABEL" == engine-scan-* ]]; then case "$RUNNER_CASE_LABEL" in *-r1-*) rps=100000 ;; *-r2-*) rps=120000 ;; *) rps=80000 ;; esac; fi' \
    '      if [[ "$scenario" == tail_fallback && "$RUNNER_CASE_LABEL" == engine-scan-* && "$group" == 256 ]]; then rps=110000; fi' \
    '      if [[ "$scenario" == tail_fallback && "$RUNNER_TAIL_MODE" == on && "$group" == 256 ]]; then rps=200000; fi' \
    '      if [[ "$scenario" == malformed_tail_csv && "$RUNNER_CASE_LABEL" == engine-scan-* && "$group" == 256 ]]; then rps=110000; fi' \
    '      if [[ "$scenario" == tail_bias && "$RUNNER_TAIL_MODE" == on ]]; then rps=90000; fi' \
    '      printf "engine_durable_single_instrument iterations=%s commands=%s trades=%s commands_per_second=%s trades_per_second=%s p50_us=1 p99_us=2 p99.9_us=3 max_us=4 elapsed_ms=%s active_orders=0 active_levels=0 group_size=%s group_delay_us=200 wal_prepare_workers=2 wal_parallel_prepare_min_commands=4096 wal_group_commits=1 wal_group_commands=%s actual_parallel_prepare_groups=0 actual_prepare_tasks=1 wal_sync_full_run_p99_us=1 actual_commands_per_group=%s wal_mib_per_second=1 wal_bytes_delta=1024 segment_count=1 measured_segment_rotations=0 fsync_mode=per_group completion_boundary=durable_callback instrument_count=1 shard_count=1 producer_lanes=8192 wal_path=/tmp/test wal_bytes=1024" "$iterations" "$commands" "$iterations" "$rps" "$rps" "$elapsed" "$group" "$commands" "$commands"' \
    '    else printf "%s iterations=%s commands=%s elapsed_ms=%s\\n" "$workload" "$iterations" "$commands" "$elapsed"; fi' \
    '    if [[ -n "$tail_output" ]]; then' \
    '      drain_events=0; [[ "$scenario" != drain_nonzero || "$RUNNER_TAIL_MODE" != on ]] || drain_events=1; printf " tail_telemetry=on tail_state_sampling=on telemetry_dropped_samples=0 tail_telemetry_file=%s drain_publisher_lag_events_last=%s drain_publisher_lag_bytes_last=%s drain_publisher_lag_age_ns_last=%s\\n" "$tail_output" "$drain_events" "$drain_events" "$drain_events"' \
    '      printf "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns\\n" >"$tail_output"' \
    '      for i in $(seq 1 12); do elapsed=$((i * 100)); lag=0; [[ ("$scenario" != positive_slope && "$scenario" != tail_fallback) || "$group" != 256 ]] || lag=$((i * 1000)); [[ "$scenario" != malformed_tail_csv || "$i" != 1 ]] || elapsed=bad; printf "state,measured,%s,,0,%s,0,0\\n" "$elapsed" "$lag" >>"$tail_output"; done' \
    '      printf "state,drain,1300,,0,0,0,0\\nstate,drain,1400,,0,0,0,0\\n" >>"$tail_output"' \
    '    fi' \
    '    ;;' \
    '  *) exec "$name" "$@" ;;' \
    'esac' >"$fake_bin/fake-command"
  chmod +x "$fake_bin/fake-command"
  for command in git taskset timeout mpstat iostat findmnt lsblk lscpu jq perf fio order_books_benchmark; do
    ln -s fake-command "$fake_bin/$command"
  done
}

run_runner() {
  local scenario=$1
  local output_file=$2
  shift 2
  set +e
  PATH="$FAKE_BIN:$PATH" FAKE_STATE="$FAKE_STATE" REAL_GIT="$REAL_GIT" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1 \
    ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO="$scenario" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_ROUNDS:-1}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_FRONTIER_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_FRONTIER_ROUNDS:-1}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_CONFIRMATION_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_CONFIRMATION_ROUNDS:-1}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_ROUNDS:-1}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT="${ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT:-100}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_BIAS_PERCENT="${ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_BIAS_PERCENT:-100}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES="${ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES:-}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS:-1}" \
    ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS="${ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS:-1}" \
    "$RUNNER" --binary="$FAKE_BIN/order_books_benchmark" --fio-bs=4096 \
      --run-parent="$TEST_ROOT/runs" --bench-cpus=2-7 --observer-cpus=0-1 \
      --cpu-list=2,3,4,5,6,7 "$@" >"$output_file" 2>&1
  RUNNER_STATUS=$?
  set -e
}

bash -n "$RUNNER"
eval "$(awk '
  /^decimal_double_u64\(\) \{/ { in_function=1 }
  in_function { print }
  in_function && /^}$/ { exit }
' "$RUNNER")"
eval "$(awk '
  /^component_extract_field\(\) \{/ { in_function=1 }
  in_function { print }
  in_function && /^component_extract_rate\(\) \{/ { rate_function=1 }
  rate_function && /^}$/ { exit }
' "$RUNNER")"
eval "$(awk '
  /^component_validate_measured_resources\(\) \{/ { in_function=1 }
  in_function { print }
  in_function && /^}$/ { exit }
' "$RUNNER")"
[[ "$(decimal_double_u64 10)" == 20 ]] || exit 1
[[ "$(decimal_double_u64 9223372036854775807)" == 18446744073709551614 ]] || exit 1
if decimal_double_u64 9223372036854775808 >/dev/null 2>&1; then exit 1; fi
numeric_fixture="$TEST_ROOT/component-numeric-fields.txt"
printf 'commands_per_second=1.05e+06\n' >"$numeric_fixture"
[[ "$(component_extract_rate "$numeric_fixture" state)" == 1.05e+06 ]] || exit 1
for malformed in nan inf 1e309 -1; do
  printf 'commands_per_second=%s\n' "$malformed" >"$numeric_fixture"
  if component_extract_rate "$numeric_fixture" state >/dev/null 2>&1; then exit 1; fi
done
printf 'commands_per_second=1 commands_per_second=2\n' >"$numeric_fixture"
if component_extract_rate "$numeric_fixture" state >/dev/null 2>&1; then exit 1; fi
printf 'groups=1 groups=2\n' >"$numeric_fixture"
if component_extract_integer_field "$numeric_fixture" groups >/dev/null 2>&1; then exit 1; fi
resource_fixture="$TEST_ROOT/component-resource-fields.txt"
printf '%s\n' \
  'measured_rusage_valid=true measured_io_valid=true measured_meminfo_valid=true measured_user_seconds=1 measured_system_seconds=2 measured_voluntary_context_switches=3 measured_involuntary_context_switches=4 measured_syscw=5 measured_wchar=6 measured_write_bytes=7 measured_cancelled_write_bytes=0 measured_dirty_bytes_before=8 measured_dirty_bytes_after=9 measured_writeback_bytes_before=10 measured_writeback_bytes_after=11 measured_wal_write_calls=5 measured_wal_sync_calls=0' \
  >"$resource_fixture"
component_validate_measured_resources "$resource_fixture" 0
sed 's/measured_io_valid=true/measured_io_valid=na/' "$resource_fixture" >"$numeric_fixture"
if component_validate_measured_resources "$numeric_fixture" 0 >/dev/null 2>&1; then exit 1; fi
sed 's/measured_wal_write_calls=5/measured_wal_write_calls=4/' "$resource_fixture" >"$numeric_fixture"
if component_validate_measured_resources "$numeric_fixture" 0 >/dev/null 2>&1; then exit 1; fi
dry_run=$($RUNNER --dry-run --binary=/bin/true --fio-bs=4096)
printf '%s\n' "$dry_run" | grep -F 'dry_run=true workload=engine_writer_unified_ceiling' >/dev/null
printf '%s\n' "$dry_run" | grep -F 'calibration_iterations=' >/dev/null
printf '%s\n' "$dry_run" | grep -F 'tail_attribution' >/dev/null
component_dry_run=$($RUNNER --scope=component --dry-run --binary=/bin/true)
printf '%s\n' "$component_dry_run" | grep -F 'scope=component' >/dev/null
printf '%s\n' "$component_dry_run" | grep -F 'wal_fsync' >/dev/null
! printf '%s\n' "$component_dry_run" | grep -F 'perf_record' >/dev/null
! printf '%s\n' "$component_dry_run" | grep -F 'fio' >/dev/null

set +e
ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO=normal \
  "$RUNNER" --dry-run --binary=/bin/true --fio-bs=4096 >"$TEST_ROOT/rejected.txt" 2>&1
override_status=$?
set -e
assert_status 2 "$override_status"
grep -F 'test overrides require ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1' "$TEST_ROOT/rejected.txt" >/dev/null

FAKE_BIN="$TEST_ROOT/fake-bin"
FAKE_STATE="$TEST_ROOT/fake-state"
write_fake_commands "$FAKE_BIN" "$FAKE_STATE"
mkdir -p "$TEST_ROOT/runs"
cached_before=$(git diff --cached --binary | sha256sum)

inside_parent_output="$TEST_ROOT/inside-parent.txt"
run_runner normal "$inside_parent_output" --run-parent="$REPO_ROOT/.engine-writer-runner-test"
assert_status 2 "$RUNNER_STATUS"
grep -F 'run parent must be outside repository' "$inside_parent_output" >/dev/null

preflight_output="$TEST_ROOT/preflight.txt"
run_runner preflight_failure "$preflight_output"
assert_status 3 "$RUNNER_STATUS"
preflight_root=$(grep -o 'run_root=.*' "$preflight_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=preflight-busy' "$preflight_root/logs/result.txt" >/dev/null
grep -F 'reason=initial-preflight-busy' "$preflight_root/logs/result.txt" >/dev/null
[[ ! -e "$FAKE_STATE/benchmark-count" ]] || {
  printf 'benchmark started after preflight failure\n' >&2
  exit 1
}

component_output="$TEST_ROOT/component-happy.txt"
rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT=3 \
  run_runner component_profile_bias_2pct "$component_output" --scope=component
assert_status 0 "$RUNNER_STATUS"
component_root=$(grep -o 'run_root=.*' "$component_output" | tail -n 1 | cut -d= -f2)
grep -F 'collection_status=complete' "$component_output" >/dev/null
grep -F 'result=valid-component-ceiling' "$component_root/logs/result.txt" >/dev/null
grep -F 'scope=component' \
  "$component_root/preflight/component-campaign-state-summary.txt" >/dev/null
grep -F 'label=calibration-state-new_crossing_pair' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=calibration-append-1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=calibration-fsync-1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=state-r1-new_crossing_pair' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=append-r1-b1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=fsync-r1-b1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=profile-append-b1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=profile-fsync-b1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=profile-control-append-b4096' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=profile-control-append-b4096' "$FAKE_STATE/invocations.log" |
  grep -F -- '--wal-phase-profile=off' >/dev/null
grep -F 'label=rotation-control-r1-b1' "$FAKE_STATE/invocations.log" |
  grep -F -- '--wal-phase-profile=on' >/dev/null
grep -F 'label=rotation-trigger-r1-b1' "$FAKE_STATE/invocations.log" |
  grep -F -- '--wal-phase-profile=on' >/dev/null
grep -F 'label=append-r1-b1' "$FAKE_STATE/invocations.log" |
  grep -F -- '--wal-no-rotation-epoch-commands=' >/dev/null
grep -F 'measured_segment_rotations=0' \
  "$component_root/logs/component-rotation-control-r1-b1.stdout" >/dev/null
grep -F 'rotation_scope=rotation_inclusive' \
  "$component_root/logs/component-rotation-control-r1-b1.stdout" >/dev/null
grep -F 'measured_segment_rotations=1' \
  "$component_root/logs/component-rotation-trigger-r1-b1.stdout" >/dev/null
grep -F 'rotation_scope=rotation_inclusive' \
  "$component_root/logs/component-rotation-trigger-r1-b1.stdout" >/dev/null
rotation_rows="$component_root/derived/component-rotation-attribution.tsv"
awk -F '\t' 'NR == 3 && NF == 22 { found=1 } END { exit !found }' "$rotation_rows"
awk 'BEGIN { ok=1 }
     /label=calibration-append-8192 / {
       found=1
       if ($0 !~ /--warmup=[1-9][0-9]*([[:space:]]|$)/ ||
           $0 ~ /--warmup=10000([[:space:]]|$)/) ok=0
     }
     END { exit !(found && ok != 0) }' "$FAKE_STATE/invocations.log"
! grep -Eq 'label=(append|fsync)-r[0-9]+-b[0-9]+ .*--wal-phase-profile=on' \
  "$FAKE_STATE/invocations.log"
! grep -F 'profile' "$component_root/derived/component-cv.tsv" >/dev/null
! grep -Eq 'perf|fio|engine_durable|pipeline-stage=(invariant_validation|runtime_handoff)' \
  "$FAKE_STATE/invocations.log"
grep -F $'state_machine\tnew_crossing_pair\t' \
  "$component_root/derived/component-frozen-plan.tsv" >/dev/null
grep -F $'wal_append\tb4096\t' \
  "$component_root/derived/component-frozen-plan.tsv" >/dev/null
grep -F $'wal_fsync\tb4096\t' \
  "$component_root/derived/component-frozen-plan.tsv" >/dev/null
grep -F 'case=append-b4096' "$component_root/derived/component-profile-bias.tsv" >/dev/null
grep -F 'bias_percent=2' "$component_root/derived/component-profile-bias.tsv" >/dev/null
[[ ! -e "$component_root/data/component-append-r1-b1" ]] || exit 1
[[ ! -e "$component_root/data/component-rotation-control-r1-b1" ]] || exit 1

for scenario in \
  component_wrong_no_rotation_name component_resource_missing component_resource_duplicate \
  component_resource_na component_write_call_mismatch component_byte_plan_mismatch \
  component_rotation_control_nonzero component_rotation_trigger_zero \
  component_rotation_trigger_multiple component_rotation_multiple_groups \
  component_rotation_case_mismatch component_rotation_position_mismatch; do
  rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
  failure_output="$TEST_ROOT/${scenario}.txt"
  run_runner "$scenario" "$failure_output" --scope=component
  assert_status 4 "$RUNNER_STATUS"
  failure_root=$(grep -o 'run_root=.*' "$failure_output" | tail -n 1 | cut -d= -f2)
  grep -F 'result=invalid-run' "$failure_root/logs/result.txt" >/dev/null
  ! grep -F 'result=valid-component-ceiling' "$failure_root/logs/result.txt" >/dev/null
  expected_reason=append-counter-1-1
  failed_label=append-r1-b1
  case "$scenario" in
    component_rotation_trigger_zero|component_rotation_trigger_multiple)
      failed_label=rotation-trigger-r1-b1
      expected_reason="rotation-output-${failed_label}"
      ;;
    component_rotation_*)
      failed_label=rotation-control-r1-b1
      expected_reason="rotation-output-${failed_label}"
      ;;
  esac
  grep -Fx "reason=${expected_reason}" "$failure_root/logs/result.txt" >/dev/null
  [[ -d "$failure_root/data/component-${failed_label}" ]] || {
    printf 'failed case data was removed for %s\n' "$scenario" >&2
    exit 1
  }
  ! grep -F 'label=fsync-r1-b1' "$FAKE_STATE/invocations.log" >/dev/null
done

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_rounding_output="$TEST_ROOT/component-rounding.txt"
run_runner component_rounding_probe "$component_rounding_output" --scope=component
assert_status 0 "$RUNNER_STATUS"
component_rounding_root=$(grep -o 'run_root=.*' "$component_rounding_output" | tail -n 1 | cut -d= -f2)
awk -F '\t' '$1 == "state_machine" && $3 == 10002 { found=1 } END { exit !found }' \
  "$component_rounding_root/derived/component-frozen-plan.tsv"
awk -F '\t' '$1 == "wal_append" && $2 == "b1" && $3 == 16384 { found=1 } END { exit !found }' \
  "$component_rounding_root/derived/component-frozen-plan.tsv"

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_preflight_output="$TEST_ROOT/component-preflight-failure.txt"
run_runner preflight_failure "$component_preflight_output" --scope=component
assert_status 3 "$RUNNER_STATUS"
component_preflight_root=$(grep -o 'run_root=.*' "$component_preflight_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=preflight-busy' "$component_preflight_root/logs/result.txt" >/dev/null
grep -F 'reason=initial-component-cpu-preflight' \
  "$component_preflight_root/logs/result.txt" >/dev/null
[[ ! -e "$FAKE_STATE/benchmark-count" ]] || exit 1

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_bias_output="$TEST_ROOT/component-profile-bias.txt"
ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT=3 \
  run_runner component_profile_bias "$component_bias_output" --scope=component
assert_status 3 "$RUNNER_STATUS"
component_bias_root=$(grep -o 'run_root=.*' "$component_bias_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=observer-biased' "$component_bias_root/logs/result.txt" >/dev/null
grep -F 'reason=component-profile-bias' "$component_bias_root/logs/result.txt" >/dev/null
! grep -F 'result=valid-component-ceiling' "$component_bias_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_missing_output="$TEST_ROOT/component-profile-missing-field.txt"
run_runner component_profile_missing_field "$component_missing_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_missing_root=$(grep -o 'run_root=.*' "$component_missing_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_missing_root/logs/result.txt" >/dev/null
grep -F 'reason=rotation-output-rotation-control-r1-b1' \
  "$component_missing_root/logs/result.txt" >/dev/null
! grep -F 'result=valid-component-ceiling' "$component_missing_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_partial_output="$TEST_ROOT/component-partial.txt"
ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS=3 \
  run_runner component_append_cv_rejected "$component_partial_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_partial_root=$(grep -o 'run_root=.*' "$component_partial_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=collection-complete-results-partial' \
  "$component_partial_root/logs/result.txt" >/dev/null
grep -F 'rejected_cases=1' "$component_partial_root/logs/result.txt" >/dev/null
grep -F 'kind=append key=b1 status=rejected-unstable' \
  "$component_partial_root/derived/component-case-status.tsv" >/dev/null
grep -F 'label=fsync-r1-b1' "$FAKE_STATE/invocations.log" >/dev/null
grep -F 'label=profile-append-b1' "$FAKE_STATE/invocations.log" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_counter_output="$TEST_ROOT/component-profile-counter-mismatch.txt"
run_runner component_profile_counter_mismatch "$component_counter_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_counter_root=$(grep -o 'run_root=.*' "$component_counter_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_counter_root/logs/result.txt" >/dev/null
grep -F 'reason=rotation-output-rotation-control-r1-b1' \
  "$component_counter_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_profile_groups_output="$TEST_ROOT/component-profile-groups-mismatch.txt"
run_runner component_profile_groups_mismatch "$component_profile_groups_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_profile_groups_root=$(grep -o 'run_root=.*' "$component_profile_groups_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_profile_groups_root/logs/result.txt" >/dev/null
grep -F 'reason=rotation-output-rotation-control-r1-b1' \
  "$component_profile_groups_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_groups_output="$TEST_ROOT/component-groups-mismatch.txt"
run_runner component_group_counter_mismatch "$component_groups_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_groups_root=$(grep -o 'run_root=.*' "$component_groups_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_groups_root/logs/result.txt" >/dev/null
grep -F 'reason=append-counter-1-1' "$component_groups_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_sync_output="$TEST_ROOT/component-sync-counter-mismatch.txt"
run_runner component_sync_counter_mismatch "$component_sync_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_sync_root=$(grep -o 'run_root=.*' "$component_sync_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_sync_root/logs/result.txt" >/dev/null
grep -F 'reason=fsync-counter-1-1' "$component_sync_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_sync_overflow_output="$TEST_ROOT/component-sync-counter-overflow.txt"
run_runner component_sync_counter_overflow "$component_sync_overflow_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_sync_overflow_root=$(grep -o 'run_root=.*' "$component_sync_overflow_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_sync_overflow_root/logs/result.txt" >/dev/null
grep -F 'reason=fsync-counter-1-1' "$component_sync_overflow_root/logs/result.txt" >/dev/null

rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
component_malformed_output="$TEST_ROOT/component-malformed-counter.txt"
run_runner component_malformed_counter "$component_malformed_output" --scope=component
assert_status 4 "$RUNNER_STATUS"
component_malformed_root=$(grep -o 'run_root=.*' "$component_malformed_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$component_malformed_root/logs/result.txt" >/dev/null
grep -F 'reason=append-counter-1-1' "$component_malformed_root/logs/result.txt" >/dev/null

for scenario in benchmark_nonzero benchmark_timeout command_mismatch identity_change observer_early_exit; do
  rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
  output="$TEST_ROOT/${scenario}.txt"
  run_runner "$scenario" "$output"
  assert_status 4 "$RUNNER_STATUS"
  scenario_root=$(grep -o 'run_root=.*' "$output" | tail -n 1 | cut -d= -f2)
  grep -F 'result=invalid-run' "$scenario_root/logs/result.txt" >/dev/null
  if [[ "$scenario" == benchmark_nonzero ]]; then
    grep -F 'reason=benchmark-exit-nonzero' "$scenario_root/logs/result.txt" >/dev/null
    [[ "$(<"$FAKE_STATE/benchmark-count")" == 1 ]] || exit 1
  elif [[ "$scenario" == benchmark_timeout ]]; then
    grep -F 'reason=benchmark-timeout' "$scenario_root/logs/result.txt" >/dev/null
    [[ ! -e "$FAKE_STATE/benchmark-count" ]] || exit 1
  fi
  if [[ "$scenario" == observer_early_exit ]]; then
    grep -F 'reason=observer-failed' "$scenario_root/logs/result.txt" >/dev/null
    grep -Eq 'pid=[0-9]+ .*status=7' "$scenario_root/logs/observer-status.txt" >/dev/null
    [[ -s "$FAKE_STATE/benchmark-count" ]] || exit 1
  fi
  if [[ "$scenario" == identity_change ]]; then
    grep -F 'reason=artifact-identity-changed' "$scenario_root/logs/result.txt" >/dev/null
    [[ "$(<"$FAKE_STATE/benchmark-count")" == 1 ]] || exit 1
  fi
  if [[ "$scenario" == command_mismatch ]]; then
    grep -F 'reason=calibration-command-count-mismatch-' "$scenario_root/logs/result.txt" >/dev/null
    [[ -s "$FAKE_STATE/benchmark-count" ]] || exit 1
  fi
done

for scenario in duration_short cv_over; do
  rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
  output="$TEST_ROOT/${scenario}.txt"
  if [[ "$scenario" == cv_over ]]; then
    ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200 \
      ENGINE_WRITER_DIAGNOSTICS_TEST_FRONTIER_ROUNDS=3 \
      run_runner "$scenario" "$output"
  else
    ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200 \
      run_runner "$scenario" "$output"
  fi
  assert_status 4 "$RUNNER_STATUS"
  scenario_root=$(grep -o 'run_root=.*' "$output" | tail -n 1 | cut -d= -f2)
  grep -F 'result=invalid-run' "$scenario_root/logs/result.txt" >/dev/null
  if [[ "$scenario" == duration_short ]]; then
    grep -F 'reason=frontier-selection-failed' "$scenario_root/logs/result.txt" >/dev/null
    grep -F $'g256-d200\texcluded\tna\tna\tna\tduration-under-30s' \
      "$scenario_root/derived/frontier-selection.tsv" >/dev/null
    awk -F '\t' 'NF != 6 { exit 1 } END { exit !(NR == 2) }' \
      "$scenario_root/derived/frontier-selection.tsv"
    [[ ! -s "$scenario_root/derived/frontier-candidates.txt" ]] || exit 1
    [[ "$(grep -c 'label=engine-scan-r[0-9]*-g256-d200 ' "$FAKE_STATE/invocations.log")" == 1 ]] || exit 1
  else
    grep -F 'reason=frontier-selection-failed' "$scenario_root/logs/result.txt" >/dev/null
    grep -F 'cv-over-5-percent' "$scenario_root/derived/frontier-selection.tsv" >/dev/null
    [[ "$(grep -c 'label=engine-scan-r[0-9]*-g256-d200 ' "$FAKE_STATE/invocations.log")" == 3 ]] || exit 1
  fi
done

for scenario in perf_unsupported perf_call_graph_failure; do
  rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" \
    "$FAKE_STATE/invocations.log" "$FAKE_STATE/perf.log"
  perf_output="$TEST_ROOT/${scenario}.txt"
  run_runner "$scenario" "$perf_output"
  assert_status 2 "$RUNNER_STATUS"
  grep -F 'result=environment-blocked' "$perf_output" >/dev/null
  if [[ "$scenario" == perf_unsupported ]]; then
    grep -F 'reason=perf-events-unavailable' "$perf_output" >/dev/null
  else
    grep -F 'reason=perf-call-graph-unavailable' "$perf_output" >/dev/null
  fi
  [[ ! -e "$FAKE_STATE/benchmark-count" ]] || exit 1
done

fallback_output="$TEST_ROOT/tail-fallback.txt"
rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200,g4096-d1000 \
  ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_ROUNDS=3 \
  ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_ROUNDS=3 \
  run_runner tail_fallback "$fallback_output"
assert_status 0 "$RUNNER_STATUS"
grep -F 'collection_status=complete' "$fallback_output" >/dev/null
fallback_root=$(grep -o 'run_root=.*' "$fallback_output" | tail -n 1 | cut -d= -f2)
[[ -s "$fallback_root/derived/frontier-selection.txt" ]] || exit 1
awk -F '\t' '$1 == "g256-d200" { exit !($2 == 10 && $3 == 20 && $4 == 42 && $5 == 27) }' \
  "$fallback_root/derived/engine-iteration-plan.tsv"
grep -F $'g256-d200\tcomparison-rejected' "$fallback_root/derived/frontier-selection.txt" >/dev/null
grep -F $'g256-d200\tcomparison-rejected\treason=positive-backlog-slope' \
  "$fallback_root/derived/frontier-selection.txt" >/dev/null
grep -F $'g256-d200\trejected' "$fallback_root/derived/frontier-selection.txt" >/dev/null
grep -F $'g4096-d1000\tselected-sustainable' "$fallback_root/derived/frontier-selection.txt" >/dev/null
for observer_case in off-r1 on-r1 on-r2 off-r2 off-r3 on-r3; do
  [[ -s "$fallback_root/logs/observer-gate-${observer_case}.stdout" ]] || exit 1
done
expected_observer_order=(
  observer-gate-off-r1 observer-gate-on-r1 observer-gate-on-r2
  observer-gate-off-r2 observer-gate-off-r3 observer-gate-on-r3
)
mapfile -t actual_observer_order < <(
  awk '/label=observer-gate-/ { for (i = 1; i <= NF; ++i) if ($i ~ /^label=/) { sub(/^label=/, "", $i); print $i; break } }' \
    "$FAKE_STATE/invocations.log"
)
[[ "${actual_observer_order[*]}" == "${expected_observer_order[*]}" ]] || exit 1
[[ -s "$fallback_root/logs/tail-gate-r1-g256-d200-off.stdout" ]] || exit 1
! grep -F 'tail_telemetry=on' "$fallback_root/logs/tail-gate-r1-g256-d200-off.stdout" >/dev/null
grep -F 'tail_telemetry=on' "$fallback_root/logs/tail-gate-r1-g256-d200-on.stdout" >/dev/null
[[ -s "$fallback_root/derived/tail-gate-g256-d200.summary" ]] || exit 1
[[ -s "$fallback_root/derived/tail-gate-g4096-d1000.summary" ]] || exit 1
[[ -s "$fallback_root/derived/tail-gate-g256-d200-validation.txt" ]] || exit 1
grep -F 'reason=positive-backlog-slope' "$fallback_root/derived/tail-gate-g256-d200.summary" >/dev/null
[[ "$(grep -c 'label=tail-gate-.*g256-d200-' "$FAKE_STATE/invocations.log")" == 2 ]] || exit 1
[[ "$(grep -c 'label=tail-gate-.*g4096-d1000-' "$FAKE_STATE/invocations.log")" == 6 ]] || exit 1
grep -F 'perf-stat events=task-clock,cycles,instructions,cache-misses,context-switches,cpu-migrations,page-faults' \
  "$FAKE_STATE/perf.log" >/dev/null
awk -F '\t' 'NF != 6 { exit 1 } END { exit !(NR == 3) }' \
  "$fallback_root/derived/frontier-selection.tsv"
! grep -Eq '^(best_case|best_median_rps)=' \
  "$fallback_root/derived/frontier-selection.tsv"
grep -Eq '^best_case=' "$fallback_root/derived/frontier-selection.txt"
grep -Eq '^best_median_rps=' "$fallback_root/derived/frontier-selection.txt"

drain_output="$TEST_ROOT/tail-drain.txt"
rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200,g4096-d1000 \
  run_runner drain_nonzero "$drain_output"
assert_status 3 "$RUNNER_STATUS"
drain_root=$(grep -o 'run_root=.*' "$drain_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=inconclusive-attribution' "$drain_root/logs/result.txt" >/dev/null
grep -F 'reason=no-sustainable-frontier-case' "$drain_root/logs/result.txt" >/dev/null
grep -F 'reason=drain-not-empty' "$drain_root/derived/tail-gate-g4096-d1000.summary" >/dev/null
[[ -s "$FAKE_STATE/benchmark-count" ]] || exit 1

bias_output="$TEST_ROOT/tail-bias.txt"
rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200,g4096-d1000 \
  ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_BIAS_PERCENT=3 \
  run_runner tail_bias "$bias_output"
assert_status 3 "$RUNNER_STATUS"
bias_root=$(grep -o 'run_root=.*' "$bias_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=inconclusive-attribution' "$bias_root/logs/result.txt" >/dev/null
grep -F 'reason=no-sustainable-frontier-case' "$bias_root/logs/result.txt" >/dev/null
grep -F 'reason=tail-observer-bias' "$bias_root/derived/tail-gate-g4096-d1000.summary" >/dev/null
[[ -s "$FAKE_STATE/benchmark-count" ]] || exit 1

malformed_output="$TEST_ROOT/malformed-tail.csv.txt"
rm -f "$FAKE_STATE/benchmark-count" "$FAKE_STATE/identity-marker" "$FAKE_STATE/invocations.log"
ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES=g256-d200,g4096-d1000 \
  run_runner malformed_tail_csv "$malformed_output"
assert_status 4 "$RUNNER_STATUS"
malformed_root=$(grep -o 'run_root=.*' "$malformed_output" | tail -n 1 | cut -d= -f2)
grep -F 'result=invalid-run' "$malformed_root/logs/result.txt" >/dev/null
grep -F 'reason=tail-attribution-g256-d200-20' "$malformed_root/logs/result.txt" >/dev/null
grep -F 'reason=malformed-tail-csv' \
  "$malformed_root/derived/tail-gate-g256-d200.summary" >/dev/null
[[ -s "$FAKE_STATE/benchmark-count" ]] || exit 1

cached_after=$(git diff --cached --binary | sha256sum)
[[ "$cached_before" == "$cached_after" ]] || { printf 'contract test changed Git index\n' >&2; exit 1; }
if grep -E 'git (add|reset|restore --staged|commit)' "$RUNNER" >/dev/null; then
  printf 'runner must not mutate the Git index\n' >&2
  exit 1
fi

printf 'engine writer diagnostics runner contract tests passed\n'
