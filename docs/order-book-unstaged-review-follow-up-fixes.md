# Order Book unstaged review：必要後續修正方案

狀態：F01～F03 已套用；F04 因目前 `docs/tmp_both.md` 已 staged 且本輪禁止改變 staging，未執行

審查基線：`docs/Order-book-spec.md`、`docs/order-book-design.md`

適用範圍：承接前一輪相對 index 的 unstaged changes review；建立本文件時，該批內容已由外部操作加入 index

## 1. 結論與修改邊界

目前 tracked unstaged production changes 的架構與技術方向符合設計，沒有新增外部依賴或超出第一版範圍的元件。後續只需要完成下列事項：

1. 補齊 `Wal::bytes_after()` 對整個查詢區間的 sequence 完整性驗證。
2. 補上 active Instrument 變更與 Instrument mapping 變更的負向回歸測試。
3. 修正 review 文件對 fatal callback 測試證據的過度宣稱。
4. 確保 `docs/tmp_both.md` 不進入專案提交。

不得藉此加入通用 fault-injection framework、dependency-injection container、動態 configuration service、新的 public API 或第三方依賴。完整 crash/failure-injection matrix 仍依設計文件列為後續 Definition of Done 工作。

## 2. FIX-F01：完整驗證 WAL byte range

### 問題

`src/persistence/wal.cpp` 的 `Wal::bytes_after(sequence, upper_bound)` 已驗證第一筆 record 是否接在 `sequence` 之後，但尚未驗證：

- range 中間是否出現 EngineSeq gap；
- 累加結束時是否確實到達 `upper_bound`；
- saturation 後剩餘 records 是否仍保持連續。

因此在 cache 不完整或 caller 傳入不存在的 durable head 時，函式可能回傳偏小的 byte count，而不是設計要求的 typed error。

### 必要修改

涉及檔案：

- `src/persistence/wal.cpp`
- `tests/integration/persistence_test.cpp`

具體作法：

1. 保留 `upper_bound <= sequence` 時回傳 `0` 的既有語意。
2. 對非空區間 `(sequence, upper_bound]`：
   - 若 index 未初始化，維持回傳 `WAL_FAILURE`；
   - 若找不到第一筆 record，回傳 `CORRUPT_WAL`；
   - 第一筆及後續每一筆都必須等於前一個 EngineSeq 加一，否則回傳 `CORRUPT_WAL`；
   - 迴圈結束後，最後驗證過的 EngineSeq 必須等於 `upper_bound`，否則回傳 `CORRUPT_WAL`。
3. 飽和加法達到 `UINT64_MAX` 後仍繼續走訪到 `upper_bound`，以完成 sequence 驗證；不得因 byte count 已飽和而提前成功返回。
4. 不重新掃描 WAL、不重新 decode record，也不把這個 API 提升成 public Engine API。

建議實作形狀：

```cpp
EngineSeq previous = sequence;
std::uint64_t total = 0;
for (auto current = iterator;
     current != records_.end() && current->command.engine_seq <= upper_bound;
     ++current) {
  if (previous == std::numeric_limits<EngineSeq>::max() ||
      current->command.engine_seq != previous + 1U) {
    return Error{ErrorCode::corrupt_wal, "WAL publisher range is not contiguous"};
  }
  previous = current->command.engine_seq;
  total = total > std::numeric_limits<std::uint64_t>::max() - current->frame_bytes
              ? std::numeric_limits<std::uint64_t>::max()
              : total + current->frame_bytes;
}
if (previous != upper_bound) {
  return Error{ErrorCode::corrupt_wal, "WAL publisher head is unavailable"};
}
return total;
```

實際實作可調整錯誤文字，但必須保留上述失敗語意。

### 必要測試

在 `PersistenceTest` 增加兩個窄測試，或在既有 WAL rotation test 中加入等價 assertions：

1. WAL 只有 EngineSeq 1、2，呼叫 `bytes_after(0, 3)`，必須回傳 `CORRUPT_WAL`，不得回傳 1、2 的 byte sum。
2. 建立已載入的 index 後 append EngineSeq 1、3 所形成的 cache gap，呼叫 `bytes_after(0, 3)`，必須回傳 `CORRUPT_WAL`。

這兩個測試只驗證 internal defensive contract；本輪不擴張成 WAL append policy 重設計。

預估：production 10～15 行，tests 15～25 行。

## 3. FIX-F02：補齊 configuration reconciliation 的負向測試

### 問題

目前測試已證明：

- shard 有 active order 時仍可新增另一個 Instrument；
- 沒有 active order 的 Instrument 可以更新 tick／lot；
- current behavior version 由 supplied configuration 選擇。

但尚未直接驗證兩個必須拒絕的分支。既有的 `RestartRejectsChangedImmutableInstrumentVersion` 使用相同 version 搭配不同 canonical content，只覆蓋 immutable version conflict，不能替代新 version 對 active Instrument 的相容性判斷。

### 必要修改

涉及檔案：

- `tests/integration/engine_test.cpp`

只新增下列兩個 integration tests，不修改 production reconciliation algorithm：

#### 3.1 Active Instrument 的新版本變更必須拒絕

1. 以 instrument configuration version 1 啟動 shard 1，Instrument 7 使用 tick／lot `1/1`。
2. 提交一筆會留下 active order 的 New command，等待 committed completion 後停止 Engine。
3. 使用 instrument configuration version 2 重啟，保留 Instrument 7 在 shard 1，但把 tick 或 lot 改為不同正值。
4. 驗證 `Engine::open()` 回傳 error，且 shard 不會啟動。

這個測試必須使用新 version，避免只重複測試同 version canonical conflict。

#### 3.2 Instrument mapping 變更必須拒絕

1. 初始設定建立 shard 1、2，將 Instrument 7 指派至 shard 1，啟動後正常停止。
2. 使用新的 instrument configuration version 重啟，將同一 Instrument 7 改指派至 shard 2。
3. 驗證 `Engine::open()` 回傳 error。

不需要先建立 active order；第一版在沒有 migration protocol 時，mapping change 一律拒絕。

### 測試實作限制

- 沿用既有 `RecordingSink`、`NullMetricsSink` 與 temporary-directory 模式。
- 可以新增一個僅供本測試檔使用的 `submit_and_wait()` helper，以減少 promise/callback 樣板；不要重構全部既有測試。
- 不新增 fixture hierarchy、mock framework 或 production-only test hook。
- 每個已成功開啟的 Engine 都必須顯式 `stop()`；失敗的 `Engine::open()` 不得解參考 result。

預估：tests 55～85 行。

## 4. FIX-F03：讓 review 文件只宣稱已有證據的能力

### 問題

`docs/order-book-unstaged-review-fixes.md` 目前把「每個 queued request 恰好完成一次」列為已完成，但現有測試只直接覆蓋正常路徑的同 Producer completion ordering，尚未用可控 WAL／apply fatal failure 驗證 callback count。

同一文件另寫明 fatal path 的所有 pending callback 都回傳 `ENGINE_UNAVAILABLE`；目前實作會保留已確定的 admission result，只把 accepted／未決定 slot 轉成 `ENGINE_UNAVAILABLE`。主設計只要求 fatal guard 補齊未完成 callback，沒有要求覆寫已確定的 admission outcome。

### 必要修改

涉及檔案：

- `docs/order-book-unstaged-review-fixes.md`

具體作法：

1. 將完成條件拆成：
   - `[x]` 正常路徑同 Producer stream callback 不失序；
   - `[ ]` WAL／apply fatal path 的 exactly-once callback count，待 fault-injection seam 完成。
2. 將 fatal-path 說明改為：
   - 已確定的 admission result 保持原結果；
   - accepted 或尚未決定的 slot 回傳 `ENGINE_UNAVAILABLE`；
   - 每個 handler dispatch 後移空，guard 只補齊仍持有 handler 的 slot。
3. 保留 `docs/order-book-design.md` 對完整 fault-injection／subprocess matrix 尚未完成的聲明，不以文件修改假裝測試已完成。

本輪不為了勾選 exactly-once fatal test 而新增通用 failpoint。該測試應與設計已列出的 storage fault-injection 工作一併完成。

預估：docs 3～6 行。

## 5. FIX-F04：排除範圍外暫存文件

### 問題

`docs/tmp_both.md` 是操作指令與對話筆記，不是需求、設計、使用者文件或貢獻指南。若進入公開 repository，會造成文件噪音且無法形成穩定契約。建立本文件時它已位於 index，因此不能只靠「維持 untracked」排除。

### 必要處理

1. 在允許調整 index 的階段，將 `docs/tmp_both.md` 從預定提交移出；本輪不得為此違反「不改變 staging」限制。
2. 若內容仍需個人保存，移至 repository 外並只從 index 移除；若不再需要，再明確刪除。
3. 不恢復 `.gitignore` 的 `tmp*` pattern。該 pattern 過度寬泛，會再次隱藏合法檔案；單一個人筆記不應以 repository-wide ignore rule 解決。

這項處理不需要修改 production 或 project documentation。

預估：預定提交內容淨減 91 行；是否保留 working-tree copy 由檔案擁有者決定。

## 6. 不需要修改的內容

下列 unstaged changes 已符合設計，不再重構：

- completion slot 依原始 batch index 保存與 dispatch；
- publisher 使用 `(confirmed_cursor, durable_head]` 的 record frame bytes；
- `max_publish_lag_age <= 0` 在 Engine open 時拒絕；
- current behavior version 從本次 supplied configuration 選擇；
- durable benchmark 每個 sample 包含固定 256 commands 與一次 sync；
- WAL append 的未使用 durable flag、OrderBook dead helpers、ScopeGuard dead method 與過寬 `tmp*` ignore rule 的移除；
- 既有設計文件對未完成完整 Definition of Done 的揭露。

不為縮短重複測試 setup 而導入新的測試框架或大規模 helper refactor。

## 7. 修改規模與完成條件

預估必要修改：

| 類別 | 預估行數 |
| --- | ---: |
| WAL production code | 10～15 |
| Persistence tests | 15～25 |
| Configuration integration tests | 55～85 |
| 文件校正 | 3～6 |
| 合計 | 約 85～130 |

完成條件：

- [x] `bytes_after()` 對首筆、中間 gap 與缺少 upper bound 都回傳 typed error。
- [x] Active Instrument 的新 version tick／lot 變更會拒絕啟動。
- [x] Instrument-to-shard mapping 變更會拒絕啟動。
- [x] Review 文件不再宣稱尚無 failure-injection 證據的 fatal exactly-once 測試已完成。
- [ ] `docs/tmp_both.md` 不在預定提交範圍。
- [x] Debug、Release、ASan/UBSan tests 通過。
- [x] Completion ordering test 重複執行仍穩定。
- [x] Release benchmark `--iterations=1 --warmup=0` 成功，且 durable workload 顯示 `commands=256 group_size=256 fsync_mode=per_group`。
- [x] `git diff --check` 通過。
- [x] staged index 在整個修正過程保持不變。
