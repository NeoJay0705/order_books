# WAL Write Ceiling Benchmark 設計審查與必要修改

## 1. 文件目的

本文件審查目前的 WAL benchmark 與「在追求單一交易對每秒一百萬筆 durable
command 前，先確定 WAL 寫入上限」的需求，並定義實作所需的最小修改。

本階段只建立量測與診斷能力，不優化 production WAL，不改變 durability、WAL
格式或 Engine 行為。實作完成後，必須先取得可重現數據，才能決定下一階段是否
需要常駐 file descriptor、batch write、調整 group size、改用 `fdatasync` 或更換
儲存裝置。

## 2. 審查結論

現有設計只部分符合需求：

- `durable_group_commit` 已直接呼叫正式 `Wal::append()` 與 `Wal::sync()`，方向正確。
- 它固定每組 256 筆、固定使用 1 MiB segment，且只能隨 `--workload=all` 執行，
  無法量測不同 group size 或指定實際 WAL 裝置。
- 現有單一 sample 只提供整個 group 的時間，無法區分 append path 與 `fsync`。
- 它完成後直接刪除資料，沒有 reopen／replay 驗證，因此不足以作為寫入上限的
  correctness guard。
- `engine_durable_single_instrument` 包含 queue、matching、invariant validation、
  completion 與 publisher 競爭，不能用來推論 WAL 本身上限。

因此，必要修改是以既有 `durable_group_commit` 為基礎，收斂成一個可獨立選取、
可設定 group size 與 sync mode 的 `wal_write_ceiling` workload。舊的固定 workload
邏輯由新 runner 取代，不保留兩套重複實作。

此方案符合需求且沒有超出範圍：它只增加 benchmark-local 程式、文件與 CI smoke，
不修改 `include/`、`src/`、WAL 磁碟格式、matching、runtime 或第三方依賴。

## 3. 需求理解與合理假設

### 3.1 本階段回答的問題

1. 目前 `Wal::append()` 在不執行顯式 group sync 時的上限是多少？
2. 目前 `Wal::append()` 加每組一次 `Wal::sync()` 的 durable 上限是多少？
3. group size 256 時，距離每秒一百萬筆 command 還差多少？
4. 主要限制較接近 append CPU／syscall、`fsync`、segment rotation，還是資料量成長？

### 3.2 「一百萬」的定義

正式目標是單一 shard、單一 WAL writer 每秒完成一百萬筆 durable command。只有該
command 所屬 group 的 `Wal::sync()` 成功後，才算 durable 完成。單純 enqueue、
`Wal::append()` 返回或資料進入 page cache 都不算達成 durable 目標。

`sync_mode=none` 只會停用 benchmark runner 的顯式 group sync，用來隔離 append path
上限；`Wal::append()` 在 segment rotation 時既有的 segment／directory sync 仍會執行並
計入 append 時間。輸出必須標記 `completion_boundary=append_return`，不得稱為 durable
throughput。

### 3.3 固定 workload record

沿用 benchmark 既有 `make_committed(sequence)`，產生單一 instrument 的固定
`NewOrder` `CommittedCommand`。所有 record 只有 identity、OrderId、EngineSeq 與
timestamp 隨 sequence 遞增，使序列化大小與內容穩定，同時走正式 binary codec 與
CRC32C。

這是 WAL 寫入上限 workload，不模擬 order book state。其他 command type 的 payload
大小差異可在未來有明確需求時另測，本階段不建立 payload matrix。

### 3.4 目標容量換算

目前實測 WAL frame 約為每筆 122 bytes；實作不得硬編碼此數字，而應由 measured WAL
byte delta 除以 commands 得到 `average_wal_bytes_per_command`。若 measured phase 發生
rotation，這個值會正確包含新 segment header，不得誤稱為純 record frame size。以
122 bytes 估算，一百萬筆每秒約需要 122 MB/s（約 116 MiB/s）的 WAL bytes。

group size 256 時，一百萬筆每秒代表約 3,906.25 次 group sync/s，單一 writer 每組的
append 加 sync 平均必須在約 256 microseconds 內完成。這些是結果判讀基準，不是 CI
門檻。

## 4. 範圍與非目標

### 4.1 納入範圍

- 正式 `Wal::append()` 的 encode、CRC32C、record index 更新及檔案寫入。
- 正式 `Wal::sync()` 的 durability boundary。
- production 預設的 256 MiB segment size。
- 無顯式 group sync 與 per-group sync 兩種診斷模式；兩者都保留 production-internal
  rotation sync。
- 可設定 group size，以相同實作量測 1、16、64、256、512、1024 等情境。
- group append、sync、group total latency 與整體 throughput。
- segment rotation 計數、WAL bytes 與測試後 reopen／replay 驗證。
- 指定實際 WAL device 上的安全資料目錄。

### 4.2 明確不納入

- 修改 `Wal`、`FileOps`、WAL codec 或磁碟格式。
- 常駐 file descriptor、`writev`／batch write、`fdatasync` 或 async I/O。
- Engine queue、matching、invariant validation、completion 或 publisher。
- 多 writer、多 shard 或多 instrument scaling。
- 在 benchmark 內整合 `perf`、`strace`、`iostat` 或平台專屬硬體探測 library。
- 固定 RPS regression gate 或宣稱跨硬體可比較的 SLA。
- 清除 OS page cache、變更 filesystem mount option 或執行其他需要 root 的調校。

上述排除項目不是否定未來優化，而是避免在基線數據前改變被測系統。

## 5. 架構與資料流

workload 保持單執行緒、單 `Wal` instance，與目前 shard writer 的 WAL 呼叫順序一致：

```text
make_committed(sequence)
    -> Wal::append() × group_size
       -> production encode + CRC32C
       -> production open/write/close
    -> sync_mode == per_group ? Wal::sync() : no explicit group sync
    -> record group timing sample
```

phase 流程：

```text
prepare empty directory
    -> Wal::open(segment_size = 256 MiB)
    -> warmup groups
    -> Wal::sync() outside measured interval
    -> capture starting WAL bytes
    -> measured groups
    -> capture ending WAL bytes and segment count
    -> append-only mode: Wal::sync() outside measured interval
    -> destroy Wal
    -> reopen + replay outside measured interval
    -> validate count and contiguous EngineSeq
    -> print result only after validation succeeds
```

warmup 後的 untimed `sync()` 必須存在，使 measured phase 從明確 durable boundary 開始。
append-only mode 的結尾 sync 只為後續 durability／replay 驗證，不得計入 append-only
throughput。segment rotation 由 `Wal::append()` 內部觸發的 sync 則是正式 append path
的一部分，不得關閉或從時間中扣除。

## 6. CLI 與執行語意

擴充既有 executable，不新增 target：

```text
--workload=all|engine_durable_single_instrument|wal_write_ceiling
--wal-group-size=N
--wal-sync=none|per_group
```

- `--wal-group-size` 預設為 256，必須大於零。
- `--wal-sync` 預設為 `per_group`。
- `--iterations=N` 沿用既有語意，在此 workload 表示 measured group 數。
- `--warmup=N` 表示 warmup group 數，允許零。
- measured command 數為 `iterations * wal_group_size`；乘法溢位必須在開始寫入前拒絕。
- `--data-dir=PATH` 指定目標裝置上的空目錄；只在單獨選取
  `wal_write_ceiling` 時供此 workload 使用。
- `--workload=all` 維持現有 smoke 行為，WAL workload 使用自己建立並清理的唯一
  temporary directory，避免與 Engine workload 的 `--data-dir` 產生 ownership 衝突。
- 使用者指定的目錄若存在且非空，必須拒絕；benchmark 不得刪除明確指定的目錄。
- benchmark 只能清理由自己建立、帶 run id 的 temporary directory。

不增加 `--duration`：既有 iterations 已能精確控制資料量，外部腳本可選擇足以跨
segment 或維持預期時間的 group 數。加入第二套終止條件只會增加語意與驗證複雜度。

## 7. 監測資料模型

benchmark-local 結果結構只需要保存：

```text
WalGroupSample
├── append_ns
├── sync_ns            # append-only mode 不產生 sync sample
└── total_ns

WalRunSummary
├── commands
├── groups
├── elapsed_ns
├── wal_bytes_delta
├── starting_segment_count
├── ending_segment_count
├── append_samples
├── sync_samples
└── total_samples
```

每筆 command 周圍不得各呼叫兩次 clock；在百萬級 workload 中這會顯著污染被測路徑。
每組只量測 append phase、sync phase與 group total。因此輸出的是 group append latency，
不是單筆 append latency；可另外由 append group time 除以 group size得到平均值，但不得
把平均值標示為單筆 p99。

## 8. 必要輸出

成功結果使用單行 machine-readable `key=value` 格式，至少包含：

```text
wal_write_ceiling
sync_mode=<none|per_group>
completion_boundary=<append_return|group_fsync>
group_size=<N>
groups=<N>
commands=<N>
commands_per_second=<value>
target_commands_per_second=1000000
target_attainment_percent=<value>
wal_mib_per_second=<value>
average_wal_bytes_per_command=<value>
append_group_p50_us=<value>
append_group_p99_us=<value>
append_group_p99.9_us=<value>
append_group_max_us=<value>
sync_samples=<N>
sync_p50_us=<value-or-na>
sync_p99_us=<value-or-na>
sync_p99.9_us=<value-or-na>
sync_max_us=<value-or-na>
group_total_p50_us=<value>
group_total_p99_us=<value>
group_total_p99.9_us=<value>
group_total_max_us=<value>
elapsed_ms=<value>
wal_bytes_delta=<value>
wal_bytes=<value>
segment_size_bytes=268435456
segment_count=<N>
measured_segment_rotations=<N>
wal_path=<resolved path>
replay_verified=true
```

`commands_per_second` 使用整個 measured phase 的 wall-clock elapsed 計算，不使用每組
sample 加總，以包含 loop 與 command construction overhead。`wal_mib_per_second` 與
`average_wal_bytes_per_command` 使用 measured phase 的 WAL byte delta，不以假設 record
size 推算；發生 rotation 時會包含 segment header。`measured_segment_rotations` 由 measured
phase 前後的 segment file count 差取得。

append-only mode 的 sync percentile 輸出 `na` 並明確輸出 `sync_samples=0`，不得用零
偽裝成零延遲。Header 繼續沿用現有 platform、compiler、build type、CPU threads 與
seed metadata。

## 9. 錯誤處理與正確性驗證

以下任一情況必須輸出 workload、phase 與 machine-readable error code，並以 non-zero
結束；不得先輸出有效-looking throughput summary：

- CLI 值無效、group size 為零或 command count 溢位。
- data directory 不存在且無法建立、不是目錄或不是空目錄。
- `Wal::open()`、任一 `append()` 或 `sync()` 失敗。
- filesystem 查詢 WAL bytes 或 segment count 失敗。
- measured commands／groups 不符合要求。
- 最後的 sync、reopen 或 replay 失敗。
- replay record 數不等於 warmup 加 measured commands。
- replay 的 `EngineSeq` 不是從 1 開始嚴格連續，或最後值不符合預期。
- replay record 的 instrument／command type 與 fixture 不符。

replay 在計時區間之外，目的只是在效能數字發布前證明資料可由正式 reader 讀回。
它不屬於寫入 throughput。

## 10. 必要檔案修改

### `benchmarks/order_book_benchmark.cpp`

- 增加 `wal_write_ceiling` workload selection。
- 在 `BenchmarkOptions` 加入 benchmark-local group size 與 sync mode。
- 新增嚴格 CLI parsing 與 workload-specific validation。
- 將現有固定 `durable_group_commit` block 重構為一個 WAL ceiling runner；不保留第二套
  append／sync loop。
- 使用 production 256 MiB segment size。
- 分別量測 group append、顯式 group sync、total 以及整體 phase elapsed；不得把
  append 內部的 rotation sync 誤歸類成顯式 group sync sample。
- 安全處理 sequence／command count／byte arithmetic overflow。
- 執行 untimed final sync、reopen、replay 與內容驗證後才輸出結果。
- 沿用現有 percentile 規則、`make_committed()`、run id 與安全資料目錄 helper；必要時
  將只帶 Engine 名稱的錯誤 helper改為 workload-neutral 命名，不建立通用 framework。

### `docs/order-book-design.md`

- 將固定 `durable_group_commit` 說明更新為 `wal_write_ceiling`。
- 明確記錄 append-only 與 per-group durability boundary。
- 說明 WAL ceiling 與 Engine end-to-end throughput 不得互相替代。

### `README.md`

- 增加 Release build 下的 WAL ceiling 執行範例。
- 提醒正式 baseline 必須使用 Linux 部署磁碟上的新空目錄。
- 說明 `none` 只量 append return，`per_group` 才是 durable throughput。

### `.github/workflows/ci.yml`

- 讓既有 Release benchmark smoke 以極小資料量覆蓋預設 per-group runner。
- 另以極小資料量執行一次 append-only mode，驗證 parser、輸出與 final replay。
- 不加入效能 threshold，也不增加 CI matrix。

### `tests/integration/wal_write_ceiling_test.cpp`（必要調整）

原始設計為避免重複既有 persistence coverage，沒有新增 tests source；實作需求明確
要求「包含測試檔案以可以編譯」，因此增加一個最小 integration contract test。它只驗證
WAL ceiling fixture 所依賴的固定 `CommittedCommand`：append 多筆、sync、reopen、replay、
sequence 連續、instrument 與 command type 正確。它不量測效能、不測量 benchmark output、
不加入 production test hook，也不取代既有 persistence tests。

這是範圍內最小的設計調整；影響是 tests target 多一個 deterministic round-trip case，
沒有 production API 或 runtime 行為變更。

### 不修改的檔案

- `include/order_books/*`
- `src/persistence/*`
- `src/runtime/*`
- `src/domain/*`
- `benchmarks/CMakeLists.txt`
- Conan／CMake dependency 設定

除上述新增的 `tests/integration/wal_write_ceiling_test.cpp` 與其 target source list 外，
既有 tests source 不修改。

若實作發現必須修改上述 production 或 build 檔案，代表設計假設不成立，應先記錄
原因並重新 review，不得直接擴張範圍。

## 11. 測試策略

### 11.1 自動化 smoke

至少驗證：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=wal_write_ceiling \
  --iterations=2 --warmup=1 \
  --wal-group-size=4 --wal-sync=per_group

./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=wal_write_ceiling \
  --iterations=2 --warmup=1 \
  --wal-group-size=4 --wal-sync=none
```

兩者都必須 exit 0、`commands=8`、`replay_verified=true`；per-group 的 sync samples
為 2，append-only 為 0。Smoke 只驗證 harness，不判斷效能。

另外執行 Debug、Release、ASan/UBSan tests 與 `git diff --check`，確認 benchmark
變更沒有破壞既有功能。Sanitizer 數字不作效能結果。

### 11.2 Linux 實機矩陣

在實際 WAL filesystem 上，以新的空目錄分別執行：

```text
sync_mode=none       group_size=256
sync_mode=per_group  group_size=1
sync_mode=per_group  group_size=16
sync_mode=per_group  group_size=64
sync_mode=per_group  group_size=256
sync_mode=per_group  group_size=512
sync_mode=per_group  group_size=1024
```

512／1024 只用於判斷較大 batch 的空間，不改變 production 預設。每組至少五次，回報
commands/s 中位數與最差 p99；每次使用新的目錄。至少一組正式 run 的寫入量必須超過
256 MiB，以包含一次 segment rotation。不得把 tmpfs、container overlay filesystem 或
macOS 開發機結果作為 Linux production baseline。

### 11.3 外部診斷

外部工具不整合進程式，但報告應保存下列獨立 run：

```bash
perf stat -e cycles,instructions,context-switches,cpu-migrations,page-faults -- <command>
strace -f -c -e trace=openat,write,close,fsync -- <command>
iostat -xz 1
```

`perf`／`strace` instrumentation 會改變時間，不得把該次 throughput 與未插樁 baseline
混用。報告另外記錄 OS、kernel、CPU、磁碟型號、filesystem、compiler、build type 與
benchmark commit。

## 12. 結果判讀

```text
append-only < 1,000,000 commands/s
  -> 瓶頸已存在於 encode／CRC／record index／每筆 open-write-close／rotation 等
     append path

append-only >= 1,000,000 commands/s
但 per-group(256) < 1,000,000 commands/s
  -> group sync latency／儲存裝置或 group size 是主要限制候選

per-group(256) >= 1,000,000 commands/s
但 Engine durable < 1,000,000 commands/s
  -> 瓶頸位於 WAL 以外的 Engine path
```

以上是分類規則，不是自動根因宣告。只有同一次 source、同一硬體、同一資料裝置與相同
run 條件的結果可直接計算差異百分比。不得用 macOS 或不同 CPU pinning 的結果宣稱
Linux production uplift。

## 13. 關鍵決策與取捨

| 決策 | 選擇 | 理由 |
| --- | --- | --- |
| Benchmark target | 沿用 `order_books_benchmark` | 避免新 target 與依賴 |
| Production seam | 只呼叫正式既有 `Wal` API | 量到真實實作，不加入 test-only hook |
| 終止條件 | 沿用 iterations／warmup | 可精確控制 groups 與資料量，不新增 duration 語意 |
| Latency sample | 每 group 分段計時 | 避免每筆 clock call 污染百萬級 hot path |
| Segment size | 256 MiB | 與 production 預設一致 |
| Correctness | untimed reopen／replay | 不把無法復原的快速寫入當成有效結果 |
| Append-only | final untimed sync | 保持 timed boundary 純粹，同時能驗證持久資料 |
| 大 group | 僅作診斷參數 | 不在量測階段改 production 設定 |
| CI | smoke only | shared runner 不適合效能 gate |

## 14. 已知限制與後續方向

- 結果是單 writer 上限，不代表多 shard process 總吞吐。
- 固定 `NewOrder` record 不代表所有 payload size；輸出實際 bytes 供正確解讀。
- buffered I/O 加 `fsync` 是目前 production 語意；即使 `sync_mode=none`，segment
  rotation 仍有內部 sync。本 workload 不模擬 direct I/O。
- `Wal` 目前在記憶體保存 cached records，長測的 RSS 成長屬真實成本，但 benchmark
  本身不修改 retention 或 index policy。
- reopen／replay 會需要額外記憶體且不在 timed region；極長 run 應先確保主機資源，
  不得因驗證成本而省略 correctness check。
- 實際 syscall 與 block-device wait attribution 依賴 Linux 外部工具，程式內數據只能
  說明哪個 phase 變慢，不能單獨證明 kernel 或硬體根因。

數據若證明 append path 是主因，下一份設計才評估常駐 descriptor 與 batch write；若
證明 sync 是主因，才評估 group size、`fdatasync` 與部署儲存裝置。兩者不得在本階段
預先實作。

## 15. 預估必要修改量

不含本設計文件，預估約 290～430 行：

```text
benchmarks/order_book_benchmark.cpp  190～285 行
docs/order-book-design.md              20～35 行
README.md                              15～25 行
.github/workflows/ci.yml                2～5 行
tests/integration/wal_write_ceiling_test.cpp  45～70 行
tests/CMakeLists.txt                     1 行
```

主要程式量來自兩種 timing boundary、overflow／failure handling、資料目錄 ownership、
統計輸出與 reopen／replay 驗證；沒有 production code 或新抽象。

## 16. 設計與實作一致性規則

實作者應以本文件為 WAL ceiling workload 的依據，並維持以下邊界：

- benchmark 只量正式 `Wal` API，不複製或簡化 WAL 寫入邏輯。
- throughput summary 只能在所有寫入、final sync 與 replay 驗證成功後輸出。
- append-only 與 durable 數字必須以 completion boundary 清楚區分。
- 不因量測需求改變 production WAL 或 Engine。
- 不把外部 profiler run 與正常 baseline 混為同一結果。

若實作遇到無法依現有 API取得必要數據，先記錄原始設計、實際問題、最小調整與影響，
重新 review 後再修改本文件；不得悄悄加入 production instrumentation seam。

## 17. Definition of Done

- 可單獨選取 `wal_write_ceiling`。
- 可設定正值 group size，以及 `none`／`per_group` sync mode。
- 使用單一 writer、正式 codec、CRC、WAL append 與 sync。
- 使用 256 MiB segment，能在足量 run 中觀察 rotation。
- warmup 不進入 measured throughput 或 latency samples。
- append、sync、group total 與整體 throughput 邊界正確。
- 輸出實際 WAL bytes、MiB/s、每 command WAL bytes、segment count 與目標達成率。
- 所有錯誤都使程序 non-zero，且失敗 run 不輸出 throughput summary。
- 明確指定的非空目錄不會被清除或覆寫。
- 測試後 reopen／replay 驗證所有 record 與連續 sequence。
- CI smoke 覆蓋兩種 sync mode，但沒有 performance gate。
- `WalWriteCeilingTest` 編譯並驗證固定 fixture 的 durable round-trip。
- production source、public API、WAL format 與依賴均未變更。
