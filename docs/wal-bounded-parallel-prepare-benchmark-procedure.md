# WAL bounded parallel prepare 壓測操作與報告規格

## 1. 目的與判定問題

本文件定義如何驗收 `docs/wal-bounded-parallel-prepare-design.md`。測試只回答：在不改變
WAL bytes、ordering、durability 與 recovery 語意下，W=2／W=4 是否比 W=1 提高
Engine durable writer 吞吐，且 CPU 與 latency 代價可接受。

主要通過條件是 group=4,096、profile-off writer 的 W=4 五輪 median RPS 相對 W=1
至少提升 10%。Direct WAL、profile-on 與 group=256 都是歸因或 crossover 證據，不可取代
主要 gate，也不可用單一最快輪判定通過。

本流程不修改 production 設定、WAL format、fsync policy 或 Git staging。開始正式矩陣後，
source、index、worktree、binary 或固定環境任一項改變，已完成輪次即不可與新輪次合併。

## 2. 固定條件

正式輪次固定使用：

```text
OS                    Linux
build                 Release，sanitizer binary 不量效能
CPU affinity          taskset -c 2-7，共六個 logical CPUs
filesystem            實體 filesystem，不使用 tmpfs／overlay
writer group delay    1,000 us
writer producer lanes 8,192
WAL sync              per_group
parallel threshold    256 commands
rounds                每個正式 case 五輪
measured duration     每輪至少 15 秒，pilot 以 20 秒為目標留裕度
data directory        每輪全新且測試前不存在
```

同一輪測試期間不得更改 CPU governor、boost、I/O scheduler、mount options、compiler 或
build flags，也不得同時執行其他高 CPU／高 I/O 工作。不清 page cache，不刪除有效的慢輪，
不重用 WAL directory。建議在測試 filesystem 預留至少 60 GiB；空間不足時先縮小保留的
非正式 pilot data，不得刪除已納入報告的 raw logs。

## 3. 建置與 correctness gate

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

Release、Debug、ASan／UBSan 任一失敗即停止，不執行效能矩陣。TSan 只在 runtime 可正常啟動
時執行；若環境仍回報 runtime mapping error，記錄為環境限制，不得寫成程式通過或失敗。

## 4. 建立 artifact 目錄與凍結身份

正式 data 放在目標實體 filesystem。下列操作只讀取 Git 狀態，不會改變 staging：

```bash
set -o pipefail
RUN_PARENT=/home/neojhou
RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-parallel-prepare-benchmark-XXXXXXXX")
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

將 `RUN_ROOT`、binary SHA-256、HEAD、index diff hash、worktree diff hash與環境資料立即記入
測試紀錄。正式矩陣完成後、建立或修改report之前重跑相同identity commands；任一值改變，
停止並重建完整矩陣。Report只能在final identity驗證完成後寫入，避免文件本身改變worktree hash。

### 4.1 通用執行函式

每輪同時保存 benchmark output、`/usr/bin/time -v` 與 exit status：

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

正式結果使用 `time` 的 `Percent of CPU this job got`、`Voluntary context switches`、
`Involuntary context switches` 與 `Maximum resident set size`。`perf stat`、`pidstat` 或
`iostat` 只能放在額外診斷輪；若權限不足，記錄限制，不修改 sysctl，也不把診斷輪混入五輪統計。

## 5. Smoke

先確認 W=1／2／4、profile-off counters 與 replay：

```bash
run_case smoke-writer-w1 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=256 \
  --iterations=10000 --warmup=1000

run_case smoke-writer-w2 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=256 \
  --iterations=10000 --warmup=1000

run_case smoke-writer-w4 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=4 --wal-parallel-prepare-min-commands=256 \
  --iterations=10000 --warmup=1000
```

三輪都必須 exit 0 且輸出 `correctness_verified=true`。W=1 必須
`actual_parallel_prepare_groups=0`；W=2／4 必須大於 0，`actual_prepare_tasks` 也必須大於 0。
Smoke 不納入正式效能統計。

## 6. Pilot 與正式 iterations

不得先假設既有報告的 iterations 在目前 artifact 仍可達 15 秒。分別對下列三種 case 做
W=1／2／4 profile-off pilot：

1. writer group=4,096，pilot `--iterations=400000 --warmup=10000`；
2. writer group=256，pilot `--iterations=400000 --warmup=10000`；
3. direct WAL group=4,096，pilot `--iterations=300 --warmup=100`。

Writer pilot 命令模板：

```bash
run_case pilot-writer-g4096-w1 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=256 \
  --iterations=400000 --warmup=10000
```

Direct WAL pilot 命令模板：

```bash
run_case pilot-wal-g4096-w1 \
  --workload=wal_write_ceiling \
  --wal-phase-profile=off --wal-sync=per_group --wal-group-size=4096 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=256 \
  --iterations=300 --warmup=100
```

其餘 pilot 只改 group 與 W。每種 workload/group 使用三個 W 中最大的下式結果，讓所有 W
使用相同 command/group count，並讓最快設定也有至少 15 秒：

```text
candidate_iterations = ceil(pilot_iterations × 20,000 / pilot_elapsed_ms)
formal_iterations = max(candidate_iterations_W1, W2, W4)
```

記為 `ITER_WRITER_G4096`、`ITER_WRITER_G256` 與 `ITER_WAL_G4096`。正式第一輪若仍少於
15 秒，該 workload/group 已完成的輪次全部作廢，以提高後的相同 iterations 從 r1 重跑；
不得只延長較快的 W 或只補一輪。

計算完成後明確設定三個值；不要讓空字串落入正式命令：

```bash
ITER_WRITER_G4096=<依pilot計算的正整數>
ITER_WRITER_G256=<依pilot計算的正整數>
ITER_WAL_G4096=<依pilot計算的正整數>
test "$ITER_WRITER_G4096" -gt 0
test "$ITER_WRITER_G256" -gt 0
test "$ITER_WAL_G4096" -gt 0
```

## 7. Writer profile sampling calibration

Profile-on 只用於解釋 phase。先以 group=4,096 對 W=1 與 W=4 校準
`--writer-profile-sample-every`。每個 W 依下列 ABBA 順序各跑三輪：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3
```

先測 N=8；命令沿用正式 writer group=4,096 參數與 `ITER_WRITER_G4096`。Profile-on 另外加：

```text
--writer-phase-profile=on --writer-profile-sample-every=8
```

Profile-off 使用 `--writer-phase-profile=off`，不可帶 sample option。對每個 W 分別計算：

```text
bias = abs(profile_on_median_rps - profile_off_median_rps)
       / profile_off_median_rps × 100%
```

W=1、W=4 都 `bias <= 5%` 才選 N=8。否則依序以 N=16、N=32 重跑兩個 W 的完整
ABBA calibration；每個新 N 都要重取 profile-off，不得沿用舊時段資料。N=32 仍有任一 W
超過 5%，停止正式 profile-on 矩陣；profile-off throughput仍可報告，但不得以 sampled phase
下 production 歸因結論。每個正式 profile-on run 另須 `profiled_groups >= 50`，否則提高
iterations並重跑該組 off/on。

選定後設定並驗證：

```bash
SAMPLE_EVERY=<8、16或32>
test "$SAMPLE_EVERY" -eq 8 -o "$SAMPLE_EVERY" -eq 16 -o "$SAMPLE_EVERY" -eq 32
```

## 8. 正式矩陣與命令

每個 profile-off case 五輪。W 執行順序固定交錯，避免 storage temperature 偏向同一設定：

```text
r1: W=1, W=2, W=4
r2: W=4, W=1, W=2
r3: W=2, W=4, W=1
r4: W=1, W=4, W=2
r5: W=4, W=2, W=1
```

### 8.1 主要 gate：writer group=4,096 profile-off

每個 W／round 執行：

```bash
run_case formal-writer-g4096-off-w1-r1 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=256 \
  --iterations="$ITER_WRITER_G4096" --warmup=10000
```

其他輪只替換 case name、round 與 `--wal-prepare-workers=2|4`。W=2／4 每輪必須有非零
`actual_parallel_prepare_groups`；若為零，該輪不是有效的 parallel prepare 測試，先檢查
actual group size與threshold，不得直接降低threshold改變設計問題。

### 8.2 Writer group=256 crossover

命令與 8.1 相同，只改：

```text
case name               formal-writer-g256-off-w{W}-r{R}
--engine-group-size     256
--iterations            ITER_WRITER_G256
```

threshold仍固定256。必須如實報告 actual parallel groups，包括因實際 group 未達threshold而
fallback的情況；不得為取得較漂亮的 W=4 數字而降低threshold。

### 8.3 Direct WAL group=4,096 profile-off

```bash
run_case formal-wal-g4096-off-w1-r1 \
  --workload=wal_write_ceiling \
  --wal-phase-profile=off --wal-sync=per_group --wal-group-size=4096 \
  --wal-prepare-workers=1 --wal-parallel-prepare-min-commands=256 \
  --iterations="$ITER_WAL_G4096" --warmup=100
```

依同一五輪交錯順序替換 W。每輪必須 `replay_verified=true`；W=2／4 必須有非零 actual
parallel groups/tasks。

### 8.4 Writer profile-on 診斷

僅對 W=1、W=4 各跑五輪，使用 calibration 選出的 `SAMPLE_EVERY`：

```bash
run_case formal-writer-g4096-on-w4-r1 \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every="$SAMPLE_EVERY" \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --wal-prepare-workers=4 --wal-parallel-prepare-min-commands=256 \
  --iterations="$ITER_WRITER_G4096" --warmup=10000
```

W=1／W=4 profile-on應與相同W的profile-off輪交錯，而非先完成全部off再做on。每輪必須
`profiled_commands == completion_profiled_commands`、`profiled_groups >= 50`，且 profile
counter hierarchy檢查通過。

### 8.5 Direct WAL profile-on 診斷

為取得 prepare、plan/copy、write、publish與sync phase，只對W=1、W=4各跑五輪：

```bash
run_case formal-wal-g4096-on-w4-r1 \
  --workload=wal_write_ceiling \
  --wal-phase-profile=on --wal-sync=per_group --wal-group-size=4096 \
  --wal-prepare-workers=4 --wal-parallel-prepare-min-commands=256 \
  --iterations="$ITER_WAL_G4096" --warmup=100
```

分別計算W=1／W=4的profile-on/off median bias。bias >5%時，nested phase只能列為方向性
證據；direct WAL throughput gate仍只使用profile-off。

### 8.6 Public Engine W=1 control

此 workload不接受prototype options，確認 public Engine仍走預設W=1：

```bash
run_case formal-public-engine-w1-r1 \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations="$ITER_WRITER_G4096" --warmup=10000
```

執行五輪。不得加入 `--wal-prepare-workers` 或 threshold option；該組只作 public path
regression control，不與 internal writer profile workload混成同一median。

## 9. 每輪有效性與停止條件

每輪必須同時符合：

- process exit status為0，且 measured `elapsed_ms >= 15000`；
- writer為`correctness_verified=true`，direct WAL為`replay_verified=true`；
- W=1的actual parallel groups為0；W=2／4在group=4,096時大於0；
- command、completion、replay與durable head檢查全部通過；
- profile-on的sample count與counter hierarchy有效；
- 沒有phase timeout、publisher failure、storage pressure、callback error或sanitizer error；
- data directory在啟動前不存在，且未被其他輪次使用；
- source與binary identity和第4節相同。

任一W=2／4出現WAL bytes、ordering、segment boundary、replay、durable head、callback count、
hang或shutdown不一致，prototype立即不通過。低吞吐、sync tail、CPU增加或context switches增加
不是排除輪次的理由，必須保留並納入統計。只有環境／命令錯誤、外部中止、identity改變或
correctness失敗可以排除，且報告必須保留run名稱、log與理由。

目前writer summary可量到Completion queue residence、callback service與max depth，但沒有輸出
Publisher lag的逐時序資料。因此報告只能記錄publisher error／stop／recovery結果，不能宣稱已證明
Publisher沒有短暫backlog。若Completion queue residence或depth隨輪次duration持續增加，W=4不建議
成為production default。

## 10. 統計方法

每個case以五輪原始值計算median及min–max，不合併舊報告樣本：

- throughput：`commands_per_second`；
- uplift：`(median_Wx / median_W1 - 1) × 100%`；
- latency：p50、p99取五輪median；p99.9與max報五輪最差值；
- p99 regression：先以同round交錯輪比較，再報median比率；
- CPU、voluntary／involuntary context switches、maximum RSS：取median及min–max；
- actual parallel groups/tasks：逐輪列值，不只列median；
- writer phase：每輪先以`phase_ns / profiled_commands`換算ns/command，再取median；
- direct WAL group phase與sync latency是group latency，不得標成command latency；
- profile bias：`abs(on_median - off_median) / off_median × 100%`；
- `prepare_task_ns`是重疊lane elapsed總和，不是end-to-end latency，也不是process CPU time，
  不得與`prepare_ns`相加。

主要 gate：

```text
writer group=4,096 profile-off W4 uplift >= 10%
```

若W=4在多數配對輪次的p99穩定比W=1退化超過10%，或Completion出現持續backlog，即使RPS
通過也不建議設成production default。未達10%則停止此方向，不在同案追加buffer pool、
lock-free queue、更多workers或改fsync policy。

## 11. 報告檔案與格式

結果另存為：

```text
docs/wal-bounded-parallel-prepare-benchmark-report.md
```

不得覆寫既有歷史報告。報告依下列順序撰寫。

### 11.1 摘要與最終判定

先寫主要gate結果、有效／排除輪數、W=2與W=4 uplift、latency／CPU代價，以及結論只能是：

- `通過prototype gate，進入獨立production configuration設計`；或
- `未通過，production維持W=1並停止此優化方向`；或
- `量測無效，修正量測方法後重測`。

### 11.2 Artifact與環境

| 項目 | 值 |
| --- | --- |
| 日期／host／kernel | |
| CPU／logical／physical cores | |
| Compiler／flags | |
| HEAD | |
| Index diff SHA-256 | |
| Worktree diff SHA-256 | |
| Binary SHA-256 | |
| CPU affinity／governor／boost | |
| Filesystem／device／mount options | |
| RUN_ROOT／可用空間 | |
| 已知工具限制 | |

### 11.3 Build、correctness、smoke與calibration

列出Release／Debug／ASan／UBSan結果、TSan狀態、smoke的actual groups/tasks，以及W=1／W=4
sampling calibration：

| W | Sample every | Off median RPS | On median RPS | Bias | Profiled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 8 | | | | | |
| 4 | 8 | | | | | |

未執行的N=16／32填`not run`，不可留白造成已測錯覺。

### 11.4 Pilot與固定參數

| Workload | Group | Pilot iterations | W1/W2/W4 elapsed | Formal iterations | 最短formal elapsed |
| --- | ---: | ---: | --- | ---: | ---: |
| Writer | 4,096 | | | | |
| Writer | 256 | | | | |
| Direct WAL | 4,096 | | | | |

### 11.5 正式profile-off吞吐與latency

| Workload | Group | W | RPS median | RPS min–max | Uplift vs W1 | p50 median | p99 median | Worst p99.9 | Worst max |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Writer | 4,096 | 1 | | | baseline | | | | |
| Writer | 4,096 | 2 | | | | | | | |
| Writer | 4,096 | 4 | | | | | | | |
| Writer | 256 | 1 | | | baseline | | | | |
| Writer | 256 | 2 | | | | | | | |
| Writer | 256 | 4 | | | | | | | |
| Direct WAL | 4,096 | 1 | | | baseline | | | | |
| Direct WAL | 4,096 | 2 | | | | | | | |
| Direct WAL | 4,096 | 4 | | | | | | | |

Direct WAL latency欄必須註明是group latency。Public Engine W=1另列五輪median／range，不與internal
writer W=1混算。

### 11.6 實際路徑與資源成本

| Workload／group | W | Actual commands/group | Parallel groups（五輪） | Prepare tasks（五輪） | CPU % median | Vol CS median | Invol CS median | Max RSS median |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| | | | | | | | | |

另列WAL bytes、rotations、data write calls與sync p50／p99／p99.9／max。不要只列aggregate，
避免把某一輪fsync tail隱藏在median中。

### 11.7 Profile-on phase診斷

| Workload | W | Profile bias | Writer/WAL service ns/command | Prepare wall ns/command | Prepare task ns/command | Encode | CRC | Frame assembly | Plan/copy | Write | Publish | Sync |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Writer g4096 | 1 | | | | | | | | | | | |
| Writer g4096 | 4 | | | | | | | | | | | |
| Direct WAL g4096 | 1 | | | | | | | | | | | |
| Direct WAL g4096 | 4 | | | | | | | | | | | |

Writer另列StateMachine apply、publisher notify、completion enqueue、Completion service RPS、queue
p50／p99／p99.9／max與max depth。`publisher_notify`不是Publisher worker service time；Publisher
backlog未直接量測的限制必須保留。

### 11.8 逐輪有效性

| Run | Exit | Elapsed ms | Correctness／replay | Actual groups/tasks | RPS | p99 | CPU % | Context switches | 納入 | 排除理由 |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | --- | --- | --- |
| | | | | | | | | | | |

所有正式輪都要列出。低吞吐但有效的輪次仍填`納入=yes`。

### 11.9 Gate與後續決策

| Gate | 門檻 | 結果 | Pass/Fail |
| --- | --- | --- | --- |
| Correctness | 所有W bytes/order/replay/shutdown一致 | | |
| Writer g4096 W4 uplift | >=10% | | |
| Writer W4 p99 | 多數配對輪不得穩定退化>10% | | |
| Completion backlog | 不得隨run持續累積 | | |
| Profile bias | W1、W4各<=5%才作主要phase證據 | | |

結論必須清楚區分已證實事實、方向性profile證據與未量測限制。即使prototype通過，也只能建議
另案設計configuration與CPU budget；不得宣稱已達成1M commands/s。
