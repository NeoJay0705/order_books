# Engine Writer Hot Path collector cleanup 後重測操作與報告規格

## 1. 目的與適用範圍

本文件定義移除 benchmark-only collector progress counters 後，如何重新驗證
`docs/engine-writer-hot-path-root-cause-analysis-design.md`。本輪重測必須回答：

1. 修改後 `engine_writer_hot_path_profile` 的 profile-on/off bias 是否仍不超過 5%；
2. writer、WAL nested phases 與 Completion worker 的歸因是否仍支持既有根因結論；
3. authoritative Engine 與 direct WAL controls 在同一份新 artifact 上是否維持相同量級；
4. 下一個 production optimization 是否仍符合設計決策門檻。

本文件是既有
`docs/engine-writer-hot-path-root-cause-analysis-retest-procedure.md` 的本次執行清單，不修改其中的
durability、統計或決策規則。舊報告及舊 logs 保留為歷史證據；新舊輪次不得合併計算 median、range、
bias 或 phase share。

本輪不修改 production code、benchmark implementation、WAL format、group defaults 或 staging。
開始 formal matrix 後若 source、index 或 worktree diff 改變，該輪測試立即失效，必須重新建置並從
calibration 開始。

## 2. 完成條件

只有全部成立才視為本輪重測完成：

- Release、Debug、ASan/UBSan correctness tests通過；
- sampling smoke、WAL reopen/replay及CLI檢查通過；
- calibration選出的sampling interval在三輪off/on median間bias不超過5%；
- formal matrix共35輪有效，且所有可比較輪次使用同一個binary；
- 每個formal measured phase至少15秒；
- writer group=4,096 profile-on每輪至少50個sampled groups；
- sampled writer commands等於sampled Completion commands；
- 報告包含artifact identity、逐輪有效性、原始logs位置及第11節所有表格；
- 結論只選一個通過門檻的下一案，或明確寫出未通過決策門檻。

若 calibration 在 `N=8、16、32` 都無法把bias降至5%以下，停止formal matrix；這是量測失敗，不能
以方向性phase數據代替正式結論。

## 3. 固定環境

正式輪次固定使用：

```text
OS                  Linux
CPU affinity        taskset -c 2-7
filesystem          實體filesystem，不使用tmpfs或overlay
writer delay        1,000 us
writer lanes        8,192
WAL durability      per_group fsync
snapshot            ceiling workload期間停用
rounds              每個formal case五輪
```

所有輪次在同一台主機、同一filesystem與block device執行。測試期間不得切換CPU governor、boost、
I/O scheduler或mount options，也不得同時執行其他大量I/O。每輪使用新的空data directory，不清
page cache、不重用舊run、不因吞吐較低刪除有效輪次。

開始前確認目標裝置至少有30 GiB可用空間。測試程式與操作流程不得自動刪除raw logs或WAL data。

## 4. 建置、correctness與artifact identity

### 4.1 優先使用專案標準工具鏈

在repository root執行：

```bash
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset release-benchmark
cmake --build --preset release-benchmark
ctest --preset release

conan install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers

BENCH_BIN=./build/ReleaseBenchmark/benchmarks/order_books_benchmark
```

### 4.2 本機缺少CMake／Ninja時的等價GCC路徑

只有標準工具不可用時才使用本節，並在報告記錄原因。本機Conan GTest prefix若已失效，先重新取得
依賴；不得以跳過tests代替。

```bash
BUILD_ROOT=$(mktemp -d /tmp/order-books-writer-retest-build-XXXXXXXX)
GTEST_PREFIX=/home/neojhou/.conan2/p/gtestf6c2063f907ad/p

PROJECT_SOURCES=(src/domain/*.cpp src/persistence/*.cpp src/runtime/*.cpp)
BENCHMARK_SOURCES=(
  benchmarks/order_book_benchmark.cpp
  benchmarks/engine_writer_profile_benchmark.cpp
  benchmarks/pipeline_ceiling_benchmark.cpp
)
TEST_SOURCES=(tests/*.cpp tests/unit/*.cpp tests/integration/*.cpp)

test -f "$GTEST_PREFIX/include/gtest/gtest.h"
test -f "$GTEST_PREFIX/lib/libgtest.a"
test -f "$GTEST_PREFIX/lib/libgtest_main.a"

g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc -Ibenchmarks \
  "${PROJECT_SOURCES[@]}" "${BENCHMARK_SOURCES[@]}" \
  -pthread -o "$BUILD_ROOT/order_books_benchmark"

g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc -I"$GTEST_PREFIX/include" \
  "${PROJECT_SOURCES[@]}" "${TEST_SOURCES[@]}" \
  "$GTEST_PREFIX/lib/libgtest_main.a" "$GTEST_PREFIX/lib/libgtest.a" \
  -pthread -o "$BUILD_ROOT/order_books_tests_release"
"$BUILD_ROOT/order_books_tests_release"

g++ -std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc -I"$GTEST_PREFIX/include" \
  "${PROJECT_SOURCES[@]}" "${TEST_SOURCES[@]}" \
  "$GTEST_PREFIX/lib/libgtest_main.a" "$GTEST_PREFIX/lib/libgtest.a" \
  -pthread -o "$BUILD_ROOT/order_books_tests_debug"
"$BUILD_ROOT/order_books_tests_debug"

g++ -std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude -Isrc -I"$GTEST_PREFIX/include" \
  "${PROJECT_SOURCES[@]}" "${TEST_SOURCES[@]}" \
  "$GTEST_PREFIX/lib/libgtest_main.a" "$GTEST_PREFIX/lib/libgtest.a" \
  -pthread -o "$BUILD_ROOT/order_books_tests_sanitizers"
ASAN_OPTIONS=detect_leaks=1 "$BUILD_ROOT/order_books_tests_sanitizers"

BENCH_BIN="$BUILD_ROOT/order_books_benchmark"
```

Sanitizer binary只用於correctness，不得用來量測效能。

### 4.3 建立run目錄並凍結身份

以下命令只讀取Git狀態，不執行`git add`、`git reset`、`git restore`或其他會改變staging的操作：

```bash
set -o pipefail
RUN_PARENT=/home/neojhou
RUN_ROOT=$(mktemp -d "$RUN_PARENT/engine-writer-post-cleanup-retest-XXXXXXXX")
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data"

git rev-parse HEAD > "$RUN_ROOT/logs/source-identity.txt"
git status --short >> "$RUN_ROOT/logs/source-identity.txt"
git diff --cached --binary | sha256sum >> "$RUN_ROOT/logs/source-identity.txt"
git diff --binary | sha256sum >> "$RUN_ROOT/logs/source-identity.txt"
sha256sum "$BENCH_BIN" >> "$RUN_ROOT/logs/source-identity.txt"

uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor \
  >> "$RUN_ROOT/logs/environment.txt" 2>&1 || true
```

將 `RUN_ROOT`、`BENCH_BIN` 及五項identity值立即記入新報告。formal matrix完成後重新執行相同identity
命令並比對；任一值改變即停止，不混用已完成輪次。

## 5. Smoke

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
  --data-dir="$RUN_ROOT/data/smoke-writer-on" \
  2>&1 | tee "$RUN_ROOT/logs/smoke-writer-on.log"
```

Smoke必須exit 0，並同時符合：

- `correctness_verified=true`；
- `profiled_commands == completion_profiled_commands`；
- `profiled_commands > 0`且`profiled_groups > 0`；
- replay後durable head與EngineSeq連續；
- 沒有`phase_timeout`、publisher failure、storage pressure或callback error。

標準CTest會執行既有CLI tests。使用直接GCC路徑時，另外執行下列等價檢查；每個命令都必須
non-zero exit，且log包含指定error code：

```bash
if "$BENCH_BIN" --workload=engine_writer_hot_path_profile \
    --writer-phase-profile=on --writer-profile-sample-every=0 \
    > "$RUN_ROOT/logs/cli-sample-zero.log" 2>&1; then
  exit 1
fi
rg -q 'error_code=writer_profile_sample_every_invalid' \
  "$RUN_ROOT/logs/cli-sample-zero.log"

if "$BENCH_BIN" --workload=engine_writer_hot_path_profile \
    --writer-phase-profile=on --writer-profile-sample-every=abc \
    > "$RUN_ROOT/logs/cli-sample-invalid.log" 2>&1; then
  exit 1
fi
rg -q 'error_code=writer_profile_sample_every_invalid' \
  "$RUN_ROOT/logs/cli-sample-invalid.log"

if "$BENCH_BIN" --workload=engine_writer_hot_path_profile \
    --writer-phase-profile=off --writer-profile-sample-every=8 \
    > "$RUN_ROOT/logs/cli-sample-requires-on.log" 2>&1; then
  exit 1
fi
rg -q 'error_code=writer_profile_sample_requires_profile_on' \
  "$RUN_ROOT/logs/cli-sample-requires-on.log"

if "$BENCH_BIN" --workload=engine_durable_single_instrument \
    --writer-profile-sample-every=8 \
    > "$RUN_ROOT/logs/cli-sample-requires-writer.log" 2>&1; then
  exit 1
fi
rg -q 'error_code=writer_profile_sample_requires_writer_profile_workload' \
  "$RUN_ROOT/logs/cli-sample-requires-writer.log"
```

Smoke與CLI結果不納入效能統計。

## 6. Sampling calibration

先測 `N=8`。固定條件為group=4,096、delay=1,000 us、lanes=8,192、warmup=10,000、
iterations=1,000,000，代表2,000,000 measured commands。依下列ABBA順序執行：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3
```

代表命令：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=off \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=1000000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/cal-n8-off-r1" \
  2>&1 | tee "$RUN_ROOT/logs/cal-n8-off-r1.log"

taskset -c 2-7 "$BENCH_BIN" \
  --workload=engine_writer_hot_path_profile \
  --writer-phase-profile=on \
  --writer-profile-sample-every=8 \
  --engine-group-size=4096 \
  --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=1000000 \
  --warmup=10000 \
  --data-dir="$RUN_ROOT/data/cal-n8-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/cal-n8-on-r1.log"
```

其餘四輪只改run名稱與profile mode。每輪至少10秒且profile-on至少25個sampled groups。分別取得三輪
`commands_per_second`的median，計算：

```text
bias = abs(on_median - off_median) / off_median * 100%
```

- bias <=5%：選 `SAMPLE_EVERY=8`；
- bias >5%：以同一ABBA矩陣依序測 `N=16`、`N=32`；
- 每個新N都重新取得三輪off，不得沿用較早時段的off輪次；
- N=32仍未通過：停止，不執行formal matrix。

## 7. Formal matrix

設 `SAMPLE_EVERY` 為calibration選出的值。Writer off/on使用下列順序：

```text
off-r1, on-r1, on-r2, off-r2, off-r3, on-r3, on-r4, off-r4, off-r5, on-r5
```

### 7.1 Writer group=4,096

兩種mode都使用：

```text
iterations=1,680,000
warmup=10,000
group size=4,096
delay=1,000 us
lanes=8,192
```

Profile-on命令：

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

Profile-off命令使用完全相同參數，移除`--writer-profile-sample-every`並設
`--writer-phase-profile=off`。每輪使用對應且唯一的`formal-g4096-{off|on}-rN`名稱。

### 7.2 Writer group=256 control

依相同十輪順序執行，參數只改為：

```text
iterations=800,000
warmup=10,000
group size=256
delay=1,000 us
lanes=8,192
```

每輪使用`formal-g256-{off|on}-rN`名稱。

### 7.3 Authoritative Engine control

執行五輪，每輪使用新的`engine-control-rN`目錄：

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

### 7.4 Direct WAL control

Profile off/on各五輪，使用和writer相同的ABBA十輪順序：

```bash
taskset -c 2-7 "$BENCH_BIN" \
  --workload=wal_write_ceiling \
  --wal-phase-profile=on \
  --wal-sync=per_group \
  --wal-group-size=4096 \
  --iterations=2103 \
  --warmup=100 \
  --data-dir="$RUN_ROOT/data/wal-on-r1" \
  2>&1 | tee "$RUN_ROOT/logs/wal-on-r1.log"
```

Profile-off只將`--wal-phase-profile`改為`off`。每輪必須輸出`replay_verified=true`。

### 7.5 時間不足的處理

任一formal case的measured elapsed少於15秒時，提高該case的iterations，並將同case的off/on全部十輪
以相同新iterations重跑。不得只延長profile-on或只補跑不足輪次後與舊輪次混算。若
`SAMPLE_EVERY=32`，group=4,096預設將iterations提高至3,360,000，確保每輪至少50個sampled groups。

## 8. 輪次有效性與立即停止條件

每輪必須逐項記錄：

- exit status為0；
- measured elapsed達門檻；
- writer輸出`correctness_verified=true`；WAL輸出`replay_verified=true`；
- submitted、accepted、completed、durable及replayed counts一致；
- EngineSeq連續，最終active orders／levels符合workload預期；
- profile-on的`profiled_commands == completion_profiled_commands`；
- group=4,096 profile-on的`profiled_groups >= 50`；
- 沒有publisher failure、storage pressure、phase timeout、callback error或sanitizer error；
- data directory在啟動前為空且未被其他輪次使用。

吞吐較低、sync tail較高或context switches較多都不是排除理由。只有correctness失敗、環境設定錯誤、
外部中止或artifact identity改變可以排除；必須保留log、data directory與理由。

## 9. 統計方法

每個case先在單輪計算，再對五輪取median與min–max：

- throughput：`commands_per_second`；
- latency：p50與p99取五輪median，p99.9與max報五輪最差值；
- phase：`phase_ns / profiled_commands`；
- phase share：先用該輪正確parent denominator計算，再取五輪median；
- bias：`abs(on_median - off_median) / off_median`；
- Completion service rate、queue residence與depth獨立呈現，不加入writer service；
- `wal_sync_ns/command`逐輪列出，不能只保留median；
- direct WAL group latency的p50、p99、p99.9與max不得誤寫為command latency。

不得把parent與child phase相加、把sampled phase除以全部measured commands、跨輪先合計duration後算
share，或只選最快輪次。新報告可以用獨立表格比較舊報告median，但不得把舊輪次放入新median或range。

## 10. 決策門檻

1. group=4,096 writer bias <=5%時，phase attribution才是主要證據；否則停止production optimization
   決策。
2. bounded parallel prepare只有在prepare為穩定主要成本、payload encode與CRC為其主要子成本，且
   Amdahl估算2至4 workers的end-to-end uplift至少10%時才可維持為下一案。
3. `StateMachine::apply`只有在占writer service至少10%、跨輪穩定且WAL不是先行ceiling時，才進入
   下一層診斷；本輪不直接平行化apply。
4. Completion只有在service rate低於arrival rate，或queue depth／residence隨run duration持續增加時，
   才能判定為瓶頸。
5. direct WAL慢輪次只有在吞吐下降與sync tail同輪出現時，才能歸因為sync latency變異；不得將此因果
   無條件外推至沒有phase clocks的writer profile-off輪次。

## 11. 新報告格式與必填內容

新報告另存為：

```text
docs/engine-writer-hot-path-root-cause-analysis-post-cleanup-retest-report.md
```

不得覆寫既有歷史報告。報告依下列順序撰寫。

### 11.1 摘要

記錄有效／排除輪數、selected sampling interval、三項bias、三個主要throughput、是否維持既有根因
結論，以及唯一下一案。

| Workload | Group | Profile | Sample every | RPS median | Range | Bias | Evidence |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| Writer | 4,096 | off | — | | | — | diagnostic control |
| Writer | 4,096 | on | | | | | attribution |
| Writer | 256 | off | — | | | — | control |
| Writer | 256 | on | | | | | attribution |
| Engine durable | 4,096 | authoritative | — | | | — | end-to-end control |
| Direct WAL | 4,096 | off | — | | | — | WAL control |
| Direct WAL | 4,096 | on | — | | | | WAL attribution |

### 11.2 測試身份與環境

必填：日期、host、kernel、CPU、logical／physical cores、compiler、flags、HEAD、index diff hash、
worktree diff hash、binary SHA-256、CPU affinity、governor、boost、filesystem、device、mount options、
可用空間、sync mode、segment size、`RUN_ROOT`及工具限制。

### 11.3 Build、correctness與calibration

- Release／Debug／ASan／UBSan結果；
- smoke及CLI checks；
- calibration逐輪RPS、elapsed及sampled groups；
- selected N與選擇理由。

| Sample every | Off median RPS | On median RPS | Bias | Sampled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | | | | | |
| 16 | | | | | 若未執行填not run |
| 32 | | | | | 若未執行填not run |

### 11.4 Formal throughput與latency

| Workload | Group | Profile | RPS median | RPS range | Elapsed median | p50 median | p99 median | Worst p99.9 | Worst max |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| | | | | | | | | | |

另外列出距離1M commands/s目標、Engine/direct-WAL比例，以及writer profile-off與authoritative Engine
的差異。

### 11.5 Writer與WAL phase

| Phase | ns/command median | min–max | Writer-service share median | 跨輪穩定性 | 證據等級 |
| --- | ---: | ---: | ---: | --- | --- |
| admission | | | | | |
| WAL append | | | | | |
| WAL sync | | | | | |
| StateMachine apply | | | | | |
| publisher notify | | | | | |
| post-apply | | | | | |
| completion enqueue | | | | | |
| unattributed | | | | | |

另列payload encode、CRC、frame assembly、prepare remainder、chunk copy、plan/copy remainder、publish及
write的`ns/command`、range與parent share；再以逐輪表列出RPS與`wal_sync_ns/command`。

### 11.6 Completion worker

| Group | Service RPS median | Queue p50 | Queue p99 | Worst p99.9 | Max depth | 是否持續累積 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,096 | | | | | | |
| 256 | | | | | | |

Publisher replay、sink ACK及cursor persistence不屬於本表；`publisher_notify`也不得解讀為Publisher
worker service time。

### 11.7 逐輪有效性

35輪逐一列出：run名稱、exit status、elapsed、correctness/replay、profile count、是否納入及排除理由。
所有低吞吐但correctness通過的輪次必須保留。

### 11.8 舊artifact對照

只比較舊、新artifact的median與bias，不混合樣本。至少列出：

- group=4,096 writer off/on RPS與bias；
- writer service及主要phase `ns/command`；
- Completion service RPS；
- authoritative Engine及direct WAL RPS；
- 差異是否超過正常run-to-run range。

### 11.9 結論、限制與artifact

結論分成「已證實」、「方向性」及「尚未證實」，並明確回答collector cleanup是否改變原根因判斷。
只選一個production optimization；若門檻未通過，寫出下一個最小診斷動作。最後記錄raw logs／data
絕對路徑、保留政策、工具限制及結果只適用於本次硬體與artifact。

## 12. 執行後檢查清單

- [ ] source、index、worktree及binary identity在測試前後一致；
- [ ] correctness與sanitizer全部通過；
- [ ] calibration bias不超過5%；
- [ ] 35輪formal matrix完整且每輪至少15秒；
- [ ] 所有profile counts、correctness與replay checks通過；
- [ ] 沒有因低吞吐排除有效輪次；
- [ ] 統計以per-run後取median，沒有混入舊artifact輪次；
- [ ] 新報告包含第11節所有表格與raw artifact路徑；
- [ ] 結論沒有超出量測證據，也沒有同時展開多個production optimization。
