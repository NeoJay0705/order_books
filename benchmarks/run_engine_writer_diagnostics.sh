#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: run_engine_writer_diagnostics.sh --binary=PATH --fio-bs=BYTES[,BYTES...] [options]

Runs the bounded, external Engine ceiling/root-cause campaign. All artifacts
are written outside the repository. The script never changes source files,
system policy, or the Git index.

Options:
  --binary=PATH              ReleaseBenchmark order_books_benchmark binary
  --fio-bs=BYTES[,BYTES...]  WAL bytes per selected group for file-backed fio controls
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
  [[ -n "$FIO_BLOCK_BYTES" ]] || die "--fio-bs is required for a full campaign"
  IFS=',' read -r -a FIO_BLOCK_LIST <<<"$FIO_BLOCK_BYTES"
  ((${#FIO_BLOCK_LIST[@]} > 0)) || die "--fio-bs must contain at least one size"
  for fio_block in "${FIO_BLOCK_LIST[@]}"; do
    is_positive_integer "$fio_block" || die "each --fio-bs value must be a positive integer"
  done
fi

if [[ "$DRY_RUN" == 1 ]]; then
  printf 'dry_run=true workload=engine_writer_unified_ceiling\n'
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
  printf 'plan=preflight,calibration,observer_gate,component,wal,engine_frontier,tail_attribution,writer_profile,perf_record,fio\n'
  exit 0
fi

[[ -x "$BENCHMARK_BINARY" ]] || classified_die environment-blocked benchmark-binary-unavailable 2

for tool in git sha256sum findmnt lsblk lscpu mpstat iostat jq perf taskset timeout awk realpath ps \
    cmp cut grep sed sort tee uname xargs mktemp date; do
  command -v "$tool" >/dev/null || classified_die environment-blocked "tool-unavailable-${tool}" 2
done
if [[ "$PROFILE_ONLY" == 0 ]]; then
  command -v fio >/dev/null || classified_die environment-blocked tool-unavailable-fio 2
fi
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

check_perf_capability

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

if ! run_preflight initial; then
  printf 'result=preflight-busy\nreason=initial-preflight-busy\n' >"$RUN_ROOT/logs/result.txt"
  printf 'run_root=%s\n' "$RUN_ROOT"
  exit 3
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
