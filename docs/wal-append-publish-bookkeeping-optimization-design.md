# WAL append publish bookkeeping 最小化設計

## 1. Review 結論

本設計承接
`docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-design-review.md` 與其 2026-09-20
重測結果。有效且非 supply-limited 的 authoritative Engine ceiling 在 group=4,096、W=2 時約為
169K commands/s；同 case 的 WAL append 約為 1,598 ns/command，derived ceiling 約為 626K
commands/s，是 production ordering 中第一個穩定低於 1M commands/s 的同步 phase。

bounded parallel prepare 已將 W=2 prepare wall time降至約 663--692 ns/command；不應在沒有新證據時
再增加 worker。W=2 的 `publish` 在 group=4,096／8,192 五輪中約為 416--431 ns/command，約占 WAL
append 的 27%--34%，而目前實作在每筆 command 都會：

- 累加同一 chunk 的 `active_bytes_` 與 `size_bytes_`；
- 複製同一個 `active_segment_` filesystem path來建立 `WalPosition`；
- 指派只需代表最後一筆 record 的 `last_appended_position_`。

這些狀態對外只需要在每個成功寫入的 segment chunk 結束時更新一次。`Wal` mutex 在整個 append
期間持有，其他 reader 不可能觀察同一 chunk 內的中間 position。因此，本階段只移除這項已由 profile
與程式碼共同支持的 per-command bookkeeping：

1. record cache仍依原順序逐筆 publish；
2. byte counters改為以 chunk byte size一次累加；
3. `WalPosition` 與 `last_appended_position_` 改為每個成功 chunk只建立／指派一次，指向該 chunk
   最後一筆 record；
4. 多 segment batch仍在每個成功 chunk後 publish狀態，保留後續 chunk失敗時既有 fail-stop邊界。

Review 判定：這是目前證據能支持的最小 production optimization。它不保證單獨將 WAL append提升至
1M commands/s，也不把 `plan/copy` 的跨輪變異推測成已確認根因。若實測收益不足，下一步應先細分
`plan/copy`／record-cache成本，而不是在本案預先導入新 buffer representation、`writev`、buffer pool
或替換 `records_` container。

## 2. 需求理解與合理假設

### 2.1 必須滿足

- 保持目前 production ordering：WAL append、per-group `fsync`、StateMachine apply、publisher notify、
  completion enqueue。
- WAL on-disk bytes、segment boundary、CRC、EngineSeq ordering與 replay結果逐位元不變。
- `append_batch()` 回傳最後一筆成功 append record的 segment與 end offset。
- `last_appended_position_`、`size_bytes_`、`active_bytes_`、`records_` 與 `active_dirty_` 在正常成功路徑
  保持一致。
- multi-segment batch若後續 chunk I/O失敗，先前已成功寫入的 chunk仍維持既有 fail-stop可恢復狀態。
- normal path不增加 clock、allocation、lock、thread或 runtime branch。
- profile-on path沿用既有 `publish_ns` 邊界；不為這個小修改增加 production metrics。

### 2.2 合理假設

- `append_prepared_unlocked()` 由持有 `Wal::mutex_` 的 caller呼叫，publish期間沒有並行 reader。
- `PreparedChunk::bytes` 已是該 chunk實際寫入的完整 bytes；其 size等於該 chunk各 frame size總和。
- batch總 byte overflow與 `records_` capacity已在第一次 I/O前完成檢查／reserve；本案不改其錯誤語意。
- append或 sync I/O failure後 shard沿用既有 fail-stop行為，不要求同一 `Wal` instance繼續服務。
- 目前 `publish_ns` 的穩定性足以驗證本修改；是否需要再拆 publish children由本案結果決定。

## 3. 範圍

### 3.1 In scope

- `src/persistence/wal.cpp` 中 `Wal::append_prepared_unlocked()` 的 publish loop；
- 同 segment及跨 segment batch的 position、byte counters、record ordering與 replay測試；
- 既有 direct WAL與 Engine writer benchmark的修改前／後比較；
- 必要時在既有 persistence test中補一個針對跨 chunk最後位置的 assertion。

### 3.2 Out of scope

- WAL record／segment格式、codec、CRC與 recovery parser；
- parallel prepare的 lane count、threshold、worker lifecycle或 public configuration；
- `PreparedRecord`、`PreparedChunk`、`CachedRecord` 或 `records_` 的 representation重設計；
- `writev`、`pwrite`、mmap、direct I/O、`io_uring` 或 background flush；
- per-command frame allocation、chunk byte copy、record-cache command copy或 container更換；
- group size、group delay、`fsync` policy、durability／acknowledgement boundary；
- StateMachine、Publisher worker、Completion worker、Snapshot或retention；
- 新 CLI、新 public API、新 runtime config、新 dependency或通用 profiler；
- CI內的固定 RPS pass/fail gate。

這些項目不是移除已確認 per-command bookkeeping所必需，且同時修改會讓效能收益無法歸因。

## 4. 架構與資料流

外部資料流不變：

```text
Shard writer
  -> Wal::append_batch(accepted)
       -> prepare records（既有 W=1／W=2）
       -> plan segment chunks、copy frame bytes
       -> for each chunk
            rotate if required
            write_all(chunk.bytes)
            publish cached records in input order
            publish one terminal position for the chunk
  -> Wal::sync()
  -> StateMachine::apply(...)
```

修改前的 chunk publish概念如下：

```text
for every record in successfully written chunk
  active_bytes += frame_size
  size_bytes += frame_size
  copy active_segment path into WalPosition
  assign last_appended_position
  move CachedRecord into records_
```

修改後：

```text
for every record in successfully written chunk
  move CachedRecord into records_

active_bytes += chunk.bytes.size
size_bytes += chunk.bytes.size
last_record = chunk.first_record + chunk.record_count - 1
last_position = {last_record.engine_seq, active_segment, active_bytes}
last_appended_position = last_position
active_dirty = true
```

這仍是 per-chunk commit-to-memory，不是整批一次 publish。保留 per-chunk邊界是為了讓跨 segment batch
在後續 rotate／write失敗時，已寫成功的前置 chunk與 in-memory last appended position維持一致。

## 5. 核心資料與不變量

本案不新增或修改資料型別。現有資料責任保持：

| 資料 | 責任 | 修改後不變量 |
| --- | --- | --- |
| `PreparedChunk::bytes` | 實際交給 `write_all()` 的連續 bytes | 成功 write後其 size才可加入 byte counters |
| `CachedRecord` | publisher/replay所需 command與 byte index | 依 input／EngineSeq順序移入 `records_` |
| `active_bytes_` | active segment目前 end offset | 每個成功 chunk增加該 chunk byte size |
| `size_bytes_` | retained WAL總 bytes | 每個成功 chunk增加相同 byte size |
| `last_appended_position_` | 最後成功 append位置 | 每個成功 chunk指向該 chunk最後 record |
| `active_dirty_` | active descriptor含未 sync資料 | 任一 chunk成功 write後為 true |

每個 chunk必須驗證或由既有 planner保證：

```text
record_count > 0
first_record + record_count <= prepared_records.size
chunk.bytes.size fits uint64_t
active_bytes + chunk.bytes.size <= segment_size
```

其中 record count與 segment capacity已是 planner成立條件；實作不重複加入 expensive runtime scan。
只保留避免 index underflow／overflow所需的局部安全寫法。

## 6. 介面與元件責任

### 6.1 `Wal` public API

以下介面完全不變：

```cpp
Result<WalPosition> append(const domain::CommittedCommand& command);
Result<WalPosition> append_batch(
    std::span<const domain::CommittedCommand> commands);
Result<WalPosition> append_batch_profiled(
    std::span<const domain::CommittedCommand> commands,
    WalAppendProfile& profile);
Status sync();
```

呼叫者不需要知道 chunk數量；回傳值仍是整個 batch最後成功 record的位置。

### 6.2 `append_prepared_unlocked()`

此函式仍負責 prepare結果的 segment plan、write與 in-memory publication。本案只調整 write成功後
publish block：

1. 由 `chunk.bytes.size()`取得一次 `chunk_bytes`；
2. 先按既有順序 move該 chunk的 `cached_records` 到 `records_`；
3. 將 `active_bytes_` 與 `size_bytes_` 各增加 `chunk_bytes` 一次；
4. 以 chunk最後 record的 EngineSeq、目前 active segment與新 end offset建立一次 `WalPosition`；
5. 指派 `last_appended_position_` 與 local return position；
6. 設定 `active_dirty_ = true`。

不得將所有 chunks延後到 batch結束才一起 publish，也不得為省一次 path copy而讓 rotation後的位置引用錯誤
segment。

### 6.3 Profile

`WalAppendProfile::publish_ns` 的邊界保持包住同一 publish block，因此修改前後可以直接比較。
不新增 `position_update_ns` 等細碎 clock，原因是：

- 目標工作本身很短，新 clock可能大於被測成本；
- 是否有效可由既有 publish parent與完整 append／throughput交叉驗證；
- 本案不需要建立長期 public observability contract。

## 7. 錯誤處理與 durability 語意

- rotate、directory sync、data write與 group `fsync` 的呼叫順序完全不變。
- `write_all()` 失敗的 chunk不得更新 byte counters、record cache、last position或dirty flag；先前成功
  chunks已完成的狀態保留。
- record-cache reserve與 `cached_records` construction仍在第一個 write前完成，避免新增 write後 allocation
  failure點。
- 本案不把 append成功解讀為 durable；只有後續 `Wal::sync()` 成功才更新 durable position。
- `append_batch_profiled()` 的錯誤、空 batch與 profile reset語意不變。
- 不新增 catch-and-continue；allocation exception與不可恢復錯誤沿用目前 fail-stop策略。

## 8. 測試策略

### 8.1 既有測試必須繼續通過

- normal／profiled batch的 position、size與 replay一致；
- W=1／2／4 parallel prepare byte identity；
- threshold fallback與不整除 lane partition；
- rotation跨多 segment後每筆 command可依序 replay；
- close／reopen後 durable head與 WAL bytes一致；
- empty batch不改變任何 WAL狀態；
- Release、Debug、ASan與UBSan test suites。

### 8.2 必要新增或加強的 assertion

在既有 rotation batch test擴充即可，不另建測試 framework：

- 一個 batch強制每個 segment只容納一筆 record；
- append回傳的 EngineSeq等於 batch最後 command；
- 回傳 segment filename等於最後 segment；
- `end_offset` 等於最後 segment header加最後 frame bytes；
- `last_engine_seq()` 與 append回傳值一致；
- 未 rotation 的 batch在 sync前 durable position仍未前進；rotation batch可因既有 segment切換同步而
  先到最後一個已完成 segment，但不得到最後尚未 sync的 chunk；明確 `sync()`後 durable position到
  最後 record；
- reopen/replay後 records、ordering與原 commands一致。

一般測試不得斷言 `publish_ns` 非零或固定耗時，也不以 RPS作 CI correctness gate。

## 9. 效能驗證與接受條件

### 9.1 比較方式

修改前先保留相同 source identity的 baseline Release binary；修改後使用相同 compiler、flags、CPU
affinity、filesystem、per-group `fsync`、group delay與 producer lanes。每 case五輪、交錯執行、每輪至少
20秒，保留所有 correctness通過的慢輪：

1. direct WAL：group=4,096／8,192、W=2、profile off；
2. writer hot-path：group=4,096、W=2、profile off／on；
3. authoritative Engine：group=4,096、W=2、profile off；
4. writer group=8,192只作 phase方向性資料；actual commands/group低於90%時仍不得宣稱 frontier。

Profile-on沿用已校準的 sampling interval；若修改後 off/on median bias超過5%，先調整 sampling並完整
重測，不得挑選低 bias輪次。

### 9.2 Correctness gate

- WAL byte-for-byte、replay、EngineSeq continuity、completion exactly-once與 order-book結果全部通過；
- 所有正式輪 exit 0，沒有 timeout、publisher failure、storage pressure或資源耗盡；
- source、index、worktree、baseline／candidate binary identity在各自矩陣內固定；
- staged changes在整個流程中保持不變。

### 9.3 Performance判定

本修改只有在以下條件成立時視為成功候選：

- g4096/W2 writer `publish_ns_per_command` 五輪 median至少下降15%；
- g4096/W2 WAL append ns/command同方向下降，且 writer profile bias不超過5%；
- direct WAL與authoritative Engine五輪 median均不得退化超過3%；
- p99／p99.9、CPU與context switches沒有跨多數輪穩定惡化；
- storage sync tail需獨立列出，不能把慢 `fsync` 誤判成 publish regression。

15%是保留此 performance patch的實驗門檻，不是 CI或production SLO。若未達門檻，報告結論應為
`no material gain`，不再把更多 speculative buffer改動塞進本案。

## 10. 關鍵決策與取捨

### 10.1 為何只做 per-chunk更新

它直接移除穩定 publish phase中的 per-command不變工作，且不改資料 ownership、I/O API或 durability。
以 chunk為邊界可同時保留 multi-segment partial failure狀態；若改成整批結束才更新，雖能再少量減少
assign，卻會改變後續 chunk失敗時的 in-memory position語意，得不償失。

### 10.2 為何不改 `records_` container

`records_` 同時服務 replay、`next_after()`、`bytes_after()`與 retention。改成 deque、segmented index或
offset-only cache會影響 publisher讀路徑與 retention複雜度；目前只有 plan/copy偶發變異，沒有足夠
證據證明 container是穩態根因。

### 10.3 為何不導入 `writev` 或 contiguous prepared batch

實測 `chunk_copy` 僅占 plan/copy的小部分，write phase本身也不是主要成本。導入 scatter/gather會新增
IOV limit、partial write、segment split與跨平台測試負擔，超出移除 position bookkeeping的必要範圍。

### 10.4 為何不繼續增加 prepare workers

W=2已把 prepare parent提升到約1.45M--1.51M derived ceiling；過去W=4 direct WAL沒有穩定優於W=2，
且會增加 CPU與context switches。本案不重新打開已結案的 lane-count決策。

## 11. 已知限制與後續方向

- 即使完全移除目前約420 ns/command的 publish成本，g4096/W2 WAL append的理想上限仍不保證高於
  1M commands/s；實際收益也一定小於此理想值。
- g8192 Engine workload目前 supply-limited，不能用它決定authoritative frontier。
- `plan/copy` 有顯著跨輪變異，但現有資料不能區分 record-cache reserve、CachedRecord construction、
  segment planning與chunk materialization；本案不猜測其中任何一項。
- StateMachine isolated ceiling約1.079M/s，仍缺少1.2M headroom；WAL append改善後它可能成為下一個
  同步 ceiling。
- Publisher／Completion worker的 service rate與lag不在本案優化範圍。

若本修改通過效能 gate但 WAL append仍低於1M/s，下一份設計只應新增 benchmark-only nested attribution，
把 `plan/copy` 分成 record-cache build／capacity growth、segment planning與chunk materialization，並把
`publish` 分成 cache insertion與terminal position update。取得有效bias後再決定是否需要 prepared batch
representation或cache container變更。

## 12. 實作清單與完成定義

預期必要修改僅包含：

1. `src/persistence/wal.cpp`：將 publish block的 byte／position更新由 per-record提升至 per-chunk；
2. `tests/integration/persistence_test.cpp`：擴充既有 rotation batch test的最後位置與durable boundary
   assertions；
3. `docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md`：記錄第9節的原始矩陣、
   彙整結果、門檻判定與候選保留結論；
4. 若現有測試名稱已完整表達契約，不新增重複測試檔；
5. 不修改 headers、CMake、public API、runtime config、benchmark CLI或 metrics schema。

完成定義：

- 實作逐項符合第6節，沒有順帶重構 prepare／plan／write；
- 第7、8節 correctness與sanitizer gate全數通過；
- 第9節 benchmark能比較 baseline與candidate並誠實保留慢輪；
- 結果文件明確寫出是否達到15% publish reduction及是否保留候選；
- `git diff --cached` 在實作、測試與壓測期間保持不變。
