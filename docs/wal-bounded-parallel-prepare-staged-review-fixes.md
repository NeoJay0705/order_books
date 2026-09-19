# WAL bounded parallel prepare staged review 必要修正

## 1. 文件目的

本文件記錄 `docs/wal-bounded-parallel-prepare-design.md` 與目前 staged changes 比對後，
仍必須完成的最小修正。目標是讓實作、測試、benchmark 輸出與設計的驗收方式一致，
不是擴大 prototype 範圍。

本次只修正兩類問題：

1. profile-off 正式壓測沒有回報實際走過多少 parallel prepare groups／tasks，無法證明
   測到的是目標路徑。
2. long-lived worker 的 repeated shutdown，以及跨 rotation 的 position／size／write count
   與 benchmark smoke 的實際平行化，尚未被測試明確證明。

不修改 WAL format、durability boundary、fsync policy、StateMachine、Publisher、Completion、
public `RuntimeConfig`，也不加入通用 executor、thread pool、buffer pool 或新的 dependency。

## 2. Review 結論

目前 staged implementation 的 bounded worker、ordered merge、threshold fallback、W=1 預設、
WAL byte identity 與 profile hierarchy 方向正確，沒有發現必須重寫架構的問題。

但設計的主要 throughput gate 使用 profile-off。現有實作只有 profile-on 才填入及輸出
`parallel_prepare_groups`／`prepare_tasks`，因此正式結果只能證明「設定了 W=4」，不能證明
measured phase 中的實際 batch 有跨過 threshold 並走 parallel path。這會讓 throughput
結果無法歸因，必須修正。

此外，目前 worker smoke 只檢查欄位存在，數值為零仍會通過；integration test 也沒有明確
執行多輪 open／append／sync／close／reopen。這些都屬於長生命週期 worker prototype 的必要
correctness gate。

## 3. 必要修正一：profile-off 的低干擾實際路徑計數

### 3.1 問題

- writer profile-off summary 只輸出 `wal_prepare_workers` 與 threshold。
- direct-WAL 只有 `--wal-phase-profile=on` 才累計 parallel groups／tasks。
- `group_size` 是最大 group size，不保證每一批都達到該大小；只看設定值與平均
  `actual_commands_per_group` 不能判定實際有多少批使用 parallel prepare。
- 不可以用開啟完整 profiling 的方式解決，因為逐 record clock 會污染主要 profile-off
  throughput gate。

### 3.2 最小資料模型

在 internal `src/persistence/wal.hpp` 增加：

```cpp
struct WalPrepareStats {
  std::uint64_t parallel_groups{};
  std::uint64_t tasks{};
};
```

`Wal` 增加 private cumulative state 與 internal accessor：

```cpp
[[nodiscard]] WalPrepareStats prepare_stats() const;

WalPrepareStats prepare_stats_{};
```

此 type 與 accessor 位於 `src/` internal header，不加入 installed public headers，也不放進
public `MetricsSnapshot`。讀取 accessor 時使用既有 WAL mutex 取得一致 snapshot，不需要新增
atomic 或第二把鎖。

counter 採 saturating add，避免長時間程序發生 unsigned wraparound。counter 只用於診斷，
溢位時飽和，不改變 append 結果或 durability 語意。

### 3.3 計數語意

只有成功完成 prepare、即將進入既有 `append_prepared_unlocked()` 的非空 batch 才計數：

- sequential W=1 或 threshold fallback：`tasks += 1`，`parallel_groups` 不變。
- parallel path：`parallel_groups += 1`，`tasks += 非空 range 數`。
- prepare error／exception：兩個 cumulative counters 都不前進。
- WAL append、write 或 sync 在 prepare 成功後失敗，不回滾 prepare counters；它們描述的是
  prepare path execution，不是 durable commit count。

profiled call 仍填原有 `WalAppendProfile`。cumulative stats 與 profile sample 的計數語意必須
一致，但 cumulative stats 不啟用任何 clock，也不逐筆更新共享 state；每個 batch 只在 caller
完成 ordered merge 後更新一次。

### 3.4 `Wal` 實作位置

修改 `prepare_records_unlocked()` 與 `PrepareWorkers::prepare()` 的成功出口：

1. sequential path 成功建立全部 `PreparedRecord` 後，更新 cumulative `tasks`。
2. parallel path 所有 lanes 成功且 ordered merge 完成後，更新 cumulative
   `parallel_groups` 與 `tasks`。
3. 不要讓 worker thread 寫 cumulative stats；只有持有 WAL mutex 的 caller 更新。
4. 不要在 record loop 內更新 counter，避免額外 contention。

若要避免 `PrepareWorkers` 持有 `Wal` 指標，可讓 `PrepareWorkers::prepare()` 回傳成功 records
時，同時透過一個 caller-owned result metadata 回報 `parallel` 與 `task_count`；或由 caller
傳入 `WalPrepareStats*`，並保證只在 join 完成後由 caller 更新。不得讓背景 worker 直接存取
`Wal::prepare_stats_`。

### 3.5 `ShardRuntime` internal forwarding

在 internal `ShardRuntime` 增加唯讀 accessor：

```cpp
[[nodiscard]] storage::WalPrepareStats wal_prepare_stats() const;
```

它只轉呼叫 `wal_->prepare_stats()`，供 writer benchmark 在 warmup／measured phase 邊界取樣。
這不是 public `Engine` API，也不加入 `RuntimeConfig`。

### 3.6 Benchmark measured-phase delta

兩個 benchmark 都必須排除 warmup：

```text
run warmup
before = prepare_stats()
run measured phase
after = prepare_stats()
actual_parallel_prepare_groups = after.parallel_groups - before.parallel_groups
actual_prepare_tasks = after.tasks - before.tasks
```

減法前驗證 `after >= before`；不成立時回報穩定的 benchmark error code，例如
`wal_prepare_stats_regressed`，不得 unsigned underflow。

#### writer benchmark

在 `run_engine_writer_hot_path_profile()`：

- warmup 完成後、measured phase 開始前讀一次 `runtime->wal_prepare_stats()`。
- measured callbacks 全部完成後再讀一次。
- summary 無論 profile on/off 都輸出：

```text
actual_parallel_prepare_groups=N
actual_prepare_tasks=N
```

- profile-on 的 sampled counters 保留原語意；不要把 sampled counter 當成 measured phase
  的完整 counter。

#### direct-WAL benchmark

在 `run_wal_write_ceiling()`：

- warmup groups 完成後讀 `wal.prepare_stats()`。
- measured groups 完成後再讀並計算 delta。
- 主 summary 無論 `--wal-phase-profile=off|on` 都輸出相同兩個 `actual_*` 欄位。
- profile-on 區塊原有 counters 是 sampled/profiled groups 的資料，命名或文件必須明確區分，
  不得與完整 measured-phase delta 混為一談。

### 3.7 必要測試

在 `persistence_test.cpp` 補 cumulative stats assertions：

- W=1 成功 batch：groups=0、tasks=1。
- W=4 threshold fallback：groups=0、tasks=1。
- W=4、N=3：groups=1、tasks=3。
- 再 append 一個成功 parallel batch，確認 counters 累加而非覆寫。
- error path 若現有 schema 有可達的 prepare error，確認 counters 不前進；不得只為此新增
  production fault-injection framework。

## 4. 必要修正二：補齊 worker correctness gate

### 4.1 強化 benchmark smoke

現有 `order_books_benchmark_writer_parallel_prepare_smoke` 的 regex 只確認欄位存在，零值也會
通過。修改方式：

1. 使用非零 group delay，例如 `--engine-group-delay-us=1000`，讓四個 producer commands
   能穩定形成至少兩筆的 group。
2. 維持 W=4，threshold=2。
3. PASS regex 必須驗證：

```text
actual_parallel_prepare_groups=[1-9][0-9]*
actual_prepare_tasks=[1-9][0-9]*
```

4. smoke 仍只驗證路徑與 correctness，不加入固定 RPS assertion。

另增加一個最小 W=2 direct-WAL smoke，使用小 iterations/group size，確認合法 W=2 CLI、
options forwarding、replay 與非零 actual parallel counters。既有 default W=1 smoke 與 W=4
writer smoke 加上此案例，即覆蓋合法 W=1／2／4。

### 4.2 跨 rotation 的一致性 assertions

擴充既有 `WalParallelPreparePreservesBytesOrderingAndReplay`，不要另建大型 fixture：

- 保存 W=1、W=2、W=4 的 `WalPosition`。
- 比較最後 `engine_seq`、segment filename 與 `end_offset`。
- 比較 `size_bytes()`。
- 對 W=1 與 W=4 使用 profiled append，確認 `frame_bytes`、`data_write_calls`、`rotations`
  相同；W=2 仍可保留 normal path，用 byte identity 覆蓋非 profiled parallel path。
- 保留現有逐檔 byte-for-byte 比較與 replay ordering assertions。

不需要新增 WAL parser 或重新實作 segment planning；現有 position、profile 與檔案比較已足夠。

### 4.3 Repeated shutdown／reopen test

新增一個小型 integration test，使用相同 WAL directory 重複至少 8 次：

```text
for each cycle:
  open W=4, threshold=1
  replay existing durable records
  append a small batch with continuous EngineSeq
  sync
  verify prepare stats used parallel path
  destroy Wal                          # 必須 stop/join workers

final open W=4
replay all records
verify count and EngineSeq ordering
destroy Wal
```

測試不得使用 sleep 判斷 worker 已停止。`Wal` destructor 返回就是 join 完成的同步點；如果有
hang，CTest timeout／sanitizer run 應直接失敗。

## 5. 修改檔案與預估行數

```text
src/persistence/wal.hpp                         10～18 行
src/persistence/wal.cpp                         20～35 行
src/runtime/shard_runtime.hpp/.cpp               4～10 行
benchmarks/engine_writer_profile_benchmark.cpp  15～25 行
benchmarks/order_book_benchmark.cpp             15～25 行
benchmarks/CMakeLists.txt                        8～18 行
tests/integration/persistence_test.cpp          30～50 行
```

總計約 100～180 行。若為取得兩個 counters 而新增通用 metrics subsystem、background queue、
public Engine config 或數百行抽象，代表已超出本修正範圍，應停止並簡化。

## 6. 修正後驗證順序

### 6.1 靜態與建置

1. `git diff --check`。
2. Release configure/build，tests 與 benchmarks 都開啟，warnings-as-errors。
3. 確認 public headers、WAL format 與既有 default 沒有變更。

### 6.2 功能測試

1. 先跑新增的 prepare stats、byte identity、repeated shutdown 與 CLI smoke。
2. 再跑完整 Release CTest。
3. 跑 Debug + ASan/UBSan 完整測試。
4. TSan 僅在 runtime 可正常啟動時執行；若環境仍回報 runtime mapping error，記錄為環境限制，
   不將其誤報成程式通過或失敗。

### 6.3 正式效能驗收

功能測試通過後，依原設計執行 controlled benchmark：

- 同一個 Release binary、固定六 CPU affinity、每輪新空目錄。
- W=1／2／4 交錯執行，避免 storage temperature 偏向單一設定。
- writer group=4,096、direct-WAL group=4,096、writer group=256。
- 每個 case 五輪，measured phase 至少 15 秒；先用 pilot 決定 iterations，不刪除有效低吞吐輪。
- profile-off summary 的 `actual_parallel_prepare_groups` 與 `actual_prepare_tasks` 必須證明
  W=2／4 確實進入目標路徑。
- profile-on 只用於解釋 phase，先驗證 off/on median bias 不超過 5%。

正式報告至少包含 RPS、p50/p99/p99.9/max、CPU、context switches、RSS、實際 commands/group、
actual parallel groups/tasks、WAL bytes、rotations、write calls、sync distribution、replay／durable
head 與每輪 exit status。

主要 gate 維持不變：writer group=4,096 profile-off 的 W=4 五輪 median RPS 相對 W=1 至少
+10%。未達門檻就停止此方向，不在同案追加 buffer pool、lock-free queue 或更多 workers。

## 7. 完成條件

- [ ] profile-off writer 與 direct-WAL summary 都回報 measured-phase actual groups/tasks。
- [ ] cumulative stats 不啟用逐筆 clock，且 warmup 已由 delta 排除。
- [ ] W=1／fallback／parallel stats 語意有測試。
- [ ] smoke test 能拒絕 `actual_parallel_prepare_groups=0`。
- [ ] W=1／2／4 的 byte、position、size、rotation/write 與 replay 一致性有明確 assertions。
- [ ] W=4 repeated open／append／sync／close／reopen 無 hang 且 replay 正確。
- [ ] Release 與 ASan/UBSan 完整測試通過。
- [ ] 正式五輪 benchmark 報告能依原設計判定通過或停止。
- [ ] 沒有修改 staging，除非使用者另行要求。
