# Engine Pipeline Ceiling Benchmark staged review 必要修正

## 1. 文件目的

本文件記錄 `docs/engine-pipeline-ceiling-benchmark-design.md` 與目前 staged changes
審查後確認的必要修正。修正範圍只處理會造成量測數字錯誤、workload 名稱與實際工作不符，
或 correctness 驗證誤判的問題；不加入新的 benchmark 維度、production instrumentation、
效能優化或第三方依賴。

目前整體架構仍符合原設計：所有新增邏輯位於 benchmark／test／documentation，沒有修改
production API、matching、WAL format、durability、Publisher cursor semantics、Completion
ordering 或 runtime defaults。

## 2. 必要修正摘要

| 項目 | 問題 | 影響 | 必要性 |
| --- | --- | --- | --- |
| wall time 與 latency 語意 | 並行／非同步 sample latency 被相加作為 elapsed time | throughput、elapsed 與 target attainment 錯誤 | 必修 |
| metrics contention fixture | writer 與 Publisher worker 都執行 writer metric mix | workload 名稱不符、command-equivalent 高估 | 必修 |
| StateMachine correctness | XOR checksum 可能合法抵消為零，且 events／EngineSeq 未精確驗證 | 正確執行可能失敗，錯誤執行也可能通過 | 必修 |

除這三項外，不應在本次修正中重構 production code、加入 batching、改變 worker 行為、增加
新 CLI 選項或擴大正式壓測 matrix。

## 3. 修正一：分離 wall elapsed 與 latency samples

### 3.1 現況與根因

`benchmarks/pipeline_ceiling_benchmark.cpp` 的 `print_timing()` 目前以
`Samples::elapsed_ns()`，也就是所有 sample 的總和，計算：

- `operations_per_second`；
- `elapsed_ms`。

這只適用於 sample 彼此不重疊的同步 stage。以下兩種 sample 不能相加當 wall time：

- `runtime_handoff`：每筆 command 從 submit 到 callback 的 latency，多筆 request 同時
  in-flight；
- `writer_publisher_contended`：兩條 thread 的 group latency 在同一時間區間內重疊。

已觀察到同一行輸出中的數值互相矛盾：

```text
runtime_handoff:
operations_per_second=21640
command_equivalent_per_second=511236

writer_publisher_contended:
operations_per_second=170904
command_equivalent_per_second=329326
```

差異不是量測抖動，而是前者使用重疊 latency 總和，後者使用 wall elapsed。

### 3.2 具體修改

修改 `print_timing()`，讓呼叫端明確傳入本次 throughput 的 `elapsed_ns`，不再由 helper
自行假設 `sum(samples) == wall elapsed`：

```cpp
void print_timing(...,
                  const Samples& latency_samples,
                  std::uint64_t operations,
                  std::uint64_t elapsed_ns,
                  std::string_view completion_boundary,
                  std::string_view latency_scope,
                  ...);
```

各 stage 傳值規則：

| Stage／case | throughput 使用的 elapsed | percentile sample | `latency_scope` |
| --- | --- | --- | --- |
| `state_machine` | measured groups 的時間總和 | 每個 command group | `command_group` |
| `invariant_validation` | measured validations 的時間總和 | 每次 validation | `validation_call` |
| `metrics/writer_only` | measured groups 的時間總和 | 每個 metric group | `metric_group` |
| `metrics/writer_publisher_contended` | barrier 放行至兩條 worker join 的 wall time | 各 worker 的 metric group | `concurrent_worker_group` |
| `runtime_handoff` | 第一筆 measured submit 前至最後 callback 完成的 wall time | 每筆 submit-to-callback latency | `command_callback` |
| `publisher_drain` | Publisher start 至 stop/join 完成的 wall time | 完整 backlog drain | `full_backlog_drain` |

`operations_per_second`、`command_equivalent_per_second`、`target_attainment_percent` 與
`elapsed_ms` 必須全部使用同一個、由上表定義的 elapsed。percentile 只使用
`latency_samples`，不得再影響 throughput 分母。

原設計的共同輸出把所有 percentile 都命名為 `group_p*`，但 runtime sample 實際是 callback
latency，Publisher sample 是完整 drain latency。為避免破壞既有 parser，本次保留
`group_p50_us`／`group_p99_us`／`group_p99.9_us`／`group_max_us`，並強制輸出上述
`latency_scope`。同時在設計文件的輸出契約補充：這四個欄位表示「該行
`latency_scope` 指定的 sample」，只有 `latency_scope=command_group` 時才能解讀成 Engine
command group latency。不要在這次修正另建一套動態欄位名稱。

### 3.3 驗收條件

- `runtime_handoff` 的 `operations_per_second` 與
  `command_equivalent_per_second` 使用相同 command count 時必須一致（只容許浮點格式差異）。
- contended metrics 的 `elapsed_ms` 必須等於 measured concurrent wall time，不能是兩條
  thread latency 的總和。
- sequential stage 的 throughput 與修正前應只有正常量測抖動。
- 每一行 summary 都包含正確的 `latency_scope`。

預估修改約 25～40 行。

## 4. 修正二：使用各自的 writer／Publisher metrics mix

### 4.1 現況與根因

`writer_publisher_contended` 目前兩條 worker 都呼叫同一個
`observe_production_metric_mix()`。該函式只包含 writer-side names，因此第二條 worker
不是 Publisher workload。這與設計要求「兩個固定 worker 同時呼叫各自 production-like
mix」不一致，也使輸出的 calls/command 與 command-equivalent 缺乏正確分母。

### 4.2 固定 metric mix

將現有函式改名為 `observe_writer_metric_mix()`，保留現有 11 次 writer observation：

```text
queue_depth
queue_latency_us
wal_commit_latency_us
execution_latency_us
end_to_end_latency_us
commands
trades
wal_size_bytes
active_orders
active_instruments
active_price_levels
```

新增 benchmark-local `observe_publisher_metric_mix()`，模擬 Publisher 每筆成功 replay 的
正常路徑，共 5 次 observation：

```text
replayed_records
publish_latency_us
event_publish_lag_events
event_publish_lag_bytes
event_publish_lag_age_ns
```

不得加入只在錯誤或 retry 時出現的 `event_publish_retry`、`publisher_*_error`，因為本
ceiling case 量測正常成功路徑。

定義兩個獨立常數與 metric-name metadata：

```cpp
constexpr std::size_t kWriterMetricCallsPerCommand = 11;
constexpr std::size_t kPublisherMetricCallsPerCommand = 5;
```

### 4.3 contended case 的計數與換算

兩條 worker 使用同一個 `MetricsRegistry`、同一個 barrier 與相同 command count：

```text
writer worker     -> observe_writer_metric_mix()
publisher worker  -> observe_publisher_metric_mix()
```

原始 operations 定義為實際 registry observations：

```text
writer_observations
  = writer_commands * writer_calls_per_command

publisher_observations
  = publisher_commands * publisher_calls_per_command

aggregate_observations
  = writer_observations + publisher_observations

operations_per_second
  = aggregate_observations / concurrent_wall_time
```

一個 pipeline command 同時造成一組 writer mix 與一組 Publisher mix，因此：

```text
completed_command_pairs
  = min(writer_commands, publisher_commands)

command_equivalent_per_second
  = completed_command_pairs / concurrent_wall_time
```

不得再把 `writer_commands + publisher_commands` 當成端到端 command-equivalent；那會把
同一個邏輯 command 的兩側工作計算兩次。

`writer_only` 也應以 observations 作為原始 operations：

```text
operations_per_second = writer_observations / elapsed
command_equivalent_per_second = writer_commands / elapsed
```

summary 至少新增或調整以下 metadata：

```text
contention_mode=shared_registry_two_workers
writer_commands=<N>
publisher_commands=<N>
writer_metric_calls_per_command=11
publisher_metric_calls_per_command=5
writer_observations=<N>
publisher_observations=<N>
observations_per_second=<value>
```

`metric_names` 應拆成 `writer_metric_names` 與 `publisher_metric_names`，避免把不存在於
Publisher worker 的名稱列成共同 mix。

### 4.4 correctness 驗證

在計時外取得 `registry.snapshot()`，至少驗證：

- `snapshot.commands == warmup_writer_commands + measured_writer_commands`；
- `snapshot.replayed_records == warmup_publisher_commands + measured_publisher_commands`；
- `snapshot.publish_latency.count` 等於 Publisher 的 warmup 加 measured commands；
- 兩條 measured worker 都完成預期 command count；
- observations 的乘法與加法全部使用既有 checked arithmetic helper。

### 4.5 驗收條件

- contended summary 同時列出 writer 與 Publisher 的 command／observation 數。
- `operations_per_second` 等於 aggregate observations/s。
- `command_equivalent_per_second` 以完成的 writer+Publisher pair 計算。
- 第二條 worker 的 metric names 與 production Publisher 成功路徑一致。
- 不修改 `MetricsRegistry` 或任何 production source。

預估修改約 40～60 行。

## 5. 修正三：StateMachine 使用精確 correctness contract

### 5.1 現況與根因

StateMachine stage 目前把每筆 result 的 EngineSeq 做 XOR，最後要求 checksum 非零。XOR
合法地可能抵消為零。例如：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=state_machine \
  --iterations=1 \
  --warmup=0 \
  --pipeline-batch-size=4 \
  --pipeline-active-orders=3
```

measured EngineSeq 為 4、5、6、7，其 XOR 為零；目前會錯誤回報
`result_count_mismatch`。同時，現況只要求 `events > 0`，也沒有把最後 EngineSeq 與已知
fixture 規模比較，未完全實作設計要求。

### 5.2 具體修改

移除 checksum 參數、區域變數、輸出欄位與 `checksum == 0` 判斷。result 已透過 status、
events 與 trades 被消費，不需要另一個容易誤判的 optimizer guard。

每次 `StateMachine::apply()` 成功後立即確認：

```cpp
output.result.command_status == CommandStatus::committed
output.result.error_code == ErrorCode::none
output.result.engine_seq == submitted_engine_seq
```

計時後以 checked arithmetic 計算並驗證：

```text
measured_commands
  = iterations * batch_size

expected_trades
  = measured_commands / 2

expected_events
  = measured_commands * 2

expected_last_engine_seq
  = active_orders + (warmup + iterations) * batch_size
```

此 fixture 中每組 crossing pair 的 Sell 先 resting、Buy 再完整成交，因此每 pair 固定產生
一筆 trade，合計四個 events；也就是兩個 events/command。若未來 domain event contract
變更，必須同步更新 fixture contract，不能退回只檢查 `events > 0`。

最後仍保留並要求：

- `validate_state()` 成功；
- `active_order_count == options.active_orders`；
- `last_committed_engine_seq == expected_last_engine_seq`；
- measured trades／events 分別等於精確預期值。

### 5.3 驗收條件

- 上述 active-orders=3 的重現命令成功並輸出 `correctness_verified=true`。
- 原有 `pipeline-stage=all` smoke 成功。
- 人為破壞 event count、result EngineSeq 或最終 EngineSeq 時，stage 必須在 validation 前後
  被拒絕，不能輸出成功 summary。

預估修改約 10～20 行；移除 checksum 後淨增加行數應更少。

## 6. 必要測試順序

實作完成後依序執行：

1. Release build 並啟用 warnings-as-errors。
2. 執行完整 GoogleTest suite。
3. 執行 StateMachine XOR-zero regression command。
4. 執行小型 `pipeline-stage=metrics`，人工或腳本核對 observation 與
   command-equivalent 公式。
5. 執行小型 `pipeline-stage=runtime_handoff`，確認兩個 throughput 欄位使用相同 wall-time
   分母。
6. 執行設計文件既有的 `pipeline-stage=all` smoke。
7. 執行既有 `workload=all`、WAL ceiling 與 durable Engine options smoke，確認沒有回歸。
8. 執行 `git diff --check`。

正式長時間壓測不屬於本次 correctness 修正的驗收必要條件；上述修正與 smoke 通過並經
review 後，再重新執行正式 pipeline ceiling 測量。舊的 metrics／runtime throughput 數字
因分母或 workload 定義錯誤，不應作為優化決策依據。

## 7. 修改檔案與行數估算

必要程式修改集中於：

```text
benchmarks/pipeline_ceiling_benchmark.cpp    約 75～120 行修改
docs/engine-pipeline-ceiling-benchmark-design.md
                                             約 5～10 行澄清
```

若既有 integration test 不需因輸出欄位調整，其他 source、test、CMake、CI 與 README 都
不需要修改。總修改量預估約 80～130 行；這是修改行數，不代表淨增加行數。

## 8. 明確不做

- 不修改 `include/order_books/*`、`src/domain/*`、`src/persistence/*` 或 `src/runtime/*`。
- 不加入 Publisher batching、cursor group persistence、Completion batching 或 lock-free
  queue。
- 不新增 benchmark framework、JSON output、CPU affinity 或 performance threshold。
- 不藉本次修正調整 production group size／delay 預設值。
- 不擴大 workload 至多 instrument、多 shard、網路 sink 或外部 broker。
- 不為縮短程式碼而把正式 worker 換成簡化的 benchmark-only queue／publisher。

完成以上三項後，實作才會與設計的 stage boundary、workload 名稱、throughput 分母及
correctness contract 一致，可用於下一階段的瓶頸判讀。
