# WAL sync／storage tail 根因分析壓測操作與報告規格

## 1. 目的與適用範圍

本文件定義如何完成 `docs/wal-sync-storage-tail-root-cause-analysis-design.md` 的正式量測與報告。
目標是把 measured window 的 WAL sync latency、Engine queue／publisher lag、process I/O 與 block-device
狀態對齊，判斷先前慢輪是否由 storage tail 解釋。這不是新一輪 W=2 rollout gate，也不調整 production
設定、group policy、WAL durability 或 staging。

本流程只回答：

1. throughput 驟降是否和 measured sync total／tail 同時發生；
2. application sync tail 是否有同期 device、filesystem 或 syscall 證據；
3. queue／publisher lag 是否持續累積且在 drain 後未收斂；
4. 排除不同 sync distribution 後，W=2 是否仍呈現可重現的 CPU／throughput 差異。

舊報告可作歷史背景，但不得和本次輪次合併計算。使用修正前約 20 ms sampler cadence 產生的 telemetry
artifact 不納入本次統計。

## 2. 完成條件與停止條件

### 2.1 必要完成條件

- ReleaseBenchmark、Debug、ASan／UBSan 全量測試通過；sanitizer binary 不量效能。
- telemetry smoke、空 path、錯誤 workload 與 CSV consistency tests 通過。
- W=1、W=2 各自完成 telemetry off/on calibration；每個 mode 至少三輪、每輪 measured >=15 秒。
- W=1、W=2 的 telemetry-on median throughput regression 均不超過各自 telemetry-off median 的 5%。
- 正式矩陣 W=1、W=2 各五輪；每輪使用全新 data directory 與唯一 CSV，measured >=15 秒。
- 所有正式輪 exit 0、`telemetry_dropped_samples=0`，且 benchmark 內建 replay、durable head、completion、
  sample count 與 group command aggregate validation 全部通過。
- 正式輪保存 benchmark summary、CSV、`/usr/bin/time -v`、`iostat`，以及 `pidstat` 或 `/proc/<pid>/io`。
- 所有有效慢輪與 tail 輪都保留；不得因結果不利而重跑或排除。

### 2.2 必須停止的情況

- correctness、replay、durable head、shutdown、CSV validation 或 sanitizer 失敗；
- identity、binary、固定參數、CPU affinity、filesystem 或背景負載在正式矩陣中途改變；
- 任一正式輪 measured <15 秒；提高共同 iterations 後必須從正式 r1 全部重跑；
- telemetry bias >5%。此時只可保留 calibration 結果，先另案降低 instrumentation 成本並重新
  calibration；不得用受干擾資料作定量歸因。

工具權限不足不等於程式失敗，但缺少 `iostat` 或 process I/O 證據時不得判定 device/storage 根因成立。

## 3. 固定條件與矩陣

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
parallel threshold         4,096 commands
warmup                     10,000 iterations（20,000 commands）
CPU affinity               同一組預先記錄的 CPUs；以下範例為 2-7
filesystem                 WAL 目標 Linux 實體 filesystem；不得使用 tmpfs／overlay
formal rounds              W=1 五輪、W=2 五輪
measured duration          每輪至少 15 秒；pilot 以 20 秒為目標
data / telemetry path      每輪唯一且執行前不存在
```

`iterations` 是 crossing pair 數；measured commands 為 `2 * iterations`。

| Case | Prepare workers | Telemetry | 用途 |
| --- | ---: | --- | --- |
| `w1-off` | 1 | off | W=1 calibration baseline |
| `w1-on` | 1 | on | W=1 instrumentation bias |
| `w2-off` | 2 | off | W=2 calibration baseline |
| `w2-on` | 2 | on | W=2 instrumentation bias及正式根因資料 |
| `w4-diagnostic` | 4 | on | 只有 W1/W2 證據不足時另跑，不納入正式 gate |

正式矩陣只比較 W=1/W=2 telemetry-on；通過 calibration 後，兩者受到相同 telemetry 與外部低頻監測。
W=4 不得取代 W=2，也不得用來重啟 rollout 判定。

## 4. 建置與 correctness gate

在 repository root 執行。此環境的工具位於 `/tmp/order_books-tools/bin`；若日後改用系統工具，只能替換
工具路徑，不得改 build preset 或 flags。

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
"$CTEST" --preset debug

"$CMAKE" --preset sanitizers
"$CMAKE" --build --preset sanitizers
"$CTEST" --preset sanitizers

BENCH_BIN=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCH_BIN"
```

任一必要測試失敗即停止。TSan 僅在環境可正常啟動時執行；runtime mapping 或權限限制只能記成
`not run`，不能宣稱通過。

## 5. 建立 artifact、凍結身份與記錄環境

`RUN_PARENT` 必須位於實際要評估的 WAL filesystem。下列 `/home/neojhou` 是目前環境範例，正式執行前
必須用 `findmnt` 確認不是 tmpfs 或 overlay，並預留足夠空間。

```bash
set -o pipefail
CPU_SET=2-7
RUN_PARENT=/home/neojhou
test -d "$RUN_PARENT"
findmnt -T "$RUN_PARENT"

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-sync-tail-analysis-XXXXXXXX")
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data" "$RUN_ROOT/telemetry" \
  "$RUN_ROOT/time" "$RUN_ROOT/iostat" "$RUN_ROOT/process-io" "$RUN_ROOT/diagnostic"

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

for tool in /usr/bin/time taskset iostat pidstat strace; do
  command -v "$tool" >> "$RUN_ROOT/logs/tool-availability.txt" 2>&1 || true
done
```

測試時不得清 page cache、改 governor、I/O scheduler、mount options、sysctl 或 perf security setting。
避免同時執行其他高 CPU／高 I/O 工作。正式矩陣結束後、建立 report 前，以相同命令建立
`source-identity-after.txt`；before/after 不一致時不得合併結果。

## 6. 通用執行函式

以下函式確保 data path 與 CSV 不會重用。第三個參數 `observed` 只在正式矩陣設為 `yes`；calibration
設為 `no`，避免外部工具影響 off/on bias。正式輪的 `iostat` 與 process I/O 取樣頻率固定為 1 秒。

```bash
run_case() {
  case_name=$1
  telemetry_mode=$2
  observed=$3
  shift 3

  case_data="$RUN_ROOT/data/$case_name"
  case_csv="$RUN_ROOT/telemetry/$case_name.csv"
  case_log="$RUN_ROOT/logs/$case_name.log"
  case_time="$RUN_ROOT/time/$case_name.time"
  case_status="$RUN_ROOT/logs/$case_name.status"
  case_meta="$RUN_ROOT/logs/$case_name.meta"
  child_pid_file="$RUN_ROOT/logs/$case_name.pid"
  monitor_status=0

  test ! -e "$case_data" || return 2
  test ! -e "$case_csv" || return 2

  telemetry_args=()
  if test "$telemetry_mode" = on; then
    telemetry_args+=("--engine-tail-telemetry-output=$case_csv")
  elif test "$telemetry_mode" != off; then
    echo "invalid telemetry mode: $telemetry_mode" >&2
    return 2
  fi

  date --iso-8601=ns > "$case_meta"
  awk '{print $1}' /proc/uptime >> "$case_meta"

  if test "$observed" = no; then
    /usr/bin/time -v -o "$case_time" \
      taskset -c "$CPU_SET" "$BENCH_BIN" "$@" "${telemetry_args[@]}" \
      "--data-dir=$case_data" > "$case_log" 2>&1
    command_status=$?
  elif test "$observed" = yes; then
    command -v iostat >/dev/null 2>&1 || {
      echo "iostat is required for an observed run" >&2
      return 2
    }
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
      taskset -c "$CPU_SET" "$BENCH_BIN" "$@" "${telemetry_args[@]}" \
      "--data-dir=$case_data" > "$case_log" 2>&1 &
    runner_pid=$!

    for attempt in $(seq 1 100); do
      test -s "$child_pid_file" && break
      sleep 0.05
    done
    if test ! -s "$child_pid_file"; then
      kill -INT "$iostat_pid" 2>/dev/null || true
      wait "$iostat_pid" 2>/dev/null || true
      wait "$runner_pid" 2>/dev/null || true
      echo "benchmark PID was not published" >&2
      return 2
    fi
    benchmark_pid=$(tr -d '[:space:]' < "$child_pid_file")

    if command -v pidstat >/dev/null 2>&1; then
      pidstat -d -p "$benchmark_pid" 1 \
        > "$RUN_ROOT/process-io/$case_name.log" 2>&1 &
      process_monitor_pid=$!
    else
      (
        while kill -0 "$benchmark_pid" 2>/dev/null; do
          date --iso-8601=ns
          sed -n '/^read_bytes:/p;/^write_bytes:/p;/^cancelled_write_bytes:/p' \
            "/proc/$benchmark_pid/io" 2>/dev/null || true
          sleep 1
        done
      ) > "$RUN_ROOT/process-io/$case_name.log" 2>&1 &
      process_monitor_pid=$!
    fi

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
    test -s "$RUN_ROOT/iostat/$case_name.log" || {
      echo "iostat produced no evidence" >&2
      monitor_status=2
    }
    test -s "$RUN_ROOT/process-io/$case_name.log" || {
      echo "process I/O monitor produced no evidence" >&2
      monitor_status=2
    }
  else
    echo "invalid observed mode: $observed" >&2
    return 2
  fi

  date --iso-8601=ns >> "$case_meta"
  awk '{print $1}' /proc/uptime >> "$case_meta"
  printf '%s\n' "$command_status" > "$case_status"
  rg '^engine_durable_single_instrument ' "$case_log" || true
  test "$monitor_status" -eq 0 || return "$monitor_status"
  return "$command_status"
}

COMMON_ARGS=(
  --workload=engine_durable_single_instrument
  --engine-group-size=4096
  --engine-group-delay-us=1000
  --engine-producer-lanes=8192
  --wal-parallel-prepare-min-commands=4096
  --warmup=10000
)
```

若 `iostat` 無法啟動，正式輪不要悄悄繼續；先修正工具可用性，或明確把 device/storage 判定標為
`incomplete`。函式只向已記錄的 monitor PID 傳送 `SIGINT`，不使用 process-name 或 broad kill。

## 7. Smoke 與 pilot

### 7.1 Smoke

```bash
run_case smoke-w1-on on no \
  "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations=10000
run_case smoke-w2-on on no \
  "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations=10000
```

兩輪必須 exit 0、各有一行 summary、CSV 存在，且包含：

```text
tail_telemetry=on
telemetry_dropped_samples=0
measured_sync_count > 0
measured_sync_count == measured_group_sample_count == wal_group_commits
measured_group_sample_commands == wal_group_commands == commands
```

Smoke 不納入 calibration 或正式統計。

### 7.2 Pilot 與共同 iterations

先以 400,000 iterations 各跑 W1/W2、off/on 一輪：

```bash
PILOT_ITERATIONS=400000
run_case pilot-w1-off off no "${COMMON_ARGS[@]}" \
  --wal-prepare-workers=1 --iterations="$PILOT_ITERATIONS"
run_case pilot-w1-on on no "${COMMON_ARGS[@]}" \
  --wal-prepare-workers=1 --iterations="$PILOT_ITERATIONS"
run_case pilot-w2-off off no "${COMMON_ARGS[@]}" \
  --wal-prepare-workers=2 --iterations="$PILOT_ITERATIONS"
run_case pilot-w2-on on no "${COMMON_ARGS[@]}" \
  --wal-prepare-workers=2 --iterations="$PILOT_ITERATIONS"
```

從四行 summary 取得 `elapsed_ms`，分別計算：

```text
candidate_iterations = ceil(PILOT_ITERATIONS * 20,000 / pilot_elapsed_ms)
FORMAL_ITERATIONS = max(四個 candidate_iterations)
```

```bash
FORMAL_ITERATIONS=<依 pilot 計算的共同正整數>
test "$FORMAL_ITERATIONS" -gt 0
```

calibration 與正式 W1/W2 全部使用同一個 `FORMAL_ITERATIONS`。第一個完整 calibration set 若任一輪
measured <15 秒，所有 calibration 輪作廢，提高共同 iterations 後從頭執行。

## 8. Telemetry bias calibration

W1 與 W2 分別使用 ABBA 順序：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3
```

W1：

```bash
run_case cal-w1-off-r1 off no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case cal-w1-on-r1  on  no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case cal-w1-on-r2  on  no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case cal-w1-off-r2 off no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case cal-w1-off-r3 off no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case cal-w1-on-r3  on  no "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
```

W2 使用相同順序，只把 case name 改成 `cal-w2-*`、`--wal-prepare-workers=2`。分別計算：

```text
W1 bias (%) = abs(median(W1-on RPS) - median(W1-off RPS)) / median(W1-off RPS) * 100
W2 bias (%) = abs(median(W2-on RPS) - median(W2-off RPS)) / median(W2-off RPS) * 100
```

兩者都 <=5% 才進入正式矩陣。off/on 比較只能使用同一個 W，不可用 W1-off 對 W2-on。所有慢輪都
納入 median。

## 9. 正式 W1/W2 五輪矩陣

正式輪全部使用 telemetry-on 與 `observed=yes`，並交錯順序：

```text
r1: W1, W2
r2: W2, W1
r3: W1, W2
r4: W2, W1
r5: W1, W2
```

```bash
run_case formal-w1-r1 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case formal-w2-r1 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations="$FORMAL_ITERATIONS"

run_case formal-w2-r2 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations="$FORMAL_ITERATIONS"
run_case formal-w1-r2 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"

run_case formal-w1-r3 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case formal-w2-r3 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations="$FORMAL_ITERATIONS"

run_case formal-w2-r4 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations="$FORMAL_ITERATIONS"
run_case formal-w1-r4 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"

run_case formal-w1-r5 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=1 --iterations="$FORMAL_ITERATIONS"
run_case formal-w2-r5 on yes "${COMMON_ARGS[@]}" --wal-prepare-workers=2 --iterations="$FORMAL_ITERATIONS"
```

完成後先做完整性檢查：

```bash
rg '^engine_durable_single_instrument ' "$RUN_ROOT"/logs/formal-*.log \
  > "$RUN_ROOT/logs/formal-summaries.txt"
test "$(wc -l < "$RUN_ROOT/logs/formal-summaries.txt")" -eq 10

for status_file in "$RUN_ROOT"/logs/formal-*.status; do
  test "$(tr -d '[:space:]' < "$status_file")" = 0 || exit 1
done

test "$(find "$RUN_ROOT/telemetry" -maxdepth 1 -name 'formal-*.csv' | wc -l)" -eq 10
if rg -n 'error_code=' "$RUN_ROOT"/logs/formal-*.log; then
  echo 'benchmark error found' >&2
  exit 1
fi
```

### 9.1 每個 CSV 與 summary 的離線一致性

benchmark 已在輸出 summary 前驗證 sample count，但正式 artifact 還要離線重算一次：

```bash
summary_value() {
  printf '%s\n' "$1" | tr ' ' '\n' | awk -F= -v key="$2" '$1 == key {print $2; exit}'
}

validate_telemetry() {
  case_name=$1
  csv="$RUN_ROOT/telemetry/$case_name.csv"
  summary=$(rg '^engine_durable_single_instrument ' "$RUN_ROOT/logs/$case_name.log")

  read csv_sync csv_groups csv_commands csv_states < <(
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
        sync += 1; next
      }
      $1 == "group_commands" {
        if ($2 != "measured" || !uint($3) || !uint($4) || $5 $6 $7 $8 != "") bad = 1
        groups += 1; commands += $4; next
      }
      $1 == "state" {
        if (($2 != "measured" && $2 != "drain") || !uint($3) || $4 != "" ||
            !uint($5) || !uint($6) || !uint($7) || !uint($8)) bad = 1
        states += 1; next
      }
      { bad = 1 }
      END {
        if (bad || sync == 0 || groups == 0 || states == 0) exit 2
        printf "%.0f %.0f %.0f %.0f\n", sync, groups, commands, states
      }
    ' "$csv"
  ) || return 1

  test "$csv_sync" = "$(summary_value "$summary" measured_sync_count)" || return 1
  test "$csv_groups" = "$(summary_value "$summary" measured_group_sample_count)" || return 1
  test "$csv_commands" = "$(summary_value "$summary" measured_group_sample_commands)" || return 1
  test "$csv_sync" = "$(summary_value "$summary" wal_group_commits)" || return 1
  test "$csv_commands" = "$(summary_value "$summary" wal_group_commands)" || return 1
  test "$(summary_value "$summary" telemetry_dropped_samples)" = 0 || return 1
}

for csv in "$RUN_ROOT"/telemetry/formal-*.csv; do
  case_name=$(basename "$csv" .csv)
  validate_telemetry "$case_name" || exit 1
done
```

## 10. 獨立 syscall 與 CPU 診斷輪

`strace` 有顯著 overhead，不能混入正式五輪。先依正式結果選擇一個正常輪對應 case及一個 tail case，
各跑一次獨立診斷。命令仍使用相同 fixed parameters與共同 iterations：

```bash
DIAG_CASE=diagnostic-w1-strace
DIAG_DATA="$RUN_ROOT/data/$DIAG_CASE"
DIAG_CSV="$RUN_ROOT/telemetry/$DIAG_CASE.csv"
test ! -e "$DIAG_DATA" && test ! -e "$DIAG_CSV"

date --iso-8601=ns > "$RUN_ROOT/diagnostic/$DIAG_CASE.meta"
awk '{print $1}' /proc/uptime >> "$RUN_ROOT/diagnostic/$DIAG_CASE.meta"
strace -ff -tt -T -e trace=fsync,fdatasync,write,pwrite64 \
  -o "$RUN_ROOT/diagnostic/$DIAG_CASE.strace" \
  taskset -c "$CPU_SET" "$BENCH_BIN" \
  "${COMMON_ARGS[@]}" --wal-prepare-workers=1 \
  --iterations="$FORMAL_ITERATIONS" \
  "--engine-tail-telemetry-output=$DIAG_CSV" \
  "--data-dir=$DIAG_DATA" \
  > "$RUN_ROOT/logs/$DIAG_CASE.log" 2>&1
```

W2 診斷只替換 case name與 workers。`strace` 只能確認慢 application sample 是否能對應到
`fsync`／write syscall；trace 下的絕對 latency與 RPS 不得當 production 數值。

`perf stat` 權限可用時，可另跑 W1/W2 各一輪取得 cycles、instructions、context-switches、
cpu-migrations與 page-faults。不得修改系統安全設定來取得 perf，也不得把 perf輪混入正式 median。

## 11. 必須擷取與計算的資料

每一正式輪至少保存：

- identity：case、round、workers、threshold、group size/delay、producer lanes、iterations、warmup；
- throughput／command latency：`commands_per_second`、`elapsed_ms`、p50、p99、p99.9、max；
- measured sync：count、p50、p99、p99.9、max、total、over 25/100/250 ms；
- group：sample count、sample commands、group commits、commands/group、parallel groups/tasks；
- backlog：measured queue max、publisher lag events/bytes/age max，以及 drain最後值；
- WAL：bytes delta、MiB/s、segment rotations、full-run p99；full-run p99含warmup，不能冒充 measured p99；
- resources：CPU percent、voluntary/involuntary context switches、maximum RSS、wall time；
- storage：device `r_await`／`w_await`、`aqu-sz`、`%util`與吞吐的range及tail窗口；
- process I/O：read/write rate或`read_bytes`／`write_bytes`增量；
- artifact：exit status、CSV row counts、dropped samples及離線validation結果。

五輪 throughput與latency至少報 median、min、max；tail count與最大值不得只報median。另計算：

```text
telemetry bias (%) = abs(on median RPS - off median RPS) / off median RPS * 100
sync wall contribution (%) = measured_sync_total_us / (elapsed_ms * 1,000) * 100
W2 throughput difference (%) = (median(W2 RPS) / median(W1 RPS) - 1) * 100
```

`sync wall contribution` 是單 writer sync等待占 measured wall time的近似比例，不是CPU利用率。

## 12. 根因判定規則

- `sync tail supported`：慢輪的 sync total／tail足以解釋大部分 elapsed增加，且RPS下降與sync tail同時出現。
- `device/storage supported`：有tail的正式輪也呈現異常device await、queue或utilization。CSV沒有absolute
  epoch anchor，所以1秒iostat只能提供同輪粗粒度證據，不能宣稱某筆sync與某秒device sample精確對齊。
  若 `%util`不高且throughput未飽和，只能稱latency tail，不能稱bandwidth ceiling。
- `filesystem/syscall supported`：獨立strace輪能把application tail對應到`fsync`／write syscall；
  strace的絕對時間不作效能結論。
- `parallel-prepare interaction supported`：W1/W2 sync distribution相近時，W2仍有可重現CPU／RPS差異。
- `queue/publisher pressure supported`：lag在多個連續10 ms state sample持續增加，且drain末值未收斂；
  單一max不能成立。
- `inconclusive`：application、process I/O、syscall與device證據不一致，或工具資料缺失。

本需求沒有production rollout gate。報告不得因W2單次較快、W4較快或某輪沒有tail而自動變更default。

## 13. 報告檔案與格式

結果寫入：

```text
docs/wal-sync-storage-tail-root-cause-analysis-benchmark-report.md
```

報告必須包含以下章節。

### 13.1 摘要與結論

| 項目 | 結果 | 判定 |
| --- | --- | --- |
| Release／Debug／ASan-UBSan | `<通過數>` | pass/fail |
| W1 telemetry bias | `<off/on median及百分比>` | pass/fail；<=5% |
| W2 telemetry bias | `<off/on median及百分比>` | pass/fail；<=5% |
| 正式輪完整性 | `<成功輪數>/10` | pass/fail |
| Artifact consistency | `<通過輪數>/10` | pass/fail |
| Sync tail | `<supported/not supported/inconclusive>` | evidence summary |
| Device/storage | `<supported/not supported/inconclusive>` | evidence summary |
| Filesystem/syscall | `<supported/not supported/not run/inconclusive>` | evidence summary |
| Queue/publisher pressure | `<supported/not supported/inconclusive>` | evidence summary |
| Parallel-prepare interaction | `<supported/not supported/inconclusive>` | evidence summary |

結論必須指出目前能解釋多少慢輪、仍缺哪些證據，以及下一案應優化storage、group policy、prepare CPU，
或先補量測；不能直接宣告production rollout。

### 13.2 Artifact、身份與環境

記錄日期、host/kernel、CPU topology、affinity、governor/boost、filesystem/mount、block device、可用空間、
compiler/flags、HEAD、index/worktree/untracked hashes、binary hash、`RUN_ROOT`、各工具版本與權限限制。
明確寫出before/after identity是否一致。

### 13.3 Correctness、smoke、pilot與calibration

| Case | Iterations | Elapsed ms | RPS | Sync count | Group count | CSV rows | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| smoke W1-on | | | | | | | |
| smoke W2-on | | | | | | | |

| W | Off RPS median | On RPS median | Bias | 最短 elapsed | Gate |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 | | | | | pass/fail |
| 2 | | | | | pass/fail |

列出四個pilot elapsed、計算出的candidate iterations及共同`FORMAL_ITERATIONS`。

### 13.4 正式 aggregate

| W | RPS median | RPS min--max | Command p99 median | Worst p99.9 | Worst max | CPU median | Invol CS median |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | | | | | | | |
| 2 | | | | | | | |

| W | Sync p50 median | Sync p99 median | Worst p99.9 | Worst max | Sync total range | >25/100/250 ms totals | Wall contribution range |
| ---: | ---: | ---: | ---: | ---: | --- | --- | --- |
| 1 | | | | | | | |
| 2 | | | | | | | |

### 13.5 Backlog 與 storage correlation

| W/round | Queue max | Lag events max | Lag age max | Drain lag last | Device await tail | aqu-sz max | util max | 判讀 |
| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- |
| W1/r1 | | | | | | | | |
| ... | | | | | | | | |

另以timeline或表格列出每個明顯sync tail的application elapsed time、duration及同一internal timeline上的
queue／publisher state。iostat與process I/O列為同輪、1秒解析度的外部證據；因CSV沒有absolute epoch
anchor，不得把某一筆sync指派給特定iostat sample。精確syscall關聯只能由獨立strace輪依呼叫順序與
duration交叉驗證，仍不可把診斷輪的絕對latency套用到正式輪。

### 13.6 逐輪明細

| W/round | RPS | Elapsed ms | Cmd p99 | Sync p99 | Sync max | Sync total ms | >25/100/250 | WAL MiB/s | CPU | Vol/Invol CS | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |
| W1/r1 | | | | | | | | | | | |
| ... | | | | | | | | | | | |

每輪補充parallel groups/tasks、WAL bytes/rotations、CSV path與是否納入。所有outlier都保留並說明。

### 13.7 Syscall診斷、限制與下一步

列出strace/perf是否執行、選輪理由、可對齊的syscall tail及工具overhead限制。限制至少包含closed-loop
workload、10 ms application state與1秒external sampling resolution、單一host/storage，以及沒有
production fixed-rate arrival。最後依第12節規則逐項下結論，再提出一個最小的下一階段設計方向。

## 14. 執行後身份確認

正式矩陣完成後、編寫report前執行：

```bash
git rev-parse HEAD > "$RUN_ROOT/logs/source-identity-after.txt"
git status --short >> "$RUN_ROOT/logs/source-identity-after.txt"
git diff --cached --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
git diff --binary | sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
git ls-files --others --exclude-standard -z \
  | xargs -0 -r sha256sum >> "$RUN_ROOT/logs/source-identity-after.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/source-identity-after.txt"
diff -u "$RUN_ROOT/logs/source-identity-before.txt" \
  "$RUN_ROOT/logs/source-identity-after.txt"
```

identity一致後才能整理報告。建立report本身會改變worktree，所以final identity必須在寫report前完成。
整個流程不得執行`git add`、`git reset`、`git restore`、`git commit`或其他會改變staging的操作。
