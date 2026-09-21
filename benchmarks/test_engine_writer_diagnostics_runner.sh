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
    '    iterations=10; workload=""; group=256; tail_output=""; for arg in "$@"; do case "$arg" in --iterations=*) iterations=${arg#*=} ;; --workload=*) workload=${arg#*=} ;; --engine-group-size=*) group=${arg#*=} ;; --engine-tail-telemetry-output=*) tail_output=${arg#*=} ;; esac; done' \
    '    : "${RUNNER_CASE_LABEL:?}"' \
    '    [[ "$scenario" != identity_change ]] || : >"$state/identity-marker"; commands=$((iterations * 2)); elapsed=42; rps=100000' \
    '    if [[ "$workload" == engine_durable_single_instrument ]]; then' \
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
[[ "$(decimal_double_u64 10)" == 20 ]] || exit 1
[[ "$(decimal_double_u64 9223372036854775807)" == 18446744073709551614 ]] || exit 1
if decimal_double_u64 9223372036854775808 >/dev/null 2>&1; then exit 1; fi
dry_run=$($RUNNER --dry-run --binary=/bin/true --fio-bs=4096)
printf '%s\n' "$dry_run" | grep -F 'dry_run=true workload=engine_writer_unified_ceiling' >/dev/null
printf '%s\n' "$dry_run" | grep -F 'calibration_iterations=' >/dev/null
printf '%s\n' "$dry_run" | grep -F 'tail_attribution' >/dev/null

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
