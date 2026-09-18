# Publisher Cursor Group Persistence staged review 必要修正

## 1. 文件目的與範圍

本文件只記錄 staged changes 對照
`docs/publisher-cursor-group-persistence-design.md` 後確認的必要修正，作為後續修改與驗證的直接
依據。現有 confirmed／durable cursor 分離、count／delay trigger、Snapshot 前 flush、benchmark
參數與 RuntimeConfig 方向均符合設計，不需要重寫架構。

本輪只處理下列項目：

1. 保留 `EventPublisher::stop()` 的永久失敗狀態，使重複 stop 不會把 final flush failure 誤報為
   成功。
2. 讓 integration tests 真正驗證 count boundary、sink ACK boundary、zero-event counting、
   Snapshot ordering 與 crash duplicate window，而不是被 clean-stop final flush 掩蓋。
3. 完成設計要求的 benchmark matrix、sanitizer／compiler regression 與結果報告。

不得藉此加入 EventSink batch API、新 worker、通用 fault-injection framework、WAL／Snapshot／
cursor format、production default、retention policy或其他 pipeline 優化。修正期間所有新修改保留
為 unstaged changes，不執行 `git add`、`git reset`、`git restore --staged`、`git commit` 或其他
會改變 index 的操作。

## 2. FIX-PC01：重複 stop 必須保留 Publisher failure

### 2.1 問題與必要性

`src/runtime/event_publisher.cpp` 的 `EventPublisher::stop()` 在 `started_ == false` 時直接回傳成功。
若第一次 stop 的 final cursor flush 失敗，第一次呼叫會正確回傳 `engine_unavailable`，但第二次
呼叫會因 `started_` 已清除而回傳成功。

這違反設計中的兩項 lifecycle 契約：

- `stop()` 必須 idempotent；重複呼叫不能改變已確立的 failure 結果。
- final cursor flush failure 必須由 `ShardRuntime::stop()` 保留，不得在後續 stop 被誤認為成功。

### 2.2 具體修改

只修改 `EventPublisher::stop()` 的 early return。當 worker 已停止時，先讀取 `failed_`；已失敗就
回傳與現有尾端相同的 `engine_unavailable`，否則才回傳成功：

```cpp
if (!started_) {
  if (failed_.load(std::memory_order_acquire)) {
    return Error{ErrorCode::engine_unavailable, "event publisher failed"};
  }
  return std::monostate{};
}
```

保留現有 request-stop、notify、join、final flush 與錯誤碼，不新增 failure enum、exception 或
public API。此修正不應改變從未 start 或正常 stop 後再次 stop 的成功行為。

### 2.3 必要測試

在 `tests/integration/event_publisher_test.cpp` 增加一個 final-flush failure case：

1. 以 `max_commands` 大於 backlog、長 delay 啟動 Publisher。
2. 發布一筆 command，等待 `confirmed_cursor == 1`，並確認 `durable_cursor == 0`。
3. 刪除該 fixture 的 `event-replay` directory；既有 `SnapshotStore` 只保存路徑，因此 final
   flush 開啟 `publisher.cursor.tmp` 會穩定得到 filesystem error，不需要 production fault hook。
4. 第一次 `stop()` 必須回傳 `ErrorCode::engine_unavailable`。
5. 第二次 `stop()` 也必須回傳 `ErrorCode::engine_unavailable`。
6. `durable_cursor` 必須仍為 0，且 successful persist count 不得增加。

這個測試只使用既有 filesystem seam；不得為測試新增 production callback、virtual filesystem
或 cursor writer interface。

### 2.4 預估修改量

- `src/runtime/event_publisher.cpp`：4～6 行。
- `tests/integration/event_publisher_test.cpp`：18～28 行。

## 3. FIX-PC02：讓 trigger tests 在 clean stop 前完成 assertion

### 3.1 問題與必要性

部分 staged tests 只在 `stop()` 後檢查 durable cursor。由於 clean stop 無條件 flush 已確認的
dirty cursor，即使 count trigger、zero-event counting 或 Snapshot-before-cursor ordering 壞掉，
測試仍可能通過。`SinkRetryDoesNotAdvanceCursorBeforeAck` 也在第二次 sink call 已完成後才檢查，
沒有直接觀察失敗 ACK 尚未成功時的 cursor boundary。

### 3.2 Count boundary

保留現有 `CountTriggerFlushesBeforeNextWalRecord`，因為它以第三次 sink call 作 barrier，能證明
第二筆 ACK 的 count flush 發生在讀取下一筆之前。另外增加或直接整合一個低於門檻的觀測點：

1. `max_commands=2`、長 delay，只通知一筆 durable WAL record。
2. 等待 `confirmed_cursor == 1`。
3. 在 stop 前斷言 `durable_cursor == 0` 且 `successful_cursor_persists == 0`。
4. stop 後斷言 final flush 將 durable cursor 推進至 1。

這個小案例與原測試分別驗證 `N-1` 不 flush 及 `N` flush，不建立新的 test fixture abstraction。

### 3.3 Sink retry boundary

直接修改現有 `SinkRetryDoesNotAdvanceCursorBeforeAck`：

1. 保留第一次 publish failure。
2. 使用既有 `TestSink::block_on_call(2)` 阻塞第二次 retry。
3. 等待第二次 call 已進入後，在 release 前斷言 confirmed／durable cursor 都仍為 0。
4. release 第二次 call，等待 `confirmed_cursor == 1` 與 `durable_cursor == 1`；因設定
   `max_commands=1`，這同時驗證相容的逐筆 persistence 行為。
5. 最後 stop，並保留 sink sequence 為 `{1, 1}` 的 assertion。

不得用固定短 sleep 判定 retry 時序；使用 TestSink condition variable 作 barrier。

### 3.4 Zero-event command

直接補強現有 `ZeroEventCommandCountsTowardCursorGroup`：

1. 等待 `confirmed_cursor == 2` 後，再等待 `durable_cursor == 2`，兩者都必須發生在 stop 前。
2. stop 前斷言 `successful_cursor_persists == 1`。
3. 保留 sink 只收到 EngineSeq 1 的 assertion，證明第二筆 zero-event command 沒有呼叫 sink，
   但確實湊滿兩筆 cursor group。

### 3.5 Snapshot ordering

直接補強現有 `SnapshotSequenceNeverExceedsDurableCursor`：

1. 在 stop 前等待 `replay_snapshot_seq == 2`，不要只等待 confirmed cursor。
2. 立即斷言 `replay_snapshot_seq <= durable_cursor` 且兩者都是 2。
3. 再呼叫 stop，避免 final flush 成為 assertion 成功的原因。

此測試只驗證公開狀態先後關係，不需要攔截 SnapshotStore write 或加入新的 hook。

### 3.6 預估修改量

`tests/integration/event_publisher_test.cpp` 約修改 25～40 行，其中大部分是同步點與 assertion；
不修改 production trigger 實作。

## 4. FIX-PC03：以 crash filesystem image 驗證 duplicate replay

### 4.1 問題與必要性

目前 `RestartReplaysOnlyAfterDurableCursor` 使用 `max_commands=1`，而且在 reopen 前正常 stop。
因此 confirmed cursor 與 durable cursor 相同，只驗證既有的一般 restart，沒有驗證 group
persistence 新增的 bounded duplicate window，也沒有比較重送 EventID。

設計要求 crash 發生於 sink ACK 與 cursor persistence 之間時，只重送
`(durable_cursor, confirmed_cursor]`，且重送 EventID 保持不變。這是本次用 throughput 換取
at-least-once duplicate window 的核心 correctness contract，必須有直接測試。

### 4.2 TestSink 最小擴充

讓 `TestSink::publish()` 在成功 ACK 時保存收到的 `EventId`，並提供 thread-safe accessor：

```cpp
std::vector<EventId> acknowledged_event_ids_;

for (const auto& event : events) {
  acknowledged_event_ids_.push_back(event.id);
}
```

EventId 只在即將回傳成功時加入，失敗 attempt 不得記入 acknowledged IDs；既有
`sequences_` 可繼續記錄所有 calls，供 retry test 使用。不建立通用 recording sink hierarchy。

### 4.3 Crash-image 測試流程

以三個 Publisher instance 和同一份 immutable durable WAL 完成：

1. 建立 EngineSeq 1～3 的 durable WAL fixture。
2. 第一個 Publisher 使用 `max_commands=1`，只通知 EngineSeq 1；等待並正常 stop，建立
   `durable_cursor == 1` 的基準 cursor。
3. 第二個 Publisher 使用高 count 與長 delay，通知完整 WAL head；等待
   `confirmed_cursor == 3`，並在 stop 前確認 `durable_cursor == 1`。
4. 此時 Publisher 已 idle 且沒有 cursor write，將原 fixture 的 `event-replay` directory recursive
   copy 到 `crash-image/event-replay`。WAL 已 durable 且不再修改，因此測試可繼續共用同一個
   `Wal` instance；copy 的目的是凍結 crash 時可見的 cursor／replay Snapshot image。
5. 保存第二個 sink 對 EngineSeq 2～3 成功 ACK 的 EventID，然後正常 stop 第二個 Publisher。
   此 stop 只會更新原 directory，不得修改已複製的 crash image。
6. 第三個 Publisher 從 `crash-image/event-replay` 開啟，啟動前 confirmed／durable cursor 都必須
   是 1。
7. 通知完整 WAL head並等待完成；sink 必須只收到 EngineSeq `{2, 3}`。
8. 第三個 sink 成功 ACK 的 EventID 必須與第二個 sink 保存的 EventID 完全相同，最後 stop 後
   durable cursor 必須是 3。

copy 必須在 `confirmed_cursor == 3 && durable_cursor == 1` 的明確狀態下執行，不得以殺 process、
fork、signal 或未定義的 destructor 跳過方式模擬 crash。測試結束時仍正常 stop 原 Publisher，
避免留下 thread。

### 4.4 預估修改量

- `TestSink` EventID recording：8～15 行。
- 取代現有 restart test：淨增加約 20～35 行。

## 5. FIX-PC04：完成設計指定的效能與 regression 驗收

### 5.1 必要性

目前 Debug build 與全部 GoogleTest 通過，只能證明基本 correctness。設計 Definition of Done
另外要求 Publisher drain 三點五輪長測、durable reopen verification、syscall shape、GCC／Clang
warnings-as-errors、ASan／UBSan 與 pipeline `all` smoke。這些是接受預設值 256／1 ms 的依據，
不能由短功能測試替代。

### 5.2 執行順序

完成 FIX-PC01～PC03 後依序執行：

1. Debug 與 Release warnings-as-errors build，分別涵蓋專案支援的 GCC／Clang toolchain。
2. 全部 GoogleTest。
3. ASan／UBSan 全部 correctness tests。
4. ReleaseBenchmark 的 pipeline `all` correctness smoke。
5. 固定同一 source revision、binary、CPU set、filesystem／device，以新的空 data directory 執行
   `publisher_drain`：

   ```text
   baseline: max_commands=1,    max_delay=1000 us
   default:  max_commands=256,  max_delay=1000 us
   ceiling:  max_commands=1024, max_delay=1000 us
   ```

6. 每點至少五輪；先 calibration，使每輪 measured drain 至少 10 秒，再固定相同 iterations、
   warmup、batch size 與 CPU affinity執行正式矩陣。
7. 另以 count 高於小 backlog、delay=1000 us 執行 time-trigger case，確認 idle deadline 能完成
   persistence。
8. 對一個代表 run 使用 `strace` 比較 `rename`／`fsync` shape；trace run 不納入 throughput
   median。

每輪必須確認：

```text
confirmed_cursor == durable_cursor == WAL head
durable_cursor_verified=true
sink calls/events符合fixture
successful_cursor_persists符合count／time policy
```

### 5.3 新增報告

新增 `docs/publisher-cursor-group-persistence-benchmark-report.md`，至少記錄：

- source revision、compiler／flags、CPU affinity、filesystem／device 與測試命令；
- baseline／default／ceiling 五輪 raw output；
- throughput median／range、worst p99／p99.9／max；
- successful persists、commands per persist 與 durable reopen 結果；
- 代表 run 的 normalized rename／fsync counts及 trace 限制；
- default 相對 baseline 是否具有可重現改善；
- time-trigger 小 backlog 結果、異常或無效 runs；
- 尚不能由本次數據判定的後續瓶頸。

若 default 沒有可重現改善，或 persist count 不符合 policy，應將需求標為未完成並重新 profile；
不得順手改大 production default、EventSink contract 或 worker 架構。

### 5.4 預估修改量

效能與 regression 執行本身不修改 production source；benchmark report 約 60～100 行。

## 6. 完成後驗證

完成條件如下：

1. 正常 stop 與重複 stop 成功；final flush failure 的每次 stop 都回傳
   `engine_unavailable`。
2. Count trigger 明確驗證 `N-1` 不 flush、`N` flush，且 flush 發生在處理下一筆 WAL record
   之前。
3. Sink retry 在 ACK 成功前不前進 confirmed／durable cursor；`max_commands=1` 維持逐筆
   persistence。
4. Zero-event command 不呼叫 sink，但會計入 cursor group並在 stop 前觸發 persistence。
5. Replay Snapshot 在 stop 前已滿足 `replay_snapshot_seq <= durable_cursor`。
6. Crash image 只重送 durable cursor 之後的 EngineSeq，EventID 與第一次成功 publish 完全相同。
7. GCC／Clang、Debug／Release、GoogleTest、ASan／UBSan 與 pipeline smoke 全部通過。
8. 三點五輪 benchmark、time-trigger case 與 syscall代表 run 已寫入報告。
9. `git diff --check` 與 `git diff --cached --check` 通過。
10. EventSink、WAL／Snapshot／cursor formats、domain output、ordering、retention policy與 production
    defaults均未改變。
11. 原 staged changes 未被任何命令改寫；所有修正仍是 unstaged changes，等待後續 review。

## 7. 修改量估算

| 檔案／工作 | 預估修改量 |
| --- | ---: |
| `src/runtime/event_publisher.cpp` | 4～6 行 |
| `tests/integration/event_publisher_test.cpp` | 約 50～75 行 |
| benchmark report | 約 60～100 行 |
| production＋tests | 約 55～80 行 |

不預期修改 public headers、`EventSink`、`Wal`、`SnapshotStore`、benchmark source、CMake、Conan 或
主設計。若實作時必須修改上述範圍，應先記錄新的具體阻礙並重新 review，不得直接擴大需求。
