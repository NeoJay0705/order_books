# Engine Writer Throughput Optimization 設計審查與必要修改

## 1. 文件目的

本文件承接以下已完成的根因分析與壓測：

- `engine-pipeline-root-cause-analysis-report.md`；
- `wal-append-hot-path-optimization-benchmark-report.md`；
- `publisher-cursor-group-persistence-benchmark-report.md`。

目標是在 Publisher 與 Completion 以 backlog／latency SLO 驗收，而不要求每個 worker 都具備
1,000,000 commands/s instantaneous ceiling 的前提下，定義提高單一 shard、單一 instrument
durable writer throughput 所需的最小 production 修改與量測方法。

本階段只處理三個已有證據支持的項目：

1. 建立 group-commit batch size／max delay 的 throughput-latency Pareto frontier；
2. 將每個 durable group 的完整 state scan 改為 command transition 的 incremental invariant
   validation；
3. 分離 command runtime 與 Publisher 的 metrics registry ownership，移除兩個 worker 對同一把
   registry mutex 的競爭。

本文件同時是實作契約。若實作需要 async WAL、WAL format 變更、lock-free queue 或新的
durability boundary，必須先另案更新設計，不得在本階段順帶加入。

## 2. Review 結論

整體方向符合需求，但必須作以下收斂才能讓設計與實作認知一致：

- 提高 group size／delay 是必要實驗，不是預先決定的 production default。選擇必須同時滿足
  durable completion latency SLO 與 correctness；不得只取最高 RPS。
- `group_commit_max_delay` 的現有語意是「取得第一筆 command 後，queue 暫時沒有下一筆時，
  最多等待多久」，不是 command 的 end-to-end latency 上限。queue 已有 backlog 時可直接收集至
  `group_commit_max_commands`。本階段保留此語意，不把它改成每次 dequeue 都讀 clock 的硬 deadline。
- full-state validator 不能直接刪除。它保留在 startup／recovery、Snapshot 邊界、測試及診斷；
  hot path 改為驗證本次 transition 的 postconditions。
- Metrics 先只做有 controlled benchmark 支持的 ownership split。typed metric ID、lock-free
  histogram、background exporter 與通用 metrics framework 都不是本階段必要內容。
- 現有數據只能證明 WAL 整體與 shared metrics／full validation 是瓶頸；尚不足以選擇 CRC、codec、
  direct I/O、`io_uring` 或新的 WAL cache representation。本階段不得猜測並實作這些修改。

因此本設計內容皆直接對應已量測瓶頸，沒有包含與單一交易對 durable writer throughput 無關的
業務功能或架構擴張。

## 3. 需求理解與合理假設

### 3.1 目標

- 長期方向仍是單一交易對 1,000,000 durable commands/s。
- Writer completion boundary 不變：WAL batch append 與 `fsync` 成功、commands 依 EngineSeq
  apply、transition validation 成功、result 移交 Completion queue。
- RPO=0 語意不變：成功 completion 不得早於 WAL durable boundary。
- Publisher 與 Completion 可以非同步消化 backlog，但必須分別滿足 queue／lag SLO；本階段不以
  它們的 instantaneous RPS 作 writer 優化 gate。
- 正式效能結論只適用於報告中記錄的 CPU、compiler、filesystem 與 WAL device。

### 3.2 初始參考 SLO

需求尚未提供正式 durable completion latency SLO。本階段採以下 balanced profile 作為選擇
Pareto point 的暫定工程門檻：

```text
group idle-fill max delay       = 1 ms
durable completion p99          <= 20 ms
durable completion p99.9        <= 50 ms
```

這些值是此專案同步 local-WAL durability 架構的初始量測門檻，不宣稱為交易產業通用標準，也不在
shared CI 作固定 performance gate。若產品需求選擇不同 SLO，只需重跑相同矩陣並重新選擇 Pareto
point；不得改變 durability 或 ordering 語意來達標。

### 3.3 現有證據

| 元件／case | 已量測 ceiling | 判讀 |
|---|---:|---|
| `StateMachine::apply` smoke | 約 1.21M commands/s | 目前不是第一瓶頸，但需將 incremental checks 計入後重測 |
| WAL append-only，group 256 | 約 668.6K commands/s | 即使沒有顯式 group sync 仍低於目標 |
| WAL durable，group 256 | 約 125.8K commands/s | production-size group 的主要限制 |
| WAL durable，group 1024 | 約 295.0K commands/s | 尚未形成 batch-size plateau |
| invariant，10K active orders | 約 251.7K amortized commands/s | full scan 隨 state size 放大 |
| invariant，100K active orders | 約 11.6K amortized commands/s | 不可留在每個 group 的 hot path |
| metrics separate registries | writer 約 1.70M commands/s | ownership split 已有足夠 headroom 證據 |
| metrics shared registry | writer 約 434.8K commands/s | shared registry contention 是 supported cause |

## 4. 範圍

### 4.1 In scope

- `ShardRuntime` group commit 的既有 size／delay benchmark matrix 與必要觀測值；
- StateMachine command transition 的 incremental postcondition checks；
- full-state validation 的 lifecycle boundary 調整；
- command-runtime metrics 與 Publisher metrics 的 registry partition；
- `MetricsSnapshot` 合併及 WAL group count observability；
- unit、integration、determinism、sanitizer 與 Release benchmark。

### 4.2 Out of scope

- WAL on-disk format、record framing、checksum coverage或recovery規則；
- async／background WAL writer、`io_uring`、direct I/O、memory mapping；
- 改變 apply-before／after-fsync 順序；
- lock-free ingress／Completion queue；
- Publisher cursor、EventSink 或 Completion callback 語意；
- Snapshot format、retention、routing 或多 shard 平行化；
- typed metrics public API、Prometheus／OpenTelemetry exporter；
- 直接把 production default 改為量測矩陣中的最大 group。

## 5. 架構與資料流

Production 資料流保持：

```text
Engine::submit
  -> bounded ingress queue
  -> ShardRuntime writer
       -> collect group by max commands / idle-fill delay
       -> admission and producer sequencing
       -> Wal::append_batch
            -> encode + CRC per command
            -> contiguous chunk construction
            -> write_all per touched segment
            -> in-memory WAL index update
       -> Wal::sync
       -> StateMachine::apply per command
            -> incremental transition postcondition validation
       -> notify Publisher durable head
       -> command-runtime metrics
       -> bounded Completion queue

EventPublisher worker
  -> WAL replay + replica StateMachine::apply
  -> EventSink ACK
  -> cursor group persistence
  -> publisher metrics partition

Engine::metrics(shard)
  -> snapshot command-runtime partition
  -> snapshot publisher partition
  -> merge by fixed metric ownership
```

Writer 與 Publisher 仍使用相同 deterministic `StateMachine::apply`。Incremental validation
因此同時保護 live execution、recovery replay 與 Publisher replica，不建立第二套 business logic。

## 6. Group Commit Pareto 設計

### 6.1 現有語意

取得第一筆 command 後：

- queue 已有 commands：立即收集，直到 `group_commit_max_commands`；
- queue 暫時為空：最多等待 `group_commit_max_delay`；
- 遇到 query、stop、failure 或 timeout：結束目前 group；
- admission 後的 accepted commands 才進入同一次 `append_batch`／`sync`。

`group_commit_max_delay` 只限制為湊 batch 而主動等待的時間。完整 command latency 仍包括 ingress
queue wait、group assembly、WAL append／sync、state execution、validation 與 Completion dispatch。

### 6.2 必要 observability

為避免把 configured maximum 誤報為實際 group size，新增兩個 fixed-name 累計 metric：

```text
wal_group_commits          # 成功 fsync 的 durable groups
wal_group_commands         # 這些 groups 內的 accepted commands
```

兩者在 `Wal::sync()` 成功後、state apply 前由 writer 更新。失敗 sync 不得計入。Benchmark 以前後
snapshot delta 計算：

```text
actual_commands_per_group = delta(wal_group_commands) / delta(wal_group_commits)
```

這是 additive public observability；不改既有欄位語意，不加入 per-command timestamp 或高基數 label。

### 6.3 正式矩陣

`engine_durable_single_instrument` 新增 benchmark-only：

```text
--engine-producer-lanes=N
```

預設維持現有 `1024`；必須大於零且不得超過 ingress capacity。它只改變 benchmark producer
lanes，不改 production config。每個 lane 仍遵守 single-in-flight，callback 前不得送出同 stream
下一筆 command。

先以 sustained backlog、`max_delay=1 ms`、producer lanes 至少等於最大候選 group 的條件掃描：

```text
group max commands = 256, 512, 1024, 2048, 4096, 8192
```

找出 throughput 開始平台化或首次違反參考 SLO 的相鄰兩到三個 size 後，只對這些候選，以
producer lanes `1／8／64／256／1024／candidate group size` 補測：

```text
max delay = 200 us, 500 us, 1 ms, 2 ms, 5 ms
```

不得直接執行沒有資訊增益的完整 Cartesian product。每個正式 case：

- 固定 CPU affinity、filesystem、device、compiler 與 Release flags；
- 至少五輪且 measured phase 至少 10 秒；
- 使用新的 data directory；
- 回報 median throughput、range、p50／p99／p99.9／max；
- 回報 configured group、actual commands/group、WAL MiB/s、rotation count；
- 驗證 completion identity/count、WAL reopen/replay 與 durable head；
- Publisher backlog 必須在測試清理前依其既有測試契約處理，不得混入 writer completion latency。

producer-lane sweep 用來形成 underfilled 到 saturated group，而不新增 wall-clock rate limiter；
sustained backlog 的結果不能代表低 concurrency latency。CLI 的 zero、非數字與大於 ingress
capacity 必須回傳明確錯誤且不開始測試。

### 6.4 Pareto 選擇規則

候選必須先通過 correctness 與參考 latency SLO，再從通過者中選擇 median durable throughput 最高者。
若相鄰較大 group 的 throughput uplift 小於 5%，選較小 group，避免用顯著 latency／memory 成本換取
量測噪音範圍內的收益。

本階段只產出建議設定與報告，不修改 `RuntimeConfig` 的 `256／200 us` production defaults。
修改預設值需由報告確認 Pareto point，並在後續小型設定變更中完成。

## 7. Incremental Invariant Validation

### 7.1 問題

現有 writer 在每個 durable group apply 完成後呼叫 `validate_state()`。該函式掃描所有 config、
books、active orders、order location index、producer states 與 tombstones，並建立 temporary sets。
成本與整個 shard state 大小相關，而不是與本批修改量相關。

### 7.2 設計原則

`StateMachine::apply` 是所有 state mutation 的唯一入口。每個 command 已取得
`OrderBookApplyResult`，其中包含 target、maker updates、terminal orders 與 active delta；因此
在同一入口驗證 touched entities，無需建立通用 transaction framework或保存額外 durable metadata。

Incremental checks 在 `apply` 成功返回前完成，至少包含：

- EngineSeq、Producer identity、result identity 與 updated ProducerState 一致；
- touched active order 的 status、price、quantity、version、priority 與 quantity conservation；
- touched order 在 `OrderBook` 與 `order_locations` 的存在性／InstrumentId 一致；
- terminal order 已從 book／location 移除，且對應 tombstone 與 tombstone-order entry 一致；
- `active_order_count` 等於 transition 前 count 加上 checked `active_delta`；
- selected instrument／behavior configuration 存在且與 command version 一致；
- `last_committed_engine_seq`、logical retention time 與 output EventID ordering 正確；
- tombstone eviction 只移除符合 age／count policy 的 prefix。

檢查成本必須是 `O(touched orders + generated events + evicted tombstones)`，不得掃描所有 active
orders、producer states 或 tombstones。不得新增 probabilistic hash 來取代精確 postconditions。

實作可在 `StateMachine` 內使用 private transition summary／helper；不得把 internal validation
metadata 加入 public `CommandResult`、Event、WAL 或 Snapshot format。

### 7.3 Full validation 保留邊界

既有 `validate_state(const ShardState&)` 保留且語意不變，只從每個 live group 的 hot path 移除。
它必須在以下邊界執行：

1. state Snapshot 載入與 WAL recovery 全部完成後；
2. Publisher replay base／cursor recovery 完成後；
3. live state Snapshot 寫入前；
4. Publisher replay Snapshot 寫入前；
5. unit、property、determinism 與 benchmark correctness verification 結束時。

Snapshot 前 validation 失敗必須 fail-stop，且不得寫出新的 Snapshot。這補足移除 per-group full
scan 後的 durable checkpoint 防線。

### 7.4 錯誤處理

Incremental postcondition failure 表示 deterministic core state 不可信，沿用既有 fatal invariant
行為：回傳 typed core error、目前 shard／Publisher fail-stop，不發布後續 event，也不對尚未完成的
request 回報成功。WAL 已 durable 的 command 由 restart replay 再次驗證；不得 rollback 或跳過。

Expected business rejection 不是 invariant failure，仍產生既有 durable rejected result／event。

## 8. Metrics Ownership

### 8.1 問題

Writer、Completion 與 Publisher 目前共用一個 `MetricsRegistry`。每次 `observe()` 都作 metric-name
分類、取得同一把 mutex並更新 map／histogram。Controlled benchmark 顯示 shared registry 使 writer
command-equivalent median 從約 1.70M/s 降至約 435K/s。

### 8.2 最小 ownership split

每個 shard 建立兩個既有 `MetricsRegistry` instance：

```text
command_runtime_metrics
  owner paths: shard writer + rare Completion error observation

publisher_metrics
  owner path: EventPublisher worker
```

`EventPublisher` 改持有 `publisher_metrics`；`ShardRuntime` 的 command／queue／WAL／execution／
warning metrics 寫入 `command_runtime_metrics`。兩個 registry 仍使用既有 fixed metric names、fixed
buckets、exception boundary 與 external `MetricsSink` port。

為消除重複 gauge ownership：

- `event_publish_lag_events／bytes／age_ns` 只由 Publisher partition 寫入；
- writer admission 仍直接讀 Publisher lag 作 storage-pressure 判斷，但只記錄
  `publisher_lag_warning／critical` counters，不再重寫 lag gauges；
- command、trade、queue、WAL、execution 與 active-state metrics 由 command partition 擁有；
- replay、publish、cursor 與 Publisher error metrics 由 Publisher partition 擁有。

### 8.3 Snapshot 合併

`ShardRuntime::metrics()` 依固定順序取得兩個 snapshot，再依 metric ownership 合併成現有 public
`MetricsSnapshot`：

- owner 唯一的 counter／gauge／histogram直接取 owner value；
- `wal_group_commits／wal_group_commands` 取 command partition；
- 不使用「兩個 gauge 取 max／最後寫入」等模糊規則；
- snapshot failure 不得影響 command processing。

本階段不改 `MetricsSink::observe(string_view, uint64_t)` public API，也不導入 typed IDs、atomic
histogram、background exporter或 lock-free aggregation。Separate-registry benchmark 已證明這個最小
切分具備超過 1M/s 的 component headroom；只有修改後的完整 writer profile 再次證明 metric-name
lookup／單 owner lock 是瓶頸時，才另案優化。

## 9. 主要介面與元件責任

| 元件 | 必要修改／責任 |
|---|---|
| `domain::StateMachine` | 在每次 mutation 成功返回前驗證 touched transition postconditions |
| `domain::validate_state` | 保持 full-state validator；移至 recovery／Snapshot／test boundaries |
| `runtime::ShardRuntime` | 移除每 group full scan；記錄成功 durable group；持有 command metrics partition；合併 metrics snapshots |
| `runtime::EventPublisher` | 使用 Publisher metrics partition；Snapshot 前執行 full validation |
| `runtime::MetricsRegistry` | 維持既有實作；每 partition 各自同步，不新增 framework |
| `MetricsSnapshot` | additive 新增 WAL group totals，既有欄位語意不變 |
| `engine_durable_single_instrument` | 新增可調 producer lanes；回報 snapshot delta 與 actual commands/group；執行 Pareto matrix |
| `engine_pipeline_ceiling` | 重測 StateMachine across state sizes 與 shared/separate metrics 對照 |

預期修改檔案限定為：

```text
include/order_books/metrics.hpp
src/domain/state_machine.hpp
src/domain/state_machine.cpp
src/domain/invariant_checker.cpp       # 只抽取／共用必要 helper 時修改
src/runtime/metrics_registry.*         # 只加入 snapshot merge helper 時修改
src/runtime/shard_runtime.*
src/runtime/event_publisher.*
benchmarks/order_book_benchmark.cpp
benchmarks/pipeline_ceiling_benchmark.*
tests/domain/*
tests/integration/*
docs/order-book-design.md
```

若實作不需要修改其中某檔案，不應為對齊清單而產生空泛變更。

## 10. 測試策略

### 10.1 Incremental validation unit tests

- New／Amend／Replace／Cancel 的 touched active／terminal orders；
- single／multiple match 的 maker、taker、quantity conservation與 location updates；
- duplicate、unknown instrument、expected-version mismatch 等 durable rejection；
- producer result、EngineSeq、EventID 與 tombstone eviction postconditions；
- 故意破壞 touched index／aggregate時回傳 fatal invariant error；
- untouched 大 state 不被每個 command 全量遍歷的 benchmark 證據。

### 10.2 Full validation boundary tests

- recovery 後 corrupted state 仍被拒絕；
- live Snapshot 前 full validation failure 不寫新 Snapshot並使 shard failed；
- Publisher replay Snapshot 前 failure 不前進 snapshot sequence並使 Publisher failed；
- 既有 invariant checker 正反案例全部保留。

### 10.3 Equivalence／determinism

使用固定 seed command sequences，在每一步或每一小批後以測試程式呼叫 full validator，並比較：

- live StateMachine；
- Snapshot + WAL recovery；
- Publisher replica replay；
- command results、events、state與 canonical Snapshot bytes。

Incremental validation 不得改變任何成功／拒絕結果、event ordering 或持久化 bytes。

### 10.4 Metrics tests

- 每個 metric 只能由指定 partition 提供；
- merged `MetricsSnapshot` 與既有單 registry 語意一致；
- lag gauge drain 至零後 snapshot 不保留 command partition 的 stale value；
- concurrent writer／Publisher 更新不遺失 owner counters；
- WAL group totals 只計成功 sync，sync failure 不計入；
- external sink exception 仍不使 shard failed。

### 10.5 Regression 與效能

- GCC／Clang warnings-as-errors build；
- GoogleTest Release；
- ASan + UBSan；
- WAL reopen／replay、Snapshot、Publisher cursor correctness；
- StateMachine stage 在 `0／1K／10K／100K` preloaded active orders 下至少五輪；
- metrics shared／separate controlled benchmark保留以驗證 uplift；
- group size／delay Pareto matrix依第 6 節執行。
- `--engine-producer-lanes` 的合法值、zero、非數字與超過 capacity CLI cases。

共享 CI 只執行小型 correctness smoke，不以 RPS 作 hard gate。

## 11. 驗收條件

功能與正確性：

- WAL format、Snapshot format、EventID、EngineSeq與 completion ordering完全不變；
- success completion 仍只在 WAL durable、apply與 incremental validation 成功後產生；
- full validator 在所有 recovery／Snapshot boundary 保留；
- metrics snapshot欄位值與 ownership定義一致；
- Release、ASan、UBSan測試通過。

效能與量測：

- StateMachine + incremental checks 在 0／1K／10K／100K active orders 下不再隨 untouched state
  cardinality 出現 full-scan 級下降；
- separate ownership 下 writer metrics component median 維持至少 1M command-equivalent/s；
- 每個 group-commit case 回報 actual commands/group，而非只回報設定上限；
- Pareto報告明確列出通過／未通過參考 SLO 的 case；
- 沒有 case達到 1M/s時如實記錄下一個瓶頸，不以調大 delay 或省略 durability冒充達標。

本階段不要求完整 durable Engine 必須立即達到 1M/s。完成條件是移除兩個已確認的 CPU
scalability bottleneck、建立可信的 group-commit frontier，並為下一個 WAL hot-path修改提供證據。

## 12. 關鍵決策與取捨

| 決策 | 選擇 | 理由／取捨 |
|---|---|---|
| Group tuning | SLO 內選 Pareto point | 避免以無界 latency換取 RPS |
| Delay 語意 | 保留 idle-fill wait | 不增加每次 dequeue clock成本，也不假裝它是 end-to-end deadline |
| Hot-path validation | touched transition postconditions | 成本隨變更量；需要完整 mutation tests |
| Full validation | recovery／Snapshot／test boundaries | 保留完整診斷，但 Snapshot可能有既有 pause／tail cost |
| Metrics | 兩個既有 registry partition | controlled benchmark已支持，修改小且容易回退 |
| Metric key API | 保留 string names | typed IDs 在 ownership split後尚無必要證據 |
| Production defaults | 本階段不改 | 先取得固定環境 Pareto結果 |
| WAL 深層修改 | 延後 | 尚未區分 encode、CRC、copy、index與device成本 |

## 13. 已知限制與後續觸發條件

- Full validation 不再每 group 掃描 untouched state；未知的 latent corruption 最晚在下一個 full
  validation boundary 被發現。所有正式 state mutation 都必須經 incremental checks，且 property／
  determinism tests需持續以 full validator作 oracle。
- Snapshot前 full validation仍可能造成 tail latency spike；只有其實測違反正式 SLO時，才設計
  immutable image或 background validation／serialization。
- External `MetricsSink` 仍可能由不同 registry thread呼叫；embedding sink沿用既有 non-throwing、
  thread-safe責任。本階段不建立 exporter thread。
- 增加 group size會增加 queue residence time、unacknowledged batch大小與 transient memory。它不改變
  acknowledged RPO，但可能惡化 completion tail。
- 若最佳 SLO-compliant durable WAL仍低於目標，下一階段先 profile encode、CRC、chunk copy、
  cached-record update、write與fsync；只修改最大實測 contributor。
- 若 WAL突破1M後 shared runtime仍受限，依序重測 metrics、incremental validation、Completion
  committed-success path；不得直接導入 lock-free queue。

## 14. 實作順序與設計偏差紀錄

建議依序：

1. 加入 WAL group totals及其 correctness tests；
2. 完成 metrics partition與snapshot merge；
3. 完成 StateMachine incremental postcondition checks；
4. 將 full validation移至Snapshot boundary並保留recovery checks；
5. 執行 regression／sanitizer；
6. 執行 group Pareto與state-size benchmark並產生報告；
7. 依報告另案決定是否調整 production defaults。

若實作發現 touched transition 無法在不掃描全 state 的情況下精確驗證某項 invariant，必須記錄：

```text
原始 invariant
缺少的 mutation information
選擇補充的 bounded metadata或保留的 full check
CPU／memory／format影響
```

不得靜默省略 invariant，也不得為方便而把 internal validation metadata寫入 WAL／Snapshot。
