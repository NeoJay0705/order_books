# Engine Writer Hot Path 低干擾重測操作與報告規格

## 1. 文件目的

本文件定義 `engine_writer_hot_path_profile` 的低干擾重測方法，以及重測報告必須呈現的
格式與內容。目的是解決既有 group=4,096 profile-on/off throughput bias 為 34.98% 的問題，
在不改變 WAL durability、Engine ordering 或 production defaults 的前提下，取得足以選擇下一個
production optimization 的證據。

本次不是原樣重跑既有 profile。正式重測前，必須先讓詳細量測支援 deterministic sampling；
否則即使增加輪數，仍可能只重現受 instrumentation 干擾的結果。

## 2. 必須回答的問題

重測必須回答以下問題：

1. group=4,096 的 profile-on/off bias 能否降至 5% 以下？
2. 正常吞吐約 161K commands/s 時，writer service 的穩定成本主要位於 WAL prepare、WAL sync、
   `StateMachine::apply`、admission、post-apply，還是 completion enqueue？
3. 既有慢輪次是否由 `Wal::sync()` wall time 增加造成，而非其他 CPU phase 同時退化？
4. WAL prepare 中 payload encode、CRC、frame assembly 與 remainder 的主要成本為何？
5. Completion worker 的 service rate 是否低於 writer arrival rate，queue residence／depth 是否持續
   增加？
6. 哪一個且只有一個 production optimization 符合既有設計的決策門檻？

Publisher worker replay、EventSink ACK 與 cursor persistence 不在這次 writer 重測範圍內。
`publisher_notify` 只代表 writer 呼叫 `notify_publishable()` 的成本，不能當成 Publisher worker 成本。

## 3. 重測前置修改

### 3.1 必要 CLI

在執行本文件的正式矩陣前，benchmark 必須提供下列 opt-in 選項；名稱若因現有程式風格微調，
報告必須記錄實際名稱與語意：

```text
--writer-profile-sample-every=N
```

必要語意：

- 僅適用於 `--workload=engine_writer_hot_path_profile` 且
  `--writer-phase-profile=on`；
- `N` 必須大於零；`N=1` 表示目前每個 group 都詳細量測的行為；
- 從 measured phase 的第一個 durable group 開始，每固定 N 個 group 選取一個 sampled group；
- 未被選取的 group 必須走正式未 profile 的 WAL append 與 Completion 路徑，不取得詳細 phase
  clock、不配置 completion sample；
- sampled group 中的 command 才攜帶 Completion enqueue timestamp，並計入 Completion phase；
- normal `Engine::open()` 與 `--writer-phase-profile=off` 路徑不得增加 clock、sample allocation 或
  completion item storage；
- sampling 不得改變 group construction、WAL bytes、CRC、fsync boundary、EngineSeq、apply ordering、
  result、event 或 replay 結果。

輸出至少增加：

```text
writer_profile_sample_every=N
eligible_groups=N
profiled_groups=N
profiled_commands=N
completion_profiled_commands=N
```

所有 phase 的 `ns_per_command` 必須以 `profiled_commands` 為 denominator。整體 throughput 與
end-to-end latency 仍以全部 measured commands 計算。不得把 sampled phase total 除以全部 command，
也不得把 Completion worker wall time加進 writer service time。

### 3.2 Sampling 正確性測試

在壓測前，測試至少覆蓋：

- `N=1`、`N=2` 與大於總 group 數的 N；
- sampled group／command count符合固定取樣規則；
- profiled writer commands與profiled completion commands完全相等；
- profile off/on產生相同 results、events、WAL bytes、durable head與replay結果；
- 未 sampled group不產生詳細 WAL 或 Completion sample；
- rejected group不混入durable sampled group；
- invalid、zero或錯誤 workload上的 sampling option被CLI拒絕；
- profile collector失敗不改寫已完成command的business result。

## 4. 測試環境固定條件

所有可比較的正式輪次必須符合：

- Linux Release build，使用相同 compiler、flags與binary；
- 同一台主機、同一CPU affinity、同一filesystem與block device；
- WAL位於實際filesystem，不使用tmpfs或overlay filesystem；
- 每輪使用新的空data directory，不重用或清空舊run；
- 不清page cache，不在正式矩陣中途修改CPU governor、boost、I/O scheduler或mount options；
- 同一時間只執行一個benchmark，避免同裝置的其他大量I/O；
- 每個正式measured phase至少15秒；
- 每個case五輪，off/on交錯執行，不刪除僅因throughput較低的有效輪次；
- correctness或replay失敗的輪次視為測試失敗，不得作為效能樣本；
- 所有正式輪次必須使用同一source state；修改source後，舊結果不得與新結果拼接。

建議沿用既有基準環境的CPU affinity：

```text
taskset -c 2-7
```

若主機允許固定為 performance governor，必須在所有 calibration 與 formal run 前完成並記錄；
若沒有權限，保留既有 governor並記錄。不得只在部分輪次改變設定。

開始前確認run所在裝置至少有30 GiB可用空間。測試資料可能超過20 GiB，完成後不要由測試腳本
自動刪除，以便追查outlier與replay結果。

## 5. 建置與測試前驗證

從repository root執行：

```bash
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset release-benchmark
cmake --build --preset release-benchmark
ctest --preset release
```

sampling修改也必須通過Debug與sanitizer測試；sanitizer binary不作效能量測：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

記錄artifact與source identity：

```bash
BENCH_BIN=./build/ReleaseBenchmark/benchmarks/order_books_benchmark
sha256sum "$BENCH_BIN"
git rev-parse HEAD
git status --short
git diff --cached | sha256sum
git diff | sha256sum
```

若使用dirty worktree，報告必須同時記錄HEAD、index diff hash、worktree diff hash與binary hash；
binary hash是本次結果的最終artifact identity。

建立repository外的結果目錄。下例的父目錄必須位於預定WAL裝置：

```bash
set -o pipefail
RUN_PARENT=/mnt/local-nvme/order-books-benchmark
RUN_ROOT=$(mktemp -d "$RUN_PARENT/engine-writer-retest-XXXXXXXX")
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data"
```

記錄環境：

```bash
uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor \
  >> "$RUN_ROOT/logs/environment.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/environment.txt"
git rev-parse HEAD >> "$RUN_ROOT/logs/environment.txt"
```

若個別環境檔不存在，記錄「不可取得」及原因，不要為了取得資料而修改kernel安全設定。

## 6. Smoke 與 sampling 校準

### 6.1 功能 smoke

先以小負載確認CLI、count與replay：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every=8 \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=10000 \
  --warmup=1000 \
  --data-dir="$RUN_ROOT/data/smoke-writer-on"
```

smoke必須成功輸出 `correctness_verified=true`，且profiled writer／Completion command counts一致。
Smoke結果不納入正式統計。

### 6.2 Calibration 目的

Calibration只選擇最低干擾且樣本足夠的sampling interval，不用來選production optimization。
候選依序為 `N=8、16、32`，不得因phase比例較符合預期而選擇N。

固定條件：

```text
group size       4,096
group delay      1,000 us
producer lanes   8,192
warmup           10,000 iterations
measured         840,000 iterations = 1,680,000 commands
rounds           profile off三輪；每個候選N profile on三輪
```

每一輪都使用新的data directory。每個候選N都以
`off-r1、on-r1、on-r2、off-r2、off-r3、on-r3` 的ABBA順序取得三輪off與三輪on；只有8未通過
才測16，只有16未通過才測32。不得用較早時段的三輪off搭配較晚時段的三輪on。每輪measured
phase應至少10秒；不足時將iterations等比例增加，off/on保持相同command count。

代表命令：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=840000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/cal-g4096-off-r1" \
  2>&1 | tee "$RUN_ROOT/logs/cal-g4096-off-r1.log"

taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every=8 \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=840000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/cal-g4096-n8-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/cal-g4096-n8-on-r1.log"
```

使用每個case三輪 `commands_per_second` 的median計算：

```text
bias = abs(profile_on_median - profile_off_median) / profile_off_median
```

選擇規則：

- 選第一個bias <=5%的N；
- profile-on每輪至少要有25個sampled groups；不足則增加iterations，不得降低N；
- bias在5%到10%之間不算校準完成；
- N=32仍大於5%時停止正式矩陣，先重新檢查Completion sampling、逐record clock、collector配置與
  filesystem／排程變異；不得直接用該結果選production修改。

## 7. 正式測試矩陣

設 `SAMPLE_EVERY` 為第6節選出的N。每個case執行五輪，順序採
`off-r1、on-r1、on-r2、off-r2、off-r3、on-r3、on-r4、off-r4、off-r5、on-r5`，降低固定順序
造成的溫度或背景負載偏差。

### 7.1 Writer group=4,096

```text
iterations       1,680,000，代表3,360,000 measured commands
warmup           10,000
group delay      1,000 us
producer lanes   8,192
profile          off/on各五輪
```

如果 `SAMPLE_EVERY=32`，將iterations提高至3,360,000，使每輪至少約50個sampled groups；off/on
必須一起提高。N=8或16可沿用1,680,000 iterations。每輪仍必須驗證實際 `profiled_groups >= 50`。

Profile-on代表命令：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every="$SAMPLE_EVERY" \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=1680000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/formal-g4096-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/formal-g4096-on-r1.log"
```

Profile-off使用相同參數，移除sampling option並設為
`--writer-phase-profile=off`。

### 7.2 Writer group=256 control

```text
iterations       800,000，代表1,600,000 measured commands
warmup           10,000
group delay      1,000 us
producer lanes   8,192
profile          off/on各五輪
```

Profile-on代表命令：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every="$SAMPLE_EVERY" \
  --engine-group-size=256 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=800000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/formal-g256-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/formal-g256-on-r1.log"
```

### 7.3 Authoritative Engine control

執行五輪：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_durable_single_instrument \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=1680000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/engine-control-r1" \
  2>&1 | tee "$RUN_ROOT/logs/engine-control-r1.log"
```

此結果是production-like throughput／latency的authoritative control。Writer profile-off只應與它
交叉確認，不取代它。

### 7.4 Direct WAL control

profile off/on各五輪，off/on交錯：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=wal_write_ceiling \
  --wal-phase-profile=on \
  --wal-sync=per_group \
  --wal-group-size=4096 \
  --iterations=2103 \
  --warmup=100 \
  --data-dir="$RUN_ROOT/data/wal-control-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/wal-control-on-r1.log"
```

Profile-off只把 `--wal-phase-profile` 改為 `off`。每輪必須通過reopen／replay，且
`replay_verified=true`。

### 7.5 外部觀測

另做一輪不納入正式throughput統計的group=4,096 profile-off：

```bash
/usr/bin/time -v taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=1680000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/time-control" \
  > "$RUN_ROOT/logs/time-control.log" 2>&1
```

同一代表性run可使用 `pidstat -t` 或 `iostat -xz 1` 作輔助觀測，但輔助工具run不得混入正式五輪。
`perf stat`僅在既有權限允許時使用；若 `perf_event_paranoid` 拒絕，記錄限制即可，不修改sysctl。

## 8. 有效輪次與停止條件

正式輪次必須同時符合：

- process exit status為0；
- measured phase >=15秒；
- `correctness_verified=true`，WAL control另須 `replay_verified=true`；
- expected、accepted、completed與profile count一致；
- durable head、EngineSeq continuity、active orders／levels與預期一致；
- sampled writer commands等於sampled Completion commands；
- group=4,096 profile-on的 `profiled_groups >= 50`；
- 沒有publisher failure、storage pressure、phase timeout或callback error；
- data directory在開始時為空，且未被其他輪次重用。

不因throughput低而排除通過上述條件的輪次。只有啟動失敗、環境設定錯誤、程序被外部終止或
correctness失敗的輪次可以排除；報告仍須列出排除原因與留下的artifact。

正式結果完成後重新計算五輪bias：

- `<=5%`：phase share可作主要歸因證據；
- `>5% 且 <=10%`：只能作方向性證據，不能單獨決定production修改；
- `>10%`：本次診斷不通過，必須改善量測後再測。

## 9. 統計方法

每個case報告五輪：

- throughput median及min–max range；
- elapsed median；
- 各輪p50、p99、p99.9的median，以及五輪最差p99.9與max；
- commands/group、WAL MiB/s與fsync mode；
- profile-on sampled groups、sampled commands及coverage；
- 每個phase先在單輪計算 `phase_ns / profiled_commands`，再取五輪median與min–max；
- phase share先在單輪以正確parent denominator計算，再取五輪median；
- `wal_sync_ns/command`另外呈現五輪min、median、max，不能只報median而隱藏慢輪次；
- Completion service rate、queue residence p50/p99/p99.9、max depth分開報告，不與writer duration相加。

不得：

- 把不同輪次的total duration先相加再算單一share；
- 把sampled phase除以全部measured commands；
- 將parent與child phase相加；
- 只選最快一輪；
- 以profile-on throughput取代profile-off／Engine authoritative throughput；
- 將local ext4結果外推成其他SSD、RAID或cloud block device的保證。

## 10. 根因與下一步決策規則

報告必須把結論分為「已證實」、「方向性」與「尚未證實」，並只選一個有證據支持的下一案。

### 10.1 Sync／storage path

若慢輪次的吞吐下降主要隨 `wal_sync_ns/command` 增加，而prepare、apply、admission等CPU phase的
absolute time保持穩定，則可以確認慢輪次的直接原因為sync latency變異。下一案應先分析裝置
latency、fsync distribution與group-commit等待，不先平行化CPU phase。

### 10.2 Parallel prepare

只有全部成立才可選擇bounded parallel prepare：

- group=4,096 writer bias <=5%；
- WAL prepare是穩定的主要cost；
- payload encode + CRC占prepare主要部分，而不是chunk copy或unknown remainder；
- profile-off顯示writer有CPU工作且主機有可用核心；
- 使用 `speedup = 1 / ((1 - P) + P / W)` 估算2至4 workers的end-to-end uplift至少10%；
- Completion與Publisher notify不是同步瓶頸。

### 10.3 StateMachine apply

若apply占writer service至少10%，且其absolute time穩定、WAL並非先行ceiling，下一案才細分
book lookup、matching、event creation與incremental validation。本次重測不得直接平行化apply。

### 10.4 Completion

只有Completion service rate低於writer arrival rate，或queue depth／residence隨run duration持續
增加，才選擇Completion batching。單純max depth等於一個group size不足以證明Completion瓶頸。

### 10.5 無法決策

若沒有候選通過門檻，報告必須明確寫成「未通過production optimization決策門檻」，並指出下一個
最小診斷修改。不得為了產出結論而挑選部分run或忽略bias。

## 11. 重測報告格式

報告建議存為：

```text
docs/engine-writer-hot-path-root-cause-analysis-retest-report.md
```

必須依下列結構撰寫。

### 11.1 摘要

- 有效／排除輪數；
- selected sampling interval與sample coverage；
- group=4,096、group=256、direct WAL的bias；
- Engine authoritative、writer profile-off與direct WAL median throughput；
- 已確認的直接原因；
- 唯一下一案，或明確記錄未通過決策門檻。

摘要表：

| Workload | Group | Profile | Sample every | RPS median | Range | Bias | Evidence |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| Writer | 4,096 | off | — | | | | authoritative diagnostic control |
| Writer | 4,096 | on | | | | | attribution |
| Writer | 256 | off | — | | | | control |
| Writer | 256 | on | | | | | attribution |
| Engine durable | 4,096 | off | — | | | — | authoritative end-to-end |
| Direct WAL | 4,096 | off/on | — | | | | WAL control |

### 11.2 測試身份與環境

- 日期、host、kernel、CPU、logical／physical cores；
- compiler、build type、完整flags；
- HEAD、index/worktree diff hash與binary SHA-256；
- CPU affinity、governor、boost；
- filesystem、device、mount options、可用空間；
- snapshot、segment size與sync mode；
- raw data與log絕對路徑；
- `perf`、`pidstat`、`iostat`可用性與限制。

### 11.3 Sampling 校準

| Sample every | Off median RPS | On median RPS | Bias | Sampled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | | | | | |
| 16 | | | | | |
| 32 | | | | | |

說明只依bias與sample sufficiency選N，沒有依phase結果挑選。

### 11.4 正式 throughput 與 latency

| Workload | Group | Profile | RPS median | RPS range | Elapsed median | p50 median | p99 median | Worst p99.9 | Worst max |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| | | | | | | | | | |

另外列出Engine/direct-WAL比例、距離1M/s目標的比例與writer profile-off對Engine control的差異。

### 11.5 Writer phase

| Phase | ns/command median | min–max | Share median | 跨輪穩定性 | 證據等級 |
| --- | ---: | ---: | ---: | --- | --- |
| admission | | | | | |
| WAL append | | | | | |
| WAL sync | | | | | |
| apply | | | | | |
| publisher notify | | | | | |
| post-apply | | | | | |
| completion enqueue | | | | | |
| unattributed | | | | | |

必須另外以逐輪表呈現 `wal_sync_ns/command` 與RPS，確認兩者是否共同變動。

### 11.6 WAL prepare／copy子階段

| Phase | ns/command median | min–max | Parent share | 判斷 |
| --- | ---: | ---: | ---: | --- |
| payload encode | | | | |
| CRC | | | | |
| frame assembly | | | | |
| prepare remainder | | | | |
| chunk copy | | | | |
| plan/copy remainder | | | | |
| publish | | | | |
| write | | | | |

### 11.7 Completion worker

| Group | Service RPS median | Queue p50 | Queue p99 | Worst p99.9 | Max depth | 是否累積 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,096 | | | | | | |
| 256 | | | | | | |

必須比較Completion service RPS與writer arrival RPS；queue depth是否「持續增長」需由時間序列或
不同run length交叉確認，不能由單一最大值推定。

### 11.8 Correctness與有效性

列出所有輪次的exit status、correctness、replay、command counts、sample counts、elapsed與是否納入。
排除輪次必須保留名稱、原因及artifact位置。

### 11.9 結論與唯一下一步

依序寫出：

1. 已證實事項；
2. 方向性證據；
3. 尚未證實事項；
4. instrumentation是否通過bias門檻；
5. 唯一被選定的production optimization及其門檻證據；若沒有，寫出唯一必要的後續診斷修改；
6. 明確列出本次未修改的durability、ordering、WAL format、StateMachine ownership、Publisher與
   public API。

## 12. 本次重測完成條件

只有全部成立才算完成：

- sampling correctness、Release、Debug與ASan/UBSan測試通過；
- calibration選出bias <=5%的sampling interval；
- 正式矩陣所有case各有五輪有效結果；
- group=4,096每輪至少50個sampled groups；
- correctness、durable head、WAL replay與Completion counts全部通過；
- 報告同時呈現absolute time、phase share、run-to-run range與instrumentation bias；
- 報告補足WAL payload／CRC／frame／copy與Completion service／queue資料；
- 結論區分sync變異、穩定CPU成本與尚未證實的causal claim；
- 只選一個通過門檻的下一步，不同時展開多個production optimization；
- raw logs、environment metadata與data directories位置可追溯。
