# WAL parallel prepare production configuration 壓測操作與報告規格

## 1. 目的與判定範圍

本文件定義如何驗收 `docs/wal-parallel-prepare-production-configuration-design.md`。測試目標是
確認 public `EngineConfig.runtime` 的 WAL prepare 設定確實傳入 production Engine path，且 W=2
在單一 instrument、durable completion、per-group `fsync` 下相對 W=1 有足夠收益，不改變 WAL、
replay 或 completion correctness。

流程分為兩個 gate：

1. **Controlled benchmark**：以既有 `engine_durable_single_instrument` workload 比較 W=1／W=2，
   並驗證 threshold fallback 與 W=4 diagnostic。
2. **Deployment canary**：以固定輸入率及 production MetricsSink/exporter 觀察 queue、publisher lag、
   完整 sync histogram 與部署 CPU budget。

現有 benchmark 是 closed-loop workload，summary 只有 `wal_sync_full_run_p99_us`，沒有 sync
p99.9/max，也沒有 queue／publisher lag 時間序列。因此 controlled benchmark 通過只代表效能與
correctness gate 通過；未完成 canary 時，報告必須標示 **production acceptance incomplete**。

本流程不修改 WAL algorithm、group-commit policy、fsync policy、StateMachine、Publisher、
Completion worker 或自動調參。測試期間不得執行會改變 Git staging 的操作。

## 2. 驗收條件

### 2.1 Controlled benchmark

- Release、Debug、ASan/UBSan 全量測試通過。
- 四個 case 各五輪；所有正式輪 exit 0、measured time 至少 15 秒，並使用全新 data directory。
- W=1：`actual_parallel_prepare_groups == 0`。
- W=2 candidate：每輪 `actual_parallel_prepare_groups > 0`。
- W=2 fallback：每輪 `actual_parallel_prepare_groups == 0`。
- W=2 五輪 median throughput 相對 W=1 至少提升 10%。
- 五個配對 round 中，不得有三輪以上的 W=2 command p99 超過同 round W=1 的 110%。
- 所有成功 summary 必須通過 benchmark 內建 completion、WAL replay、durable head、empty-book 與
  counter validation；任何 validation error 都是失敗，不能只排除該輪。
- 保留所有慢輪及真實 tail。只有命令未啟動、data directory 已存在或環境／工具明確失敗的輪次
  可標成 setup failure，並以新 case name 重跑。

計算方式：

```text
W2 uplift (%) = (median(W2 commands_per_second)
                 / median(W1 commands_per_second) - 1) * 100
round p99 regression = W2 p99_us > W1 p99_us * 1.10
```

W=4 只提供 diagnostic evidence，不是 rollout 必要條件，不能取代 W=2 gate。

### 2.2 Deployment canary

- 固定輸入率下，queue depth 與 publisher lag events/bytes/age 不得跨觀測窗口持續增加。
- command p99 不得相對 W=1 baseline 持續退化超過 10%。
- 同時保存 `wal_sync_latency` 的 p99、p99.9、max。
- CPU 與 context-switch 增量不得超過部署前記錄的 per-Engine budget。
- 正常 shutdown 後改回 W=1，必須能從相同資料目錄重啟，不需 migration。

CPU／context-switch budget 若未在測試前定義，該 gate 必須標為 `not evaluated`，不能事後選一個
剛好通過的門檻。

## 3. 固定條件與矩陣

```text
OS                         Linux
build                      ReleaseBenchmark；sanitizer binary 不量效能
workload                   engine_durable_single_instrument
instrument / shard         1 / 1
completion boundary        durable callback
WAL sync                   per_group
engine group size          4,096 commands
engine group delay         1,000 us
engine producer lanes      8,192
parallel threshold         candidate 4,096；fallback 8,192
CPU affinity               同一組預先記錄的 CPUs；範例為 2-7
filesystem                 目標 Linux 實體 filesystem；不得為 tmpfs／overlay
rounds                     每 case 五輪
measured duration          每輪至少 15 秒；pilot 以 20 秒為目標
warmup                     每輪 10,000 iterations（20,000 commands）
data directory             每輪全新且執行前不存在
```

`iterations` 代表 crossing pair 數，summary 的 measured commands 是 `2 * iterations`。

| Case | Lanes | Threshold | 用途 | 必要 counter |
| --- | ---: | ---: | --- | --- |
| `w1-baseline` | 1 | 4,096 | production default | parallel groups = 0 |
| `w2-candidate` | 2 | 4,096 | rollout candidate | parallel groups > 0 |
| `w2-fallback` | 2 | 8,192 | threshold fallback | parallel groups = 0 |
| `w4-diagnostic` | 4 | 4,096 | CPU/tail diagnostic | parallel groups > 0 |

不得為 W=2 單獨提高 group size、delay 或 producer lanes。如果 candidate 沒有 parallel group，
結論是目前 group policy 無法觸發 threshold，而不是降低 threshold 後宣稱 production 設定有效。

同一矩陣固定 compiler、binary、CPU affinity、governor、boost、I/O scheduler、mount options 與背景
負載。不清 page cache、不刪慢輪、不重用 WAL directory。正式矩陣開始後若 source、index、
worktree、binary 或固定環境改變，既有輪次不得與新輪次合併。

## 4. 建置與 correctness gate

在 repository root 執行：

```bash
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset release-benchmark
cmake --build --preset release-benchmark
ctest --test-dir build/ReleaseBenchmark --output-on-failure

conan install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers

BENCH_BIN=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCH_BIN"
```

任一必要測試失敗即停止。TSan 只在 runtime 可啟動時執行；若遇到 runtime mapping error，記錄為
環境限制，不得宣稱 TSan 通過或程式失敗。

## 5. Artifact、身份與環境

先把 `RUN_PARENT` 設為目標 WAL 所在的實體 filesystem；以下只是範例：

```bash
set -o pipefail
RUN_PARENT=/mnt/local-nvme/order-books-benchmark
test -d "$RUN_PARENT"
findmnt -T "$RUN_PARENT"

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-prepare-production-XXXXXXXX")
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data" "$RUN_ROOT/time"

git rev-parse HEAD > "$RUN_ROOT/logs/source-identity.txt"
git status --short >> "$RUN_ROOT/logs/source-identity.txt"
git diff --cached --binary | sha256sum >> "$RUN_ROOT/logs/source-identity.txt"
git diff --binary | sha256sum >> "$RUN_ROOT/logs/source-identity.txt"
git ls-files --others --exclude-standard -z \
  | xargs -0 -r sha256sum >> "$RUN_ROOT/logs/source-identity.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/source-identity.txt"

uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor \
  >> "$RUN_ROOT/logs/environment.txt" 2>&1 || true
taskset -c 2-7 true
```

若 affinity 不是 `2-7`，必須在所有命令一致替換。建議預留至少 30 GiB。正式矩陣完成後、建立
結果報告前，重新確認 HEAD、index/worktree diff hash 與 binary hash 都沒有改變；報告在 final
identity check 後才建立，以免報告本身改變 worktree hash。

## 6. 通用執行函式

```bash
run_case() {
  case_name=$1
  shift
  case_data="$RUN_ROOT/data/$case_name"
  case_log="$RUN_ROOT/logs/$case_name.log"
  case_time="$RUN_ROOT/time/$case_name.time"
  case_status="$RUN_ROOT/logs/$case_name.status"

  if test -e "$case_data"; then
    echo "data directory already exists: $case_data" >&2
    return 2
  fi

  /usr/bin/time -v -o "$case_time" \
    taskset -c 2-7 "$BENCH_BIN" "$@" --data-dir="$case_data" \
    2>&1 | tee "$case_log"
  command_status=${PIPESTATUS[0]}
  printf '%s\n' "$command_status" > "$case_status"
  return "$command_status"
}
```

報告使用 `time` 的 CPU percent、voluntary/involuntary context switches、maximum RSS 與 wall time。
`perf stat`、`pidstat`、`iostat` 只能用於額外 diagnostic，不能混入五輪統計；權限不足時只記錄
限制，不修改 sysctl。

## 7. Smoke

```bash
run_case smoke-w1 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=4096 \
  --iterations=10000 --warmup=1000

run_case smoke-w2 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096 \
  --iterations=10000 --warmup=1000

run_case smoke-fallback \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=8192 \
  --iterations=10000 --warmup=1000

run_case smoke-w4 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=4 --wal-parallel-prepare-min-commands=4096 \
  --iterations=10000 --warmup=1000
```

四輪必須 exit 0 且各有一行成功 summary。W=1 與 fallback 的 parallel groups 必須為 0；W=2
candidate 與 W=4 必須大於 0。Smoke 不納入正式統計；candidate 未觸發時不進行正式矩陣。

## 8. Pilot 與正式 iterations

四個 case 各跑一次 pilot，初始使用 `--iterations=400000 --warmup=10000`，其餘參數沿用 smoke。
從 summary 讀取 `elapsed_ms`：

```text
candidate_iterations = ceil(400000 * 20000 / pilot_elapsed_ms)
FORMAL_ITERATIONS = max(candidate_iterations of all four cases)
```

所有 case 使用相同 `FORMAL_ITERATIONS`：

```bash
FORMAL_ITERATIONS=<依 pilot 計算的正整數>
test "$FORMAL_ITERATIONS" -gt 0
```

正式第一個完整 round 若任一 case 的 `elapsed_ms < 15000`，已完成正式輪全部作廢；提高共同
iterations 後從 r1 重跑。目標維持約 20 秒，不要逼近 benchmark 每 phase 的 60 秒 timeout。

## 9. 正式五輪矩陣

使用交錯順序：

```text
r1: W1, W2, fallback, W4
r2: W2, W4, W1, fallback
r3: fallback, W1, W4, W2
r4: W4, fallback, W2, W1
r5: W1, W4, W2, fallback
```

共同模板：

```bash
run_case formal-w1-r1 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=4096 \
  --iterations="$FORMAL_ITERATIONS" --warmup=10000
```

其他 case 只替換 case name 與 options：

```text
W2 candidate:  --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
W2 fallback:   --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=8192
W4 diagnostic: --wal-prepare-workers=4 --wal-parallel-prepare-min-commands=4096
```

每輪立即檢查 status、summary 與 counter。完成後執行：

```bash
rg '^engine_durable_single_instrument ' "$RUN_ROOT"/logs/formal-*.log \
  > "$RUN_ROOT/logs/formal-summaries.txt"
test "$(wc -l < "$RUN_ROOT/logs/formal-summaries.txt")" -eq 20

for status_file in "$RUN_ROOT"/logs/formal-*.status; do
  test "$(tr -d '[:space:]' < "$status_file")" = 0 || exit 1
done

if rg -n 'error_code=' "$RUN_ROOT"/logs/formal-*.log; then
  echo 'benchmark error found' >&2
  exit 1
fi
```

成功 summary 表示 benchmark 已完成 stop、WAL reopen/replay、record count 與 durable head
validation；不得只看 process exit 0 而忽略 summary 數量。

## 10. Controlled benchmark 必須收集的資料

每一輪至少擷取：

- identity：case、round、lanes、threshold、group size/delay、producer lanes、iterations、warmup；
- throughput：`commands_per_second`、`trades_per_second`、`elapsed_ms`；
- command latency：`p50_us`、`p99_us`、`p99.9_us`、`max_us`；
- path：`wal_group_commits`、`wal_group_commands`、`actual_parallel_prepare_groups`、
  `actual_prepare_tasks`、`actual_commands_per_group`；
- storage：`wal_sync_full_run_p99_us`、`wal_mib_per_second`、`wal_bytes_delta`、
  `measured_segment_rotations`、`wal_bytes`；
- correctness context：`active_orders`、`active_levels`、`fsync_mode`、`completion_boundary`；
- resources：CPU percent、voluntary/involuntary context switches、maximum RSS、wall time。

`wal_sync_full_run_p99_us` 包含 warmup，不能標成 measured-only sync p99。四個 case 使用相同
warmup，所以可作 storage-tail context，但不能取代 exporter 的 measured-window p99/p99.9/max。

五輪至少計算 median、min、max。uplift 使用 median，p99 gate 使用同 round 配對值；不得使用最佳
單輪或刪除慢輪後的平均值。

## 11. Deployment canary

本節需要 production-like fixed-rate load generator 與 MetricsSink/exporter；repository 現有
closed-loop benchmark 不能替代。若環境未提供，跳過本節並把最終狀態標為
`production acceptance incomplete`。

測試前凍結：

```text
CPU affinity / quota                    <填寫>
deployment peak offered rate            <測試前填寫 commands/s>
允許的 per-Engine CPU 增量               <填寫>
允許的 voluntary context-switch 增量     <填寫>
允許的 involuntary context-switch 增量   <填寫>
sample interval                         1 second
warmup / measured window                5 / 15 minutes
```

固定輸入率不得高於 controlled benchmark 五輪 W=1 最低 throughput 的 80%，並以測試前記錄的
production peak rate 為上限：

```text
fixed_rate = min(deployment_peak_rate,
                 floor(min(W1 commands_per_second) * 0.80))
```

W=1 與 W=2 使用相同 command mix、rate、storage 類型、affinity、group policy 與 exporter interval；
效能比較使用獨立的新 data directory。每秒保存：

- commands、queue depth、queue/end-to-end latency；
- WAL group commits/commands、parallel groups/tasks；
- WAL commit latency與 sync latency p50/p99/p99.9/max；
- publisher lag events/bytes/age、retry/error/pressure counters；
- process CPU、thread count、voluntary/involuntary context switches、RSS。

Counter 比較窗口 delta；gauge 與 lag 比較 slope、max、窗口末值，不能只看最後 snapshot。如果 W=2
parallel groups 沒增加，只能判定 production arrival pattern 無法觸發 threshold；這代表此設定在該
流量下沒有收益，不得提高 canary rate 製造 backlog 來宣稱通過。

最後使用 W=2 data directory 正常 shutdown，改回 W=1 後從相同 directory 重啟並送入短
correctness workload，驗證 rollback 不需 migration；此輪不納入效能統計。

## 12. 報告格式

結果寫入 `docs/wal-parallel-prepare-production-configuration-benchmark-report.md`，包含以下章節。

### 12.1 摘要與判定

| Gate | 結果 | 判定 |
| --- | --- | --- |
| Release／Debug／ASan-UBSan | `<通過數>` | pass/fail |
| W2 median uplift | `<百分比>` | pass/fail，門檻 +10% |
| W2 paired p99 regression | `<退化輪數>/5` | pass/fail |
| W2 parallel path | `<每輪 groups 範圍>` | pass/fail |
| Fallback path | `<每輪 groups>` | pass/fail |
| Replay／durable head | `<成功輪數>/20` | pass/fail |
| Queue／publisher lag canary | `<結果或 not run>` | pass/fail/incomplete |
| Sync p99/p99.9/max | `<結果或 not available>` | pass/fail/incomplete |
| CPU／context-switch budget | `<結果或 not defined>` | pass/fail/incomplete |
| Rollback restart | `<結果或 not run>` | pass/fail/incomplete |

最終結論只能是：

- `reject`：correctness 或 controlled benchmark 必要 gate 失敗；
- `benchmark pass, production acceptance incomplete`：controlled gate 通過但 canary 尚未完成；
- `W=2 canary accepted`：全部必要 gate 通過，可有限 rollout；
- `rollback to W=1`：canary 觸發 rollback。

不得宣稱達到 1M commands/s，也不得把 W=4 當 production default。

### 12.2 Artifact、環境與測試

列出日期、host/kernel、CPU topology、affinity、governor/boost、filesystem/mount、storage、
compiler/flags、HEAD、index/worktree hashes、binary hash、`RUN_ROOT`、可用空間、工具限制、各 build
與 test 通過數、sanitizer/TSan 狀態，以及四個 smoke 的 counters。

### 12.3 Pilot 與固定參數

| Case | Pilot iterations | Pilot elapsed ms | Calculated iterations |
| --- | ---: | ---: | ---: |
| W1 |  |  |  |
| W2 |  |  |  |
| W2 fallback |  |  |  |
| W4 |  |  |  |

明列共同 `FORMAL_ITERATIONS` 與最短正式 elapsed time。

### 12.4 Controlled aggregate

| Case | RPS median | RPS min--max | Uplift vs W1 | p50 median | p99 median | Worst p99.9 | Worst max | Sync full-run p99 range |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| W1 |  |  | baseline |  |  |  |  |  |
| W2 |  |  |  |  |  |  |  |  |
| W2 fallback |  |  |  |  |  |  |  |  |
| W4 |  |  | diagnostic |  |  |  |  |  |

另列五個 round 的 W1/W2 p99 配對值及是否超過 10%。Latency 單位為 us。

### 12.5 路徑、資源與 WAL

| Case | Parallel groups range | Tasks range | Commands/group range | CPU median | Vol CS median | Invol CS median | Max RSS |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| W1 |  |  |  |  |  |  |  |
| W2 |  |  |  |  |  |  |  |
| W2 fallback |  |  |  |  |  |  |  |
| W4 |  |  |  |  |  |  |  |

列出 WAL bytes delta、MiB/s、rotation 及 case 間是否一致；差異必須解釋。

### 12.6 Canary、rollback、逐輪明細與限制

報告 W1/W2 fixed rate、queue/publisher lag slope/max/end、sync tail、CPU/context switch、parallel
group delta 與 rollback。未執行逐項標成 `not run`，不得從 controlled benchmark 推測。

逐輪保留 case、round、elapsed、RPS、p50/p99/p99.9/max、sync full-run p99、groups/tasks、WAL
bytes、CPU、context switches、exit status、是否納入及理由；所有 outlier 都保留並討論。

限制至少說明 closed-loop workload、sync histogram 包含 warmup、以及現有 benchmark 無
time-series。最後依 gate 給出 W=1、W=2 canary、reject 或 rollback 結論。

## 13. 停止與 rollback

下列任一情況立即停止 canary，以正常 shutdown 關閉 Engine，改回 W=1 並從相同資料目錄重啟：

- correctness、replay、durable head、shutdown 或 sanitizer failure；
- W=2 多數窗口 throughput 未達 baseline，或 command p99 持續退化超過 10%；
- queue depth 或 publisher lag 持續成長；
- sync/storage tail 超出部署允許範圍；
- CPU、thread 或 context-switch budget 超標；
- exporter／監測缺失導致無法判斷上述條件。

Rollback 不刪除或轉換 WAL/Snapshot。W=1 若無法從相同資料重啟，屬 correctness failure，不能用
清空 data directory 規避。
