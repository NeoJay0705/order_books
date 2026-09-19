# WAL sync／storage tail post-benchmark 重測操作與報告規格

## 1. 目的與文件關係

本文件定義如何驗證 `docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-review.md` 的必要修正。
重測先隔離 raw collector 與 10 ms state sampler 的成本；只有 component calibration 通過後，才允許重跑
W1／W2 calibration 與正式五輪矩陣。

以下文件仍是基準，不在本文件重複未變更的設計：

- `docs/wal-sync-storage-tail-root-cause-analysis-design.md`：量測邊界與根因判定；
- `docs/wal-sync-storage-tail-root-cause-analysis-benchmark-procedure.md`：固定 workload、正式矩陣與外部證據；
- `docs/wal-sync-storage-tail-root-cause-analysis-benchmark-report.md`：修正前 blocked 歷史結果，不得覆寫。

新結果寫入：

```text
docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-retest-report.md
```

本流程不修改 production WAL、durability、group policy、prepare worker default、Publisher 或 Completion，
也不得執行 `git add`、`git reset`、`git restore`、`git commit` 等改變 staging 的操作。

## 2. 階段、gate 與停止條件

### 2.1 Phase A：必要 component calibration

只測 W2，三種模式各三輪，每輪 measured 至少 60 秒：

| 模式 | Raw collector | State sampler | CLI |
| --- | --- | --- | --- |
| `off` | 否 | 否 | 不傳 telemetry option |
| `collector` | 是 | 否 | output + `--engine-tail-state-sampling=off` |
| `full` | 是 | 10 ms | output + `--engine-tail-state-sampling=on` |

固定且平衡的執行順序：

```text
r1: off -> collector -> full
r2: full -> off -> collector
r3: collector -> full -> off
```

計算：

```text
collector_bias = abs(median(collector RPS) - median(off RPS)) / median(off RPS)
sampler_increment = abs(median(full RPS) - median(collector RPS)) /
                    median(collector RPS)
full_bias = abs(median(full RPS) - median(off RPS)) / median(off RPS)
```

三者都必須 `<= 5%`。任何一項失敗就停止，不執行 Phase B，也不得以重跑替換慢輪。

### 2.2 Phase B：條件式 calibration 與正式矩陣

只有 Phase A 通過才執行：

1. W1、W2 各自重跑 off／full 三輪 calibration；兩個 full bias 都必須 `<= 5%`。
2. gate 通過後，執行 W1／W2 各五輪 full telemetry 正式矩陣。
3. 正式輪必須保存 `iostat`、process I/O、`/usr/bin/time -v` 與 deterministic drain boundaries。

Phase B 任一 calibration gate 失敗即停止；不得用 Phase A、pilot 或 calibration 輪次代替正式五輪。

### 2.3 其他停止條件

- correctness、sanitizer、replay、durable head、completion、CSV 或 aggregate validation 失敗；
- binary、source identity、固定參數、CPU affinity 或 filesystem 中途改變；
- Phase A 任一正式 component 輪 measured `<60,000 ms`；
- Phase B 任一 calibration／正式輪 measured `<15,000 ms`；
- telemetry sample drop、overflow、sampler error，或 full 模式缺少明確 drain-start／drain-end；
- 需要的 `iostat` 或 process I/O evidence 無法建立。

## 3. 固定參數

```text
OS                         Linux
build                      ReleaseBenchmark
workload                   engine_durable_single_instrument
instrument / shard         1 / 1
completion boundary        durable callback
WAL sync                   per_group
engine group size          4,096 commands
engine group delay         1,000 us
engine producer lanes      8,192
parallel prepare threshold 4,096 commands
prepare workers            2（Phase A）
warmup                     10,000 iterations
CPU affinity               2-7（若環境不同，執行前固定並記錄一次）
component target           measured >=60 seconds
formal target              pilot 20 seconds；acceptance >=15 seconds
data / CSV                 每個 case 唯一路徑且執行前不存在
```

測試期間不得清 page cache、調整 governor／I/O scheduler／mount options／sysctl，或同時執行其他高
CPU／I/O 工作。

## 4. 建置與 correctness gate

在 repository root 執行：

```bash
TOOLS=/tmp/order_books-tools/bin
CONAN="$TOOLS/conan"
CMAKE="$TOOLS/cmake"
CTEST="$TOOLS/ctest"
test -x "$CONAN" && test -x "$CMAKE" && test -x "$CTEST"

"$CONAN" install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset release-benchmark
"$CMAKE" --build --preset release-benchmark
"$CTEST" --test-dir build/ReleaseBenchmark --output-on-failure

"$CONAN" install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset debug
"$CMAKE" --build --preset debug
"$CTEST" --preset debug --output-on-failure

"$CMAKE" --preset sanitizers
"$CMAKE" --build --preset sanitizers
"$CTEST" --preset sanitizers --output-on-failure

"$CTEST" --test-dir build/ReleaseBenchmark \
  -R 'engine_tail_telemetry|engine_tail_state_sampling' --output-on-failure

BENCH_BIN=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCH_BIN"
```

記錄三個完整 CTest 的通過數與 targeted telemetry test 通過數。任一失敗即停止；sanitizer binary 不用於
效能量測。

## 5. 建立 run root、身份與環境

`RUN_PARENT` 必須位於要評估的 WAL filesystem：

```bash
set -o pipefail
CPU_SET=2-7
RUN_PARENT=/home/neojhou
test -d "$RUN_PARENT"
findmnt -T "$RUN_PARENT"

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-sync-tail-post-review-XXXXXXXX")
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data" "$RUN_ROOT/telemetry" \
  "$RUN_ROOT/time" "$RUN_ROOT/iostat" "$RUN_ROOT/process-io"

git rev-parse HEAD > "$RUN_ROOT/logs/source-identity-before.txt"
git status --short >> "$RUN_ROOT/logs/source-identity-before.txt"
git diff --cached --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-before.txt"
git diff --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-before.txt"
git ls-files --others --exclude-standard -z \
  | xargs -0 -r sha256sum >> "$RUN_ROOT/logs/source-identity-before.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/source-identity-before.txt"

uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
lsblk -o NAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS >> "$RUN_ROOT/logs/environment.txt"
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor \
  >> "$RUN_ROOT/logs/environment.txt" 2>&1 || true
taskset -c "$CPU_SET" true

for tool in /usr/bin/time taskset iostat pidstat; do
  command -v "$tool" >> "$RUN_ROOT/logs/tool-availability.txt" 2>&1 || true
done
```

## 6. 通用執行函式

`run_case <case> <off|collector|full> <yes|no> <iterations> <workers>`。`observed=yes` 會以相同頻率啟用
外部監測；Phase A 三模式與 Phase B 正式輪必須使用 `yes`，Phase B pilot／calibration 使用 `no`。

```bash
COMMON_ARGS=(
  --workload=engine_durable_single_instrument
  --engine-group-size=4096
  --engine-group-delay-us=1000
  --engine-producer-lanes=8192
  --wal-parallel-prepare-min-commands=4096
  --warmup=10000
)

run_case() {
  case_name=$1
  telemetry_mode=$2
  observed=$3
  iterations=$4
  workers=$5

  case_data="$RUN_ROOT/data/$case_name"
  case_csv="$RUN_ROOT/telemetry/$case_name.csv"
  case_log="$RUN_ROOT/logs/$case_name.log"
  case_time="$RUN_ROOT/time/$case_name.time"
  case_status="$RUN_ROOT/logs/$case_name.status"
  case_meta="$RUN_ROOT/logs/$case_name.meta"
  child_pid_file="$RUN_ROOT/logs/$case_name.pid"

  test ! -e "$case_data" || return 2
  test ! -e "$case_csv" || return 2

  telemetry_args=()
  case "$telemetry_mode" in
    off) ;;
    collector)
      telemetry_args+=("--engine-tail-telemetry-output=$case_csv")
      telemetry_args+=(--engine-tail-state-sampling=off)
      ;;
    full)
      telemetry_args+=("--engine-tail-telemetry-output=$case_csv")
      telemetry_args+=(--engine-tail-state-sampling=on)
      ;;
    *) return 2 ;;
  esac

  command_args=(
    taskset -c "$CPU_SET" "$BENCH_BIN"
    "${COMMON_ARGS[@]}"
    --wal-prepare-workers="$workers"
    --iterations="$iterations"
    "${telemetry_args[@]}"
    --data-dir="$case_data"
  )

  {
    date --iso-8601=ns
    awk '{print $1}' /proc/uptime
    printf 'mode=%s observed=%s iterations=%s workers=%s\n' \
      "$telemetry_mode" "$observed" "$iterations" "$workers"
    printf 'command='
    printf '%q ' "${command_args[@]}"
    printf '\n'
  } > "$case_meta"

  monitor_status=0
  if test "$observed" = no; then
    if /usr/bin/time -v -o "$case_time" \
      "${command_args[@]}" > "$case_log" 2>&1; then
      command_status=0
    else
      command_status=$?
    fi
  elif test "$observed" = yes; then
    command -v iostat >/dev/null 2>&1 || return 2
    command -v pidstat >/dev/null 2>&1 || return 2

    iostat -y -xz -t 1 > "$RUN_ROOT/iostat/$case_name.log" 2>&1 &
    iostat_pid=$!

    /usr/bin/time -v -o "$case_time" \
      bash -c '
        pid_file=$1
        shift
        "$@" &
        child=$!
        printf "%s\n" "$child" > "$pid_file"
        wait "$child"
      ' benchmark-runner "$child_pid_file" \
      "${command_args[@]}" > "$case_log" 2>&1 &
    runner_pid=$!

    for attempt in $(seq 1 100); do
      test -s "$child_pid_file" && break
      sleep 0.05
    done
    if test ! -s "$child_pid_file"; then
      kill -INT "$iostat_pid" 2>/dev/null || true
      wait "$iostat_pid" 2>/dev/null || true
      wait "$runner_pid" 2>/dev/null || true
      return 2
    fi

    benchmark_pid=$(tr -d '[:space:]' < "$child_pid_file")
    pidstat -d -p "$benchmark_pid" 1 \
      > "$RUN_ROOT/process-io/$case_name.log" 2>&1 &
    process_monitor_pid=$!

    if wait "$runner_pid"; then
      command_status=0
    else
      command_status=$?
    fi
    for monitor_pid in "$process_monitor_pid" "$iostat_pid"; do
      if kill -0 "$monitor_pid" 2>/dev/null; then
        kill -INT "$monitor_pid" 2>/dev/null || true
      fi
      wait "$monitor_pid" 2>/dev/null || true
    done
    test -s "$RUN_ROOT/iostat/$case_name.log" || monitor_status=2
    test -s "$RUN_ROOT/process-io/$case_name.log" || monitor_status=2
  else
    return 2
  fi

  date --iso-8601=ns >> "$case_meta"
  awk '{print $1}' /proc/uptime >> "$case_meta"
  printf '%s\n' "$command_status" > "$case_status"

  test "$command_status" -eq 0 || return "$command_status"
  test "$monitor_status" -eq 0 || return "$monitor_status"
  if test "$telemetry_mode" = off; then
    test ! -e "$case_csv" || return 2
  else
    test -s "$case_csv" || return 2
  fi
}
```

函式只停止自己記錄的 monitor PID，不得使用 `killall`、`pkill` 或 process-name matching。

## 7. Phase A 操作

### 7.1 三模式 pilot 與共同 iterations

新 binary 必須重新 pilot，不沿用修正前的 1,750,962：

```bash
COMPONENT_PILOT_ITERATIONS=400000
run_case component-pilot-off off yes "$COMPONENT_PILOT_ITERATIONS" 2
run_case component-pilot-collector collector yes "$COMPONENT_PILOT_ITERATIONS" 2
run_case component-pilot-full full yes "$COMPONENT_PILOT_ITERATIONS" 2
```

從三行 summary 取得 `elapsed_ms`：

```bash
summary_value() {
  tr ' ' '\n' < "$1" | awk -F= -v key="$2" '$1 == key {print $2; exit}'
}

candidate_iterations() {
  awk -v iterations="$1" -v elapsed_ms="$2" '
    BEGIN {
      value = iterations * 60000 / elapsed_ms
      rounded = int(value)
      if (rounded < value) rounded += 1
      print rounded
    }'
}

for mode in off collector full; do
  log="$RUN_ROOT/logs/component-pilot-$mode.log"
  elapsed=$(summary_value "$log" elapsed_ms)
  candidate_iterations "$COMPONENT_PILOT_ITERATIONS" "$elapsed"
done > "$RUN_ROOT/logs/component-candidates.txt"

COMPONENT_ITERATIONS=$(sort -n "$RUN_ROOT/logs/component-candidates.txt" | tail -n 1)
test "$COMPONENT_ITERATIONS" -gt 0
printf '%s\n' "$COMPONENT_ITERATIONS" > "$RUN_ROOT/logs/component-iterations.txt"
```

使用最大 candidate，讓最快模式也達到約 60 秒；pilot 不納入 bias。

### 7.2 固定九輪 component calibration

```bash
run_case component-r1-off off yes "$COMPONENT_ITERATIONS" 2
run_case component-r1-collector collector yes "$COMPONENT_ITERATIONS" 2
run_case component-r1-full full yes "$COMPONENT_ITERATIONS" 2

run_case component-r2-full full yes "$COMPONENT_ITERATIONS" 2
run_case component-r2-off off yes "$COMPONENT_ITERATIONS" 2
run_case component-r2-collector collector yes "$COMPONENT_ITERATIONS" 2

run_case component-r3-collector collector yes "$COMPONENT_ITERATIONS" 2
run_case component-r3-full full yes "$COMPONENT_ITERATIONS" 2
run_case component-r3-off off yes "$COMPONENT_ITERATIONS" 2
```

任何一輪失敗都停止。即使慢輪是環境 tail，也保留原 artifact，不以同名或新名字補跑取代。

### 7.3 Artifact 與語意檢查

九個 status 必須都是 0，elapsed 必須至少 60 秒：

```bash
for status_file in "$RUN_ROOT"/logs/component-r*.status; do
  test "$(tr -d '[:space:]' < "$status_file")" = 0 || exit 1
done

for log in "$RUN_ROOT"/logs/component-r*.log; do
  elapsed=$(summary_value "$log" elapsed_ms)
  awk -v value="$elapsed" 'BEGIN { exit !(value >= 60000) }' || exit 1
done
```

若任一輪不足60秒，整個Phase A attempt判為invalid。保留原`RUN_ROOT`，提高
`COMPONENT_ITERATIONS`後以新的`RUN_ROOT`從第5節重新開始；不得刪除舊artifact或在原目錄覆跑同名case。

逐模式驗證：

- `off`：summary沒有`tail_telemetry=on`，且沒有CSV；
- `collector`：`tail_state_sampling=off`、`drain_state_sample_count=0`，CSV有sync／group但沒有state；
- `full`：`tail_state_sampling=on`，至少一筆measured state與兩筆drain state，summary first／last皆為整數；
- collector／full：sync row count、group row count與group command sum等於summary，且
  `telemetry_dropped_samples=0`；
- full：CSV第一／最後一筆drain row的events／bytes／age分別等於summary first／last。

使用下列函式逐輪驗證，不只檢查CSV存在：

```bash
validate_telemetry_case() {
  case_name=$1
  mode=$2
  log="$RUN_ROOT/logs/$case_name.log"
  csv="$RUN_ROOT/telemetry/$case_name.csv"

  if test "$mode" = off; then
    test ! -e "$csv" || return 1
    ! rg -q 'tail_telemetry=on' "$log" || return 1
    return 0
  fi

  test -s "$csv" || return 1
  read csv_sync csv_groups csv_commands measured_states drain_states \
       first_events first_bytes first_age last_events last_bytes last_age < <(
    awk -F, '
      function uint(value) { return value ~ /^[0-9]+$/ }
      NR == 1 {
        expected = "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns"
        if ($0 != expected) bad = 1
        next
      }
      NF != 8 { bad = 1; next }
      $1 == "sync" {
        if ($2 != "measured" || !uint($3) || !uint($4) || $5 $6 $7 $8 != "") bad = 1
        sync += 1
        next
      }
      $1 == "group_commands" {
        if ($2 != "measured" || !uint($3) || !uint($4) || $5 $6 $7 $8 != "") bad = 1
        groups += 1
        commands += $4
        next
      }
      $1 == "state" && $2 == "measured" {
        if (!uint($3) || $4 != "" || !uint($5) || !uint($6) || !uint($7) || !uint($8)) bad = 1
        measured += 1
        next
      }
      $1 == "state" && $2 == "drain" {
        if (!uint($3) || $4 != "" || !uint($5) || !uint($6) || !uint($7) || !uint($8)) bad = 1
        if (drain == 0) {
          first_events = $6
          first_bytes = $7
          first_age = $8
        }
        last_events = $6
        last_bytes = $7
        last_age = $8
        drain += 1
        next
      }
      { bad = 1 }
      END {
        if (bad || sync == 0 || groups == 0) exit 2
        printf "%.0f %.0f %.0f %.0f %.0f %s %s %s %s %s %s\n",
               sync, groups, commands, measured, drain,
               first_events, first_bytes, first_age, last_events, last_bytes, last_age
      }' "$csv"
  ) || return 1

  test "$csv_sync" = "$(summary_value "$log" measured_sync_count)" || return 1
  test "$csv_groups" = "$(summary_value "$log" measured_group_sample_count)" || return 1
  test "$csv_commands" = "$(summary_value "$log" measured_group_sample_commands)" || return 1
  test "$(summary_value "$log" telemetry_dropped_samples)" = 0 || return 1

  if test "$mode" = collector; then
    rg -q 'tail_state_sampling=off' "$log" || return 1
    test "$measured_states" = 0 || return 1
    test "$drain_states" = 0 || return 1
    test "$(summary_value "$log" drain_state_sample_count)" = 0 || return 1
    return 0
  fi

  test "$mode" = full || return 1
  rg -q 'tail_state_sampling=on' "$log" || return 1
  test "$measured_states" -ge 1 || return 1
  test "$drain_states" -ge 2 || return 1
  test "$drain_states" = "$(summary_value "$log" drain_state_sample_count)" || return 1
  test "$first_events" = "$(summary_value "$log" drain_publisher_lag_events_first)" || return 1
  test "$first_bytes" = "$(summary_value "$log" drain_publisher_lag_bytes_first)" || return 1
  test "$first_age" = "$(summary_value "$log" drain_publisher_lag_age_ns_first)" || return 1
  test "$last_events" = "$(summary_value "$log" drain_publisher_lag_events_last)" || return 1
  test "$last_bytes" = "$(summary_value "$log" drain_publisher_lag_bytes_last)" || return 1
  test "$last_age" = "$(summary_value "$log" drain_publisher_lag_age_ns_last)" || return 1
}

for round in 1 2 3; do
  validate_telemetry_case "component-r${round}-off" off || exit 1
  validate_telemetry_case "component-r${round}-collector" collector || exit 1
  validate_telemetry_case "component-r${round}-full" full || exit 1
done

# 另輸出人類可讀的CSV計數，供報告逐輪表引用。
for mode in collector full; do
  for round in 1 2 3; do
    csv="$RUN_ROOT/telemetry/component-r${round}-${mode}.csv"
    awk -F, '
      NR == 1 {
        expected = "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns"
        if ($0 != expected) exit 2
        next
      }
      $1 == "sync" && $2 == "measured" { sync += 1; next }
      $1 == "group_commands" && $2 == "measured" { groups += 1; commands += $4; next }
      $1 == "state" && $2 == "measured" { measured_state += 1; next }
      $1 == "state" && $2 == "drain" { drain_state += 1; next }
      { exit 2 }
      END {
        if (sync == 0 || groups == 0) exit 2
        printf "sync=%d groups=%d commands=%.0f measured_state=%d drain_state=%d\n",
               sync, groups, commands, measured_state, drain_state
      }' "$csv" || exit 1
  done
done
```

### 7.4 計算 component gate

```bash
median_three() {
  printf '%s\n%s\n%s\n' "$1" "$2" "$3" | sort -n | sed -n '2p'
}

for mode in off collector full; do
  r1=$(summary_value "$RUN_ROOT/logs/component-r1-$mode.log" commands_per_second)
  r2=$(summary_value "$RUN_ROOT/logs/component-r2-$mode.log" commands_per_second)
  r3=$(summary_value "$RUN_ROOT/logs/component-r3-$mode.log" commands_per_second)
  median_three "$r1" "$r2" "$r3"
done > "$RUN_ROOT/logs/component-medians.txt"

OFF_MEDIAN=$(sed -n '1p' "$RUN_ROOT/logs/component-medians.txt")
COLLECTOR_MEDIAN=$(sed -n '2p' "$RUN_ROOT/logs/component-medians.txt")
FULL_MEDIAN=$(sed -n '3p' "$RUN_ROOT/logs/component-medians.txt")

awk -v off="$OFF_MEDIAN" -v collector="$COLLECTOR_MEDIAN" -v full="$FULL_MEDIAN" '
  function abs(value) { return value < 0 ? -value : value }
  BEGIN {
    collector_bias = abs(collector - off) / off * 100
    sampler_increment = abs(full - collector) / collector * 100
    full_bias = abs(full - off) / off * 100
    printf "off_median_rps=%.6f\n", off
    printf "collector_median_rps=%.6f\n", collector
    printf "full_median_rps=%.6f\n", full
    printf "collector_bias_pct=%.6f\n", collector_bias
    printf "sampler_increment_pct=%.6f\n", sampler_increment
    printf "full_bias_pct=%.6f\n", full_bias
    if (collector_bias > 5 || sampler_increment > 5 || full_bias > 5) exit 2
  }' > "$RUN_ROOT/logs/component-gate.txt"
COMPONENT_GATE_STATUS=$?
```

`COMPONENT_GATE_STATUS != 0`時停止 Phase B，建立 blocked retest report。不要用「可能是noise」忽略 gate。

## 8. Phase B 操作（僅限 Phase A 通過）

### 8.1 W1／W2 pilot 與共同 iterations

使用同一 run root 與 binary；四種 pilot 不開外部監測：

```bash
FORMAL_PILOT_ITERATIONS=400000
run_case formal-pilot-w1-off off no "$FORMAL_PILOT_ITERATIONS" 1
run_case formal-pilot-w1-full full no "$FORMAL_PILOT_ITERATIONS" 1
run_case formal-pilot-w2-off off no "$FORMAL_PILOT_ITERATIONS" 2
run_case formal-pilot-w2-full full no "$FORMAL_PILOT_ITERATIONS" 2
```

以第7.1節相同方法把target改成20,000 ms，取四個candidate最大值作`FORMAL_ITERATIONS`。Phase B所有
calibration與正式輪使用同一值；任何有效輪measured `<15秒`時，提高共同iterations並從該階段r1重跑，
舊輪保留但標為invalid，不得刪除。重跑使用新的`RUN_ROOT`並從第5節重新記錄identity，避免覆寫同名
artifact；報告分別列出invalid與有效attempt。

### 8.2 W1／W2 off-full calibration

每個W使用固定ABBA順序，calibration不開外部監測：

```bash
run_case cal-w1-off-r1 off no "$FORMAL_ITERATIONS" 1
run_case cal-w1-full-r1 full no "$FORMAL_ITERATIONS" 1
run_case cal-w1-full-r2 full no "$FORMAL_ITERATIONS" 1
run_case cal-w1-off-r2 off no "$FORMAL_ITERATIONS" 1
run_case cal-w1-off-r3 off no "$FORMAL_ITERATIONS" 1
run_case cal-w1-full-r3 full no "$FORMAL_ITERATIONS" 1

run_case cal-w2-off-r1 off no "$FORMAL_ITERATIONS" 2
run_case cal-w2-full-r1 full no "$FORMAL_ITERATIONS" 2
run_case cal-w2-full-r2 full no "$FORMAL_ITERATIONS" 2
run_case cal-w2-off-r2 off no "$FORMAL_ITERATIONS" 2
run_case cal-w2-off-r3 off no "$FORMAL_ITERATIONS" 2
run_case cal-w2-full-r3 full no "$FORMAL_ITERATIONS" 2
```

分別計算 `abs(median(full)-median(off))/median(off)*100`；W1與W2都必須`<=5%`。失敗即停止，不執行
正式矩陣。

### 8.3 正式五輪矩陣

全部使用full telemetry與外部監測：

```bash
run_case formal-w1-r1 full yes "$FORMAL_ITERATIONS" 1
run_case formal-w2-r1 full yes "$FORMAL_ITERATIONS" 2

run_case formal-w2-r2 full yes "$FORMAL_ITERATIONS" 2
run_case formal-w1-r2 full yes "$FORMAL_ITERATIONS" 1

run_case formal-w1-r3 full yes "$FORMAL_ITERATIONS" 1
run_case formal-w2-r3 full yes "$FORMAL_ITERATIONS" 2

run_case formal-w2-r4 full yes "$FORMAL_ITERATIONS" 2
run_case formal-w1-r4 full yes "$FORMAL_ITERATIONS" 1

run_case formal-w1-r5 full yes "$FORMAL_ITERATIONS" 1
run_case formal-w2-r5 full yes "$FORMAL_ITERATIONS" 2
```

必須正好10個exit-0 status、10個CSV、10個time／iostat／process-I/O artifact。每個CSV套用第7.3節的full
語意檢查。慢輪與outlier全部納入aggregate。

只有正式矩陣重現明顯sync tail時，才另跑獨立`strace`診斷；strace輪不得納入正式RPS或latency aggregate。

## 9. 執行後身份確認

在建立新report前記錄身份：

```bash
git rev-parse HEAD > "$RUN_ROOT/logs/source-identity-after.txt"
git status --short >> "$RUN_ROOT/logs/source-identity-after.txt"
git diff --cached --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
git diff --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
git ls-files --others --exclude-standard -z \
  | xargs -0 -r sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/source-identity-after.txt"
diff -u "$RUN_ROOT/logs/source-identity-before.txt" \
  "$RUN_ROOT/logs/source-identity-after.txt" \
  > "$RUN_ROOT/logs/source-identity.diff"
test ! -s "$RUN_ROOT/logs/source-identity.diff"
```

不一致時停止統計合併，報告列出差異與受影響輪次。report本身在identity確認後才建立，因此不會污染
before／after比較。

## 10. 報告格式與必要內容

### 10.1 摘要與gate

| 項目 | 結果 | Gate／判定 |
| --- | --- | --- |
| Release／Debug／ASan-UBSan | `<passed/total>` | pass/fail |
| Targeted telemetry | `<passed/total>` | pass/fail |
| Component artifact consistency | `<passed>/9` | pass/fail |
| Collector bias | `<off/collector median；%>` | `<=5%` pass/fail |
| Sampler increment | `<collector/full median；%>` | `<=5%` pass/fail |
| Full bias | `<off/full median；%>` | `<=5%` pass/fail |
| Phase B | `<executed/stopped>` | 原因 |
| Formal completeness | `<passed>/10`或`N/A` | pass/fail/not run |
| Sync tail | `<supported/not supported/inconclusive>` | 證據摘要 |
| Device/storage | `<supported/not supported/inconclusive>` | 證據摘要 |

結論先回答偏差來自collector、sampler、兩者皆非，或仍受storage tail影響；不得因gate失敗提出production
durability變更。

### 10.2 身份、環境與固定參數

記錄日期、host/kernel、CPU topology與affinity、governor/boost、filesystem/mount/block device、可用空間、
compiler/flags、HEAD、index/worktree/untracked hashes、binary hash、`RUN_ROOT`、工具版本，以及before/after
identity是否一致。

### 10.3 Correctness 與 pilot

列出各CTest通過數。Phase A pilot表：

| Mode | Pilot iterations | Elapsed ms | Candidate iterations | Selected iterations |
| --- | ---: | ---: | ---: | ---: |
| off | | | | |
| collector | | | | |
| full | | | | |

### 10.4 Component逐輪與aggregate

| Round/mode | RPS | Elapsed ms | Cmd p99/p99.9/max | Sync p99/max/total | CPU | Vol/Invol CS | Storage摘要 | Status |
| --- | ---: | ---: | --- | --- | ---: | --- | --- | --- |
| r1/off | | | | N/A | | | | |
| r1/collector | | | | | | | | |
| r1/full | | | | | | | | |
| ... | | | | | | | | |

另列三種mode的RPS median/min/max、三個bias、CSV consistency，以及full drain start/end的lag events／bytes／
age。off沒有sync telemetry時填`N/A`，不得填0。

### 10.5 Phase B（若未執行則明確標N/A）

- 四個pilot與共同`FORMAL_ITERATIONS`；
- W1/W2 off/full calibration逐輪、median與bias gate；
- 正式W1/W2各五輪完整性；
- throughput、command latency、sync latency與tail count aggregate；
- queue／publisher drain收斂與同輪storage evidence；
- 每輪RPS、elapsed、CPU、context switches、WAL MiB/s、parallel groups/tasks、artifact path與status。

### 10.6 根因判定、限制與下一步

逐項使用`supported`、`not supported`或`inconclusive`：

- collector overhead；
- state sampler contention；
- application sync tail；
- device/storage；
- filesystem/syscall；
- queue/publisher pressure；
- parallel-prepare interaction。

限制至少包含closed-loop workload、單一host/storage、10 ms application state與1秒external sampling解析度，
以及沒有production fixed-rate arrival。下一步只能選擇證據直接支持的一個最小方向；若Phase A失敗，下一步
是處理失敗component或補同期storage證據，不是調整durability、group delay或prepare default。
