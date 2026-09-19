# WAL sync 最終 syscall／storage 歸因測試操作與報告規格

## 1. 目的與輸出

本文件定義如何執行 `docs/wal-sync-final-attribution-design.md` 的最後一次診斷型壓測。目標不是重新估算
production capacity，而是用共同時鐘將 application `wal_sync_latency_us`、WAL fd 的 `write`／`fsync`
syscall，以及 WAL 所在 block device 的一秒統計對齊，將根因停在可由證據支持的層級。

結果必須另寫入：

```text
docs/wal-sync-final-attribution-benchmark-report.md
```

本流程不得修改 production 程式、durability、group commit、filesystem 或 storage 設定，也不得執行
`git add`、`git reset`、`git restore`、`git commit` 等會改變 staging 的操作。

## 2. 固定 run set 與停止規則

### 2.1 固定輪次

總共正好六輪，不做 pilot，也不得因結果不理想而追加輪次：

```text
untraced-r1
traced-r1
traced-r2
untraced-r2
untraced-r3
traced-r3
```

- 三輪 `untraced`：collector-only telemetry、timestamped `iostat`、`pidstat` 與 `/usr/bin/time -v`。
- 三輪 `traced`：相同監測，另用 `strace` 捕捉 `openat`、`write`、`fsync`、`close`。
- traced 與 untraced 的絕對 RPS 分開列示，不互相比較，也不納入 capacity 結論。
- 每輪使用唯一 data、CSV、log 與 trace 路徑，不得覆寫或刪除慢輪。

固定 workload：

| 項目 | 值 |
| --- | --- |
| Build | `ReleaseBenchmark` |
| Workload | `engine_durable_single_instrument` |
| Instrument／shard | 1／1 |
| Prepare workers | 2 |
| Group size | 4,096 commands |
| Group delay | 1,000 us |
| Producer lanes | 8,192 |
| Parallel prepare threshold | 4,096 |
| Warmup | 10,000 iterations |
| Measured iterations | 5,258,626（10,517,252 commands） |
| Telemetry | collector-only，state sampling off |
| Benchmark CPU | 2-7 |
| Tracer／monitor CPU | 0-1 |
| 每輪 hard timeout | 300 seconds |
| iostat／pidstat interval | 1 second |

`5,258,626` 沿用前次有效報告中可產生約 60 秒 measured phase 的共同 iterations。本需求沒有改變
WAL、Engine 或 workload，不重新 pilot 可避免以本次結果重新挑選負載。

### 2.2 停止與有效性

執行前任一 correctness test 或工具 preflight 失敗即停止，不建立部分 run set。開始六輪後：

- benchmark、timeout、tracer 或 monitor 非零結束：保留 artifact，該輪為 invalid；
- clock anchor、CSV、WAL fd mapping 或 trace 完整性失敗：保留 artifact，最終結果為 `inconclusive`；
- 六輪都沒有任何 `>=25 ms` application sync sample：結果為 `not_reproduced`；
- tail只出現在traced或untraced其中一組：結果為`inconclusive`，分別記錄可能的observer effect或
  traced輪未重現；
- `not_reproduced` 不是 artifact failure，不得因此追加輪次；
- 不在同一 `RUN_ROOT` 重跑任何 case。若是執行前即可發現的環境錯誤，修正後必須建立新的
  `RUN_ROOT`，並在報告列出舊 attempt；不得選擇性合併兩個 attempt。

## 3. 建置與 correctness gate

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

記錄三種 build 的通過數與 targeted telemetry test 通過數。sanitizer binary 不得用於壓測。

## 4. 環境、工具與 run root

`RUN_PARENT` 必須與要評估的 WAL 位於相同 filesystem。先確認六輪約需數 GiB 可用空間：

```bash
set -o pipefail
BENCH_CPU_SET=2-7
MONITOR_CPU_SET=0-1
CASE_TIMEOUT_SECONDS=300
ITERATIONS=5258626
RUN_PARENT=/home/neojhou

for tool in /usr/bin/time timeout taskset strace iostat pidstat findmnt lsblk; do
  command -v "$tool" >/dev/null || exit 1
done
taskset -c "$BENCH_CPU_SET" true
taskset -c "$MONITOR_CPU_SET" true
test -d "$RUN_PARENT"

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-sync-final-attribution-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,telemetry,logs,time,iostat,pidstat,trace,derived}

env LC_ALL=C strace -qq -o "$RUN_ROOT/logs/strace-preflight.log" \
  -e trace=openat true
test -s "$RUN_ROOT/logs/strace-preflight.log"
```

記錄 source、binary、clock、mount 與 block topology：

```bash
{
  git rev-parse HEAD
  git status --short
  git diff --cached --binary | sha256sum
  git diff --binary | sha256sum
  git ls-files --others --exclude-standard -z | xargs -0 -r sha256sum
  sha256sum "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-before.txt"

{
  date --utc --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  timedatectl show 2>&1 || true
  strace -V
  iostat -V
  pidstat -V
} > "$RUN_ROOT/logs/environment.txt" 2>&1

findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS -T "$RUN_ROOT" \
  > "$RUN_ROOT/logs/wal-filesystem.txt"
WAL_SOURCE=$(findmnt -no SOURCE -T "$RUN_ROOT")
lsblk -s -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS "$WAL_SOURCE" \
  > "$RUN_ROOT/logs/wal-block-topology.txt"
```

若 `WAL_SOURCE` 是 dm-crypt、LVM、mdraid 或多路裝置，報告必須列出所有 leaf device；若無法唯一對應
`iostat` row，只撤回 device 判定，syscall 判定仍可保留。

測試期間不得清 page cache、調整 governor、boost、I/O scheduler、mount option、sysctl 或 NTP policy，
也不得同時執行其他 CPU／I/O 壓力工作。

## 5. 通用執行函式

以下函式只會停止自己建立並記錄的 monitor PID，不使用 `killall`、`pkill` 或模糊 process matching。
`strace -ff` 讓每個 tracee 使用獨立檔案；`-ttt` 提供 Unix epoch timestamp，`-T` 提供 syscall
duration，`-yy` 提供 fd path annotation。

```bash
COMMON_ARGS=(
  --workload=engine_durable_single_instrument
  --engine-group-size=4096
  --engine-group-delay-us=1000
  --engine-producer-lanes=8192
  --wal-prepare-workers=2
  --wal-parallel-prepare-min-commands=4096
  --warmup=10000
  --iterations="$ITERATIONS"
  --engine-tail-state-sampling=off
)

run_case() {
  case_name=$1
  trace_mode=$2

  case_data="$RUN_ROOT/data/$case_name"
  case_csv="$RUN_ROOT/telemetry/$case_name.csv"
  case_stdout="$RUN_ROOT/logs/$case_name.stdout"
  case_stderr="$RUN_ROOT/logs/$case_name.stderr"
  case_meta="$RUN_ROOT/logs/$case_name.meta"
  case_status="$RUN_ROOT/logs/$case_name.status"
  case_time="$RUN_ROOT/time/$case_name.time"
  case_pid_file="$RUN_ROOT/logs/$case_name.pid"
  trace_prefix="$RUN_ROOT/trace/$case_name"

  test "$trace_mode" = traced || test "$trace_mode" = untraced || return 2
  test ! -e "$case_data" && test ! -e "$case_csv" && \
    test ! -e "$case_stdout" && test ! -e "$case_stderr" && \
    test ! -e "$case_status" || return 2

  command_args=(
    env LC_ALL=C taskset -c "$BENCH_CPU_SET" "$BENCH_BIN"
    "${COMMON_ARGS[@]}"
    --engine-tail-telemetry-output="$case_csv"
    --data-dir="$case_data"
  )

  {
    date --utc --iso-8601=ns
    awk '{print $1}' /proc/uptime
    printf 'trace_mode=%s timeout_seconds=%s\n' \
      "$trace_mode" "$CASE_TIMEOUT_SECONDS"
    printf 'command='
    printf '%q ' "${command_args[@]}"
    printf '\n'
  } > "$case_meta"

  env LC_ALL=C TZ=UTC S_TIME_FORMAT=ISO S_TIME_DEF_TIME=UTC \
    taskset -c "$MONITOR_CPU_SET" iostat -y -x -z -t 1 \
    > "$RUN_ROOT/iostat/$case_name.log" 2>&1 &
  iostat_pid=$!

  if test "$trace_mode" = traced; then
    runner=(
      taskset -c "$MONITOR_CPU_SET"
      strace -ff -ttt -T -yy -s 256
      -e trace=openat,write,fsync,close
      -o "$trace_prefix"
    )
  else
    runner=()
  fi

  /usr/bin/time -v -o "$case_time" \
    timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
    "${runner[@]}" \
    bash -c '
      pid_file=$1
      shift
      printf "%s\n" "$$" > "$pid_file"
      exec "$@"
    ' benchmark-runner "$case_pid_file" "${command_args[@]}" \
    > "$case_stdout" 2> "$case_stderr" &
  runner_pid=$!

  for attempt in $(seq 1 200); do
    test -s "$case_pid_file" && break
    kill -0 "$runner_pid" 2>/dev/null || break
    sleep 0.05
  done

  monitor_status=0
  if test -s "$case_pid_file"; then
    benchmark_pid=$(tr -d '[:space:]' < "$case_pid_file")
    env LC_ALL=C taskset -c "$MONITOR_CPU_SET" \
      pidstat -d -p "$benchmark_pid" 1 \
      > "$RUN_ROOT/pidstat/$case_name.log" 2>&1 &
    pidstat_pid=$!
  else
    monitor_status=2
    pidstat_pid=
  fi

  if wait "$runner_pid"; then
    command_status=0
  else
    command_status=$?
  fi

  for monitor_pid in "$pidstat_pid" "$iostat_pid"; do
    test -n "$monitor_pid" || continue
    if kill -0 "$monitor_pid" 2>/dev/null; then
      kill -INT "$monitor_pid" 2>/dev/null || true
    fi
    wait "$monitor_pid" 2>/dev/null || true
  done

  test -s "$RUN_ROOT/iostat/$case_name.log" || monitor_status=2
  test -s "$RUN_ROOT/pidstat/$case_name.log" || monitor_status=2
  rg -q 'Device' "$RUN_ROOT/iostat/$case_name.log" || monitor_status=2
  rg -q 'kB_rd/s' "$RUN_ROOT/pidstat/$case_name.log" || monitor_status=2
  if test "$trace_mode" = traced; then
    compgen -G "$trace_prefix.*" >/dev/null || monitor_status=2
  fi

  {
    date --utc --iso-8601=ns
    awk '{print $1}' /proc/uptime
  } >> "$case_meta"
  printf 'command_status=%s\nmonitor_status=%s\n' \
    "$command_status" "$monitor_status" > "$case_status"

  test "$command_status" -eq 0 && test "$monitor_status" -eq 0
}
```

## 6. 執行固定六輪

```bash
run_case untraced-r1 untraced || exit 1
run_case traced-r1 traced || exit 1
run_case traced-r2 traced || exit 1
run_case untraced-r2 untraced || exit 1
run_case untraced-r3 untraced || exit 1
run_case traced-r3 traced || exit 1
```

若任何一輪函式回傳非零，停止後續輪次並保留整個 `RUN_ROOT`。這種 attempt 不可產生成功歸因，只能在
報告列為 `inconclusive` 與說明未執行輪次。

## 7. Artifact validation

### 7.1 Summary、clock 與 CSV

```bash
summary_value() {
  tr ' ' '\n' < "$1" | awk -F= -v key="$2" '$1 == key {print $2; exit}'
}

validate_case() {
  case_name=$1
  trace_mode=$2
  stdout="$RUN_ROOT/logs/$case_name.stdout"
  csv="$RUN_ROOT/telemetry/$case_name.csv"
  status="$RUN_ROOT/logs/$case_name.status"

  test "$(rg -c '^engine_durable_single_instrument ' "$stdout")" = 1 || return 1
  rg -q '^command_status=0$' "$status" || return 1
  rg -q '^monitor_status=0$' "$status" || return 1
  test -s "$csv" || return 1
  rg -q 'tail_telemetry=on' "$stdout" || return 1
  rg -q 'tail_state_sampling=off' "$stdout" || return 1
  test "$(summary_value "$stdout" telemetry_dropped_samples)" = 0 || return 1

  start=$(summary_value "$stdout" tail_clock_start_realtime_epoch_ns)
  start_u=$(summary_value "$stdout" tail_clock_start_uncertainty_ns)
  end=$(summary_value "$stdout" tail_clock_end_realtime_epoch_ns)
  elapsed=$(summary_value "$stdout" tail_clock_end_steady_elapsed_ns)
  end_u=$(summary_value "$stdout" tail_clock_end_uncertainty_ns)
  for value in "$start" "$start_u" "$end" "$elapsed" "$end_u"; do
    test -n "$value" && test "$value" -ge 0 || return 1
  done
  test "$start" -gt 0 && test "$end" -gt 0 && test "$elapsed" -gt 0 || return 1

  realtime_delta=$((end - start))
  if test "$realtime_delta" -ge "$elapsed"; then
    drift=$((realtime_delta - elapsed))
  else
    drift=$((elapsed - realtime_delta))
  fi
  tolerance=$((start_u + end_u + 1000))
  test "$drift" -le "$tolerance" || return 1

  read csv_sync csv_groups csv_commands < <(
    awk -F, '
      NR == 1 {
        expected = "record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns"
        if ($0 != expected) bad = 1
        next
      }
      NF != 8 { bad = 1; next }
      $1 == "sync" && $2 == "measured" { sync += 1; next }
      $1 == "group_commands" && $2 == "measured" {
        groups += 1; commands += $4; next
      }
      $1 == "state" { bad = 1; next }
      { bad = 1 }
      END {
        if (bad || sync == 0 || groups == 0) exit 2
        printf "%.0f %.0f %.0f\n", sync, groups, commands
      }' "$csv"
  ) || return 1

  test "$csv_sync" = "$(summary_value "$stdout" measured_sync_count)" || return 1
  test "$csv_groups" = "$(summary_value "$stdout" measured_group_sample_count)" || return 1
  test "$csv_commands" = "$(summary_value "$stdout" measured_group_sample_commands)" || return 1

  if test "$trace_mode" = traced; then
    compgen -G "$RUN_ROOT/trace/$case_name.*" >/dev/null || return 1
  fi
}

for round in 1 2 3; do
  validate_case "untraced-r$round" untraced || exit 1
  validate_case "traced-r$round" traced || exit 1
done
```

### 7.2 Trace 完整性與 WAL fd 限定

每輪從 summary 取得 canonical `wal_path`，只保留該目錄下檔名符合正整數加 `.wal` 的 regular-file fd：

```text
<wal_path>/<positive-engine-seq>.wal
```

解析時遵守以下規則：

1. 使用 `-yy` 的 fd annotation為主要依據。將各PID／TID檔依timestamp合併後，以`openat` result交叉
   檢查同一process共享的fd table；`close(fd) = 0`後立即移除。若無法確認thread共享關係或canonical
   path，該事件為ambiguous，不得假設每個TID有獨立fd table。
2. `write`／`fsync` return `<0`、errno缺失、fd沒有唯一WAL path，均保留在error表，但不得納入成功統計。
3. partial `write` 依實際return bytes計算；同一logical `FileOps::write_all`可能有多個syscall，不得假設
   一次寫完。
4. `unfinished ...` 與 `<... resumed>` 必須在同一 PID file 依 syscall順序一對一重組：entry timestamp取
   unfinished line，return與`<duration>`取resumed line。任何unpaired或ambiguous pair使該輪invalid。
5. `+++ exited with ... +++` 是 `strace` 的正常 PID termination marker，不是 syscall，也不計入
   malformed；真正的 trace 截斷、任一WAL `fsync`沒有return／duration、或同一fd在未close前被不明確
   重新指派，該輪invalid。
6. stdout、telemetry CSV、snapshot、cursor、directory fd與非WAL檔案全部排除。

將有效事件輸出為 `$RUN_ROOT/derived/<case>-wal-syscalls.csv`，固定schema：

```text
pid,syscall,path,start_epoch_ns,end_epoch_ns,duration_ns,return_value,errno
```

`strace -ttt/-T` 預設可能只有microsecond精度；轉為ns後仍須保留原始trace，且對齊容許值另加
`1,000 ns` trace rounding，不得把補零誤解為nanosecond量測精度。

裁切 measured window 前，先保留所有 WAL 事件；再以：

```text
window_start = start_realtime_epoch_ns - clock_tolerance_ns
window_end   = end_realtime_epoch_ns + clock_tolerance_ns
clock_tolerance_ns = start_uncertainty_ns
                   + end_uncertainty_ns
                   + 1,000 clock-resolution allowance
                   + 999 CSV microsecond truncation
                   + 1,000 strace rounding
```

選出與 measured window overlap 的事件。所有減法使用 checked／saturating arithmetic。裁切後不得直接要求
成功 WAL `fsync` 總數等於 `measured_sync_count`：WAL segment 輪轉、warmup 或 drain 可能在 measured
window 內產生不屬於任何 application sync row 的合法 `fsync`。先以一對一 overlap 將事件分成：

1. `data_sync`：唯一對應一筆 measured application sync row 的 WAL `fsync`；
2. `rotation_sync`：沒有 application row 對應，且由 `openat`／path sequence 證實為 segment 輪轉的 WAL
   `fsync`；
3. `ambiguous`：無法證實前兩者的事件。

`data_sync` count 必須等於 `measured_sync_count`，且每筆 measured row 必須恰好對應一筆；這是
application 與 syscall 對齊的有效性條件。`rotation_sync` 必須在報告中另外列出 count、duration 與
segment path，不得從總數中靜默刪除。任何無法證實為 rotation 的額外事件，或缺少 data sync，均使該輪
invalid／inconclusive（若只是 window 邊界的一筆，列為 `boundary event` 並保留原始 trace）。WAL
`write` 也使用相同的 data/rotation/ambiguous 分類；不能把 rotation write 當成 application data write。

### 7.3 Application interval 與 syscall matching

對 telemetry CSV 每一筆 measured sync row建立：

```text
app_end_epoch_ns   = start_realtime_epoch_ns + elapsed_us * 1,000
app_start_epoch_ns = app_end_epoch_ns - value * 1,000
```

所有乘加減必須檢查overflow／underflow。輸出
`$RUN_ROOT/derived/<case>-application-sync-intervals.csv`：

```text
sample_index,start_epoch_ns,end_epoch_ns,duration_ns,over_25ms,over_100ms,over_250ms
```

只對`>=25 ms` sample做逐筆tail matching。將application interval兩端各擴張
`clock_tolerance_ns`後，與成功的 WAL `fsync` interval 做 overlap；rotation／ambiguous 事件先依 7.2
分類，不得把未對應的 rotation `fsync` 當作 application sample 的 match：

- 零個match：記為`unmatched`；
- 一個match：計算overlap、`fsync_duration/app_duration`及
  `max(0, app_duration-fsync_duration)`；
- 多個match：記為`ambiguous`，不得任選一筆。

輸出 `$RUN_ROOT/derived/<case>-tail-attribution.csv`：

```text
sample_index,app_start_ns,app_end_ns,app_duration_ns,match_count,fsync_start_ns,fsync_end_ns,fsync_duration_ns,fsync_share_pct,unexplained_ns,iostat_window
```

`write`發生在`Wal::sync()`之前，不與sync sample強制逐筆配對。它在整個measured window按count、成功bytes、
total、p50、p99與max duration統計，再和`fsync` aggregate比較。

### 7.4 Device window correlation

使用 `wal-block-topology.txt` 確認 `iostat` row。ISO timestamp視為前一秒interval的結束時間，因此每個sample
window為`(timestamp - 1 second, timestamp]`。若所用sysstat版本語意不同，必須在報告明記並依該版本修正，
不可平移資料直到看似吻合。

對每個matched slow syscall列出所在window的：

- `r_await`、`w_await`；
- `aqu-sz`；
- `%util`；
- read/write throughput與IOPS。

同輪measured window內、未包含任何`>=25 ms` WAL syscall的iostat windows作normal baseline。若至少一半
slow-syscall windows的`w_await`或`aqu-sz`高於同輪normal-window p95，才可標記`device_correlated`；
`%util`與`r_await`只作輔助證據，不能單獨觸發判定。沒有上升只能撤回correlation，不能證明device沒有
參與。若沒有normal window或一秒window不足以唯一對應，標記`inconclusive`。

## 8. 預先固定的歸因規則

先完整列出所有`>=25 ms` sample，不得只挑最大值。分類順序如下：

1. artifact、clock、fd mapping或trace validation失敗：`inconclusive`。
2. 六輪都沒有`>=25 ms` application sync：`not_reproduced`；若tail只存在於traced或untraced其中一組，
   為`inconclusive`。
3. traced與untraced都重現tail後，單一slow sample若只有一個WAL fsync match，且fsync解釋至少90%的
   application duration，記為
   `fsync-explained`。
4. 若所有slow sample的application duration總和中，`fsync-explained`部分超過50%，主分類可為
   `fsync_syscall`；否則若syscall外差額超過50%，主分類為`application_outside_syscall`；介於兩者或有
   大量ambiguous sample時為`inconclusive`。
5. measured window中，若成功WAL write duration超過WAL write+fsync duration的50%，且write tail是主要
   syscall成本，可加註或改判`wal_write_syscall`。這不表示write與某筆sync sample重疊。
6. 符合第7.4節條件時，在syscall分類後加註`device_correlated`。
7. fsync明顯慢、但device correlation不成立或iostat解析度不足時，分類為
   `fsync_syscall + filesystem_or_kernel`；不得因此排除device。

90%／50%門檻在執行前固定，是為避免看完outlier後改變「一致」或「主要」的定義。報告仍須提供原始count、
duration與share，不能只給分類標籤。允許複合結論；若不同輪落在不同層，應列為混合結果或
`inconclusive`，不得只保留支持單一假說的輪次。

## 9. 執行後身份確認

在建立report前執行：

```bash
{
  git rev-parse HEAD
  git status --short
  git diff --cached --binary | sha256sum
  git diff --binary | sha256sum
  git ls-files --others --exclude-standard -z | xargs -0 -r sha256sum
  sha256sum "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-after.txt"

diff -u "$RUN_ROOT/logs/source-identity-before.txt" \
  "$RUN_ROOT/logs/source-identity-after.txt" \
  > "$RUN_ROOT/logs/source-identity.diff"
test ! -s "$RUN_ROOT/logs/source-identity.diff"
```

不一致時不得合併統計；列出變更時間與受影響輪次，結果為`inconclusive`。report在此檢查完成後才建立，
因此新增report本身不污染before／after identity。

## 10. 報告格式與必要內容

### 10.1 摘要

報告開頭直接填寫：

| 項目 | 結果 | 判定 |
| --- | --- | --- |
| Release／Debug／ASan-UBSan | `<passed/total>` | pass/fail |
| Targeted telemetry | `<passed/total>` | pass/fail |
| Run completeness | `<valid>/6` | pass/inconclusive |
| Clock／CSV consistency | `<valid>/6` | pass/inconclusive |
| Trace／WAL fd validity | `<valid>/3` | pass/inconclusive |
| Tail reproduced | `<slow samples>/<all sync samples>` | yes/no |
| Primary attribution | `<封閉分類>` | 證據摘要 |
| Device correlation | `<supported/not supported/inconclusive>` | 證據摘要 |

主分類只能使用：

```text
fsync_syscall
wal_write_syscall
device_correlated
filesystem_or_kernel
application_outside_syscall
not_reproduced
inconclusive
```

### 10.2 身份、環境與固定參數

記錄日期、host/kernel、CPU topology、兩組affinity、governor／boost、clocksource與NTP狀態、filesystem、
mount options、WAL source與leaf device、可用空間、compiler／flags、HEAD、index/worktree/untracked hashes、
binary hash、工具版本、`RUN_ROOT`及before／after identity結果。

### 10.3 六輪完整性與application結果

| Case | Mode | Status | Elapsed ms | RPS | Sync count | Sync p99/max us | >25/>100/>250 ms | Clock tolerance ns | Artifact validity |
| --- | --- | --- | ---: | ---: | ---: | --- | --- | ---: | --- |
| untraced-r1 | untraced | | | | | | | | |
| traced-r1 | traced | | | | | | | | |
| traced-r2 | traced | | | | | | | | |
| untraced-r2 | untraced | | | | | | | | |
| untraced-r3 | untraced | | | | | | | | |
| traced-r3 | traced | | | | | | | | |

traced與untraced RPS不得計算共同median或互相宣稱regression；它們只用來證明workload與tail是否出現。

### 10.4 Traced syscall aggregate

| Case | Measured groups | Data fsyncs | Rotation fsyncs | WAL fsync count | fsync total/p50/p99/max | WAL write calls/bytes (data/rotation) | write total/p50/p99/max | Errors | Valid |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- | --- | --- |
| traced-r1 | | | | | | | |
| traced-r2 | | | | | | | |
| traced-r3 | | | | | | | |

另列每輪 data／rotation write 與 fsync 各占成功 WAL syscall duration 的百分比，以及 window 外
warmup／drain／recovery 事件數。總 WAL syscall count 不得直接用來代替 measured data group count。

### 10.5 Tail逐筆對齊

| Case/sample | App interval UTC | App duration ms | WAL fsync interval UTC | fsync duration ms | Match count | fsync share | Outside-syscall ms | Device window |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| | | | | | | | | |

表格必須包含全部`>=25 ms` sample，並在其後彙總：總slow sample數、matched/unmatched/ambiguous數、
application slow duration總和、matched fsync duration總和、fsync-explained share及outside-syscall share。

### 10.6 Device correlation

逐輪列normal-window p50/p95與slow windows的`r_await`、`w_await`、`aqu-sz`、`%util`、throughput／IOPS。
說明partition到leaf device mapping與一秒解析度限制。只允許使用`device_correlated`，不得寫成
`device_proven`。

### 10.7 結論、限制與下一步

結論依第8節規則逐步列出證據與反證，最後選擇一個主分類及必要的複合標籤。限制至少包含：

- strace observer effect與timestamp精度；
- iostat一秒聚合不能證明單筆block request因果；
- 單一host、filesystem、storage、instrument與shard；
- closed-loop workload，不代表production fixed-rate arrival；
- traced RPS不作capacity結論。

若分類成功，下一步另立一份最小optimization設計；若為`not_reproduced`或`inconclusive`，保留限制並停止
擴張production telemetry。本需求完成後不得在沒有新設計的情況下修改fsync、group policy或durability。
