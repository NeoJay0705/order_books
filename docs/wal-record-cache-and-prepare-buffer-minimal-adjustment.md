# WAL record cache／prepare buffer 最小必要調整

## 1. 結論

依 `docs/wal-prepare-rotation-minimal-experiment-benchmark-report.md`，目前只應先修改
WAL in-memory record cache 的成長方式，不應同時重構 prepare representation。

本案分成兩個有順序的調整：

1. **現在實作：**把 `records_` 從會整體搬移的單一 `std::vector` 改為分段配置的
   `std::deque`，並在任何 WAL I/O 前完成本批 cache record 的配置與暫存。
2. **條件式後續：**只有第一項重測後，正常 append latency／service throughput 仍不足時，
   才把每筆獨立 frame vector 改成每條 prepare lane 的 contiguous buffer。

兩項不得放在同一個 performance patch。否則 throughput 或 tail 改善時，無法判斷收益來自
record-cache growth、frame allocation，還是兩者交互作用。

本文件不調整 WAL format、CRC、durability boundary、group size、fsync、segment size、
prepare worker 數、Publisher、Completion 或 public API。

## 2. 為何第一項已成為必要調整

W=2、batch=8192 的單輪 no-rotation 結果為：

- service throughput `1.03754M commands/s`；
- append p50 `5,761.093 us`，等效約 `1.422M commands/s`；
- append p99.9 `676,690.794 us`，max `1,483,040 us`；
- measured window 累積 `16,777,216` 筆 record；
- fsync、rotation、iowait 與 device saturation 均未出現。

現有 `records_` 使用 `std::vector<CachedRecord>`，容量不足時採約 1.5 倍 geometric growth。
後期擴容會搬移數百萬筆既有 `CachedRecord`。這與「大部分 group 約 5--7 ms，但少數 group
停頓 0.7--1.5 秒」具有直接的結構性對應。

先前設計拒絕更換 container，是因為當時只有不穩定的 plan/copy 數據；本次長 epoch 已實際跨越
多次大型 capacity growth，且物理磁碟、fsync、rotation 已被排除，因此可以進行封閉的最小修正。
這仍不是已完成的因果證明；最終保留與否由第 6 節的前後比較決定。

## 3. 第一項：record cache 分段成長

### 3.1 資料結構

修改 `src/persistence/wal.hpp`：

```cpp
#include <deque>

std::deque<CachedRecord> records_;
```

移除 `append_prepared_unlocked()` 中只屬於 vector 的 `capacity()`／`reserve()` geometric-growth
區塊。保留既有的 overflow 檢查：

```cpp
if (records.size() > records_.max_size() - records_.size()) {
  return wal_error(ErrorCode::wal_failure, "WAL record count overflow");
}
```

`std::deque` 仍提供 random-access iterator，因此既有 `std::upper_bound()`、
`std::lower_bound()`、range iteration、`front()`、`back()` 與 prefix `erase()` 的語意可保持。
不得為這次修改改成 linear lookup、額外 map 或 offset-only cache。

### 3.2 不得直接在 write 成功後配置

單純把型別換成 `std::deque` 並保留下列流程是不正確的：

```text
write_all() succeeds
  -> records_.push_back() may allocate
```

這可能在 WAL bytes 已寫入後才因 allocation failure 中斷，破壞現有「配置與驗證先於 I/O」的
失敗邊界。

`append_prepared_unlocked()` 必須依下列順序執行：

```text
prepare frames
  -> build CachedRecord metadata
  -> plan chunks and build chunk byte buffers
  -> stage every CachedRecord into records_       # may allocate; no I/O yet
  -> for each chunk:
       optional rotation durability work
       write_all(chunk bytes)
       mark that chunk's staged records published
       update positions / byte counters / dirty state
  -> release rollback guard
```

### 3.3 Staged-record rollback

在 staging 前保存：

```cpp
const auto original_record_count = records_.size();
std::size_t published_record_count = 0;
```

把本批 `cached_records` 依原順序 move 到 `records_`。若 staging 過程拋出例外，必須先：

```cpp
records_.resize(original_record_count);
```

再重新拋出；此時尚未執行任何 WAL I/O。

staging 完成後使用 scope guard。只要函式尚未成功完成，guard 都將 container 縮回：

```text
original_record_count + published_record_count
```

每個 chunk 的 `write_all()` 完整成功後，才把該 chunk 的 `record_count` 加入
`published_record_count`，並更新既有 position／bytes／dirty state。如果 rotation sync、segment
creation 或 data write 失敗：

- 先前已成功寫入 chunk 的 records 保留；
- 當前與後續尚未成功寫入的 staged records 移除；
- 既有 fail-stop 與 restart valid-prefix 語意不變。

WAL mutex 在整個 append 期間保持持有，因此暫存但尚未 publish 的 records 不會被
`next_after()`、`bytes_after()`、replay 或 retention 觀察到。

不得改成在 I/O 前更新 `last_appended_position_`、`active_bytes_`、`size_bytes_` 或
`active_dirty_`。

### 3.4 Replay 與 retention

`replay()` 目前以 local `std::vector<CachedRecord>` 建立 cache，最後 move-assign 到
`records_`。最小修改是把該 local container 同樣改成 `std::deque<CachedRecord>`；decode、CRC、
sequence validation 與 durable-position 語意不變。

`retain_through()` 保留現有 prefix erase 與 `rebuild_record_index_unlocked()`。deque prefix erase
不需搬移全部後綴元素，但 rebuild 仍為 O(N)；本案不順帶重設 cumulative index，也不建立
segment-aligned index。

### 3.5 Profile 語意

staging records 的時間仍累加到既有 `publish_ns`，即使它在實際時間順序上移到 write 前。
`publish_ns` 的語意維持「建立可供 WAL reader／publisher 使用的 in-memory cache」，不新增
production metric 或新 profile 欄位。

這表示修改前後 `publish_ns` 可比較 cache insertion 成本，但不能用它推導 I/O 發生順序；
durability 與可見性仍由 write 成功及 position 更新決定。

## 4. 第一項的修改範圍

預期只需要修改：

| 檔案 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| `src/persistence/wal.hpp` | include、`records_` 型別 | 2--4 |
| `src/persistence/wal.cpp` | replay local container、移除 reserve、staging／rollback | 35--60 |
| `tests/integration/persistence_test.cpp` | partial failure／retention 回歸測試（若既有 seam 足夠則擴充既有案例） | 20--45 |

不修改 benchmark CLI、RuntimeConfig、`FileOps`、binary codec 或 installed headers。

測試不得斷言 STL deque 的 block size、iterator implementation 或配置次數；只驗證 observable WAL
語意。效能效果由 benchmark 驗證，不放進 CTest 固定 RPS gate。

## 5. 第一項必要測試

至少通過：

1. single append 與 batch append 產生相同 WAL bytes／replay command；
2. no-rotation 與跨 segment rotation 的 EngineSeq、position、durable boundary 不變；
3. `next_after()`／`bytes_after()` 在 replay 前後結果不變；
4. `retain_through()` 後查詢、再 append、再 replay 均正確；
5. 若既有 fault-injection seam 可讓後段 chunk write 失敗，驗證只保留先前成功 chunk 的 cache；
   若目前沒有可靠 seam，不為本案新增通用 I/O mocking framework，但必須保留 scope guard 的直接
   unit coverage或以最小 test-only seam 驗證 rollback；
6. Debug、Release 與 ASan/UBSan CTest 通過；若現有 TSan 環境不可用，明確記錄，不偽報通過。

WAL format identity 必須以相同 deterministic commands 比較 baseline／candidate segment SHA-256；
container 變更不得改變任何 on-disk byte。

## 6. 第一項的最小效能驗證與保留條件

沿用目前的單一 component case，不擴大成 Engine／Publisher／Completion campaign：

```text
workload=wal_write_ceiling
batch=8192
W=2
sync=none
segment_size=4 GiB
zero rotation
16,777,216 measured commands
phase_profile=off
```

在同一個 idle gate、CPU affinity、filesystem 與 build flags 下，各執行 baseline 與 candidate。
第一輪只做一對；若方向不一致或環境觀測不穩定，結果標記 inconclusive，而不是反覆挑快輪。

保留第一項修改必須同時符合：

- replay、byte plan、zero rotation、zero sync 與 WAL SHA-256 identity 全部通過；
- candidate service throughput 不得退化超過 3%；
- candidate p99.9 與 max 不得比 baseline 惡化；
- 至少一項大型-tail指標有實質改善：p99.9 或 max 降低 50%以上；
- device util／iowait 仍低，沒有把差異改由外部 I/O 解釋；
- 沒有 sanitizer error、uncaught exception 或 record-cache rollback mismatch。

單輪不能宣稱正式 production ceiling；它只決定 container 修改是否值得保留並進入後續多輪確認。
若 tail 沒有改善，撤回 container 修改，不進入第二項，也不再以 vector growth 作主根因。

## 7. 第二項：lane-contiguous prepare buffer（條件式）

### 7.1 啟動條件

只有第一項通過第 6 節，而且重測後仍符合下列任一條件，才另立 patch 實作第二項：

- service throughput 仍低於 `1.53M commands/s`；或
- append p50 等效 throughput 仍低於 `1.53M commands/s`，且既有 profile 顯示 prepare／frame
  allocation 仍是主要正常路徑成本。

第一項完成前，不修改 `PreparedRecord`、`encode_frame()` 或 prepare worker protocol。

### 7.2 最小 representation

第二項不使用 `writev`，仍保留目前經驗證的 contiguous chunk與 `FileOps::write_all()`。
每條 prepare lane 改為產生：

```cpp
struct PreparedRecord {
  domain::CommittedCommand command;
  std::size_t frame_offset{};
  std::size_t frame_size{};
};

struct PreparedLane {
  std::vector<std::byte> frame_bytes;
  std::vector<PreparedRecord> records;
  WalAppendProfile profile;
};
```

每個 lane 按其 command range 順序，將 frame append 到自己的 `frame_bytes`，metadata 只記 offset／size。
lane merge 仍依原始 range 順序，不允許 completion order 改變 WAL order。

`encode_frame()` 增加 internal append-to-buffer 版本：

1. 在 lane buffer append length placeholder；
2. append record version 與既有 encoded payload；
3. 對完全相同的 body bytes 計算 CRC32C；
4. append CRC，回填 length；
5. 回傳 frame offset／size。

第一版可以保留 `encode_committed_command()` 產生 payload vector；只移除 body vector、frame vector與
每筆 `PreparedRecord::frame` allocation。不得在同一 patch 重寫整個 binary codec。

segment planner 以 `{lane, offset, size}` 取得 frame span，再沿用現有 chunk buffer copy與 rotation
邏輯。這仍有一次 frame-to-chunk copy，但可先消除每 command 多個小 allocation；沒有證據前不加入
scatter/gather、buffer pool、custom allocator或 thread-local lifetime。

### 7.3 第二項驗收

- baseline／candidate WAL segment byte-for-byte 相同；
- sequential、W=2、W=4 prepare 的成功與錯誤順序不變；
- single-segment與跨 segment batch replay一致；
- profile parent／child counters與 prepare task count仍一致；
- 在第一項已固定的 candidate上，service throughput至少再提升5%，或跨過`1.53M commands/s`；
- p99／p99.9、CPU seconds/M與context switches不得穩定惡化超過10%。

若第二項未達上述任一收益條件，撤回它；不得為保留修改而順帶增加 worker、batch或segment size。

## 8. 明確不做

本階段不做：

- `writev`、mmap、direct I/O、io_uring；
- custom allocator或通用 buffer pool；
- WAL format／CRC演算法變更；
- adaptive group size、`max_group_wal_bytes`或dirty-page feedback；
- production segment size、fsync policy或prepare worker預設值調整；
- Publisher／Completion／StateMachine修改；
- 為測試暴露 public record-cache API；
- segment-aligned persistent index重設計。

## 9. 完成定義

第一個實作階段只在以下條件全部成立時完成：

- 只實作第 3--6 節，沒有提前實作第 7 節；
- staged changes未被任何命令改動；
- correctness、sanitizer與WAL byte identity通過；
- 單一 paired benchmark能判斷 tail是否來自record-cache growth；
- 報告清楚區分單輪實驗結果與production ceiling；
- 若結果不支持修改，能完整撤回第一項而不影響其他 WAL功能。

第二項必須在第一項報告完成後另案實作與review，不能視為本階段未完成工作。
