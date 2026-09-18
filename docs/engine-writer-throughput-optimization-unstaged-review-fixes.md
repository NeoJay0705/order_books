# Engine Writer Throughput Optimization unstaged review 必要修正

## 1. 文件目的

本文件記錄 `docs/engine-writer-throughput-optimization-design.md`、
`docs/engine-writer-throughput-optimization-staged-review-fixes.md` 與目前 unstaged changes
交叉審查後，仍需完成的必要修正。

修正目標是同時滿足：

1. incremental validation 保留原設計要求的 exact postconditions；
2. validator 成本隨 touched entities 線性成長，且一般單筆 command 不因驗證額外配置多個 heap
   container；
3. deterministic corruption 一律 fail-stop，不轉成正常業務 rejection；
4. tests 真正等待非同步狀態完成，且覆蓋設計要求的 corruption／Snapshot boundaries；
5. 不改 WAL、Snapshot、Event、CommandResult 或公開 API format。

以下不在本次修正範圍：async WAL、lock-free queue、Publisher batching、Completion 語意、production
group-commit defaults、新 metrics framework及新業務功能。

## 2. Review 結論與量測證據

目前 tombstone fail-stop、quantity/result/event validation 與 invalid CLI tests 的方向正確，但
incremental validator 的實作不是最小熱路徑方案，且仍有 corruption 分類與非同步測試缺口。

以相同 Release binary configuration、`state_machine/crossing_pair`、batch 256、0 preloaded active
orders、每輪 512,000 commands 作五輪短測：

| 版本 | 五輪中位數 |
| --- | ---: |
| staged/index | 1,023,750 commands/s |
| 加入目前 unstaged changes | 701,758 commands/s |

短測顯示約 31.5% 退化。這不是正式 benchmark report，但足以證明目前每 command 建立多個
`unordered_set`／`unordered_map`／temporary vector 的寫法不能直接保留。修正不得刪除 invariant；
應改為原修正文件第 3.2 節已允許的另一方案：OrderBook mutation helper 就地驗證本地
order／level postconditions，StateMachine 只驗證跨元件契約。

## 3. 修正一：收斂 incremental validator 的資料與配置成本

### 3.1 問題

目前每個成功 command 的 `StateMachine::validate_transition()` 可能建立：

- evicted tombstone ID set；
- terminal order map；
- maker ID set；
- terminal ID set與expected terminal ID set；
- expected level vector；
- touched level map。

這些結構部分重複表示 `OrderBookApplyResult` 已有的順序資訊，且讓簡單的 New／single-match
command 也承擔 allocator、hash及rehash成本。它雖是 `O(touched)`，但不符合本需求移除 writer CPU
bottleneck 的目的。

### 3.2 採用單一驗證責任分界

選擇下列唯一方案，不保留目前兩套重疊的 level validation：

```text
OrderBook mutation helper
  -> 驗證本地 before/after quantity、status、version、priority、level aggregate

StateMachine::validate_transition
  -> 驗證 location、tombstone、ProducerState、CommandResult、events 與跨 order conservation
```

`OrderBookApplyResult` 不再保存只為第二次驗證而建立的：

```text
maker_before
level_transitions
```

並移除 `PriceLevelTransition` 及 `friend class StateMachine`。`level_total()` 可保留為 OrderBook
private helper，但不得暴露成 public API。

### 3.3 OrderBook 就地 postconditions

將 `record_level_transition()` 改為不保存 metadata 的 private helper，例如：

```cpp
[[nodiscard]] bool level_delta_is_valid(
    Side side, Price price, Quantity total_before, Quantity expected_delta) const noexcept;
```

helper 使用 checked arithmetic 計算 `total_before + expected_delta`，再與 `level_total(side, price)`
比較。每個 mutation 在修改後立即呼叫；失敗回傳 `ErrorCode::corrupt_snapshot`，由上層作 fatal
處理。

必要檢查位置：

- `apply_match_plan()`：每個 maker 的 before/after remaining、filled、status、version，以及該 maker
  level 的 `-trade.quantity`；
- taker：所有 trade quantity 加總不得超過 initial remaining，final remaining／filled 必須守恆；
- resting taker：目的 level 增加 final remaining；
- amend decrease：同一 level delta 為 `new_remaining - old_remaining`；
- amend increase：先驗證移除 old remaining，再驗證加回 new remaining及priority重排；
- replace：先驗證原 level 移除，再沿用 match/rest checks；
- cancel：原 level 必須減少完整 remaining。

錯誤發生時 state 已不可信，允許直接 fail-stop；不得嘗試 rollback，也不得產生 rejected result。

### 3.4 StateMachine 改為順序式單次驗證

保留現有以下檢查：

- EngineSeq、configuration、ProducerState與active count delta；
- target與trades的taker quantity conservation；
- active order的book/location一致性；
- terminal order的book/location/tombstone一致性；
- result及event type、ordering、payload完整相等；
- rejected command只能有一個一致的rejection event。

移除上述重複 hash containers，改用 `OrderBookApplyResult` 的既定順序：

1. `maker_updates[i]` 對應 `trades[i]`；
2. `terminal_orders` 依序包含 terminal maker，最後才是 terminal target；
3. events 依序為 trades、maker updates、changed target。

以 terminal cursor 驗證第 2 點；cursor 最後必須剛好到 `terminal_orders.end()`。Active maker直接驗證
目前book/location；terminal maker及terminal target依序消耗terminal cursor。因此不需要
`terminal_views`、`maker_ids`、`terminal_ids` 或 `expected_terminal_ids`。

後置 eviction 由 `evict_tombstones()` 保證只移除合法queue prefix且不產生重複ID。若本 command
新增的terminal已立即被evict，只需確認缺失的terminal IDs與`evicted_tombstones`尾端依建立順序
一致；不得建立額外set。

修正後 common New／single-match validator 不應自行建立 `unordered_set` 或 `unordered_map`。
原本 `output.events`、`outcome.trades` 等業務輸出容器不在此限制內。

### 3.5 Core error不得轉成業務 rejection

New與existing-order兩條路徑目前都把所有`outcome.error`交給`reject()`。改為：

```cpp
if (outcome.error == ErrorCode::corrupt_snapshot ||
    outcome.error == ErrorCode::corrupt_wal) {
  return Error{outcome.error, "order book transition invariant failed"};
}
if (outcome.error != ErrorCode::none) {
  return reject(command, outcome.error);
}
```

只允許預期業務錯誤產生 durable rejection。Invariant failure 必須讓 writer／Publisher沿既有路徑
fail-stop。

### 3.6 效能驗收

完成後先執行與第 2 節完全相同的五輪 A/B diagnostic：

- Release；
- state-machine crossing pair；
- batch 256；
- active orders 0；
- 每輪至少 512,000 commands。

必要門檻：unstaged修正後的中位數不得比staged/index低超過5%。之後再依設計執行
0／1K／10K／100K active-order正式矩陣。若仍有超過5%的退化，先profile，不再增加validation
abstraction。

預估修改：約140～220行，且預期`state_machine.cpp`為淨刪除。

## 4. 修正二：區分錯誤instrument輸入與損壞location index

### 4.1 問題

existing-order path在`order_locations[order_id] != request.instrument_id`時直接回傳
`order_not_found`。若index指向不存在的book/order，corruption會被誤分類成業務rejection。
New-order path只要看到location entry也直接回傳duplicate，存在相同問題。

### 4.2 必要helper

新增private bounded helper，名稱可依實作調整：

```cpp
[[nodiscard]] Status validate_location_entry(
    OrderId order_id, InstrumentId indexed_instrument) const;
```

只作兩次lookup：

1. `state_.books.find(indexed_instrument)`；
2. `book.find(order_id)`。

book不存在、order不存在或order內的instrument不等於indexed instrument時，回傳
`corrupt_snapshot`。不得掃描所有books。

處理規則：

- location不存在且沒有tombstone：既有`order_not_found`；
- location存在且entry自洽，但command使用另一instrument：既有`order_not_found`；
- location存在但entry無法由所指book/order證明：fatal `corrupt_snapshot`；
- New遇到location duplicate時先驗證entry自洽，再回傳`duplicate_order_id`。

新增兩個unit tests：existing-order與New各一個corrupt location案例，均必須得到typed fatal error，
且不得產生rejection event。

預估修改：約25～40行。

## 5. 修正三：消除metrics integration test競態

### 5.1 問題

`BlockingAfterFirstSink::wait_until_calls(4)`只證明第四次`publish()`已進入sink，不能證明該次ACK
之後的cursor、lag gauges及metrics snapshot已更新。立即斷言lag為零可能偶發失敗。

### 5.2 具體修改

保留blocking sink，但在`release()`後以steady-clock deadline輪詢`Engine::metrics(1)`：

```text
deadline = now + 5 seconds
while now < deadline:
  snapshot = engine.metrics(1)
  if lag_events == 0 && lag_bytes == 0 && lag_age_ns == 0:
    drained = true; break
  yield或等待1 ms
assert drained
```

不要用固定長sleep。每次snapshot若回傳Error，測試立即失敗。

blocked snapshot另外補驗證：

- `commands == 4`；
- `trades == 2`；
- `wal_group_commands == 4`且`wal_group_commits > 0`；
- `replayed_records > 0`；
- `publish_latency.count > 0`；
- lag events／bytes／age在blocked期間皆大於零。

drained snapshot再確認commands、trades與WAL totals仍保留，避免merge時被Publisher partition覆蓋。

預估修改：約20～30行。

## 6. 修正四：補齊必要corruption與eviction tests

在`tests/unit/state_machine_test.cpp`只補設計契約尚未覆蓋的案例：

1. location指向不存在instrument/book時fail-stop；
2. location指向book但該book沒有order時fail-stop；
3. 未達age/count policy時front tombstone保留；
4. age expiry只刪除已到期prefix，遇到第一個未到期entry停止；
5. count overflow只刪到`size == max_count`；
6. 一個command先因logical time作pre-eviction，之後新增terminal並因count作post-eviction；
7. tombstone queue sequence與entry sequence不一致。

每個case只檢查：typed result、保留／刪除的IDs與最後`validate_state()`。不要重複所有OrderBook
業務assertions。

現有`RejectsTouchedAggregateMismatch`實際只破壞`active_order_count`，應改名為
`RejectsActiveOrderCountMismatch`。若沒有tests-only窄介面，不新增public API來直接破壞private
price-level aggregate；level delta helper的正反unit tests即可覆蓋該契約。

預估修改：約80～130行。

## 7. 修正五：補齊Snapshot write boundary tests

Production已在live與Publisher Snapshot write前呼叫`validate_state()`，不需新增Snapshot
abstraction。只增加窄test peer：

```cpp
friend struct ShardRuntimeTestPeer;
friend struct EventPublisherTestPeer;
```

peer只定義在integration test source，不加入public method。它只允許測試破壞一個不會被下一筆
incremental transition碰到、但full validator會發現的inactive configuration entry。

### 7.1 Live Snapshot case

1. 以`snapshot_interval_commands = 1`建立`ShardRuntime`；
2. start前由peer加入invalid inactive instrument configuration；
3. start並提交一筆合法command；
4. command apply後，Snapshot前full validation必須使runtime fail-stop；
5. 驗證Snapshot sequence/file沒有前進，後續submit/query不回報成功。

### 7.2 Publisher replay Snapshot case

1. 建立含一筆durable command的WAL及`snapshot_interval_commands = 1`的Publisher；
2. start前由peer破壞replica的inactive configuration；
3. start並notify durable head；
4. 等待`publisher.failed()`；
5. 驗證replay Snapshot sequence沒有前進、sink不再收到後續events。

等待方式一律使用condition/deadline polling，不使用固定長sleep。Peer不得提供任意state替換或成為
production API。

預估修改：約80～130行，header friend宣告只需2～4行。

## 8. Benchmark CLI tests

目前`benchmarks/check_invalid_cli.cmake`及三個CTest cases已正確驗證exit code 2與error code，
保留即可，不需再抽出production parser或增加CLI framework。

## 9. 實作順序

依下列順序修正：

1. 將OrderBook-local checks與StateMachine跨元件checks分責，移除重複metadata／hash containers；
2. 修正core error propagation；
3. 修正location corruption分類；
4. 補StateMachine／eviction tests；
5. 修正metrics test等待條件與ownership assertions；
6. 補live／Publisher Snapshot boundary tests；
7. 執行GCC／Clang、Release、ASan／UBSan及完整CTest；
8. 執行五輪StateMachine A/B diagnostic；
9. 通過後才執行正式group-size／delay Pareto matrix並撰寫benchmark report。

## 10. 完成條件與修改量

完成條件：

- incremental validation仍完整驗證quantity、status、version、priority、location、tombstone、result
  與events；
- common New／single-match validator不建立unordered container；
- invariant error不產生business rejection；
- corrupt location與tombstone index一律fail-stop；
- lag drain test不依賴sink call count推測Publisher已完成；
- live與Publisher Snapshot validation failure均不寫新Snapshot；
- StateMachine五輪中位數相對staged/index退化不超過5%；
- WAL、Snapshot、Event與public API format不變；
- staging內容完全不受修正操作影響。

預估必要修改量：

| 類型 | 預估修改行數 |
| --- | ---: |
| validator／OrderBook責任收斂 | 140～220 |
| location corruption分類 | 25～40 |
| metrics integration test | 20～30 |
| corruption／eviction tests | 80～130 |
| Snapshot boundary tests與test peers | 80～130 |
| 合計 | 約345～550 |

以上是modified lines估計，不是淨新增行數；validator重構應大量刪除目前unstaged的重複程式。
正式Pareto benchmark report另計約60～100行，數字只能由實測產生。
