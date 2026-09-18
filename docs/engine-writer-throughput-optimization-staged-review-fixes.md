# Engine Writer Throughput Optimization staged review 必要修正

## 1. 文件目的

本文件記錄 `docs/engine-writer-throughput-optimization-design.md` 與目前 staged changes
審查後確認的必要修正，讓設計、實作、測試與效能驗收使用相同契約。

修正只處理以下問題：

1. incremental validation 尚未完整覆蓋 touched transition；
2. tombstone eviction 可能隱藏損壞，且前置 eviction 未被驗證；
3. 新增防線與 metrics ownership 缺少必要的負向／整合測試；
4. 尚未執行設計要求的 group-commit Pareto 驗收。

以下內容不在本次修正範圍：async WAL、WAL format、Snapshot format、lock-free queue、
typed metrics、Publisher batching、Completion 語意、production group-commit defaults，以及任何
新的業務功能。

## 2. Review 結論與優先順序

| 優先度 | 項目 | 問題 | 必要性 |
| --- | --- | --- | --- |
| P1 | touched transition validation | 未驗證 trade quantity conservation、嚴格 terminal status、event payload 與 touched level aggregate | 移除每 group full scan 後的主要 correctness 防線，必修 |
| P1 | tombstone eviction | 缺失 index 會被靜默移除，且 `apply()` 前置 eviction 結果被丟棄 | 可能掩蓋 deterministic state corruption，必修 |
| P2 | correctness／ownership tests | 現有新增測試只有正常成交與 counter 累加 | 無法證明上述防線及 registry split 契約，必修 |
| 驗收 | Pareto benchmark/report | 尚無正式 size／delay matrix 結果 | 不需先改 production code，但完成本設計前必做 |

上述項目皆直接來自原設計第 7、10、11 節。除此之外，不應在本次修正中順帶進行效能
micro-optimization 或架構重構。

## 3. 修正一：補齊 touched transition postconditions

### 3.1 現況

目前 `StateMachine::validate_transition()` 已驗證：

- EngineSeq、command identity 與 ProducerState；
- active count delta；
- touched active order 可由 book 與 location index 找到；
- terminal order 已由 book／location 移除且 tombstone 基本欄位相符；
- EventID header 連續。

但它沒有讀取 `OrderBookApplyResult::trades`，也沒有 transition 前的 touched order／level 資訊，
因此下列錯誤仍可能通過：

- maker 扣除量與 taker 增加量不同；
- trade quantity 與 maker／taker remaining quantity 不一致；
- `filled` order 仍有非零 remaining quantity；
- maker／target update 與產生的 event payload 不一致；
- touched price level 的 total quantity 沒有套用正確 delta；
- `terminal_orders` 含有未由 target／maker transition 產生的額外項目。

### 3.2 最小資料設計

不要把驗證 metadata 加入 public `CommandResult`、Event、WAL 或 Snapshot。只在 `src/domain`
內加入 command-lifetime 的 bounded transition summary。

建議在 `OrderBookApplyResult` 旁加入 internal-only 結構，名稱可依實作調整：

```cpp
struct TouchedOrderTransition {
  std::optional<OrderView> before;
  OrderView after;
};

struct TouchedLevelTransition {
  Side side;
  Price price;
  Quantity total_before;
  Quantity expected_total_after;
};
```

`OrderBookApplyResult` 只保存本次實際 touched 的 orders／levels。資料量上限與 match steps 成
正比，不得保存整本 book snapshot。

若不希望把 summary 放進 `OrderBookApplyResult`，可由 `OrderBook` 的 mutation helper 在返回前
完成 order／level postcondition，然後讓 `StateMachine` 只處理跨元件的 location、tombstone、
producer、result 與 events。兩種方式擇一，不要同時建立兩套驗證。

### 3.3 OrderBook 層必要檢查

對 New／Amend／Replace／Cancel 及每個 match step 驗證：

1. quantity arithmetic 不溢位；
2. `after.total == after.remaining + after.filled`；
3. status 嚴格符合：
   - `active`: `filled == 0 && remaining > 0`；
   - `partially_filled`: `filled > 0 && remaining > 0`；
   - `filled`: `remaining == 0 && filled == total`；
   - `cancelled`: `remaining == 0`；
4. 每筆 trade 的 quantity 大於零；
5. maker：`before.remaining - trade.quantity == after.remaining`，filled quantity 同量增加；
6. taker：所有 trade quantity 加總等於 taker filled quantity 的增量；
7. trade 的 maker/taker IDs、price、side、逐步 remaining quantities 與對應 transition 一致；
8. order version 與 priority 規則符合該 command；
9. touched level 實際 total 等於 `expected_total_after`。

為第 9 點可新增 private/internal `OrderBook::level_total(Side, Price)`，以 map lookup 取得保存的
aggregate，成本為 price-level lookup，不遍歷該 level 或整本 order book。`total_before` 與所有
touched-order delta 必須使用 checked arithmetic 算出 `expected_total_after`。

### 3.4 StateMachine 層必要檢查

在現有 `validate_transition()` 補上：

- `CommandResult` 的 status、order ID、version、remaining／filled 與 outcome target 一致；
- committed／no-change／rejected result 只能對應合法 outcome；
- maker updates、target 與 terminal orders 是完整且無重複的對應關係；
- `active_delta` 由 transitions 推導出的增減量一致，而不只相信 outcome 提供的 delta；
- 每個 trade event payload 與 `outcome.trades` 一一相同；
- 每個 order-updated event payload 與 maker updates／changed target 一一相同；
- rejected command 只能產生一個與 result error code 相同的 rejection event；
- event count、type、payload 與 EventID ordering 同時一致。

不要新增跨 command cache、probabilistic checksum 或第二套 matching implementation。

### 3.5 複雜度與驗收

修正後仍須符合：

```text
time   = O(touched orders + trades + events + evicted tombstones)
memory = O(touched orders + touched levels)
```

不得呼叫 `snapshot_orders()`、掃描所有 books、order locations、producer states 或 tombstones。

預估 production 修改約 50～90 行；若需要加入 internal transition summary，結構宣告與填值約再
增加 20～35 行。

## 4. 修正二：讓 tombstone eviction fail-stop 且可驗證

### 4.1 現況

目前 `evict_tombstones()` 遇到 `tombstone_order.front()` 指向不存在的 tombstone 時會直接
`pop_front()`。這會把 corrupt index 修掉後繼續執行，而不是回報 invariant failure。

此外，`StateMachine::apply()` 更新 logical retention time 後執行的第一次 eviction 直接丟棄
回傳結果；只有 command mutation 後的第二次 eviction 會傳給 validator。因此 validator 無法覆蓋
整個 command transition 中發生的 eviction。

### 4.2 具體修改

將 helper 改為可回傳錯誤：

```cpp
[[nodiscard]] Result<std::vector<OrderId>> evict_tombstones();
```

每次處理 front 時，刪除前依序檢查：

1. order ID 在 `tombstones` 中存在；不存在立即回傳 `corrupt_snapshot`；
2. queue sequence 等於 tombstone 的 `terminal_engine_seq`；
3. tombstone ID、terminal status、version 與 terminal time 合法；
4. `terminal_time <= logical_retention_time`；
5. 只有 `expired || over_count` 時才可刪除；
6. 若 front 不符合 eviction policy，立即停止，不能跳過它去刪後面的項目。

呼叫端處理規則：

- `apply()` 的前置 eviction 若失敗，立即回傳 fatal core error；
- `reject()`、New 與 existing-order path 的後置 eviction亦同；
- 前置 eviction 不得移除，因為它會影響已超過 retention policy 的 order ID 是否可重新使用；
- 後置 eviction仍保留，用來處理本 command 新增 tombstone 後超過 count limit 的情況；
- validator 只需要收到後置 eviction IDs，以判斷本 command 剛產生但立即被 eviction 的 terminal
  order；前置 eviction correctness 已由 helper 自身完成。

在 `record_terminal()` 後、post-command eviction 前，以 bounded suffix check 驗證本次新增的
`tombstone_order` entries：sequence、order ID 與 map entry 必須一致。檢查數量只等於本次
`terminal_orders.size()`。

不得掃描完整 tombstone queue，也不得改變 retention age／count 或 order-ID reuse semantics。

預估 production 修改約 20～35 行。

## 5. 修正三：補上最小必要測試

### 5.1 StateMachine transition tests

擴充 `tests/unit/state_machine_test.cpp`，至少覆蓋：

- New resting order；
- Amend decrease、increase、terminal cancel；
- Replace 到新 price、replace crossing；
- Cancel；
- single match、multiple makers、partial maker／partial taker、maker 與 taker 同時 terminal；
- no-change 與 duplicate／unknown instrument／version conflict rejection；
- 每個案例核對 result、events、producer state，最後呼叫 `validate_state()` 作 oracle。

每個測試只驗證該 command 的必要 postconditions，不重複測試 OrderBook 已有的所有業務案例。

### 5.2 Corruption 與 eviction tests

利用現有 mutable `StateMachine::state()` 建立負向案例，不增加 public test API：

- touched order location 指向錯誤 instrument；
- `active_order_count` 與 location size 不一致；
- tombstone queue front 指向不存在 ID；
- tombstone queue sequence 與 entry sequence 不一致；
- 未達 age/count policy 的 tombstone 不被刪除；
- age expiry 與 count overflow 只刪除合資格 prefix；
- 一個 command 同時觸發前置與後置 eviction。

預期結果必須是 typed fatal invariant error，不得靜默修正後回傳成功。

若需要直接製造 private price-level aggregate corruption，只能增加 tests-only narrow peer；不得把
mutation hook 加入 public `OrderBook` API。若以 transition summary 的 before/after delta 測試已能
覆蓋該錯誤，則不要新增 test peer。

### 5.3 Metrics ownership／merge tests

擴充 integration fixture，以可控制 ACK 的 EventSink 驗證：

1. 第一批 event ACK 後讓第二批 publish 暫停；
2. writer 繼續產生 durable backlog；
3. `Engine::metrics()` 同時保留 command partition 的 commands、trades、WAL totals；
4. replayed records、publish latency 與 lag gauges 來自 publisher partition；
5. 解除 ACK 阻塞並等待 Publisher drain；
6. lag events／bytes／age 最終都回到零，不殘留 command partition 的 stale gauge。

現有 `MetricsRegistry` counter unit test保留。不要為此新增通用 metrics merge framework；合併規則
只有目前公開 `MetricsSnapshot` 中的 Publisher-owned 欄位。

### 5.4 Snapshot boundary tests

驗證 live Snapshot 與 Publisher replay Snapshot 都是先 `validate_state()`、成功後才 write。若現有
fixture 無法注入損壞 state，可在 private class 中加入只對測試可見的 narrow test peer／friend，
不得加入 public runtime API或新的 Snapshot abstraction。

至少確認：

- validation failure 使對應 worker fail-stop；
- snapshot sequence／檔案不前進；
- failure 後不再回報成功 completion或發布後續 event。

### 5.5 Benchmark CLI tests

在 benchmarks 與 testing 同時啟用時，以 CTest 執行不會開始 workload 的 invalid CLI cases：

```text
--engine-producer-lanes=0
--engine-producer-lanes=abc
--engine-producer-lanes=65537
```

逐一驗證 exit code `2` 與對應 error code。合法值由正式 benchmark run 覆蓋。不要為測 parser
把 benchmark CLI 搬入 production library。

全部必要測試預估約 120～200 行。

## 6. Pareto benchmark 與報告

這一項在 production correctness 修正及 regression tests 通過後執行，不先修改 runtime defaults。

### 6.1 第一階段：saturated size sweep

固定 `max_delay=1000 us`，producer lanes 至少等於候選 group，依序測：

```text
group size = 256, 512, 1024, 2048, 4096, 8192
```

每個 case：

- Release build；
- 固定 CPU affinity、filesystem 與 WAL device；
- 五輪；
- measured phase至少 10 秒；
- 每輪使用新的 data directory；
- 保留 durability、completion identity/count、WAL reopen/replay驗證。

### 6.2 第二階段：相鄰 Pareto candidates

只選 throughput 開始平台化或首次違反 latency SLO 附近的二至三個 group size，再測：

```text
producer lanes = 1, 8, 64, 256, 1024, candidate group size
max delay      = 200 us, 500 us, 1 ms, 2 ms, 5 ms
```

不執行沒有資訊增益的完整 Cartesian product。

### 6.3 報告必要欄位

新增 `docs/engine-writer-throughput-optimization-benchmark-report.md`，至少記錄：

- host、CPU、compiler、Release flags、filesystem、WAL device；
- iterations／duration、producer lanes、configured group／delay；
- 五輪 throughput、median、min／max；
- p50／p99／p99.9／max durable completion latency；
- actual commands/group、WAL MiB/s、segment rotations；
- replay verification與 Publisher drain結果；
- 是否通過 p99 20 ms、p99.9 50 ms參考門檻；
- 依「uplift 小於 5% 時選較小 group」規則得到的建議值；
- 若仍未達 1M/s，記錄下一個有量測證據的瓶頸。

報告約 60～100 行；實際數字必須由測試輸出產生，不得預填或推估。

## 7. 實作順序

依下列順序執行，避免以尚未可信的 benchmark 選 production 設定：

1. 修正 tombstone eviction fail-stop；
2. 補齊 touched transition validation；
3. 新增 StateMachine 負向與 transition tests；
4. 新增 metrics ownership、Snapshot boundary與 CLI tests；
5. 執行 GCC／Clang warnings-as-errors、Release tests、ASan／UBSan；
6. 執行 StateMachine 0／1K／10K／100K state-size benchmark；
7. 執行 group size／delay Pareto matrix並撰寫報告；
8. 只有報告支持時，另案調整 production defaults。

## 8. 完成條件與修改量

完成條件：

- incremental checker 覆蓋 quantity、level、result、event與 tombstone touched invariants；
- invariant failure一律 fail-stop，不靜默修復 corruption；
- hot path沒有 full-state／full-book／full-tombstone scan；
- registry ownership合併與 lag drain有 integration test；
- WAL、Snapshot、Event與 public API format完全不變；
- production default仍為既有值；
- correctness、sanitizer與正式 benchmark報告皆完成。

預估必要修改量：

| 類型 | 預估行數 |
| --- | ---: |
| production code | 70～125 |
| tests／CTest wiring | 120～200 |
| benchmark report | 60～100 |
| 合計 | 約 250～425 |

行數會因 internal transition summary 的放置方式而變動；不得為追求較少行數而省略 invariant，也
不得為追求完整框架而擴張成通用 transaction、metrics 或 storage injection architecture。
