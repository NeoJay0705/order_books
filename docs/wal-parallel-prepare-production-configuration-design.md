# WAL parallel prepare production configuration 設計

## 1. Review 結論

本設計承接 `wal-bounded-parallel-prepare-design.md` 與
`wal-bounded-parallel-prepare-benchmark-report.md`。Prototype 在 writer group=4,096 時，
W=2／W=4 相對 W=1 的五輪 median throughput 分別提升 14.62%／21.96%，WAL bytes、ordering、
replay 與 shutdown correctness 均通過；但 group=256 的提升只有 2.50%／4.03%，Direct WAL
的 W=2 median（496,622 commands/s）高於 W=4（469,582 commands/s），且 W=4 有明顯 storage
tail 與變異。

因此目前足以進入 **production configuration**，但不足以把平行 prepare 預設開啟，也不足以
宣稱達到 1M commands/s。本案只讓既有、已驗證的 bounded parallel prepare 可以由 public
`RuntimeConfig` 明確 opt-in，補齊執行路徑與 fsync 的必要可觀測性，並定義驗證及 rollback。

經 review，以下內容是讓 prototype 成為可安全部署選項的必要修改：

1. public runtime configuration 提供 lane count 與啟用門檻，預設仍為 W=1；
2. production Engine 與 benchmark 使用同一份設定來源，避免兩條路徑認知不一致；
3. 暴露實際 parallel groups／tasks 與 group sync latency，確認設定有生效並辨識 storage tail；
4. 文件化每 shard thread 成本、啟用條件及 restart rollback；
5. 以 public Engine path 驗證設定傳遞、正確性與效能。

本案不修改 WAL algorithm、durability boundary、group-commit policy、StateMachine、Publisher、
Completion worker 或 fsync policy，也不新增自動調參、hot reload、通用 executor 或自動 rollback。
這些變更不是把設定安全公開所必需，納入反而會使效能歸因與 rollback 邊界不清楚。

## 2. 需求理解與合理假設

### 2.1 必須滿足

- 未設定新欄位的既有程式維持 W=1，不建立 WAL prepare 背景 thread，行為與資源用量不變。
- production path 僅接受 1、2、4 lanes；lane count 包含持有 WAL mutex 的 caller。
- 平行化只在實際 accepted commands 數量大於等於 threshold 時啟用。
- production Engine、internal writer benchmark 與 direct-WAL benchmark 使用相同的
  `WalPrepareOptions` 語意。
- RuntimeConfig 只影響執行資源與 timing，不影響 WAL bytes、logical output 或 replay，因此
  不寫入 WAL／Snapshot，也不需要 migration。
- 公開 metrics 能回答：配置為多少、實際有多少 group 使用平行 prepare、建立多少 tasks，以及
  WAL group 的 sync latency distribution。
- rollback 不依賴資料轉換；停止 Engine、改回 W=1 並用相同資料目錄重啟即可。

### 2.2 合理假設

- 現有 `Wal::PrepareWorkers`、partition、ordered merge、錯誤處理與 shutdown 契約已由 prototype
  correctness tests 驗證，本案不重新實作。
- 目前只有 group=256 與 group=4,096 的 controlled evidence，不能宣稱已找到精確 crossover；
  4,096 是保守且有實證的初始 production threshold。
- W=2 是第一個 production candidate；W=4 保留為明確 opt-in 的進階選項，不是建議預設。
- 專案目前沒有 runtime config reload protocol，因此配置只在 `Engine::open()` 生效。
- Queue depth、end-to-end latency、publisher lag bytes/events/age 與 publisher storage pressure 已有
  metrics/backpressure；本案只補目前缺少的 prepare-path 與 sync 可觀測性。

## 3. 明確不在範圍內

- 不修改 WAL record／segment format、CRC、rotation、write、publish、sync 或 recovery。
- 不改 per-group `fsync`、RPO=0、group-commit max commands／delay 的既有預設。
- 不將 W=2 或 W=4 設為 library default，也不依 `hardware_concurrency()` 自動選擇 lanes。
- 不新增跨 shard shared pool、work stealing、thread affinity、scheduler priority 或 NUMA policy。
- 不提供 runtime hot reload、自動升降 lanes、adaptive threshold 或自動 rollback。
- 不新增新的 prepare queue、backpressure 狀態或 admission error；現有 bounded one-job workers 與
  ingress／publisher pressure 契約不變。
- 不優化 `StateMachine::apply`、Publisher、Completion、metrics contention 或其他 writer phase。
- 不以本案調整 group size 來提高吞吐；group policy 必須以獨立設計處理。
- 不建立 CI 固定 RPS gate；效能 gate 只在 controlled Linux benchmark 執行。

## 4. Public configuration 與單一設定來源

在 `RuntimeConfig` 增加：

```cpp
std::size_t wal_prepare_lanes{1};
std::size_t wal_parallel_prepare_min_commands{4096};
```

語意：

- `wal_prepare_lanes` 只接受 1、2、4，包含 caller；每 shard 的額外背景 thread 數為
  `wal_prepare_lanes - 1`。
- `wal_parallel_prepare_min_commands` 必須大於 0；只有 lanes > 1 且當次 accepted command count
  達到門檻時才走 parallel path。
- 預設 W=1，確保升級後不會在使用者未同意 CPU 成本時改變 production 行為。
- 第一個建議試行配置為 W=2、threshold=4,096。這是 deployment 建議，不是 API default。
- W=4 只能在目標硬體重新通過吞吐、tail latency 與 CPU gate 後使用。

總額外 thread budget 可直接計算為：

```text
number_of_shards * (wal_prepare_lanes - 1)
```

不另加重複的 `thread_budget` 欄位，也不以硬體核心數做不可靠的隱式拒絕。部署者必須在設定審查
中同時記錄 shard 數、可用 CPU affinity 與上述 thread 數；若資源不足則使用 W=1。

### 4.1 Validation

`Engine::open()` 在建立任何 shard/thread 前驗證：

- lanes 不是 1、2、4：回傳 `invalid_command`；
- threshold 為 0：回傳 `invalid_command`。

錯誤訊息應分別指出 invalid lane count 或 zero threshold，不能沿用模糊的
`runtime limits must be positive`。`Wal::open()` 保留相同 validation 作為 internal safety boundary。

### 4.2 Runtime 傳遞

`Engine::open()` 仍只把 `EngineConfig` 傳給 `ShardRuntime::open()`；`ShardRuntime::open()` 從
`config.runtime` 建立 internal `storage::WalPrepareOptions` 並傳給 `Wal::open()`。

移除 `ShardRuntime::open()` 額外的 `WalPrepareOptions` 參數。Internal writer benchmark 改寫
`EngineConfig.runtime`，不再從旁路注入 options。Direct-WAL benchmark 仍可直接建立 internal
`WalPrepareOptions`，因為它不經過 Engine。

這使 public Engine、writer benchmark 與 production code 對 lanes/threshold 只有一份來源。

## 5. 資料流與模組邊界

```text
EngineConfig.runtime
  wal_prepare_lanes
  wal_parallel_prepare_min_commands
          |
          v
Engine::open validation
          |
          v
ShardRuntime::open
          |
          `-- WalPrepareOptions --> Wal::open --> existing PrepareWorkers

writer group
  -> accepted commands
  -> existing sequential/parallel prepare selection
  -> append/rotation/write/publish
  -> fsync
  -> observe actual prepare counters + sync latency
  -> existing apply/publisher/completion flow
```

Public configuration 不穿透到 `PrepareWorkers` 以外的模組。Prepare workers 不取得 MetricsSink、
EngineConfig、Publisher 或 StateMachine；所有 production metrics 仍由 shard writer thread 發出。

## 6. 必要可觀測性

### 6.1 `MetricsSnapshot`

增加以下欄位：

```cpp
std::uint64_t wal_prepare_lanes{};                    // gauge
std::uint64_t wal_parallel_prepare_min_commands{};    // gauge
std::uint64_t wal_parallel_prepare_groups{};           // cumulative counter
std::uint64_t wal_prepare_tasks{};                     // cumulative counter
HistogramSnapshot wal_sync_latency;                   // one sample per synced group
```

metric names 為：

```text
wal_prepare_lanes
wal_parallel_prepare_min_commands
wal_parallel_prepare_groups
wal_prepare_tasks
wal_sync_latency_us
```

lanes 與 threshold 是 gauges；parallel groups 與 tasks 是 saturating cumulative counters；sync
latency 是 histogram。`wal_prepare_tasks` 沿用現有 `WalPrepareStats::tasks` 語意，包含 sequential
fallback 的單一 task，不能解讀為背景 thread 數。

### 6.2 收集位置與成本

- Shard open 時各寫入一次 configured lanes 與 threshold gauges。
- 每次成功 append 後，writer 讀取一次 cumulative `WalPrepareStats`，與上次 snapshot 做 checked
  delta，再把 delta 寫入 registry。這些 counters 在 WAL lifetime 內只會 saturating increase；
  若讀值意外倒退則跳過該次 metrics delta，不得影響已成功的 command。
- `wal_sync_latency_us` 從 `Wal::sync()` 呼叫前後量一次，每 group 只 observe 一筆，不逐 command
  複製相同 sample。
- 現有 `wal_commit_latency_us` 語意與相容性不在本案修改。

不得在 prepare worker 內直接呼叫 MetricsSink，也不得新增 per-command clock。新增觀測是 per-group
固定成本；實作後仍須以 profile-off benchmark 確認沒有實質 regression。

### 6.3 現有 metrics 的使用方式

外部監控應將以下既有指標做 time-series，而非新增重複狀態：

- `queue_depth`、`queue_latency`、`end_to_end_latency`；
- `event_publish_lag_events/bytes/age_ns`；
- `wal_commit_latency` 與新增的 `wal_sync_latency`。

`wal_sync_latency` 用來區分 fsync/storage tail；`wal_commit_latency` 仍代表 prepare+append+sync 的
durable group latency。Publisher lag/backpressure 的既有 threshold 與行為保持不變。

## 7. 啟用、驗收與 rollback

### 7.1 建議 rollout

1. 先以完全相同 workload、storage、affinity 與 group policy 建立 W=1 baseline。
2. Canary 只改成 W=2、threshold=4,096；不得同時修改 group size、delay 或 fsync policy。
3. 確認 `wal_parallel_prepare_groups` 增加，否則只能判定門檻未被觸發，不能宣稱 W=2 無效。
4. 比較至少五輪 median throughput、p50/p99/p99.9/max、CPU、context switches、queue、publisher
   lag 與 sync latency。
5. W=4 只有在 W=2 通過且仍有 CPU 餘裕時作獨立實驗，不是 rollout 的必要步驟。

### 7.2 Production acceptance gate

- 所有 correctness、replay、shutdown、Release、Debug、ASan/UBSan tests 通過。
- W=2 的 public Engine durable path 五輪 median throughput 相對 W=1 至少提升 10%。
- 多數配對輪次的 p99 不得穩定退化超過 10%；所有真實 p99.9/max tail 都保留並報告。
- Queue depth 與 publisher lag 在固定輸入率下不得跨觀測窗口持續成長。
- Sync p99/p99.9/max 必須與 throughput 同時報告；storage tail 不得被歸因成 prepare CPU regression。
- CPU 與 context-switch 增量必須落在部署預先記錄的 per-Engine budget。

### 7.3 Rollback

以下任一情況成立即停止 canary，以正常 shutdown 關閉 Engine，將 lanes 改回 1 後從相同資料目錄
重啟：

- correctness、replay、shutdown 或 sanitizer failure；
- 多數窗口 throughput 未達 baseline，或 p99 持續退化超過 10%；
- queue/publisher lag 持續成長或觸發既有 storage pressure；
- CPU/thread budget 超出部署限制。

因 WAL bytes 與 recovery format 不變，rollback 不需轉換或刪除資料。Fail-stop I/O error 仍沿用
既有 Engine 行為；本案不加入 process 內 live rollback，以免在 active WAL workers 間引入新的
生命週期與 ordering 風險。

## 8. 錯誤處理

- Config error 在任何 shard 開啟前失敗，不可只開啟部分 shards。
- Worker 建立失敗仍由 `Wal::open()` 回傳 `wal_failure`，已建立 worker 以 RAII join。
- Prepare、append 或 sync error 的 fail-stop/durability 語意完全沿用既有實作。
- Metrics exporter exception 仍由 `MetricsRegistry` 隔離，不得使 writer 或 WAL 失敗。
- Metrics counter delta 必須先比較再相減，不得 unsigned underflow；異常 metrics sample 不得使
  command、writer 或 shard 失敗。

## 9. 測試策略

### 9.1 Unit／integration

- `engine_test.cpp`：public config 接受 1/2/4，拒絕其他 lanes 與 zero threshold，且在開啟任何
  shard 前失敗。
- `engine_durable_single_instrument_test.cpp`：W=2、threshold 小於 group 時實際增加 parallel
  groups/tasks；threshold 大於 group 時保持 fallback；completion、replay 與 durable head 正確。
- `metrics_registry_test.cpp`：新 gauges、saturating counters、sync histogram snapshot 與未知 metric
  行為。
- 既有 `persistence_test.cpp` 繼續負責 byte identity、partition、rotation、replay 與 worker shutdown；
  不在 Engine test 重複底層矩陣。
- default config 測試確認 W=1 的 parallel groups 為 0，且沒有改變既有 Engine 行為。

一般 CI 不斷言 clock value 必須非零，也不以固定 RPS 判斷 pass/fail。

### 9.2 Benchmark

- `engine_durable_single_instrument` 允許既有 WAL prepare CLI options，並把它們寫入 public
  `RuntimeConfig`；summary 必須輸出 configured lanes/threshold 與 actual groups/tasks。
- `wal_sync_full_run_p99_us` 是 Engine 啟動後累積 histogram，包含 warmup；throughput、group
  counters 與 actual prepare groups/tasks 仍只計 measured phase。
- Internal writer benchmark 同樣改用 RuntimeConfig，但保留既有輸出 key，避免歷史報告無法比較。
- Direct-WAL benchmark 繼續直接使用 `WalPrepareOptions`，不假裝經過 public Engine path。
- controlled matrix 至少包含 public Engine W=1/W=2 group=4,096、W=2 threshold fallback，以及
  W=4 diagnostic；每 case 五輪、每輪新 data directory、至少 15 秒、保留慢輪。

### 9.3 Regression

Release、Debug、ASan/UBSan 全量測試必須通過；TSan 在環境可用時執行，但已知環境無法啟動時
要明確記錄，不能宣稱通過。實作後必須重新跑 controlled benchmark，因 public config 傳遞與
per-group metrics 是新的 production hot-path 變更。

## 10. 檔案層級實作指引

```text
include/order_books/engine.hpp
  新增兩個 RuntimeConfig 欄位，預設 W=1 / threshold=4096

include/order_books/metrics.hpp
  新增 prepare gauges/counters 與 wal_sync_latency snapshot

src/runtime/engine.cpp
  public config validation；Engine 仍只傳 EngineConfig

src/runtime/shard_runtime.hpp/.cpp
  從 RuntimeConfig 建立 WalPrepareOptions；移除旁路參數；收集 per-group delta 與 sync latency

src/runtime/metrics_registry.cpp
  註冊新 metric、分類 gauge/counter/histogram、填入 MetricsSnapshot

benchmarks/engine_writer_profile_benchmark.cpp
benchmarks/order_book_benchmark.cpp
benchmarks/CMakeLists.txt
  writer/public Engine CLI 設定傳遞與 summary/negative smoke

tests/unit/metrics_registry_test.cpp
tests/integration/engine_test.cpp
tests/integration/engine_durable_single_instrument_test.cpp
  configuration、metrics、parallel/fallback 與 correctness tests

docs/order-book-design.md
README.md
  RuntimeConfig、CPU thread formula、opt-in/rollback 與 benchmark 使用說明
```

不需要修改 WAL format、`StateMachine`、EventPublisher、Completion worker、Snapshot schema、Conan／
CMake dependency 或新增 source module。

預估必要變更約 180--300 行：production headers/runtime 70--120 行、tests 70--120 行、benchmark
與文件 40--60 行。若明顯超出，應先檢查是否誤納 hot reload、adaptive policy、shared executor 或
其他本案排除內容。

## 11. 關鍵決策與取捨

### 預設 W=1，而不是直接採 W=2

Prototype 證明特定硬體與大 group 有收益，但尚未涵蓋不同 shard 數、CPU affinity、storage 與
workload。Opt-in 保留相容性；W=2 是有證據支持的 rollout candidate，不是普遍安全的預設。

### threshold=4,096，而不是猜測 crossover

256 已證明收益不足，4,096 已證明有效，中間點尚未測量。選 4,096 可避免把未驗證的小 batch
納入 production path；日後可由獨立 benchmark 調低，不需要改 API。

### per-shard lanes，而不是共享 executor

現有 WAL ordering 與 worker lifecycle 已通過 correctness；共享 executor 會引入跨 shard queue、
fairness、shutdown 與 head-of-line blocking。當前只需要把既有能力安全公開。

### restart rollback，而不是 live reconfiguration

RuntimeConfig 本來就是 restart-time operational config。Restart rollback 不改 WAL bytes，邊界清楚；
live resize workers 需要新的 generation/lifetime protocol，風險與需求不成比例。

### 補 sync histogram，而不是重做 tracing

報告已出現真實 fsync/storage tail，只有 aggregate WAL latency無法區分 prepare 與 sync。每 group
一筆 sync sample 足以支援 production判斷，不需要在本案加入 tracing framework。

## 12. 已知限制與後續方向

- 本設計不提高 Direct WAL ceiling；目前最佳 median 約 496.6K commands/s，距離 1M 仍有差距。
- 本設計不改善 `StateMachine::apply`；在 W=4 profile 中它約占 writer service 29%。
- Histogram 是 process 內 bounded approximation；完整 time-series 與 alert retention 由
  `MetricsSink` exporter 負責。
- threshold=4,096 可能因正常流量無法聚滿而很少觸發；這是可觀察的保守結果，不應在同案偷偷
  降低 threshold 或提高 group delay。
- 若後續在多 shard 部署發現 dedicated workers 浪費 CPU，應另案比較 shared pool；不能在本案
  預先加入未驗證抽象。
- 下一個獨立效能工作應處理 WAL durable ceiling 或 `StateMachine::apply`，不得把它們混入本次
  configuration change。

## 13. 設計與實作一致性清單

- [ ] RuntimeConfig 只有 lanes 與 threshold 兩個新 knobs，預設仍為 W=1。
- [ ] lanes 只接受 1/2/4，threshold 不接受 0，且在建立 shard 前完成 validation。
- [ ] ShardRuntime 不再接受第二份旁路 WalPrepareOptions。
- [ ] public Engine 與 writer benchmark 使用相同 RuntimeConfig 傳遞路徑。
- [ ] Direct-WAL benchmark 保持 internal options，不冒充 Engine path。
- [ ] WAL bytes、ordering、durability、replay 與 worker implementation 未改變。
- [ ] metrics 能看見 configured lanes/threshold、actual groups/tasks 與 group sync latency。
- [ ] 新增 metrics 只有 per-group 固定成本，沒有 per-command clock 或 worker callback。
- [ ] 現有 publisher pressure 與 ingress backpressure 沒有重複實作。
- [ ] W=2/threshold=4096 只是 rollout 建議；library default 與 rollback 都是 W=1。
- [ ] rollback 只需正常停止、改設定、以原資料重啟，不需 migration。
- [ ] 測試覆蓋 public config、parallel/fallback、metrics、replay 與 shutdown。
- [ ] controlled benchmark 使用 public Engine path，保留慢輪並完整報告 tail。
- [ ] 沒有順帶修改 group policy、fsync、StateMachine、Publisher、Completion 或加入 executor。
