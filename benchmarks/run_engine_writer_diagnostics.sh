#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: run_engine_writer_diagnostics.sh --binary=PATH [--fio-bs=BYTES[,BYTES...]] [options]

Runs the bounded, external Engine ceiling/root-cause campaign. All artifacts
are written outside the repository. The script never changes source files,
system policy, or the Git index.

Options:
  --binary=PATH              ReleaseBenchmark order_books_benchmark binary
  --fio-bs=BYTES[,BYTES...]  WAL bytes per selected group for file-backed fio controls
  --scope=full|component     Full Engine campaign or StateMachine/WAL component campaign
  --run-parent=PATH          Parent directory for the run root (outside repo)
  --bench-cpus=LIST          Benchmark CPUs (default: 2-7)
  --observer-cpus=LIST       Observer CPUs (default: 0-1)
  --cpu-list=LIST            mpstat CPU list (default: 2,3,4,5,6,7)
  --iterations=N             Benchmark iterations (default: 4000000)
  --calibration-iterations=N Engine calibration iterations (default: 250000)
  --warmup=N                 Benchmark warmup iterations (default: 10000)
  --case-timeout-seconds=N   Per benchmark timeout (default: 300)
  --observer=off|on          Per-run perf/mpstat/iostat (default: on)
  --profile-only             Run only the selected perf-record case
  --profile-group-size=N     perf-record group size (default: 4096)
  --profile-delay-us=N       perf-record group delay (default: 1000)
  --dry-run                  Validate and print the campaign plan only
  --help
EOF
}

die() {
  printf 'engine_writer_diagnostics: %s\n' "$*" >&2
  exit 2
}

classified_die() {
  local result=$1
  local reason=$2
  local status=$3
  printf 'result=%s\nreason=%s\n' "$result" "$reason" >&2
  exit "$status"
}

campaign_fail() {
  local result=$1
  local reason=$2
  printf 'result=%s\nreason=%s\n' "$result" "$reason" >"$RUN_ROOT/logs/result.txt"
  printf 'run_root=%s\n' "$RUN_ROOT"
  exit 4
}

REPO_ROOT=$(git rev-parse --show-toplevel)
BENCHMARK_BINARY=""
FIO_BLOCK_BYTES=""
SCOPE=full
RUN_PARENT="$(dirname "$REPO_ROOT")"
BENCH_CPU_SET="2-7"
OBSERVER_CPU_SET="0-1"
CPU_LIST="2,3,4,5,6,7"
ITERATIONS=4000000
WARMUP=10000
CASE_TIMEOUT_SECONDS=300
MONITOR_SECONDS=340
OBSERVER_MODE=on
PROFILE_ONLY=0
PROFILE_GROUP_SIZE=4096
PROFILE_DELAY_US=1000
PROFILE_GROUP_SIZE_SET=0
PROFILE_DELAY_US_SET=0
DRY_RUN=0
CALIBRATION_ITERATIONS=250000
ENGINE_TARGET_DURATION_MS=40000
MIN_ENGINE_DURATION_MS=30000
PREFLIGHT_SECONDS=30
OBSERVER_BIAS_LIMIT_PERCENT=3.0
TAIL_BIAS_LIMIT_PERCENT=3.0
TEST_MODE=${ENGINE_WRITER_DIAGNOSTICS_TEST_MODE:-0}

declare -a ENGINE_CASES=(
  g256-d200 g256-d1000 g1024-d200 g1024-d1000
  g4096-d200 g4096-d1000 g8192-d200 g8192-d1000
)
declare -a PIPELINE_STAGES=(state_machine invariant_validation metrics runtime_handoff)
declare -a WAL_CASES=(
  none:256 none:1024 none:4096 none:8192
  per_group:256 per_group:1024 per_group:4096 per_group:8192
)
OBSERVER_GATE_ROUNDS=3
FRONTIER_ROUNDS=3
CONFIRMATION_ROUNDS=5
TAIL_ATTRIBUTION_ROUNDS=3
PIPELINE_ROUNDS=5
WAL_ROUNDS=5

for argument in "$@"; do
  case "$argument" in
    --binary=*) BENCHMARK_BINARY=${argument#*=} ;;
    --fio-bs=*) FIO_BLOCK_BYTES=${argument#*=} ;;
    --scope=full) SCOPE=full ;;
    --scope=component) SCOPE=component ;;
    --scope=*) die "scope must be full or component" ;;
    --run-parent=*) RUN_PARENT=${argument#*=} ;;
    --bench-cpus=*) BENCH_CPU_SET=${argument#*=} ;;
    --observer-cpus=*) OBSERVER_CPU_SET=${argument#*=} ;;
    --cpu-list=*) CPU_LIST=${argument#*=} ;;
    --iterations=*) ITERATIONS=${argument#*=} ;;
    --warmup=*) WARMUP=${argument#*=} ;;
    --case-timeout-seconds=*) CASE_TIMEOUT_SECONDS=${argument#*=} ;;
    --observer=off) OBSERVER_MODE=off ;;
    --observer=on) OBSERVER_MODE=on ;;
    --profile-only) PROFILE_ONLY=1 ;;
    --profile-group-size=*) PROFILE_GROUP_SIZE=${argument#*=}; PROFILE_GROUP_SIZE_SET=1 ;;
    --profile-delay-us=*) PROFILE_DELAY_US=${argument#*=}; PROFILE_DELAY_US_SET=1 ;;
    --calibration-iterations=*) CALIBRATION_ITERATIONS=${argument#*=} ;;
    --dry-run) DRY_RUN=1 ;;
    --help) usage; exit 0 ;;
    *) usage; die "unknown option: $argument" ;;
  esac
done

if [[ -z "$BENCHMARK_BINARY" ]]; then
  BENCHMARK_BINARY="$REPO_ROOT/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
fi

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

decimal_double_u64() {
  local value=$1
  local digit product carry=0 result="" index
  [[ "$value" =~ ^[0-9]+$ ]] || return 1
  while [[ ${#value} -gt 1 && ${value:0:1} == 0 ]]; do
    value=${value:1}
  done
  [[ "$value" != 0 ]] || return 1
  ((${#value} < 19)) || {
    ((${#value} == 19)) || return 1
    [[ "$value" < "9223372036854775807" ||
       "$value" == "9223372036854775807" ]] || return 1
  }
  for ((index=${#value}-1; index>=0; --index)); do
    digit=${value:index:1}
    product=$((digit * 2 + carry))
    result="${product: -1}${result}"
    carry=$((product / 10))
  done
  ((carry == 0)) || result="${carry}${result}"
  printf '%s\n' "$result"
}

TEST_OVERRIDE_NAMES=(
  ENGINE_WRITER_DIAGNOSTICS_TEST_PREFLIGHT_SECONDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_MONITOR_SECONDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_CALIBRATION_ITERATIONS
  ENGINE_WRITER_DIAGNOSTICS_TEST_TARGET_DURATION_MS
  ENGINE_WRITER_DIAGNOSTICS_TEST_MIN_DURATION_MS
  ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES
  ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_FRONTIER_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_CONFIRMATION_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS
  ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT
  ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_BIAS_PERCENT
  ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO
)
test_override_is_set() {
  local name
  for name in "${TEST_OVERRIDE_NAMES[@]}"; do
    [[ -z "${!name+x}" ]] || return 0
  done
  return 1
}

[[ "$TEST_MODE" == 0 || "$TEST_MODE" == 1 ]] || die \
  "ENGINE_WRITER_DIAGNOSTICS_TEST_MODE must be 0 or 1"
if [[ "$TEST_MODE" == 0 ]] && test_override_is_set; then
  die "test overrides require ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1"
fi
if [[ "$TEST_MODE" == 1 ]]; then
  PREFLIGHT_SECONDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_PREFLIGHT_SECONDS:-1}
  MONITOR_SECONDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_MONITOR_SECONDS:-2}
  CALIBRATION_ITERATIONS=${ENGINE_WRITER_DIAGNOSTICS_TEST_CALIBRATION_ITERATIONS:-10}
  ENGINE_TARGET_DURATION_MS=${ENGINE_WRITER_DIAGNOSTICS_TEST_TARGET_DURATION_MS:-100}
  MIN_ENGINE_DURATION_MS=${ENGINE_WRITER_DIAGNOSTICS_TEST_MIN_DURATION_MS:-30}
  OBSERVER_GATE_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_ROUNDS:-3}
  FRONTIER_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_FRONTIER_ROUNDS:-3}
  CONFIRMATION_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_CONFIRMATION_ROUNDS:-5}
  TAIL_ATTRIBUTION_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_ROUNDS:-3}
  PIPELINE_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS:-1}
  WAL_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS:-1}
  OBSERVER_BIAS_LIMIT_PERCENT=${ENGINE_WRITER_DIAGNOSTICS_TEST_OBSERVER_BIAS_PERCENT:-3.0}
  TAIL_BIAS_LIMIT_PERCENT=${ENGINE_WRITER_DIAGNOSTICS_TEST_TAIL_BIAS_PERCENT:-3.0}
  if [[ -n "${ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES:-}" ]]; then
    IFS=',' read -r -a ENGINE_CASES <<<"$ENGINE_WRITER_DIAGNOSTICS_TEST_ENGINE_CASES"
  fi
fi

is_positive_integer "$ITERATIONS" || die "iterations must be a positive integer"
is_positive_integer "$WARMUP" || die "warmup must be a positive integer"
is_positive_integer "$CASE_TIMEOUT_SECONDS" || die "case timeout must be a positive integer"
is_positive_integer "$PROFILE_GROUP_SIZE" || die "profile group size must be a positive integer"
is_positive_integer "$CALIBRATION_ITERATIONS" || die "calibration iterations must be a positive integer"
is_positive_integer "$PREFLIGHT_SECONDS" || die "preflight seconds must be a positive integer"
is_positive_integer "$MONITOR_SECONDS" || die "monitor seconds must be a positive integer"
is_positive_integer "$ENGINE_TARGET_DURATION_MS" || die "engine target duration must be a positive integer"
is_positive_integer "$MIN_ENGINE_DURATION_MS" || die "minimum engine duration must be a positive integer"
is_positive_integer "$OBSERVER_GATE_ROUNDS" || die "observer gate rounds must be a positive integer"
is_positive_integer "$FRONTIER_ROUNDS" || die "frontier rounds must be a positive integer"
is_positive_integer "$CONFIRMATION_ROUNDS" || die "confirmation rounds must be a positive integer"
is_positive_integer "$TAIL_ATTRIBUTION_ROUNDS" || die "tail attribution rounds must be a positive integer"
is_positive_integer "$PIPELINE_ROUNDS" || die "pipeline rounds must be a positive integer"
is_positive_integer "$WAL_ROUNDS" || die "WAL rounds must be a positive integer"
[[ "$PROFILE_DELAY_US" =~ ^[0-9]+$ ]] || die "profile delay must be a non-negative integer"
[[ "$OBSERVER_BIAS_LIMIT_PERCENT" =~ ^[0-9]+([.][0-9]+)?$ ]] || die \
  "observer bias limit must be a non-negative number"
[[ "$TAIL_BIAS_LIMIT_PERCENT" =~ ^[0-9]+([.][0-9]+)?$ ]] || die \
  "tail bias limit must be a non-negative number"
if [[ "$PROFILE_ONLY" == 0 ]]; then
  if [[ "$SCOPE" == full ]]; then
    [[ -n "$FIO_BLOCK_BYTES" ]] || die "--fio-bs is required for a full campaign"
  fi
  if [[ -n "$FIO_BLOCK_BYTES" ]]; then
  IFS=',' read -r -a FIO_BLOCK_LIST <<<"$FIO_BLOCK_BYTES"
  ((${#FIO_BLOCK_LIST[@]} > 0)) || die "--fio-bs must contain at least one size"
  for fio_block in "${FIO_BLOCK_LIST[@]}"; do
    is_positive_integer "$fio_block" || die "each --fio-bs value must be a positive integer"
  done
  fi
fi

if [[ "$DRY_RUN" == 1 ]]; then
  printf 'dry_run=true workload=engine_writer_unified_ceiling scope=%s\n' "$SCOPE"
  printf 'binary=%s\n' "$BENCHMARK_BINARY"
  printf 'bench_cpus=%s observer_cpus=%s cpu_list=%s observer=%s\n' \
    "$BENCH_CPU_SET" "$OBSERVER_CPU_SET" "$CPU_LIST" "$OBSERVER_MODE"
  printf 'iterations=%s warmup=%s profile_group_size=%s profile_delay_us=%s\n' \
    "$ITERATIONS" "$WARMUP" "$PROFILE_GROUP_SIZE" "$PROFILE_DELAY_US"
  printf 'calibration_iterations=%s engine_target_duration_ms=%s\n' \
    "$CALIBRATION_ITERATIONS" "$ENGINE_TARGET_DURATION_MS"
  if [[ -n "$FIO_BLOCK_BYTES" ]]; then
    printf 'fio_bs=%s\n' "$FIO_BLOCK_BYTES"
  fi
  if [[ "$SCOPE" == component ]]; then
    printf 'plan=identity,cpu_preflight,state_machine,wal_append_preflight,wal_append,wal_fsync_preflight,wal_fsync,artifacts\n'
  else
    printf 'plan=preflight,calibration,observer_gate,component,wal,engine_frontier,tail_attribution,writer_profile,perf_record,fio\n'
  fi
  exit 0
fi

[[ -x "$BENCHMARK_BINARY" ]] || classified_die environment-blocked benchmark-binary-unavailable 2

COMMON_TOOLS=(git sha256sum findmnt lsblk lscpu mpstat iostat jq taskset timeout awk realpath ps \
    cmp cut grep sed sort tee uname xargs mktemp date df)
if [[ "$SCOPE" == full ]]; then
  COMMON_TOOLS+=(perf)
fi
if [[ "$SCOPE" == full && "$PROFILE_ONLY" == 0 ]]; then
  COMMON_TOOLS+=(fio)
fi
for tool in "${COMMON_TOOLS[@]}"; do
  command -v "$tool" >/dev/null || classified_die environment-blocked "tool-unavailable-${tool}" 2
done
[[ -x /usr/bin/time ]] || classified_die environment-blocked tool-unavailable-time 2

REPO_REAL=$(realpath "$REPO_ROOT")
RUN_PARENT_REAL=$(realpath -m "$RUN_PARENT")
case "$RUN_PARENT_REAL/" in
  "$REPO_REAL/"|"$REPO_REAL"/*) die "run parent must be outside repository: $RUN_PARENT_REAL" ;;
esac

if ! taskset -c "$BENCH_CPU_SET" true || ! taskset -c "$OBSERVER_CPU_SET" true; then
  classified_die environment-blocked cpu-affinity-unavailable 2
fi

expand_cpu_set() {
  local item first last cpu
  IFS=',' read -r -a items <<<"$1"
  for item in "${items[@]}"; do
    if [[ "$item" == *-* ]]; then
      IFS='-' read -r first last <<<"$item"
    else
      first=$item
      last=$item
    fi
    [[ "$first" =~ ^[0-9]+$ && "$last" =~ ^[0-9]+$ && first -le last ]] || return 1
    for ((cpu=first; cpu<=last; ++cpu)); do
      printf '%s\n' "$cpu"
    done
  done
}

declare -A BENCH_CPU_MAP=()
declare -A OBSERVER_CPU_MAP=()
BENCH_CPU_TEXT=$(expand_cpu_set "$BENCH_CPU_SET") || die "invalid benchmark CPU set"
OBSERVER_CPU_TEXT=$(expand_cpu_set "$OBSERVER_CPU_SET") || die "invalid observer CPU set"
CPU_LIST_TEXT=$(expand_cpu_set "$CPU_LIST") || die "invalid mpstat CPU list"
while read -r cpu; do
  BENCH_CPU_MAP[$cpu]=1
done <<<"$BENCH_CPU_TEXT"
while read -r cpu; do
  OBSERVER_CPU_MAP[$cpu]=1
done <<<"$OBSERVER_CPU_TEXT"
for cpu in "${!OBSERVER_CPU_MAP[@]}"; do
  [[ -z "${BENCH_CPU_MAP[$cpu]:-}" ]] || classified_die environment-blocked cpu-sets-overlap 2
done
while read -r cpu; do
  [[ -n "${BENCH_CPU_MAP[$cpu]:-}" ]] || classified_die environment-blocked cpu-list-outside-benchmark-set 2
done <<<"$CPU_LIST_TEXT"

check_perf_capability() {
  local stat_output record_output record_data
  stat_output=$(mktemp)
  record_output=$(mktemp)
  record_data=$(mktemp)
  if ! perf stat --no-big-num -x, \
      -e task-clock,cycles,instructions,cache-misses,context-switches,cpu-migrations,page-faults \
      -- true >/dev/null 2>"$stat_output" ||
     grep -Eq '<not supported>|<not counted>' "$stat_output"; then
    rm -f "$stat_output" "$record_output" "$record_data"
    classified_die environment-blocked perf-events-unavailable 2
  fi
  if ! perf record -F 99 -g --call-graph fp -o "$record_data" -- true \
      >/dev/null 2>"$record_output" || [[ ! -s "$record_data" ]]; then
    rm -f "$stat_output" "$record_output" "$record_data"
    classified_die environment-blocked perf-call-graph-unavailable 2
  fi
  rm -f "$stat_output" "$record_output" "$record_data"
}

if [[ "$SCOPE" == full ]]; then
  check_perf_capability
fi

RUN_ROOT=$(mktemp -d "$RUN_PARENT/engine-writer-unified-campaign-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,derived,logs,monitors,perf,preflight,time,fio}

MOUNT_SOURCE=$(findmnt -no SOURCE -T "$RUN_ROOT") || \
  campaign_fail environment-blocked wal-mount-source-unavailable
BLOCK_DEVICE=$(lsblk -no PKNAME "$MOUNT_SOURCE" 2>/dev/null | awk 'NF {print; exit}')
if [[ "$MOUNT_SOURCE" != /dev/* ]]; then
  campaign_fail environment-blocked "wal-filesystem-not-block-backed-${MOUNT_SOURCE}"
fi
if [[ -z "$BLOCK_DEVICE" ]]; then
  BLOCK_DEVICE=$(basename "$MOUNT_SOURCE")
fi
[[ -n "$BLOCK_DEVICE" ]] || campaign_fail environment-blocked wal-block-device-unavailable

record_identity() {
  local output=$1
  {
    printf 'head='; git rev-parse HEAD
    git status --short
    printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
    printf 'worktree_diff_sha256='; git diff --binary | sha256sum
    printf 'untracked_manifest_sha256='
    git ls-files --others --exclude-standard -z | LC_ALL=C sort -z |
      xargs -0 -r sha256sum | sha256sum
  } >"$output"
}

record_identity "$RUN_ROOT/logs/repository-identity-before.txt"
sha256sum "$BENCHMARK_BINARY" >"$RUN_ROOT/logs/binary-sha256-before.txt"

check_frozen_artifacts() {
  local current_identity="$RUN_ROOT/logs/repository-identity-current.txt"
  local current_binary="$RUN_ROOT/logs/binary-sha256-current.txt"
  record_identity "$current_identity"
  sha256sum "$BENCHMARK_BINARY" >"$current_binary"
  if ! cmp -s "$current_identity" "$RUN_ROOT/logs/repository-identity-before.txt" ||
     ! cmp -s "$current_binary" "$RUN_ROOT/logs/binary-sha256-before.txt"; then
    printf 'result=invalid-run\nreason=artifact-identity-changed\n' \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  fi
}

{
  date --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS
  cat /proc/loadavg
  printf 'bench_cpus=%s\nobserver_cpus=%s\ncpu_list=%s\n' \
    "$BENCH_CPU_SET" "$OBSERVER_CPU_SET" "$CPU_LIST"
  printf 'mount_source=%s\nblock_device=%s\n' "$MOUNT_SOURCE" "$BLOCK_DEVICE"
} >"$RUN_ROOT/logs/environment-before.txt" 2>&1

capture_cpu_policy() {
  local cpu token first last field path
  IFS=',' read -r -a policy_cpus <<<"$CPU_LIST"
  for token in "${policy_cpus[@]}"; do
    if [[ "$token" == *-* ]]; then
      IFS='-' read -r first last <<<"$token"
    else
      first=$token
      last=$token
    fi
    for ((cpu=first; cpu<=last; ++cpu)); do
      for field in scaling_governor energy_performance_preference; do
        path="/sys/devices/system/cpu/cpu${cpu}/cpufreq/$field"
        if [[ -r "$path" ]]; then
          printf 'cpu=%s %s=' "$cpu" "$field"
          cat "$path"
        else
          printf 'cpu=%s %s=not_available\n' "$cpu" "$field"
        fi
      done
    done
  done
  if [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
    printf 'boost='; cat /sys/devices/system/cpu/cpufreq/boost
  else
    printf 'boost=not_available\n'
  fi
}
capture_cpu_policy >"$RUN_ROOT/logs/cpu-policy-before.txt"

run_preflight() {
  local label=$1
  local cpu_json="$RUN_ROOT/preflight/${label}-mpstat.json"
  local disk_json="$RUN_ROOT/preflight/${label}-iostat.json"
  local summary="$RUN_ROOT/preflight/${label}-summary.txt"
  taskset -c "$OBSERVER_CPU_SET" mpstat -P "$CPU_LIST" 1 "$PREFLIGHT_SECONDS" -o JSON >"$cpu_json" &
  local cpu_pid=$!
  taskset -c "$OBSERVER_CPU_SET" iostat -y -dx "$BLOCK_DEVICE" 1 "$PREFLIGHT_SECONDS" -o JSON >"$disk_json" &
  local disk_pid=$!
  local cpu_status=0
  local disk_status=0
  wait "$cpu_pid" || cpu_status=$?
  wait "$disk_pid" || disk_status=$?
  if [[ "$cpu_status" != 0 || "$disk_status" != 0 ]]; then
    printf 'result=preflight-busy\nmonitor_exit_cpu=%s\nmonitor_exit_disk=%s\n' \
      "$cpu_status" "$disk_status" >"$summary"
    return 1
  fi
  local cpu_idle_min cpu_iowait_max disk_util_avg disk_aqu_max
  cpu_idle_min=$(jq -r '
    [.sysstat.hosts[0].statistics[] | .["cpu-load"][]]
    | sort_by(.cpu) | group_by(.cpu)
    | map(map(.idle) | add / length) | min' "$cpu_json")
  cpu_iowait_max=$(jq -r '
    [.sysstat.hosts[0].statistics[] ["cpu-load"][] | .iowait] | max' "$cpu_json")
  disk_util_avg=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .util] | if length == 0 then null else add / length end' "$disk_json")
  disk_aqu_max=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]] | if length == 0 then null else max end' "$disk_json")
  printf 'cpu_idle_min=%s\ncpu_iowait_max=%s\ndisk_util_avg=%s\ndisk_aqu_max=%s\n' \
    "$cpu_idle_min" "$cpu_iowait_max" "$disk_util_avg" "$disk_aqu_max" >"$summary"
  awk -v value="$cpu_idle_min" 'BEGIN { exit !(value >= 90.0) }' || return 1
  awk -v value="$cpu_iowait_max" 'BEGIN { exit !(value <= 5.0) }' || return 1
  [[ "$cpu_idle_min" != null && "$cpu_iowait_max" != null &&
     "$disk_util_avg" != null && "$disk_aqu_max" != null ]] || return 1
  awk -v value="$disk_util_avg" 'BEGIN { exit !(value <= 5.0) }' || return 1
  awk -v value="$disk_aqu_max" 'BEGIN { exit !(value <= 0.25) }' || return 1
}

if [[ "$SCOPE" == full ]]; then
  if ! run_preflight initial; then
    printf 'result=preflight-busy\nreason=initial-preflight-busy\n' >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  fi
fi

OBSERVER_PIDS=()
OBSERVER_NAMES=()
OBSERVER_START_TIMES=()
stop_observers() {
  local index pid name start_time status process_state output_path
  local failure=0
  for index in "${!OBSERVER_PIDS[@]}"; do
    pid=${OBSERVER_PIDS[index]}
    name=${OBSERVER_NAMES[index]}
    start_time=${OBSERVER_START_TIMES[index]}
    process_state=$(ps -o stat= -p "$pid" 2>/dev/null || true)
    if [[ -n "$process_state" && "$process_state" != Z* ]]; then
      kill "$pid" 2>/dev/null || true
      status=0
      wait "$pid" 2>/dev/null || status=$?
      if [[ "$status" != 0 && "$status" != 143 && "$status" != 130 ]]; then
        failure=1
      fi
    else
      status=0
      wait "$pid" 2>/dev/null || status=$?
      failure=1
    fi
    printf '%s pid=%s started=%s status=%s\n' "$name" "$pid" "$start_time" "$status" \
      >>"$RUN_ROOT/logs/observer-status.txt"
    output_path="$RUN_ROOT/monitors/${name}.txt"
    [[ -s "$output_path" ]] || failure=1
  done
  OBSERVER_PIDS=()
  OBSERVER_NAMES=()
  OBSERVER_START_TIMES=()
  return "$failure"
}
trap 'stop_observers || true' EXIT INT TERM

start_observers() {
  local label=$1
  if [[ "$OBSERVER_MODE" != on ]]; then
    return 0
  fi
  taskset -c "$OBSERVER_CPU_SET" mpstat -P "$CPU_LIST" 1 "$MONITOR_SECONDS" \
    >"$RUN_ROOT/monitors/${label}-mpstat.txt" &
  OBSERVER_PIDS+=("$!")
  OBSERVER_NAMES+=("${label}-mpstat")
  OBSERVER_START_TIMES+=("$(date --iso-8601=ns)")
  taskset -c "$OBSERVER_CPU_SET" iostat -y -dx -t "$BLOCK_DEVICE" 1 "$MONITOR_SECONDS" \
    >"$RUN_ROOT/monitors/${label}-iostat.txt" &
  OBSERVER_PIDS+=("$!")
  OBSERVER_NAMES+=("${label}-iostat")
  OBSERVER_START_TIMES+=("$(date --iso-8601=ns)")
}

run_case() {
  local label=$1
  shift
  local case_iterations="$ITERATIONS"
  local tail_mode=off
  local allow_short=0
  while [[ "${1:-}" == --runner-* ]]; do
    case "$1" in
      --runner-iterations=*) case_iterations=${1#*=} ;;
      --runner-tail=*) tail_mode=${1#*=} ;;
      --runner-allow-short) allow_short=1 ;;
      *) die "unknown runner-internal option: $1" ;;
    esac
    shift
  done
  is_positive_integer "$case_iterations" || die "case iterations must be positive"
  [[ "$tail_mode" == off || "$tail_mode" == on ]] || die "invalid runner tail mode"
  check_frozen_artifacts
  run_preflight "${label}-preflight" || {
    printf 'result=preflight-busy\nfailed_case=%s\nreason=case-preflight-busy\n' "$label" \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  }
  local data_dir="$RUN_ROOT/data/$label"
  mkdir -p "$data_dir"
  local stdout="$RUN_ROOT/logs/${label}.stdout"
  local stderr="$RUN_ROOT/logs/${label}.stderr"
  local time_file="$RUN_ROOT/time/${label}.time"
  local perf_file="$RUN_ROOT/perf/${label}.stat"
  local status_file="$RUN_ROOT/logs/${label}.status"
  local durable_engine=0
  local argument
  for argument in "$@"; do
    if [[ "$argument" == --workload=engine_durable_single_instrument ]]; then
      durable_engine=1
      break
    fi
  done
  local command=(taskset -c "$BENCH_CPU_SET" timeout "${CASE_TIMEOUT_SECONDS}s"
                 "$BENCHMARK_BINARY" --iterations="$case_iterations" --warmup="$WARMUP"
                 --data-dir="$data_dir")
  if [[ "$durable_engine" == 1 && "$tail_mode" == on ]]; then
    command+=(
      "--engine-tail-telemetry-output=$RUN_ROOT/derived/${label}-engine-tail.csv"
      --engine-tail-state-sampling=on
    )
  fi
  command+=("$@")
  start_observers "$label"
  local status=0
  if [[ "$OBSERVER_MODE" == on ]]; then
    RUNNER_CASE_LABEL="$label" RUNNER_TAIL_MODE="$tail_mode" \
    /usr/bin/time -v -o "$time_file" perf stat --no-big-num -x, \
      -e task-clock,cycles,instructions,cache-misses,context-switches,cpu-migrations,page-faults \
      -o "$perf_file" -- "${command[@]}" >"$stdout" 2>"$stderr" || status=$?
  else
    RUNNER_CASE_LABEL="$label" RUNNER_TAIL_MODE="$tail_mode" \
    /usr/bin/time -v -o "$time_file" -- "${command[@]}" >"$stdout" 2>"$stderr" || status=$?
  fi
  local observer_status=0
  stop_observers || observer_status=$?
  printf '%s\n' "$status" >"$status_file"
  if [[ "$observer_status" != 0 ]]; then
    printf 'result=invalid-run\nfailed_case=%s\nreason=observer-failed\n' "$label" \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  fi
  if [[ "$status" == 0 && "$allow_short" == 0 ]] && ! validate_engine_duration "$stdout"; then
    printf 'result=invalid-run\nfailed_case=%s\nreason=measured-duration-under-30s\n' "$label" \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  fi
  check_frozen_artifacts
  write_normalized_row "$label" "$status" "$stdout"
  if [[ "$status" != 0 ]]; then
    local failure_reason=benchmark-exit-nonzero
    [[ "$status" == 124 ]] && failure_reason=benchmark-timeout
    printf 'result=invalid-run\nfailed_case=%s\nreason=%s\nexit_status=%s\n' \
      "$label" "$failure_reason" "$status" \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  fi
}

declare -A ENGINE_ITERATIONS_BY_CASE=()
ENGINE_PLAN_INITIALIZED=0

engine_case_iterations() {
  local case_name=$1
  local value=${ENGINE_ITERATIONS_BY_CASE[$case_name]:-}
  [[ -n "$value" ]] || campaign_fail invalid-run "missing-iteration-plan-${case_name}"
  printf '%s\n' "$value"
}

run_engine_case() {
  local label=$1
  local case_name=$2
  local tail_mode=$3
  local group=$4
  local delay=$5
  local allow_short=${6:-0}
  local iterations
  iterations=$(engine_case_iterations "$case_name")
  local -a runner_options=(
    --runner-iterations="$iterations"
    --runner-tail="$tail_mode"
  )
  [[ "$allow_short" == 1 ]] && runner_options+=(--runner-allow-short)
  run_case "$label" "${runner_options[@]}" \
    --workload=engine_durable_single_instrument --engine-group-size="$group" \
    --engine-group-delay-us="$delay" --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
}

extract_engine_elapsed_ms() {
  local output=$1
  grep -o 'elapsed_ms=[^ ]*' "$output" | tail -n 1 | cut -d= -f2
}

extract_engine_commands() {
  local output=$1
  grep -oE '(^|[[:space:]])commands=[^[:space:]]+' "$output" | tail -n 1 \
    | sed 's/^[[:space:]]*//' | cut -d= -f2
}

extract_engine_field() {
  local output=$1
  local field=$2
  grep -oE "(^|[[:space:]])${field}=[^[:space:]]+" "$output" | tail -n 1 \
    | sed 's/^[[:space:]]*//' | cut -d= -f2
}

run_observer_gate_case() {
  local mode=$1
  local round=$2
  local previous_mode=$OBSERVER_MODE
  OBSERVER_MODE=$mode
  run_engine_case "observer-gate-${mode}-r${round}" g256-d200 off 256 200
  OBSERVER_MODE=$previous_mode
}

extract_engine_rps() {
  local output=$1
  grep -o 'commands_per_second=[^ ]*' "$output" | tail -n 1 | cut -d= -f2
}

extract_user_seconds() {
  local time_file=$1
  awk -F: '/User time \(seconds\)/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' \
    "$time_file"
}

extract_system_seconds() {
  local time_file=$1
  awk -F: '/System time \(seconds\)/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' \
    "$time_file"
}

extract_total_cpu_seconds() {
  local time_file=$1
  local user_seconds system_seconds
  user_seconds=$(extract_user_seconds "$time_file")
  system_seconds=$(extract_system_seconds "$time_file")
  [[ -n "$user_seconds" && -n "$system_seconds" ]] || return 1
  awk -v user="$user_seconds" -v system_time="$system_seconds" \
    'BEGIN { printf "%.9f\n", user + system_time }'
}

prepare_engine_iteration_plan() {
  local requested_mode=$OBSERVER_MODE
  local entry group delay elapsed_ms commands expected_commands formal_iterations estimated_ms
  local -a cases=("${ENGINE_CASES[@]}")
  if (($# > 0)); then
    cases=("$@")
  fi
  local plan="$RUN_ROOT/derived/engine-iteration-plan.tsv"
  if [[ "$ENGINE_PLAN_INITIALIZED" == 0 ]]; then
    printf 'case\tcalibration_iterations\tcalibration_commands\tcalibration_elapsed_ms\tformal_iterations\testimated_duration_ms\tbinary_sha256\n' \
      >"$plan"
    ENGINE_PLAN_INITIALIZED=1
  fi
  OBSERVER_MODE=off
  for entry in "${cases[@]}"; do
    group=${entry#*g}
    group=${group%%-*}
    delay=${entry##*-d}
    run_case "engine-calibration-${entry}" --runner-iterations="$CALIBRATION_ITERATIONS" \
      --runner-tail=off --runner-allow-short \
      --workload=engine_durable_single_instrument --engine-group-size="$group" \
      --engine-group-delay-us="$delay" --engine-producer-lanes=8192 \
      --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
    elapsed_ms=$(extract_engine_elapsed_ms \
      "$RUN_ROOT/logs/engine-calibration-${entry}.stdout")
    commands=$(extract_engine_commands \
      "$RUN_ROOT/logs/engine-calibration-${entry}.stdout")
    [[ -n "$elapsed_ms" && -n "$commands" ]] || \
      campaign_fail invalid-run "calibration-output-incomplete-${entry}"
    expected_commands=$(decimal_double_u64 "$CALIBRATION_ITERATIONS") || \
      campaign_fail invalid-run "calibration-command-count-overflow-${entry}"
    [[ "$commands" == "$expected_commands" ]] || \
      campaign_fail invalid-run "calibration-command-count-mismatch-${entry}"
    awk -v elapsed="$elapsed_ms" \
      'BEGIN { exit !(elapsed > 0) }' || \
      campaign_fail invalid-run "calibration-output-invalid-${entry}"
    formal_iterations=$(awk -v base="$CALIBRATION_ITERATIONS" -v elapsed="$elapsed_ms" \
      -v target="$ENGINE_TARGET_DURATION_MS" \
      'BEGIN {
         raw = base * target / elapsed * 1.10
         value = int(raw)
         if (value < raw) ++value
         if (value < 1 || value > 9223372036854775807) exit 1
         printf "%.0f\n", value
       }') || campaign_fail invalid-run "calibration-iterations-invalid-${entry}"
    decimal_double_u64 "$formal_iterations" >/dev/null || \
      campaign_fail invalid-run "calibration-iterations-overflow-${entry}"
    estimated_ms=$(awk -v base="$CALIBRATION_ITERATIONS" -v elapsed="$elapsed_ms" \
      -v formal="$formal_iterations" \
      'BEGIN { printf "%.0f\n", elapsed * formal / base }')
    awk -v estimated="$estimated_ms" -v timeout="$CASE_TIMEOUT_SECONDS" \
      'BEGIN { exit !(estimated < (timeout * 1000.0 - 30000.0)) }' || \
      campaign_fail invalid-run "calibration-exceeds-timeout-${entry}"
    ENGINE_ITERATIONS_BY_CASE[$entry]=$formal_iterations
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$entry" "$CALIBRATION_ITERATIONS" \
      "$commands" "$elapsed_ms" "$formal_iterations" "$estimated_ms" \
      "$(cut -d' ' -f1 "$RUN_ROOT/logs/binary-sha256-before.txt")" >>"$plan"
  done
  OBSERVER_MODE=$requested_mode
}

median_file_values() {
  sort -n | awk '
    { values[NR] = $1 }
    END {
      if (NR == 0) exit 1
      if (NR % 2 == 1) print values[(NR + 1) / 2]
      else print (values[NR / 2] + values[NR / 2 + 1]) / 2
    }'
}

validate_engine_duration() {
  local output=$1
  if ! grep -q '^engine_durable_single_instrument ' "$output"; then
    return 0
  fi
  local elapsed_ms
  elapsed_ms=$(grep -o 'elapsed_ms=[^ ]*' "$output" | tail -n 1 | cut -d= -f2)
  [[ -n "$elapsed_ms" ]] || return 1
  awk -v elapsed="$elapsed_ms" -v minimum="$MIN_ENGINE_DURATION_MS" \
    'BEGIN { exit !(elapsed >= minimum) }'
}

write_normalized_row() {
  local label=$1
  local status=$2
  local output=$3
  local summary
  summary=$(grep -E '^(engine_durable_single_instrument|wal_write_ceiling|engine_pipeline_ceiling|workload=engine_writer_hot_path_profile)' \
    "$output" | tail -n 1 || true)
  printf 'label=%s status=%s %s\n' "$label" "$status" "$summary" \
    >"$RUN_ROOT/derived/${label}.row"
}

run_observer_overhead_gate() {
  local requested_mode=$OBSERVER_MODE
  local mode round
  for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      for mode in off on; do
        run_observer_gate_case "$mode" "$round"
      done
    else
      for mode in on off; do
        run_observer_gate_case "$mode" "$round"
      done
    fi
  done
  OBSERVER_MODE=$requested_mode

  for mode in off on; do
    for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
      local output="$RUN_ROOT/logs/observer-gate-${mode}-r${round}.stdout"
      local time_file="$RUN_ROOT/time/observer-gate-${mode}-r${round}.time"
      [[ -s "$output" && -s "$time_file" ]] || campaign_fail invalid-run "observer-gate-artifact-${mode}-r${round}"
      [[ -n "$(extract_engine_rps "$output")" ]] || campaign_fail invalid-run "observer-gate-rps-${mode}-r${round}"
      [[ -n "$(extract_engine_commands "$output")" ]] || campaign_fail invalid-run "observer-gate-commands-${mode}-r${round}"
      [[ -n "$(extract_total_cpu_seconds "$time_file")" ]] || campaign_fail invalid-run "observer-gate-cpu-${mode}-r${round}"
    done
  done

  local off_rps on_rps off_cpu on_cpu off_commands on_commands
  local observer_rows="$RUN_ROOT/preflight/observer-overhead.tsv"
  printf 'mode\tround\trps\tcommands\tcpu_seconds_per_million\n' >"$observer_rows"
  for mode in off on; do
    for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
      local raw_output="$RUN_ROOT/logs/observer-gate-${mode}-r${round}.stdout"
      local raw_time="$RUN_ROOT/time/observer-gate-${mode}-r${round}.time"
      local raw_commands raw_cpu raw_rps
      raw_rps=$(extract_engine_rps "$raw_output") || \
        campaign_fail invalid-run "observer-gate-rps-${mode}-r${round}"
      raw_commands=$(extract_engine_commands "$raw_output") || \
        campaign_fail invalid-run "observer-gate-commands-${mode}-r${round}"
      raw_cpu=$(extract_total_cpu_seconds "$raw_time") || \
        campaign_fail invalid-run "observer-gate-cpu-${mode}-r${round}"
      printf '%s\t%s\t%s\t%s\t%s\n' "$mode" "$round" "$raw_rps" "$raw_commands" \
        "$(awk -v cpu="$raw_cpu" -v commands="$raw_commands" \
          'BEGIN { if (commands <= 0) exit 1; print cpu / commands * 1000000 }')" \
        >>"$observer_rows"
    done
  done
  off_rps=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    extract_engine_rps "$RUN_ROOT/logs/observer-gate-off-r${round}.stdout"
  done | median_file_values)
  on_rps=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    extract_engine_rps "$RUN_ROOT/logs/observer-gate-on-r${round}.stdout"
  done | median_file_values)
  off_commands=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    extract_engine_commands "$RUN_ROOT/logs/observer-gate-off-r${round}.stdout"
  done | median_file_values)
  on_commands=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    extract_engine_commands "$RUN_ROOT/logs/observer-gate-on-r${round}.stdout"
  done | median_file_values)
  off_cpu=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    cpu=$(extract_total_cpu_seconds "$RUN_ROOT/time/observer-gate-off-r${round}.time")
    commands=$(extract_engine_commands "$RUN_ROOT/logs/observer-gate-off-r${round}.stdout")
    awk -v cpu="$cpu" -v commands="$commands" \
      'BEGIN { if (commands <= 0) exit 1; print cpu / commands * 1000000 }'
  done | median_file_values)
  on_cpu=$(for ((round=1; round<=OBSERVER_GATE_ROUNDS; ++round)); do
    cpu=$(extract_total_cpu_seconds "$RUN_ROOT/time/observer-gate-on-r${round}.time")
    commands=$(extract_engine_commands "$RUN_ROOT/logs/observer-gate-on-r${round}.stdout")
    awk -v cpu="$cpu" -v commands="$commands" \
      'BEGIN { if (commands <= 0) exit 1; print cpu / commands * 1000000 }'
  done | median_file_values)
  local rps_bias cpu_bias
  rps_bias=$(awk -v off="$off_rps" -v on="$on_rps" \
    'BEGIN { if (off <= 0) exit 1; value = (on - off) / off * 100; if (value < 0) value = -value; print value }') || \
    campaign_fail invalid-run observer-gate-rps-bias
  cpu_bias=$(awk -v off="$off_cpu" -v on="$on_cpu" \
    'BEGIN { if (off <= 0) exit 1; value = (on - off) / off * 100; if (value < 0) value = -value; print value }') || \
    campaign_fail invalid-run observer-gate-cpu-bias
  printf 'rps_off_median=%s\nrps_on_median=%s\n' "$off_rps" "$on_rps" \
    >"$RUN_ROOT/preflight/observer-overhead-summary.txt"
  printf 'commands_off_median=%s\ncommands_on_median=%s\n' "$off_commands" "$on_commands" \
    >>"$RUN_ROOT/preflight/observer-overhead-summary.txt"
  printf 'cpu_seconds_per_million_off_median=%s\ncpu_seconds_per_million_on_median=%s\n' \
    "$off_cpu" "$on_cpu" >>"$RUN_ROOT/preflight/observer-overhead-summary.txt"
  printf 'rps_bias_percent=%s\ncpu_seconds_per_million_bias_percent=%s\n' \
    "$rps_bias" "$cpu_bias" >>"$RUN_ROOT/preflight/observer-overhead-summary.txt"
  if ! awk -v rps="$rps_bias" -v cpu="$cpu_bias" -v limit="$OBSERVER_BIAS_LIMIT_PERCENT" \
      'BEGIN { exit !(rps <= limit && cpu <= limit) }'; then
    printf 'result=observer-biased\n' >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  fi
}

run_pipeline_cases() {
  local round index stage
  local -a stages=("${PIPELINE_STAGES[@]}")
  for ((round=1; round<=PIPELINE_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      for stage in "${stages[@]}"; do
        run_case "pipeline-r${round}-${stage}" \
          --workload=engine_pipeline_ceiling --pipeline-stage="$stage" \
          --pipeline-batch-size=256 --pipeline-producer-lanes=64
      done
    else
      for ((index=${#stages[@]} - 1; index >= 0; --index)); do
        stage=${stages[index]}
        run_case "pipeline-r${round}-${stage}" \
          --workload=engine_pipeline_ceiling --pipeline-stage="$stage" \
          --pipeline-batch-size=256 --pipeline-producer-lanes=64
      done
    fi
  done
}

run_wal_cases() {
  local round index entry sync group
  local -a cases=("${WAL_CASES[@]}")
  for ((round=1; round<=WAL_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      for entry in "${cases[@]}"; do
        sync=${entry%%:*}
        group=${entry#*:}
        run_case "wal-r${round}-${sync}-g${group}" \
          --workload=wal_write_ceiling --wal-group-size="$group" --wal-sync="$sync" \
          --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096 \
          --wal-phase-profile=on
      done
    else
      for ((index=${#cases[@]} - 1; index >= 0; --index)); do
        entry=${cases[index]}
        sync=${entry%%:*}
        group=${entry#*:}
        run_case "wal-r${round}-${sync}-g${group}" \
          --workload=wal_write_ceiling --wal-group-size="$group" --wal-sync="$sync" \
          --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096 \
          --wal-phase-profile=on
      done
    fi
  done
}

run_engine_frontier() {
  local round index entry group delay
  local -a cases=("${ENGINE_CASES[@]}")
  for ((round=1; round<=FRONTIER_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      for entry in "${cases[@]}"; do
        group=${entry#*g}; group=${group%%-*}
        delay=${entry##*-d}
        run_engine_case "engine-scan-r${round}-${entry}" "$entry" off "$group" "$delay" 1
      done
    else
      for ((index=${#cases[@]} - 1; index >= 0; --index)); do
        entry=${cases[index]}
        group=${entry#*g}; group=${group%%-*}
        delay=${entry##*-d}
        run_engine_case "engine-scan-r${round}-${entry}" "$entry" off "$group" "$delay" 1
      done
    fi
  done
}

FRONTIER_REJECTION_REASON=invalid-run
FRONTIER_REASON_FILE=""

set_frontier_rejection_reason() {
  FRONTIER_REJECTION_REASON=$1
  if [[ -n "$FRONTIER_REASON_FILE" ]]; then
    printf '%s\n' "$FRONTIER_REJECTION_REASON" >"$FRONTIER_REASON_FILE"
  fi
}

validate_frontier_case() {
  local entry=$1
  local round status output row time_file perf_file rps elapsed commands group_commits group_commands wal_bytes
  local -a rps_values=()
  for ((round=1; round<=FRONTIER_ROUNDS; ++round)); do
    status=$(<"$RUN_ROOT/logs/engine-scan-r${round}-${entry}.status") || {
      set_frontier_rejection_reason missing-artifact
      return 1
    }
    [[ "$status" == 0 ]] || {
      set_frontier_rejection_reason benchmark-failed
      return 1
    }
    output="$RUN_ROOT/logs/engine-scan-r${round}-${entry}.stdout"
    row="$RUN_ROOT/derived/engine-scan-r${round}-${entry}.row"
    time_file="$RUN_ROOT/time/engine-scan-r${round}-${entry}.time"
    [[ -s "$output" && -s "$row" && -s "$time_file" ]] || {
      set_frontier_rejection_reason missing-artifact
      return 1
    }
    if [[ "$OBSERVER_MODE" == on ]]; then
      perf_file="$RUN_ROOT/perf/engine-scan-r${round}-${entry}.stat"
      [[ -s "$perf_file" ]] || {
        set_frontier_rejection_reason missing-artifact
        return 1
      }
      [[ -s "$RUN_ROOT/monitors/engine-scan-r${round}-${entry}-mpstat.txt" ]] || {
        set_frontier_rejection_reason missing-artifact
        return 1
      }
      [[ -s "$RUN_ROOT/monitors/engine-scan-r${round}-${entry}-iostat.txt" ]] || {
        set_frontier_rejection_reason missing-artifact
        return 1
      }
    fi
    rps=$(extract_engine_rps "$output")
    elapsed=$(extract_engine_elapsed_ms "$output")
    commands=$(extract_engine_commands "$output")
    group_commits=$(extract_engine_field "$output" wal_group_commits)
    group_commands=$(extract_engine_field "$output" wal_group_commands)
    wal_bytes=$(extract_engine_field "$output" wal_bytes_delta)
    [[ -n "$rps" && -n "$elapsed" && -n "$commands" && -n "$group_commits" &&
      -n "$group_commands" && -n "$wal_bytes" ]] || {
      set_frontier_rejection_reason counter-invalid
      return 1
    }
    local formal_iterations expected_commands
    formal_iterations=$(awk -v case_name="$entry" -F '\t' \
      '$1 == case_name { print $5; found=1 } END { if (!found) exit 1 }' \
      "$RUN_ROOT/derived/engine-iteration-plan.tsv") || {
      set_frontier_rejection_reason counter-invalid
      return 1
    }
    expected_commands=$(decimal_double_u64 "$formal_iterations") || {
      set_frontier_rejection_reason counter-invalid
      return 1
    }
    if [[ "$commands" != "$expected_commands" ]]; then
      set_frontier_rejection_reason command-count-mismatch
      return 1
    fi
    awk -v rps="$rps" -v commands="$commands" -v expected="$expected_commands" \
      -v group_commands="$group_commands" -v group_commits="$group_commits" \
      -v wal_bytes="$wal_bytes" \
      'BEGIN { exit !(rps > 0 && commands == expected && group_commands == commands &&
                      group_commits > 0 && wal_bytes > 0) }' || {
      set_frontier_rejection_reason counter-invalid
      return 1
    }
    awk -v elapsed="$elapsed" -v minimum="$MIN_ENGINE_DURATION_MS" \
      'BEGIN { exit !(elapsed >= minimum) }' || {
      set_frontier_rejection_reason duration-under-30s
      return 1
    }
    rps_values+=("$rps")
  done
  local cv median
  cv=$(printf '%s\n' "${rps_values[@]}" | awk '
    { sum += $1; values[NR] = $1 }
    END {
      if (NR == 0 || sum <= 0) exit 1
      mean = sum / NR
      for (i = 1; i <= NR; ++i) variance += (values[i] - mean) ^ 2
      printf "%.6f\n", sqrt(variance / NR) / mean * 100
    }') || {
    set_frontier_rejection_reason counter-invalid
    return 1
  }
  awk -v cv="$cv" 'BEGIN { exit !(cv <= 5.0) }' || {
    set_frontier_rejection_reason cv-over-5-percent
    return 1
  }
  median=$(printf '%s\n' "${rps_values[@]}" | median_file_values)
  printf '%s %s %s\n' "$median" "$entry" "$cv"
}

select_engine_frontier_case() {
  local entry value median candidate cv best_case="" best_value="" rank reason
  local -a cases=("${ENGINE_CASES[@]}")
  : >"$RUN_ROOT/derived/frontier-selection.tsv"
  : >"$RUN_ROOT/derived/frontier-selection.txt"
  : >"$RUN_ROOT/derived/frontier-candidates.txt"
  printf 'case\tstatus\tmedian_rps\tcv_percent\trank\treason\n' \
  >"$RUN_ROOT/derived/frontier-selection.tsv"
  printf 'case\tstatus\tmedian_rps\tcv_percent\trank\treason\n' \
    >"$RUN_ROOT/derived/frontier-selection.txt"
  for entry in "${cases[@]}"; do
    FRONTIER_REJECTION_REASON=invalid-run
    FRONTIER_REASON_FILE="$RUN_ROOT/derived/frontier-${entry}.reason"
    : >"$FRONTIER_REASON_FILE"
    if ! value=$(validate_frontier_case "$entry"); then
      reason=$(<"$FRONTIER_REASON_FILE")
      [[ -n "$reason" ]] || reason=invalid-run
      rm -f "$FRONTIER_REASON_FILE"
      printf '%s\texcluded\tna\tna\tna\t%s\n' "$entry" "$reason" \
        >>"$RUN_ROOT/derived/frontier-selection.tsv"
      printf '%s\texcluded\tna\tna\tna\t%s\n' "$entry" "$reason" \
        >>"$RUN_ROOT/derived/frontier-selection.txt"
      continue
    fi
    rm -f "$FRONTIER_REASON_FILE"
    read -r median candidate cv <<<"$value"
    printf '%s %s %s\n' "$median" "$candidate" "$cv" \
      >>"$RUN_ROOT/derived/frontier-candidates.txt"
  done
  rank=1
  while read -r median candidate cv; do
    [[ -n "$candidate" ]] || continue
    printf '%s\tvalid\t%s\t%s\t%s\tna\n' "$candidate" "$median" "$cv" "$rank" \
      >>"$RUN_ROOT/derived/frontier-selection.tsv"
    printf '%s\tvalid\t%s\t%s\t%s\tna\n' "$candidate" "$median" "$cv" "$rank" \
      >>"$RUN_ROOT/derived/frontier-selection.txt"
    if [[ -z "$best_value" ]]; then
      best_case=$candidate
      best_value=$median
    fi
    ((rank += 1))
  done < <(sort -k1,1nr "$RUN_ROOT/derived/frontier-candidates.txt")
  [[ -n "$best_case" ]] || {
    printf 'best_case=na\nbest_median_rps=na\n' >>"$RUN_ROOT/derived/frontier-selection.txt"
    return 1
  }
  printf 'best_case=%s\nbest_median_rps=%s\n' "$best_case" "$best_value" \
    >>"$RUN_ROOT/derived/frontier-selection.txt"
  printf '%s %s\n' "$best_case" "$best_value"
}

validate_tail_csv() {
  local csv=$1
  local group=$2
  local label=$3
  local validation_file=${4:-$RUN_ROOT/derived/tail-gate-${label}-validation.txt}
  [[ -s "$csv" ]] || {
    printf '%s reason=missing-tail-csv\n' "$label" >>"$validation_file"
    return 20
  }
  local metrics
  local metrics_status=0
  metrics=$(awk -F, -v group="$group" '
    BEGIN {
      expected_header = "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns"
    }
    function sort_values(a, n, i, j, tmp) {
      for (i = 1; i <= n; ++i) {
        for (j = i + 1; j <= n; ++j) {
          if (a[j] < a[i]) { tmp = a[i]; a[i] = a[j]; a[j] = tmp }
        }
      }
    }
    NR == 1 {
      if ($0 != expected_header) malformed = 1
      next
    }
    $1 == "state" && $2 == "measured" {
      if (NF != 8 || $3 !~ /^[0-9]+$/ || $6 !~ /^[0-9]+$/) {
        malformed = 1
        next
      }
      ++n
      times[n] = $3 + 0
      lags[n] = $6 + 0
      sx += times[n]
      sy += lags[n]
      sxx += times[n] * times[n]
      sxy += times[n] * lags[n]
    }
    END {
      if (malformed) exit 3
      if (n < 10) exit 1
      first_n = int((n + 3) / 4)
      last_start = int(3 * n / 4) + 1
      last_n = n - last_start + 1
      for (i = 1; i <= first_n; ++i) first[i] = lags[i]
      for (i = last_start; i <= n; ++i) last[i - last_start + 1] = lags[i]
      sort_values(first, first_n)
      sort_values(last, last_n)
      first_median = first[int((first_n + 1) / 2)]
      last_median = last[int((last_n + 1) / 2)]
      denominator = n * sxx - sx * sx
      if (denominator <= 0) exit 3
      slope = (n * sxy - sx * sy) / denominator
      increment_30s = slope > 0 ? slope * 30000000 : 0
      printf "state_samples=%d first_quartile_lag_median=%d last_quartile_lag_median=%d slope_per_us=%.9f increment_30s=%.3f\n", \
        n, first_median, last_median, slope, increment_30s
      if (last_median > first_median + group || increment_30s > group) exit 2
    }' "$csv") || metrics_status=$?
  if [[ "$metrics_status" == 2 ]]; then
    printf '%s reason=positive-backlog-slope\n' "$label" \
      >>"$validation_file"
    return 10
  fi
  if [[ "$metrics_status" != 0 ]]; then
    printf '%s reason=malformed-tail-csv\n' "$label" >>"$validation_file"
    return 20
  fi
  printf '%s %s\n' "$label" "$metrics" \
    >>"$validation_file"
}

write_tail_rejection_summary() {
  local entry=$1
  local reason=$2
  printf 'case=%s\nstatus=rejected\nreason=%s\n' "$entry" "$reason" \
    >"$RUN_ROOT/derived/tail-gate-${entry}.summary"
}

write_tail_invalid_summary() {
  local entry=$1
  local reason=$2
  printf 'case=%s\nstatus=invalid\nreason=%s\n' "$entry" "$reason" \
    >"$RUN_ROOT/derived/tail-gate-${entry}.summary"
}

validate_tail_engine_output() {
  local output=$1
  local expected_commands=$2
  local rps elapsed commands group_commits group_commands wal_bytes
  rps=$(extract_engine_rps "$output") || return 1
  elapsed=$(extract_engine_elapsed_ms "$output") || return 1
  commands=$(extract_engine_commands "$output") || return 1
  group_commits=$(extract_engine_field "$output" wal_group_commits) || return 1
  group_commands=$(extract_engine_field "$output" wal_group_commands) || return 1
  wal_bytes=$(extract_engine_field "$output" wal_bytes_delta) || return 1
  [[ -n "$rps" && -n "$elapsed" && -n "$commands" && -n "$group_commits" &&
    -n "$group_commands" && -n "$wal_bytes" && "$commands" == "$expected_commands" ]] ||
    return 1
  awk -v rps="$rps" -v elapsed="$elapsed" -v commands="$commands" \
    -v expected="$expected_commands" -v group_commits="$group_commits" \
    -v group_commands="$group_commands" -v wal_bytes="$wal_bytes" \
    -v minimum="$MIN_ENGINE_DURATION_MS" \
    'BEGIN { exit !(rps > 0 && elapsed >= minimum && commands == expected &&
                    group_commands == commands && group_commits > 0 && wal_bytes > 0) }'
}

run_tail_attribution_impl() {
  local entry=$1
  local round mode group delay label csv output
  local off_rps on_rps off_cpu on_cpu rps_bias cpu_bias
  local -a modes
  group=${entry#*g}; group=${group%%-*}
  delay=${entry##*-d}
  local formal_iterations expected_commands
  formal_iterations=$(awk -v case_name="$entry" -F '\t' \
    '$1 == case_name { print $5; found=1 } END { if (!found) exit 1 }' \
    "$RUN_ROOT/derived/engine-iteration-plan.tsv") || {
    write_tail_invalid_summary "$entry" counter-invalid
    return 20
  }
  expected_commands=$(decimal_double_u64 "$formal_iterations") || {
    write_tail_invalid_summary "$entry" counter-invalid
    return 20
  }
  local validation_file="$RUN_ROOT/derived/tail-gate-${entry}-validation.txt"
  : >"$validation_file"
  for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      modes=(off on)
    else
      modes=(on off)
    fi
    for mode in "${modes[@]}"; do
      label="tail-gate-r${round}-${entry}-${mode}"
      run_engine_case "$label" "$entry" "$mode" "$group" "$delay"
      output="$RUN_ROOT/logs/${label}.stdout"
      if [[ ! -s "$output" || ! -s "$RUN_ROOT/time/${label}.time" ]]; then
        write_tail_invalid_summary "$entry" missing-artifact
        return 20
      fi
      if ! validate_tail_engine_output "$output" "$expected_commands"; then
        write_tail_invalid_summary "$entry" counter-invalid
        return 20
      fi
      if [[ "$mode" == on ]]; then
        csv="$RUN_ROOT/derived/${label}-engine-tail.csv"
        if ! grep -Eq '(^|[[:space:]])tail_telemetry=on([[:space:]]|$)' "$output" ||
           ! grep -Eq '(^|[[:space:]])tail_state_sampling=on([[:space:]]|$)' "$output" ||
           ! grep -Eq '(^|[[:space:]])telemetry_dropped_samples=0([[:space:]]|$)' "$output" ||
           ! grep -F "tail_telemetry_file=$csv" "$output" >/dev/null; then
          write_tail_invalid_summary "$entry" telemetry-dropped
          return 20
        fi
        if ! grep -Eq 'drain_publisher_lag_events_last=0' "$output" ||
           ! grep -Eq 'drain_publisher_lag_bytes_last=0' "$output" ||
           ! grep -Eq 'drain_publisher_lag_age_ns_last=0' "$output"; then
          printf '%s reason=drain-not-empty\n' "$label" \
            >>"$validation_file"
          write_tail_rejection_summary "$entry" drain-not-empty
          return 10
        fi
        validate_tail_csv "$csv" "$group" "$label" "$validation_file"
        case $? in
          0) ;;
          10)
            write_tail_rejection_summary "$entry" positive-backlog-slope
            return 10
            ;;
          *)
            write_tail_invalid_summary "$entry" malformed-tail-csv
            return 20
            ;;
        esac
      fi
    done
  done
  for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    if [[ -z "$(extract_engine_rps "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-off.stdout")" ||
          -z "$(extract_engine_rps "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-on.stdout")" ||
          -z "$(extract_engine_commands "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-off.stdout")" ||
          -z "$(extract_engine_commands "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-on.stdout")" ]]; then
      write_tail_invalid_summary "$entry" counter-invalid
      return 20
    fi
  done
  off_rps=$(for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    extract_engine_rps "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-off.stdout"
  done | median_file_values) || {
    write_tail_invalid_summary "$entry" counter-invalid
    return 20
  }
  on_rps=$(for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    extract_engine_rps "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-on.stdout"
  done | median_file_values) || {
    write_tail_invalid_summary "$entry" counter-invalid
    return 20
  }
  off_cpu=$(for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    cpu=$(extract_total_cpu_seconds "$RUN_ROOT/time/tail-gate-r${round}-${entry}-off.time")
    commands=$(extract_engine_commands "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-off.stdout")
    awk -v cpu="$cpu" -v commands="$commands" \
      'BEGIN { if (commands <= 0) exit 1; print cpu / commands * 1000000 }'
  done | median_file_values) || {
    write_tail_invalid_summary "$entry" cpu-invalid
    return 20
  }
  on_cpu=$(for ((round=1; round<=TAIL_ATTRIBUTION_ROUNDS; ++round)); do
    cpu=$(extract_total_cpu_seconds "$RUN_ROOT/time/tail-gate-r${round}-${entry}-on.time")
    commands=$(extract_engine_commands "$RUN_ROOT/logs/tail-gate-r${round}-${entry}-on.stdout")
    awk -v cpu="$cpu" -v commands="$commands" \
      'BEGIN { if (commands <= 0) exit 1; print cpu / commands * 1000000 }'
  done | median_file_values) || {
    write_tail_invalid_summary "$entry" cpu-invalid
    return 20
  }
  rps_bias=$(awk -v off="$off_rps" -v on="$on_rps" \
    'BEGIN { if (off <= 0) exit 1; value = (on-off)/off*100; print value < 0 ? -value : value }') || {
    write_tail_invalid_summary "$entry" bias-invalid
    return 20
  }
  cpu_bias=$(awk -v off="$off_cpu" -v on="$on_cpu" \
    'BEGIN { if (off <= 0) exit 1; value = (on-off)/off*100; print value < 0 ? -value : value }') || {
    write_tail_invalid_summary "$entry" bias-invalid
    return 20
  }
  [[ -n "$rps_bias" && -n "$cpu_bias" ]] || {
    write_tail_invalid_summary "$entry" bias-invalid
    return 20
  }
  printf 'case=%s\nstatus=accepted\nrps_off_median=%s\nrps_on_median=%s\ncpu_seconds_per_million_off_median=%s\ncpu_seconds_per_million_on_median=%s\nrps_bias_percent=%s\ncpu_seconds_per_million_bias_percent=%s\n' \
    "$entry" "$off_rps" "$on_rps" "$off_cpu" "$on_cpu" "$rps_bias" "$cpu_bias" \
    >"$RUN_ROOT/derived/tail-gate-${entry}.summary"
  if ! awk -v rps="$rps_bias" -v cpu="$cpu_bias" -v limit="$TAIL_BIAS_LIMIT_PERCENT" \
      'BEGIN { exit !(rps <= limit && cpu <= limit) }'; then
    sed -i 's/^status=accepted$/status=rejected\nreason=tail-observer-bias/' \
      "$RUN_ROOT/derived/tail-gate-${entry}.summary"
    return 11
  fi
}

declare -A TAIL_ATTRIBUTION_STATUS_BY_CASE=()

run_tail_attribution() {
  local entry=$1
  local requested_mode=$OBSERVER_MODE
  local status=0
  if [[ "${TAIL_ATTRIBUTION_STATUS_BY_CASE[$entry]+present}" == present ]]; then
    return "${TAIL_ATTRIBUTION_STATUS_BY_CASE[$entry]}"
  fi
  OBSERVER_MODE=off
  run_tail_attribution_impl "$entry" || status=$?
  OBSERVER_MODE=$requested_mode
  TAIL_ATTRIBUTION_STATUS_BY_CASE[$entry]=$status
  return "$status"
}

run_engine_confirmations() {
  local best_case=$1
  local round entry index group delay
  local -a cases=(g256-d200 g4096-d1000 "$best_case")
  declare -A seen=()
  for entry in "${cases[@]}"; do
    [[ -n "${seen[$entry]:-}" ]] && continue
    seen[$entry]=1
    group=${entry#*g}; group=${group%%-*}
    delay=${entry##*-d}
    for ((round=1; round<=CONFIRMATION_ROUNDS; ++round)); do
      run_engine_case "engine-confirm-r${round}-${entry}" "$entry" off "$group" "$delay"
    done
    local confirmation_cv
    confirmation_cv=$(for ((round=1; round<=CONFIRMATION_ROUNDS; ++round)); do
      extract_engine_rps "$RUN_ROOT/logs/engine-confirm-r${round}-${entry}.stdout"
    done | awk '
      { sum += $1; values[NR] = $1 }
      END {
        if (NR == 0 || sum <= 0) exit 1
        mean = sum / NR
        for (i = 1; i <= NR; ++i) variance += (values[i] - mean) ^ 2
        print sqrt(variance / NR) / mean * 100
      }') || campaign_fail invalid-run "confirmation-cv-${entry}"
    awk -v cv="$confirmation_cv" 'BEGIN { exit !(cv <= 5.0) }' || \
      campaign_fail invalid-run "confirmation-cv-over-5-percent-${entry}-${confirmation_cv}"
  done
}

run_writer_profile_case() {
  local entry=$1
  local group=${entry#*g}; group=${group%%-*}
  local delay=${entry##*-d}
  local iterations
  iterations=$(engine_case_iterations "$entry")
  run_case "writer-profile-${entry}-off" --runner-iterations="$iterations" \
    --workload=engine_writer_hot_path_profile --writer-phase-profile=off \
    --engine-group-size="$group" --engine-group-delay-us="$delay" --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
  run_case "writer-profile-${entry}-on" --runner-iterations="$iterations" \
    --workload=engine_writer_hot_path_profile --writer-phase-profile=on \
    --writer-profile-sample-every=16 --engine-group-size="$group" \
    --engine-group-delay-us="$delay" --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
}

run_writer_profile() {
  local best_case=$1
  local entry
  local -a cases=(g256-d200 g4096-d1000 "$best_case")
  declare -A seen=()
  for entry in "${cases[@]}"; do
    [[ -n "${seen[$entry]:-}" ]] && continue
    seen[$entry]=1
    run_writer_profile_case "$entry"
  done
}

run_perf_record() {
  local label="perf-g${PROFILE_GROUP_SIZE}-d${PROFILE_DELAY_US}"
  local profile_case="g${PROFILE_GROUP_SIZE}-d${PROFILE_DELAY_US}"
  local profile_iterations
  if [[ -z "${ENGINE_ITERATIONS_BY_CASE[$profile_case]:-}" ]]; then
    prepare_engine_iteration_plan "$profile_case"
  fi
  profile_iterations=$(engine_case_iterations "$profile_case")
  check_frozen_artifacts
  run_preflight "$label-preflight" || {
    printf 'result=preflight-busy\nfailed_case=%s\n' "$label" >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  }
  local data_dir="$RUN_ROOT/data/$label"
  mkdir -p "$data_dir"
  local command=(taskset -c "$BENCH_CPU_SET" timeout "${CASE_TIMEOUT_SECONDS}s"
                 "$BENCHMARK_BINARY" --iterations="$profile_iterations" --warmup="$WARMUP"
                 --data-dir="$data_dir" --workload=engine_writer_hot_path_profile
                 --writer-phase-profile=on --writer-profile-sample-every=16
                 --engine-group-size="$PROFILE_GROUP_SIZE" --engine-group-delay-us="$PROFILE_DELAY_US"
                 --engine-producer-lanes=8192 --wal-prepare-workers=2
                 --wal-parallel-prepare-min-commands=4096)
  RUNNER_CASE_LABEL="$label" RUNNER_TAIL_MODE=off \
  perf record -F 99 -g --call-graph fp -o "$RUN_ROOT/perf/${label}.data" \
    -- "${command[@]}" >"$RUN_ROOT/logs/${label}.stdout" \
    2>"$RUN_ROOT/logs/${label}.stderr" || {
      printf 'result=invalid-run\nfailed_case=%s\n' "$label" >"$RUN_ROOT/logs/result.txt"
      printf 'run_root=%s\n' "$RUN_ROOT"
      exit 4
    }
  [[ -s "$RUN_ROOT/perf/${label}.data" ]] || campaign_fail invalid-run "perf-record-empty-${label}"
  if ! perf report --stdio --sort comm,symbol -i "$RUN_ROOT/perf/${label}.data" \
      >"$RUN_ROOT/perf/${label}.report.txt" 2>"$RUN_ROOT/logs/${label}.report.stderr"; then
    printf 'result=invalid-run\nfailed_case=%s\nreason=perf-report-failed\n' "$label" \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  fi
  check_frozen_artifacts
}

run_fio_control() {
  local fio_block label filename
  IFS=',' read -r -a FIO_BLOCK_LIST <<<"$FIO_BLOCK_BYTES"
  for fio_block in "${FIO_BLOCK_LIST[@]}"; do
    check_frozen_artifacts
    label="fio-bs-${fio_block}"
    filename="$RUN_ROOT/fio/${label}.dat"
    if ! fio --name="$label" --filename="$filename" --size=1G --rw=write \
        --ioengine=sync --iodepth=1 --numjobs=1 --bs="$fio_block" --direct=0 \
        --fsync=1 --runtime=30 --time_based=1 --group_reporting=1 \
        --output-format=json --output="$RUN_ROOT/fio/${label}.json"; then
      printf 'result=invalid-run\nfailed_case=%s\nreason=fio-failed\n' "$label" \
        >"$RUN_ROOT/logs/result.txt"
      printf 'run_root=%s\n' "$RUN_ROOT"
      exit 4
    fi
    check_frozen_artifacts
  done
}

# Component-only campaign -------------------------------------------------
# This path deliberately does not call any of the full Engine, tail,
# perf-record, writer-profile, or fio functions above.  It uses the same
# identity/run-root conventions, but has its own CPU-only and WAL storage
# gates so a component result cannot be mistaken for an Engine result.
COMPONENT_COOLDOWN_SECONDS=60
COMPONENT_TARGET_DURATION_MS=$ENGINE_TARGET_DURATION_MS
COMPONENT_MIN_DURATION_MS=$MIN_ENGINE_DURATION_MS
COMPONENT_FSYNC_CALIBRATION_GROUPS=100
COMPONENT_STATE_COMMANDS=0
COMPONENT_APPEND_COMMANDS=0
COMPONENT_APPEND_EPOCH_COMMANDS=0
COMPONENT_CALIBRATION_COMMANDS=0
COMPONENT_WAL_WARMUP_COMMANDS=0
declare -A COMPONENT_APPEND_EPOCH_COMMANDS_BY_BATCH=()
declare -A COMPONENT_FSYNC_GROUPS_BY_BATCH=()
declare -A COMPONENT_APPEND_RATES_BY_BATCH=()
declare -A COMPONENT_APPEND_BYTES_BY_BATCH=()
declare -A COMPONENT_FSYNC_BYTES_BY_BATCH=()
declare -A COMPONENT_STATE_RATES_BY_SCENARIO=()
COMPONENT_MONITOR_PIDS=()
COMPONENT_REJECTED_CASES=0

# These values mirror the benchmark's production WAL segment geometry.  They
# are used only to freeze an epoch plan; the benchmark still opens WALs with
# the production segment size and verifies that each measured epoch rotates 0
# times.
COMPONENT_WAL_SEGMENT_BYTES=$((256 * 1024 * 1024))
COMPONENT_WAL_HEADER_BYTES=22
COMPONENT_WAL_EPOCH_SAFETY_BYTES=$((1024 * 1024))

if [[ "$TEST_MODE" == 1 ]]; then
  COMPONENT_COOLDOWN_SECONDS=0
  COMPONENT_FSYNC_CALIBRATION_GROUPS=2
fi

component_residual_processes() {
  ps -eo comm= | awk '
    $1 == "order_books_benchmark" || $1 == "mpstat" || $1 == "iostat" ||
    $1 == "fio" || $1 == "perf" { found = 1 }
    END { exit found ? 0 : 1 }'
}

component_round_up() {
  local value=$1
  local quantum=$2
  awk -v value="$value" -v quantum="$quantum" \
    'BEGIN {
       if (value < 1 || quantum < 1) exit 1
       printf "%.0f\n", int((value + quantum - 1) / quantum) * quantum
     }'
}

component_wal_warmup_groups() {
  local batch=$1
  local groups
  groups=$(component_round_up "$COMPONENT_WAL_WARMUP_COMMANDS" "$batch") || return 1
  printf '%s\n' "$((groups / batch))"
}

component_no_rotation_epoch_commands() {
  local batch=$1
  local bytes_per_command=$2
  local warmup_commands=$COMPONENT_WAL_WARMUP_COMMANDS
  local payload_budget max_commands epoch_commands
  [[ "$batch" =~ ^[1-9][0-9]*$ ]] || return 1
  component_is_non_negative_number "$bytes_per_command" || return 1
  payload_budget=$((COMPONENT_WAL_SEGMENT_BYTES - COMPONENT_WAL_HEADER_BYTES -
                    COMPONENT_WAL_EPOCH_SAFETY_BYTES))
  max_commands=$(awk -v budget="$payload_budget" -v bytes="$bytes_per_command" \
    'BEGIN { if (bytes <= 0) exit 1; printf "%.0f\n", int(budget / bytes) }') || return 1
  ((max_commands > warmup_commands + batch)) || return 1
  max_commands=$((max_commands - warmup_commands))
  epoch_commands=$((max_commands / batch * batch))
  ((epoch_commands >= batch)) || return 1
  printf '%s\n' "$epoch_commands"
}

component_available_bytes() {
  df -B1 --output=avail "$RUN_ROOT" | awk 'NR == 2 {print $1; exit}'
}

component_check_disk_budget() {
  local label=$1
  local commands=$2
  local bytes_per_command=$3
  local available reserve budget estimated
  available=$(component_available_bytes)
  [[ "$available" =~ ^[0-9]+$ ]] || campaign_fail environment-blocked "disk-available-invalid-${label}"
  reserve=$((available / 5))
  ((reserve > 20 * 1024 * 1024 * 1024)) || reserve=$((20 * 1024 * 1024 * 1024))
  budget=$((available - reserve))
  ((budget > 0)) || campaign_fail environment-blocked "disk-budget-empty-${label}"
  estimated=$(awk -v commands="$commands" -v bytes="$bytes_per_command" \
    'BEGIN { if (commands < 1 || bytes < 0) exit 1; printf "%.0f\n", commands * bytes }') ||
    campaign_fail invalid-run "disk-estimate-invalid-${label}"
  if ((estimated > budget)); then
    campaign_fail environment-blocked "component-disk-budget-exceeded-${label}"
  fi
  printf 'label=%s available_bytes=%s reserve_bytes=%s budget_bytes=%s estimated_case_bytes=%s\n' \
    "$label" "$available" "$reserve" "$budget" "$estimated" \
    >>"$RUN_ROOT/derived/component-disk-budget.tsv"
}

component_validate_estimated_duration() {
  local label=$1
  local commands=$2
  local rate=$3
  local estimated_ms
  estimated_ms=$(awk -v commands="$commands" -v rate="$rate" \
    'BEGIN { if (commands < 1 || rate <= 0) exit 1; printf "%.0f\n", commands / rate * 1000 }') ||
    campaign_fail invalid-run "duration-estimate-invalid-${label}"
  awk -v estimated="$estimated_ms" -v timeout="$CASE_TIMEOUT_SECONDS" \
    'BEGIN { exit !(estimated < timeout * 1000 - 30000) }' ||
    campaign_fail invalid-run "calibration-exceeds-timeout-${label}"
  printf 'label=%s rate=%s commands=%s estimated_duration_ms=%s\n' \
    "$label" "$rate" "$commands" "$estimated_ms" >>"$RUN_ROOT/derived/component-duration-plan.tsv"
}

component_extract_field() {
  local output=$1
  local field=$2
  grep -oE "(^|[[:space:]])${field}=[^[:space:]]+" "$output" |
    sed 's/^[[:space:]]*//' | cut -d= -f2
}

component_extract_integer_field() {
  local output=$1
  local field=$2
  local -a values=()
  mapfile -t values < <(component_extract_field "$output" "$field")
  ((${#values[@]} == 1)) || return 1
  [[ "${values[0]}" =~ ^[0-9]+$ ]] || return 1
  printf '%s\n' "${values[0]}"
}

component_extract_number() {
  local output=$1
  local field=$2
  local -a values=()
  mapfile -t values < <(component_extract_field "$output" "$field")
  ((${#values[@]} == 1)) || return 1
  component_is_non_negative_number "${values[0]}" || return 1
  printf '%s\n' "${values[0]}"
}

component_is_non_negative_number() {
  [[ "$1" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]] || return 1
  local normalized
  normalized=$(awk -v value="$1" 'BEGIN {
    parsed = value + 0
    if (parsed != parsed || parsed < 0) exit 1
    printf "%.17g\n", parsed
  }') || return 1
  [[ "$normalized" != inf && "$normalized" != +inf && "$normalized" != -inf ]]
}

component_extract_rate() {
  local output=$1
  local kind=${2:-wal}
  local field=service_commands_per_second
  [[ "$kind" == state ]] && field=commands_per_second
  local value
  value=$(component_extract_number "$output" "$field") || return 1
  awk -v value="$value" 'BEGIN { exit !(value > 0) }' || return 1
  printf '%s\n' "$value"
}

component_validate_measured_resources() {
  local output=$1
  local expected_sync_calls=$2
  local field value syscw wal_write_calls
  for field in measured_rusage_valid measured_io_valid measured_meminfo_valid; do
    value=$(component_extract_field "$output" "$field") || return 1
    [[ "$value" == true ]] || return 1
  done
  for field in measured_user_seconds measured_system_seconds; do
    component_extract_number "$output" "$field" >/dev/null || return 1
  done
  for field in measured_voluntary_context_switches measured_involuntary_context_switches \
    measured_syscw measured_wchar measured_write_bytes measured_cancelled_write_bytes \
    measured_dirty_bytes_before measured_dirty_bytes_after measured_writeback_bytes_before \
    measured_writeback_bytes_after measured_wal_write_calls measured_wal_sync_calls; do
    component_extract_integer_field "$output" "$field" >/dev/null || return 1
  done
  syscw=$(component_extract_integer_field "$output" measured_syscw) || return 1
  wal_write_calls=$(component_extract_integer_field "$output" measured_wal_write_calls) || return 1
  [[ "$syscw" == "$wal_write_calls" ]] || return 1
  value=$(component_extract_integer_field "$output" measured_wal_sync_calls) || return 1
  [[ "$value" == "$expected_sync_calls" ]] || return 1
}

component_preflight() {
  local label=$1
  local storage_gate=$2
  local cpu_json="$RUN_ROOT/preflight/component-${label}-mpstat.json"
  local disk_json="$RUN_ROOT/preflight/component-${label}-iostat.json"
  local summary="$RUN_ROOT/preflight/component-${label}-summary.txt"
  if component_residual_processes; then
    printf 'result=preflight-busy\nreason=residual-process\n' >"$summary"
    return 1
  fi
  if [[ "$storage_gate" == 1 ]]; then
    sleep "$COMPONENT_COOLDOWN_SECONDS"
  fi
  taskset -c "$OBSERVER_CPU_SET" mpstat -P "$CPU_LIST" 1 "$PREFLIGHT_SECONDS" \
    -o JSON >"$cpu_json" &
  local cpu_pid=$!
  taskset -c "$OBSERVER_CPU_SET" iostat -y -dx "$BLOCK_DEVICE" 1 "$PREFLIGHT_SECONDS" \
    -o JSON >"$disk_json" &
  local disk_pid=$!
  local cpu_status=0
  local disk_status=0
  wait "$cpu_pid" || cpu_status=$?
  wait "$disk_pid" || disk_status=$?
  if [[ "$cpu_status" != 0 || "$disk_status" != 0 ]]; then
    printf 'result=preflight-busy\nmonitor_exit_cpu=%s\nmonitor_exit_disk=%s\n' \
      "$cpu_status" "$disk_status" >"$summary"
    return 1
  fi
  local cpu_idle_min cpu_iowait_p95 cpu_iowait_max disk_util_avg disk_aqu_p95 disk_aqu_max
  cpu_idle_min=$(jq -r '
    [.sysstat.hosts[0].statistics[] | .["cpu-load"][]]
    | sort_by(.cpu) | group_by(.cpu)
    | map(map(.idle) | add / length) | min' "$cpu_json")
  cpu_iowait_p95=$(jq -r '
    [.sysstat.hosts[0].statistics[] | .["cpu-load"][] | .iowait]
    | sort | .[((length - 1) * 0.95 | floor)]' "$cpu_json")
  cpu_iowait_max=$(jq -r '
    [.sysstat.hosts[0].statistics[] | .["cpu-load"][] | .iowait] | max' "$cpu_json")
  disk_util_avg=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .util]
    | if length == 0 then null else add / length end' "$disk_json")
  disk_aqu_p95=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]]
    | if length == 0 then null else sort | .[((length - 1) * 0.95 | floor)] end' "$disk_json")
  disk_aqu_max=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]]
    | if length == 0 then null else max end' "$disk_json")
  printf 'scope=component\nstorage_gate=%s\ncpu_idle_min=%s\ncpu_iowait_p95=%s\n' \
    "$storage_gate" "$cpu_idle_min" "$cpu_iowait_p95" >"$summary"
  printf 'cpu_iowait_max=%s\ndisk_util_avg=%s\ndisk_aqu_p95=%s\ndisk_aqu_max=%s\n' \
    "$cpu_iowait_max" "$disk_util_avg" "$disk_aqu_p95" "$disk_aqu_max" >>"$summary"
  awk -v value="$cpu_idle_min" 'BEGIN { exit !(value != "null" && value >= 90.0) }' || return 1
  awk -v value="$cpu_iowait_p95" 'BEGIN { exit !(value != "null" && value <= 5.0) }' || return 1
  if [[ "$storage_gate" == 1 ]]; then
    awk -v value="$cpu_iowait_max" 'BEGIN { exit !(value != "null" && value <= 10.0) }' || return 1
    awk -v value="$disk_util_avg" 'BEGIN { exit !(value != "null" && value <= 5.0) }' || return 1
    awk -v value="$disk_aqu_p95" 'BEGIN { exit !(value != "null" && value <= 0.25) }' || return 1
    awk -v value="$disk_aqu_max" 'BEGIN { exit !(value != "null" && value <= 0.50) }' || return 1
  fi
}

component_start_monitors() {
  local label=$1
  taskset -c "$OBSERVER_CPU_SET" mpstat -P "$CPU_LIST" 1 "$MONITOR_SECONDS" \
    >"$RUN_ROOT/monitors/component-${label}-mpstat.txt" &
  COMPONENT_MONITOR_PIDS+=("$!")
  taskset -c "$OBSERVER_CPU_SET" iostat -y -dx -t "$BLOCK_DEVICE" 1 "$MONITOR_SECONDS" \
    >"$RUN_ROOT/monitors/component-${label}-iostat.txt" &
  COMPONENT_MONITOR_PIDS+=("$!")
}

component_stop_monitors() {
  local pid status failure=0
  for pid in "${COMPONENT_MONITOR_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
    status=0
    wait "$pid" 2>/dev/null || status=$?
    if [[ "$status" != 0 && "$status" != 143 && "$status" != 130 ]]; then
      failure=1
    fi
  done
  COMPONENT_MONITOR_PIDS=()
  return "$failure"
}

component_safe_remove_data() {
  local data_dir=$1
  case "$data_dir" in
    "$RUN_ROOT/data/component-"*) ;;
    *) campaign_fail invalid-run "component-data-path-invalid-${data_dir}" ;;
  esac
  [[ "$data_dir" != "$RUN_ROOT/data" && -n "$data_dir" ]] ||
    campaign_fail invalid-run component-data-path-empty
  rm -rf -- "$data_dir"
}

component_finalize_case_data() {
  local label=$1
  component_safe_remove_data "$RUN_ROOT/data/component-${label}"
}

component_run_binary() {
  local label=$1
  local storage_gate=$2
  local warmup_groups=$WARMUP
  shift 2
  while [[ "${1:-}" == --component-warmup-groups=* ]]; do
    warmup_groups=${1#*=}
    shift
  done
  is_positive_integer "$warmup_groups" || die "component warmup groups must be positive"
  local data_dir="$RUN_ROOT/data/component-${label}"
  local stdout="$RUN_ROOT/logs/component-${label}.stdout"
  local stderr="$RUN_ROOT/logs/component-${label}.stderr"
  local time_file="$RUN_ROOT/time/component-${label}.time"
  component_preflight "$label" "$storage_gate" || {
    printf 'result=preflight-busy\nfailed_case=%s\n' "$label" >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  }
  mkdir -p "$data_dir"
  component_start_monitors "$label"
  local status=0
  RUNNER_CASE_LABEL="$label" \
  /usr/bin/time -v -o "$time_file" taskset -c "$BENCH_CPU_SET" timeout \
    "${CASE_TIMEOUT_SECONDS}s" "$BENCHMARK_BINARY" --warmup="$warmup_groups" \
    --data-dir="$data_dir" "$@" >"$stdout" 2>"$stderr" || status=$?
  local monitor_status=0
  component_stop_monitors || monitor_status=$?
  printf '%s\n' "$status" >"$RUN_ROOT/logs/component-${label}.status"
  [[ "$monitor_status" == 0 ]] || campaign_fail invalid-run "component-observer-${label}"
  [[ -s "$RUN_ROOT/monitors/component-${label}-mpstat.txt" &&
     -s "$RUN_ROOT/monitors/component-${label}-iostat.txt" ]] ||
    campaign_fail invalid-run "component-observer-artifact-${label}"
  check_frozen_artifacts
  [[ "$status" == 0 ]] || campaign_fail invalid-run "component-exit-${label}-${status}"
  [[ -s "$stdout" && -s "$time_file" ]] || campaign_fail invalid-run "component-artifact-${label}"
  write_normalized_row "component-${label}" "$status" "$stdout"
}

component_calibrate() {
  local scenario batch output rate average_bytes warmup_groups
  local max_state_rate=0
  local max_append_rate=0
  local calibration_groups state_iterations raw_state_commands raw_append_commands
  local estimated_ms estimated_bytes state_warmup_commands
  local calibration_plan="$RUN_ROOT/derived/component-frozen-plan.tsv"
  COMPONENT_CALIBRATION_COMMANDS=$(component_round_up "$((CALIBRATION_ITERATIONS * 2))" 8192) ||
    campaign_fail invalid-run component-calibration-command-overflow
  COMPONENT_WAL_WARMUP_COMMANDS=$(component_round_up "$((WARMUP * 2))" 8192) ||
    campaign_fail invalid-run component-warmup-command-overflow
  : >"$RUN_ROOT/derived/component-duration-plan.tsv"
  : >"$RUN_ROOT/derived/component-disk-budget.tsv"
  printf 'kind\tkey\tfixed_commands\twarmup_commands\twarmup_groups\tcalibration_rate\testimated_duration_ms\testimated_case_bytes\tformal_rounds\n' \
    >"$calibration_plan"
  state_iterations=$((COMPONENT_CALIBRATION_COMMANDS / 2))
  state_warmup_commands=$((WARMUP * 2))
  for scenario in new_crossing_pair new_resting_cancel amend_quantity replace_order; do
    component_run_binary "calibration-state-${scenario}" 0 \
      --component-warmup-groups="$WARMUP" \
      --workload=engine_pipeline_ceiling --pipeline-stage=state_machine \
      --pipeline-command-scenario="$scenario" --pipeline-batch-size=2 \
      --pipeline-active-orders=0 --iterations="$state_iterations"
    output="$RUN_ROOT/logs/component-calibration-state-${scenario}.stdout"
    component_validate_output "$output" "$COMPONENT_CALIBRATION_COMMANDS" state ||
      campaign_fail invalid-run "state-calibration-output-${scenario}"
    state_rate=$(component_extract_rate "$output" state)
    [[ -n "$state_rate" ]] || campaign_fail invalid-run "state-calibration-rate-${scenario}"
    COMPONENT_STATE_RATES_BY_SCENARIO[$scenario]=$state_rate
    if [[ "$max_state_rate" == 0 ]] || awk -v a="$state_rate" -v b="$max_state_rate" 'BEGIN { exit !(a > b) }'; then
      max_state_rate=$state_rate
    fi
    component_finalize_case_data "calibration-state-${scenario}"
  done
  raw_state_commands=$(awk -v rate="$max_state_rate" -v duration="$COMPONENT_TARGET_DURATION_MS" \
    'BEGIN {
       value = rate * duration / 1000
       if (value < 2) value = 2
       value = int(value)
       if (value < rate * duration / 1000) ++value
       printf "%.0f\n", value
     }') ||
    campaign_fail invalid-run state-command-plan-invalid
  COMPONENT_STATE_COMMANDS=$(( ((raw_state_commands + 1) / 2) * 2 ))
  for scenario in new_crossing_pair new_resting_cancel amend_quantity replace_order; do
    component_validate_estimated_duration "state-${scenario}" "$COMPONENT_STATE_COMMANDS" \
      "${COMPONENT_STATE_RATES_BY_SCENARIO[$scenario]}"
    estimated_ms=$(awk -v commands="$COMPONENT_STATE_COMMANDS" \
      -v rate="${COMPONENT_STATE_RATES_BY_SCENARIO[$scenario]}" \
      'BEGIN { printf "%.0f\n", commands / rate * 1000 }')
    printf 'state_machine\t%s\t%s\t%s\t%s\t%s\t%s\tna\t%s\n' \
      "$scenario" "$COMPONENT_STATE_COMMANDS" "$state_warmup_commands" "$WARMUP" \
      "${COMPONENT_STATE_RATES_BY_SCENARIO[$scenario]}" "$estimated_ms" "$PIPELINE_ROUNDS" \
      >>"$calibration_plan"
  done

  for batch in 1 256 1024 4096 8192; do
    calibration_groups=$((COMPONENT_CALIBRATION_COMMANDS / batch))
    warmup_groups=$(component_wal_warmup_groups "$batch") ||
      campaign_fail invalid-run "warmup-plan-invalid-${batch}"
    component_run_binary "calibration-append-${batch}" 0 \
      --component-warmup-groups="$warmup_groups" \
      --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=off \
      --wal-group-size="$batch" --iterations="$calibration_groups"
    output="$RUN_ROOT/logs/component-calibration-append-${batch}.stdout"
    component_validate_output "$output" "$((calibration_groups * batch))" wal off \
      "$calibration_groups" 0 ||
      campaign_fail invalid-run "append-calibration-output-${batch}"
    rate=$(component_extract_rate "$output" wal)
    [[ -n "$rate" ]] || campaign_fail invalid-run "append-calibration-rate-${batch}"
    average_bytes=$(component_extract_number "$output" average_wal_bytes_per_command)
    [[ -n "$average_bytes" ]] ||
      campaign_fail invalid-run "append-calibration-bytes-${batch}"
    COMPONENT_APPEND_RATES_BY_BATCH[$batch]=$rate
    COMPONENT_APPEND_BYTES_BY_BATCH[$batch]=$average_bytes
    if [[ "$max_append_rate" == 0 ]] || awk -v a="$rate" -v b="$max_append_rate" 'BEGIN { exit !(a > b) }'; then
      max_append_rate=$rate
    fi
    component_finalize_case_data "calibration-append-${batch}"
  done
  raw_append_commands=$(awk -v rate="$max_append_rate" -v duration="$COMPONENT_TARGET_DURATION_MS" \
    'BEGIN {
       value = rate * duration / 1000
       if (value < 8192) value = 8192
       value = int(value)
       if (value < rate * duration / 1000) ++value
       printf "%.0f\n", value
     }') ||
    campaign_fail invalid-run append-command-plan-invalid
  COMPONENT_APPEND_COMMANDS=$(( ((raw_append_commands + 8191) / 8192) * 8192 ))
  for batch in 1 256 1024 4096 8192; do
    warmup_groups=$(component_wal_warmup_groups "$batch")
    epoch_commands=$(component_no_rotation_epoch_commands "$batch" \
      "${COMPONENT_APPEND_BYTES_BY_BATCH[$batch]}") ||
      campaign_fail invalid-run "no-rotation-epoch-plan-invalid-${batch}"
    COMPONENT_APPEND_EPOCH_COMMANDS_BY_BATCH[$batch]=$epoch_commands
    component_validate_estimated_duration "append-b${batch}" "$COMPONENT_APPEND_COMMANDS" \
      "${COMPONENT_APPEND_RATES_BY_BATCH[$batch]}"
    component_check_disk_budget "append-b${batch}" \
      "$((COMPONENT_APPEND_COMMANDS + COMPONENT_WAL_WARMUP_COMMANDS))" \
      "${COMPONENT_APPEND_BYTES_BY_BATCH[$batch]}"
    estimated_ms=$(awk -v commands="$COMPONENT_APPEND_COMMANDS" \
      -v rate="${COMPONENT_APPEND_RATES_BY_BATCH[$batch]}" \
      'BEGIN { printf "%.0f\n", commands / rate * 1000 }')
    estimated_bytes=$(awk -v commands="$((COMPONENT_APPEND_COMMANDS + COMPONENT_WAL_WARMUP_COMMANDS))" \
      -v bytes="${COMPONENT_APPEND_BYTES_BY_BATCH[$batch]}" \
      'BEGIN { printf "%.0f\n", commands * bytes }')
    printf 'wal_append\tb%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\tepoch_commands=%s\tepochs=%s\n' \
      "$batch" "$COMPONENT_APPEND_COMMANDS" "$COMPONENT_WAL_WARMUP_COMMANDS" "$warmup_groups" \
      "${COMPONENT_APPEND_RATES_BY_BATCH[$batch]}" "$estimated_ms" "$estimated_bytes" "$WAL_ROUNDS" \
      "$epoch_commands" "$(( (COMPONENT_APPEND_COMMANDS + epoch_commands - 1) / epoch_commands ))" \
      >>"$calibration_plan"
  done
  for batch in 1 256 1024 4096 8192; do
    warmup_groups=$(component_wal_warmup_groups "$batch") ||
      campaign_fail invalid-run "warmup-plan-invalid-fsync-${batch}"
    component_run_binary "calibration-fsync-${batch}" 0 \
      --component-warmup-groups="$warmup_groups" \
      --workload=wal_write_ceiling --wal-sync=per_group --wal-phase-profile=off \
      --wal-group-size="$batch" --iterations="$COMPONENT_FSYNC_CALIBRATION_GROUPS"
    output="$RUN_ROOT/logs/component-calibration-fsync-${batch}.stdout"
    component_validate_output "$output" \
      "$((COMPONENT_FSYNC_CALIBRATION_GROUPS * batch))" wal off \
      "$COMPONENT_FSYNC_CALIBRATION_GROUPS" "$COMPONENT_FSYNC_CALIBRATION_GROUPS" ||
      campaign_fail invalid-run "fsync-calibration-output-${batch}"
    rate=$(component_extract_rate "$output" wal)
    [[ -n "$rate" ]] || campaign_fail invalid-run "fsync-calibration-rate-${batch}"
    average_bytes=$(component_extract_number "$output" average_wal_bytes_per_command)
    [[ -n "$average_bytes" ]] ||
      campaign_fail invalid-run "fsync-calibration-bytes-${batch}"
    COMPONENT_FSYNC_BYTES_BY_BATCH[$batch]=$average_bytes
    component_finalize_case_data "calibration-fsync-${batch}"
    local groups
    groups=$(awk -v rate="$rate" -v duration="$COMPONENT_TARGET_DURATION_MS" -v batch="$batch" \
      'BEGIN {
         value = rate * duration / 1000 / batch
         if (value < 1000) value = 1000
         value = int(value)
         if (value < rate * duration / 1000 / batch) ++value
         printf "%.0f\n", value
       }') || campaign_fail invalid-run "fsync-command-plan-invalid-${batch}"
    COMPONENT_FSYNC_GROUPS_BY_BATCH[$batch]=$groups
    component_validate_estimated_duration "fsync-b${batch}" "$((groups * batch))" "$rate"
    component_check_disk_budget "fsync-b${batch}" \
      "$((groups * batch + COMPONENT_WAL_WARMUP_COMMANDS))" \
      "$average_bytes"
    estimated_ms=$(awk -v commands="$((groups * batch))" -v rate="$rate" \
      'BEGIN { printf "%.0f\n", commands / rate * 1000 }')
    estimated_bytes=$(awk -v commands="$((groups * batch + COMPONENT_WAL_WARMUP_COMMANDS))" \
      -v bytes="$average_bytes" 'BEGIN { printf "%.0f\n", commands * bytes }')
    printf 'wal_fsync\tb%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$batch" "$((groups * batch))" "$COMPONENT_WAL_WARMUP_COMMANDS" "$warmup_groups" \
      "$rate" "$estimated_ms" "$estimated_bytes" "$WAL_ROUNDS" >>"$calibration_plan"
  done
}

component_validate_duration() {
  local output=$1
  local elapsed
  elapsed=$(grep -o 'elapsed_ms=[^ ]*' "$output" | tail -n 1 | cut -d= -f2)
  [[ -n "$elapsed" ]] || return 1
  awk -v value="$elapsed" -v minimum="$COMPONENT_MIN_DURATION_MS" \
    'BEGIN { exit !(value >= minimum) }'
}

component_validate_output() {
  local output=$1
  local expected_commands=$2
  local kind=$3
  local profile_mode=${4:-off}
  local expected_groups=${5:-}
  local expected_sync_samples=${6:-}
  local expected_rotation_scope=${7:-}
  local expected_rotation_count=${8:-}
  local expected_rotation_triggered=${9:-}
  local expected_rotation_case=${10:-}
  local actual
  grep -Eq 'correctness_verified=true|replay_verified=true' "$output" || return 1
  if [[ "$kind" == state ]]; then
    grep -Eq "measured_commands=${expected_commands}([[:space:]]|$)" "$output"
  else
    [[ -n "$expected_groups" && -n "$expected_sync_samples" ]] || return 1
    actual=$(component_extract_integer_field "$output" commands) || return 1
    [[ "$actual" == "$expected_commands" ]] || return 1
    actual=$(component_extract_integer_field "$output" groups) || return 1
    [[ "$actual" == "$expected_groups" ]] || return 1
    grep -Eq "(^|[[:space:]])phase_profile=${profile_mode}([[:space:]]|$)" "$output" ||
      return 1
    actual=$(component_extract_integer_field "$output" sync_samples) || return 1
    [[ "$actual" == "$expected_sync_samples" ]] || return 1
    local sample_stride append_sample_count sync_sample_count
    sample_stride=$(component_extract_integer_field "$output" latency_sample_stride) || return 1
    ((sample_stride > 0)) || return 1
    append_sample_count=$(component_extract_integer_field "$output" append_latency_sample_count) ||
      return 1
    ((append_sample_count > 0 && append_sample_count <= expected_groups)) || return 1
    sync_sample_count=$(component_extract_integer_field "$output" sync_latency_sample_count) ||
      return 1
    if ((expected_sync_samples == 0)); then
      ((sync_sample_count == 0)) || return 1
    else
      ((sync_sample_count > 0 && sync_sample_count <= expected_sync_samples)) || return 1
    fi
    component_validate_measured_resources "$output" "$expected_sync_samples" || return 1
    local expected_name=wal_write_ceiling
    [[ "$expected_rotation_scope" == no_rotation ]] && expected_name=wal_append_no_rotation
    [[ "$expected_rotation_scope" == rotation_inclusive &&
       "$expected_rotation_count" != "" ]] && expected_name=wal_rotation_diagnostic
    grep -Eq "^${expected_name}([[:space:]]|$)" "$output" || return 1
    if [[ "$expected_rotation_scope" == no_rotation ]]; then
      grep -Eq 'wal_byte_plan_verified=true([[:space:]]|$)' "$output" || return 1
      local planned actual_bytes
      planned=$(component_extract_integer_field "$output" planned_wal_bytes_delta) || return 1
      actual_bytes=$(component_extract_integer_field "$output" wal_bytes_delta) || return 1
      [[ "$planned" == "$actual_bytes" ]] || return 1
    fi
    if [[ -n "$expected_rotation_case" ]]; then
      grep -Eq '^wal_rotation_diagnostic([[:space:]]|$)' "$output" || return 1
      actual=$(component_extract_field "$output" case) || return 1
      [[ "$actual" == "$expected_rotation_case" ]] || return 1
      [[ "$sample_stride" == 1 && "$append_sample_count" == 1 ]] || return 1
      [[ "$(component_extract_field "$output" wal_byte_plan_verified)" == true ]] || return 1
      local planned actual_bytes frame_bytes header_bytes before_id after_id target_id
      planned=$(component_extract_integer_field "$output" planned_wal_bytes_delta) || return 1
      actual_bytes=$(component_extract_integer_field "$output" wal_bytes_delta) || return 1
      [[ "$planned" == "$actual_bytes" ]] || return 1
      frame_bytes=$(component_extract_integer_field "$output" frame_bytes_per_command) || return 1
      header_bytes=$(component_extract_integer_field "$output" segment_header_bytes) || return 1
      ((frame_bytes > 0 && header_bytes > 0)) || return 1
      before_id=$(component_extract_integer_field "$output" segment_id_before) || return 1
      after_id=$(component_extract_integer_field "$output" segment_id_after) || return 1
      local before_offset after_offset
      before_offset=$(component_extract_integer_field "$output" segment_offset_before) || return 1
      after_offset=$(component_extract_integer_field "$output" segment_offset_after) || return 1
      if [[ "$expected_rotation_case" == control ]]; then
        [[ "$before_id" == "$after_id" ]] || return 1
        awk -v before="$before_offset" -v frame="$frame_bytes" -v after="$after_offset" \
          'BEGIN { exit !(before + frame == after) }' || return 1
      else
        target_id=$(component_extract_integer_field "$output" target_sequence) || return 1
        [[ "$after_id" == "$target_id" ]] || return 1
        awk -v header="$header_bytes" -v frame="$frame_bytes" -v after="$after_offset" \
          'BEGIN { exit !(header + frame == after) }' || return 1
      fi
      for field in wal_prepare_group_total_us wal_plan_copy_group_total_us \
        wal_rotation_group_total_us wal_write_group_total_us wal_publish_group_total_us \
        rotation_sync_total_us rotation_header_write_total_us rotation_header_sync_total_us \
        rotation_directory_sync_total_us; do
        component_extract_number "$output" "$field" >/dev/null || return 1
      done
      [[ "$(component_extract_field "$output" replay_verified)" == true ]] || return 1
    fi
    if [[ -n "$expected_rotation_scope" ]]; then
      grep -Eq "(^|[[:space:]])rotation_scope=${expected_rotation_scope}([[:space:]]|$)" \
        "$output" || return 1
    fi
    if [[ -n "$expected_rotation_count" ]]; then
      actual=$(component_extract_integer_field "$output" measured_segment_rotations) || return 1
      [[ "$actual" == "$expected_rotation_count" ]] || return 1
    fi
    if [[ -n "$expected_rotation_triggered" ]]; then
      actual=$(component_extract_field "$output" rotation_triggered) || return 1
      [[ "$actual" == "$expected_rotation_triggered" ]] || return 1
    fi
    if [[ "$profile_mode" == on ]]; then
      actual=$(component_extract_integer_field "$output" profiled_groups) || return 1
      [[ "$actual" == "$expected_groups" ]] || return 1
      actual=$(component_extract_integer_field "$output" profiled_commands) || return 1
      [[ "$actual" == "$expected_commands" ]] || return 1
      actual=$(component_extract_integer_field "$output" profiled_data_write_calls) || return 1
      awk -v actual="$actual" -v expected="$expected_groups" \
        'BEGIN { exit !(actual >= expected) }' || return 1
      grep -Eq 'wal_prepare_group_total_us=' "$output" || return 1
      grep -Eq 'wal_write_group_total_us=' "$output" || return 1
    fi
  fi
}

component_validate_cv() {
  local kind=$1
  local key=$2
  local rounds=$3
  local round output rate cv
  cv=$(for ((round=1; round<=rounds; ++round)); do
    output="$RUN_ROOT/logs/component-${kind}-r${round}-${key}.stdout"
    rate=$(component_extract_rate "$output" "$kind")
    [[ -n "$rate" ]] || return 1
    printf '%s\n' "$rate"
  done | awk '
    { sum += $1; values[NR] = $1 }
    END {
      if (NR == 0 || sum <= 0) exit 1
      mean = sum / NR
      for (i = 1; i <= NR; ++i) variance += (values[i] - mean) ^ 2
      printf "%.9f\n", sqrt(variance / NR) / mean * 100
    }') || return 1
  printf 'kind=%s key=%s rounds=%s cv_percent=%s\n' "$kind" "$key" "$rounds" "$cv" \
    >>"$RUN_ROOT/derived/component-cv.tsv"
  awk -v value="$cv" 'BEGIN { exit !(value <= 5.0) }'
}

component_record_cv_status() {
  local kind=$1
  local key=$2
  local rounds=$3
  local round output
  for ((round=1; round<=rounds; ++round)); do
    output="$RUN_ROOT/logs/component-${kind}-r${round}-${key}.stdout"
    [[ -s "$output" ]] || campaign_fail invalid-run "cv-artifact-${kind}-${key}-r${round}"
    component_extract_rate "$output" "$kind" >/dev/null ||
      campaign_fail invalid-run "cv-counter-${kind}-${key}-r${round}"
  done
  if component_validate_cv "$kind" "$key" "$rounds"; then
    printf 'kind=%s key=%s status=accepted\n' "$kind" "$key" \
      >>"$RUN_ROOT/derived/component-case-status.tsv"
  else
    COMPONENT_REJECTED_CASES=$((COMPONENT_REJECTED_CASES + 1))
    printf 'kind=%s key=%s status=rejected-unstable\n' "$kind" "$key" \
      >>"$RUN_ROOT/derived/component-case-status.tsv"
  fi
}

component_run_rotation_attribution() {
  local round output actual_rotations rotation_triggered replay_verified
  local rotation_rows="$RUN_ROOT/derived/component-rotation-attribution.tsv"
  printf 'case\tround\trotation_triggered\texpected_rotations\tmeasured_rotations\tsegment_id_before\tsegment_id_after\tsegment_offset_before\tsegment_offset_after\tdirty_bytes_before\tdirty_bytes_after\twriteback_bytes_before\twriteback_bytes_after\tappend_p50_us\tappend_p99_us\tappend_max_us\trotation_total_us\told_segment_sync_us\theader_write_us\theader_sync_us\tdirectory_sync_us\treplay_verified\n' \
    >"$rotation_rows"
  for ((round=1; round<=WAL_ROUNDS; ++round)); do
    for mode in control trigger; do
      local label="rotation-${mode}-r${round}-b1"
      local expected_rotations=0
      [[ "$mode" == trigger ]] && expected_rotations=1
      component_check_disk_budget "$label" 1 "$((COMPONENT_WAL_SEGMENT_BYTES + COMPONENT_WAL_HEADER_BYTES))"
      component_run_binary "$label" 1 \
        --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=on \
        --wal-rotation-diagnostic="$mode" --wal-group-size=1 --iterations=1
      output="$RUN_ROOT/logs/component-${label}.stdout"
      component_validate_output "$output" 1 wal on 1 0 rotation_inclusive \
        "$expected_rotations" "$([[ "$mode" == trigger ]] && printf true || printf false)" \
        "$mode" ||
        campaign_fail invalid-run "rotation-output-${label}"
      actual_rotations=$(component_extract_integer_field "$output" measured_segment_rotations) ||
        campaign_fail invalid-run "rotation-counter-${label}"
      [[ "$actual_rotations" == "$expected_rotations" ]] ||
        campaign_fail invalid-run "rotation-count-${label}-${actual_rotations}"
      rotation_triggered=$(component_extract_field "$output" rotation_triggered) ||
        campaign_fail invalid-run "rotation-triggered-${label}"
      if [[ "$mode" == trigger ]]; then
        [[ "$rotation_triggered" == true ]] ||
          campaign_fail invalid-run "rotation-not-triggered-${label}"
      else
        [[ "$rotation_triggered" == false ]] ||
          campaign_fail invalid-run "rotation-control-triggered-${label}"
      fi
      replay_verified=$(component_extract_field "$output" replay_verified) ||
        campaign_fail invalid-run "rotation-replay-${label}"
      [[ "$replay_verified" == true ]] ||
        campaign_fail invalid-run "rotation-replay-invalid-${label}"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$round" "$rotation_triggered" "$expected_rotations" "$actual_rotations" \
        "$(component_extract_integer_field "$output" segment_id_before)" \
        "$(component_extract_integer_field "$output" segment_id_after)" \
        "$(component_extract_integer_field "$output" segment_offset_before)" \
        "$(component_extract_integer_field "$output" segment_offset_after)" \
        "$(component_extract_field "$output" measured_dirty_bytes_before)" \
        "$(component_extract_field "$output" measured_dirty_bytes_after)" \
        "$(component_extract_field "$output" measured_writeback_bytes_before)" \
        "$(component_extract_field "$output" measured_writeback_bytes_after)" \
        "$(component_extract_number "$output" append_group_p50_us)" \
        "$(component_extract_number "$output" append_group_p99_us)" \
        "$(component_extract_number "$output" append_group_max_us)" \
        "$(component_extract_number "$output" wal_rotation_group_total_us)" \
        "$(component_extract_number "$output" rotation_sync_total_us)" \
        "$(component_extract_number "$output" rotation_header_write_total_us)" \
        "$(component_extract_number "$output" rotation_header_sync_total_us)" \
        "$(component_extract_number "$output" rotation_directory_sync_total_us)" \
        "$replay_verified" \
        >>"$rotation_rows"
      component_finalize_case_data "$label"
    done
  done
}

component_run_profile_rounds() {
  local batch warmup_groups groups output rate median bias
  for batch in 1 256 1024 4096 8192; do
    warmup_groups=$(component_wal_warmup_groups "$batch") ||
      campaign_fail invalid-run "profile-warmup-invalid-${batch}"
    component_check_disk_budget "profile-append-b${batch}" \
      "$((COMPONENT_APPEND_COMMANDS + COMPONENT_WAL_WARMUP_COMMANDS))" \
      "${COMPONENT_APPEND_BYTES_BY_BATCH[$batch]}"
    component_run_binary "profile-append-b${batch}" 1 \
      --component-warmup-groups="$warmup_groups" \
      --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=on \
      --wal-group-size="$batch" --iterations="$((COMPONENT_APPEND_COMMANDS / batch))"
    output="$RUN_ROOT/logs/component-profile-append-b${batch}.stdout"
    component_validate_output "$output" "$COMPONENT_APPEND_COMMANDS" wal on \
      "$((COMPONENT_APPEND_COMMANDS / batch))" 0 ||
      campaign_fail invalid-run "profile-append-output-${batch}"
    component_validate_duration "$output" || campaign_fail invalid-run "profile-append-duration-${batch}"
    component_finalize_case_data "profile-append-b${batch}"

    groups=${COMPONENT_FSYNC_GROUPS_BY_BATCH[$batch]}
    component_check_disk_budget "profile-fsync-b${batch}" \
      "$((groups * batch + COMPONENT_WAL_WARMUP_COMMANDS))" \
      "${COMPONENT_FSYNC_BYTES_BY_BATCH[$batch]}"
    component_run_binary "profile-fsync-b${batch}" 1 \
      --component-warmup-groups="$warmup_groups" \
      --workload=wal_write_ceiling --wal-sync=per_group --wal-phase-profile=on \
      --wal-group-size="$batch" --iterations="$groups"
    output="$RUN_ROOT/logs/component-profile-fsync-b${batch}.stdout"
    component_validate_output "$output" "$((groups * batch))" wal on "$groups" "$groups" ||
      campaign_fail invalid-run "profile-fsync-output-${batch}"
    component_validate_duration "$output" || campaign_fail invalid-run "profile-fsync-duration-${batch}"
    component_finalize_case_data "profile-fsync-b${batch}"
  done

  warmup_groups=$(component_wal_warmup_groups 4096) ||
    campaign_fail invalid-run profile-control-warmup-invalid
  component_check_disk_budget "profile-control-append-b4096" \
    "$((COMPONENT_APPEND_COMMANDS + COMPONENT_WAL_WARMUP_COMMANDS))" \
    "${COMPONENT_APPEND_BYTES_BY_BATCH[4096]}"
  component_run_binary "profile-control-append-b4096" 1 \
    --component-warmup-groups="$warmup_groups" \
    --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=off \
    --wal-group-size=4096 --iterations="$((COMPONENT_APPEND_COMMANDS / 4096))"
  output="$RUN_ROOT/logs/component-profile-control-append-b4096.stdout"
  component_validate_output "$output" "$COMPONENT_APPEND_COMMANDS" wal off \
    "$((COMPONENT_APPEND_COMMANDS / 4096))" 0 ||
    campaign_fail invalid-run profile-control-output
  component_validate_duration "$output" || campaign_fail invalid-run profile-control-duration
  median=$(component_extract_rate "$output" wal) ||
    campaign_fail invalid-run profile-control-rate-missing
  component_finalize_case_data profile-control-append-b4096

  output="$RUN_ROOT/logs/component-profile-append-b4096.stdout"
  rate=$(component_extract_rate "$output" wal) || campaign_fail invalid-run profile-rate-missing
  bias=$(awk -v profile="$rate" -v baseline="$median" \
    'BEGIN { if (baseline <= 0) exit 1; value = (profile - baseline) / baseline * 100; if (value < 0) value = -value; print value }') ||
    campaign_fail invalid-run profile-bias-invalid
  printf 'case=append-b4096\nprofile_off_median=%s\nprofile_on_rate=%s\nbias_percent=%s\n' \
    "$median" "$rate" "$bias" >"$RUN_ROOT/derived/component-profile-bias.tsv"
  if ! awk -v value="$bias" -v limit="$OBSERVER_BIAS_LIMIT_PERCENT" \
      'BEGIN { exit !(value <= limit) }'; then
    printf 'result=observer-biased\nreason=component-profile-bias\nrun_root=%s\n' "$RUN_ROOT" \
      | tee "$RUN_ROOT/logs/result.txt"
    exit 3
  fi
}

run_component_campaign() {
  [[ "$PROFILE_ONLY" == 0 ]] || die '--profile-only is not valid with --scope=component'
  [[ "$OBSERVER_MODE" == on ]] || die '--observer=off is not valid with --scope=component'
  component_preflight campaign-state 0 || {
    printf 'result=preflight-busy\nreason=initial-component-cpu-preflight\n' >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  }
  component_calibrate
  local scenario round batch groups
  local -a scenarios=(new_crossing_pair new_resting_cancel amend_quantity replace_order)
  : >"$RUN_ROOT/derived/component-case-status.tsv"
  for ((round=1; round<=PIPELINE_ROUNDS; ++round)); do
    if ((round % 2 == 1)); then
      for scenario in "${scenarios[@]}"; do
        component_run_binary "state-r${round}-${scenario}" 0 \
          --workload=engine_pipeline_ceiling --pipeline-stage=state_machine \
          --pipeline-command-scenario="$scenario" --pipeline-batch-size=2 \
          --pipeline-active-orders=0 --iterations="$((COMPONENT_STATE_COMMANDS / 2))"
        component_validate_output "$RUN_ROOT/logs/component-state-r${round}-${scenario}.stdout" \
          "$COMPONENT_STATE_COMMANDS" state || campaign_fail invalid-run "state-counter-${round}-${scenario}"
        component_validate_duration "$RUN_ROOT/logs/component-state-r${round}-${scenario}.stdout" ||
          campaign_fail invalid-run "state-duration-${round}-${scenario}"
        component_finalize_case_data "state-r${round}-${scenario}"
      done
    else
      for ((scenario=${#scenarios[@]} - 1; scenario>=0; --scenario)); do
        component_run_binary "state-r${round}-${scenarios[scenario]}" 0 \
          --workload=engine_pipeline_ceiling --pipeline-stage=state_machine \
          --pipeline-command-scenario="${scenarios[scenario]}" --pipeline-batch-size=2 \
          --pipeline-active-orders=0 --iterations="$((COMPONENT_STATE_COMMANDS / 2))"
        component_validate_output "$RUN_ROOT/logs/component-state-r${round}-${scenarios[scenario]}.stdout" \
          "$COMPONENT_STATE_COMMANDS" state || campaign_fail invalid-run "state-counter-${round}-${scenarios[scenario]}"
        component_validate_duration "$RUN_ROOT/logs/component-state-r${round}-${scenarios[scenario]}.stdout" ||
          campaign_fail invalid-run "state-duration-${round}-${scenarios[scenario]}"
        component_finalize_case_data "state-r${round}-${scenarios[scenario]}"
      done
    fi
  done
  : >"$RUN_ROOT/derived/component-cv.tsv"
  for scenario in "${scenarios[@]}"; do
    component_record_cv_status state "$scenario" "$PIPELINE_ROUNDS"
  done
  for batch in 1 256 1024 4096 8192; do
    warmup_groups=$(component_wal_warmup_groups "$batch") ||
      campaign_fail invalid-run "formal-append-warmup-invalid-${batch}"
    for ((round=1; round<=WAL_ROUNDS; ++round)); do
      component_check_disk_budget "append-r${round}-b${batch}" \
        "$((COMPONENT_APPEND_COMMANDS + COMPONENT_WAL_WARMUP_COMMANDS))" \
        "${COMPONENT_APPEND_BYTES_BY_BATCH[$batch]}"
      component_run_binary "append-r${round}-b${batch}" 1 \
        --component-warmup-groups="$warmup_groups" \
        --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=off \
        --wal-group-size="$batch" --iterations="$((COMPONENT_APPEND_COMMANDS / batch))" \
        --wal-no-rotation-epoch-commands="${COMPONENT_APPEND_EPOCH_COMMANDS_BY_BATCH[$batch]}"
      component_validate_output "$RUN_ROOT/logs/component-append-r${round}-b${batch}.stdout" \
        "$COMPONENT_APPEND_COMMANDS" wal off "$((COMPONENT_APPEND_COMMANDS / batch))" 0 \
        no_rotation 0 false ||
        campaign_fail invalid-run "append-counter-${round}-${batch}"
      component_validate_duration "$RUN_ROOT/logs/component-append-r${round}-b${batch}.stdout" ||
        campaign_fail invalid-run "append-duration-${round}-${batch}"
      component_finalize_case_data "append-r${round}-b${batch}"
    done
    component_record_cv_status append "b${batch}" "$WAL_ROUNDS"
  done
  component_run_rotation_attribution
  for batch in 1 256 1024 4096 8192; do
    groups=${COMPONENT_FSYNC_GROUPS_BY_BATCH[$batch]}
    warmup_groups=$(component_wal_warmup_groups "$batch") ||
      campaign_fail invalid-run "formal-fsync-warmup-invalid-${batch}"
    for ((round=1; round<=WAL_ROUNDS; ++round)); do
      component_check_disk_budget "fsync-r${round}-b${batch}" \
        "$((groups * batch + COMPONENT_WAL_WARMUP_COMMANDS))" \
        "${COMPONENT_FSYNC_BYTES_BY_BATCH[$batch]}"
      component_run_binary "fsync-r${round}-b${batch}" 1 \
        --component-warmup-groups="$warmup_groups" \
        --workload=wal_write_ceiling --wal-sync=per_group --wal-phase-profile=off \
        --wal-group-size="$batch" --iterations="$groups"
      component_validate_output "$RUN_ROOT/logs/component-fsync-r${round}-b${batch}.stdout" \
        "$((groups * batch))" wal off "$groups" "$groups" ||
        campaign_fail invalid-run "fsync-counter-${round}-${batch}"
      component_validate_duration "$RUN_ROOT/logs/component-fsync-r${round}-b${batch}.stdout" ||
        campaign_fail invalid-run "fsync-duration-${round}-${batch}"
      component_finalize_case_data "fsync-r${round}-b${batch}"
    done
    component_record_cv_status fsync "b${batch}" "$WAL_ROUNDS"
  done
  component_run_profile_rounds
  capture_cpu_policy >"$RUN_ROOT/logs/cpu-policy-after.txt"
  record_identity "$RUN_ROOT/logs/repository-identity-after.txt"
  sha256sum "$BENCHMARK_BINARY" >"$RUN_ROOT/logs/binary-sha256-after.txt"
  check_frozen_artifacts
  if ((COMPONENT_REJECTED_CASES != 0)); then
    printf 'collection_status=complete\nresult=collection-complete-results-partial\nrejected_cases=%s\nrun_root=%s\n' \
      "$COMPONENT_REJECTED_CASES" "$RUN_ROOT" \
      | tee "$RUN_ROOT/logs/result.txt"
    return 4
  fi
  printf 'collection_status=complete\nresult=valid-component-ceiling\nrun_root=%s\n' \
    "$RUN_ROOT" | tee "$RUN_ROOT/logs/result.txt"
}

if [[ "$SCOPE" == component ]]; then
  run_component_campaign
  exit $?
fi

if [[ "$PROFILE_ONLY" == 1 ]]; then
  profile_case="g${PROFILE_GROUP_SIZE}-d${PROFILE_DELAY_US}"
  prepare_engine_iteration_plan "$profile_case"
  run_perf_record
else
  prepare_engine_iteration_plan
  run_observer_overhead_gate
  run_pipeline_cases
  run_wal_cases
  run_engine_frontier
  frontier_selection=$(select_engine_frontier_case) || {
    printf 'result=invalid-run\nreason=frontier-selection-failed\n' >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 4
  }
  best_frontier_case=${frontier_selection%% *}
  best_frontier_rps=${frontier_selection#* }
  sustainable_case=""
  while read -r candidate_rps candidate_case candidate_cv; do
    [[ -n "$candidate_case" ]] || continue
    if run_tail_attribution "$candidate_case"; then
      sustainable_case=$candidate_case
      best_frontier_rps=$candidate_rps
      break
    else
      tail_status=$?
      case "$tail_status" in
        10|11)
          tail_reason=$(awk -F= '/^reason=/{print $2; exit}' \
            "$RUN_ROOT/derived/tail-gate-${candidate_case}.summary" 2>/dev/null || true)
          [[ -n "$tail_reason" ]] || tail_reason="tail-gate-status-${tail_status}"
          printf '%s\trejected\t%s\t%s\tna\t%s\n' \
            "$candidate_case" "$candidate_rps" "$candidate_cv" "$tail_reason" \
            >>"$RUN_ROOT/derived/frontier-selection.txt"
          ;;
        *)
          campaign_fail invalid-run "tail-attribution-${candidate_case}-${tail_status}"
          ;;
      esac
    fi
  done < <(sort -nr "$RUN_ROOT/derived/frontier-candidates.txt")
  if [[ -z "$sustainable_case" ]]; then
    printf 'result=inconclusive-attribution\nreason=no-sustainable-frontier-case\n' \
      >"$RUN_ROOT/logs/result.txt"
    printf 'run_root=%s\n' "$RUN_ROOT"
    exit 3
  fi
  best_frontier_case=$sustainable_case
  printf 'best_frontier_case=%s\nbest_frontier_median_rps=%s\n' \
    "$best_frontier_case" "$best_frontier_rps" \
    >>"$RUN_ROOT/derived/frontier-selection.txt"
  best_group=${best_frontier_case#*g}; best_group=${best_group%%-*}
  best_delay=${best_frontier_case##*-d}
  if [[ "$PROFILE_GROUP_SIZE_SET" == 0 ]]; then
    PROFILE_GROUP_SIZE=$best_group
  fi
  if [[ "$PROFILE_DELAY_US_SET" == 0 ]]; then
    PROFILE_DELAY_US=$best_delay
  fi

  declare -A TAIL_COMPARISON_SEEN=()
  for comparison_case in g256-d200 g4096-d1000 "$best_frontier_case"; do
    [[ -n "${TAIL_COMPARISON_SEEN[$comparison_case]:-}" ]] && continue
    TAIL_COMPARISON_SEEN[$comparison_case]=1
    if [[ "$comparison_case" == "$best_frontier_case" ]]; then
      printf '%s\tselected-sustainable\t%s\n' "$comparison_case" "$best_frontier_rps" \
        >>"$RUN_ROOT/derived/frontier-selection.txt"
      continue
    fi
    if run_tail_attribution "$comparison_case"; then
      printf '%s\tcomparison-valid\n' "$comparison_case" \
        >>"$RUN_ROOT/derived/frontier-selection.txt"
    else
      comparison_status=$?
      case "$comparison_status" in
        10|11)
          comparison_reason=$(awk -F= '/^reason=/{print $2; exit}' \
            "$RUN_ROOT/derived/tail-gate-${comparison_case}.summary" 2>/dev/null || true)
          [[ -n "$comparison_reason" ]] || comparison_reason="tail-gate-status-${comparison_status}"
          printf '%s\tcomparison-rejected\treason=%s\n' \
            "$comparison_case" "$comparison_reason" \
            >>"$RUN_ROOT/derived/frontier-selection.txt"
          ;;
        *)
          campaign_fail invalid-run "tail-comparison-${comparison_case}-${comparison_status}"
          ;;
      esac
    fi
  done
  run_engine_confirmations "$best_frontier_case"
  run_writer_profile "$best_frontier_case"
  run_perf_record
  run_fio_control
fi

capture_cpu_policy >"$RUN_ROOT/logs/cpu-policy-after.txt"
record_identity "$RUN_ROOT/logs/repository-identity-after.txt"
sha256sum "$BENCHMARK_BINARY" >"$RUN_ROOT/logs/binary-sha256-after.txt"
printf 'collection_status=complete\nresult=inconclusive-attribution\nrun_root=%s\n' \
  "$RUN_ROOT" | tee "$RUN_ROOT/logs/result.txt"
