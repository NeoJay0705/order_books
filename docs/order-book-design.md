# Order Book / Matching Engine 系統設計

狀態：第二階段實作與驗收基線（核心修正已實作；完整驗收仍在進行）<br>
需求基線：`docs/Order-book-spec.md` 1.0-draft<br>
範圍：第一版 Order Book／Matching Engine，不包含網路服務、Kafka、Replication 或 HA

## 1. 文件定位與審查結論

本文件將 `docs/Order-book-spec.md` 轉換為可直接實作的 C++20 架構、模組邊界、資料流、持久化模型與測試策略。需求文件定義「系統必須做什麼」，本文件定義「第一版如何做到」。若兩者不一致，以需求文件為準，並應先修正設計後再修改程式碼。

設計審查結論：

- 設計覆蓋需求中的 matching、query、idempotency、WAL、Snapshot、Recovery、deterministic event、publisher、capacity、metrics 與 benchmark。
- 設計沒有加入 Market Order、risk、account、network protocol、Kafka、dynamic rebalancing、replication 或 HA。
- 第一版只新增三個有明確責任的產品 target：deterministic core、persistence、runtime；測試與 benchmark 為獨立驗證 target。
- WAL 與 Snapshot 使用專案內部的版本化 binary codec；不加入未被需求使用的 serialization framework。
- Event publisher 使用 WAL 驅動的獨立 state-machine replica。這會增加記憶體與 CPU，但能在不新增 durable event log 的前提下滿足 event replay correctness。
- Snapshot 第一版由 shard writer 同步建立，優先確保一致性；因此 Snapshot 期間會暫停該 shard 的 command，屬已知限制而非隱藏的效能承諾。

除文件中明確標為未來方向的項目外，後續實作不得預建額外 adapter、抽象層或 service。

## 2. 需求理解與合理假設

### 2.1 已確認需求

第一版是一個可嵌入同一 process 的 matching engine runtime：

- 一個 process 可以啟動多個 shard；每個 shard 有獨立 writer、WAL、Snapshot、publisher cursor 與 replay state。
- 一個 Instrument 固定路由至一個 shard；Hot Instrument 可以獨占 shard。
- Upstream 透過 C++ API 提交 typed command；公開網路協定不在本版範圍。
- Downstream 透過抽象 EventSink 接收 event batch；特定 broker adapter 不在本版範圍。
- 只有 durable committed command 可以回覆成功。
- Domain state、command result 與 event 必須可由 Snapshot + WAL deterministic rebuild。

### 2.2 實作所需假設

| 項目 | 第一版決策 | 理由 |
| --- | --- | --- |
| 支援平台 | Linux 與 macOS 的 POSIX filesystem semantics | 對應既有開發／部署需求；Windows 不在目前矩陣 |
| `ShardId` | `std::uint32_t` | 足以涵蓋 bounded shard 數並固定 on-disk width |
| `InstrumentId` | `std::uint64_t` | 避免以字串參與 hot path 與 binary format |
| `ProducerId` | `std::uint64_t` | Producer 數量 bounded；具固定 serialization |
| `OrderId` | 兩個 `std::uint64_t` 組成的 128-bit value type | C++20 可攜且符合不可重用 ID 建議 |
| Epoch、ProducerSeq、EngineSeq、Version | `std::uint64_t` | 對應需求並禁止 wrap |
| Price、Quantity | `std::int64_t` | 對應 ticks／lots 與 overflow checking |
| Timestamp | UTC Unix epoch nanoseconds，`std::int64_t` | 可持久化與跨 process 傳遞；不參與 matching |
| Instrument／behavior 設定 | 啟動時載入的 immutable versioned value objects | 使同一 WAL 永遠使用相同 tick／lot、capacity 與 tombstone 規則 |
| On-disk byte order | little-endian | 格式固定，不依賴 host ABI |

公開型別必須使用具語意名稱、固定寬度的 type alias 或 value type；不得在 domain API spelling 直接使用未命名裸整數。需要 composite representation 的 `OrderId` 使用 128-bit value type；enum 一律使用 `enum class`。On-disk enum value 一旦發布不得改義；新增值只能使用新數值。

### 2.3 明確不做的推論

- 不因「Downstream」一詞推論必須使用 Kafka。
- 不因「多 shard」推論需要跨機器 cluster manager。
- 不因未來可能公開 API 而承諾第一版 ABI stability 或安裝套件。
- 不建立 account、risk、settlement、market-data book builder。
- 不將 query 寫入 WAL，也不為 query 分配 EngineSeq。

## 3. 系統架構與依賴方向

```text
Application / future adapter
        │
        ▼
order_books_runtime
  Engine ──► ShardRuntime ──► bounded ingress queue
                    │
                    ├──► order_books_storage ──► POSIX files
                    │
                    └──► order_books_core
                              │
                              ├── OrderBook
                              ├── StateMachine
                              └── Event generation

EventPublisher
  sequential WAL index ──► independent StateMachine replica ──► EventSink
```

依賴只能由外向內：

```text
runtime → storage → core
runtime ──────────→ core
core 不依賴 storage 或 runtime
```

測試可以依賴任一產品 target；產品 target 不得依賴 GoogleTest 或測試 helper。

## 4. CMake Targets 與專案組織

### 4.1 Targets

| Target | 類型 | 責任 |
| --- | --- | --- |
| `order_books_core` | STATIC | Domain types、Order Book、matching、state transition、event generation、invariants |
| `order_books_storage` | STATIC | Binary codec、CRC32C、WAL、Snapshot、cursor、atomic file operations |
| `order_books_runtime` | STATIC | Multi-shard lifecycle、queues、group commit、recovery、publisher、retention、metrics |
| `order_books_tests` | executable | Unit 與較小型 integration tests |
| `order_books_benchmark` | executable，opt-in | 固定 workload benchmark 與 latency report |

提供 alias：

```text
order_books::core
order_books::storage
order_books::runtime
```

所有 target 連結既有 `order_books::project_options`。Runtime 額外連結標準 CMake `Threads::Threads`。第一版不新增 runtime 第三方依賴；GoogleTest 仍只屬於 test requirement。

Benchmark 使用小型專案內 harness、`std::chrono::steady_clock` 與固定 seed workload，不新增 Google Benchmark。原因是需求只要求可重現量測，尚未需要另一個 dependency 與其維護成本。

### 4.2 必要檔案結構

```text
include/order_books/
├── engine.hpp
├── event_sink.hpp
├── metrics.hpp
└── model.hpp

src/
├── domain/
│   ├── invariant_checker.cpp
│   ├── order_book.cpp
│   ├── order_book.hpp
│   ├── state_machine.cpp
│   └── state_machine.hpp
├── persistence/
│   ├── binary_codec.cpp
│   ├── binary_codec.hpp
│   ├── config_store.cpp
│   ├── config_store.hpp
│   ├── crc32c.cpp
│   ├── file_ops.cpp
│   ├── file_ops.hpp
│   ├── snapshot_store.cpp
│   ├── snapshot_store.hpp
│   ├── wal.cpp
│   └── wal.hpp
└── runtime/
    ├── engine.cpp
    ├── event_publisher.cpp
    ├── event_publisher.hpp
    ├── metrics_registry.cpp
    ├── shard_runtime.cpp
    └── shard_runtime.hpp

tests/
├── integration/
├── unit/
└── CMakeLists.txt

benchmarks/
├── CMakeLists.txt
└── order_book_benchmark.cpp
```

只建立上列有實際責任的檔案。若實作後兩個相鄰的小型型別只需數十行且沒有獨立依賴邊界，可以合併，不得為符合樹狀圖而製造空檔案。

### 4.3 既有檔案的必要修改

| 檔案 | 必要修改 |
| --- | --- |
| `CMakeLists.txt` | 加入三個產品 target、`Threads`、tests 與 opt-in benchmarks subdirectory |
| `CMakePresets.json` | 新增 Release benchmark preset；既有 debug／sanitizer／tidy preset 延伸至產品 targets |
| `conanfile.py` | 不新增 runtime dependency；保留 GoogleTest test requirement |
| `README.md` | 更新目前能力、build／test／benchmark 指令與非目標 |
| `CONTRIBUTING.md` | 加入 persistence format 與需求／設計同步規則 |
| `.clang-tidy` | 只有實際規則誤報時才作最小調整，不預先放寬 |
| CI workflow | 在既有 compiler matrix 建置產品與測試；Release job 執行 benchmark smoke，不設效能 gate |

第一版不加入 install/export rules、package publishing、Docker、coverage service、Doxygen 或 broker SDK。

## 5. 公開模型與介面

### 5.1 `model.hpp`

公開 value types：

```cpp
struct OrderId { std::uint64_t high; std::uint64_t low; };
struct CommandIdentity;
struct EventId;
struct TradeId;

enum class Side;
enum class CommandType;
enum class CommandStatus;
enum class OrderStatus;
enum class ErrorCode;
enum class EventType;

struct NewOrder;
struct AmendQuantity;
struct ReplaceOrder;
struct CancelOrder;
using CommandPayload = std::variant<...>;

struct Command;
struct CommandResult;
struct Event;
struct BookLevel;
struct BookDepth;
struct OrderView;
struct InstrumentConfig;
struct ShardBehaviorConfig;
```

設計規則：

- Command payload 使用 `std::variant`，使 command type 與 payload 在 C++ type system 中一致。
- Optional 欄位使用 `std::optional`。
- Public value object 不含 raw pointer、mutex、filesystem handle 或 allocator ownership。
- `CommandResult` 永遠以 machine-readable `ErrorCode` 表達錯誤；文字訊息只作診斷。
- Public model 不暴露內部 intrusive links 或 PriceLevel pointer。

### 5.2 `Engine`

`Engine` 是 application-facing facade，負責 routing 與 shard lifecycle。語意介面：

```cpp
class Engine {
public:
    static OpenResult open(EngineConfig, EventSink&, MetricsSink&);

    SubmitResult submit(Command, CompletionHandler);
    QueryResult get_order(InstrumentId, OrderId);
    QueryResult best_bid_ask(InstrumentId);
    QueryResult top_n(InstrumentId, std::size_t depth);

    StopResult stop();
};
```

精確的 callback／result wrapper spelling 可在實作時依 C++ ergonomics 小幅調整，但必須維持：

- `submit` 可由非 shard thread 呼叫，且只把工作放入目標 shard 的 bounded queue。
- Completion 只在 definitive admission error 或 committed result 可用時呼叫一次。
- Completion 不在 shard writer thread 直接執行，避免 user code 阻塞或 re-enter state machine。
- Query 排入相同 shard queue，於 command 邊界執行，保證不看見中間狀態。
- Engine stop 先停止 ingress，再完成或明確拒絕 queued request，最後關閉 publisher 與 file handles。

第一版 queue 使用 `std::mutex + std::condition_variable + std::deque` 的 bounded MPSC 實作。Lock-free queue 在有 benchmark 證明 queue 是瓶頸前不加入。

### 5.3 EventSink

```cpp
class EventSink {
public:
    virtual PublishResult publish(
        ShardId shard,
        EngineSeq engine_seq,
        std::span<const Event> events,
        std::stop_token stop_token) = 0;
    virtual ~EventSink() = default;
};
```

契約：

- `publish` 只在 downstream 已 durable 接受完整 batch 時回傳 success。
- Failure 不得表示部分 batch 已被 Engine 視為 confirmed。
- Sink 可以重複收到相同 EventID。
- Sink 必須在 stop requested 時於 bounded time 內返回，讓 Engine 可以安全關閉。
- Runtime 不提供會靜默丟棄事件的 production NullSink。
- 測試使用 RecordingSink／FaultingSink；broker adapter 留給後續專案或階段。

### 5.4 MetricsSink

Runtime 內建有界的 `MetricsRegistry`，記錄 counters、gauges 與 fixed-bucket histograms，並提供 snapshot。Optional `MetricsSink` 可將 snapshot 轉接至外部監控，但本版不內建 Prometheus／OpenTelemetry exporter。

Histogram 使用固定且文件化的 microsecond buckets；p50／p99／p99.9 是 bucket estimate，benchmark 另外保留精確 samples 計算報告。Metrics 更新不得改變 command 或 event 順序。

### 5.5 EngineConfig 與預設

影響 logical result 的設定與只影響 runtime 的設定必須分開：

```text
ShardBehaviorConfig (immutable, versioned, persisted)
├── max_active_orders_per_shard = 1,000,000
├── terminal_tombstone_max_age = 1 hour
└── terminal_tombstone_max_count = 1,000,000

RuntimeConfig (operational, not part of replay result)
├── max_instruments_per_shard = 1,024
├── max_envelope_bytes = 1 MiB
├── ingress_queue_capacity = 65,536
├── group_commit_max_delay = 200 microseconds
├── group_commit_max_commands = 256
├── wal_prepare_lanes = 1 (caller only; opt-in 2 or 4)
├── wal_parallel_prepare_min_commands = 4,096
├── wal_segment_size = 256 MiB
├── wal_soft_limit_per_shard = 64 GiB
├── snapshot_interval = 5 minutes or 1,000,000 commands
├── event_replay_snapshot_interval = 5 minutes or 1,000,000 commands
├── publisher_cursor_persist_max_commands = 256
├── publisher_cursor_persist_max_delay = 1 millisecond
├── max_publish_lag_age = 24 hours
└── max_publish_lag_bytes = 32 GiB
```

`ShardBehaviorConfig` 的 version 必須寫入 WAL 與 Snapshot，歷史版本在相關資料截斷前不可刪除。RuntimeConfig 可以在 restart 後調整，因為它只改變 batching、resource protection 與 operational timing，不改變同一 command 的 logical output。

`wal_prepare_lanes` 是每個 shard 的 bounded WAL preparation lane 數，包含 shard writer caller，
只接受 1、2、4；預設 1 不建立背景 prepare thread。`wal_parallel_prepare_min_commands` 預設
4,096，只有當實際 accepted group 達到門檻且 lanes 大於 1 時才啟用平行 prepare。每個 shard 的
額外背景 thread 數為 `wal_prepare_lanes - 1`，部署時必須依 shard 數與 CPU affinity 評估總量。
這兩個欄位只在 Engine 啟動時生效，rollback 以停止後改回 W=1 並重啟完成。

## 6. Domain Model 與資料結構

### 6.1 ShardState

```text
ShardState
├── shard_id
├── last_committed_engine_seq
├── logical_retention_time
├── instrument_configs by version
├── behavior_configs by version
├── books: unordered_map<InstrumentId, OrderBook>
├── producer_states
└── terminal_tombstones
```

`ShardState` 是 Snapshot 與 deterministic replay 的完整 logical state。File handles、queues、timers、metrics 與 publisher cursor 不屬於 domain state。

### 6.2 OrderBook

每個 Instrument 一個 `OrderBook`：

```text
OrderBook
├── orders: unordered_map<OrderId, unique_ptr<OrderNode>>
├── bids: map<Price, PriceLevel, greater<Price>>
└── asks: map<Price, PriceLevel, less<Price>>
```

`std::map::begin()` 是最佳價位；從 begin 迭代 N 個 level 滿足 Top-N O(N)，不需要額外 level linked list。

`OrderNode` 由 `unique_ptr` 穩定擁有，並具有：

```text
Order value fields
PriceLevel* level
OrderNode* previous
OrderNode* next
```

`PriceLevel` 保存 head、tail、aggregate quantity 與 count。FIFO 採專案內 intrusive doubly linked list，不引入 Boost.Intrusive。所有 pointer 都是 non-owning，刪除順序固定為 unlink、更新 aggregate、移除 index ownership，避免 dangling pointer。

這個選擇滿足：

- Order lookup 平均 O(1)；
- known Order unlink O(1)；
- empty level erase O(log P)；
- level lookup／insert O(log P)；
- best O(1)；
- Top-N O(N)。

### 6.3 ProducerState

Key：

```text
(ProducerId, ProducerStreamId)
```

Value：

```text
current_epoch
last_processed_seq
last_canonical_command
last_result
```

Duplicate 判定直接逐 byte 比較 `last_canonical_command`；canonical bytes 同時是持久化的 authoritative digest，不另外保存未使用的 fingerprint。每個 producer stream 只保存最後一筆，空間仍為 bounded。

第一次看見 `(ProducerId, ProducerStreamId)` 時只接受 `producer_seq = 1`，並以該 command 的 epoch 建立 state。`producer_seq = 0` 保留為 invalid；epoch 是 monotonic generation，零同樣保留為 invalid。ProducerSeq 達最大值後，Producer 必須切換到更高 epoch 並從 1 開始；若 epoch 也耗盡，該 ProducerId 必須退役。任何欄位都不得 wrap。

### 6.4 Terminal Tombstones

使用：

```text
unordered_map<OrderId, Tombstone>
+ deque<(terminal_engine_seq, OrderId)>
```

Map 提供平均 O(1) lookup，deque 提供 deterministic oldest-first eviction。Count eviction 依 terminal EngineSeq；age eviction 不直接讀 wall clock，而使用：

```text
logical_retention_time = max(previous_value, command_received_at)
```

每次 committed command 後，以該 durable logical time 淘汰過期 tombstone。如此 replay 不會因執行當下時間不同而得到不同 state。Idle 期間不由 background timer 改變 domain state；下一筆 committed command 才觸發 age eviction。

## 7. Deterministic State Machine

### 7.1 單一入口

Core 提供單一語意入口：

```text
apply(ShardState&, CommittedCommand)
    -> ExecutionOutput {
           CommandResult,
           vector<Event>
       }
```

`CommittedCommand` 包含 EngineSeq、durable received timestamp、Instrument config version 與 canonical Command。Core 不自行配置 EngineSeq、不讀 clock、不寫檔案、不 publish event。

### 7.2 執行順序

`apply` 固定：

1. 更新 deterministic logical retention time 並做 tombstone eviction。
2. 依 command type 執行 business validation。
3. Rejection 時建立 stable result 與一個 `COMMAND_REJECTED`。
4. Success 時呼叫 OrderBook operation／matching。
5. 收集受影響 maker，完成每張 Order 的單次 version increment。
6. 依需求順序建立 TRADE、maker ORDER_UPDATED、target ORDER_UPDATED。
7. 更新 ProducerState 的 seq、canonical command 與 result。
8. 在 Debug／test mode 執行 invariants。

No-op Amend 不改 Order、version 或 event，但仍更新 ProducerState 並回傳具有 EngineSeq 的 `NO_CHANGE` result。

Event payload 必須在 terminal OrderNode 從 ownership map 刪除前複製到 command-local `ExecutionOutput`；不得在 unlink／erase 後保留 node pointer。Shard 已達 active-order 上限時，NEW_ORDER 在 matching 前以 `SHARD_CAPACITY_EXCEEDED` 拒絕，即使該 Order 理論上可能完全成交；這是需求允許且可 deterministic replay 的保守 admission policy。Cancel、Amend decrease 與 query 不受此上限阻擋。

### 7.3 Matching 的確定性

- 價格由 ordered map comparator 決定。
- 同價位由 intrusive FIFO 決定。
- 成交價固定為 maker price。
- Event／Trade index 使用 local counter，從零開始。
- 不迭代 unordered container 來決定任何 output ordering。
- Snapshot serialization 若需要遍歷 unordered container，必須先依 stable key 排序。

### 7.4 Overflow

集中提供 checked add／subtract helper。PriceLevel aggregate、quantity 更新、sequence 與 version increment 都使用 checked operation。偵測到 domain input overflow 時產生 `NUMERIC_OVERFLOW` business rejection；內部已存在 state 發生不可能的 underflow／overflow 表示 invariant violation，shard fail-stop。

## 8. Runtime 與 Command Data Flow

### 8.1 Shard Lifecycle

```text
CLOSED → RECOVERING → READY → DRAINING → CLOSED
                       │
                       └──────────────→ FAILED
```

- `RECOVERING`：載入設定、Snapshot、WAL，驗證 invariants；拒絕 ingress。
- `READY`：接受 command／query。
- `DRAINING`：停止新 ingress，完成已接受工作並 flush。
- `FAILED`：WAL、corruption、unexpected core failure 或 storage pressure；不得繼續 mutation。

### 8.2 Ingress 與 Backpressure

每個 shard 一個 bounded MPSC queue，預設 capacity 65,536，屬 runtime 可調值。Queue full、not ready 或 failed 時回傳 `ENGINE_UNAVAILABLE` admission error，不消耗 ProducerSeq。

Router 依 `producer_stream_id` 找到目標 shard，並驗證該 shard 存在。若 Instrument 已知，mapping 與 stream 不一致是 `WRONG_PRODUCER_STREAM` admission error；未知 Instrument 仍送入指定 shard，之後成為有 EngineSeq 的 durable `UNKNOWN_INSTRUMENT` business rejection。Producer epoch／sequence validation 必須在 shard writer 上完成，避免並行讀寫 ProducerState。

新 command 通過 producer validation 後，writer 讀取一次 system clock 作為 `command_received_at`，並與 command 一起寫入 WAL。該時間不屬於 upstream command fingerprint；retry 不產生新 WAL timestamp，而是回傳原結果。

### 8.3 Group Commit

Shard writer 取到第一筆可接受的新 command 後，收集至：

```text
max 256 commands
或等待最長 200 microseconds
```

同一 producer stream 的下一筆在前一筆完成前屬違反 single-in-flight contract，不納入同批。Writer 為 batch 依序分配 tentative EngineSeq、encode、batch append，單次 fsync。WAL active segment 保持 descriptor 開啟；未跨 segment 的 batch 合併為一次 data write，跨 segment 時每個 touched segment 各寫一次，且不改變 record framing。Durable success 後再依 EngineSeq 逐筆呼叫 state machine 並完成 callback。

若 append 或 fsync 失敗，整個未確認 batch 不得 apply 或 success；shard 進入 FAILED。Recovery 以可驗證的完整 WAL tail 決定哪些 records committed。

fsync 成功只代表 WAL durable；writer 仍須依序完成 live `StateMachine::apply`，並在整個 group 的 transition 通過 invariant validation 後，以 durable `WalPosition` 通知 publisher。Publisher 不得讀取或發布超過此位置的 active WAL bytes，因此 event 不會早於 live state application，也不會來自尚未 durable 的 command。Restart 後必須先完成全部 WAL recovery／apply，再把 publishable position 設為 recovery 驗證出的 WAL head。

### 8.4 Completion Dispatch

Shard writer 將 completed result 移交 bounded completion queue，由 Engine-owned completion worker 執行 application callback。Callback exception 必須被 boundary 捕捉並記錄，不得使 shard failed；callback 不得被重試，因上游可用相同 identity 主動 retry 取得 last result。

同一 shard 的 completion 必須保持 EngineSeq 順序；跨 shard 不保證順序。Admission error 沒有 EngineSeq，但 writer 仍依原始 batch request 順序 dispatch completion，不得越過同一 producer stream 尚未完成的前一筆 command。

### 8.5 Query

Query message 與 command 共用 shard ingress queue，但：

- 不進 WAL；
- 不取得 EngineSeq；
- 不更新 ProducerState；
- 在兩個 command／batch application 之間執行；
- 只回傳 value copy，不外洩 domain pointer。

Top-N 的 `N = 0` 回傳空集合；超過 configured response limit 回傳 `INVALID_COMMAND`，避免無界 response allocation。具體 response limit 是 runtime config，不屬撮合語意。

## 9. WAL 與 Binary Format

### 9.1 Codec 原則

- 不得以 `reinterpret_cast` 或直接 dump C++ struct 寫入磁碟。
- 所有 integer 使用明確 width 與 little-endian encode／decode。
- 所有 length 必須先驗證上限再 allocation。
- Decoder 拒絕 unknown required field、invalid enum、overflow、trailing garbage。
- Format version 位於 segment／snapshot header 與 record；不支援版本必須 fail-stop。

### 9.2 WAL Segment

目錄：

```text
data/shard-<id>/wal/
    <first-engine-seq>.wal
```

Segment header：

```text
magic
format_version
shard_id
first_engine_seq
header_crc32c
```

Record framing：

```text
record_length : u32
record_version: u16
record_payload: bytes
record_crc32c : u32
```

CRC32C 覆蓋 `record_version + record_payload`。`record_length` 表示其後 record version、payload 與 checksum 的總 bytes。Decoder 必須先驗證長度範圍，再讀完整 record，最後驗證 CRC。

Record payload 的 logical fields 以需求文件為準，另包含 command payload discriminant 與 `behavior_configuration_version`。Optional ExpectedVersion 使用 presence byte + value，不使用 sentinel。Replay 必須同時解析 WAL 指定的 Instrument configuration 與 ShardBehaviorConfig；不得以目前啟動參數取代歷史版本。

### 9.3 Commit Boundary

第一版每個 record 在完整 append 後，只有涵蓋該 record 的成功 fsync 才 committed。Group fsync 同時 commit batch 中所有完整 records。Process crash 後：

- 最後 segment 的 partial record 截斷至上一筆 valid record；
- valid checksum record 視為 committed；
- checksum 錯誤但 record bytes 完整視為 corruption，不自動修復。

### 9.4 Segment Rotation

達 256 MiB soft boundary 時，在下一筆 record 前 rotate。單筆 record 不得跨 segment。Rotation 建立新 segment header 並 fsync file；directory entry 依 POSIX crash-safe 流程持久化。

## 10. Snapshot 與 Recovery

### 10.1 Snapshot Format

```text
Snapshot header
├── magic / format version / shard id
├── snapshot EngineSeq
├── Instrument config manifest version
├── Shard behavior config version
└── payload length (u64)

Canonical payload
├── logical retention time
├── books sorted by InstrumentId
├── levels in matching order
├── orders in FIFO order
├── ProducerState sorted by key
└── tombstones sorted by terminal EngineSeq then OrderId

Snapshot footer
└── CRC32C over header + payload
```

Aggregate quantity、count 與 indexes 可以存入 Snapshot 供驗證，但 recovery 必須重建 pointers／iterators，並交叉檢查 stored aggregate；不得 deserialize raw address。

目前實作使用 Snapshot format version 2；移除未使用的 producer fingerprint 後，舊的未發布格式不相容時直接拒絕，不做自動 migration。

### 10.2 Snapshot 建立

第一版在 shard writer 的 command boundary：

1. 暫停該 shard ingress consumption。
2. Canonical serialize 當前 ShardState 至 temporary file。
3. fsync temporary file。
4. atomic rename。
5. fsync parent directory。
6. 恢復 command consumption。

此方案不需要 state copy、copy-on-write 或額外 locking，是第一版最小且正確的實作。Snapshot duration 必須量測；若實測違反未來 latency SLO，再以 immutable snapshot image 或 background writer 取代。

### 10.3 Recovery

Recovery 固定：

1. 掃描並驗證可用 Snapshot，選擇最高 valid EngineSeq。
2. 重建 domain pointers／indexes 並執行全部 invariants。
3. 依 segment 與 EngineSeq 讀取後續 WAL。
4. 對每筆 record 呼叫同一 `StateMachine::apply`，不得使用 recovery 專用 business logic。
5. 驗證 sequence 連續、shard/config version 相容。
6. 截斷唯一允許的 partial tail。
7. 設定 next EngineSeq 並轉為 READY。

任何 middle corruption、checksum failure、sequence gap、unsupported version 或 invariant failure 都使 shard FAILED，且錯誤需包含 shard、file、offset、expected／actual sequence。

## 11. Event Publisher 與 WAL Retention

### 11.1 獨立 Replay Replica

每個 shard 的 EventPublisher 擁有獨立 `ShardState`：

```text
durable event-replay Snapshot
        ↓
read next WAL record
        ↓
StateMachine::apply on publisher replica
        ↓
Event batch for one EngineSeq
        ↓
EventSink::publish
        ↓ durable ACK
confirmed cursor (in-memory)
        ↓ count/time/snapshot/clean-stop trigger
persist publisher cursor → durable cursor
```

Publisher 不直接依賴 live writer 暫存的 events，因為那些 events 在 crash 後不存在。Live writer 與 publisher 使用同一 state-machine code；determinism tests 必須比較兩者 output。

成本是每 shard 最多兩份 logical state 與第二次 command execution。這是「WAL 作為唯一 durable source」下必要且清楚的取捨；若未來資源成本不可接受，再引入同 WAL transaction 的 durable execution output，而不是偷加非 atomic event file。

Publisher startup 固定：

1. 載入 durable publisher cursor 與最新 valid event-replay Snapshot。
2. 若 cursor valid，使用 EngineSeq 不大於 cursor 的 replay base；若最新 Snapshot 晚於 cursor，實作會退回 immutable configuration genesis 並從 WAL replay 到 cursor。若 cursor 遺失／毀損，安全退回最新 replay Snapshot 的 EngineSeq，後續最多造成 duplicate。若兩者皆無，從 genesis state／EngineSeq 0 開始；缺少所需 WAL 時必須 fail-stop。
3. 從 Snapshot 後 replay 到 cursor，但不 publish；用途是重建 cursor 時點的 replica state。
4. 從 `cursor + 1` 開始 apply 並 publish。

若 ACK 成功但 cursor 尚未 durable 就 crash，舊 cursor 會使該 batch 再次 publish；EventID 不變，由 consumer 去重。

同一 shard 同時只允許一個未確認 batch。Publisher apply 下一筆 command 後保留其 EventBatch，持續 retry 到成功或 stop；未確認前不得 apply／publish 後續 EngineSeq，也不得建立包含該 command 的 event-replay Snapshot。不同 shard 可以並行 publish，且不提供跨 shard ordering。Sink ACK 後先前進 confirmed cursor；cursor persistence 以 runtime 的 command count 或 delay policy 分組，並在下一筆 WAL record 前完成 count trigger。ACK 與 cursor fsync 之間的 bounded window 可能在 crash 後重送，仍由 EventID 去重。

### 11.2 Cursor

Cursor file 只保存：

```text
format_version
shard_id
last_durable_engine_seq
checksum
```

使用 temporary file + fsync + rename + directory fsync。confirmed cursor 表示 downstream 已 ACK 的連續 EngineSeq，durable cursor 表示 restart 可安全採用的 recovery boundary；兩者不可混用。ACK 成功但 cursor 尚未 durable 時 crash，只會造成 bounded duplicate publish。零 event command 不呼叫 sink，仍按 WAL command 計入 cursor group。建立 event-replay Snapshot 前必須先 flush 最新 confirmed cursor，避免 Snapshot sequence 超過 durable cursor。

Publisher 失敗採 bounded exponential retry，預設 1 ms 起、最多 1 s；stop 時可中斷等待。Retry policy 是 runtime config，不影響 EventID 或 ordering。

### 11.3 Event-Replay Snapshot

Publisher replica 定期在已 confirmed EngineSeq 建立 replay Snapshot，格式與 state Snapshot 相同但 metadata 標記用途。建立新 replay Snapshot durable 後，舊 Snapshot 才可刪除。

RetentionManager 計算：

```text
truncate_watermark = min(
    durable_state_snapshot_seq,
    durable_event_replay_snapshot_seq)
```

只刪除 end sequence 不大於 watermark 的完整 WAL segment；不做 record-level prefix rewrite。刪除後 fsync WAL directory。Publisher cursor 本身不能取代 replay Snapshot watermark。

### 11.4 Storage Pressure

Metrics 監控 WAL bytes、publisher lag age／bytes 與 filesystem free space：

- 達 configured 50% lag budget：warning。
- 達 80%：critical。
- 仍有安全空間時繼續服務並嘗試 Snapshot／publish。
- 無法保證下一批 WAL durable append 時，停止接受 mutation 並進入 storage-pressure unavailable；不得刪除未安全截斷 WAL。

## 12. Instrument Configuration

Instrument 設定以 immutable manifest 管理；每個 manifest 有單一 `instrument_configuration_version`，內含多個 `InstrumentConfig`：

```text
instrument_id
tick_size
lot_size
assigned_shard
```

Engine 啟動時驗證：

- ID 與 version 唯一；
- tick／lot 為正數；
- assigned shard 存在；
- 同一 Instrument 只指向一個 shard；
- 每 shard Instrument 數不超過 `max_instruments_per_shard`；
- Snapshot／WAL 引用的所有歷史版本仍可取得。

`ShardBehaviorConfig` 另有獨立 version，保存會影響 business result 或 deterministic state 的 active-order capacity 與 tombstone retention。Config repository 是啟動輸入，不建立動態 configuration service。歷史版本保存在 shard data directory 的 immutable manifest；寫入與更新使用 atomic file 流程。

第一版檔名固定為 `config/instruments-<version>.bin` 與 `config/behavior-<version>.bin`；同一 version 的 canonical payload 不可被覆寫成不同內容。

第一版不提供一般性的設定更新 API。Tick／lot 或 behavior 設定只能在 shard 停止時新增 immutable version；新增 Instrument 或修改沒有 active Order 的 Instrument 可以在啟動時建立新 manifest，若受影響 Instrument 仍有 active Order 則必須拒絕不相容變更。Instrument-to-shard mapping 變更需要 migration protocol，依需求明確留待未來版本。Current behavior version 由本次 supplied `shard_behaviors` 的選擇決定；persisted versions 僅供 replay 使用。

## 13. 錯誤處理

### 13.1 分類

| 類別 | 表達方式 | Shard 行為 |
| --- | --- | --- |
| Admission error | `CommandResult`，無 EngineSeq | 保持 READY |
| Business rejection | durable `CommandResult` + event | 保持 READY |
| Query error | `QueryResult` | 保持 READY |
| Recoverable sink failure | retry + metrics | Writer 可繼續至 storage budget |
| WAL／Snapshot corruption | startup/runtime error | FAILED |
| WAL fsync／I/O failure | runtime error | FAILED，不再 mutation |
| Unexpected exception／invariant violation | diagnostic + fail-stop | FAILED |

C++20 沒有 `std::expected`。第一版以小型 value-based `Result<T>`（`std::variant<T, Error>` 封裝）處理 startup、query 與 storage outcome；expected domain failures 不使用 exception。Allocation failure 或 programmer error 可以在 shard boundary 捕捉後 fail-stop，不嘗試繼續使用可能不一致的 state。

### 13.2 診斷內容

Storage／recovery error 至少包含：

```text
error code
shard id
path
byte offset when applicable
engine sequence when known
OS error code when applicable
```

不得把 filesystem message 或 exception text 當成 machine-readable contract。

## 14. Observability 設計

MetricsRegistry 按 shard 記錄目前已接入的 command、queue、WAL、execution、publisher lag 與 active-state metrics。完整的 command-type/result dimensions、snapshot/recovery duration 與 filesystem free-space exporter 仍屬後續驗收項目，不在本切片宣稱已完成。Hot path 原則：

- Counter 使用 shard-writer-owned value，避免不必要 atomic contention。
- Registry 只接受固定 metric name set；未知名稱直接忽略，避免把 InstrumentID、OrderID 或錯誤文字形成高基數 key。
- Queue depth 由 queue 自身提供 gauge。
- Publisher metrics 由 publisher thread 擁有。
- 外部 snapshot 以短鎖複製 aggregate，不讀 domain internal pointer。
- Timestamp measurement 使用 `steady_clock`；event occurred_at 使用 durable system clock value，兩者不得混用。

Command latency 分段點：

```text
received
enqueued
dequeued
wal_appended
wal_durable
execution_complete
completion_dispatched
```

由此計算 queue、WAL commit、execution 與 end-to-end latency。Metrics failure 不得使 command failed，也不得改變 event。

## 15. 測試策略

### 15.1 Unit Tests

`order_books_core`：

- price／time priority；
- partial fill、multiple match、maker price；
- New／Amend／Replace／Cancel 的所有表格分支；
- version、priority reset、aggregate、best、Top-N；
- checked arithmetic；
- deterministic event ordering；
- tombstone count／age eviction；
- invariant checker 的正反案例。

`order_books_storage`：

- every scalar／enum／variant encode-decode round trip；
- golden WAL／Snapshot fixtures；
- invalid length、enum、version、checksum、trailing bytes；
- segment rotation；
- partial tail 與 middle corruption；
- atomic Snapshot／cursor lifecycle。

`order_books_runtime`：

- producer epoch／seq state table；
- retry same payload、identity conflict、duplicate too old；
- business rejection durable replay；
- group commit ordering；
- queue backpressure；
- lifecycle transitions；
- query serialization；
- publisher duplicate after lost ACK；
- retention watermark。

### 15.2 Reference Model 與 Property Tests

Tests 提供只追求簡單正確、不追求複雜度的 reference book。固定 seed 生成合法 command sequence，逐步比較：

- active orders；
- depth；
- trade sequence；
- quantity conservation；
- command results。

不新增 property-test framework；使用 GoogleTest parameterized tests 與專案內 deterministic generator。失敗時印出 seed 與最短可重放 command log。

### 15.3 Persistence failure／crash coverage

目前 `order_books_tests` 已覆蓋 WAL rotation、last-segment partial tail、middle-segment corruption、checksum failure、config manifest round trip，以及 publisher snapshot invariant rejection。`FileOps` 保持窄 POSIX boundary，未把 fault-injection seam 暴露到 production public API。

真實 process failpoint（append／fsync／Snapshot rename／sink ACK 後）與完整 subprocess restart matrix 尚未納入這個實作切片；在加入可注入 FileOps 前，不宣稱已完成 crash Definition of Done。後續若需求把 crash matrix 列為 release gate，應在 storage boundary 加入 test-only implementation，並比較 restart 後 state、result 與 events。

### 15.4 Determinism Tests

相同 fixture 至少執行：

1. live state-machine execution；
2. state Snapshot + WAL recovery；
3. publisher replay Snapshot + WAL execution。

逐欄位比較 state、ProducerState、result 與 events；Snapshot canonical bytes 也應相同。不得只比較 Order 數量或 hash。

### 15.5 Integration 與 Sanitizers

- 使用 temporary directory，測試結束後清除。
- 不依賴 network、真實 broker 或測試執行順序。
- 時間以 injected clock／WAL timestamp 控制。
- 主要 unit／integration tests 在 ASan + UBSan 下執行。
- Persistence tests 在 Linux GCC、Linux Clang、macOS Apple Clang 執行。

## 16. Benchmark 設計

`order_books_benchmark` 預設不建置；Release benchmark preset 明確啟用。Workloads：

```text
resting_new
crossing_new
cancel
amend_decrease
amend_increase
replace_crossing
multiple_match
mixed_single_instrument
wal_write_ceiling
recovery_snapshot_plus_wal
engine_durable_single_instrument
engine_pipeline_ceiling
```

每次報告包含：

- warmup 與 measured duration；
- fixed random seed；
- command／trade throughput；
- p50／p99／p99.9／max；
- active orders／levels；
- OS、CPU、compiler、build type；
- WAL device/path、fsync 與 group-commit config。

CI 只執行小型 benchmark smoke，驗證 harness 可運作，不以 shared runner 數字作 regression gate。取得專用硬體 baseline 後才新增 threshold。

`wal_write_ceiling` 是只測正式 WAL path 的 storage microbenchmark。它預設以 256 筆
command 為一組，並可用 `--wal-group-size` 與 `--wal-sync=none|per_group` 分別量測
append-return 上限與每組 `fsync` 的 durable 上限；使用 production 256 MiB segment，
完成後 reopen／replay 驗證，任何 open、write、append、sync 或 replay failure 都使
benchmark 以 non-zero 結束。`sync=none` 仍包含 WAL segment rotation 內部的必要 sync，
不得解讀為完全沒有 fsync。

`engine_durable_single_instrument` 是單一 instrument、單一 shard 的 Engine durable
end-to-end workload。每個 measured command 由公開 `Engine::submit()` 進入 bounded
ingress queue，並以 `CompletionHandler` 收到 committed result 作為完成邊界；因此計時包含
queue wait、group commit、WAL append、`fsync`、state apply、invariant validation 與
completion dispatch。它使用多個 producer lanes 遵守 single-in-flight contract，並以
Sell／Buy pair 維持單一 order book 的 bounded size。此 workload 不等待 downstream
EventSink ACK，也延後 Snapshot／replay Snapshot trigger；`wal_write_ceiling` 與此
Engine workload 不得混為同一個 RPS 數字。

`engine_pipeline_ceiling` 是 benchmark-only 診斷 workload，分別量測正式
`StateMachine::apply`、完整 `validate_state`、`MetricsRegistry`、公開 Engine 的 ingress／
admission／Completion handoff，以及 Publisher backlog drain。CPU stage 使用 group-based
loop；queue、Completion、WAL 與 Publisher 保留正式 worker、同步、I/O 與 backpressure。
Publisher stage 必須等待 durable cursor 追上 WAL head 並在 stop/join 後 reopen 驗證，不能
只用 in-memory confirmed cursor 推論可持續吞吐。此 workload 產出 component ceiling 與
latency Pareto frontier，不修改 production default、Publisher cursor semantics 或任何
batching implementation。

根因分析時，pipeline benchmark 可用 `--pipeline-producer-lanes=N` 掃描 runtime handoff
的 producer concurrency；metrics stage 同時輸出 separate-registry 與 shared-registry
雙 worker 對照。兩者都只屬 benchmark 診斷控制，不改變 production runtime 設定。

`engine_durable_single_instrument` 可用 benchmark-only `--engine-group-size` 與
`--engine-group-delay-us` 執行 matrix；這些選項只覆寫 benchmark 建立的 `RuntimeConfig`，
不改變 production defaults。component ceiling、WAL durable、Publisher drain 與 Engine
end-to-end 結果必須分開報告，不能互相替代。

Release 文件必須另外記錄 recovery benchmark 的參考硬體、Snapshot order count、WAL record count／bytes 與實測時間，用來驗證暫定 60 秒 RTO。RTO 不在不穩定的 shared CI runner 上作硬性 gate；若參考環境無法達成，必須先調整 Snapshot／retention 參數或更新需求，不能忽略結果。

## 17. 安全關閉與資源管理

Engine 使用 RAII 管理 threads 與 file descriptors。正常停止順序：

1. Router 停止接受新 request。
2. Shards 進入 DRAINING。
3. 已接受 command 完成 durable commit 或得到明確 failure。
4. Completion queue drain。
5. Publisher 完成當前 call；不要求等待 downstream catch up 至 WAL head。
6. 持久化最新 cursor／必要 metadata。
7. join `std::jthread` 並關閉 files。

Destructor 不得隱藏可能失敗的完整 shutdown；application 應顯式呼叫 `stop()` 並檢查結果。Destructor 只作 noexcept best-effort cleanup。

## 18. 關鍵決策與取捨

| 決策 | 選擇 | 未選方案與原因 |
| --- | --- | --- |
| Domain concurrency | shard single writer | Core 加 locks 會破壞簡單 determinism，需求也不需要 |
| Price index | `std::map` + FIFO intrusive list | Heap 不利 arbitrary cancel；自製 tree 不必要 |
| Ownership | order map owns stable nodes | 多重 shared ownership 增加成本與生命週期模糊 |
| Ingress queue | bounded mutex queue | Lock-free 複雜且尚無瓶頸證據 |
| Persistence | custom narrow versioned codec | Protobuf／FlatBuffers 增加依賴；直接 dump struct 不安全 |
| Event durability | WAL replay replica | 獨立 event log 需要 atomic commit protocol，超出第一版 |
| Snapshot | synchronous shard pause | COW／fork／background copy 增加一致性與記憶體複雜度 |
| Error model | value result + fail-stop fatal path | Expected rejection 不應用 exception；fatal storage error 不可假裝恢復 |
| Metrics | internal registry + port | 不綁定監控 vendor／network stack |
| Benchmark | internal harness | 需求未要求額外 framework，先維持依賴最小化 |

## 19. 已知限制與擴充方向

### 19.1 已知限制

- Single in-flight producer stream 限制單一 Producer 的 pipeline throughput；可用 bounded producers 並行，但不得為每個 user 建永久 ProducerID。
- 同步 Snapshot 會增加 tail latency，且 Snapshot 頻率預設值需由 benchmark 校正。
- Publisher replica 會增加接近一份 shard state 的記憶體與第二次 execution CPU。
- Static routing 不支援 Hot Instrument 在線搬移。
- Local WAL 沒有節點故障容忍；RPO=0 只針對 process crash 與持久儲存仍可用的情況。
- Metrics 沒有內建 exporter；network integration 由 embedding application 負責。
- 第一版 binary format 只保證明確版本拒絕與 golden tests，尚未承諾跨 major version 自動 migration。

### 19.2 擴充觸發條件

- Queue 經 benchmark 證明為瓶頸後，才考慮 lock-free implementation。
- Snapshot pause 無法滿足已制定 latency SLO 後，才導入 immutable image／background serialization。
- Publisher replica 成本不可接受或 event history 需長期保存後，才設計 transactional durable event output。
- 出現跨節點 availability 要求後，才設計 replicated log／consensus。
- 出現實際 broker requirement 後，才建立獨立 adapter target。
- 需要動態 rebalancing 後，必須先版本化 routing、producer stream fencing 與 migration protocol。

## 20. 實作順序與完成定義

### 20.1 Vertical Slices

1. Public value types、checked arithmetic、OrderBook、matching、invariants。
2. StateMachine 的四種 command、result、events、ProducerState、tombstone。
3. Binary codec、WAL、Snapshot、recovery。
4. ShardRuntime、bounded queue、group commit、query、lifecycle。
5. EventPublisher、cursor、replay Snapshot、retention。
6. Metrics、benchmark、README／CONTRIBUTING／CI 更新。

每個 slice 必須同時完成 tests；不得先建立所有 interface 再留下空 implementation。

### 20.2 Definition of Done

完成條件仍以需求第 20 節的 Functional、Persistence、Determinism、Quality 驗收為準；目前這個切片已具備 Debug／Sanitizer 的自動化基線、Release benchmark smoke，以及 WAL／config／publisher invariant 的回歸測試，但尚未具備完整 fault-injection、subprocess crash、reference/property matrix 或全樹 clang-format 證據。因此目前狀態是「核心修正可驗證」，不是全部 Definition of Done 已完成。

後續仍必須補齊：

- 每個需求驗收項目的可辨識測試與 live／recovery／publisher 三路 determinism comparison；
- append／fsync／Snapshot／cursor failure 的 test-only injection 與 restart matrix；
- Linux GCC／Clang、macOS Apple Clang、ASan／UBSan、clang-tidy、format 與乾淨環境 benchmark smoke 的 CI 證據；
- README、CONTRIBUTING 與本節的狀態同步。

## 21. 需求追溯與範圍審查

| 需求 | 設計對應 |
| --- | --- |
| SCOPE-01～05、DATA-01～12、QUERY-01～06 | 第 5～7 節 Domain model、OrderBook、StateMachine、query |
| SCOPE-06、ARCH-01～05 | 第 3、6、8 節依賴方向、ProducerState、single-writer runtime |
| SCOPE-07 | 第 9～10 節 WAL、Snapshot、Recovery |
| SCOPE-08 | 第 7、11 節 deterministic events、publisher replica、cursor |
| SCOPE-09 | 第 8、12 節 multi-shard runtime、static Instrument config |
| Group commit／capacity defaults | 第 8、11 節 runtime batching、storage pressure |
| Tombstone retention | 第 6.4 節 deterministic eviction |
| Observability | 第 14 節 MetricsRegistry 與 latency boundaries |
| Error Contract、INV-01～11 | 第 7、9～13 節 value errors、fail-stop、codec validation、invariant checker |
| Functional／Persistence／Determinism／Quality | 第 15～16、20 節 tests、benchmark、完成定義 |
| OUT-01～08 | 第 1、2.3、4.3、19 節明確排除，未建立相應元件 |

審查結果：每個產品模組都能追溯到至少一項已確認需求；沒有為未來的 network、broker、HA、risk 或額外 order type 預建 implementation。設計保留的彈性只限於不影響外部行為的 class spelling、小型檔案合併與經 benchmark 證明後的內部資料結構替換。

## 22. 實作調整紀錄

本階段實作相對原始設計有下列必要調整：

1. 原設計要求所有 ID 都使用 strong wrapper；現有 public model 已使用具語意的 fixed-width aliases，只有 composite `OrderId` 使用 value type。需求沒有要求 compile-time non-interchangeability，故保留 aliases 以避免不必要的 codec／API churn；若未來誤傳風險成為實際問題，再另案導入 wrappers。
2. 移除未使用的 `last_command_fingerprint` 與 Snapshot 欄位；canonical command bytes 直接作 duplicate authoritative value。Snapshot format 更新為未發布的 version 2，避免保存冗餘 digest。
3. 原設計的獨立 `WalReader` 以 WAL 內部 mutex-protected decoded sequential index 實作。Publisher 每筆只做 bounded lookup，不重新 decode 整份 WAL；publishable API 傳遞 `WalPosition`，目前以連續 EngineSeq 作安全上界，segment／offset 保留作 durability diagnostics。取捨是 process 內保留 decoded records，換取不新增第二個 storage reader abstraction。
4. Immutable configuration manifest 使用 `instruments-<version>.bin` 與 `behavior-<version>.bin`，採 versioned header、checksum、temporary file、fsync、rename 與 directory fsync；mapping 歷史漂移會拒絕啟動。
5. 新增 `Engine::metrics(ShardId)` 作為唯一明確的 metrics read path，並以 fixed buckets 提供 latency quantile；不新增 exporter 或高基數 label。
6. Crash failpoint／subprocess matrix 尚未加入；目前只宣稱已完成的 WAL corruption、rotation、config compatibility、publisher snapshot invariant、atomicity 與 metrics tests，避免把未驗證的能力當成完成條件。
7. Publisher lag bytes 改由 WAL 內部 sequential index 計算 `(confirmed_cursor, durable_head]` 的實際 record frame bytes；index 不可用或不連續時標記 publisher failure，不把錯誤當成零 lag。`max_publish_lag_age` 的零值在 `Engine::open()` 拒絕，`max_publish_lag_bytes == 0` 保留 disabled 語意。
8. Completion slot 以原始 batch index 保存 admission 與 accepted result，全部 durable/apply/invariant 工作完成後才按 request 順序 dispatch，並由 fatal guard 補齊尚未完成的 callback。
9. Runtime configuration reconciliation 只對 active Instrument 的 immutable 欄位變更或任何 mapping removal／change fail-stop；新增 Instrument 與未有 active order 的欄位更新可建立新 manifest，current behavior version 取本次 supplied configuration，而非 persisted history 最大值。
10. Durable benchmark 將每個 sample 定義為完整 group commit，檢查所有 persistence／recovery 結果並使用每次執行唯一 temporary directory；OrderBook dead helpers、unused guard API、未使用的 WAL durable branch 與過寬 `tmp*` ignore rule 已移除。
11. WAL append hot path 保留 `append()` 相容介面並新增 internal batch append；兩者共用 frame preparation、rotation 與 metadata path。active descriptor 的 RAII lifecycle 消除逐筆 open／close，但 durability boundary 仍是既有 group `fsync`，不引入新的 on-disk format 或 async writer。
