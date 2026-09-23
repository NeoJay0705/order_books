# WAL prepare／writeback／rotation 最小實驗設計

## 1. Review 結論

目前證據支持先做一個封閉的 benchmark 實驗，不支持直接修改 production WAL、group commit、
durability 或 page-cache policy。實驗只回答三個問題：

1. 現有 bounded prepare lane 從 W=1 調成 W=2／W=4，能否讓 batch=8192 的
   append-return 達到支撐 1M durable commands/s 所需的至少約 1.53M commands/s？
2. 排除 segment rotation 後，append tail 是否仍與 blocking `write(2)`／writeback 同期？
3. 256 MiB segment 的 rotation 是否對 throughput 或 p99/p99.9 有實質影響，值得另案調整
   production segment size？

最小範圍如下：

- 沿用既有 `--wal-prepare-workers=1|2|4` 與
  `--wal-parallel-prepare-min-commands=4096`，不新增 worker implementation。
- 只為 `wal_write_ceiling` benchmark 新增 `--wal-segment-size-bytes=<N>`，讓測試能建立
  256 MiB 與 4 GiB 的 WAL；不加入 public API 或 production runtime config。
- 不實作 `max_group_wal_bytes`。目前固定 workload 約 122 bytes/command，batch=8192 約
  0.95 MiB，command count 已提供足夠的近似 byte bound；尚無 variable-size group 造成問題的證據。
- 不修改 `Wal::append_batch()`、`Wal::sync()`、CRC、format、`FileOps`、StateMachine、Publisher、
  Completion 或 completion boundary。
- 不加入 adaptive batch、dirty-page feedback、`fdatasync`、`writev`、mmap、direct I/O 或 io_uring。

這是診斷實驗，不是 production optimization。只有本文件的判定條件成立後，才另立最小 production
設計；不得在本實驗順帶調整預設 lane、segment size 或 group policy。

## 2. 現況與必要假設

### 2.1 現有執行路徑

目前 writer group 由 command count／delay 關閉，沒有 byte-based boundary：

```text
collect up to max commands or max delay
  -> admission / accepted CommittedCommand vector
  -> Wal::append_batch(accepted)
       -> prepare / serialize / CRC
       -> segment plan + cached-record build + chunk copy
       -> blocking write(2) to page cache
       -> in-memory record publication / terminal position update
  -> Wal::sync() / fsync(2)
  -> durable completion boundary
```

`Wal::append_batch()` 只有 serialize 後才知道精確 frame bytes；若 group 跨越 segment，WAL 會切成
多個 storage chunks，但它們仍屬同一 durable group。rotation 可能額外執行舊 segment sync、header
write、header fsync 與 directory fsync。

### 2.2 已知數據

`docs/state-machine-wal-component-ceiling-benchmark-report.md` 的單輪補充觀測為：

| Batch | Prepare lanes | Append-only service RPS | Durable service RPS | Sync p50 等效 RPS |
| ---: | ---: | ---: | ---: | ---: |
| 4096 | 1 | 808,059 | 545,188 | 約 2.18M |
| 8192 | 1 | 790,973 | 603,148 | 約 2.88M |

串行 append 與 sync 的近似上限為：

```text
R_durable ~= 1 / (1 / R_append + 1 / R_sync_equivalent)
```

在 batch=8192、sync-equivalent 約 2.88M/s 時，要讓 direct durable WAL 達到 1M/s，append
至少需要：

```text
R_append >= 1 / (1 / 1M - 1 / 2.88M) ~= 1.53M commands/s
```

因此「append 超過 1M/s」不是充分條件；本實驗固定以 1.53M/s 作為 worker-only hypothesis 的必要
門檻，不把它宣稱為 production SLA。

### 2.3 Page cache 語意

Kernel background writeback 可能在 application `fsync()` 前送出 dirty pages，也可能在 dirty pressure
下阻塞 `write()`；它不會替 application 呼叫 `fsync()`，也不會推進 WAL durable position。本實驗只觀測
writeback／syscall correlation，不以 host-wide Dirty 值動態改變 batch。

## 3. 唯一必要的 benchmark 程式修改

### 3.1 新增 segment-size CLI

在 `benchmarks/order_book_benchmark.cpp` 的 `BenchmarkOptions` 增加：

```cpp
std::size_t wal_segment_size_bytes{kWalCeilingSegmentSize};
```

新增：

```text
--wal-segment-size-bytes=<positive integer>
```

限制如下：

- 只允許 `--workload=wal_write_ceiling`；其他 workload 帶入時回傳明確 CLI error。
- 值必須是可表示為 `std::size_t` 的正整數；segment 太小時沿用 `Wal::open()` 與既有
  frame-fit 檢查回傳錯誤，不在 benchmark 複製另一份 WAL format 常數。
- 所有 `wal_write_ceiling`、no-rotation epoch 與 rotation diagnostic 的 `Wal::open()` 使用該值，
  不再直接使用 hard-coded `kWalCeilingSegmentSize`。
- 所有 summary 的 `segment_size_bytes` 輸出實際 option 值。
- default 保持 256 MiB，未帶 option 的既有行為與測試輸出不變。

不將此 option 接到 `RuntimeConfig`，也不改 production `wal_segment_size`。本修改只建立受控實驗能力。

### 3.2 CLI contract tests

在既有 benchmark CTest 旁最小新增或擴充以下案例：

1. 非數字、零與 overflow：exit non-zero，回傳固定 error code。
2. 非 `wal_write_ceiling` workload 使用 option：exit non-zero。
3. 小型 `wal_write_ceiling` smoke 明確傳入 segment size，summary 回報相同
   `segment_size_bytes`，並有 `replay_verified=true`。
4. 既有未帶 option 的 smoke 仍回報 256 MiB，證明 default 不變。

不為此 option 建立新 library test seam；它只是把既有 `Wal::open(segment_size)` 參數暴露給 benchmark。

### 3.3 Measured-window marker

為了讓外部 `mpstat`／`iostat` 能排除 setup、epoch reopen／replay 與清理階段，benchmark-only
路徑新增：

```text
--wal-measurement-marker=PATH
```

限制如下：

- 只允許 `wal_write_ceiling` 的 no-rotation、`sync=none` workload；
- 啟用 marker 時，所有 measured commands 必須落在單一 no-rotation epoch；若總 command 數超過
  epoch 上限，benchmark 必須拒絕執行；
- 在第一個 measured append 開始前，以 truncate 寫入
  `measured-window-start`；
- 最後一個 measured append 結束後，以 append 寫入
  `measured-window-end`；
- marker 寫入不包含在 append service timer 中；
- 不接到 production runtime config、public WAL API 或 production data path。

操作腳本在 start marker 後啟動 observer，在 end marker 後停止 observer。若任一 marker 缺失，
該輪只能作為無效診斷資料，不能宣稱 I/O 與 append tail 已對齊。

### 3.4 預估修改量

| 檔案 | 必要修改 |
| --- | ---: |
| `benchmarks/order_book_benchmark.cpp` | 約 55～85 行 |
| `benchmarks/CMakeLists.txt`／既有 CLI check | 約 30～45 行 |
| production source／public headers | 0 行 |

若實作需要修改 production WAL 才能傳入此值，代表範圍設計錯誤，應停止而不是擴大修改。

## 4. 測試前置條件

每個正式 case 前執行相同 30 秒 idle preflight；任一條件不合格只重試 preflight，不保留 workload
結果：

- benchmark affinity 中每個 CPU 的 30 秒平均 idle，取最低值後必須 >= 90%；
- CPU iowait p95 <= 5%；
- WAL device util average <= 5%；
- WAL device aqu-sz p95 <= 0.25；單一 max outlier只記錄，不單獨阻塞；
- 沒有其他 order-books benchmark、fio、strace 或 observer workload 殘留；
- filesystem free space足以容納當前 case加 20% safety margin。

固定條件：Release build、同一 binary SHA-256、同一 CPU affinity、同一 filesystem/device、boost／governor
不在 campaign 中途修改。每輪使用全新 data directory；replay驗證完成後只保留 summary、time、mpstat、
iostat與必要 trace，case WAL data可從該 run root刪除，避免再次累積數百 GiB。

刪除只限 runner剛建立且已驗證位於本次 run root下的 `data/`；不得使用空變數、glob、repository path或
home directory作為刪除目標。

## 5. 最小實驗矩陣

### 5.1 A：prepare lane append ceiling（必要）

目的：完全排除 group fsync與rotation，驗證 worker-only hypothesis。

固定：

```text
workload=wal_write_ceiling
batch=8192
sync=none
segment_size=4 GiB
no-rotation epoch enabled
parallel_prepare_min_commands=4096
phase_profile=off
```

矩陣：

```text
W=1, W=2, W=4
每個 W 五輪
```

校準後凍結所有 W 共用的 measured command count，且所有 measured commands 必須位於單一
no-rotation epoch。應在 4 GiB segment 容量內盡量讓最快 case 至少執行 20 秒；若單一 segment
容量不足，使用可容納且 batch-aligned 的最大 command count，並在報告記錄實際量測時間限制。
不得為延長量測時間增加 segment size 或改用多個 epoch。

必要輸出：

- service／wall commands/s、MiB/s；
- append group p50／p99／p99.9／max；
- measured user/system CPU seconds與 CPU seconds/M commands；
- voluntary／involuntary context switches；
- max RSS；
- actual parallel groups／prepare tasks；
- write calls、WAL bytes、zero rotations、byte-plan與 replay驗證；
- measured-window marker 的 start／end 各一次；
- 同期 mpstat／iostat。

正式 ceiling使用 profile-off。只對 W=1與A階段選出的最佳W各做三對短off/on overhead check；未入選的
W不跑profile。bias >5%時，nested prepare／encode／CRC／frame數字只能定性使用。

停止條件：

- 若 W=2 已達 1.53M/s，W=4仍執行五輪，但只判斷是否提供至少額外 5%且不惡化 tail／CPU；
- 若 W=2與W=4都低於 1.53M/s，不再增加 lane count或新增通用 thread pool，直接判定 worker-only
  hypothesis 不足；下一案才考慮減少 per-record allocations/copies。
- W=4 相對 W=2 throughput提升 <5%，或 p99/p99.9惡化 >10%，或 CPU seconds/M增加 >10%，
  則不選 W=4。

### 5.2 B：no-rotation durable 驗證（必要，限兩個 W）

只取 A 的 W=1 baseline與最佳 candidate，不重跑全部 lane矩陣：

```text
batch=8192
sync=per_group
segment_size=4 GiB
1,000 measured groups
phase_profile=off
W=1, best-W
每個五輪
```

先以 byte plan證明整輪不 rotation；若實際 byte budget可能超過 segment，將 groups等比例降到不 rotation，
但每輪至少保留 500 sync samples，並在報告明示這是實驗性 frontier而非 production SLO。

必要輸出除 A 外再包含 sync與group-total p50／p99／p99.9／max、fsync/s、commands/fsync及
`measured_wal_sync_calls == planned groups`。

主要判定：

- 觀測 durable RPS是否接近
  `1 / (1/R_append + 1/R_sync_equivalent)`；
- candidate是否達到 1M/s；
- 若 append >=1.53M/s但 durable仍明顯低於模型，才調查未歸因的同步／量測成本；
- 若 append未達1.53M/s，durable未達1M是預期結果，不再歸咎fsync。

### 5.3 C：rotation影響（必要，僅最佳 W）

以 B 的最佳 W、相同 command count與相同 sync mode比較：

```text
segment_size=256 MiB
segment_size=4 GiB
batch=8192
sync=per_group
每個五輪
```

4 GiB case必須為zero-rotation control；256 MiB case保留所有rotation，不重試或排除含rotation的慢輪。
另固定使用 256 MiB segment及既有 `--wal-rotation-diagnostic=control|trigger` 做十對 paired
single-rotation診斷，避免為了預置4 GiB segment產生不必要的磁碟用量；取得：

- rotation total；
- old-segment sync；
- header write；
- header fsync；
- directory fsync。

segment-size決策只在下列任一條件成立時進入下一份production設計：

- 256 MiB相對4 GiB throughput median降低 >=5%；
- p99或p99.9惡化 >=10%，且慢sample與rotation count／diagnostic penalty一致；
- rotation group比例足以落入目標latency percentile，例如超過0.1% groups而直接影響p99.9。

若未達任一條件，segment size保持現狀，不新增production調整。

### 5.4 D：page-cache/writeback歸因（獨立診斷，不作ceiling）

只對 A 的 W=1與最佳 W各執行一輪短診斷：

```text
batch=8192
sync=none
segment_size=4 GiB
zero rotation
phase_profile=on
```

同步收集 mpstat、iostat、`/proc/self/io`、Dirty／Writeback before/after。另跑一輪相同短 workload，以：

```text
strace -ff -ttt -T -e trace=write,fsync
```

只統計 WAL descriptor的write duration；strace輪不得用於throughput比較。判定：

- append tail與慢 `write(2)`、iowait／await／aqu-sz同期：`writeback-supported`；
- append tail存在但write syscall不慢，且plan/copy profile有相同tail：`userspace-memory-supported`；
- 時間無法對齊或profile bias不合格：`inconclusive`。

本實驗不因任何分類修改kernel dirty ratios、sysctl、filesystem mount options或I/O scheduler。

## 6. 正確性與有效性 gates

所有納入結果的 case必須同時滿足：

- exit status 0；
- `replay_verified=true`；
- WAL byte plan、EngineSeq順序與command count一致；
- sync case的sync counter等於planned groups；
- no-rotation case為zero rotations，rotation-inclusive case的實際count完整保留；
- 五輪 throughput CV <=5%，否則標記unstable，不挑最快輪；
- profile-on/off bias符合第5.1節限制；
- binary與repository identity在campaign前後一致。

不將不同run root、不同binary、不同segment size或不同W的samples拼接成一個distribution。

## 7. 報告格式

結果另寫：

```text
docs/wal-prepare-rotation-minimal-experiment-report.md
```

至少包含：

1. identity、host、CPU、filesystem、device、affinity與preflight；
2. frozen command/group count、W、threshold、batch、segment size、sync mode；
3. A/B/C完整五輪原始值及median、range、CV；
4. append、sync、group-total latency與CPU seconds/M commands；
5. actual prepare task、write、sync、rotation counters；
6. control/trigger rotation breakdown；
7. page-cache診斷與strace限制；
8. 下列封閉結論之一，不得改寫成更強宣稱。

| 結果 | 後續 |
| --- | --- |
| W=2或W=4 append >=1.53M/s，durable接近／達1M/s | 另案評估最小production lane default/config；本實驗不直接改 |
| 最佳W append <1.53M/s | worker-only不足；下一案只處理serialize allocation/copy |
| 4 GiB顯著改善且rotation證據對齊 | 另案設計production segment size與retention/recovery取捨 |
| segment size沒有顯著效果 | 不改segment；停止rotation方向 |
| writeback-supported | 保留fsync語意，另評估storage與batch latency frontier |
| userspace-memory-supported | 下一案細分record-cache growth／chunk materialization |
| 證據無效或不完整 | 報告inconclusive；不得修改production |

## 8. 明確排除的調整

本實驗完成前，下列變更都不是必要修改：

- `max_group_wal_bytes`；
- dynamic/adaptive batch；
- 根據Dirty／Writeback即時調整group size；
- 修改production `wal_prepare_lanes` default；
- 修改production segment size default；
- 改用`fdatasync`或減少durability boundary；
- prepared-buffer representation重寫；
- `writev`、mmap、direct I/O、io_uring；
- Publisher／Completion優化。

這些項目只有在本文件的封閉判定指向它們時才另案設計，避免把多個變因混入同一輪而失去歸因能力。
