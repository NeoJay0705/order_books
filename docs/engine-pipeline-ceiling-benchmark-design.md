# Engine Pipeline Ceiling Benchmark 設計審查與必要修改

## 1. 文件目的

本文件審查目前單一交易對效能量測能力，並定義下一階段為找出百萬
commands/s 路徑瓶頸所需的最小修改。量測範圍涵蓋：

```text
Engine::submit
  -> ingress enqueue / shard dequeue / group formation
  -> admission + prepare
  -> WAL append + group sync
  -> StateMachine::apply
  -> invariant validation
  -> publisher notification
  -> completion enqueue / Completion worker callback

Publisher worker
  -> WAL next_after
  -> publisher replica StateMachine::apply
  -> EventSink::publish
  -> lag accounting
  -> durable cursor persistence
```

本階段只建立可重現的 ceiling、latency 與 backlog 診斷能力，不修改 production
matching、durability、WAL format、Publisher cursor 語意、Completion ordering 或預設
group-commit 設定。取得數據後，效能不足的元件才另案設計優化。

## 2. 審查結論

現有設計只部分符合需求：

- `wal_write_ceiling` 已能分離 WAL append 與 group `fsync`，並量測 group-size matrix。
- `engine_durable_single_instrument` 已涵蓋公開 `Engine::submit()` 到 durable callback，
  但只能看到整體吞吐與 completion latency，無法判斷 state execution、validation、
  Publisher 或 Completion worker 的獨立上限。
- 既有 `crossing_new` 等 workload 直接操作 `OrderBook`，不能代表
  `StateMachine::apply`，因為它不包含 ProducerState、event/result construction、
  tombstone 與 shard state 更新。
- 目前 durable workload 不等待 Publisher cursor 追上 durable WAL head；短時間 RPS
  可能只是累積 publisher backlog，不能證明可持續吞吐。
- Publisher 目前逐 command replay、publish 並 durable persist cursor；Completion worker
  目前逐 command dequeue 與 callback。兩者都需要以真實 worker 驗證，不可用單執行緒
  `for` loop 取代。
- `validate_state()` 會掃描 active orders、order-location index、producer states 與
  tombstones。現有 crossing fixture 的 active book 維持為空，不能代表大型單一交易對的
  validation 成本。
- `MetricsRegistry::observe()` 位於 writer 與 Publisher 熱路徑，包含 mutex、字串/map
  lookup 與 downstream callback，也必須列入診斷。

因此必要修改是新增一組 `engine_pipeline_ceiling` 診斷 workload，並讓既有 durable
Engine workload 可設定 group size 與 max delay。這些 workload 沿用正式元件，不新增
test-only production hook，也不在量測階段預先實作 batching 或 lock-free 結構。

## 3. 需求理解與合理假設

### 3.1 要回答的問題

1. `StateMachine::apply` 在與 durable benchmark 相同的 single-instrument crossing
   workload 下，上限與每 command CPU service time是多少？
2. state size 增長時，完整 invariant validation 是否成為 group 熱路徑瓶頸？
3. ingress、admission 與 Completion worker 在不含 WAL／apply 時，可持續處理多少
   callbacks/s？
4. Publisher 在既有逐筆 cursor durability 語意下，能否持續追上 durable WAL head？
5. metrics 更新及 writer／Publisher 競爭是否占用不可忽略的 CPU？
6. 在不同 group size 與 max delay 下，WAL durable throughput 和端到端 completion
   latency 的 Pareto frontier 為何？
7. 下一階段應優先優化 WAL、state execution、validation、metrics、Completion 還是
   Publisher？

### 3.2 百萬目標的判讀方式

單 shard writer 上的階段是串行成本，不能只要求每個 isolated component 剛好達到
1,000,000 commands/s：

```text
writer service time per command
  = admission + append + sync amortization + apply
  + validation amortization + result/metrics + completion enqueue

writer ceiling
  = 1 / total service time per command
```

一百萬 commands/s 代表 writer 全部串行成本合計不得超過約 1 microsecond/command。
若一個元件單獨為 1M/s、另一個為 5M/s，兩者串行後理論上限只有約 833k/s。因此
isolated ceiling 用於成本分解，不得直接相加或把單一元件達標宣稱為端到端達標。

Publisher 與 Completion worker 雖與 writer 並行，但各自 sustained ceiling 必須高於
writer arrival rate，且長測期間 queue depth／publisher lag 不得持續增加。短 burst 完成
不等於穩態可持續。

### 3.3 Latency 與 batch 的未決需求

目前需求只要求記錄 p50／p99／p99.9，沒有定義可接受上限。因此本階段產出 throughput
與 latency Pareto frontier，保留 production 預設 `256 commands / 200 microseconds`，
不擅自選出新的預設值。

只有取得明確 completion latency SLO 後，才能從 frontier 選擇「可接受延遲下最大的
batch」。報告可以指出非支配組合，但不得把主觀數值當成驗收門檻。

## 4. 範圍與非目標

### 4.1 納入範圍

- 單一 shard、單一 instrument、單一 writer。
- 正式 `StateMachine::apply()` 與 `validate_state()`。
- 正式 `MetricsRegistry`，包含單 writer 與 writer／Publisher 競爭情境。
- 經公開 `Engine` API 的 ingress、admission、completion queue 與 Completion worker。
- 正式 `EventPublisher`、WAL reader、replica state machine、立即成功 sink、lag accounting
  與 durable cursor persistence。
- 既有 WAL ceiling 與 durable Engine workload 的 group-size／delay matrix。
- throughput、group latency、callback latency、publisher drain time、correctness counters
  與 backlog 是否收斂。
- Release Linux 實機重複量測，以及獨立的 `perf`／`strace`／`iostat` 診斷 run。

### 4.2 明確不納入

- 修改 WAL write、sync policy、codec、CRC 或磁碟格式。
- Publisher event batching、cursor group persistence 或 EventSink API 變更。
- Completion queue batching、lock-free queue 或 thread affinity production config。
- 增量 invariant、抽樣 invariant 或降低 correctness 檢查。
- 網路 ingress、真實 broker、外部 database 或多 shard scaling。
- 新增第三方 benchmark framework或依賴。
- 在 CI 設定 RPS threshold。
- 將 profiler 插樁 run 的 throughput 當成正常 baseline。

Publisher 或 Completion ceiling 若未達目標，本文件只要求提出下一階段設計輸入，不在
同一變更中同時量測並重寫實作。

## 5. 量測架構與邊界

新增 workload selection：

```text
--workload=engine_pipeline_ceiling
```

並以 benchmark-only stage selector 選擇：

```text
--pipeline-stage=all
--pipeline-stage=state_machine
--pipeline-stage=invariant_validation
--pipeline-stage=metrics
--pipeline-stage=runtime_handoff
--pipeline-stage=publisher_drain
```

`all` 只使用預設參數執行 smoke／單點診斷；正式 matrix 由外部腳本分次執行單一 stage，
避免不同 stage 的 cache、filesystem 與 background thread 互相污染。

### 5.1 哪些可使用 `for` loop

下列為同步、純 CPU 或明確受控元件，可使用 cache-warm loop：

- `StateMachine::apply`；
- `validate_state`；
- `MetricsRegistry::observe` 的固定 production-like metric mix。

每個 timing sample 包住一個 command group，不在每筆 command 前後讀 clock。輸出是 group
latency percentile；`group_time / commands` 只能標為平均 service time，不得標成單筆 p99。
測試必須消費 result/event counters 並在計時外驗證最終 state，防止 optimizer 移除工作。

### 5.2 哪些必須使用真實 worker

下列成本依賴 mutex、condition variable、thread scheduling、I/O 或 backpressure，必須驅動
正式 worker：

- ingress enqueue／shard dequeue／group formation；
- Completion queue／Completion worker／callback；
- EventPublisher replay／publish／cursor persistence；
- WAL append／sync。

不得複製一份簡化 queue 或 cursor writer 當成 production ceiling。若現有 API 無法將內部
子步驟完全分開，本階段量測完整 worker，再用 direct component ceiling 與外部 profiler
歸因；不為 benchmark 抽出新的 production abstraction。

## 6. Stage Workload 定義

### 6.1 `state_machine`

直接建立正式 `domain::StateMachine`，fixture 與 durable Engine workload 使用相同：

```text
shard_id       = 1
instrument_id  = 1
tick_size      = 1
lot_size       = 1
command shape  = Sell price=100 quantity=1
                 Buy  price=100 quantity=1
```

每兩筆 command 形成一筆 trade，measured phase 前可在不交叉的價位預先建立
`--pipeline-active-orders=N` 筆 resting orders，使 matching workload 保持相同，同時觀察
較大 state 對 lookup、tombstone 與 ProducerState 的影響。Fixture construction 不計時。

必須：

- EngineSeq 嚴格連續；
- producer sequence 合法且 single stream state 不無限增長；
- OrderId 唯一；
- tombstone retention 使用明確的 bounded fixture 值並輸出；
- warmup 後清除 samples，但延續同一合法 state；
- 計時後呼叫 `validate_state()`；
- 驗證 measured commands、trades、events、最後 EngineSeq 與預期 active orders。

此 stage 包含完整 `StateMachine::apply` 的 result/event construction，不包含 WAL、runtime
queue、metrics、Publisher 或 Completion。

### 6.2 `invariant_validation`

在計時外建立合法且固定不變的 `ShardState`，active orders 由
`--pipeline-active-orders=N` 指定。每個 measured operation 只呼叫一次正式
`validate_state(state)`。

輸出至少包含：

```text
active_orders
active_price_levels
producer_states
tombstones
validations_per_second
validation_group_p50/p99/p99.9/max
amortized_commands_per_second
```

`amortized_commands_per_second` 只表示「每次 validation 若攤提到指定
`--pipeline-batch-size`」的診斷值，不包含 apply 或其他 writer 成本，輸出必須明確標記
`completion_boundary=invariant_return`。

正式 matrix 至少量測 active orders：

```text
0, 1,000, 10,000, 100,000
```

不得在 measured loop 重建 fixture，也不得跳過每次回傳狀態檢查。

### 6.3 `metrics`

直接使用正式 `runtime::MetricsRegistry` 與 `NullMetricsSink`，執行目前 writer 每筆成功
command 的固定 metric mix；metric 名稱與呼叫次數必須列入輸出或報告，避免未來 production
mix 改變後仍誤用舊 baseline。

同一 stage 執行兩個 case：

1. `writer_only`：單執行緒量測固定 mix。
2. `writer_publisher_contended`：兩個固定 worker 同時呼叫各自 production-like mix，使用
   同一 registry，報告 aggregate throughput 與各 worker完成數。

這是 registry ceiling，不代表完整 command throughput。輸出必須同時提供 observations/s
與依該固定 mix 換算的 command-equivalent/s，且標明換算假設。`writer_only` 使用 writer
mix；`writer_publisher_contended` 的兩個 worker 必須分別使用 writer mix 與 Publisher 成功
replay mix，command-equivalent 以兩側完成 command pair 計算，不得將兩側 command 數相加。

### 6.4 `runtime_handoff`

使用公開 `Engine::submit()`、正式 shard writer 與正式 Completion worker。Command fixture
使用可預期的 admission rejection，使 shard dequeue、batch admission、result ordering、
completion enqueue/dequeue 與 callback 都會執行，但不寫 WAL、不 apply state，也不通知
Publisher。

沿用 durable Engine workload 的 1,024 個 producer lanes；每個 lane 維持 single-in-flight，
callback 後才歸還 available queue。這讓 writer 能形成 batch，同時不違反 producer contract。

此 workload 的完成邊界是收到預期的 admission-error callback；不得把它稱為 business
command throughput 或 durable throughput。它回答的是 runtime handoff 與 Completion
worker 的合併 ceiling。

必須驗證：

- 每個 queued request 恰好一個 callback；
- callback identity 與 request 相同；
- status／error code 符合 fixture；
- callback 順序符合既有 producer stream contract；
- 沒有 queue rejection、timeout 或 Engine failure；
- warmup 完全 drain 後才開始 measured phase；
- measured end time 是最後一個 callback，不是最後一次 enqueue。

本階段不為取得「Completion worker 單獨數字」拆出 production dispatcher。若此合併 ceiling
接近或低於目標，再以 `perf` call stacks 和 queue wait 判斷是否需要下一份 dispatcher
設計。

### 6.5 `publisher_drain`

在計時外準備 durable WAL backlog 與對應的合法 live state，使用與 durable Engine 相同的
single-instrument crossing commands。接著以正式 `EventPublisher::open()` 建立從 genesis
開始的 publisher replica，使用：

- 正式 WAL sequential reader；
- 正式 publisher `StateMachine::apply`；
- benchmark-local immediate-success sink；
- 正式 `MetricsRegistry` 搭配 `NullMetricsSink`；
- 延後的 replay Snapshot trigger，避免 Snapshot 混入本 stage；
- 正式逐筆 cursor persistence。

Measured flow：

```text
prebuilt durable WAL backlog
  -> start publisher
  -> notify durable WAL head
  -> wait confirmed cursor reaches head
  -> stop and join publisher
  -> reopen publisher metadata
  -> verify durable cursor equals head
```

計時終點必須包含最後 cursor persistence 完成；不能只看到 in-memory
`confirmed_cursor` 就結束。實作者應在 confirmed cursor 到達 head 後呼叫 `stop()` 並等待
join，再以 reopen 驗證 durable cursor。WAL fixture construction、live-state preparation與
reopen correctness validation不計入 drain throughput。

Immediate sink 必須記錄 call count、EngineSeq continuity 與 event count。任一 gap、duplicate、
publisher failure、timeout 或 cursor mismatch 都使 run 失敗。

Publisher warmup 使用獨立 temporary subdirectory 與獨立 backlog，不能讓 warmup durable
cursor 使 measured backlog 被跳過。使用者指定 data root 時，只建立明確命名的子目錄，
不得刪除 root 或既有內容。

此 stage 量測現有逐筆 Publisher。它不新增 publisher batch 參數，因為 production 尚無該
語意。若結果不足，下一階段才設計 cursor batch size／max interval，並同時定義 crash 後
最大 duplicate replay window。

## 7. WAL 與 Engine Batch/Latency Matrix

### 7.1 WAL

沿用既有 `wal_write_ceiling`，不建立第二套 WAL loop。正式 matrix：

```text
sync=none:      group size 256
sync=per_group: group size 64, 128, 256, 512, 1024, 2048, 4096
```

每組記錄 append、sync、group total latency、commands/s、MiB/s、rotation 與 replay
correctness。

### 7.2 Durable Engine

擴充既有 `engine_durable_single_instrument`，加入：

```text
--engine-group-size=N
--engine-group-delay-us=N
```

只設定 benchmark 建立的 `RuntimeConfig`，不改 production defaults。正式 matrix：

```text
group size: 64, 128, 256, 512, 1024, 2048, 4096
max delay:  50, 100, 200, 500, 1000 microseconds
```

不需要一開始執行全部 35 個組合。先固定 delay=200 掃 group size，再對 throughput/latency
frontier 上的 2～3 個 group size 掃 delay。這能回答需求並避免沒有資訊增益的測試量。

每個正式組合至少五次，報告 throughput median、worst p99、p99.9、max；結果必須包含
configured group size/delay。現有 API 無法無侵入取得實際 batch-size distribution，因此
本階段不為此增加 production instrumentation；可用獨立 `strace` 的 fsync count估算平均
commands/group，但該次 throughput 不與正常 run 混用。

## 8. CLI 與資料目錄

新增選項：

```text
--pipeline-stage=<stage>
--pipeline-batch-size=N
--pipeline-active-orders=N
--engine-group-size=N
--engine-group-delay-us=N
```

規則：

- `--pipeline-batch-size` 預設 256，必須大於零；用於 CPU stage 的 group timing 與
  invariant amortization。
- `--pipeline-active-orders` 預設 0，允許零。
- `--engine-group-size` 預設 256，必須大於零。
- `--engine-group-delay-us` 預設 200，允許零。
- `--iterations` 必須大於零，`--warmup` 允許零；pipeline CPU stage 將它解讀為 groups，
  總 operations 為 groups × pipeline batch size，並檢查 overflow。
- `--data-dir` 沿用現有安全規則。Publisher stage 將其視為空的 benchmark root，分別建立
  warmup／measured 子目錄；未指定時使用唯一 temporary root 並由 workload 清理。
- 使用者指定的路徑必須為空目錄；不得覆寫、清空或遞迴刪除使用者資料。
- pipeline-specific 選項若搭配不相關 workload，parser 必須拒絕或明確忽略並報錯，不能
  悄悄產生誤解。

不新增 duration、JSON、CPU affinity 或 profiler options；這些由外部 runner 管理。

## 9. 輸出契約

成功結果維持單行 `key=value`，共同欄位至少包含：

```text
engine_pipeline_ceiling
stage=<name>
case=<name>
iterations=<N>
warmup=<N>
operations=<N>
batch_size=<N>
operations_per_second=<value>
target_commands_per_second=1000000
group_p50_us=<value>
group_p99_us=<value>
group_p99.9_us=<value>
group_max_us=<value>
elapsed_ms=<value>
completion_boundary=<stage-specific boundary>
latency_scope=<sample meaning>
correctness_verified=true
```

`latency_scope` 定義 percentile sample 的語意。`command_group`、`metric_group` 使用同步
group sample；`validation_call` 使用單次 invariant validation sample；`command_callback` 使用
submit-to-callback sample；`concurrent_worker_group` 使用並行 worker 的 group sample；
`full_backlog_drain` 使用完整 Publisher drain sample。
`operations_per_second`、`elapsed_ms` 與 command-equivalent rate 一律使用該 stage 的
measured wall elapsed；不能將重疊的 callback 或 worker latency sample 相加當作 wall time。

只有能明確換算 command service capacity 的 stage 才輸出
`command_equivalent_per_second` 與 `target_attainment_percent`。Invariant 的換算以指定 batch
size 攤提，metrics 的換算以輸出中列出的 calls/command 為準；原始
`operations_per_second` 仍須保留，避免把假設值誤當直接量測。

Stage-specific 欄位：

- state machine：commands、trades、events、active orders／levels、tombstone limit、最後
  EngineSeq；
- invariant：state cardinalities、validations/s、amortized commands/s；
- metrics：writer/publisher observations、metric calls/command、contention mode；
- runtime handoff：queued、callbacks、expected admission errors、producer lanes、configured
  group size/delay；
- publisher：WAL commands/bytes、sink calls/events、confirmed head、durable cursor verified、
  backlog drain commands/s。

失敗時輸出：

```text
workload=engine_pipeline_ceiling
stage=<name>
phase=<setup|warmup|measured|validation|shutdown>
error_code=<machine-readable-code>
detail=<text>
```

失敗 run 不得先輸出有效-looking throughput summary。

## 10. 錯誤處理與正確性

所有 stage 必須：

- 使用 `steady_clock` 計時；
- 檢查 arithmetic overflow；
- 有 bounded timeout，避免 CI 或實機永久卡住；
- 保存第一個 failure，停止產生新工作，安全 drain／stop 已啟動的 worker；
- 只有 correctness validation 成功後才輸出 summary；
- benchmark 自己建立的 temporary directory 才能清理；
- 明確指定的 data directory即使失敗也保留供診斷。

Sanitizer run 用來驗證記憶體與 undefined behavior，不產生可比較的效能數字。

## 11. 必要檔案修改

### `benchmarks/pipeline_ceiling_benchmark.hpp`（新增）

- 定義 benchmark-only stage enum、options 與 runner entry point。
- 不放置 production model 或 public API。

### `benchmarks/pipeline_ceiling_benchmark.cpp`（新增）

- 實作五個 stage、fixture、統計、timeout、正確性驗證與安全目錄 ownership。
- 直接呼叫正式 domain/runtime/storage 元件，不複製 production algorithm。
- helpers 保持本 executable 私有，不建立可被 production link 的 library。

### `benchmarks/order_book_benchmark.cpp`

- 擴充 workload enum、CLI parser、usage 與 dispatch。
- 將 pipeline options 傳給新 runner。
- 讓既有 durable Engine workload使用可設定的 group size／delay。
- 不重寫既有 WAL ceiling、durable driver 或一般 OrderBook workloads。

### `benchmarks/CMakeLists.txt`

- 將新增的 `.cpp` 加入既有 `order_books_benchmark` target。
- 不新增 executable、third-party dependency 或 production compile definition。

### `docs/order-book-design.md`

- 在 benchmark 清單加入 `engine_pipeline_ceiling`。
- 說明 component ceiling、parallel worker sustained ceiling 與 end-to-end throughput 不得
  混為同一數字。
- 記錄 durable Engine 的 benchmark-only group size／delay options。

### `README.md`

- 增加最小 smoke 與 Linux 實機 stage/matrix 範例。
- 說明正式數據需 Release、CPU pinning、相同 WAL device，且至少五次。

### `.github/workflows/ci.yml`

- 在既有 benchmark smoke 中，以極小數量執行 pipeline `all` 或逐 stage smoke。
- 不加入 performance threshold。

### 不修改

- `include/order_books/*`
- `src/domain/*`
- `src/persistence/*`
- `src/runtime/*`
- WAL／Snapshot format
- Conan dependencies

若實作發現必須修改 production source 才能取得某個獨立數字，先保留較粗粒度的真實
worker ceiling並記錄限制，不可直接增加 test hook或改變行為。這是維持本階段必要範圍的
主要護欄。

## 12. 測試策略

### 12.1 自動化 correctness

先執行既有完整 GoogleTest suite、Release build及 ASan／UBSan。因本階段不修改
production source，既有 unit／integration tests 繼續負責 domain、persistence、Engine、
Publisher 與 Completion contract；不新增重複 production test case。

新增 benchmark smoke：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=all \
  --iterations=2 \
  --warmup=1 \
  --pipeline-batch-size=4 \
  --pipeline-active-orders=8
```

另以小型參數驗證 durable Engine 新 options：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --iterations=20 \
  --warmup=4 \
  --engine-group-size=8 \
  --engine-group-delay-us=100
```

Smoke 只檢查 exit 0、各 stage `correctness_verified=true`、durable workload完成數與
輸出 metadata，不比較 RPS。

### 12.2 Linux 正式測量

- Release `-O2` 或專案 Release preset；不得使用 sanitizer executable。
- 固定 CPU affinity，記錄 governor、CPU、kernel、filesystem、device、compiler 與 source
  revision。
- 每組至少五次，使用中位數與 worst p99；warmup 不納入 samples。
- CPU stage 的 measured phase應維持數秒以上；Publisher／WAL／end-to-end 使用足夠資料量
  觀察穩態，而非只有數千 commands 的 burst。
- Publisher run 必須在結束前確認 lag 回到零且 durable cursor=head。
- 每次 filesystem workload 使用新的空目錄。

外部診斷命令分開執行：

```bash
perf stat -e cycles,instructions,cache-misses,context-switches,cpu-migrations -- <command>
perf record -g -- <command>
strace -f -c -e trace=openat,write,close,fsync,fdatasync,rename -- <command>
iostat -xz 1
```

Profiler run 只用於 attribution，不與未插樁 baseline 計算 uplift。

## 13. 結果判讀與下一步門檻

### 13.1 Writer 串行階段

- 以 batch/group service time 換算 amortized ns/command，估算各階段占 writer budget比例。
- 若 `StateMachine::apply` ceiling 顯著高於 WAL，且 amortized cost不是 writer top
  contributor，下一步優先 WAL；不能因 apply「快於 WAL」就完全忽略 validation／metrics。
- 若 validation 隨 state size 線性增長並在合理 active-order規模占主要成本，下一步先設計
  不降低 correctness 的 incremental invariant策略。
- 若 metrics 成本顯著，下一步才評估 typed counters、thread ownership或批次 exporter，
  不直接刪除觀測。

### 13.2 Parallel workers

- Completion callback rate低於 writer arrival rate，或 completion queue造成 writer阻塞時，
  才評估 batch drain／queue implementation。
- Publisher drain rate低於 writer durable rate，或長測 lag單調增加時，系統不能宣稱該
  writer throughput可持續。
- 若 Publisher 的 state apply／reader ceiling足夠，但完整 Publisher ceiling很低，且
  syscall trace顯示逐筆 cursor sync為主，下一階段才設計 cursor group persistence。

Cursor batching設計必須同時定義：

- batch size；
- max persist interval；
- confirmed cursor與durable cursor的語意；
- crash後最大 duplicate replay commands/events；
- retention watermark只能依 durable cursor或 replay Snapshot前進；
- stop與failure時的 final flush。

不能只用 throughput挑 batch size而忽略 crash correctness。

### 13.3 建議的容量餘裕

1,000,000 commands/s是端到端目標，不是每個元件的安全 ceiling。報告應標示各元件相對
目標的 headroom；是否要求例如 20%或 30%餘裕，需由後續 SLA決策，暫不設 hard gate。

## 14. 關鍵設計決策與取捨

| 決策 | 選擇 | 理由 |
| --- | --- | --- |
| Benchmark target | 沿用 `order_books_benchmark` | 保持 build／Release流程一致，不新增 executable |
| Source organization | 新增 benchmark-only `.hpp/.cpp` | 避免繼續膨脹既有單一 source，又不形成 production library |
| CPU stage timing | 每 group計時 | 避免每 command clock污染百萬級路徑 |
| Queue／worker timing | 正式多執行緒元件 | 保留 lock、wake-up、scheduler與backpressure成本 |
| Completion isolation | admission-only完整 handoff | 不為 benchmark拆出 private dispatcher；先量真實合併 ceiling |
| Publisher isolation | 預建 durable backlog後 drain | 排除 writer速度，仍保留完整 publisher語意與cursor I/O |
| Batch選擇 | 產出 Pareto frontier | 未有 latency SLO，不能合理改 production default |
| Instrumentation | 不改 production hot path | 避免為量測新增永久 clock／observer成本 |
| Correctness | 計時外完整驗證 | 快但錯誤或未 durable的結果不得發布 |
| CI | correctness smoke only | shared runner不適合效能門檻 |

## 15. 已知限制與擴充方向

- `runtime_handoff` 是 ingress、admission與Completion合併 ceiling，不能精確分配每個 mutex
  的百分比；需要時以 profiler決定是否值得新增更細設計。
- Publisher使用 immediate sink，只量本機publisher與cursor persistence，不代表真實broker
  latency或availability。
- CPU microbenchmark為cache-warm理想上限；端到端 workload才包含實際 cache競爭。
- fixed crossing command不代表所有payload或multi-match event fan-out；本階段刻意對齊現有
  durable benchmark，避免建立不必要的 workload matrix。
- CPU未固定、背景負載不同或WAL device不同的數據不可直接比較。
- Publisher cursor batching、incremental invariant與Completion batching都可能是後續方向，
  但必須由本階段數據觸發。

## 16. 預估必要修改量

不含本設計文件，預估約 700～1,000 行：

```text
benchmarks/pipeline_ceiling_benchmark.hpp       35～60 行
benchmarks/pipeline_ceiling_benchmark.cpp      520～760 行
benchmarks/order_book_benchmark.cpp             70～110 行
benchmarks/CMakeLists.txt                         1～3 行
docs/order-book-design.md                        20～35 行
README.md                                        15～25 行
.github/workflows/ci.yml                          3～8 行
```

程式量主要來自五個不同 completion boundary、合法 fixture、非同步 timeout／cleanup、
directory ownership與correctness validation。若實作估算顯著超過此範圍，應先檢查是否
加入了production optimization、通用framework或需求外維度。

## 17. 設計與實作一致性規則

- 實作者以本文件定義的 stage boundary與輸出語意為準。
- component ceiling、runtime handoff、publisher drain、WAL durable與Engine end-to-end數字
  必須分開命名，不得互相替代。
- 不得用 enqueue count、append return或in-memory confirmed cursor冒充durable completion。
- 不因benchmark需要修改production API或關閉correctness檢查。
- 不在本階段實作任何被測元件的效能修正。
- 若實作發現設計假設不成立，先記錄原始設計、實際問題、最小調整與影響，再更新本文件；
  不得在程式中形成未記錄的第二套語意。

## 18. Definition of Done

- `engine_pipeline_ceiling`可單獨選取五個stage或執行小型`all` smoke。
- StateMachine、invariant與metrics使用正式實作並有防optimizer與correctness驗證。
- Runtime handoff經公開Engine與正式Completion worker完成，所有request恰好一次callback。
- Publisher以預建durable backlog驅動正式worker，結束時lag為零且durable cursor驗證等於head。
- WAL ceiling不重複實作；durable Engine可設定benchmark-only group size與delay。
- 所有latency與completion boundary名稱正確，失敗run不輸出有效-looking summary。
- 明確指定的data directory不被覆寫或刪除。
- 既有GoogleTest、Release smoke與ASan／UBSan通過。
- CI只驗證harness與correctness，不加入硬體相依效能threshold。
- production source、public API、durability、ordering與預設runtime config均未改變。
