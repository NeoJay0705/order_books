# Engine writer 同步路徑 ceiling／根因分析壓測操作與報告規格

## 1. 是否需要重測

需要。`docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-design-review.md` 新增並改變了可用於
根因判定的量測契約：

- 目前 source 已包含 bounded parallel WAL prepare，舊 writer phase 數據不能代表目前實作；
- `StateMachine::apply` 新增 opt-in exclusive child profile，舊報告沒有這些資料；
- 每個同步 phase 現在必須使用一致公式回報 derived ceiling 與 1M／1.2M headroom；
- actual commands/group 已改為 measured-only counter delta；
- 零耗時 phase 現在以 `ceiling_observed=false` 表示，不能沿用舊的零 ceiling；
- Publisher lag 與 Completion queue 必須作為當輪有效性護欄。

因此，舊報告只能當歷史背景，不能與本輪結果合併計算 median、range、bias 或 phase share。本輪只量
Engine writer 同步路徑；不優化 Publisher worker、Completion worker，不修改 production defaults。

本次正式報告另存為：

```text
docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-benchmark-report.md
```

## 2. 測試問題與停止規則

本輪必須回答：

1. 單 shard、單 instrument、per-group `fsync` 的 authoritative Engine ceiling；
2. group=4,096／8,192、W=1／2 時，各 writer exclusive phase 的 ns/command、share 與 derived ceiling；
3. `StateMachine::apply` 六個 child 中的主要成本；
4. 第一個低於 1M commands/s，或低於 1.2M commands/s 診斷餘裕的同步 phase；
5. Engine 與 direct WAL ceiling 的差距是否可由 writer 同步 phase 解釋。

遇到以下任一條件，該輪不得納入正式 aggregate：

- correctness、WAL replay、EngineSeq continuity 或 completion exactly-once 失敗；
- profile child 大於 parent、counter overflow/regression 或 sample denominator 不一致；
- timeout、publisher failure、storage pressure、Completion capacity wait 或資源耗盡；
- source、index、worktree、binary 或固定環境在矩陣期間改變；
- actual commands/group 小於 configured group 的 90%。最後一項標為 `supply-limited`；保留 raw data，
  但不得用該輪宣稱 Engine frontier。

低吞吐、慢 `fsync` 或較高 context switches 本身不是排除理由。有效慢輪必須保留。

## 3. 固定矩陣

### 3.1 Writer 正式矩陣

```text
group size             4,096、8,192
WAL prepare workers    1、2
parallel threshold     4,096
producer lanes         8,192，所有 case 固定
group delay            1,000 us
writer profile         off、on
apply subprofile       profile-on 正式 case 固定 on
rounds                  每 case 5 輪
measured duration       每輪至少 20 秒
instrument / shard      1 / 1
durability              per-group fsync
snapshot                benchmark 已停用 measured-period snapshot
```

共 `2 groups × 2 W × 2 profile modes × 5 rounds = 40` 個 writer formal runs。W=4、group=16,384
不在本需求內。

### 3.2 必要 controls

- `engine_durable_single_instrument`：四個 `(group, W)` case 各五輪，作 authoritative end-to-end control；
- `wal_write_ceiling`：四個 `(group, W)` case 各五輪，`sync=per_group`、profile off，作 direct WAL control；
- `engine_pipeline_ceiling/state_machine`：相同 alternating crossing mix，五輪；
- `engine_pipeline_ceiling/runtime_handoff`：五輪，只驗證 handoff headroom；
- `engine_pipeline_ceiling/metrics`：只有本輪 writer `post_apply` share 重新超過 10% 時才執行五輪。

Publisher drain 與 Completion worker service optimization 不屬於本輪 controls。Writer profile 中已有的
Publisher lag、Completion queue depth／residence 只用來判斷結果是否受 downstream 壓力污染。

## 4. Build、correctness 與 staging 護欄

在 repository root 執行。以下流程禁止 `git add`、`git reset`、`git restore --staged`、commit 或其他
會改變 index 的操作。`git status`、`git diff`、`git rev-parse` 只讀，不會改變 staging。

優先使用專案目前的工具 wrapper：

```bash
set -o pipefail
TOOLS=/tmp/order_books-tools/bin
CONAN="$TOOLS/conan"
CMAKE="$TOOLS/cmake"
CTEST="$TOOLS/ctest"

test -x "$CONAN" && test -x "$CMAKE" && test -x "$CTEST"

"$CONAN" install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset release-benchmark
"$CMAKE" --build --preset release-benchmark
"$CTEST" --test-dir build/ReleaseBenchmark --output-on-failure

"$CONAN" install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset debug
"$CMAKE" --build --preset debug
"$CTEST" --preset debug --output-on-failure

"$CMAKE" --preset sanitizers
"$CMAKE" --build --preset sanitizers
"$CTEST" --preset sanitizers --output-on-failure

BENCH_BIN=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCH_BIN"
git diff --check
git diff --cached --check
```

任一 build、GoogleTest、CTest、ASan 或 UBSan gate 失敗即停止。Sanitizer binary 只作 correctness，不能
拿來量效能。

## 5. 環境與 artifact identity

`RUN_PARENT` 必須位於實體 filesystem，不可使用 tmpfs 或 overlay。正式矩陣期間不得切換 CPU
governor、boost、I/O scheduler、mount options 或 block device，也不要同時執行大量 CPU／I/O 工作。

```bash
BENCH_CPU_SET=2-7
CASE_TIMEOUT_SECONDS=120
RUN_PARENT=/home/neojhou

for tool in /usr/bin/time timeout taskset findmnt lsblk sha256sum; do
  command -v "$tool" >/dev/null || exit 1
done
taskset -c "$BENCH_CPU_SET" true

RUN_ROOT=$(mktemp -d "$RUN_PARENT/engine-writer-sync-path-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,logs,time,derived}

{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  sha256sum \
    docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-design-review.md \
    docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-benchmark-procedure.md \
    "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-before.txt"

{
  date --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>&1 || true
} > "$RUN_ROOT/logs/environment.txt" 2>&1

findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS -T "$RUN_ROOT" \
  > "$RUN_ROOT/logs/filesystem.txt"
```

報告必須記錄 `RUN_ROOT`。開始前建議至少保留 30 GiB 空間。每輪使用新的空 data directory；不得清
page cache、重用舊目錄或自動刪除 raw logs／WAL data。矩陣執行期間的筆記先寫入
`$RUN_ROOT/derived`；在第15節完成before/after identity比對前，不要建立或修改repository內的正式
report，避免報告檔本身改變worktree identity。

## 6. 通用執行函式

此函式保留 stdout、stderr、exit status 與 `/usr/bin/time -v`。不要把正式命令接到 `tee` 後，避免
pipeline 掩蓋 benchmark exit status。

```bash
run_case() {
  phase=$1
  case_name=$2
  iterations=$3
  warmup=$4
  shift 4

  case_data="$RUN_ROOT/data/$phase-$case_name"
  case_stdout="$RUN_ROOT/logs/$phase-$case_name.stdout"
  case_stderr="$RUN_ROOT/logs/$phase-$case_name.stderr"
  case_time="$RUN_ROOT/time/$phase-$case_name.time"
  case_status="$RUN_ROOT/logs/$phase-$case_name.status"
  case_meta="$RUN_ROOT/logs/$phase-$case_name.meta"

  test ! -e "$case_data" || return 2
  test ! -e "$case_stdout" && test ! -e "$case_status" || return 2
  printf 'iterations=%s warmup=%s args=' "$iterations" "$warmup" > "$case_meta"
  printf '%q ' "$@" >> "$case_meta"
  printf '\n' >> "$case_meta"

  /usr/bin/time -v -o "$case_time" \
    timeout --signal=TERM --kill-after=10s "${CASE_TIMEOUT_SECONDS}s" \
    taskset -c "$BENCH_CPU_SET" "$BENCH_BIN" \
      --iterations="$iterations" --warmup="$warmup" \
      --data-dir="$case_data" "$@" \
      > "$case_stdout" 2> "$case_stderr"
  status=$?
  printf '%s\n' "$status" > "$case_status"
  return "$status"
}
```

每次呼叫後確認 status 為 0。需要觀看輸出時，在命令完成後使用 `less` 或 `tail` 讀取 log。

## 7. Smoke 與 CLI contract

先執行已建置的自動 smoke：

```bash
"$CTEST" --test-dir build/ReleaseBenchmark --output-on-failure \
  -R 'order_books_benchmark_writer_(phase_profile|apply_subprofile).*smoke|order_books_tests'
```

再執行一輪小型 profile-on smoke：

```bash
run_case smoke writer-apply-on 10000 1000 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-apply-subprofile=on \
  --writer-profile-sample-every=8 \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

Smoke 必須 exit 0，且輸出至少符合：

```bash
SMOKE_LOG="$RUN_ROOT/logs/smoke-writer-apply-on.stdout"
rg -q 'phase=summary.*correctness_verified=true' "$SMOKE_LOG"
rg -q 'phase=profile.*writer_apply_subprofile=on' "$SMOKE_LOG"
for field in apply_precheck_ns apply_book_apply_ns apply_state_update_ns \
  apply_output_events_ns apply_producer_result_ns apply_incremental_validation_ns \
  apply_unattributed_ns apply_commands apply_events apply_trades; do
  rg -q "$field=" "$SMOKE_LOG" || exit 1
done
```

CTest 負責 valid、empty、unknown、wrong-workload 與 wrong-parent-profile CLI 組合。Smoke 不納入效能
統計。

## 8. Pilot：決定每個 case 的 command budget

先對四個 `(group, W)` 執行 profile-off pilot。建議從 `PILOT_ITERATIONS=200000` 開始；writer workload
每個 iteration 產生兩個 commands。

```bash
PILOT_ITERATIONS=200000
PILOT_WARMUP=10000

for group_size in 4096 8192; do
  for workers in 1 2; do
    run_case pilot "writer-g${group_size}-w${workers}" \
      "$PILOT_ITERATIONS" "$PILOT_WARMUP" \
      --workload=engine_writer_hot_path_profile \
      --writer-phase-profile=off \
      --engine-group-size="$group_size" \
      --engine-group-delay-us=1000 \
      --engine-producer-lanes=8192 \
      --wal-prepare-workers="$workers" \
      --wal-parallel-prepare-min-commands=4096 || exit 1
  done
done
```

從各輪 `elapsed_ms` 計算：

```text
formal_iterations = ceil(pilot_iterations * 25,000 / pilot_elapsed_ms)
```

向上取整到 10,000 的倍數，以約 25 秒作目標，並為每個 `(group, W)` 固定一個
`FORMAL_ITERATIONS_g*_w*`。預估時間若超過 55 秒，先以較小值再跑一次 pilot；不得讓 benchmark
內部 60 秒 phase timeout 污染正式輪。任何正式輪 measured elapsed 小於 20 秒時，該 `(group, W)`
的 off/on 五輪全部以較大且相同的 iterations 重跑，不能只補慢／快的一側。

## 9. Calibration

### 9.1 Writer profile sampling interval

固定 group=4,096、W=1。候選 `N=8、16、32` 依序測試，每個 N 使用三組交錯 pair：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3
```

其中 `off` 為 `--writer-phase-profile=off`；`on` 為 `--writer-phase-profile=on
--writer-apply-subprofile=off --writer-profile-sample-every=N`。兩邊使用相同 iterations、warmup、group、W
與 producer lanes。命令模板：

```bash
CAL_ITERATIONS="$FORMAL_ITERATIONS_g4096_w1"
CAL_WARMUP=10000

run_case calibration sample-n8-off-r1 "$CAL_ITERATIONS" "$CAL_WARMUP" \
  --workload=engine_writer_hot_path_profile --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096

run_case calibration sample-n8-on-r1 "$CAL_ITERATIONS" "$CAL_WARMUP" \
  --workload=engine_writer_hot_path_profile --writer-phase-profile=on \
  --writer-apply-subprofile=off --writer-profile-sample-every=8 \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

完成同 N 的六輪後，以三輪 `commands_per_second` median 計算：

```text
writer_profile_bias = abs(on_median - off_median) / off_median * 100%
```

- `<=5%`：選擇該 N，停止掃描更大 N；
- `>5%`：完整重做 N=16，再視需要做 N=32；每個 N 都要取得新的三輪 off；
- N=32 仍 `>10%`：停止，不執行 formal matrix；
- N=32 介於 5% 與 10%：可繼續取得 directional data，但不能把 phase share 當主要根因證據。

將選定值記為 `SAMPLE_EVERY`，formal matrix 期間不得按結果更換。

### 9.2 Apply 子量測局部 bias

使用已選定的 `SAMPLE_EVERY`，固定 group=4,096、W=1，以相同交錯順序執行三組：

```text
subprofile-off-r1, subprofile-on-r1, subprofile-on-r2,
subprofile-off-r2, subprofile-off-r3, subprofile-on-r3
```

兩邊都使用 `--writer-phase-profile=on`；唯一差異是
`--writer-apply-subprofile=off|on`。從 profile line 的 `apply_ns_per_command` 取三輪 median：

```text
apply_subprofile_bias =
  abs(apply_parent_ns_per_command(on) - apply_parent_ns_per_command(off))
  / apply_parent_ns_per_command(off) * 100%
```

- `<=5%`：apply children 可作主要歸因；
- `>5%` 且 `<=10%`：apply children 只能作 directional evidence；
- `>10%`：不能用 apply children 選 optimization，停止 apply-child 根因判定並記為
  `invalid_attribution`。Top-level formal matrix仍可在 writer profile bias 合格時執行。

Calibration 不納入 formal throughput aggregate。

## 10. Writer formal matrix

每個 `(group, W)` 使用 pilot 選定的 iterations，依下列交錯順序執行：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3, on-r4, off-r4, off-r5, on-r5
```

Profile-off 模板：

```bash
run_case formal "writer-g4096-w1-off-r1" \
  "$FORMAL_ITERATIONS_g4096_w1" 10000 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

Profile-on 模板：

```bash
run_case formal "writer-g4096-w1-on-r1" \
  "$FORMAL_ITERATIONS_g4096_w1" 10000 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-apply-subprofile=on \
  --writer-profile-sample-every="$SAMPLE_EVERY" \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

其餘 case 只替換 group、workers、對應 iterations 與唯一 case name。不得改變 delay、producer lanes、
threshold 或 warmup。每個 profile-on 輪次必須同時符合：

- `profiled_commands == completion_profiled_commands`；
- `profiled_accepted_commands == apply_commands`；
- `profiled_groups >= 50`，否則增加整個case的command budget後重跑off/on五輪；
- writer remainder 與 apply remainder 均不超過 parent 的 10%；
- `writer_service_ceiling_observed=true`；
- `actual_commands_per_group >= group_size * 0.9`；
- W=2時`actual_parallel_prepare_groups > 0`且`actual_prepare_tasks > 0`；
- summary 有三個 Publisher lag 欄位與 `correctness_verified=true`。

每個 `(group, W)` 以五輪 off/on median 計算 writer profile bias。`<=5%` 可作主要 attribution；5%～10%
降級為 directional；`>10%` 時該 case attribution 無效，必須提高固定 sampling interval後把整個 formal
matrix 重跑，不能只重跑該 case 或挑低 bias 輪次。

## 11. Controls

### 11.1 Authoritative Engine

四個 `(group, W)` 各五輪。使用與對應 writer case 相同的 iterations、warmup、delay、lanes 與 threshold：

```bash
run_case control "engine-g4096-w1-r1" \
  "$FORMAL_ITERATIONS_g4096_w1" 10000 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

每輪要求 `correctness_verified=true`、完成數與 durable/replay counts 一致，並記錄 completion latency。

### 11.2 Direct WAL

Direct WAL 的 `iterations` 是 WAL groups，不是 commands。先 pilot 每個 `(group, W)`，再選擇能維持
20～55 秒的 group count。四個 case 各五輪：

```bash
run_case control "wal-g4096-w1-r1" "$WAL_GROUPS_g4096_w1" 100 \
  --workload=wal_write_ceiling \
  --wal-phase-profile=off \
  --wal-sync=per_group \
  --wal-group-size=4096 \
  --wal-prepare-workers=1 \
  --wal-parallel-prepare-min-commands=4096
```

每輪必須 `replay_verified=true`。Direct WAL latency 是 group latency，不可與 Engine command completion
latency放在同一欄比較。

### 11.3 Isolated StateMachine

使用 alternating crossing pair，batch=4,096、active-orders=0。先 pilot iterations，再執行五輪且每輪
至少20秒：

```bash
run_case control "state-machine-r1" "$STATE_MACHINE_GROUPS" 1000 \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=state_machine \
  --pipeline-batch-size=4096 \
  --pipeline-active-orders=0
```

記錄 `command_equivalent_per_second`、commands、trades、events 與 `correctness_verified=true`。這是 isolated
ceiling，不是 Engine throughput。

### 11.4 Runtime handoff

五輪、每輪至少20秒：

```bash
run_case control "runtime-handoff-r1" "$HANDOFF_GROUPS" 1000 \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=runtime_handoff \
  --pipeline-batch-size=256 \
  --pipeline-producer-lanes=8192 \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000
```

只用來證明 enqueue／callback handoff 有足夠 headroom；不得把它解讀成 Completion worker 的完整服務
能力。

### 11.5 Conditional metrics control

只有正式 writer 結果的 `post_apply_share_percent` median 超過 10% 才執行五輪：

```bash
run_case control "metrics-r1" "$METRICS_GROUPS" 1000 \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=metrics \
  --pipeline-batch-size=4096
```

否則報告填 `not run: post_apply share <=10%`，避免超出需求。

## 12. 每輪有效性與資料擷取

每輪至少擷取：

```text
identity: case、group、W、threshold、profile mode、sample every、iterations、warmup
summary: commands/s、elapsed、p50/p99/p99.9/max、actual commands/group
writer: writer service/cycle、group collect/wait/active collect、每個 exclusive phase
WAL: prepare/plan-copy/publish/write/sync 與 nested children、rotation、write calls
apply: parent、六個 children、children sum、unattributed、commands/events/trades
validity: correctness/replay、Publisher lag、Completion depth/residence、profile counts
system: user/system time、CPU percent、RSS、voluntary/involuntary context switches
```

若某 phase 輸出 `ceiling_observed=false`，報告保留 raw total 與原因，但不得填入 0 ceiling 或套用
1M／1.2M 判定。

正式統計一律先逐輪計算，再對五輪取 median 與 min–max：

```text
phase_ns_per_command = phase_total_ns / sampled_accepted_commands
derived_phase_ceiling = 1e9 / phase_ns_per_command
target_utilization = 1,000,000 / derived_phase_ceiling
headroom_percent = (derived_phase_ceiling / 1,000,000 - 1) * 100
phase_share = phase_total_ns / writer_service_ns * 100
active_collect_ns = group_collect_ns - group_wait_ns
syncs_per_second = measured_wal_group_commits / measured_seconds
commands_per_sync = measured_wal_group_commands / measured_wal_group_commits
```

Apply child 另算 apply-parent share 與 writer-service share；WAL child 使用 WAL parent share。Parent 與
children不可重複加總，queue/group wait 不得換算成 CPU ceiling。不得跨輪先合計 total 再算 share，
也不得只選最快輪。

## 13. Root-cause 判定

| 條件 | 報告判定 |
| --- | --- |
| derived／isolated ceiling <1M/s，且跨五輪穩定 | confirmed synchronous ceiling candidate |
| ceiling 介於1M與1.2M/s | insufficient headroom |
| writer share >=10%，且W1/W2或isolated control同方向 | supported root cause |
| share <5%且derived ceiling >=2M/s | stop，不再細分 |
| 高phase time只與慢storage tail同輪出現 | tail contributor |
| writer profile bias >5% | attribution降級為directional |
| writer bias >10%或remainder >10% | invalid attribution |

單一高phase百分比不足以宣稱confirmed root cause；至少還需要 direct WAL、isolated StateMachine、W1/W2
差異或同輪storage/syscall evidence之一。最後只能選一個下一階段同步路徑 optimization；證據不足時寫
`inconclusive`，不得用本輪結果改動 downstream workers。

## 14. 正式報告格式

### 14.1 摘要與結論

先直接回答：

- authoritative Engine ceiling 是否達1M/s；
- 第一個缺少1.2M/s headroom的同步phase；
- apply child attribution是否通過local bias gate；
- Engine/direct-WAL gap是否由同步phase加總解釋；
- 唯一下一個optimization，或`inconclusive`。

### 14.2 Identity、環境與 gates

必填日期、host、kernel、CPU、physical/logical cores、compiler、flags、HEAD、cached diff hash、worktree diff
hash、binary SHA-256、CPU affinity、governor、clocksource、filesystem、device/topology、mount options、可用
空間、`RUN_ROOT`及工具限制。

| Gate | 結果 | 證據 |
| --- | --- | --- |
| Release build／CTest | | |
| Debug build／CTest | | |
| ASan／UBSan | | |
| CLI／smoke | | |
| identity before/after | | |

### 14.3 Calibration

| Calibration | Setting | Off median | On median | Bias | Evidence level |
| --- | ---: | ---: | ---: | ---: | --- |
| writer profile | N=8/16/32 | RPS | RPS | | primary/directional/invalid |
| apply subprofile | selected N | apply ns/cmd | apply ns/cmd | | primary/directional/invalid |

逐輪附 run name、RPS、elapsed、sampled groups與`apply_ns_per_command`。

### 14.4 End-to-end 與 isolated ceilings

| Type | Workload | Group | W | Median commands/s | Min–max | 1M attainment | Evidence |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| authoritative | Engine durable | | | | | | |
| diagnostic | Writer profile off | | | | | | |
| isolated | Direct WAL | | | | | | |
| isolated | StateMachine | 4096 | — | | | | |
| isolated | Runtime handoff | 256 | — | | | | |

Direct WAL 額外列 MiB/s、commands/sync、syncs/s 與 group tail latency。

### 14.5 Writer top-level phases

每個 `(group, W)` 各一表：

| Phase | ns/command median | Min–max | Writer share | Derived ceiling | Headroom | 判定 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| active group collect | | | | | | |
| admission | | | | | | |
| WAL append | | | | | | |
| WAL sync | | | | | | |
| StateMachine apply | | | | | | |
| publisher notify call | | | | | | |
| post-apply | | | | | | |
| completion enqueue | | | | | | |
| writer unattributed | | | | | | |

`group_wait`獨立列出但不換算CPU ceiling。

### 14.6 WAL 與 apply nested phases

| Parent | Child | ns/command median | Parent share | Writer share | Derived ceiling | Evidence |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| WAL append | prepare/serialize | | | | | |
| WAL append | plan/copy | | | | | |
| WAL append | publish/write/rotation | | | | | |
| apply | precheck | | | | | |
| apply | book apply/matching | | | | | |
| apply | state update | | | | | |
| apply | output/events | | | | | |
| apply | producer result | | | | | |
| apply | incremental validation | | | | | |
| apply | unattributed | | | | | |

零耗時phase填`not observed`，不可填0 commands/s。

### 14.7 Latency、group occupancy 與有效性

| Case | p50 | p99 | Worst p99.9 | Worst max | Actual cmd/group | CPU | Context switches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| | | | | | | | |

| Case | Publisher lag events/bytes/age | Completion max depth | Queue p99/p99.9 | Resource-limited? |
| --- | --- | ---: | --- | --- |
| | | | | |

Publisher lag非零不自動判失敗；必須結合warning/critical、storage pressure、timeout及是否能drain判定。

### 14.8 逐輪有效性與 root-cause evidence

所有 calibration、formal 與 control runs逐一列出：run name、exit status、elapsed、correctness、group
occupancy、profile counts、是否納入與原因。

| Candidate | Phase evidence | Independent evidence | Classification | 理由 |
| --- | --- | --- | --- | --- |
| | | | confirmed/supported/unconfirmed | |

結論分成「已證實」、「方向性」、「尚未證實」。不得把derived、isolated與end-to-end ceiling混成同一
數字。

### 14.9 限制與 artifacts

記錄 command mix、單instrument／單shard限制、local storage限制、profile bias、未執行的conditional
controls、raw logs與data的絕對路徑。歷史報告只可另表比較median，不得混入本輪樣本。

## 15. 收尾 identity 與完成清單

正式矩陣結束後、建立repository內的正式report之前，再次執行：

```bash
{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  sha256sum \
    docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-design-review.md \
    docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-benchmark-procedure.md \
    "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-after.txt"

diff -u "$RUN_ROOT/logs/source-identity-before.txt" \
  "$RUN_ROOT/logs/source-identity-after.txt"
```

`diff`必須沒有輸出且exit 0。通過後才依第14節從`$RUN_ROOT`資料建立正式report；report本身是測試後
產物，不再拿來反向改寫本輪identity。

只有 identity 完全一致且以下項目完成，才能發布正式結論：

- [ ] Release、Debug、ASan／UBSan與smoke通過；
- [ ] writer sampling與apply local bias均已分級；
- [ ] 40個writer formal runs完成且每輪至少20秒；
- [ ] actual commands/group達90%，或已誠實標示`supply-limited`；
- [ ] authoritative Engine、direct WAL、StateMachine與handoff controls完成；
- [ ] 每個同步phase都有raw time、derived ceiling、share與證據等級；
- [ ] writer/apply remainder不超過10%；
- [ ] Publisher／Completion有效性護欄已記錄；
- [ ] 報告只選一個同步路徑下一案，或明確寫`inconclusive`；
- [ ] 未修改Publisher／Completion worker、durability、WAL format或production defaults；
- [ ] 未執行任何會改變staging的操作。
