# Order Book / Matching Engine 需求規格

狀態：第二階段設計輸入<br>
版本：1.0-draft<br>
權威性：本文件是 Order Book／Matching Engine 業務需求的唯一來源

## 1. 文件定位

本文件定義第一版 Order Book／Matching Engine 的可觀察行為、正確性契約、持久性語意、操作限制與驗收條件。後續設計與實作不得改變這些語意；若發現需求矛盾，必須先更新本文件再調整設計。

關鍵字：

- 「必須」表示強制需求。
- 「不得」表示明確禁止。
- 「應」表示預期做法；若偏離，設計文件必須記錄理由與影響。
- 標示為「可調」的數值是第一版預設，不是 production SLA。

既有 `docs/project-design.md` 仍只規範第一階段的 C++ 專案骨架。本文件是下一階段設計的輸入，不代表目前已實作業務功能。

## 2. 第一版範圍

### 2.1 包含

| ID | 需求 |
| --- | --- |
| SCOPE-01 | 支援多個 Instrument；每個 Instrument 只屬於一個 shard。 |
| SCOPE-02 | 每個 shard 是 deterministic single-writer state machine。 |
| SCOPE-03 | 支援限價 GTC Order 的 New、Amend Quantity、Replace Price 與 Cancel。 |
| SCOPE-04 | 使用 Price-Time Priority，支援 partial fill 與 multiple match。 |
| SCOPE-05 | 支援 Order lookup、Best Bid／Ask 與 Top-N Depth。 |
| SCOPE-06 | 支援 Producer idempotency、stale producer fencing 與 shard 內 command 全序。 |
| SCOPE-07 | 支援本機 WAL、Snapshot、Crash Recovery 與 deterministic replay。 |
| SCOPE-08 | 支援 deterministic downstream events 與 at-least-once delivery。 |
| SCOPE-09 | 支援同一 process 內的多個獨立 shard；instrument-to-shard routing 由靜態設定提供。 |

### 2.2 不包含

| ID | 非本版需求 |
| --- | --- |
| OUT-01 | Market Order、IOC、FOK、Stop Order、Iceberg Order。 |
| OUT-02 | Account、balance、risk check、self-trade prevention、fee、settlement。 |
| OUT-03 | 動態 shard rebalancing 或不停機搬移 Instrument。 |
| OUT-04 | Replication、Raft、automatic failover 或跨節點 HA。 |
| OUT-05 | Matching Engine 公開網路協定、Gateway 實作或 authentication。 |
| OUT-06 | Kafka 等特定 broker 的內建 adapter。Downstream 只定義 sink contract。 |
| OUT-07 | 跨 shard 的全域 command 或 event 順序。 |
| OUT-08 | Production throughput／latency 保證；第一版只要求可重現量測。 |

未列入第一版的能力可以後續擴充，但不得預先增加未被使用的抽象或依賴。

## 3. 核心架構約束

```text
Upstream Command
      ↓
Structural Validation
      ↓
Producer Idempotency / Fencing
      ↓
Shard Sequencer
      ↓
Durable WAL
      ↓
Single-Writer State Machine
      ↓
Order Book / Matching
      ↓
Deterministic Events
      ↓
Asynchronous Downstream Publisher
```

| ID | 需求 |
| --- | --- |
| ARCH-01 | 同一 shard 同時只能有一個 active writer。 |
| ARCH-02 | 所有 committed command 必須依 `engine_seq` 順序執行。 |
| ARCH-03 | Order Book core 不得依賴網路、資料庫、broker 或特定 application framework。 |
| ARCH-04 | Matching correctness 不得依賴 wall clock、thread scheduling、hash-map iteration order或非持久化外部狀態。 |
| ARCH-05 | 單一 Instrument 不得同時由多個 shard 寫入。 |

## 4. 基本資料定義

### 4.1 Price 與 Quantity

| ID | 需求 |
| --- | --- |
| DATA-01 | Price 必須使用 signed 64-bit integer ticks，不得使用 floating point。 |
| DATA-02 | Quantity 必須使用 signed 64-bit integer lots，不得使用 floating point。 |
| DATA-03 | New Order 的 price 與 quantity 必須大於零，且符合 Instrument 的 tick／lot 規則。 |
| DATA-04 | 所有加總及換算必須檢查 overflow；不得依賴 signed integer overflow。 |

### 4.2 OrderID

| ID | 需求 |
| --- | --- |
| DATA-05 | `order_id` 必須在 shard domain 內唯一。 |
| DATA-06 | Upstream 必須產生不可重用的 OrderID，建議使用具全域唯一性的 128-bit ID。 |
| DATA-07 | Engine 只在 terminal tombstone retention window 內保證偵測已終止的 OrderID；超過後可回傳 `ORDER_NOT_FOUND`。 |

### 4.3 Order 狀態

```text
ACTIVE
PARTIALLY_FILLED
FILLED
CANCELLED
```

`FILLED` 與 `CANCELLED` 為 terminal status。只有仍有 `remaining_quantity > 0` 的 `ACTIVE` 或 `PARTIALLY_FILLED` Order 可以存在於 Price Level queue。

每張 Order 至少具有：

```text
order_id
instrument_id
side
price
total_quantity
remaining_quantity
filled_quantity
status
version
priority_seq
```

必須永遠符合：

```text
ACTIVE / PARTIALLY_FILLED:
total_quantity = remaining_quantity + filled_quantity

FILLED:
total_quantity = filled_quantity
remaining_quantity = 0

CANCELLED:
remaining_quantity = 0
0 <= filled_quantity <= total_quantity
```

`total_quantity` 是最近一次成功 New／Amend／Replace 所接受的總量。Cancelled Order 未成交的取消量可由 `total_quantity - filled_quantity` 得到。

### 4.4 Order Version

| ID | 需求 |
| --- | --- |
| DATA-08 | New Order 建立時 `version = 1`。 |
| DATA-09 | 每個 committed command 對同一 Order 最多增加一次 version。 |
| DATA-10 | 該 command 實際改變 Order 狀態時 version 增加一次。 |
| DATA-11 | Rejection、duplicate retry 與 no-op 不增加 version。 |
| DATA-12 | 一個 command 命中的每個 maker Order 各自最多增加一次 version。 |

## 5. Command Contract

### 5.1 Command 類型

```text
NEW_ORDER
AMEND_QUANTITY
REPLACE_ORDER
CANCEL_ORDER
```

### 5.2 Command Envelope

```text
CommandEnvelope {
    producer_id
    producer_epoch
    producer_stream_id
    producer_seq

    instrument_id
    command_type
    order_id
    expected_version?
    payload
}
```

`expected_version` 為 optional：

- 提供時，實際 version 不相等必須回傳 `VERSION_CONFLICT`。
- 未提供時，操作是 unconditional，但仍必須符合其他狀態規則。

### 5.3 Command Identity

唯一 command identity 為：

```text
ProducerCommandIdentity = (
    producer_id,
    producer_epoch,
    producer_stream_id,
    producer_seq
)
```

第一版固定：

```text
producer_stream_id = shard_id
```

Gateway 必須為每個 shard 維護獨立 Producer Sequence。ProducerSeq 只表示 retry identity；不得用於 matching priority。真正的 execution order 由 `engine_seq` 決定。

### 5.4 單一 In-Flight 限制

同一 `(producer_id, producer_epoch, producer_stream_id)` 同時最多只能存在一個尚未取得 definitive result 的 command。第一版不支援同一 stream 的 pipelined 或 out-of-order delivery。

Engine 必須保存：

```text
ProducerState {
    current_epoch
    last_processed_seq
    last_command_digest
    last_result
}
```

`last_command_digest` 必須由 canonical command content 決定，不得包含 network attempt、接收位址等不穩定資料。

### 5.5 Producer Sequence Validation

對同一 `(producer_id, producer_stream_id)`：

| 條件 | 結果 |
| --- | --- |
| `incoming_epoch < current_epoch` | `STALE_PRODUCER_EPOCH` |
| `incoming_epoch > current_epoch` 且 `seq != 1` | `PRODUCER_SEQUENCE_GAP` |
| `incoming_epoch > current_epoch` 且 `seq == 1` | 開始新 epoch |
| 相同 epoch，`seq == last_seq + 1` | 新 command |
| `seq == last_seq` 且 digest 相同 | retry；回傳原本 `last_result`，不得重做 |
| `seq == last_seq` 且 digest 不同 | `COMMAND_IDENTITY_CONFLICT` |
| `seq < last_seq` | `DUPLICATE_TOO_OLD`，不得重做 |
| `seq > last_seq + 1` | `PRODUCER_SEQUENCE_GAP` |

Retry 必須使用完全相同的四元組與 payload。

## 6. Admission、Commit 與 Rejection

### 6.1 不進入 durable history 的 Admission Error

以下錯誤不得消耗 ProducerSeq、不得分配 committed EngineSeq、不得產生 event：

- request 無法解析或超過 envelope size limit；
- 缺少 identity／routing 必要欄位；
- `producer_stream_id` 與目標 shard 不符；
- stale epoch、sequence gap、identity conflict、duplicate too old；
- shard 尚未 ready 或已 fail-stop。

### 6.2 進入 durable history 的 Business Rejection

通過 structural 與 producer validation 的新 command，即使因下列原因被拒絕，也必須取得 EngineSeq、寫入 WAL、推進 ProducerState 並可 deterministic replay：

- Unknown Instrument；
- 無效 price／quantity；
- 重複 OrderID；
- Order 不存在或已 terminal；
- ExpectedVersion conflict；
- Amend／Replace 違反狀態規則；
- shard active-order capacity 已滿。

這些 command 必須回傳穩定結果；retry 必須得到相同 logical result。

### 6.3 Durable Commit Flow

```text
Parse and Structural Validation
      ↓
Producer Identity / Sequence Validation
      ↓
Assign tentative EngineSeq
      ↓
Append canonical command to WAL
      ↓
fsync / Durable Commit
      ↓
Execute deterministic business validation and state transition
      ↓
Advance ProducerState
      ↓
Generate result and events
      ↓
ACK upstream
```

只有 WAL durable 後，command 才是 committed。WAL append／fsync 失敗時：

- 不得回覆成功；
- shard 必須進入 fail-stop／unavailable；
- recovery 只以最後一筆有效 committed WAL record 為準；
- 未 committed 且未對外可見的 tentative EngineSeq 可以在 recovery 後重新使用。

## 7. Matching Rules

### 7.1 Price Priority

- BUY：較高價格優先。
- SELL：較低價格優先。

BUY crossing 條件：

```text
buy_price >= best_ask
```

SELL crossing 條件：

```text
sell_price <= best_bid
```

成交價格必須使用 resting maker Order 的價格。

### 7.2 Time Priority

同一 Price Level 內依 `priority_seq` FIFO。`priority_seq` 使用 Order 當次進入 queue 的 EngineSeq；同一 command 不會把兩張相同 Order 放入 queue，因此不需要額外 tie breaker。

### 7.3 Matching

- Aggressive Order 必須從 opposite side 的最佳 Price Level 開始。
- 同一 level 必須從 FIFO head 開始。
- 每次成交量為 maker 與 taker remaining quantity 的較小值。
- 必須持續撮合，直到 taker 完全成交或不再 crossing。
- 有剩餘量的 taker 才能加入自身 side 的 Price Level。

## 8. Command 行為

### 8.1 New Order

Payload：

```text
{ side, price, quantity }
```

行為：

1. 驗證 Instrument、OrderID、side、price、quantity 與 capacity。
2. 依 Price-Time Priority 撮合。
3. 剩餘量大於零時加入 Order Book。

最終 Order status：

- 無成交且進入 book：`ACTIVE`。
- 部分成交且仍有剩餘：`PARTIALLY_FILLED`。
- 完全成交：`FILLED`。

### 8.2 Amend Quantity

Payload 固定為：

```text
{ new_total_quantity }
```

規則：

| 條件 | 行為 |
| --- | --- |
| `new_total_quantity <= 0` | `INVALID_QUANTITY` |
| `new_total_quantity < filled_quantity` | `INVALID_QUANTITY` |
| 等於目前 total quantity | 成功 `NO_CHANGE`；不改 version／priority，不產生 event |
| 小於目前 total 且大於 filled | 減少 remaining quantity；保留 priority |
| 等於 filled quantity | 移除全部 remaining quantity；狀態改為 `CANCELLED` |
| 大於目前 total | 增加 remaining quantity；移到同價格 queue 尾端並重設 priority |

Amend 不改價格，不需要重新 crossing matching。Terminal Order 不可 Amend。

### 8.3 Replace Order

Payload：

```text
{ new_price, new_total_quantity? }
```

- 未提供 `new_total_quantity` 時保留目前 total quantity。
- 新 total quantity 必須大於零且不得小於 filled quantity。
- 必須在移除舊 Order 前完成所有 validation；失敗時舊 Order 完全不變。
- 成功 Replace 是同一 OrderID 的 atomic state transition，必定失去舊 priority。
- 移除舊 queue position 後，使用新價格重新進入 matching。
- 有剩餘量才加入新 Price Level。
- Terminal Order 不可 Replace。

如果新 total quantity 等於 filled quantity，Replace 沒有可重新進入市場的剩餘量，必須以 `INVALID_QUANTITY` 拒絕；需要移除剩餘量時應使用 Cancel 或 Amend。

### 8.4 Cancel Order

- Active／Partially Filled Order：移出 queue，剩餘量歸零，狀態改為 `CANCELLED`。
- Terminal Order tombstone 尚存在：`ORDER_ALREADY_TERMINAL`。
- Order 完全未知或 tombstone 已過期：`ORDER_NOT_FOUND`。
- 相同 Cancel command 的 retry 回傳原始成功結果，不得轉為 terminal error。

### 8.5 Command Result

Response 必須分開表達 command 與 Order 狀態：

```text
CommandResult {
    command_identity
    engine_seq?
    command_status   // COMMITTED, REJECTED, NO_CHANGE, ADMISSION_ERROR
    error_code?
    order_id?
    order_status?
    order_version?
    remaining_quantity?
    filled_quantity?
}
```

Admission Error 沒有 committed EngineSeq。Business Rejection 的 `command_status = REJECTED` 且具有 committed EngineSeq。

## 9. Order Book 與查詢

概念資料結構：

```text
OrderBook
├── orders: HashMap<OrderID, Order*>
├── bids: ordered PriceLevel index
├── asks: ordered PriceLevel index
├── best_bid
└── best_ask

PriceLevel
├── price
├── total_quantity
├── order_count
└── FIFO orders
```

具體 C++ container 由設計決定，但必須滿足：

| ID | 複雜度需求 |
| --- | --- |
| QUERY-01 | Active Order lookup：平均 O(1)。 |
| QUERY-02 | 已知 Order 的一般 Cancel：O(1)；若 level 清空，允許額外 O(log P)。 |
| QUERY-03 | Price Level lookup／insert／remove：O(log P)。 |
| QUERY-04 | Best Bid／Ask：O(1)。 |
| QUERY-05 | Top-N Depth：O(N)，每層回傳 price、total quantity、order count。 |
| QUERY-06 | Matching：O(K + L log P)，K 為被撮合 Order 數，L 為被清空 level 數。 |

查詢必須回傳 shard 已完成 command 的一致狀態，不得觀察到 command 執行一半的中間狀態。跨 shard snapshot consistency 不在第一版範圍。

## 10. Sharding 與 EngineSeq

- Instrument 到 shard 的 mapping 必須由版本化靜態設定決定。
- Instrument 設定至少包含 tick size 與 lot size，必須版本化並持久化；WAL replay 必須使用 command 當時的設定版本。
- 已被 WAL 或 Snapshot 引用的 Instrument 設定版本不得在相關資料截斷前刪除或就地修改。
- Hot Instrument 可以獨占 shard；低流量 Instruments 可以共享 shard。
- 第一版變更 mapping 需要停止相關 shard、完成 recovery／migration 程序後再啟動；不支援線上搬移。
- `engine_seq` 為 shard-local unsigned 64-bit sequence，第一筆為 1，committed records 必須連續且嚴格遞增。
- 達到最大值時必須 fail-stop，不得 wrap。
- Price-Time Priority 只依 shard execution order，不使用 timestamp。

## 11. WAL

每個 shard 維護獨立 WAL。每筆 record 至少包含：

```text
record_length
format_version
shard_id
engine_seq
command_received_at
instrument_configuration_version

producer_id
producer_epoch
producer_stream_id
producer_seq

instrument_id
command_type
order_id
expected_version?
canonical_payload

checksum
```

要求：

- Checksum 使用適合偵測 torn write／corruption 的演算法；第一版建議 CRC32C。
- WAL 必須按 EngineSeq 寫入。
- Canonical command 必須包含 deterministic replay 所需的全部資料。
- `command_received_at` 只供 event、audit 與 observability 使用，不參與 matching。

### 11.1 Group Commit 預設

```text
group_commit_max_delay = 200 microseconds
group_commit_max_commands = 256
```

達到任一條件即 flush + fsync。兩者必須可設定；delay 設為零表示每筆 command 單獨 durable commit。

### 11.2 Corruption Policy

- 最後一筆不完整 record 視為 crash tail，可以截斷。
- 完整 record checksum 錯誤必須視為資料毀損並停止啟動。
- WAL 中間出現 EngineSeq gap、重複或倒退，必須停止啟動。
- 不得自動跳過 corrupt record 後繼續服務。

## 12. Snapshot、Recovery 與 Retention

### 12.1 Snapshot 內容

Snapshot 至少保存：

```text
format_version
shard_id
snapshot_engine_seq
instrument_configuration_version

all active orders
price levels and aggregates
FIFO ordering
order versions and priority sequences

all ProducerState entries
retained terminal tombstones
checksum / integrity metadata
```

ProducerState 必須包含 last command digest 與 last result，確保 snapshot 後 retry 語意不變。

### 12.2 Snapshot Crash Safety

```text
write snapshot.tmp
→ fsync(snapshot.tmp)
→ atomic rename to final name
→ fsync(parent directory)
```

只有完成全部步驟後，Snapshot 才是 durable。Shard ID、format version、checksum 或 Instrument configuration 不相容時必須拒絕載入。

### 12.3 Recovery

```text
Load latest valid snapshot
→ restore state at snapshot_engine_seq
→ replay valid WAL records with greater EngineSeq
→ validate all invariants
→ next EngineSeq = last committed EngineSeq + 1
→ become ready
```

對相同 Snapshot + WAL，必須得到完全相同的 Order Book、ProducerState、results、TradeIDs 與 EventIDs。

### 12.4 預設與目標

```text
wal_segment_size = 256 MiB
snapshot_interval_time = 5 minutes
snapshot_interval_commands = 1,000,000
wal_soft_limit_per_shard = 64 GiB
```

時間或 command 數任一條件先到即觸發 Snapshot。以上均為可調預設。

第一版暫定：

- Committed command RPO = 0：已回覆 committed 的 command 不得因 process crash 遺失。
- Recovery RTO = 60 秒：在文件化的參考硬體與資料集上，從啟動到 ready 應在 60 秒內完成；正式數值須由 benchmark 校正。

WAL 只能在同時滿足 state recovery 與 event replay 要求後截斷，不得為遵守容量上限而刪除必要資料。

## 13. Terminal Tombstone

Terminal Order 從 active book 移除後，保存精簡資料：

```text
TerminalOrderTombstone {
    order_id
    final_status
    final_version
    terminal_engine_seq
}
```

預設：

```text
terminal_tombstone_max_age = 1 hour
terminal_tombstone_max_count = 1,000,000 per shard
```

任一上限先達到即可淘汰最舊項目。Tombstone 必須隨 Snapshot 持久化，使 retention window 內的 restart 不改變查詢語意。它不是永久 Order history；長期歷史由 downstream 系統負責。

## 14. Deterministic Events

### 14.1 Event Identity

```text
EventID = (shard_id, engine_seq, event_index)
TradeID = (shard_id, engine_seq, trade_index)
```

`event_index` 與 `trade_index` 均從零開始，按 deterministic generation order 遞增。

### 14.2 Event Types

第一版固定三種：

```text
TRADE
ORDER_UPDATED
COMMAND_REJECTED
```

`BOOK_UPDATED` 不在第一版輸出；Market Data adapter 可以由 Order／Trade events 衍生 book updates。

### 14.3 Event Envelope

```text
EventEnvelope {
    event_id
    shard_id
    engine_seq
    event_index
    command_identity
    instrument_id
    event_type
    occurred_at
    payload
}
```

`occurred_at` 必須取自 WAL 中的 `command_received_at`，replay 時不得重新讀取 wall clock。

Trade payload 至少包含：

```text
trade_id
price
quantity
maker_order_id
taker_order_id
maker_side
maker_remaining_quantity
taker_remaining_quantity
```

OrderUpdated payload 至少包含：

```text
order_id
status
side
price
total_quantity
remaining_quantity
filled_quantity
version
update_reason
```

CommandRejected payload 至少包含 command type、order ID（若有）與 error code；不得包含 replay 時可能變動的錯誤文字作為 correctness 欄位。

### 14.4 Event Generation Order

一個 committed command 必須依序產生：

1. 每筆成交各一個 `TRADE`，順序與 matching 順序相同。
2. 每個受影響 maker 各一個 `ORDER_UPDATED`，順序與 maker 首次被 match 的順序相同。
3. Command target Order 若狀態有改變，最後產生一個 `ORDER_UPDATED`。

Business Rejection 只產生一個 `COMMAND_REJECTED`。No-op command 不產生 event。所有 event 的內容與數量必須能由 command 執行前狀態加 canonical command 唯一決定。

## 15. Downstream Delivery

Delivery semantics：

```text
at-least-once delivery
+ deterministic EventID
+ idempotent consumer
```

不得宣稱 network exactly-once。Consumer 必須使用 EventID 去重；若 consumer 同時修改 business state，兩者應在同一 transaction 內提交。

### 15.1 Publisher Cursor

第一版使用 command-level durable cursor：

```text
publisher_cursor = last_confirmed_engine_seq
```

- 同一 EngineSeq 的 events 作為一個 publish batch。
- 整批獲得 downstream durable acknowledgment 後才能推進 cursor。
- ACK 遺失時允許重送整批 events。
- 零 event command 仍必須按 EngineSeq 推進 cursor。

### 15.2 Event Replay Snapshot

因 Event 是 `f(previous_state, command)`，publisher 必須保留一個不晚於 cursor 的 deterministic replay base：

```text
event_replay_snapshot_seq <= publisher_cursor
```

並保留其後所有必要 WAL。安全截斷 watermark 不得大於：

```text
min(
    durable_state_recovery_snapshot_seq,
    durable_event_replay_snapshot_seq
)
```

僅保留「最新 state snapshot + cursor 後 WAL」不足以重建 cursor 之前開始執行之 command 的 events。

### 15.3 Downstream 離線預算

可調預設：

```text
max_publish_lag_age = 24 hours
max_publish_lag_bytes = 32 GiB per shard
```

- 50% 預算時發出 warning。
- 80% 預算時發出 critical alert。
- 超過預算不代表可以丟棄 event。
- 無法再安全持久化時，shard 必須停止接受 mutating command，回傳 `ENGINE_STORAGE_PRESSURE`／`ENGINE_UNAVAILABLE`，不得造成 event 永久遺失。

## 16. Capacity 與效能

### 16.1 可調安全上限

```text
max_active_orders_per_shard = 1,000,000
max_instruments_per_shard = 1,024
```

這些是初始保護值，不是已驗證的 production capacity。

- 達 active-order 上限時，Cancel、Amend decrease 與查詢仍必須可用。
- 可能新增 resting Order 的 command 可以回傳 `SHARD_CAPACITY_EXCEEDED`。
- Instrument 分配必須依 command rate、active orders、active levels、queue depth、WAL latency 與 command tail latency 調整。
- Hot Instrument 可以獨占 shard。

### 16.2 Benchmark 要求

第一版不設定固定 commands/second，但必須能分別量測：

- in-memory matching；
- durable WAL path；
- resting New、crossing New、Cancel、Amend、Replace、multiple match；
- commands/second、trades/second、CPU、active orders、queue depth；
- p50、p99、p99.9 與 max latency。

Benchmark 報告必須記錄硬體、OS、compiler、build mode、fsync 模式、資料集與測試持續時間，避免把不可比較的數字當成 SLA。

## 17. Observability

至少提供下列 counter／gauge／histogram：

```text
command_count by type/result
command_queue_latency
command_wal_commit_latency
command_execution_latency
command_end_to_end_latency

ingress_queue_depth
active_orders
active_price_levels
active_instruments
match_count
trade_count

wal_append_latency
wal_fsync_latency
wal_size_bytes
snapshot_duration
snapshot_size_bytes
recovery_duration
replayed_record_count

duplicate_command_count
sequence_gap_count
stale_epoch_count
identity_conflict_count

event_publish_latency
event_publish_lag_events
event_publish_lag_bytes
event_publish_lag_age
event_retry_count
```

Latency histogram 至少輸出 count、p50、p99、p99.9 與 max，並能按 command type 區分。

## 18. Error Contract

至少區分：

```text
INVALID_ENVELOPE
INVALID_COMMAND
UNKNOWN_INSTRUMENT
WRONG_PRODUCER_STREAM
INVALID_SIDE
INVALID_PRICE
INVALID_QUANTITY
NUMERIC_OVERFLOW

DUPLICATE_ORDER_ID
ORDER_NOT_FOUND
ORDER_ALREADY_TERMINAL
VERSION_CONFLICT
SHARD_CAPACITY_EXCEEDED

STALE_PRODUCER_EPOCH
PRODUCER_SEQUENCE_GAP
COMMAND_IDENTITY_CONFLICT
DUPLICATE_TOO_OLD

WAL_FAILURE
CORRUPT_WAL
CORRUPT_SNAPSHOT
ENGINE_STORAGE_PRESSURE
ENGINE_UNAVAILABLE
```

Error response 必須包含 machine-readable code。Human-readable message 可提供，但不得成為 retry、replay 或測試 correctness 的唯一依據。

## 19. 核心 Invariants

| ID | Invariant |
| --- | --- |
| INV-01 | 同一 active OrderID 最多存在一次。 |
| INV-02 | `PriceLevel.total_quantity` 等於 level 內所有 live Order remaining quantity 總和。 |
| INV-03 | `PriceLevel.order_count` 等於 FIFO queue 中 Order 數量。 |
| INV-04 | 同 Price Level 的 Order 按 priority sequence 排序。 |
| INV-05 | Active Order index 必須指向 Price Level queue 中的同一 Order。 |
| INV-06 | Bid／Ask 不得在一個 command 完成後維持 crossed 狀態。 |
| INV-07 | 所有 state mutation 依 committed EngineSeq 執行。 |
| INV-08 | 每個 committed EngineSeq 都具有 durable WAL representation。 |
| INV-09 | 相同 ProducerCommandIdentity 最多執行一次 state transition。 |
| INV-10 | 相同 Snapshot + WAL 產生相同 state、result、events、EventIDs 與 TradeIDs。 |
| INV-11 | Event publish failure 只能造成 delay 或 duplicate，不得造成永久遺失。 |

Debug build 與測試應提供可執行的 invariant validation；production 是否持續執行完整檢查由設計依成本決定。

## 20. 驗收要求

### 20.1 Functional

必須以自動化測試覆蓋：

- Price priority、FIFO priority、partial fill、multiple match；
- New、Amend decrease／increase／to-filled、Replace crossing、Cancel；
- ExpectedVersion success／conflict；
- duplicate retry、payload conflict、sequence gap、stale epoch；
- active-order capacity；
- Best Bid／Ask、Top-N、aggregate quantity；
- 所有 business rejection 的 durable replay。

### 20.2 Persistence

必須測試 crash point：

- WAL append 前；
- append 後、fsync 前；
- fsync 後、apply 前；
- apply 後、response 前；
- event publish 前；
- broker success 後、cursor persist 前；
- Snapshot temporary write、fsync、rename 各階段。

必須驗證 truncated tail 可恢復，而 middle-record corruption、checksum failure、sequence gap 會 fail-stop。

### 20.3 Determinism

相同 Snapshot 與 WAL 必須可重複 replay 多次，逐 byte 或逐欄位比較：

- final Order Book state；
- ProducerState 與 duplicate result；
- command results；
- event count、order、payload、EventID、TradeID。

### 20.4 Quality

- Domain unit tests 不得依賴 wall clock、network 或執行順序。
- 需包含 property／model-based tests 驗證數量守恆與 invariants。
- 需包含 WAL／Snapshot format version compatibility tests。
- 需在 ASan／UBSan 下執行主要測試。
- 需提供可重現 benchmark，但 benchmark regression threshold 待取得穩定 baseline 後另定。

## 21. 未來擴充方向

後續可在不改變本文件核心 correctness contract 的前提下加入：

- durable event log／transactional outbox；
- Kafka 等 broker adapter；
- dynamic shard migration；
- replication／Raft／HA；
- 其他 order type 與 time-in-force；
- production capacity 與 latency SLO。

任何擴充若改變 Producer stream、EngineSeq、OrderID、event identity 或 durability 語意，必須先版本化協定並更新需求與 migration 規則。
