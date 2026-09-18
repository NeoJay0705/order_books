# Engine Writer Throughput Optimization unstaged follow-up 必要修正

## 1. 文件目的

本文件記錄完成
`docs/engine-writer-throughput-optimization-unstaged-review-fixes.md` 所列修改後，針對目前
unstaged changes 再次審查所發現的必要 follow-up。內容只補足尚未符合
`docs/engine-writer-throughput-optimization-design.md` 的部分，不重做已完成的 validator
重構、metrics ownership、Snapshot boundary 或 benchmark CLI 修改。

本次只需要處理：

1. 補完整 maker 的 OrderBook-local exact postconditions；
2. 消除 live Snapshot integration test 對 `ShardRuntime::failed_` 的 data race；
3. 補齊既有修正文件已要求、但目前測試尚未真正覆蓋的邊界案例。

不得修改 WAL／Snapshot／Event／CommandResult format、durability boundary、Publisher／Completion
語意、production group-commit defaults或公開 API。

## 2. Review 結論

目前修改方向正確，而且以下內容都應保留：

- OrderBook 負責本地 order／price-level postconditions；
- StateMachine 負責 location、tombstone、result、event及跨 order conservation；
- common New／single-match validator 不配置額外 unordered containers；
- `corrupt_snapshot`／`corrupt_wal` 沿 fatal path 傳遞；
- metrics lag test 等待實際 gauges 歸零；
- live與Publisher Snapshot write前保留 full-state validation；
- test peer只提供窄測試介面，不增加public production API。

目前沒有證據支持加入新的 abstraction、通用 transaction／rollback、額外 hash index或新的測試
framework。以下三項是完成原設計契約所必需的最小修改。

## 3. 修正一：補完整 maker local postconditions

### 3.1 問題

`OrderBook::apply_match_plan()` 已保存 mutation 前的 `maker_before`，但 mutation 後目前只驗證：

- level aggregate delta；
- filled／remaining quantity；
- priority；
- status；
- `maker->view.version == step.maker_version`。

最後一項只比較剛賦入的值，不能證明 version 由原值正確遞增；同時沒有明確確認 maker 的 identity、
instrument、side、price與total quantity未被改變。移除`OrderBookApplyResult::maker_before`後，這些
before／after條件只能在仍持有local `maker_before`的mutation helper內精確驗證。

### 3.2 具體修改

修改`src/domain/order_book.cpp`的`OrderBook::apply_match_plan()`：

1. mutation前以既有`checked_increment()`由`maker_before.version`算出
   `expected_maker_version`；失敗時回傳`corrupt_snapshot`。
2. 確認`step.maker_version == expected_maker_version`。
3. mutation後確認下列欄位：
   - `order_id`不變；
   - `instrument_id`不變；
   - `side`不變；
   - `price`不變；
   - `total_quantity`不變；
   - `priority_seq`不變；
   - `version == expected_maker_version`；
   - filled／remaining／status及level delta維持現有檢查。

建議形態：

```cpp
OrderVersion expected_maker_version = 0;
if (!checked_increment(maker_before.version, expected_maker_version) ||
    expected_maker_version != step.maker_version) {
  return failure(ErrorCode::corrupt_snapshot);
}

// mutation後，併入現有postcondition判斷
if (maker->view.order_id != maker_before.order_id ||
    maker->view.instrument_id != maker_before.instrument_id ||
    maker->view.side != maker_before.side ||
    maker->view.price != maker_before.price ||
    maker->view.total_quantity != maker_before.total_quantity ||
    maker->view.priority_seq != maker_before.priority_seq ||
    maker->view.version != expected_maker_version) {
  return failure(ErrorCode::corrupt_snapshot);
}
```

不要重新把`maker_before`放回`OrderBookApplyResult`，也不要把這些檢查移回StateMachine；那會重新引入
跨層metadata及hot-path成本。

預估修改：約8～12行。

## 4. 修正二：同步讀取 ShardRuntime failure state

### 4.1 問題

`ShardRuntime::fail()`在持有`queue_mutex_`時寫入非atomic的`failed_`，但
`ShardRuntimeTestPeer::failed()`目前未取得mutex便由測試執行緒讀取。這是C++ memory model下的data
race；即使現有ASan／UBSan測試通過，測試仍可能出現未定義行為或偶發timeout。

### 4.2 具體修改

只修改`tests/integration/engine_test.cpp`內的test peer：

```cpp
static bool failed(ShardRuntime& runtime) {
  std::lock_guard lock(runtime.queue_mutex_);
  return runtime.failed_;
}
```

呼叫端已有non-const `ShardRuntime`，不需要修改production欄位、不需要新增public accessor，也不需要
把`failed_`改成atomic。這可沿用production既有同步責任，修改範圍最小。

預估修改：約3～5行。

## 5. 修正三：補齊必要邊界測試

只修改現有test files，不新增production test hook。

### 5.1 Location指向存在book但缺少order

在`tests/unit/state_machine_test.cpp`新增一個case：

1. 使用`make_state()`，其中instrument 7及其book可先透過一筆正常order建立；
2. 為另一個OrderId加入`order_locations[order_id] = 7`，但不要在book加入該order；
3. 對該OrderId送出New或existing-order command；
4. 驗證回傳typed `ErrorCode::corrupt_snapshot`，不是`duplicate_order_id`、`order_not_found`或
   rejected event；
5. 最後不必對刻意損壞的state呼叫`validate_state()`。

這直接覆蓋`validate_location_entry()`的第二個lookup，不需要再修改helper。

預估新增：約15～20行。

### 5.2 Age eviction只移除到第一個未到期entry

目前`EvictsOnlyExpiredTombstonePrefix`只有一個tombstone，只能證明expired entry會被刪除，不能證明
遇到第一個未到期entry後停止。

將case擴充或另加一個case：

1. 設`terminal_tombstone_max_age_ns = 3`；
2. 依序建立並cancel兩筆order，使terminal time分別為`1002`與`1004`；
3. 在received time `1005`送出下一筆command；
4. 此時第一筆age為3，應被pre-evict；第二筆age為1，必須保留；
5. 驗證queue只剩第二筆，且`validate_state()`成功。

使用既有遞增EngineSeq與timestamp helper即可，不要用sleep或wall clock。

預估新增／修改：約20～30行。

### 5.3 同一command同時發生pre-eviction與post-eviction

新增一個deterministic case：

1. 設`terminal_tombstone_max_age_ns = 3`及`terminal_tombstone_max_count = 2`；
2. 先建立一筆可在最後被成交的resting maker；
3. 再建立兩筆terminal tombstones，terminal time分別為`1003`與`1005`；
4. 在time `1006`送出會完全成交maker與taker的command；
5. command開始時，time `1003`的舊tombstone因age先被pre-evict；
6. match新增maker與taker兩個terminal後，總數超過2，post-eviction再移除time `1005`的舊
   tombstone；
7. 最後只保留本command建立的maker／taker tombstones，順序正確且`validate_state()`成功。

此case專門驗證pre-eviction結果不會干擾post-eviction terminal cursor／suffix判斷，不需要重複trade
payload的完整業務assertions。

預估新增：約30～40行。

### 5.4 Fail-stop後query也不得成功

在`LiveSnapshotValidationFailureDoesNotWriteSnapshot`現有submit rejection檢查後加入：

1. 呼叫`shard->get_order(...)`；
2. 以短deadline確認future ready；
3. 驗證結果為`ErrorCode::engine_unavailable`。

這只補足既有修正文件「後續submit/query不回報成功」的另一半，不需要新增新的integration test。

預估新增：約5～10行。

## 6. 不需要修改的內容

本次不得順帶進行：

- 改寫`validate_transition()`或重新加入unordered containers；
- 修改`evict_tombstones()`policy或retention語意；
- 新增OrderBook／ShardRuntime public debug API；
- 將`ShardRuntime::failed_`改成atomic；
- 修改Snapshot寫入順序、WAL格式或recovery流程；
- 調整Publisher／Completion worker或group-commit defaults；
- 新增與上述case無關的測試矩陣。

## 7. 驗證方法

完成修正後依序執行：

1. `git diff --check`，確認格式及whitespace；
2. GCC Release build與完整CTest；
3. ASan／UBSan完整CTest；
4. StateMachine crossing-pair五輪短測，沿用batch 256、active orders 0、每輪512,000 commands；
5. 比較既有staged/index中位數，退化不得超過5%；
6. 確認`git diff --cached`完全未被修改。

不需要為這幾項follow-up重新執行完整group-size／delay Pareto壓測；修改未改WAL、fsync、batching或
RuntimeConfig。若StateMachine短測超過5%退化，才需要先profile再決定後續。

## 8. 完成條件與修改量

完成條件：

- maker identity、immutable fields、quantity、status、priority、version與level delta皆有local exact
  postconditions；
- Snapshot boundary test不再以未同步方式讀取`failed_`；
- location missing-order、age prefix stop、同command pre/post eviction及failed query均有測試；
- 所有新增corruption仍回傳fatal typed error，不產生business rejection；
- 沒有新增public API或持久化格式；
- Release、ASan／UBSan與效能門檻通過；
- staged changes保持不變。

預估必要修改量：

| 類型 | 預估modified lines |
| --- | ---: |
| maker postconditions | 8～12 |
| test peer同步 | 3～5 |
| location missing-order test | 15～20 |
| age prefix boundary test | 20～30 |
| pre/post eviction test | 30～40 |
| failed query assertion | 5～10 |
| 合計 | 約65～115 |

以上皆是原設計或既有修正文件尚未完成的驗收內容；不包含重構、美化或後續效能功能。
