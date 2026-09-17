# WAL Append Hot-path Optimization 設計

## 1. Review 結論

本設計針對 `docs/wal-write-ceiling-benchmark-report.md` 已量化的瓶頸，只處理目前每筆
command 都執行 `open/write/close`，以及 Engine group commit 未合併實際 write 的問題。

必要修改只有兩項：

1. `Wal` 在生命週期內持有 active segment 的 file descriptor，append、sync 與 rotation
   共用該 descriptor。
2. `Wal` 提供 batch append；Shard writer 已形成的 accepted group 與
   `wal_write_ceiling` workload 改用此介面，使通常未跨 segment 的一組 command 合併為
   一次 `write`，之後仍只執行一次既有 `fsync`。

這兩項直接對應壓測證據，且不改 WAL 格式、durability boundary、Engine 公開 API、matching、
publisher、Snapshot 或 recovery 規則。設計不承諾本階段即可達到一百萬 commands/s；完成後
以同一 benchmark 重新建立上限，再決定是否需要下一階段 codec、allocation、CRC 或 sync
優化。

Review 判定：此範圍符合「先消除已證實瓶頸」的需求，內容皆為實作所需，沒有為尚未證實的
問題預建 framework。

## 2. 需求理解與證據

### 2.1 已確認需求

- 目標情境為單一 shard、單一 instrument、單一 WAL writer。
- 保持 C++20、POSIX filesystem、現有 binary codec 與 CRC32C。
- 一組 command 必須在 WAL append 完成且 group `fsync` 成功後，才可執行 live state
  transition 與成功 completion。
- crash recovery 仍以完整 frame、checksum 與連續 EngineSeq 判定可重放 records。
- publisher 只能看到 live apply 與 invariant validation 後通知的 durable position。
- macOS 開發與 Linux 部署都必須可編譯；不加入 Linux-only I/O API。
- 修改後必須以既有 WAL ceiling 與 Engine durable workload 驗證正確性和效能。

### 2.2 壓測事實

目前 group size 256：

- durable throughput：81,756 commands/s；
- append-only throughput：189,306 commands/s；
- durable group 平均約 3.131 ms，其中 sync path 增量估計約 1.779 ms；
- `strace` 顯示每筆 command 各有一組 `openat/write/close`，三者占 traced syscall time
  97.99%。

因此完全移除顯式 group sync 仍只能達約 189k commands/s。調大 group 只會攤薄 sync，
不會消除逐筆 file descriptor lifecycle 與逐筆 write；先修改 WAL append path 是必要步驟。

### 2.3 合理假設

- 同一 `Wal` 的 mutation 仍由 shard writer 發起；publisher 的 read API 透過既有 mutex
  與 writer 同步。
- `group_commit_max_commands` 提供 runtime group 的自然記憶體上限；batch API 不新增另一個
  production batch-size 設定。
- 現有 record format、segment size 與 recovery parser 已正確，本階段不改 on-disk bytes。
- I/O failure 後 shard 採 fail-stop；不要求同一 `Wal` instance 在 partial write 後繼續服務。

## 3. 範圍

### 3.1 In scope

- active WAL segment descriptor 的 RAII lifecycle；
- 單筆與 batch append 共用的 frame preparation 與 write path；
- batch 跨 segment 時的切分、rotation、sync 與 metadata 更新；
- Shard writer 改用 batch append；
- WAL ceiling benchmark 改為量測 production batch append；
- persistence、runtime 與 benchmark smoke tests；
- 主設計文件補記新的內部 append 行為與實作調整。

### 3.2 Out of scope

- `io_uring`、AIO、mmap、direct I/O；
- `writev`、`pwrite` 或 platform-specific batching；
- `fdatasync`、sync policy 或 group commit default 調整；
- WAL record／segment format 或 checksum 變更；
- codec、CRC32C implementation、allocator 或 `records_` representation 優化；
- 多 writer、background flush thread 或新的 queue；
- publisher batching、cursor persistence 或 Snapshot 優化；
- 新 CLI flag、新 runtime config 或新第三方 dependency；
- performance threshold 作為 shared CI gate；
- production fault-injection framework。

上述項目只有在本階段完成後的新 profile 證明為主要限制時，才另案設計。

## 4. 不變的架構與邊界

整體資料流保持：

```text
Shard writer
  -> admission / sequence validation
  -> accepted CommittedCommand group
  -> Wal::append_batch(group)
       -> encode + frame + CRC
       -> one write per touched segment
  -> Wal::sync()                 # existing durability boundary
  -> StateMachine::apply in EngineSeq order
  -> invariant validation
  -> notify publisher with durable position
  -> dispatch completions
```

模組責任不變：

| 元件 | 責任 |
| --- | --- |
| `ShardRuntime` | 建立 accepted group、呼叫 batch append 與一次 sync、維持 fail-stop |
| `Wal` | frame encoding、segment placement、write、durable position、replay index |
| `FileOps` | 窄 POSIX open/write/fsync/close boundary |
| `StateMachine` | durable success 後才執行 deterministic domain transition |
| `EventPublisher` | 只讀已被 runtime 宣告 publishable 的 WAL 上界 |

不新增 public library target，也不把 storage batch 細節暴露到 `include/order_books/*`。

## 5. WAL 介面

在 internal `storage::Wal` 增加：

```cpp
Result<WalPosition> append_batch(
    std::span<const domain::CommittedCommand> commands);
```

契約：

- `commands` 必須非空；空 batch 回傳 `ErrorCode::wal_failure`，不執行 I/O。
- command 順序即 WAL frame 順序；介面不排序、不去重、不改寫 EngineSeq。
- 成功回傳最後一筆 command 的 `WalPosition`。
- 成功只代表 bytes 已完整交給 filesystem，不代表 durable；commit boundary 仍是後續成功的
  `Wal::sync()`。
- batch 可以跨 segment，但任何單一 frame 不得跨 segment。
- `append()` 保留，維持現有 callers 與測試的 source compatibility；它與 batch API 必須
  共用同一 internal preparation/write implementation，不能保留第二套 record framing。
- `sync()`、`replay()`、`next_after()`、`bytes_after()`、`retain_through()` 與
  `durable_position()` 的外部語意不變。

不新增 `append_and_sync()`：batching 與 durability policy 已由 Shard writer 清楚負責，合併
介面會把 runtime group policy帶入 storage layer，且不是消除目前瓶頸的必要條件。

## 6. Active descriptor lifecycle

`Wal` 新增只在既有 mutex 保護下使用的 active descriptor state。具體成員名稱可由實作決定，
但生命週期必須符合：

1. 新 WAL 建立第一個 segment 時，寫入並 sync header、sync directory 後保留該 descriptor。
2. 開啟既有 WAL 時，選出 active segment 後開啟一個 append descriptor 並保留。
3. `append()`／`append_batch()` 直接使用 active descriptor，不逐筆 open／close。
4. `sync()` 直接 fsync active descriptor，不為每組重新 open／close。
5. rotation 前先 sync dirty active segment；舊 descriptor 關閉後建立並持久化新 segment，
   然後保留新 descriptor。
6. `Wal` destructor 關閉仍有效的 descriptor；close error 不改寫已回報的 commit 結果。
7. open 或 rotation 任一步失敗即回傳 storage error；runtime 依既有規則進入 FAILED。

descriptor 不公開，不可由 caller 借用。`Wal` 維持 non-copyable；不需要新增 generic descriptor
framework。現有 `FileOps` 已提供所需 operation，除非實作發現缺少安全轉移 descriptor ownership
的最小 helper，否則不修改 `file_ops.*`。

Recovery 可能以另一個 descriptor 截斷 last-segment partial tail；active descriptor 使用
`O_APPEND`，截斷後仍可繼續 append。replay 完成後的 `active_bytes_` 必須反映實際截斷位置。

## 7. Batch preparation 與 write algorithm

### 7.1 Preflight

持有 WAL mutex 後，在第一個 batch byte 寫入前完成：

- 對每筆 command 執行現有 binary encoding、record version 與 CRC32C framing；
- 驗證每個 frame 不超過既有 `kMaxRecordSize` 且可放入空 segment；
- 驗證 frame bytes、cumulative bytes 與 container size arithmetic 不 overflow；
- 預留 batch 所需暫存與 `records_` capacity，避免正常成功路徑在寫入後才因 vector growth
  失敗。
- `records_` capacity 可採 overflow-safe 的 geometric growth；這只是 write 前 preflight 的
  capacity policy，不改變 `records_` representation，也不引入 custom allocator。

不在 storage layer 驗證 producer sequence 或 business semantics；那仍是 runtime/domain 責任。
既有 WAL 允許 index 記錄 sequence discontinuity 的行為保持不變。

### 7.2 Segment plan

依 command 順序把 prepared frames 放入目前 segment：

- 若所有 frames 都放得下，組成一個 contiguous byte buffer並呼叫一次既有
  `FileOps::write_all()`。
- 若下一個 frame 放不下，先寫出目前 segment 的 buffer；依既有 rotation durability 流程
  sync 舊 segment、建立並持久化新 header，再繼續累積。
- 一個 batch 因 rotation 可以產生多次 write，但每個 touched segment 至多一次 batch-data
  write；header write 不計入 batch-data write。
- 本階段選擇 contiguous buffer而非 `writev`，以重用已驗證的 `write_all()`、簡化 partial
  write 處理並保持 Linux/macOS 一致。額外 memory copy 留待重新 profile 後判斷。

### 7.3 Metadata publication

每次 batch-data `write_all()` 完整成功後，才更新該 chunk 對應的：

- `active_bytes_` 與 `size_bytes_`；
- `last_appended_position_`；
- `active_dirty_`；
- `records_` 的 command、frame bytes、cumulative bytes 與 continuity id。

若 write 回傳失敗，不能把未確認完整寫入的 chunk 發布到 in-memory index。caller 必須
fail-stop；restart 時沿用現有規則截斷 last segment partial frame，完整且 checksum 正確的
prefix 仍可 recovery。這與目前逐筆 append 發生中途 I/O failure 的 uncertain outcome 相同，
不新增 batch atomicity 承諾。

`append_batch()` 不得宣稱「all-or-nothing disk transaction」。若 batch 跨 segment，rotation
可能已使前一 segment 的完整 prefix durable；但在本次 process 中，整批仍不得 apply 或回傳
成功 completion。restart 以 WAL valid prefix 為準。

## 8. Runtime 整合

`ShardRuntime::process_command_batch()` 保留 admission、publisher pressure 與 EngineSeq 分配
邏輯，只將：

```text
for each accepted command:
    wal.append(command)
wal.sync()
```

替換為：

```text
wal.append_batch(accepted)
wal.sync()
```

錯誤處理維持：

- append batch 或 sync 任一失敗，所有尚未完成 request 回傳 unavailable；
- 不執行任何 live `StateMachine::apply`；
- shard 進入 FAILED；
- 不通知 publisher 新 publishable position；
- 不回傳部分 batch success。

batch append 持有 WAL mutex 的時間會比單筆 append 長，但 publisher 在 writer 完成 group apply
前本來就不能發布該 group。此變更減少 mutex acquisition 次數，不改 publisher ordering 或
durability boundary。

## 9. Benchmark 整合

`wal_write_ceiling` 必須改用 `append_batch()`，否則 benchmark 量到的不是 production storage
path。每個 group 的 command construction 仍位於既有 append phase 計時範圍，避免悄悄縮小
測量邊界。

保留所有 CLI、輸出欄位、sync mode、segment size、final sync、reopen／replay 與 correctness
validation。不得加入 old/new implementation switch；舊行為只保留在 baseline report，避免
建立兩套 production append path。

重新量測至少包含：

```text
sync=none       group=256
sync=per_group  group=1,16,64,256,512,1024
rotation        sync=per_group group=256, WAL delta > 256 MiB
engine durable  single instrument, production group=256
```

每個正式矩陣至少五次，回報 median throughput 與 worst p99。shared CI 只執行極小 smoke，
不設定 performance threshold。

外部 `strace` 驗收重點是 syscall shape：

- `write` 應隨 group／touched segment 數成長，而不是隨 command 數成長；
- `openat`／`close` 不再隨 command 或 group 線性成長；
- `fsync` 在 per-group mode 仍約隨 group 數成長；
- replay 與 benchmark lifecycle 產生的少量 syscall 必須在報告中註明，不要求精確為零。

## 10. 錯誤處理與 crash semantics

| Failure point | Runtime outcome | Restart outcome |
| --- | --- | --- |
| encode／size／overflow preflight | 無 I/O，batch failure | WAL 不變 |
| batch write 前 open state invalid | batch failure、shard FAILED | 依既有 WAL recovery |
| partial batch-data write | batch failure、shard FAILED | last partial frame 截斷；valid prefix replay |
| rotation 舊 segment sync failure | batch failure、shard FAILED | 只接受 recovery 驗證出的完整 records |
| new header write／file sync／directory sync failure | batch failure、shard FAILED | 不隱藏 incomplete segment；由既有 validation fail-stop |
| final group sync failure | 不 apply、不 success、shard FAILED | valid durable WAL prefix決定結果 |

錯誤仍使用現有 value-based `Result<T>`／`ErrorCode::wal_failure`。不以 exception 表達預期
I/O failure；allocation failure 或其他 unexpected exception 由既有 shard fatal boundary
處理。錯誤文字只供診斷，不建立新的 public machine-readable error taxonomy。

## 11. 測試策略

### 11.1 Persistence tests

在既有 `tests/integration/persistence_test.cpp` 增加最小案例：

1. 同 segment batch append：sync 前 durable position 不前進，sync 後指向 batch 最後一筆；
   reopen／replay bytes 與 commands 全部一致。
2. batch 跨 segment：沒有 record 跨 segment，所有 records 按 EngineSeq replay，最後位置與
   segment count 正確。
3. single append 與 batch append 交錯：`next_after()`、`bytes_after()`、retention prefix index
   與 replay 保持正確。
4. reopen existing WAL 後 batch append：寫入 active segment 或正確 rotation，不覆寫既有 bytes。
5. empty batch：回傳 error且 WAL size、last sequence、durable position 不變。

既有 rotation、partial tail、middle corruption、checksum 與 retention tests 必須全部通過。
不為觀察 descriptor 新增 production getter 或 test hook；descriptor lifecycle 由 sanitizer、
reopen tests 與外部 syscall trace驗證。

### 11.2 Runtime tests

既有 group commit ordering 與 durable single-instrument tests 必須通過。必要時只補一個 runtime
integration assertion，證明整組 completion 仍在 durable sync 後依 request order 出現；不重複
persistence cases。

### 11.3 Build matrix

- Linux GCC Debug／Release；
- Linux Clang（CI 既有範圍）；
- macOS Apple Clang（CI 既有範圍）；
- ASan + UBSan + leak detection；
- `git diff --check`；
- WAL ceiling per-group／append-only smoke；
- `--workload=all` smoke。

## 12. 驗收條件

功能正確性是硬性條件：

- on-disk format bytes 不變；
- 所有既有與新增 tests 通過；
- durable position 只在成功 sync 後前進；
- batch 內與跨 segment 的 replay order、checksum、EngineSeq 正確；
- I/O failure 不造成 live apply、publisher notification 或 success completion；
- benchmark final reopen／replay 顯示 `replay_verified=true`。

效能驗收不設定跨機器固定 RPS gate，但必須提供：

- 同環境、同 compiler、同 filesystem 的 before/after 五次結果；
- append-only 與 per-group 的 median throughput、p50／p99／p99.9；
- syscall count before/after，證明 open／close 不再逐筆、write 已按 group 合併；
- segment rotation 的 throughput 與 tail latency；
- 若沒有改善，保存證據並停止繼續堆疊其他優化，先重新 profile。

一百萬 commands/s 是後續方向，不是本切片的完成條件。這避免在缺乏 CPU、device 與新 hot
path 證據時，同時加入 codec、CRC、allocator 或 async I/O 變更。

## 13. 必要檔案修改

| 檔案 | 必要修改 | 預估修改量 |
| --- | --- | ---: |
| `src/persistence/wal.hpp` | batch API、destructor／active descriptor state、private helpers | 15～30 行 |
| `src/persistence/wal.cpp` | descriptor lifecycle、shared preparation、batch write／rotation | 120～190 行 |
| `src/runtime/shard_runtime.cpp` | accepted group 改用 batch append | 5～15 行 |
| `benchmarks/order_book_benchmark.cpp` | 每組建立 commands 並呼叫 batch API | 15～35 行 |
| `tests/integration/persistence_test.cpp` | batch、rotation、reopen、empty/interoperability tests | 90～150 行 |
| `docs/order-book-design.md` | group write 行為與實作調整紀錄 | 8～20 行 |

預估總修改量約 250～440 行，主要來自 storage implementation 與必要 persistence coverage。
若可重用現有 helper，應取區間下緣；不得為追求行數而複製 framing／rotation 邏輯。

預期不修改：

- `include/order_books/*`；
- binary codec／CRC source；
- Snapshot、publisher、cursor source；
- CMake／Conan dependency；
- WAL format fixture；
- runtime configuration 與 README CLI。

若實作必須修改上述預期不修改項目，應先記錄原設計假設、實際阻礙、最小調整與影響，再
重新 review；不得順手擴大優化範圍。

## 14. 關鍵取捨

| 決策 | 選擇 | 理由與代價 |
| --- | --- | --- |
| Descriptor | active segment 常駐 | 消除已證實 open/close 成本；增加一個受 mutex 保護的資源生命週期 |
| Batch API | internal span + last position | 滿足 runtime 所需且不配置 position vector；caller 必須保證 span lifetime涵蓋 call |
| Write aggregation | contiguous buffer | 重用 `write_all`、partial-write 語意清楚；多一次 memory copy |
| Rotation | batch 依 segment 切分 | 保持 record 不跨 segment與現有 crash format；跨 segment 不能只有一次 write |
| Sync | 保留每 group `fsync` | durability 不變；本階段不處理約 57% 的 sync-path 時間 |
| Compatibility | 保留 `append()` | 避免不必要 caller churn；必須和 batch 共用 implementation |
| Performance gate | 報告而非 CI threshold | shared runner 不穩定；correctness仍是硬 gate |

## 15. 已知限制與後續方向

- batch preparation 仍為每筆 encode、CRC 與 command copy；本設計只減少 descriptor 與 write
  syscall 成本。
- contiguous aggregation 會增加 memory copy；若新 profile 顯示為主要成本，再評估
  `writev` 或 encoder 直接寫入 batch buffer。
- `records_` 仍保存 decoded command，長時間 WAL 的 RSS 問題不在本階段處理。
- single writer 架構不會因增加 CPU threads 自動擴展；若 WAL path 已低於 1 microsecond／command
  仍未達標，再分析 shard/partition 或 pipeline，而不是在本設計預建多 writer。
- SATA SSD、filesystem 與 sync latency會影響 durable ceiling；正式部署裝置必須重測。
- fault-injection／subprocess crash matrix 仍是既有已知缺口，本設計不宣稱補齊完整 crash DoD。

後續候選只能依新證據排序：codec／CRC、allocation與 copy、`fdatasync`、publisher/cursor、
或 storage device。不得因本文件列出候選就視為已核准實作。

## 16. 設計與實作一致性檢查表

實作者完成後逐項確認：

- [ ] production Shard writer 每個 accepted group 只呼叫一次 batch append與一次 sync。
- [ ] common case 每 group 每 segment 只有一次 batch-data write。
- [ ] active descriptor 不在每筆或每組重新 open／close。
- [ ] `append()` 與 `append_batch()` 沒有重複 framing／rotation implementation。
- [ ] WAL bytes、record version、CRC coverage與 segment header完全不變。
- [ ] batch crossing rotation 不拆分單一 frame。
- [ ] metadata 只在相應 data write完整成功後更新。
- [ ] sync failure前沒有 state apply、publisher notify或 success completion。
- [ ] recovery仍截斷唯一允許的 last-segment partial tail。
- [ ] benchmark使用 batch production path，計時與輸出語意未改。
- [ ] 沒有新增 CLI、runtime config、thread、dependency或 public Engine API。
- [ ] tests、sanitizers、smoke、replay verification與 working-tree whitespace check通過。
- [ ] before/after benchmark與 syscall count寫入獨立報告，不覆寫 baseline。

若任何項目無法成立，必須在實作調整紀錄中寫明原設計、實際問題、採取調整及其取捨後，
再進行 review。
