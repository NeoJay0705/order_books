# Publisher Cursor Group Persistence 設計

## 1. 文件目的與 review 結論

本文件根據 `docs/engine-pipeline-root-cause-analysis-report.md` 的量測結果，定義下一階段
Publisher cursor persistence 的最小 production 優化。現行 Publisher 每完成一個 EngineSeq
的 sink ACK，就執行一次 cursor temporary-file write、file fsync、rename 與 directory fsync；
5,120-command backlog 的五輪 median drain rate 約為 363 commands/s，代表 traced run 中
`rename` 約 1.05 calls/command，`fsync` 約 2.10 calls/command。雖然 trace 同時包含 fixture
建置，逐筆 durable cursor 仍已有 syscall shape 與長 backlog ceiling 支持，適合進入受控的
before/after 優化。

本階段只做以下必要修改：

1. 將 sink 已確認的連續 cursor 與已持久化 cursor 明確分離。
2. 以「最多未持久化 commands」或「最長等待時間」觸發 cursor persistence。
3. clean stop 與 event-replay snapshot 前強制持久化最新已確認 cursor。
4. 擴充既有 Publisher drain benchmark 與 integration tests，驗證 durability、duplicate window
   與效能改善。

不修改 EventSink batch contract、不合併 event publish、不改 WAL／Snapshot／cursor 格式，也不
處理 invariant、MetricsRegistry、runtime queue 或 Completion worker。本設計符合「先處理已有
supported cause 的瓶頸」需求；所有內容都直接服務 cursor group persistence，沒有預建通用
batch framework 或擴張到其他 pipeline 階段。

## 2. 需求理解與合理假設

### 2.1 必須維持的契約

- EventPublisher 仍依 EngineSeq 順序讀 WAL，使用獨立 StateMachine replica deterministic
  產生 events。
- `EventSink::publish()` 仍以單一 EngineSeq／event batch 呼叫；成功回傳代表 downstream
  durable ACK，失敗沿用既有 bounded exponential retry。
- 只有 sink ACK 成功的連續 EngineSeq 可以進入 cursor persistence；不得跳過 pending 或失敗
  的 EngineSeq。
- Cursor file 格式、checksum、temporary file + file fsync + rename + directory fsync 流程不變。
- Crash 後從 durable cursor 的下一筆 replay；ACK 已成功但尚未進入 durable cursor 的 commands
  可以重送，EventID 保持不變，由 consumer 去重，因此仍是 at-least-once delivery。
- Cursor 遺失／毀損、replay Snapshot fallback、WAL retention watermark 與 fail-stop 規則不變。
- Publisher lag age／bytes 仍以 sink 已確認的 in-memory cursor 計算；cursor persistence batching
  不應把已成功發布的 records 誤算成 downstream lag。
- Runtime 設定可以在 restart 後調整，不寫入 WAL、Snapshot 或 domain state。

### 2.2 新增的 bounded duplicate window

現行實作的正常 crash duplicate window 約為一筆已 ACK 但 cursor 尚未 durable 的 command。
group persistence 後，正常運作時最多有
`publisher_cursor_persist_max_commands` 筆連續 ACK commands 尚未進入 durable cursor；達到數量
門檻後，Publisher 必須在處理下一筆 WAL record 前嘗試 persistence。

時間門檻從 durable cursor 後第一筆新 ACK 開始，以 `steady_clock` 計時。期限到達代表必須開始
persistence；filesystem fsync latency 與 thread scheduling 使它不是 real-time completion SLA。
Publisher 在 persistence 中不繼續 ACK 新 records，因此 persistence 延遲不會讓 command 數量
window 無界增長。

## 3. 範圍

### 3.1 納入

- `RuntimeConfig` 的 cursor persistence count／delay 設定與 validation。
- `EventPublisher` 內 confirmed cursor、durable cursor 與 dirty interval 的狀態分離。
- count、time、snapshot 與 clean-stop 四種 flush trigger。
- cursor persistence failure 的既有 fail-stop 整合。
- 既有 `publisher_drain` benchmark 的可調 cursor policy、計數與 before/after matrix。
- Publisher restart、final flush、time/count trigger、duplicate replay 與 snapshot ordering tests。
- 主設計中 Publisher cursor 資料流的必要更新。

### 3.2 明確不納入

- EventSink 多 EngineSeq batch API、partial ACK 或 broker adapter。
- WAL record、cursor file、Snapshot file 或 EventID 格式變更。
- 新 publisher thread、queue、lock-free structure 或跨 shard ordering。
- Publisher StateMachine apply、event generation 或 replay batching。
- Replay Snapshot interval、WAL retention policy 或 lag budget default 調整。
- MetricsRegistry、invariant validation、Engine ingress、Completion worker 或 WAL append 優化。
- Exactly-once delivery、distributed transaction 或 consumer-side dedup storage。
- 固定 throughput threshold 作為 shared CI gate。
- 通用 timer／scheduler abstraction或第三方 dependency。

## 4. 架構與資料流

資料流仍是單一 Publisher worker：

```text
durable WAL record
  -> publisher StateMachine::apply
  -> EventSink::publish(shard, engine_seq, events)
  -> durable ACK
  -> advance confirmed cursor
  -> mark cursor dirty
  -> count reached OR delay reached OR snapshot due OR clean stop
       -> write cursor.tmp
       -> fsync(cursor.tmp)
       -> rename(cursor.tmp, publisher.cursor)
       -> fsync(event-replay directory)
       -> advance durable cursor
```

元件責任：

| 元件 | 本輪責任 |
| --- | --- |
| `RuntimeConfig` | 提供 per-shard cursor persistence count／delay policy |
| `EventPublisher` | 維持連續 ACK ordering、觸發 persistence、final flush 與 failure state |
| `ShardRuntime` | 驗證並傳遞 runtime policy，不參與 cursor batching |
| `SnapshotStore`／`Wal` | 沿用既有 API 與格式，不新增 batching 責任 |
| Benchmark | 使用 production EventPublisher 比較 count=1 與 group policy，驗證 durable reopen |
| Tests | 驗證 cursor、crash replay、snapshot ordering 與 lifecycle contract |

## 5. Cursor 狀態模型

`EventPublisher` 必須明確保存三個概念：

```text
published_cursor
  最高的連續 sink-ACK EngineSeq；worker thread 擁有

confirmed_cursor
  published_cursor 的 atomic read view；供 lag、benchmark 與 runtime observation 使用

durable_cursor
  restart 可安全採用的 durable recovery boundary；通常來自 cursor file，cursor 缺少／毀損時
  可以來自已驗證的 durable event-replay Snapshot
```

建議將現有模糊的 `cursor_` 重新命名為 `published_cursor_`，並新增 atomic
`durable_cursor_`。`confirmed_cursor()` 的既有語意維持「sink 已 ACK」，不因本輪改成 durable
cursor；另提供 internal `durable_cursor()` accessor，供 tests 與 benchmark 驗證。這兩個
accessor 都位於 `src/runtime` internal class，不加入 `include/order_books` public API。

額外狀態只需：

```text
dirty_since                 # 第一筆尚未 durable ACK 的 steady_clock time
successful_cursor_persists  # internal diagnostic counter
```

不建立 pending cursor queue。Publisher 已按 EngineSeq 單執行緒處理，最高連續 ACK cursor 本身
就是可持久化 group 的完整表示。

### 5.1 狀態不變量

- `durable_cursor <= published_cursor <= publishable_seq`。
- `confirmed_cursor == published_cursor` 的 atomic view。
- `durable_cursor == published_cursor` 時 cursor 不 dirty，`dirty_since` 為空。
- cursor dirty 時 `dirty_since` 必須存在。
- persistence 只寫入當次擷取的 `published_cursor`，成功完成 directory fsync 後才更新
  `durable_cursor`。
- sink failure、StateMachine failure 或 WAL replay failure不得前進任一 cursor。

## 6. Runtime 設定

在 `RuntimeConfig` 增加：

```cpp
std::size_t publisher_cursor_persist_max_commands{256};
std::chrono::microseconds publisher_cursor_persist_max_delay{1000};
```

選擇理由：

- 256 與現行 group-commit command default 一致，容易理解；高 backlog 且 count 先於 timer
  觸發時，cursor filesystem transaction 可由每 command 一次降低為約每 256 commands 一次。
- 1 ms 只限制 recovery cursor 的滯後時間，不延遲 sink publish ACK；低流量下可避免 dirty
  cursor 長時間留在記憶體。
- 兩者都是 runtime policy，不改 deterministic output；部署者可依 duplicate tolerance 與 storage
  latency 調整。

兩個值都必須大於零；任一為零時 `Engine::open()`／`ShardRuntime::open()` 回傳既有
`invalid_command` configuration error。第一版不提供「停用 count」或「停用 timer」的零值特殊
語意，避免產生無界 duplicate window。

預設值是保守起點，不宣稱為所有設備的最佳值。正式報告必須呈現 count、delay、throughput 與
cursor persists/command 的 Pareto 資料；若部署有更嚴格 duplicate recovery SLO，可設為 1，
恢復既有逐筆 persistence 行為。

## 7. Persistence trigger 與 worker loop

### 7.1 ACK 後處理

每筆 WAL record apply 並取得 sink ACK 後：

1. 將 `published_cursor_` 前進到該 EngineSeq。
2. release-store `confirmed_cursor_`。
3. 若原本 clean，將 `dirty_since_` 設為目前 `steady_clock`。
4. 更新既有 publisher lag metrics。
5. 若 `published_cursor_ - durable_cursor_ >= max_commands`，立即呼叫 cursor flush；成功前不得
   讀取或 publish 下一筆 WAL record。

零 event command 不呼叫 sink，但 StateMachine apply 成功即視為該 EngineSeq 已確認，依相同
規則計入 cursor group。Count 以 WAL commands／EngineSeq 計算，不以 event 數量計算。

### 7.2 Time trigger

當 cursor dirty 且尚未達 count trigger，worker 的 idle wait deadline 必須包含：

```text
dirty_since + publisher_cursor_persist_max_delay
```

condition variable 因新 publishable position、stop 或 deadline 喚醒。Deadline 到達時先 flush
cursor，再決定是否繼續讀 WAL。不得沿用固定 50 ms poll 作為 1 ms 設定的實際 timer，也不新增
第二條 timer thread。

若 backlog 持續存在，worker 在每筆 ACK 後檢查 elapsed time；因此即使沒有進入 idle wait，
delay trigger 仍可生效。

### 7.3 Flush helper

將 trigger 共用邏輯集中於一個 private helper，例如：

```cpp
Status flush_cursor_if_dirty();
```

契約：

- clean cursor 直接成功，不執行 I/O。
- 擷取目前 `published_cursor_`，呼叫既有 cursor persistence primitive。
- 只有完整 temporary-file、file fsync、rename、directory fsync 成功後，才更新
  `durable_cursor_`、清除 dirty state 並增加 diagnostic count。
- 失敗時保留舊 durable cursor，observe `publisher_cursor_error`，標記 Publisher failed 並離開
  worker；不得重試 cursor persistence 或繼續 publish，避免在未知 filesystem 狀態繼續推進。

既有 `persist_cursor(EngineSeq)` 負責單次原子檔案替換即可，不在 storage layer新增 cursor batch
API；group policy 屬於 EventPublisher lifecycle。

## 8. Snapshot、recovery 與 retention

### 8.1 Snapshot ordering

event-replay Snapshot 不得 durable 在 cursor 之前。當 command/time snapshot trigger 成立時：

```text
flush latest confirmed cursor
  -> write durable event-replay snapshot at the same publisher state
  -> publish replay_snapshot_seq
```

若 cursor flush 失敗，不得寫 Snapshot。若 Snapshot 寫入失敗，沿用既有
`publisher_snapshot_error`／fail-stop。如此維持：

```text
replay_snapshot_seq <= durable_cursor <= confirmed_cursor
```

並避免 retention 依較新的 replay Snapshot 截斷 WAL 後，restart 卻只能從較舊 cursor 恢復。

### 8.2 Startup

- 載入 cursor 成功時，`published_cursor`、`confirmed_cursor` 與 `durable_cursor` 都初始化為
  persisted EngineSeq。
- Cursor 缺少或毀損時沿用現有 replay Snapshot fallback；選定安全起點後三個 cursor 使用同一
  起始值。
- Startup replay 到 durable cursor 不 publish；從 `durable_cursor + 1` 起照常 apply／publish。
- 不新增 cursor migration，因為 on-disk bytes 完全不變。

### 8.3 Crash duplicate 邊界

Crash 發生於 sink ACK 與下一次成功 cursor persistence 之間時，restart 會重送
`(durable_cursor, published_cursor]`。正常 count trigger 下最多重送 configured max commands；
timer 只提供時間上的 persistence trigger，不承諾 filesystem stall 下的 hard completion deadline。
不得宣稱 exactly-once。

Retention watermark 仍只使用 durable state Snapshot 與 durable event-replay Snapshot；不直接以
confirmed 或 durable cursor 截斷 WAL。

## 9. Lifecycle 與錯誤處理

### 9.1 Clean stop

`EventPublisher::stop()` 維持 idempotent，流程為：

1. request stop 並喚醒 worker。
2. worker 不再取得新的 WAL record。
3. 若已有 sink ACK、cursor dirty，執行一次 final cursor flush。
4. join worker。
5. final flush 失敗時回傳既有 `engine_unavailable`，使 `ShardRuntime::stop()` 保留 failure。

Clean stop 不要求 drain 到最新 publishable WAL head；尚未取得 sink ACK 的 records 留待 restart
replay。Final flush 只覆蓋已 ACK 的連續 cursor，不能把 pending publish 當成成功。

### 9.2 Failure cases

| Failure | 行為 |
| --- | --- |
| WAL replay／StateMachine apply | 沿用既有 publisher fail-stop，不前進 cursor |
| EventSink publish error／throw | 保留 pending batch並重試，不前進 cursor |
| Cursor temp open／write／file fsync | durable cursor 不變，publisher fail-stop |
| Cursor rename／directory fsync | 不宣稱新 cursor durable，publisher fail-stop；restart 以實際可讀 cursor 驗證 |
| Snapshot write | cursor 已 durable，但 publisher fail-stop；restart 可安全從 cursor 恢復 |
| Stop during retry | 不 final-flush 未 ACK pending batch；已 ACK dirty cursor 必須 final-flush |

錯誤型別與 public error code 不增加；不得以 exception message 建立新的 machine-readable
contract。

## 10. 主要介面與必要檔案修改

### 10.1 Production

- `include/order_books/engine.hpp`
  - 在 `RuntimeConfig` 增加兩個 cursor persistence policy 欄位。
- `src/runtime/engine.cpp`
  - 驗證 count／delay 都大於零。
- `src/runtime/shard_runtime.cpp`
  - 在 internal open validation 檢查設定，並傳給 `EventPublisher::open()`。
- `src/runtime/event_publisher.hpp`
  - 擴充 `open()`／constructor policy 參數。
  - 分離 published／confirmed／durable cursor state。
  - 增加 internal durable cursor與successful-persist count accessor，以及 private flush helper。
- `src/runtime/event_publisher.cpp`
  - 實作 count/time/snapshot/stop trigger、deadline wait、final flush 與 startup initialization。
- `tests/integration/event_publisher_test.cpp`（新增）
  - 以 internal EventPublisher seam 驗證 count/time trigger、final flush、retry、crash replay 與
    snapshot ordering。
- `tests/integration/engine_test.cpp`
  - 補 RuntimeConfig 非法值與 public Engine lifecycle regression。
- `tests/CMakeLists.txt`
  - 將新增 integration test 納入既有 test executable；不建立新 target。

不修改 `EventSink`、WAL、SnapshotStore、domain model 或 cursor codec。

### 10.2 Benchmark 與文件

- `benchmarks/pipeline_ceiling_benchmark.hpp/.cpp`
  - 讓既有 `publisher_drain` 接收 cursor count／delay，輸出 persist count 與
    commands-per-persist，維持 reopen durability validation。
- `benchmarks/order_book_benchmark.cpp`
  - 新增 benchmark-only CLI：

```text
--publisher-cursor-persist-max-commands=N
--publisher-cursor-persist-max-delay-us=N
```

  - 兩者必須大於零，只影響 `publisher_drain`（`all` 傳入同一 options）。
- `docs/order-book-design.md`
  - 將「每次 ACK 後 persist」更新為 confirmed／durable cursor 與 group persistence 流程。
- `docs/publisher-cursor-group-persistence-benchmark-report.md`（實測後新增）
  - 記錄 before/after 環境、五輪 raw results、median/range、tail latency、persist count 與限制。

不新增 benchmark executable、script、JSON reporter 或 CI performance gate。

### 10.3 預估修改量

```text
include/order_books/engine.hpp                         2～5 行
src/runtime/engine.cpp                                2～6 行
src/runtime/shard_runtime.cpp                         4～10 行
src/runtime/event_publisher.hpp/.cpp                 80～140 行
benchmarks/pipeline_ceiling_benchmark.hpp/.cpp       30～60 行
benchmarks/order_book_benchmark.cpp                  15～30 行
tests/integration/event_publisher_test.cpp          140～220 行
tests/integration/engine_test.cpp                     8～20 行
tests/CMakeLists.txt                                   1～3 行
docs/order-book-design.md                            15～30 行
```

預估 production、benchmark 與 tests 共約 280～490 行，不含本設計與實測報告。若實作需要大幅
超過此範圍，應先確認是否誤把 EventSink batching、通用 timer 或 fault-injection framework
納入本輪。

## 11. Benchmark 與驗收方法

沿用 `engine_pipeline_ceiling --pipeline-stage=publisher_drain`，completion boundary 保持：

```text
drain durable WAL head
-> sink ACK all EngineSeq
-> stop/join final cursor flush
-> reopen EventPublisher
-> verify durable cursor == WAL head
```

每個正式 case 使用新的空 data directory、相同 5,120-command 以上 backlog、固定 CPU set，至少
五輪且 measured drain 至少 10 秒；若優化後 5,120 commands 已不足 10 秒，增加 iterations，
不得以短測替代穩態結果。

必要 matrix：

```text
baseline: max_commands=1,   max_delay=1000 us
default:  max_commands=256, max_delay=1000 us
ceiling:  max_commands=1024,max_delay=1000 us
```

這三點足以回答逐筆、預設與較大 group 的差異；本輪不做沒有明確 SLO 的完整 count × delay
二維排列。另以小 backlog 驗證 time trigger，以 count 高於 backlog、delay=1000 us 確認 idle
時仍會 durable。

每個 summary 必須輸出：

```text
commands_per_second
group p50/p99/p99.9/max
cursor_persist_max_commands
cursor_persist_max_delay_us
successful_cursor_persists
commands_per_cursor_persist
confirmed_cursor
durable_cursor
durable_cursor_verified=true
```

正式報告比較 median throughput、range、worst p99/p99.9、persist calls/command，並另外以
`strace` 代表 run 驗證 `rename`／`fsync` shape。Fixture 建置仍位於 process lifetime，syscall
數只能作輔助；`successful_cursor_persists` 是 measured Publisher instance 的 authoritative
計數。

效能驗收不使用跨主機固定 RPS，但必須同時滿足：

- default 相對 baseline 的五輪 throughput range 可區分，且 median 有正向改善；
- backlog 下 `successful_cursor_persists` 與設定 group 規模一致，沒有退化為逐筆 persistence；
- sink calls/events、confirmed cursor、durable reopen cursor 與 WAL head 全部一致；
- 若 default 沒有可重現改善，或 cursor count 不符合 policy，本設計視為未完成，不改用更大
  default 掩蓋問題。

## 12. 測試策略

### 12.1 Configuration tests

- count=0 拒絕。
- delay=0 或負值拒絕。
- count=1 可恢復逐筆 persistence 行為。

### 12.2 EventPublisher integration tests

- count trigger：高 delay、N 筆 ACK 前 durable cursor 不前進，第 N 筆後前進至最高連續序號。
- time trigger：count 高於 backlog，最後一筆 ACK 後即使沒有新 WAL notification，deadline 仍
  使 durable cursor 前進。
- clean stop：count／delay 都未觸發時，stop 將最新 confirmed cursor durable；reopen 不重送。
- sink retry：失敗的 pending EngineSeq 不進入 confirmed／durable cursor，成功 retry 後才前進。
- zero-event command：不呼叫 sink但仍按 EngineSeq 計入 cursor group。
- snapshot ordering：任何公開的 replay snapshot sequence 都不大於 durable cursor。
- crash image：保留舊 durable cursor、讓後續 ACK 尚未 flush，從該 filesystem image reopen 後
  驗證只重送 durable cursor 之後的相同 EngineSeq／EventID。
- cursor persistence failure：若能以既有 filesystem seam 穩定製造，驗證 publisher failed；
  不為此新增 production fault-injection framework。

測試以 internal `durable_cursor()`／persist count觀察狀態，不使用短 sleep 猜測 count trigger。
Time-trigger case 可使用寬裕 deadline 的 integration wait；不得以精確 scheduler timing 作 assertion。

### 12.3 Regression matrix

- GCC／Clang Debug 與 Release warnings-as-errors build。
- 全部 GoogleTest。
- ASan／UBSan。
- pipeline `all` smoke。
- Publisher drain baseline/default/ceiling 五輪長測與 before/after 報告。
- `git diff --check`。

CI 只加入極小 count-trigger、final-flush 與 benchmark smoke，不執行長時間 performance gate。

## 13. 關鍵設計決策與取捨

- 選擇 batch cursor persistence，而不是 batch EventSink：證據直接指向 cursor filesystem
  transaction；改 EventSink 會新增 partial ACK 與 broker contract，超出需求。
- 保留 confirmed cursor作 lag boundary：downstream 已 durable ACK 的 records 不應因本機 cursor
  尚未 fsync 而觸發 publisher lag backpressure。
- 新增 durable cursor state：沒有這個分離，snapshot、recovery與測試容易把 ACK boundary 誤當
  crash recovery boundary。
- 使用 count + delay：count 提供高流量效率與 command duplicate 上界，delay避免低流量 dirty
  cursor 無限等待。
- 使用既有 worker condition variable：足以處理 deadline 與 stop，不需要 timer thread。
- Snapshot 前強制 flush：增加一次可能的 persistence，但維持 retention/recovery safety，不能為
  吞吐省略。
- Default 256／1 ms：提供保守起點與可回退至 count=1 的相容路徑；正式結果只決定是否接受
  此 default，不在實作時自動調參。
- 不保證 exactly-once：group persistence刻意擴大 bounded duplicate window，換取 filesystem
  transaction amortization；這是本設計的核心取捨，必須由部署者與 consumer dedup 契約共同接受。

## 14. 已知限制與後續擴充方向

- Publisher 仍逐筆 WAL replay、StateMachine apply 與 `EventSink::publish()`；cursor 優化後它們
  可能成為下一個 ceiling，必須重新 profiling 後再決定。
- Timer 是 best effort，不是 real-time guarantee；filesystem stall 仍可能拉長 clean stop 與
  durable cursor latency。
- 高 count 會增加 crash duplicate 數量；本輪不建立 consumer dedup service。
- 多 shard 各自持有 cursor timer與file，不提供跨 shard atomic cursor。
- `successful_cursor_persists` 是 process-lifetime internal diagnostic，不是持久化或 public metrics
  contract。
- 若重新量測顯示 cursor transaction 已不再主導，下一階段才評估 replay/apply、sink call 或
  Snapshot 成本；不得在本輪順手實作。

## 15. 設計與實作一致性規則

- 任一 durable cursor 都必須對應已取得 sink ACK 的最高連續 EngineSeq。
- `confirmed_cursor()` 與 `durable_cursor()` 不得混用；前者是 downstream ACK boundary，後者是
  crash recovery boundary。
- Snapshot write 前必須先成功 flush 同一 state sequence 的 cursor。
- Stop final flush只涵蓋已 ACK cursor，不 drain 或假定 pending publish 成功。
- Benchmark elapsed 必須包含 final cursor flush，不包含 fixture 建置與 reopen verification。
- Baseline、default、ceiling 與 strace runs 分開報告；不可混合計算 median。
- 若實作需要修改 EventSink、cursor format、WAL、retention policy 或新增 worker，應先更新本
  文件說明必要性與語意影響，再修改程式。

## 16. Definition of Done

- count、time、snapshot與clean-stop trigger 都能使最新 confirmed cursor安全 durable。
- Crash/restart只重送 durable cursor之後的records，EventID與ordering不變。
- Cursor／Snapshot ordering invariant與retention safety通過integration tests。
- `max_commands=1`保留既有逐筆行為；default group policy不退化成逐筆persistence。
- Publisher drain三點matrix完成五輪長測，結果報告包含persist count、throughput與tail latency。
- 全部GoogleTest、GCC／Clang warnings-as-errors與ASan／UBSan通過。
- Public EventSink、WAL／Snapshot／cursor formats、domain output與production ordering均未改變。
- 實作修改限於第10節檔案與必要測試；第3.2節項目均未實作。
