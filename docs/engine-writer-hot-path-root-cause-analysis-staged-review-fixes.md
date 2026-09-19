# Engine Writer Hot Path 根因分析 staged review 必要修正

## 1. 目的與範圍

本文件記錄 `docs/engine-writer-hot-path-root-cause-analysis-design.md` 與目前 staged changes
對照後，正式根因量測前仍需完成的最小修正。目標是讓 warmup／measured phase 隔離、writer
phase 邊界、sample storage 與 hierarchy validation 符合設計定義，避免根據受污染的數據選錯下一個
production optimization。

只修正下列四項：

1. warmup 期間停用 profiling，並在 measured phase 開始前重設 sampling phase；
2. 將 writer service／cycle 的共同終點固定在 completion enqueue 完成時；
3. 移除正式長測中 worker-side sample storage reallocation；
4. phase child 大於 parent 時讓 run 失敗，並補齊既有 WAL lock-wait 輸出。

不修改 WAL format、durability boundary、Engine ordering、group-commit defaults、StateMachine、
Publisher replay、Completion batching或public installed API，也不加入parallel prepare、queue redesign、
新的telemetry framework或其他production optimization。

## 2. 修正一：以 profiling gate 隔離 warmup 與 measured phase

### 2.1 問題

根因量測只需要 measured phase 的 samples。若 warmup 也啟用 profiling，原始 Completion callback
更新 `RunState::completed` 時，writer 或 Completion sample 可能仍未發布；此時清除 collector 會造成
phase 混入或 data race。讓 warmup 產生 samples後再建立額外 fence，還會在診斷熱路徑增加每筆
Completion 的 atomic bookkeeping。

### 2.2 最小隔離方法

使用 `ShardRuntime` 既有的 opt-in phase gate，不收集 warmup samples：

```text
open runtime with collector
  -> set_writer_profile_phase_active(false)
  -> start runtime
  -> run warmup through the production unprofiled path
  -> wait until all warmup callbacks complete
  -> reset collector
  -> reset_writer_profile_phase()
       -> reset sampled-group sequence
       -> enable measured profiling
  -> submit measured commands
```

### 2.3 phase 切換順序

profile-on warmup完成後必須依序執行：

1. `run_phase()`確認所有warmup callbacks已完成；
2. `collector.reset()`清除collector狀態；warmup期間沒有observer寫入，因此不需要等待sample；
3. `reset_writer_profile_phase()`將group序號歸零並啟用profiling；
4. 才開始提交measured commands。

measured phase結束後仍先呼叫`ShardRuntime::stop()`並join workers，再讀取完整samples；不以callback
完成本身取代stop/join。這也維持主設計的collector ownership規則。

`warmup=0`時使用相同切換順序即可。不得重新加入未被phase切換使用的progress counter、polling wait或
sleep；它們不能提供額外正確性，且會污染profile-on成本。

## 3. 修正二：固定 writer parent phase 的共同終點

### 3.1 問題

目前`completion_enqueue_ns`在`dispatch_results()`返回後結束，但`writer_service_ns`與
`writer_cycle_ns`是在建立`WriterGroupProfile`途中分別再次取clock。結果會把`batch_finished`、
sample建構及部分欄位填值算進unattributed cost，而且兩個parent phase不是使用同一終點。

這與設計中的定義不一致：

```text
writer_service = admission start -> completion enqueue complete
writer_cycle   = first dequeue   -> completion enqueue complete
```

### 3.2 修改方法

在`ShardRuntime::process_command_batch()`中，`dispatch_results()`返回後立即取得唯一的
`writer_end`：

```cpp
dispatch_results();
const auto writer_end = profile_ != nullptr
                            ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
const auto completion_enqueue_ns =
    profile_ != nullptr
        ? steady_elapsed_ns(completion_enqueue_start, writer_end)
        : 0U;
batch_finished = true;
```

建立sample時只使用該時間點：

```cpp
profile.writer_service_ns = steady_elapsed_ns(service_start, writer_end);
profile.writer_cycle_ns = steady_elapsed_ns(group_start, writer_end);
```

不得在parent duration中再取clock，也不得把`observe_writer_group()`本身納入service／cycle。這項
修改不增加profile-off clock call，profile-on反而少取兩次clock。

## 4. 修正三：避免正式量測期間重新配置 sample storage

### 4.1 問題

Completion samples目前最多只預留2,000,000筆。設計要求每個正式measured phase至少15秒；以目前
約162K commands/s估算會產生約243萬筆Completion samples，因此worker必然在量測期間重新配置
vector。大型allocation與copy會污染callback service、queue residence、tail latency及profile bias。

Writer group samples目前只用來求總和，保存每一筆group並不是輸出契約所必需；若實際group小於
設定上限，也可能超過用`measured_commands / group_size`估算的reserve。

### 4.2 Completion storage

在開啟runtime前：

1. 依measured commands、group size與sampling interval計算measured phase的sample capacity；warmup
   profiling停用，不需要為warmup samples預留空間；
2. 檢查它可由`std::size_t`表示；不能表示時回報
   `error_code=writer_profile_sample_capacity_overflow`；
3. profile-on時一次reserve完整的Completion sample capacity；
4. reserve失敗維持既有`writer_profile_reserve_failed`，不得啟動worker；
5. 移除任意的2,000,000筆上限。

capacity在runtime啟動前完成配置，measured phase不應主動縮減或重新建立collector storage。

### 4.3 Writer samples

`print_profile()`只使用writer samples的累計值與group count，不計算per-group percentile。因此採用
下列最小且有界的方法：

- `ProfileCollector`保存一份writer-owned aggregate與`profiled_groups`，不保存
  `std::vector<WriterGroupProfile>`；
- `observe_writer_group()`使用checked／saturating aggregation累加所有欄位；
- overflow時設定`invalid_`，不得繼續輸出可採信的phase share；
- runner仍只在stop/join後讀取aggregate；
- `reset()`清空aggregate、group count與Completion vector size，但保留Completion vector capacity；

這比為最壞情況預留`measured_commands`個大型`WriterGroupProfile`更節省記憶體，也避免為實際
group size建立新的估算規則。不要為此引入通用histogram或第三方benchmark library。

## 5. 修正四：拒絕無效的 phase hierarchy

### 5.1 問題

目前以下情況都以remainder為零繼續輸出成功summary：

```text
writer children > writer_service
group_collect + writer_service > writer_cycle
payload_encode + crc + frame_assembly > wal_prepare
wal_chunk_copy > wal_plan_copy
```

這會隱藏phase overlap、錯誤計時邊界或counter overflow。根因分析工具不能以clamp偽造一個看似
合法的零remainder。

### 5.2 修改方法

在`print_profile()`計算任何remainder前：

1. 使用checked addition計算`writer_children`、`cycle_children`與`prepare_children`；
2. 加法overflow時回報`error_code=profile_counter_overflow`；
3. 若任一child total大於parent，回報
   `error_code=profile_hierarchy_invalid`並返回false；
4. 只有驗證成功後才直接執行parent減child，不再以條件運算clamp為零。

輸出同時補上已經收集但目前未回報的欄位：

```text
wal_lock_wait_ns
wal_lock_wait_ns_per_command
```

它們使用和其他WAL phase相同的accepted-command denominator。這只補齊既有coarse WAL phase，
不新增量測點或新的phase。

## 6. 最小必要測試

### 6.1 Warmup／measured 隔離

強化profile-on benchmark smoke：

- warmup command數明顯大於measured command數；
- 成功輸出時`profiled_input_commands`、`profiled_accepted_commands`與`completion_count`只能等於
  measured commands；
- 輸出包含`wal_lock_wait_ns`；
- 不比對任何實際duration或RPS。

此測試鎖定phase隔離的外部契約；同步正確性由warmup profiling gate、phase切換順序與measured結束後
的stop/join保證，不使用sleep猜測worker是否完成。

### 6.2 Parent／child 邊界

擴充`EngineWriterProfileTest`，對每個durable group驗證：

```text
admission + wal_append + wal_sync + apply + publisher_notify
  + post_apply + completion_enqueue <= writer_service

group_collect + writer_service <= writer_cycle

payload_encode + crc + frame_assembly <= wal.prepare
chunk_copy <= wal.plan_copy
```

測試不得要求任一duration大於零，避免低解析度clock平台出現flaky failure。既有callback throw案例
繼續驗證Completion sample exactly once與exception isolation。

### 6.3 Profile-off 不變條件

保留既有profile-off smoke，並確認輸出沒有profile-only summary。Production `Engine::open()`仍使用
null collector；本次修正不得增加profile-off clock、atomic或sample storage操作。

## 7. 驗證順序

修正完成後依序執行：

1. `git diff --check`與`git diff --cached --check`；
2. C++20 Release warnings-as-errors build；
3. 全部GoogleTest與benchmark CLI／smoke CTest；
4. ASan／UBSan correctness run；
5. profile-on使用非零warmup重複執行，確認count不混入warmup；
6. profile off/on各做短測，確認輸出契約與replay correctness；
7. correctness完成後才執行主設計要求的五輪、至少15秒正式矩陣與bias分析。

本修正不需要先做正式壓測；但正式根因報告必須在修正完成後重新量測，不能沿用含上述race、
bookkeeping與reallocation污染的profile-on數據。

## 8. 修改規模與非目標

預估必要修改：

- warmup profiling gate與phase reset：約10～20行；
- writer共同計時終點：約5～10行；
- bounded collector storage／aggregation：約20～35行；
- hierarchy validation與缺漏輸出：約10～20行；
- 回歸測試與CTest契約：約20～30行；
- 合計約65～115行。

若修正明顯超過此範圍，應先檢查是否順帶加入了通用profiling framework、production metrics、
parallel prepare、Completion batching或其他未經本次根因證據支持的優化；這些都超出本文件範圍。
