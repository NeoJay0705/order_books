# WAL Append Hot-path Optimization staged changes 必要修正方案

狀態：已實作並驗證
審查基線：`docs/wal-append-hot-path-optimization-design.md`  
審查範圍：建立本文件時的 staged changes  
目的：只修正已確認的效能退化、測試缺口與驗收文件不一致，使設計、實作及壓測結論一致

驗證：Release GoogleTest 41/41、ASan/UBSan 41/41，WAL ceiling／rotation／durable engine
before/after 矩陣各五次，WAL runs 均 `replay_verified=true`。

## 1. 結論與範圍

Active descriptor、batch append、runtime group integration 與 WAL format 相容方向符合設計，
不需要更換架構或增加介面。合併前只需完成四項調整：

1. 修正 `records_` 每個 batch 精確擴容造成的近似 O(n²) 搬移。
2. 補上 single append 與 batch append 混用後的 retention index 驗證。
3. 依設計指定的參數與統計方式重新壓測並更新報告。
4. 修正 staged diff whitespace 與報告中的本機絕對連結。

本次不得順便加入新的 WAL index representation、custom allocator、memory pool、`writev`、
mmap、AIO、`io_uring`、background flush thread、sync policy、publisher／Snapshot／codec／CRC
優化、新 CLI flag、runtime config、dependency 或 performance CI threshold。

Capacity growth 修正不是另做 allocator 優化，而是避免本次 staged implementation 為了
write 前 reserve 而引入的全量搬移退化。WAL format、durability boundary、recovery 與
fail-stop 語意都必須保持不變。

所有修改先保留為 unstaged changes；不得執行 `git add`、`git restore --staged`、`git reset`、
`git commit` 或其他會改變 index 的操作。

## 2. FIX-WA01：修正 `records_` capacity growth

### 問題與必要性

`src/persistence/wal.cpp` 的 `append_prepared_unlocked()` 目前執行：

```cpp
records_.reserve(records_.size() + records.size());
```

Capacity 不足時，這可能讓每個 batch 都重新配置並搬移全部歷史 `CachedRecord`。壓測已觀察到
WAL 增長後 append group p50 約 111.5 ms、長測吞吐約 8.9k commands/s；此成本明顯大於同期
約 1.94 ms 的 sync p50。這是本次變更新增的 hot-path regression，屬必要修正。

### 具體修正

保留既有 record-count overflow 檢查，只有 capacity 不足時才以目前 capacity 加上至少 50%
餘裕的方式成長；所有加法先檢查 `max_size()`。直接在目前 `reserve()` 位置使用局部計算，
不新增 helper 或 growth framework：

```cpp
const auto required_capacity = records_.size() + records.size();
if (required_capacity > records_.capacity()) {
  const auto current_capacity = records_.capacity();
  const auto growth = std::max(records.size(), current_capacity / 2U);
  const auto grown_capacity =
      growth > records_.max_size() - current_capacity
          ? records_.max_size()
          : current_capacity + growth;
  records_.reserve(std::max(required_capacity, grown_capacity));
}
```

實作時必須確認：

- `records.size() > records_.max_size() - records_.size()` 的既有檢查仍在計算前；
- `reserve()` 仍在任何 batch data write 之前，維持 preflight 保證；
- 不移除 `cached_records.reserve()` 或 chunk byte buffer 的 write 前配置；
- 不改 `CachedRecord`、`PreparedRecord`、`records_` 型別或 ownership；
- 不以固定超大容量取代幾何成長。

預估修改量：`src/persistence/wal.cpp` 8～15 行。

## 3. FIX-WA02：補齊 mixed append 的 retention 測試

### 問題與必要性

`WalSingleAndBatchAppendShareIndexes` 已驗證 `next_after()` 與 `bytes_after()`，但沒有執行設計
第 11.1 節要求的 `retain_through()`。Batch path 一次加入多個 `CachedRecord`，retention 會
erase prefix 並重建 cumulative byte 與 continuity index，需要直接驗證兩者交互。

### 具體修正

直接擴充現有 test，不新增內容重複的案例：

1. 保留 single append sequence 1、batch append sequences 2～3、sync 與 replay。
2. 完成現有 assertions 後呼叫：

   ```cpp
   ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->retain_through(1)));
   ```

3. 再呼叫 `bytes_after(1, 3)`，預期只包含第二與第三筆 frame bytes。
4. 再呼叫 `next_after(1, 3)`，預期仍取得 EngineSeq 2。
5. 呼叫 `bytes_after(0, 3)`，預期回傳 `ErrorCode::corrupt_wal`，證明已淘汰 cursor 不會被
   誤認為仍可發布的範圍。

本測試只驗證既有 retention contract，不新增 production getter、descriptor hook 或
fault-injection framework。

預估修改量：`tests/integration/persistence_test.cpp` 10～18 行。

## 4. FIX-WA03：重新建立可比較的壓測證據

### 問題與必要性

目前 benchmark report 與設計有三個明確落差：

- 設計要求 rotation 使用 `group=256`，報告使用 `group=1024`；
- 設計要求回報 worst p99，報告回報五次 p99 的中位數；
- baseline 使用 100 warmup／1,000 measured groups，修改後主要矩陣使用 20／100。對會隨
  WAL 長度變慢的 implementation，兩者不可作正式 before/after 比較。

FIX-WA01 完成前不得先重跑並改寫最終結論，否則仍只會量到已知退化。

### 固定測試矩陣

Baseline 與修正版必須使用相同主機、filesystem、compiler、flags、segment size、fixture、
warmup、measured groups 與 repetitions。每個正式案例執行五次並保留原始輸出。

| Workload | Sync | Group | Warmup | Measured | Repetitions |
| --- | --- | ---: | ---: | ---: | ---: |
| WAL ceiling | `none` | 256 | 100 groups | 1,000 groups | 5 |
| WAL ceiling | `per_group` | 1、16、64、256、512、1024 | 100 groups | 1,000 groups | 各 5 |
| Rotation | `per_group` | 256 | 100 groups | WAL delta > 256 MiB | 5 |
| Durable Engine | production fsync | 256 max | 100 iterations | 1,000 iterations | 5 |

Rotation 可沿用 baseline 的 11,000 measured groups；若 record size 改變，只能調高 measured
groups 以確保 WAL delta 超過 256 MiB，不得改 group size。Baseline 若需重建，應從 base commit
在 `/tmp` 建立獨立 source copy與 executable，不修改 repository index。

### 報告統計方式

每個設定至少回報：

- 五次 throughput 的 median；
- 五次執行中數值最高的 p99，明確標示為 worst p99；
- p50、p99.9、max、WAL MiB/s 與 target attainment；
- WAL bytes delta、segment count、rotation count 與 `replay_verified`；
- durable engine completion latency 與 correctness counters；
- syscall shape，證明 data write 隨 group／touched segment，而非 command 數成長。

Before/after 表格必須使用相同參數。若沒有五次 baseline，不得將既有單次結果標成正式
before median；只能標示為 historical reference，正式比較維持未完成。

若修正後改善，僅依數據回報改善比例；若仍未改善，保存結果並停止擴大優化，重新 profile
後另寫設計。不得因結果不理想而改 group default、sync policy 或 durability boundary。

預估修改量：benchmark report 15～30 行；正常情況不修改 benchmark source。

## 5. FIX-WA04：修正文件一致性

1. 移除 `docs/wal-append-hot-path-optimization-benchmark-report.md` EOF 的多餘空白行，使
   `git diff --cached --check` 不再回報 `new blank line at EOF`。
2. 將報告中的本機絕對連結改為 repository-relative link：

   ```markdown
   [wal.cpp](../src/persistence/wal.cpp)
   ```

3. FIX-WA03 後一次更新測試參數、統計定義與結論，不保留互相衝突的舊結果。Staging 未被
   修改的敘述若保留，必須明確寫成「執行測試時」。

預估修改量：不含壓測數據更新時為 2～4 行。

## 6. 執行順序與驗收

依序執行：

1. FIX-WA01 capacity growth。
2. FIX-WA02 retention coverage。
3. Release tests、ASan／UBSan／leak detection 與 `git diff --check`。
4. FIX-WA03 固定矩陣 benchmark 與 syscall trace。
5. FIX-WA04 完成報告。
6. Review unstaged changes，確認 index 未被操作。

完成條件：

- `records_` 不再每個 batch 精確擴容並搬移全部歷史 records；
- 所有 reserve 與 batch buffers 仍在第一個 data byte write 前完成；
- mixed append 後的 retention、`bytes_after()` 與 `next_after()` assertions 通過；
- 既有及新增 tests、sanitizers 與 leak detection 通過；
- rotation 使用 group 256、WAL delta 超過 256 MiB，且執行五次；
- 報告回報 median throughput 與 worst p99，before/after 參數一致；
- 每次 benchmark 都是 `replay_verified=true`；
- syscall trace 保持 group-level data write，沒有逐 command open／close；
- `git diff --check` 通過且文件沒有本機絕對連結；
- WAL format、Engine API、durability、recovery、publisher 與 completion contract 均未改變；
- staged changes 在修正過程中未被任何命令改寫。

## 7. 修改量估算

| 類別 | 預估修改量 |
| --- | ---: |
| `wal.cpp` capacity growth | 8～15 行 |
| persistence test | 10～18 行 |
| benchmark report與文件清理 | 17～34 行 |
| 合計 | 約 35～65 行 |

不預期修改 `wal.hpp`、`shard_runtime.cpp`、benchmark source、public headers、CMake 或 Conan。
若實作過程證明必須修改這些檔案，應先記錄新的具體阻礙並重新 review，不得順手擴大範圍。
