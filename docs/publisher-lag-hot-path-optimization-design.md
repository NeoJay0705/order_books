# Publisher Lag Hot-Path Optimization Design

## 1. 文件目的與結論

本文件是 `docs/order-book-design.md` 中 publisher lag 與 storage-pressure 設計的效能修正補充。它只處理目前 profiling 已證實的熱點：`Wal::bytes_after()` 在 command admission 路徑反覆線性掃描 publisher backlog，使執行時間隨 lag 增長而接近 O(n²)。

既有需求仍成立：Engine 必須以 publisher lag bytes／age 監控 downstream backlog，於 50%／80% 預算產生 warning／critical，達上限時拒絕新的 mutation，且不得丟棄尚未安全發布的 event。問題不在需求，而在查詢資料結構與取樣位置。

本次必要修改只有：

1. 使用 WAL 記憶體索引中的累積 record bytes，以 O(log N) 查詢 `(confirmed_cursor, durable_head]` 的實際 bytes，不再逐筆累加區間。
2. 同一個 admission batch 只建立一次 publisher pressure sample，供該 batch 中通過 identity admission 的新 mutation 共用。
3. 保留現有錯誤、backpressure、metrics、durability、ordering 與 at-least-once event delivery 語意，補足必要的功能與效能回歸驗證。

本文件通過需求與範圍 review，可作為下一階段實作依據。若與主設計對 `bytes_after()` 複雜度或 pressure sample 邊界的描述不同，以本文件為準；其餘仍以主設計為準。

## 2. 證據與問題界定

目前量測事實：

- 42,000 commands 的 profiling 中，`Wal::bytes_after()` 呼叫 42,801 次並占 80.93% self CPU。
- `bytes_after()` 先 binary search cursor，之後持有 WAL mutex 逐筆掃描到 durable head。
- workload 增加時吞吐由約 18,905 RPS 降至約 971 RPS，延遲同步惡化，符合 backlog 擴大後每筆掃描成本上升的特徵。
- 解除單 CPU 綁定只改善約 4.9%；syscall trace 中 `fsync` 約占 6.4 秒執行時間中的 55 ms。現有證據不支持 CPU 配置或 WAL `fsync` 是本輪主要根因。

因此，本輪只修正已定位的演算法與重複取樣問題。修正後必須使用相同 workload 重新 profiling，才能決定下一個瓶頸。

## 3. 需求理解與合理假設

### 3.1 必須維持

- `lag_bytes` 的定義仍是 WAL record frame bytes 的總和，範圍固定為 `(confirmed_cursor, durable_head]`。
- Segment header、cursor 以前的 retained records 及尚未 durable 的 records 不計入 lag。
- `upper_bound <= cursor` 回傳 0。
- Cursor 或 durable head 無法由 retained WAL index 完整表示，或區間 EngineSeq 不連續時，回傳既有 typed error；不得將錯誤當成 lag 0。
- `EventPublisher::lag_bytes()` 遇到 WAL index error 時仍標記 publisher failed，由 shard 走既有 fail-stop path。
- `max_publish_lag_bytes == 0` 仍表示停用 byte limit；`max_publish_lag_age`、50%／80%／100% 邊界與既有預設不變。
- Producer identity／duplicate 判定仍先於 storage-pressure rejection；已處理的 duplicate 不得因 storage pressure 改成另一個結果。
- Matching、WAL durable boundary、completion callback、publisher cursor、event ordering 與 at-least-once 語意不變。

### 3.2 Batch sample 假設

Admission 階段尚未 append 或 sync 本批 commands，因此同一 batch 內的 durable head 不會因本批工作前進；publisher 只可能在背景推進 confirmed cursor，使 lag 減少。第一個需要 storage admission 的新 mutation 取得一次 pressure sample，供同批後續新 mutation 共用，是保守且有界的判定：若 sample 已達壓力門檻，本批新 mutation 都拒絕，下一批重新取樣；若未達門檻，最多接受既有 `group_commit_max_commands` 所界定的一批，與目前「admission 時尚未計入本批 WAL bytes」的行為一致。

## 4. 範圍

### 4.1 納入

- WAL decoded sequential index 的 prefix-byte metadata。
- `Wal::bytes_after()` 的等價 O(log N) range query。
- Replay、append、retention 時維護該 metadata。
- `ShardRuntime::process_command_batch()` 內每 batch 最多一次 publisher lag bytes／age sample。
- 既有測試補強與現有 durable single-instrument benchmark 的重跑。

### 4.2 明確不納入

- Publisher batch replay、跨 command batch publish 或 `EventSink` 介面變更。
- Publisher cursor batch persistence 或其 crash／duplicate 邊界變更。
- WAL active file descriptor 常駐、`open/write/close` 優化。
- 新增 thread、queue、exporter、設定項或調整既有預設門檻。
- Snapshot、retention policy、WAL format 或磁碟檔案格式變更。
- 固定 RPS 門檻；共享 CI 環境只做功能測試。

這些項目可能成為修正後的下一個瓶頸，但目前沒有足夠證據支持在同一變更中實作。

## 5. 架構與模組邊界

| 元件 | 本輪責任 |
| --- | --- |
| `storage::Wal` | 維護 decoded record 的 prefix index，提供正確且 bounded 的 range-byte query |
| `runtime::EventPublisher` | 沿用 `Wal::bytes_after()`，維持 failure 與 confirmed cursor 語意 |
| `runtime::ShardRuntime` | 在 admission batch 內重用單一 pressure sample，套用既有門檻與錯誤 precedence |
| Tests | 驗證 range 語意、gap／retention、admission 行為及既有公開契約 |
| Benchmark | 驗證修正消除已知熱點，不新增 benchmark mode |

不新增 public API，也不讓 runtime 讀取 `Wal` 私有 metadata。Prefix index 仍是 storage implementation detail。

## 6. WAL Prefix Index 設計

### 6.1 `CachedRecord` metadata

在既有 `CachedRecord` 增加兩個 internal 欄位：

```text
CachedRecord
├── command
├── frame_bytes
├── cumulative_frame_bytes
└── continuity_id
```

- `cumulative_frame_bytes`：從目前 `records_` 第一筆起，到本 record 為止的 frame bytes 累積值。
- `continuity_id`：相鄰 record 的 EngineSeq 不等於前一筆加一時遞增；同一個連續區間具有相同值。

`continuity_id` 是必要資料，因為現有 `bytes_after()` 不只計算 bytes，也負責拒絕 range 內的 EngineSeq gap。只保存 prefix bytes 而移除 gap validation 會造成 correctness regression。

累積值使用 checked addition。若無法以 `std::uint64_t` 表示，操作回傳 `wal_failure`，不得 wrap。正常上限遠低於此界線，但仍不得依賴未定義或靜默溢位行為。

### 6.2 Metadata 建立與維護

- `replay()`：在既有逐筆 decode 過程同步建立 prefix bytes 與 continuity metadata，不新增第二次 WAL decode。
- `append()`：由最後一筆 cached record 以 O(1) 建立新 metadata；計算失敗必須在寫入新 frame 前回報，避免「磁碟已 append、記憶體 index 未更新」。
- `retain_through()`：完成既有 prefix erase 後，以剩餘 `records_` 為新基準重建 metadata。此操作可為 O(N)，因為 retention 已有 vector prefix erase／搬移成本且不在每 command query 熱路徑；不得把重建放到後續第一次 `bytes_after()` 延遲執行。

WAL on-disk record 與 segment format 不變，因此不需要 format migration。

### 6.3 `bytes_after(sequence, upper_bound)`

方法簽名與 error types 不變。鎖定既有 WAL mutex 後：

1. `records_loaded_ == false`：回傳既有 `wal_failure`。
2. `upper_bound <= sequence`：回傳 0。
3. 以 binary search 找到第一筆 `engine_seq > sequence` 與 `engine_seq == upper_bound` 的 record。
4. 驗證第一筆恰為 `sequence + 1`、upper bound 存在，且兩端 `continuity_id` 相同；否則回傳既有 `corrupt_wal`。
5. 使用兩個 prefix 值相減得到區間 frame bytes；使用 checked subtraction，任何 index invariant 違反都回傳 `corrupt_wal`。

查詢最多做固定次數的 binary search 與常數次算術，不得以 loop、`accumulate` 或其他方式遍歷 `(sequence, upper_bound]`。時間複雜度為 O(log N)，mutex 持有時間不再與 publisher lag 長度成正比。

## 7. Admission Pressure Sample

在 `ShardRuntime::process_command_batch()` 內建立 method-local、非 public 的 cached sample，內容只需：

```text
PublisherPressureSample
├── lag_bytes
├── lag_age_ns
├── warning
├── critical
└── pressure
```

具體流程：

1. 保留函式入口的 publisher failed 檢查。
2. 逐筆執行既有 producer identity、epoch、sequence 與 duplicate admission。
3. 第一筆需要繼續進行 storage admission 的新 mutation 才讀取 publisher oldest-unconfirmed time 與 lag bytes，建立 sample；全為 duplicate／identity rejection 的 batch 不做無用查詢。
4. 建立 sample 後立即檢查 publisher failure；WAL lag query error 沿用既有 shard fail-stop path。
5. 同批後續新 mutation 重用 sample，但仍維持既有 `wal_soft_limit_bytes` 判定與錯誤 precedence。
6. Lag gauges 與 warning／critical observation 每個實際建立 sample 的 batch 各記錄一次；不把 batch size 當成相同 operational condition 的重複事件。
7. 達 100% budget 的 sample 對本批所有新 mutation 回傳 `engine_storage_pressure`；下一個 batch 重新取樣，因此 publisher 恢復後能繼續服務。
8. 保留 durable apply 前後既有 publisher failed 防線；本輪不改 publisher lifecycle。

不把 pressure sample 保存成 shard 長期狀態，也不新增 timer。如此避免 stale cache invalidation 與額外同步機制。

## 8. 錯誤處理

- Index 尚未初始化：`wal_failure`。
- Cursor range 已不在 retained WAL、upper bound 不存在或 EngineSeq gap：`corrupt_wal`。
- Prefix metadata overflow：`wal_failure`；underflow 或 metadata invariant 不成立：`corrupt_wal`。兩者都使 publisher 進入既有 failed path。
- Publisher failure：本批尚未完成的 request 依既有規則完成一次 callback，shard fail-stop。
- 達 lag budget：只拒絕需要新 WAL mutation 的 command；不是 WAL corruption，也不得停止或刪除 publisher 所需資料。

不得以 exception text 建立新 machine-readable contract，也不得因效能修正把 failure 降級為 lag 0。

## 9. 測試策略

### 9.1 WAL integration tests

擴充既有 `tests/integration/persistence_test.cpp`，至少覆蓋：

- Cursor 為 0、中間與 durable head 時，bytes 與現行結果相同。
- 多 segment range 只計 record frames，不計 segment headers。
- Retention 後以新 index base 計算仍正確。
- Cursor 已被 retention 越過時回傳 `corrupt_wal`。
- Upper bound 不存在、range 起點缺失及 range 內 EngineSeq gap 均回傳 `corrupt_wal`。
- Replay 後 append 的 prefix metadata 與純 replay 結果一致。
- `records_loaded_ == false` 的既有 error contract 不變。

不得使用不穩定的 wall-clock unit test 宣稱 O(log N)。複雜度由實作結構 review 保證：range query 內不得存在依 range 長度執行的迴圈。

### 9.2 Runtime integration tests

只補足本次可能改變的 admission 邊界：

- Publisher lag 未達預算時，新 mutation 正常完成。
- 達預算時，新 mutation 回傳 `engine_storage_pressure`。
- Duplicate 在 storage pressure 下仍回傳先前 stable result。
- Publisher lag query failure 仍造成 shard fail-stop，而非被當成 0。

如果既有 public seams 無法可靠製造指定 publisher lag，不為本輪加入 production-only test hook；應使用既有 WAL／blocking sink integration fixture，或保留已覆蓋的行為測試並以 code review 驗證 batch-local cache。

### 9.3 驗證矩陣

- Debug build 與全部 tests。
- Release build 與全部 tests。
- ASan／UBSan tests。
- `git diff --check`。
- 現有 `engine-durable-single-instrument` smoke benchmark。
- 使用修正前相同參數重跑 20k、40k、80k measured commands，保留 RPS、p50、p99、p99.9 與 elapsed time。
- 使用 profiler 確認 `Wal::bytes_after()` 不再按 backlog 長度消耗 CPU；效能結果不設跨機器固定門檻，但若仍為主要 self-CPU consumer，視為本設計未完成。

## 10. 具體修改清單

預期只修改：

- `src/persistence/wal.hpp`：擴充 private `CachedRecord` metadata。
- `src/persistence/wal.cpp`：在 replay／append／retention 維護 index，將 `bytes_after()` 改為 prefix range query。
- `src/runtime/shard_runtime.cpp`：加入 batch-local lazy pressure sample，移除同批逐 command 的 lag query。
- `tests/integration/persistence_test.cpp`：補足 prefix index、gap、retention 測試。
- 必要時修改既有 runtime integration test 檔案，且只加入第 9.2 節能以既有 seam 穩定驗證的案例。

不修改 public headers、WAL disk format、CMake target、benchmark CLI、README 或 CI。若實作發現必須修改這些項目，應先記錄設計缺口並重新 review，不得直接擴張範圍。

## 11. 關鍵決策與取捨

- 選擇 prefix metadata 而不是 cached total lag：前者由 WAL index 提供任意合法 cursor range 的 authoritative 結果，避免在 append、publisher ACK、retention 三方維護易失同步的第二份狀態。
- 選擇 O(log N) binary search 而不是以 EngineSeq 直接計算 vector index：保留 retention 後 base 變動與 corruption／gap 檢查，修改較小且已足以消除線性 range scan。
- Retention 時允許 O(N) rebuild：把成本留在既有低頻 maintenance path，換取 command hot path 的 bounded lookup。
- Pressure sample 只在 batch 內重用：能移除重複查詢，同時避免跨 batch stale cache、timer 或新同步元件。
- 不在本輪 batch publisher 或 cursor persistence：會改變 ACK、crash duplicate 與 public sink contract，且尚未由修正後 profiling 證明必要。

## 12. 已知限制與後續決策門檻

本輪完成後，publisher 仍逐筆 replay、publish 並持久化 cursor；WAL append 仍可能逐筆 open/write/close。這些是已知但刻意保留的限制。

只有重新 profiling 出現下列證據時，才另立設計：

- Cursor persistence／publisher worker 成為主要等待時間：設計 batch cursor persistence，明確定義 clean shutdown flush 與 crash duplicate 上界。
- WAL file open/close 成為主要 syscall 成本：設計 active descriptor lifecycle、rotation、sync 與 failure recovery。
- Sink call overhead 成為主要成本：評估跨 command batch publish，並先修改 `EventSink` ACK／partial failure contract。

不得把上述後續項目併入本輪實作。

## 13. 完成條件與需求對照

| 需求 | 完成證據 |
| --- | --- |
| 保留 exact publisher lag | 現有及新增 range／segment／retention tests 通過 |
| 消除近似 O(n²) 熱點 | `bytes_after()` 無 range traversal，profiler 不再顯示其按 backlog 主導 CPU |
| 保留 storage protection | 50%／80%／100% 判定與 runtime behavior tests 通過 |
| 保留 correctness | Gap、missing cursor、publisher failure 仍走 typed error／fail-stop |
| 不改公開契約 | Public headers、WAL format、EventSink、config defaults 均無變更 |
| 不超出需求 | 第 4.2 與第 12 節項目均未實作 |

實作與測試全部符合以上條件，並在相同 workload 重新取得 profile 後，本階段才算完成。
