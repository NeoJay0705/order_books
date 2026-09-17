# 單一交易對 Durable Engine Benchmark 設計審查與必要修改

## 1. 文件目的

本文件審查 `docs/order-book-design.md` 與目前 benchmark 實作，並定義新增單一交易對端到端 RPS 測試所需的最小修改。目標是量測下列實際 command path：

```text
Engine::submit
  -> bounded ingress queue
  -> shard writer group commit
  -> WAL append
  -> fsync
  -> StateMachine apply
  -> invariant validation
  -> completion queue
  -> CompletionHandler
```

本文是實作規格，不改變 matching、durability 或 public API 語意。

## 2. 審查結論

現有設計只部分符合新需求：

- `mixed_single_instrument` 固定使用單一 instrument，但直接操作 `OrderBook`，不包含 Engine queue、WAL、group commit、fsync 或 completion。
- `durable_group_commit` 包含 WAL append 與 fsync，但直接操作 `Wal`，不包含 Engine queue、state transition、invariant validation 或 completion。
- 現有輸出已具備 command throughput 與 latency percentile 的基本格式，可以沿用。

因此必須新增一個 workload；不能把上述任一既有 workload 重新命名為端到端 RPS。除此之外，不需要修改 production code。

## 3. 需求與合理假設

### 3.1 必須滿足

- 只配置一個 shard 與一個 instrument；instrument 即一個交易對。
- 所有 measured commands 必須經過公開的 `Engine::submit()`。
- 只有收到 `CompletionHandler` 的 committed result 才計入完成數。
- 計時區間必須包含 queue wait、組批等待、WAL append、`fsync`、matching、invariant validation 與 completion dispatch。
- 報告 completed commands/second，以及每筆 command 從 submit 前至 callback 的 p50、p99、p99.9 與 max latency。
- 使用 Release build，並記錄 group-commit 設定與實際資料目錄。
- 任一 enqueue failure、非 committed result、callback 重複／遺失、Engine failure 或 stop failure，均使 benchmark 以 non-zero 結束，不得輸出看似有效的 RPS。

### 3.2 本次不納入

- 網路 ingress、serialization protocol 或 broker adapter。
- 等待 downstream EventSink durable ACK 的發布延遲。Publisher 仍由 Engine 正常執行，使用立即成功的 benchmark sink，但 completion boundary 不等待 publisher cursor。
- Snapshot、recovery、WAL retention 與 crash injection；已有或應由其他 workload 負責。
- 多 instrument、多 shard scaling 與跨 shard routing。
- 固定效能門檻。CI shared runner 只做功能 smoke test。

這些排除項目不影響本 workload 對「單一交易對 durable command RPS」的定義。

## 4. Benchmark 定義

新增 workload 名稱：

```text
engine_durable_single_instrument
```

### 4.1 Engine 設定

使用 production public types 建立 Engine：

```text
shard_ids                         = [1]
instrument_id                     = 1
assigned_shard                    = 1
tick_size                         = 1
lot_size                          = 1
group_commit_max_commands         = 256
group_commit_max_delay            = 200 microseconds
ingress_queue_capacity            = 65,536
snapshot_interval_commands        = maximum practical value
snapshot_interval                 = 24 hours
event_replay_snapshot_interval_*  = 24 hours
```

Snapshot trigger 必須在此 workload 中延後，避免把未要求的週期性 Snapshot 混入測量。使用安全的 24 小時時間值，不使用 duration 的極限值，以避免跨單位轉換溢位。不得把 `snapshot_interval_commands` 設為零，因為目前 live snapshot 判斷會使零值每批觸發。

EventSink 使用 benchmark-local `AcknowledgingSink`，收到 batch 後立即回傳 success；MetricsSink 使用現有 `NullMetricsSink`。兩者只存在 benchmark source，不加入 public library。

### 4.2 負載

command 依 submit 順序交替：

```text
Sell quantity=1 price=100
Buy  quantity=1 price=100
```

每兩筆形成一次完整成交，因此測試結束時不累積 active order。所有 command 使用相同 `instrument_id=1`，OrderId 全域唯一。

一個 workload iteration 是依序提交一筆 Sell 與一筆 Buy，也就是 2 commands、1 trade。`--iterations=N` 表示 N 組 measured pair；`--warmup=N` 表示 N 組 warmup pair。這與既有 `crossing_new` 等 workload 的 iteration 語意一致，不會對既有 CLI 增加奇偶數限制。輸出 throughput 一律以實際完成的 `2 * N` 筆 command 計算。

### 4.3 Producer lanes

同一 producer stream 遵守 single-in-flight contract：前一筆尚未 callback 前，不提交該 producer 的下一筆。為使 group commit 能形成 batch，benchmark 固定建立 1,024 個 producer lanes：

```text
producer_id        = lane index + 1
producer_epoch     = 1
producer_stream_id = 1
producer_seq       = 該 lane 已完成數 + 1
```

每個 lane 只有在 callback 後才能回到 available queue。單一 driver thread 從 available queue 取得 lane 並 submit，callback 只記錄結果、latency 並歸還 lane，不直接遞迴呼叫 `submit()`。

1,024 lanes 是 benchmark fixture，不是 production 建議值；它只需高於預設 group size，讓單一交易對在遵守 producer contract 時能維持足夠 outstanding commands。本次不增加 `--producers` 選項，避免引入需求以外的調校介面。

### 4.4 Warmup 與計時邊界

warmup 和 measured phase 使用相同 Engine，但必須分別 drain：

1. 提交 `2 * warmup` 筆 warmup commands。
2. 等待全部 warmup callbacks，確認全部 committed。
3. 清除 latency samples；確認沒有 outstanding command。
4. 在第一筆 measured command submit 前取得 `steady_clock` 起點。
5. 提交 `2 * iterations` 筆 measured commands，維持 bounded outstanding window。
6. 等待最後一筆 measured callback，立即取得終點。
7. 在計時區間外讀 metrics 並呼叫 `Engine::stop()`。

吞吐量公式：

```text
commands_per_second = committed measured commands / measured wall-clock seconds
```

單筆 latency：

```text
callback steady_clock time - 該 command 首次 submit 前的 steady_clock time
```

不得用 `SubmitResult::queued` 數量計算 durable RPS；`queued=true` 只代表進入 ingress queue。

## 5. CLI 與輸出

### 5.1 必要 CLI 修改

在既有 parser 增加兩個選項：

```text
--workload=all|engine_durable_single_instrument
--data-dir=PATH
```

- `--workload` 預設為 `all`，維持既有呼叫方式。指定新 workload 時只執行該 workload。
- `--data-dir` 未提供時使用帶 PID／run id 的 temporary directory 並於結束清除；正式數據必須明確指定位於目標 WAL 裝置的空目錄。
- 指定的資料目錄若已存在且非空，benchmark 必須拒絕執行，不得刪除使用者既有資料。
- benchmark 只能刪除自己建立的 temporary directory；明確指定的資料目錄保留，方便檢查 WAL。

本次不新增 `--group-size`、`--group-delay`、`--producers` 或 JSON output。Runtime 預設值已是本次要測的設定，增加更多調校介面會超出需求。

### 5.2 必要輸出

單行結果至少包含：

```text
engine_durable_single_instrument
iterations=<requested measured pairs>
commands=<committed measured commands>
trades=<committed trades>
commands_per_second=<value>
trades_per_second=<value>
p50_us=<value>
p99_us=<value>
p99.9_us=<value>
max_us=<value>
elapsed_ms=<value>
active_orders=0
active_levels=0
group_size=256
group_delay_us=200
fsync_mode=per_group
completion_boundary=durable_callback
instrument_count=1
shard_count=1
producer_lanes=1024
wal_path=<resolved path>
wal_bytes=<value>
```

既有 header 的 platform、compiler、build type、seed 與 CPU threads 繼續沿用。`cpu_model=unavailable` 應如實保留，除非另有獨立需求實作可攜式 CPU 探測。

## 6. 元件責任與同步方式

新增的 benchmark-local helper 責任如下：

```text
AcknowledgingSink
  立即確認 publisher batch；不模擬網路或 broker

ProducerLane
  保存 producer_id、下一個 sequence 與是否 available

DurableRunState
  保存 mutex、condition_variable、available lanes、outstanding 數、
  completed 數、trade 數、first error 與 latency samples

run_engine_durable_single_instrument
  建立 Engine、執行 warmup、執行 measured phase、驗證結果、輸出統計並 stop
```

callback 與 driver thread 共享的欄位必須受同一 mutex 保護；callback 在更新完成後通知 condition variable。等待必須有合理 timeout，timeout 視為 benchmark failure，避免 CI 永久卡住。

latency sample 的 command index 或對應 timestamp 必須由 callback capture，不可依 callback 執行時再查詢會被重用的 lane 狀態。

## 7. 錯誤處理與結果驗證

以下任一情況立即記錄第一個錯誤、停止新增 submit，drain 已接受 command，安全呼叫 `Engine::stop()`，最後回傳 non-zero：

- `Engine::open()` 失敗。
- `submit()` 回傳 `queued=false`。
- callback result 不是 `CommandStatus::committed`。
- callback 次數超過或少於已接受 command 數。
- callback identity 與預期 command 不一致。
- phase 等待 timeout。
- measured 完成數不等於 `2 * iterations`。
- 最終 `active_orders` 或 `active_price_levels` 不為零。
- Engine metrics 的 measured-phase command 增量不等於 `2 * iterations`。
- `Engine::stop()` 失敗。

錯誤輸出必須包含 workload、phase 與 machine-readable error code；失敗 run 不得輸出 throughput summary。

trade 數與最終 book gauges 取自 warmup drain 後和 measured drain 後的兩次 `Engine::metrics(1)` snapshot 差值／終值；`CommandResult` 本身不攜帶 event list，不應由 callback 猜測 trade 數。

## 8. 必要檔案修改

### `benchmarks/order_book_benchmark.cpp`

- 擴充 option parser，加入 workload filter 與 data directory。
- 新增 benchmark-local sink、producer lane、phase runner 與 durable Engine workload。
- 沿用既有 percentile 計算規則，但 samples 改為每筆 command submit-to-callback latency。
- 保留所有既有 workloads 與其輸出語意。

### `benchmarks/CMakeLists.txt`

- 將 benchmark target 連結 `order_books::runtime`。Runtime 已傳遞 core、storage 與 Threads；不新增第三方 dependency。

### `docs/order-book-design.md`

- 在 workload 清單加入 `engine_durable_single_instrument`。
- 加入本文件第 4 節的 completion boundary、單一 instrument 與 Snapshot 排除說明。
- 明確區分 `durable_group_commit` 是 storage microbenchmark，新 workload 才是 Engine durable end-to-end benchmark。

### `README.md`

- 增加一個 Linux Release 執行範例，提醒資料目錄必須位於實際 WAL 裝置。
- 說明 `commands_per_second` 以 committed completion 計算，而非 enqueue 計算。

### `tests/integration/engine_durable_single_instrument_test.cpp`

- 使用相同的單一 instrument 與 Engine public API 提交少量 Sell／Buy pair。
- 等待所有 completion，驗證 committed status、command／trade counters 與空 book。
- 這是 deterministic integration contract test，不是效能測試，也不取代 benchmark smoke。

不需要修改 `include/`、`src/domain/`、`src/persistence/` 或 `src/runtime/`；只把上述測試檔加入既有 GoogleTest target。

## 9. 測試與驗收

### 9.1 功能 smoke

CI Release benchmark 使用小型值：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --iterations=20 \
  --warmup=4
```

驗收：exit code 為零、commands 為 40、trades 為 20、active orders／levels 為零，且輸出標示 durable callback completion boundary。

### 9.2 Linux 實機測量

```bash
taskset -c 2 ./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --iterations=1000000 \
  --warmup=10000 \
  --data-dir=/mnt/local-nvme/order-books-benchmark/run-001
```

每次 run 使用新的空目錄。至少執行五次並報告中位數；不得把 macOS 開發機、container overlay filesystem 或 tmpfs 結果當成 Linux production baseline。

可另用一次非正式診斷 run 驗證實際呼叫 `fsync`：

```bash
strace -f -c -e trace=fsync \
  ./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_durable_single_instrument \
  --iterations=2000 \
  --warmup=100
```

`strace` run 只驗證系統呼叫，不作效能數據。

## 10. 關鍵取捨與已知限制

- callback 是目前最早可觀察的 durable success boundary；以它計時可防止把 enqueue throughput 誤報為 durable throughput。
- 使用多 producer lanes 是形成 group commit 的必要條件，不代表多交易對；所有 commands 仍進入同一 instrument 與 shard。
- paired crossing 維持固定 book size，使長時間結果可比較；它代表高成交單一交易對，不代表大量 resting orders 的 workload。既有 `resting_new` 等 microbenchmarks 繼續覆蓋其他形態。
- invariant validation 是 production durable path 的一部分，必須保留在數據內；本 benchmark 不提供關閉選項。
- publisher 會正常併行並產生系統資源競爭，但 completion 不等待 downstream ACK。若未來要量測 publish-confirmed latency，應新增獨立 workload，不能改變本 workload 的完成定義。
- 實測結果高度依賴 WAL 裝置與 filesystem；輸出 metadata 是可比較性的必要條件，不是 SLA。

## 11. 預估修改量

不含本設計文件，依照完整 callback／lane／timeout／failure cleanup 實作，必要修改約 500 至 620 行：

```text
benchmarks/order_book_benchmark.cpp  380-460 行
benchmarks/CMakeLists.txt               1-3 行
docs/order-book-design.md              15-25 行
README.md                              10-20 行
tests/integration/engine_durable_single_instrument_test.cpp  90-120 行
tests/CMakeLists.txt                    1 行
CI smoke command                        1-2 行
```

相較於初步估算增加的行數主要來自正確處理非同步 callback、producer single-in-flight、timeout、資料目錄安全檢查與錯誤收斂；沒有因此增加 production API、runtime 功能或新的 benchmark 維度，不應為縮短程式碼而省略這些正確性條件。

## 12. Definition of Done

- 新 workload 只使用一個 instrument 與一個 shard。
- measured command 全部經過 `Engine::submit()` 並以 committed callback 完成。
- 數據包含 queue、group commit、WAL append、真實 `fsync`、state apply、invariant validation 與 completion dispatch。
- workload 遵守 producer single-in-flight contract。
- warmup 不進入 measured throughput 或 latency samples。
- failure 不會產生有效-looking throughput summary。
- 既有 benchmark 行為保持相容。
- CI smoke 通過，Linux 實機命令與結果 metadata 已文件化。
- production API 與 runtime implementation 沒有因 benchmark 需求而改動。
