# Order Book staged changes 必要修正方案

狀態：實作修正依據  
審查基線：`docs/order-book-design.md`
審查範圍：建立本文件時的 staged changes  
目的：以最小且可驗證的修改，使實作、需求與設計的認知一致

## 1. 使用方式與範圍

本文件只列出審查後確認必要的調整。必要的定義如下：

- 修正可能破壞 durability、recovery、determinism、state invariant 或 completion contract 的問題。
- 補齊 `docs/order-book-design.md` 已承諾的第一版能力與驗收證據。
- 移除或落實目前已公開但未使用的設定與介面，避免形成錯誤承諾。
- 修正文件與實作的明確矛盾。

本次不得順便加入下列內容：

- network server、Kafka 或其他 broker adapter；
- replication、HA、Raft 或跨節點 shard migration；
- risk、account、settlement 或新 Order type；
- lock-free queue、自製 allocator 或未經 benchmark 證明必要的 hot-path 抽象；
- install/export、package publishing、Docker、coverage service 或 Doxygen。

修正應依第 2 節順序進行。P0、P1 未完成前，不應把 staged changes 視為可合併；第 5 節的驗收證據完成前，不應宣稱已符合設計的 Definition of Done。

## 2. 執行順序

| 階段 | 內容 | 可獨立驗證 |
| --- | --- | --- |
| A | WAL durability 與 corruption policy | 是 |
| B | EngineSeq、OrderBook atomicity、completion contract | 是 |
| C | incremental publisher、cursor/replay failure handling | 是 |
| D | configuration version recovery | 是 |
| E | metrics、benchmark、acceptance tests 與 CI | 是 |
| F | 文件與無效介面整理 | 是 |

每個階段都必須同時加入對應測試。不要先重構全部模組，再一次補測試。

## 3. Correctness blockers

### FIX-01：跨 WAL segment 的 group commit durability

#### 問題

`Wal::append(command, false)` 可能在同一 batch 中 rotate segment。Rotate 只同步新 segment header，最後的 `Wal::sync()` 又只同步當前 active segment，因此 rotate 前已 append 的 records 不一定完成 fsync，writer 卻會 apply 並回覆成功。

這違反：

- 只有 durable committed command 可以成功；
- committed command RPO = 0；
- group fsync 必須涵蓋該 batch 的所有 records。

#### 最小修正

1. 在 `Wal` 內保存：
   - `last_appended_position_`；
   - `durable_position_`；
   - active segment 是否含有尚未同步 records。
2. 抽出只在持有 WAL mutex 時呼叫的 `sync_active_unlocked()`：
   - 對 active file 執行 fsync；
   - 成功後把 `durable_position_` 更新為該 segment 最後 append 的 position；
   - 清除 dirty 狀態。
3. Rotate 前若舊 segment dirty，先呼叫 `sync_active_unlocked()`，成功後才建立新 segment。
4. Batch 全部 append 完成後，`Wal::sync()` 同步最後一個 active segment。
5. 任一 fsync、segment create 或 directory fsync 失敗時：
   - 不 apply 尚未確認的 batch；
   - 不回覆 committed；
   - shard 進入 FAILED。
6. 不要求整批具有單一 atomic commit boundary。若 rotate 前的 records 已 durable、後續 append 失敗，它們可在 restart 時 replay；因尚未對外回覆，上游 retry 仍由 Producer identity 正確去重。

不得只把 segment size 放大或禁止 batch 跨 segment；那只能降低發生機率，不能建立 durability guarantee。

#### 涉及檔案

- `src/persistence/wal.hpp`
- `src/persistence/wal.cpp`
- `src/runtime/shard_runtime.cpp`（只在 API spelling 改變時調整）
- WAL persistence tests

#### 必要測試

- 使用很小的 segment size，讓一個 group-commit batch 跨兩個以上 segments。
- 驗證每個含 batch record 的 segment 都在 callback success 前完成 fsync。
- 在舊 segment fsync 後、新 segment append 前注入失敗；restart 後只能 replay durable records，且 retry 可得到一致結果。
- 在最後 segment fsync 失敗時，所有尚未回覆的 callback 得到 `ENGINE_UNAVAILABLE`，不得得到 committed success。

#### 完成條件

沒有任何 success callback 能早於涵蓋該 command record 的成功 fsync。

預估修改量：production 30～70 行，tests 60～120 行。

### FIX-02：只允許修復真正的 WAL crash tail

#### 問題

目前 replay 對每個 segment 的短 frame、非法 length 或不完整 payload 都執行 `resize_file()`。這可能修改 middle segment，或把完整但已毀損的 frame 誤判為 crash tail。

#### 最小修正

1. 掃描 WAL directory 後先驗證所有 segment filename：
   - 檔名必須是非零的 `<first-engine-seq>.wal`；
   - sequence 不可重複；
   - header 中的 `first_engine_seq` 必須與檔名相同。
2. Replay 時明確知道目前是否為最後一個 segment。
3. 依下列規則處理 frame：
   - 剩餘 bytes 少於 length field：只有最後 segment 可截斷；其他位置回傳 `CORRUPT_WAL`。
   - `record_length < minimum` 或大於設定上限：一律 `CORRUPT_WAL`，不可截斷。
   - length 合法但 record bytes 不完整：只有最後 segment 可視為 partial tail 並截斷。
   - record bytes 完整但 checksum、version 或 payload decode 失敗：一律 `CORRUPT_WAL`。
4. 非最後 segment 不可為空；第一筆 record 的 EngineSeq 必須等於 segment header 的 `first_engine_seq`。
5. 截斷最後 partial tail 後同步該 file，並更新 `active_bytes_`、`size_bytes_` 與 durable position。
6. 錯誤訊息至少包含 shard、path、offset、expected/actual sequence；若不擴充 `Error` 結構，先以穩定欄位組成 diagnostic message，但 machine-readable code 必須是 `CORRUPT_WAL`。
7. `decode_committed_command()` 回傳的 codec error 進入 WAL boundary 時，要轉成 `CORRUPT_WAL`，不可洩漏成 `CORRUPT_SNAPSHOT`。

#### 涉及檔案

- `src/persistence/wal.cpp`
- `src/persistence/file_ops.hpp`
- `src/persistence/file_ops.cpp`
- `src/persistence/binary_codec.cpp`（若拆分 codec error context）
- WAL corruption tests

#### 必要測試

- last segment 的 partial length field 與 partial payload 可恢復。
- middle segment 的 partial record 必須 fail-stop，且原檔不可先被截斷。
- 合法長度但 checksum 錯誤必須 fail-stop。
- length 小於最小值、超過最大值、unsupported version 必須 fail-stop。
- segment filename/header mismatch、空的非最後 segment、EngineSeq gap／duplicate／rollback 必須 fail-stop。
- 錯誤 code 與 diagnostic path/offset 正確。

#### 完成條件

Recovery 只能修改最後 segment 的不完整 crash tail；其他 corruption 不得自動修復或略過。

預估修改量：production 40～90 行，tests 100～180 行。

### FIX-03：Admission error 不得留下 EngineSeq 洞

#### 問題

`process_command_batch()` 在 `prepare()` 成功前先遞增 `next_sequence`。若 command 因零 epoch／sequence 等 structural condition 被拒絕，下一筆合法 command 會取得跳號 EngineSeq，之後 live apply 或 recovery 會判定 sequence 不連續。

#### 最小修正

1. 計算 `candidate_sequence = next_sequence + 1`，但先不要更新 `next_sequence`。
2. 以 candidate 呼叫 `prepare()`。
3. 只有 `prepare()` 成功並加入 accepted batch 後，才設定 `next_sequence = candidate_sequence`。
4. 保留 overflow 檢查，禁止 `EngineSeq` wrap。
5. `producer_epoch == 0`、`producer_seq == 0` 與不存在的 stream 仍是 admission error，不寫 WAL、不推進 ProducerState。

#### 必要測試

- 送入 epoch 0／seq 0 command，接著送入合法 seq 1 command。
- 驗證合法 command 使用 `last_committed_engine_seq + 1`，shard 保持 READY。
- Restart 後結果相同且 WAL 無 gap。

#### 完成條件

只有實際加入 WAL batch 的 command 才消耗 tentative EngineSeq。

預估修改量：production 5～15 行，tests 30～60 行。

### FIX-04：OrderBook operation 必須先完整驗證，再一次提交 mutation

#### 問題

- Matching 逐 maker 修改狀態；若後續 maker version 或數量計算 overflow，前面的 mutation 無法回復。
- Amend increase 先移除舊節點，之後才可能發現 destination aggregate overflow。
- Replace 先移除舊 Order，再執行可能失敗的 matching/resting path。

Business rejection 因此可能伴隨部分 state mutation，違反 atomic transition、quantity conservation 與 invariant contract。

#### 最小修正

使用「plan then commit」，不複製整本 OrderBook：

1. 新增 internal `MatchPlan`，只保存本次 command 必要的 bounded 資料：
   - 每個 maker 的 OrderID、fill quantity、計算後 version/status/remaining；
   - taker 最終 remaining/filled/status；
   - 是否及在哪個 price level resting；
   - active-order delta。
2. Planning pass 只讀 book：
   - 依實際 price-time 順序掃描 maker；
   - 對每次 quantity、aggregate 與 version 計算執行 checked arithmetic；
   - Replace planning 時舊 target 仍留在原 queue；因 target 與 maker 位於相反 side，不會影響 crossing scan；
   - 若結果需要 resting，預先計算 destination level aggregate。若 Replace／Amend 位於同 side、同 price，計算時先扣除原 remaining 再加入新 remaining。
3. Planning 任一檢查失敗時回傳 `NUMERIC_OVERFLOW`，book 完全不變。
4. Plan 完成後先 reserve command-local trade／event update vectors，再進行 commit pass。
5. Commit pass 只執行 planning 已證明不會發生 domain failure 的 unlink、field update、erase 與 append。
6. Amend decrease 可保留目前直接 mutation 流程，但所有 arithmetic 必須在修改任何 field 前完成。
7. Allocation failure 或其他 unexpected exception 仍由 shard boundary fail-stop；不得轉成 business rejection 後繼續使用可能已部分修改的 state。

不要以整本 book copy/restore 實作 rollback；其 O(active orders) 成本會改變 hot-path 複雜度，也不是本問題需要的抽象。

#### 涉及檔案

- `src/domain/order_book.hpp`
- `src/domain/order_book.cpp`
- `src/domain/state_machine.cpp`（只在 outcome spelling 改變時調整）
- OrderBook／StateMachine tests

#### 必要測試

- destination level aggregate 接近 `int64_t` 上限時，Amend increase rejection 後原 Order、FIFO、aggregate 與 version 完全不變。
- Replace destination aggregate overflow 時原 Order 完全不變。
- 多 maker matching 在後段遇到 maker version exhaustion 時，所有 maker 與 taker state 完全不變。
- 成功的 multiple match 仍維持 maker-price、FIFO、quantity conservation 與 event order。
- 每個失敗案例執行 invariant checker。

#### 完成條件

任何 business rejection 都不改變 OrderBook；成功 command 的全部 mutation 對 core caller 是單一 atomic transition。

預估修改量：production 100～200 行，tests 100～180 行。

### FIX-05：每個已接受 request 必須得到一次 definitive completion

#### 問題

WAL append、fsync 或 apply 失敗時，當前 batch 已從 ingress queue 移出，但 `fail()` 只處理仍在 queue 的 request。這些 callback 可能永遠不被呼叫。Shutdown 時 completion queue 已滿也可能走 drop 路徑。

#### 最小修正

1. 在 `process_command_batch()` 保存每個 accepted command 與其 completion 的 pending 狀態。
2. 每次成功 dispatch 後標記該 entry completed。
3. append／fsync／apply／snapshot fatal failure 或 unexpected exception 時：
   - shard 先標記 FAILED，停止接受新 request；
   - 對當前 batch 中尚未完成的 entry dispatch `ENGINE_UNAVAILABLE` admission-style result，不得假稱 committed；
   - 再拒絕 ingress queue 內剩餘工作。
4. Completion queue 不得因 `stopping_` 而丟棄 callback。正常 stop 的順序固定為：
   - 停止 ingress；
   - writer drain 或明確拒絕所有 accepted work；
   - completion worker drain；
   - 最後才停止 completion worker。
5. 保留 callback exception boundary；application callback throw 只記 metrics，不重試 callback、不使 shard FAILED。
6. 用 scoped pending-batch guard 或單一 `fail_batch()` helper 收斂所有 early return，避免新增錯誤路徑再次漏 callback。

若 fsync outcome 不明，回覆 `ENGINE_UNAVAILABLE` 是正確行為；restart 後 upstream 以相同 identity retry，若 record 已 durable，會得到原 logical result。

#### 必要測試

- append failure、fsync failure、第一筆／中間 apply failure。
- stop 時 ingress 與 completion queue 同時有資料。
- callback 丟出 exception。
- 每個 request callback invocation count 必須恰為一次；不得 deadlock 或 drop。

#### 完成條件

所有 `SubmitResult.queued == true` 的 request 最終恰好收到一次 committed result 或明確 failure。

預估修改量：production 50～100 行，tests 80～160 行。

## 4. Runtime 與 recovery 必要調整

### FIX-06：EventPublisher 改為 sequential WAL reader

#### 問題

Publisher 每處理一個 EngineSeq 都重新呼叫 `Wal::replay()`，反覆從 retained WAL 起點讀取全部 records。處理 N 筆 command 會接近 O(N²) decode/I/O，且 replay 持有 WAL mutex，會阻塞 shard writer。

#### 最小修正

1. 在 storage 層加入單一用途的 sequential `WalReader`：
   - 建立時從 replay snapshot sequence 後定位一次；
   - 保存目前 segment、offset 與 next EngineSeq；
   - 一次只 decode 下一筆 record；
   - segment 結束後按 header sequence 前進；
   - 不執行 recovery truncation。
2. Writer 在 fsync 成功且 live apply 完成後發布 `PublishableWalPosition`，至少包含：
   - EngineSeq；
   - segment first sequence 或 path identity；
   - record end offset。
3. Publisher reader 不得讀過 publishable position。這比只比較 EngineSeq 更明確地避免讀取 active file 中尚未 durable/apply 的 bytes。
4. Publisher startup：
   - 載入不晚於 durable cursor 的 replay snapshot；
   - reader 從 snapshot 後依序 replay 到 cursor，不 publish；
   - 再從 cursor + 1 逐筆 apply/publish。
5. 每個 EngineSeq 同時只保留一個 pending event batch；sink 未 ACK 前不可 apply 下一筆。
6. Sink failure 使用可中斷的 bounded exponential backoff：1 ms 起，最大 1 s。
7. Cursor 或 replay snapshot persistence failure 不得讓 publisher thread靜默退出：
   - 先記錄 typed failure 與 metrics；
   - retry 可恢復 I/O；
   - 若 storage 已不可用，向 ShardRuntime 回報 publisher FAILED，使 storage-pressure/failure 狀態可被觀察；
   - 保留 WAL，不得推進 retention watermark。
8. Event-replay snapshot 同時支援 command-count 與 time interval，任一先到即建立；目前未使用的 `event_replay_snapshot_interval` 必須真正接入。

不要加入第二份 durable event log；設計已選擇 WAL replay replica，本修正只完成該方案。

#### 涉及檔案

- `src/persistence/wal.hpp`
- `src/persistence/wal.cpp`
- `src/runtime/event_publisher.hpp`
- `src/runtime/event_publisher.cpp`
- `src/runtime/shard_runtime.cpp`
- publisher integration/crash tests

#### 必要測試

- 大量 records 時每筆 record 只 decode 一次；可用 test counter 驗證，不使用 wall-clock threshold。
- Publisher 不讀超過 publishable position。
- ACK failure 重試保持相同 EventID 與 batch。
- ACK success、cursor persist 前 crash，restart 只產生允許的 duplicate。
- cursor 遺失／毀損時從 replay snapshot 安全回退。
- replay snapshot 的 count/time trigger 與 retention watermark。
- cursor/snapshot I/O failure 不刪除仍需要的 WAL。

#### 完成條件

Publisher replay 對新增 records 為 O(N)，不長時間持有 writer 的 WAL mutex，且 publisher failure 只能造成 delay 或 duplicate。

預估修改量：production 180～320 行，tests 160～280 行。

### FIX-07：持久化並驗證 immutable configuration manifests

#### 問題

Engine router 使用本次啟動的 config，但 shard 載入 Snapshot 後直接以 recovered state 取代 genesis state，未驗證兩者是否相容。WAL 只保存 configuration version，沒有保存該版本的內容；沒有 Snapshot 時，歷史版本無法只由 WAL 還原。

#### 最小修正

1. 在 storage target 內加入窄責任 `ConfigStore`，不新增 target 或 service：
   - `data/shard-<id>/config/instruments-<version>.bin`；
   - `data/shard-<id>/config/behavior-<version>.bin`；
   - 使用 versioned little-endian codec、checksum、temporary file、fsync、rename、directory fsync。
2. 同一 version 若檔案已存在，新 supplied value 必須 canonical-byte 相同；不同內容視為 incompatible configuration，拒絕啟動。
3. 啟動時先載入所有 persisted manifests，再 recovery Snapshot/WAL。Snapshot/WAL 引用的每個 version 都必須存在。
4. supplied `instrument_configuration_version`：
   - 已存在時必須內容相同；
   - 不存在時可新增 immutable version；
   - Instrument-to-shard mapping 改變一律拒絕，因第一版沒有 migration protocol；
   - tick/lot 改變且該 Instrument 有 active orders 時拒絕。
5. supplied behavior version同樣 immutable；新 version 可於 stopped restart 啟用。
6. Recovered shard state 必須合併歷史 manifests，並把明確選定的新 current version 設為後續 WAL record 使用的 version；不得讓 Engine router 與 shard state 各自持有不同 mapping。
7. `Engine::open()` 在啟動任一 shard thread 前完成所有 shard config compatibility validation，失敗時不留下部分 READY shards。

#### 涉及檔案

- 新增 `src/persistence/config_store.hpp/.cpp`
- `src/persistence/binary_codec.hpp/.cpp`
- `src/runtime/engine.cpp`
- `src/runtime/shard_runtime.cpp`
- `CMakeLists.txt`
- configuration recovery tests

#### 必要測試

- 第一次啟動持久化 manifests，restart 載入相同內容。
- 相同 version、不同內容拒絕啟動。
- WAL／Snapshot 引用缺失 version 時 fail-stop。
- 有 active orders 時改 tick/lot 拒絕。
- static mapping 改變拒絕。
- 新 behavior version restart 後寫入 WAL，舊 WAL 仍以歷史 version deterministic replay。

#### 完成條件

相同資料目錄不可能同時出現 router 與 recovered shard 使用不同 Instrument/configuration semantics 的 READY 狀態。

預估修改量：production 180～320 行，tests 120～220 行。

### FIX-08：Publisher Snapshot 必須執行完整 invariant validation

#### 問題

Live shard recovery 最後會呼叫 `validate_state()`，但 EventPublisher 載入 replay Snapshot 後沒有做同等驗證。Codec 可解析但語意毀損的 publisher state 可能產生錯誤事件。

#### 最小修正

1. `EventPublisher::open()` 在接受 replay Snapshot 前呼叫 `domain::validate_state()`。
2. 驗證失敗回傳 `CORRUPT_SNAPSHOT`，不得開始 publisher thread。
3. Snapshot sequence 必須不大於 cursor；若 cursor 毀損而回退至 snapshot，仍需先通過 invariants。

#### 必要測試

- active-order count、order location、producer state、tombstone index 任一不一致時 publisher open 失敗。

預估修改量：production 5～15 行，tests 30～60 行。

## 5. 設計完成度與自動化證據

### FIX-09：補齊需求指定的測試矩陣

目前少量 happy-path tests 不能證明 `docs/order-book-design.md` 第 15、20 節的完成條件。只補下列需求明列的 coverage，不加入額外 test framework。

#### Unit tests

- New：resting、partial fill、full fill、multiple maker、capacity rejection。
- Amend：decrease、increase、to-filled、no-op、invalid total、priority preserve/reset。
- Replace：same/different price、optional quantity、crossing、invalid quantity、atomic failure。
- Cancel：active、partially filled、terminal tombstone、unknown。
- ExpectedVersion：success、conflict、no version。
- Producer：retry、identity conflict、sequence gap、stale epoch、new epoch。
- Query：empty/non-empty best、Top-N 0／limit／over-limit、aggregates。
- Tombstone：count eviction、logical-time age eviction、restart preservation。
- Checked arithmetic 與 invariant checker 正反案例。

#### Persistence/determinism tests

- 每個 scalar／enum／variant round trip 與 invalid value。
- Golden WAL、Snapshot、cursor、config fixture；format version incompatibility。
- Partial tail、middle corruption、checksum、sequence gap、segment rotation。
- Snapshot temporary write/fsync/rename failure。
- Live execution、state recovery、publisher replay 三條路徑逐欄位比較 state/result/events。

#### Crash tests

依設計建立獨立 `order_books_crash_tests` target，覆蓋：

- append 前；
- partial append；
- fsync 前後；
- apply 前後；
- response 前；
- sink ACK 後、cursor persist 前；
- Snapshot write/fsync/rename。

File failure injection 只放在 storage 的窄 `FileOps` boundary 或 test-only failpoint；不得出現在 public production API。

#### Reference/property tests

以專案內簡單 reference book 和固定 seed generator，比較 active orders、depth、trades、quantity conservation 與 results。失敗輸出 seed 及可 replay command log；不新增 property-test dependency。

#### 完成條件

每項需求驗收至少有一個可辨識的自動化 test case；不得只由人工 review 或 benchmark 間接證明。

預估新增量：tests/test support 800～1,400 行，production test seam 80～160 行。

### FIX-10：完成有界 metrics 與 latency measurement

#### 問題

目前 registry 只有字串 counter/gauge；`HistogramSnapshot` 幾乎未使用，且多個 RuntimeConfig lag 欄位未接入。這不符合需求列出的 counters、gauges、histograms 與 p50/p99/p99.9。

#### 最小修正

1. 使用固定、文件化的 microsecond buckets，不引入 metrics dependency。
2. Metric dimensions 只使用 bounded enum：command type、command result、shard；禁止以 InstrumentID、OrderID 或錯誤文字作 label。
3. 在 request metadata 記錄 `steady_clock` 時點：received、enqueued、dequeued、WAL appended、WAL durable、execution complete、completion dispatched。
4. 實作需求列出的 command、queue、matching、WAL、snapshot、recovery、producer、publisher counters/gauges/histograms。
5. `MetricsRegistry::snapshot()` 在單一短鎖內複製一致 snapshot，不逐欄位重複加鎖。
6. 提供明確讀取方式。最小公開方案是在 `Engine` 加入 `Result<MetricsSnapshot> metrics(ShardId)`；若改採 typed `MetricsSink` snapshot callback，必須在設計調整紀錄說明，且不得同時保留無法使用的兩套 API。
7. `max_publish_lag_age` 與 `max_publish_lag_bytes` 接入 publisher/retention metrics 與 warning/critical/storage-pressure decision；不得只保留設定欄位。

#### 必要測試

- Fixed buckets 的 count、p50、p99、p99.9、max。
- Counter saturation、gauge replacement、snapshot consistency。
- 各 latency boundary 使用 injected/test clock，測試不依賴 sleep。
- Sink throw 不影響 shard；高基數 key 無法進入 registry。
- Publisher lag threshold 與 storage-pressure 行為。

#### 完成條件

所有公開 metrics/config 欄位都有實際 producer、consumer 與測試，且不影響 command/event ordering。

預估修改量：production 220～400 行，tests 120～220 行。

### FIX-11：Benchmark 必須覆蓋設計要求的 workload

#### 最小修正

擴充既有 project-local harness，至少包含：

- resting New；
- crossing New；
- Cancel；
- Amend decrease／increase；
- Replace crossing；
- multiple match；
- mixed single instrument；
- durable group commit；
- Snapshot + WAL recovery。

每個 workload：

- 具有固定 seed、warmup、measured iteration；
- 輸出 commands/s、trades/s、p50、p99、p99.9、max；
- 記錄 active orders/levels、OS、CPU、compiler、build type；
- durable workload 額外記錄 path、fsync 與 group-commit config；
- recovery workload 記錄 snapshot order count、WAL records/bytes 與 elapsed time。

CI 只以小型參數執行 smoke，不設定效能 threshold。

#### 完成條件

每個列出的 workload 可從 Release preset 重現；輸出足以判斷兩次結果是否使用可比較環境。

預估修改量：benchmark 250～450 行，少量共用 test data helper 50～100 行。

### FIX-12：CI 驗證實際新增的產品程式碼

#### 最小修正

1. 保留 Linux GCC、Linux Clang、ASan/UBSan、macOS Apple Clang matrix。
2. 所有 compiler jobs 對產品 targets 啟用 warnings-as-errors。
3. clang-format 檢查所有 tracked C/C++ source/header，不只 smoke test；使用明確副檔名清單。
4. Linux Release job configure/build `release-benchmark` 並以小型參數執行 benchmark smoke。
5. Crash tests 若只支援 POSIX，Linux/macOS 都執行；若某 failpoint 只能 Linux 使用，測試內明確 skip 並記錄原因。
6. CI 不新增 performance regression gate。

預估修改量：CI/CMake 30～70 行。

## 6. 必要的一致性與冗餘整理

### FIX-13：Scalar ID aliases 與設計文字統一

設計目前宣稱所有 ID 都必須是 strong type，但實作除 `OrderId` 外使用 fixed-width aliases。需求規格沒有要求 compile-time non-interchangeability；現在全面導入 wrapper 會大幅改動 codec、hash、arithmetic 與 public API，且不是修復現有 observable correctness 所必需。

採取最小調整：

1. 保留 `ShardId`、`InstrumentId`、`ProducerId`、sequence/version 等 fixed-width aliases。
2. 保留 `OrderId` 為 128-bit value type，所有 enum 繼續使用 `enum class`。
3. 將設計第 2.2 節規則改成：公開型別必須使用具語意名稱、固定寬度的 type alias 或 value type；不得在 API spelling 直接使用未命名裸整數。只有需要 composite representation 的 `OrderId` 強制為 value type。
4. 在設計「實作調整紀錄」說明：原決策、降低不必要 wrapper/codec churn 的原因，以及 scalar aliases 無法防止誤傳的取捨。

若未來公開 API 的誤傳事故或靜態分析證明 aliases 不足，再另案導入 strong wrappers；本次不預建。

預估修改量：design 5～12 行，production 不變。

### FIX-14：移除未使用的 producer fingerprint

需求只要求 stable digest/deduplication；目前實作已持久化完整 canonical command bytes，並以逐 byte equality 作 authoritative comparison。額外的 FNV fingerprint 已保存與序列化，卻未用於 duplicate path，而且設計原文要求的 length + CRC32C 也未真正實作。

採取最小調整：

1. 移除 `last_command_fingerprint`、計算函式與 Snapshot serialization 欄位。
2. ProducerState 保留 `last_canonical_command` 與 `last_result`。
3. Duplicate 判定維持 canonical bytes 逐 byte equality，避免 collision correctness 問題。
4. 因格式尚未發布，可在第一版 Snapshot format 固定前直接調整；若已有外部 fixture，必須提升未發布 format version 並更新 golden fixture。
5. 將設計第 6.3 節的 fingerprint 快速篩選刪除，記錄第一版不為未證明的效能瓶頸保存冗餘欄位。

預估修改量：production/codec 15～30 行，tests 10～25 行，design 3～8 行。

### FIX-15：落實或移除 dead public/internal interfaces

逐一處理，不允許保留「未來可能使用」的欄位：

- `Wal::durable_position()`：由 FIX-01/FIX-06 修正並實際用於 publishable position，不移除。
- `event_replay_snapshot_interval`：由 FIX-06 接入 time trigger。
- `max_publish_lag_age`、`max_publish_lag_bytes`：由 FIX-10 接入 lag/storage-pressure。
- `HistogramSnapshot`、`MetricsSnapshot`：由 FIX-10 完成實際讀取路徑。
- 若完成上述功能後仍有未引用設定或型別，應刪除，不新增空 adapter。

### FIX-16：文件狀態與連結同步

1. `docs/order-book-design.md` 第 22 節不再寫「尚未進入實作」。
2. 實作完成後逐項記錄實際偏離：
   - scalar ID alias；
   - 移除 fingerprint；
   - 若任何 API spelling 因實作調整，說明影響。
3. README 與 CONTRIBUTING 的主要實作依據改為 `docs/order-book-design.md`；`docs/project-design.md` 只描述專案骨架時，不再稱為目前業務實作 source of truth。
4. README 只宣稱已通過自動化證據的能力；在 FIX-09～12 完成前，標明 implementation in progress。
5. 列出實際可重現的 Conan/CMake/test/benchmark 指令，並由乾淨 build directory 驗證。

預估修改量：文件 15～35 行。

## 7. 建議的測試基礎設施最小邊界

Crash/failure tests 需要 fault injection，但不得把 test concern 擴散到 domain/public API。最小邊界如下：

```text
domain
  不知道 filesystem、clock 或 failpoint

storage
  FileOps interface
    open/read/write/fsync/rename/directory-fsync/truncate/remove
  PosixFileOps (production)
  FaultInjectingFileOps (tests only)

runtime
  Clock interface 或窄 function object
    system timestamp：寫入 WAL/event occurred_at
    steady timestamp：latency/interval
  Production clock
  Fake clock (tests only)
```

只在確實需要 failure/clock 控制的 constructor 注入，預設 production wiring 由 `Engine::open()` 建立。不要建立通用 dependency-injection container。

Subprocess crash tests 只需一個小型 helper executable，以 command-line failpoint 決定退出位置。Failpoint 名稱採固定 enum，不允許 production build 由任意字串觸發。

## 8. 修改量總估計

| 類別 | 預估新增／修改行數 |
| --- | ---: |
| P0/P1 correctness production code | 400～750 |
| Configuration/publisher runtime | 350～600 |
| Metrics | 220～400 |
| Functional/persistence/crash/property tests | 1,200～2,000 |
| Benchmark | 300～550 |
| CMake/CI/docs | 70～150 |
| 合計 | 約 2,500～4,000 |

估計包含必要測試，並以「完整符合目前設計 Definition of Done」為準。若只修正會破壞 correctness 的 FIX-01～08，約 900～1,600 行；但此時必須在 README 與設計狀態明確標示尚未完成驗收，不能宣稱整體設計已實作完成。

## 9. 最終驗收清單

完成全部必要調整後，逐項確認：

- [ ] 所有 success result 的 WAL record 已由成功 fsync 涵蓋。
- [ ] Recovery 只截斷最後 incomplete tail，其他 corruption fail-stop。
- [ ] Admission error 不消耗 EngineSeq 或 ProducerSeq。
- [ ] Business rejection 不留下任何 domain mutation。
- [ ] 每個 accepted request completion 恰好一次。
- [ ] Publisher sequential replay，失敗只造成 delay/duplicate，不造成 event loss。
- [ ] Router、Snapshot、WAL 使用相容且可取得的 immutable config version。
- [ ] Live、recovery、publisher 三條路徑產生相同 state/result/events。
- [ ] 需求列出的 metrics、benchmark workloads 與 acceptance tests 都可執行。
- [ ] GCC、Clang、Apple Clang、ASan/UBSan、clang-tidy、format、benchmark smoke 通過。
- [ ] `git diff --check` 通過。
- [ ] 設計調整紀錄、README 與 CONTRIBUTING 與實作一致。
- [ ] 沒有新增任何第 1 節排除的元件或 dependency。

只有清單全部完成，才可將 staged changes 判定為「剛好滿足設計，沒有必要功能缺漏，也沒有超出需求的預建實作」。
