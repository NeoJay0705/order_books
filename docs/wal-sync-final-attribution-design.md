# WAL sync 最終 syscall／storage 歸因設計

## 1. Review 結論

本設計接續：

- `docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-retest-report.md`；
- 已完成的 application-level WAL sync collector；
- 已完成的 collector-only／full telemetry 拆分與 deterministic drain snapshots。

前次重測已證明兩件事：

1. raw collector 相對 telemetry-off 的 median bias 為 0.887%，沒有證據顯示它是主要退化來源；
2. 有 telemetry 的慢輪相對快輪多出約 8--9 秒 WAL sync wait，可解釋約 94%--96% 的
   elapsed 差距。

但現有 artifact 只有相對於 measured phase 的 application timestamp；`iostat` 沒有可與它精確換算的
共同時鐘，而前次沒有執行 syscall tracing。因此目前只能將瓶頸縮小到 `Wal::sync()` 所涵蓋的同步邊界，
不能區分：

- `fsync` syscall 本身；
- filesystem／kernel writeback；
- block device latency 或 queueing；
- application 在 syscall 外的等待。

要完成最後一次 fsync 監測調整，只需補上共同時鐘起訖錨點，並以獨立診斷輪收集 syscall 與 device artifact。
不需要再增加 production metrics，也不應在取得證據前修改 durability、group commit 或 storage policy。

## 2. 需求與完成定義

### 2.1 必須回答

1. application 的慢 `wal_sync_latency_us` sample 是否與同一時間區間的 `fsync` syscall tail一致？
2. WAL data `write` 是否在慢輪中佔主要等待時間？
3. 慢 syscall 是否同時伴隨 WAL 所在 block device 的 await、queue 或 utilization 上升？
4. 若 syscall 不慢，`Wal::sync()` 邊界內是否仍有可見的 application-only 差額？

### 2.2 完成條件

完成一次預先固定的 attribution run set 並產生報告後，本需求即結束。報告必須將根因判定為下列之一：

- `fsync_syscall`：application sync tail 與WAL descriptor的`fsync` tail對齊；
- `wal_write_syscall`：WAL descriptor的`write`是主要慢區段；
- `device_correlated`：慢 syscall 與同時間窗 device latency／queue 同時上升；
- `filesystem_or_kernel`：syscall 慢，但現有 device 證據沒有相應 tail；
- `application_outside_syscall`：application sync 區間慢，但對應 syscall duration 不慢；
- `not_reproduced`：預定輪次沒有重現既有 tail；
- `inconclusive`：artifact 不完整、時鐘無法對齊或 tracing 明顯失真。

允許複合結果，例如 `fsync_syscall + device_correlated`。完成條件不是必須找到硬體根因，而是證據足以
停在明確層級，或誠實標示 `not_reproduced`／`inconclusive`。除非 artifact validity 失敗，否則不得以
追加未預先排定輪次的方式追到想要的結果。

## 3. 範圍與不變條件

### 3.1 本次範圍

- benchmark-only clock anchor 與輸出欄位；
- 既有 sync CSV sample 與外部 tracing 的時間換算規則；
- Linux attribution procedure、artifact validation 與報告格式；
- 對新增 benchmark 行為的 unit／integration tests。

### 3.2 明確不做

- 不修改 `FileOps::sync_file()`、`Wal::sync()` 或其 public/internal signature；
- 不在 production hot path 增加 per-syscall callback、virtual interface、mutex 或 allocation；
- 不把 strace、eBPF、iostat 或 exporter 加入 runtime dependency；
- 不改用 `fdatasync`、`O_DIRECT`、`O_DSYNC` 或 io_uring；
- 不改 fsync 次數、group size、group delay、WAL format、RPO=0 或 durable callback 語意；
- 不處理 Publisher／Completion 優化；
- 不以 traced run 的絕對 RPS 作 production capacity 結論；
- 不重新設計通用 clock synchronization framework。

這些項目不是完成歸因所必需，納入會混淆 baseline 或超出需求。

## 4. 架構與資料流

```text
Engine writer
  append_batch() -> write(WAL fd)
  Wal::sync()     -> fsync(WAL fd)
       |
       +-- existing benchmark collector
       |     sync duration + measured elapsed timestamp
       |
       +-- external syscall trace (diagnostic run only)
             open/write/fsync/close + fd/path + epoch timestamp

benchmark measured phase
  system_clock before
  steady_clock start/end
  system_clock after
       |
       +-- start/end clock anchors + uncertainty
                  |
                  +-- map application elapsed timestamp to epoch
                                  |
iostat -t --------+---------------+-- correlate device time window
```

production data flow、durability boundary 與 thread ownership 完全不變。clock anchor 只在已啟用
`--engine-tail-telemetry-output` 的 benchmark execution 中建立。

## 5. 必要修改一：共同時鐘錨點

### 5.1 問題

CSV 的 `elapsed_us` 是相對 `std::chrono::steady_clock` measured epoch；外部工具通常輸出 realtime epoch。
缺少 mapping 時，無法判定某個 150 ms application sync sample 是否就是某次 150 ms `fsync`，也無法把它
放入正確的 iostat 秒級時間窗。

### 5.2 最小資料模型

只在 `benchmarks/engine_tail_telemetry.*` 增加 benchmark-local anchor：

```cpp
struct TailClockAnchor {
  std::uint64_t realtime_epoch_ns{};
  std::uint64_t steady_elapsed_ns{};
  std::uint64_t uncertainty_ns{};
};
```

`begin_measured()` 與 `begin_drain()` 各依下列順序擷取一次：

```text
realtime_before = system_clock::now()
steady_observed = steady_clock::now()
realtime_after  = system_clock::now()
```

- `realtime_epoch_ns` 為 before／after 的安全 midpoint；
- start 的 `steady_elapsed_ns` 為0，end 為 `steady_observed - measured_epoch`；
- `uncertainty_ns` 為完整 bracket width，而不是假設為零；
- 若 clock value、elapsed 或 midpoint 換算溢位，phase transition失敗，該輪不得作 attribution；
- start/end anchor各只能建立一次，不可在同一輪被重設；
- end anchor同時給出精確measured window上界，避免把warmup、drain或rotation syscall混入。

選擇 bracket 而非直接連續讀取兩個 clock，是為了讓對齊誤差有可驗證上限。這不要求假設 C++
`steady_clock` 的 epoch 等同 Linux `CLOCK_MONOTONIC`。起訖兩個anchor也用來檢查測試期間是否發生
realtime clock step：若realtime差與steady elapsed的差異超過兩端bracket uncertainty及clock
resolution allowance，該輪不得作跨時鐘歸因。

### 5.3 輸出介面

在 telemetry summary 增加且只增加：

```text
tail_clock_start_realtime_epoch_ns=<uint64>
tail_clock_start_uncertainty_ns=<uint64>
tail_clock_end_realtime_epoch_ns=<uint64>
tail_clock_end_steady_elapsed_ns=<uint64>
tail_clock_end_uncertainty_ns=<uint64>
```

既有 CSV header、row type 與 `elapsed_us` 語意保持不變，避免破壞已有 parser。CSV 中 sync row 的
`elapsed_us` 是 `observe()` 時刻，也就是 application sync 區間的近似結束點；`value` 是該區間 duration
（microseconds）。離線換算固定為：

```text
application_end_epoch_ns = tail_clock_start_realtime_epoch_ns + elapsed_us * 1,000
application_start_epoch_ns = application_end_epoch_ns - value * 1,000
```

判定 overlap 時必須把 `tail_clock_uncertainty_ns` 與 microsecond truncation 納入容許範圍。不得把
`elapsed_us` 誤解成 syscall entry timestamp。只有落在start/end anchor定義的measured window內的外部事件
才可納入count與duration aggregate。

不新增每筆 realtime timestamp：它會重複資料、增加熱路徑 clock read，且共同 anchor 已足以換算。

### 5.4 元件責任

- `EngineTailTelemetry`：建立、保存及提供起訖anchor；不執行外部工具。
- `order_book_benchmark.cpp`：將 anchor 印入現有單行 summary，並在 validation 中要求欄位有效。
- `check_engine_tail_telemetry.cmake`：驗證epoch／end elapsed為非零整數、uncertainty為合法整數且有界。
- production `MetricsRegistry`、`Wal`、`FileOps`：零修改。

## 6. 必要修改二：獨立 syscall／device 診斷輪

### 6.1 為何使用外部工具

`FileOps` 目前明確使用 `write(2)` 與 `fsync(2)`。在 wrapper 內再加計時只能再次證明 application
看到 syscall 很慢，且會把診斷介面永久帶入 production storage code。外部 tracing 可以同時取得 syscall
名稱、fd/path、開始時間、duration 與結果，因此是本需求較小且證據更直接的作法。

### 6.2 固定 workload

診斷沿用產生問題的設定，不重新調參：

- workload：`engine_durable_single_instrument`；
- instrument/shard：1/1；
- W=2；
- group size 4096；
- group delay 1000 us；
- producer lanes 8192；
- parallel threshold 4096；
- telemetry：collector-only，state sampling off；
- CPU affinity、binary、filesystem、warmup 與 iteration count在同一 run set 固定。

collector-only 已通過 0.887% bias，且本需求只需要 sync/group sample；full state sampler 對 syscall
歸因不是必要條件。這些輪次是 diagnostic evidence，不取代先前被 gate 阻擋的正式 W1/W2 performance
matrix。

### 6.3 預先固定輪次

後續 procedure 應在執行前固定有限輪次，至少包含：

1. untraced observed runs：確認 clock anchor、sync CSV 與 timestamped `iostat` 可對齊；
2. syscall-traced runs：使用同一 workload，捕捉 `openat`、`write`、`fsync`、`close`；
3. 每輪均保留 exit status、stdout/stderr、CSV、trace、`iostat -x -t`、process I/O 與
   `/usr/bin/time -v`。

實際輪數與 timeout 寫入獨立 procedure，不能由執行者看到結果後臨時增加。traced 與 untraced run
分開報告，絕對 throughput 不互相比較。

### 6.4 WAL syscall 識別

syscall trace 必須能由 fd annotation 或同輪 `openat` mapping 將事件限定至 benchmark summary 所列
`wal_path` 下的 `.wal` segment。stdout、CSV、cursor、snapshot、directory fsync 與其他 fd 不得合併到
WAL data 統計。

每輪至少驗證：

- WAL `fsync` 成功次數與 application `measured_sync_count` 可解釋；warmup、rotation、drain 等
  measured window 外呼叫須分開；
- syscall return value／errno 完整；unfinished/resumed syscall 能正確重組；
- application 慢 sample 可以對應到至多一個主要 WAL sync syscall interval；
- trace 若截斷、fd path 不可辨識或 mapping ambiguous，該輪標記 invalid，但 artifact 保留。

不要求 syscall count 和 measured group count 無條件完全相等，因為 trace 含 warmup／rotation 等階段；
必須先依共同時鐘裁切 measured window後再比較。

### 6.5 Device correlation

`iostat -x -t 1` 必須在固定UTC與ISO time format下輸出absolute timestamp，並記錄WAL filesystem到
block device的mapping。
device 指標只用來判定同期 correlation：

- application sample 對齊 WAL sync syscall；
- syscall interval 落入相同 device sample window；
- 同一窗口的 await／aqu-sz／utilization 相對該 run 的正常窗口上升。

一秒聚合無法證明單次 block request 因果，因此只允許輸出 `device_correlated`，不得寫成
`device_proven`。若必須再區分 filesystem 與實體裝置，可在同一份 procedure 預先定義一個有權限才執行的
block trace；工具不可用時結果維持 `filesystem_or_kernel`，不因此修改 application code。

## 7. 歸因規則

對每個超過既定 tail threshold 的 application sync sample，建立：

```text
application interval = [mapped end - duration, mapped end]
syscall interval     = [trace start, trace start + trace duration]
device window        = timestamped iostat interval
```

`write`發生在`Wal::sync()`之前，不能與sync sample強行建立一對一overlap；它應在同一measured window內
按WAL fd統計count、total與tail distribution，再與fsync及elapsed contribution比較。報告至少列出fsync
overlap、duration差額、write/fsync aggregate及所在device window。結論規則如下：

| 證據 | 允許結論 |
| --- | --- |
| application 與 WAL fsync interval 對齊，duration 在 clock uncertainty 後一致 | `fsync_syscall` |
| measured window 的WAL write total/tail是主要syscall成本，fsync不是 | `wal_write_syscall` |
| fsync/write syscall tail 同期 device await/queue 上升 | 加註 `device_correlated` |
| syscall tail 存在但 device 指標未同期上升 | `filesystem_or_kernel`，不能排除 device |
| application interval 顯著大於所有對應 syscall interval | `application_outside_syscall` |
| 預定輪次沒有達到既有 tail threshold | `not_reproduced` |
| clock、fd 或 artifact validation 失敗 | `inconclusive` |

tail threshold 沿用既有 25/100/250 ms buckets，不新增事後挑選的 threshold。結論需同時報告命中 sample
數與總 sample 數，不可只展示最大 outlier。

## 8. 錯誤處理與 artifact validity

- 任一起訖clock anchor失敗：benchmark非零結束；不得輸出看似有效的零值。
- 起訖anchor顯示clock discontinuity：該輪只保留單一clock domain內的資料，跨工具歸因為invalid。
- summary/CSV anchor 或 sample 換算溢位：該輪 invalid。
- 外部工具缺失或權限不足：在執行前失敗；不得把沒有 trace 的輪次標成 traced。
- benchmark 或 tracer 非零結束：保留全部 artifact並標記 invalid。
- syscall trace 造成 dropped events、截斷或無法解析 fd：結果為 `inconclusive`。
- `iostat` 缺 sample 或 device mapping 不唯一：只撤回 device 判定，不撤回已有 syscall 判定。
- WAL／Engine 原始錯誤優先；diagnostic error不得覆蓋 durability failure。
- 所有輪次、invalid attempt 與環境變化都列入報告，不刪除 outlier。

## 9. 測試策略

### 9.1 Unit tests

- `begin_measured()` 與 `begin_drain()` 各只建立一次有效anchor；
- realtime midpoint位於各自擷取bracket內，零uncertainty合法且不得大於bracket width；
- 起訖realtime差與steady elapsed一致，clock discontinuity可被拒絕；
- elapsed/value 到 epoch interval 的 checked arithmetic 正確處理正常值、下溢及上溢；
- phase transition失敗時不留下可誤用的有效 anchor；
- 既有 sync/group/state/drain aggregate 與 CSV schema 不變。

clock test 不比較實際 sleep duration，也不要求 nanosecond 級精準值，避免 flaky test。

### 9.2 Integration／CTest

- telemetry full與collector-only smoke summary都含五個clock欄位；
- telemetry off 不輸出 clock anchor；
- 既有 artifact consistency、dropped sample與drain boundary檢查繼續通過；
- traced tooling 不納入一般 CTest，因為 Linux tool與權限不是 build correctness 的必要條件。

### 9.3 執行驗證

實作階段先完成 ReleaseBenchmark、Debug 與 ASan/UBSan build/test，再由使用者確認後建立獨立 benchmark
procedure並執行。不能以 compile success 取代外部 artifact validation。

## 10. 重要取捨

### 10.1 起訖 anchor，而非逐筆 realtime clock

兩個bracketed anchor能把既有elapsed timestamp映射到外部時間、限定measured window並偵測clock step；
額外成本只在phase boundary。逐筆同時讀system/steady clock只會提高collector成本，沒有增加必要資訊。

### 10.2 外部 syscall trace，而非 production hook

tracing只在診斷輪啟用，能看到真實 syscall與fd，又不永久增加 WAL abstraction。代價是 traced RPS不能作
容量數據；本設計已將 attribution與performance aggregate分開。

### 10.3 device correlation，而非過度宣稱因果

一秒 iostat只適合correlation。保留這個限制比導入必須root權限的常駐block tracer更符合目前規模；只有
需要再區分filesystem/device時才使用預先定義的條件式block trace。

## 11. 已知限制與後續方向

- tracing可能放大syscall duration，因此只判定時間重疊與成本所在層，不使用其絕對RPS。
- 起訖anchor可偵測但不能修正測試期間的system clock step；procedure需記錄clock source與NTP/time
  adjustment。若發生clock discontinuity，該輪跨工具歸因invalid。
- iostat仍是聚合資料，不能單獨證明某一筆I/O的因果。
- 本設計只定位現有sync tail，不保證解決距離1M commands/s的其餘5.8--6.8倍差距。
- 根因定位後另立最小optimization設計，例如group policy、filesystem/storage設定或WAL write策略；不在
  本需求提前選方案。

## 12. 必要修改與預估範圍

| 檔案／區域 | 必要修改 | 預估 |
| --- | --- | ---: |
| `benchmarks/engine_tail_telemetry.hpp/.cpp` | 起訖clock anchor、checked mapping所需helper與summary欄位 | 163行 |
| `benchmarks/order_book_benchmark.cpp` | validation與summary輸出 | 28行 |
| `benchmarks/check_engine_tail_telemetry.cmake` | smoke欄位驗證 | 10行 |
| `tests/unit/engine_tail_telemetry_test.cpp` | anchor、discontinuity與arithmetic測試 | 25行 |
| production WAL／Engine／FileOps | 不修改 | 0行 |

實作後的實際程式與測試差異為約226行；相較原先預估上限增加的部分，全部來自system/steady clock
checked conversion、起訖anchor overflow/discontinuity validation及其純函式測試，沒有加入trace parser
framework、production instrumentation、通用clock library或效能調整。後續若再超過約240行，應先檢查
是否引入上述非必要項目。procedure與結果報告另計，因為它們是診斷的可重現交付物，不是runtime
implementation。

## 13. 設計與實作一致性清單

- [ ] production `Wal`、`FileOps`、Engine API、durability與group policy完全不變。
- [ ] 起訖clock anchor只在benchmark telemetry啟用時建立。
- [ ] summary只新增第5.3節五個clock欄位；CSV schema不變。
- [ ] 起訖anchor限定measured window並能拒絕clock discontinuity。
- [ ] sync CSV timestamp明確代表application interval近似結束點。
- [ ] 所有epoch換算使用checked arithmetic並計入uncertainty。
- [ ] collector-only仍不啟動state sampler。
- [ ] syscall tracing只存在獨立診斷輪，不納入正式RPS aggregate。
- [ ] 只統計`wal_path`下WAL segment所對應的fd事件。
- [ ] warmup、measured、drain與rotation syscall不混算。
- [ ] device證據只宣稱correlation，不宣稱單筆因果。
- [ ] 預定輪次全部保留；沒有重跑挑選或隱藏invalid artifact。
- [ ] 報告使用第2.2節封閉分類，完成後停止增加fsync telemetry。
- [ ] 根因成立前不修改production效能或durability設定。

滿足本清單後，設計、實作、artifact與結論的認知即一致。本需求是最後一次fsync監測調整；其後工作應是
依歸因結果設計最小優化，或在`not_reproduced`／`inconclusive`時保留限制，而不是繼續擴張telemetry。
