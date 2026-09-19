# WAL sync／storage tail 根因分析設計

## 1. 文件目的與 review 結論

本文件承接：

- `wal-parallel-prepare-production-configuration-design.md`；
- `wal-parallel-prepare-production-configuration-benchmark-report.md`。

最近一次 public Engine durable benchmark 已確認 bounded parallel prepare 的設定、parallel／fallback
路徑、WAL bytes、replay、durable head 與 completion boundary 都正確；但 W=2 五輪 median throughput
只由 157,880 提升至 169,982 commands/s（+7.67%），未達 +10% rollout gate。W1 r4、W2 r2/r3
與 W4 r5 同時出現 `wal_sync_full_run_p99_us=250,000` 及吞吐驟降，顯示 storage tail 是重要干擾因子，
但現有資料不足以判定 tail 位於 warmup 或 measured window，也不足以把原因歸給 device、filesystem、
其他程序 I/O 或 parallel prepare。

因此下一階段只做 **WAL sync/storage tail 根因量測**，不直接進行效能優化。經 review，必要修改只有：

1. 在 `engine_durable_single_instrument` 加入 opt-in、benchmark-only telemetry，保存 measured window 的
   每 group sync sample 與 group command sample；
2. 以低頻、固定週期取得 queue depth 與 publisher lag snapshot，讓 sync tail、backlog 與 drain 能在
   同一 monotonic timeline 對齊；
3. 在 hot path 完成後才輸出結構化 artifact 與摘要，並用 telemetry off/on calibration 量化觀測偏差；
4. 定義同環境的 W=1/W=2 對照矩陣及外部 block-device／syscall 觀測，產出根因報告。

本案不修改 WAL append、`fsync`、group commit、parallel prepare、StateMachine、Publisher、Completion、
durability boundary 或 production default。現階段若直接調整演算法，將無法區分真正收益與 storage
tail 噪音，也會超出已有證據支持的需求。

## 2. 需求理解與合理假設

### 2.1 必須回答的問題

1. 慢輪中的 100／250 ms 等級 sync tail 是否真的發生在 measured window，而不是 warmup？
2. 吞吐下降能否由 measured sync duration、tail 次數及 tail time contribution 解釋？
3. W=1 與 W=2 在相同 group policy 下，sync distribution 是否有可重現差異？
4. sync tail 發生時，writer queue 或 publisher lag 是否持續增加，停止輸入後能否收斂？
5. application sync tail 是否和 `fsync` syscall duration、block-device await／queue／utilization 同期？
6. 證據足以支持下一案處理 storage/filesystem、group policy、prepare CPU，或只改善測試環境嗎？

### 2.2 不變條件

- 單一 shard、單一 instrument、public Engine durable path。
- 一筆 command 仍只在 WAL append 與當次 group `fsync` 成功後完成 durable callback。
- WAL record bytes、ordering、RPO=0、replay、Publisher visibility 與 callback semantics 均不變。
- group size、group delay、producer lanes、CPU affinity、filesystem 與資料目錄策略在配對 case 間固定。
- W=1 是 production default；本案不因診斷結果自動啟用 W=2 或 W=4。
- telemetry 預設關閉；未指定 telemetry option 時，benchmark 行為與輸出相容。
- 所有 application timestamps 使用 `std::chrono::steady_clock`；不能拿 wall-clock 做 duration。
- sanitizer run 只驗證 correctness，不產生效能或 tail 結論。

### 2.3 量測語意

- `wal_sync_latency_us` 已由 writer 在每次成功 `Wal::sync()` 前後量測，並以一個 sample／group 傳入
  `MetricsSink`；新 telemetry 直接接收這個 sample，不在 WAL 內再取一次 clock。
- sample timestamp 是 `MetricsSink::observe()` 收到 sample 的時間，近似 sync end；sync start 可用
  `end - duration` 推算。這是 correlation timestamp，不宣稱是 kernel trace timestamp。
- measured window 從 warmup durable completion 完成後開始，到 measured durable completion 完成為止。
- drain window 從 measured window 結束到 `Engine::stop()` 完成。吞吐及 sync percentile只計 measured；
  queue／publisher lag 必須分別報告 measured 與 drain，不能混成單一 percentile。
- 現有 `wal_sync_full_run_p99_us` 保留相容性，但明確標記為包含 warmup 的 bounded histogram；新的
  measured p50/p99/p99.9/max 由 raw measured samples計算，兩者不可混用。

## 3. 範圍與非目標

### 3.1 In scope

- `engine_durable_single_instrument` 的 opt-in telemetry sink、periodic state sampler 與 artifact 輸出；
- measured sync／group sample 的 exact emitted values、percentiles、threshold counts 與 total duration；
- measured／drain queue depth及 publisher lag的週期 snapshot；
- telemetry correctness、CLI validation、artifact schema 與 instrumentation-bias smoke/calibration；
- W=1/W=2 五輪 controlled comparison；W=4 只可作 diagnostic；
- `strace`、`iostat`、`pidstat`、`/usr/bin/time` 及可用時的 `perf` 作外部交叉證據；
- 根因分析報告。

### 3.2 Out of scope

- 修改 `fsync` 為 `fdatasync`、降低 sync 頻率或改變 RPO；
- async WAL、multiple in-flight sync、AIO、`io_uring`、direct I/O 或 mmap；
- 自動調整 group size／delay、prepare lanes 或 production default；
- kernel、filesystem、mount option、I/O scheduler、CPU governor 或 page cache 的自動修改；
- 在 core library 新增 tracing framework、Prometheus client 或第三方 telemetry dependency；
- 逐 command trace、完整 queue event trace或把所有 metrics 複製到記憶體；
- Publisher／Completion throughput 優化；
- production exporter 或部署平台 dashboard。Core library 已透過 `MetricsSink` 提供 sample，實際 exporter
  由部署整合負責；本案只讓 repository benchmark 能建立可重現證據。

## 4. 架構與模組邊界

production 路徑保持不變：

```text
Shard writer
  -> Wal::append_batch()
  -> Wal::sync()
  -> MetricsRegistry::observe("wal_sync_latency_us", duration)
  -> existing downstream MetricsSink
```

telemetry 開啟時，benchmark 提供專用 downstream sink：

```text
Engine MetricsRegistry
  -> BenchmarkTailTelemetrySink
       -> measured sync samples       (one per durable group)
       -> measured group-command rows (one per durable group)

Benchmark controller
  -> warmup durable completion
  -> begin measured telemetry
  -> run measured load
  -> mark drain
  -> Engine::stop()
  -> stop telemetry
  -> write artifact and summary

Periodic sampler (fixed 10 ms)
  -> Engine::metrics(shard)
  -> queue depth + publisher lag snapshot
  -> in-memory bounded rows
```

`BenchmarkTailTelemetrySink` 與 sampler 只存在 benchmark target，不加入 public header、RuntimeConfig、
`ShardRuntime` 或 WAL。不得為了 benchmark 暴露 `MetricsRegistry` internal buckets 或 reset API。

## 5. Benchmark telemetry 設計

### 5.1 CLI 與預設行為

新增一個 benchmark-only option：

```text
--engine-tail-telemetry-output=<path>
```

- 未提供時 telemetry 完全關閉，仍使用 `NullMetricsSink`。
- 只允許搭配 `--benchmark=engine_durable_single_instrument`；其他 benchmark 使用時回報明確 CLI error。
- path 必須非空，且不得覆寫既有檔案；開啟或寫入失敗時，在 Engine 正常停止後以非零狀態回報
  `telemetry_output_failed`。
- state sampling interval 固定為 10 ms，不另外增加 production config 或初版 CLI knob。若 calibration
  顯示偏差過高，先降低採樣頻率並更新本設計，不在 runtime 自動調整。

### 5.2 Phase state

collector 使用明確 phase：

```cpp
enum class TelemetryPhase : std::uint8_t {
  disabled,
  measured,
  drain,
  stopped,
};
```

- Engine open 與 warmup 期間為 `disabled`，不得進入 measured sample。
- `begin_measured()` 在 warmup完成並取得 warmup metrics 後呼叫，設定 monotonic epoch。
- measured durable completion 完成後先切換 `drain`，再取得 final metrics 與停止 Engine。
- `Engine::stop()` 完成後切換 `stopped`，停止 sampler thread，再產生 artifact。
- phase transition 由 benchmark controller 單一執行緒負責；sink/sampler 只讀 atomic phase。

### 5.3 Hot-path sample

sink 只保留下列 downstream metric：

```text
wal_sync_latency_us
wal_group_commands
```

其他 metric 立即返回，不配置、不取額外 clock、不鎖 collector mutex。保留的 sample 含：

```cpp
struct TimedMetricSample {
  std::uint64_t elapsed_us;  // relative to measured epoch
  std::uint64_t value;
};
```

sync 與 group-command sample 分開保存並維持 observation order。兩者都是 writer 每 durable group 各發出
一次；正常完成時數量必須相等，`sum(group_commands)` 必須等於 measured command count。若不相等，
benchmark validation 失敗，不能用 index 猜測配對後繼續產生結論。

collector 必須在 measured 前預留容量；hot path 不寫檔案。每類 raw sample 上限為 1,000,000，超過時
只增加 saturating dropped counter。任何 dropped sample 都使該輪 telemetry 無效並在 summary 回報，
但不得改變 Engine durability 或 shutdown。

`observe()` 必須隔離 allocation/clock 例外並保持 non-throwing behavior；collector lock 只涵蓋新增一筆
保留 sample，不得包住 percentile 計算、檔案 I/O 或 Engine call。

### 5.4 Periodic state sampler

sampler 每 10 ms 呼叫一次既有 `Engine::metrics(1)`，保存：

```cpp
struct EngineStateSample {
  TelemetryPhase phase;
  std::uint64_t elapsed_us;
  std::uint64_t queue_depth;
  std::uint64_t publisher_lag_events;
  std::uint64_t publisher_lag_bytes;
  std::uint64_t publisher_lag_age_ns;
};
```

- sampler 只在 measured／drain phase 保存資料；使用 deadline-based wait，避免 `sleep_for` 漂移累積。
- snapshot error 記錄一次 error state並停止 sampling；benchmark 完成正常 shutdown 後回報 telemetry
  validation failure。
- state rows 上限 100,000；超過同樣以 dropped counter 使 telemetry 輪次無效。
- sampler 不讀 WAL 檔、不呼叫 query、不參與 producer lane或 durable callback。
- drain 的目的只是判斷停止輸入後 backlog 是否收斂；不得把 drain 時間加入 measured throughput。

### 5.5 Structured artifact

指定 path 輸出 CSV，schema 固定為：

```text
record_type,phase,elapsed_us,value,queue_depth,publisher_lag_events,publisher_lag_bytes,publisher_lag_age_ns
sync,measured,<end_us>,<duration_us>,,,,
group_commands,measured,<observed_us>,<commands>,,,,
state,measured|drain,<sample_us>,,<depth>,<events>,<bytes>,<age_ns>
```

- 第一行固定 header；整數使用 base-10，缺值留空，不輸出 locale-dependent formatting。
- rows 先依 `elapsed_us` 穩定排序；相同 timestamp 保留各來源原始順序。
- artifact 只在 Engine 已停止、collector 不再被呼叫後產生，避免 I/O 汙染 measured window。
- CSV 不包含 order id、producer id、價格、數量或其他業務 payload。

## 6. Summary 與驗證

telemetry 開啟時，在既有單行 summary 追加：

```text
tail_telemetry=on
measured_sync_count=<n>
measured_sync_p50_us=<n>
measured_sync_p99_us=<n>
measured_sync_p99.9_us=<n>
measured_sync_max_us=<n>
measured_sync_total_us=<n>
measured_sync_over_25ms=<n>
measured_sync_over_100ms=<n>
measured_sync_over_250ms=<n>
measured_group_sample_count=<n>
measured_group_sample_commands=<n>
measured_queue_depth_max=<n>
measured_publisher_lag_events_max=<n>
measured_publisher_lag_bytes_max=<n>
measured_publisher_lag_age_ns_max=<n>
drain_publisher_lag_events_last=<n>
drain_publisher_lag_bytes_last=<n>
drain_publisher_lag_age_ns_last=<n>
telemetry_dropped_samples=<n>
tail_telemetry_file=<path>
```

percentile 使用 nearest-rank，和 direct-WAL benchmark 現有 raw sample percentile 語意一致。沒有 sample
時輸出 `na`，不能用 0 冒充量測值。`over_25ms` 等欄位採 `duration > threshold`，實作與文件必須一致。

該輪只有在以下條件全部成立時可用於根因分析：

- Engine benchmark 既有 correctness、replay、durable head、completion validation 全部通過；
- measured sync count等於 measured group commits；
- group sample count等於 measured group commits；
- group sample commands總和等於 measured commands；
- telemetry dropped samples為 0；
- sampler 沒有 metrics error；
- artifact row count及 aggregate可由離線解析重算並與 summary 一致。

## 7. 錯誤處理與生命週期

- collector／sampler 以 RAII 管理；所有 benchmark early return 都必須停止 sampler，再停止或確認已停止
  Engine，不得留下 thread。
- Engine/WAL failure 優先保留原 error code；telemetry failure作為附加 detail，不可掩蓋 durability error。
- telemetry output error不回滾或刪除 WAL data；該輪標為無效並保留其他 log。
- `Engine::stop()` 失敗時仍停止 sampler及輸出已收集 artifact，但報告必須標成 failed，不納入統計。
- percentile加總、row count、timestamp conversion及容量估算使用 checked/saturating arithmetic，避免 overflow。
- sink不得向 Engine callback、EventSink 或 MetricsRegistry反向呼叫，避免 lock cycle。

## 8. 測試策略

### 8.1 Compile／correctness

- ReleaseBenchmark build 與全量 tests。
- Debug build 與全量 tests。
- ASan/UBSan build 與全量 tests。
- TSan 僅在環境可用時執行；不可因環境無法啟動而宣稱通過。

### 8.2 Benchmark smoke

新增 CTest smoke，使用小型 public Engine workload及唯一暫存輸出 path，驗證：

- telemetry option只接受非空 path及正確 benchmark；
- summary包含 measured sync p50/p99/p99.9/max及 dropped=0；
- sync/group sample count與 measured commits一致；
- CSV header、record types、phase與整數欄位可解析；
- replay/durable head仍通過；
- 測試結束後由 test fixture清理自己建立的 artifact。

不要在 CI 斷言固定 RPS、sync latency 必須非零、一定出現 tail，或 publisher lag一定大於零。

### 8.3 Instrumentation bias calibration

正式根因矩陣前，以 W=1 與 W=2 各做 telemetry off/on 配對 calibration：

- 完全相同 group size、delay、producer lanes、affinity與資料目錄裝置；
- 採 ABBA 或交錯順序，各至少三輪，每輪至少 15 秒；
- 比較 off/on median throughput及 p99；所有慢輪保留；
- throughput median regression <=5% 才能以 telemetry-on資料做定量歸因；
- 若超過 5%，raw timeline仍可作方向性診斷，但 authoritative throughput必須使用 telemetry-off，且先
  降低 state sampling頻率或簡化 collector後重新 calibration。

### 8.4 Controlled root-cause matrix

固定沿用上一份報告的單 instrument／shard、per-group sync、group=4,096、delay=1,000 us、producer
lanes=8,192與 threshold=4,096：

| Case | Prepare lanes | 用途 |
| --- | ---: | --- |
| baseline | 1 | production default與 storage-tail baseline |
| candidate | 2 | 判斷 parallel prepare收益是否在相同 sync distribution下成立 |
| diagnostic | 4 | 只有 W=2證據仍不足時執行，不作 rollout gate替代品 |

W1/W2各五輪、每輪全新 data directory、每輪 measured >=15秒。case順序交錯，source/binary identity、
CPU affinity、governor、mount、可用空間與背景負載都寫入報告。不得只重跑最快輪，亦不得在看到結果後
刪除 tail round。

外部工具分工：

- `/usr/bin/time -v`：process CPU、context switches與 RSS；
- `iostat -xz 1`：WAL block device的 await、aqu-sz、utilization及吞吐；
- `pidstat -d -p <pid> 1` 或 `/proc/<pid>/io` sampler：process I/O；
- `strace -ff -tt -T -e trace=fsync,fdatasync,write,pwrite64`：只跑獨立診斷輪，確認慢 sample對應 syscall；
- `perf`：權限可用時才收集，不修改安全設定。

`strace` 輪有明顯 overhead，不得混入 authoritative throughput五輪。

## 9. 根因判定規則

報告必須依證據強度下結論：

- **sync tail supported**：慢輪 measured sync total/tail contribution足以解釋大部分 elapsed增加，且
  throughput下降與 application sync tail同時出現。
- **device/storage supported**：application sync tail與同期 device await／queue／utilization或 process
  I/O stall一致；若 device未飽和，只能說 latency tail，不得宣稱 bandwidth ceiling。
- **filesystem/syscall supported**：獨立 strace輪能把 application tail對齊到 `fsync`／write syscall；
  strace本身的絕對 latency不作 production數值。
- **parallel-prepare interaction supported**：在相近 sync distribution下，W=2仍有可重現 CPU／throughput
  差異；若只在不同 tail樣本下有差異，不可歸因給 prepare lanes。
- **queue/publisher pressure supported**：lag在固定輸入率下跨多個 state sample持續成長，且 drain未
  收斂；單一瞬時 max不足以成立。
- **inconclusive**：application、syscall及device evidence不一致時，保留未知，不以直覺指定根因。

本階段沒有 production rollout gate，也不重新定義 W=2 的 +10% gate；交付物是可重現根因證據與下一
份優化設計的輸入。

## 10. 檔案層級實作指引

```text
benchmarks/order_book_benchmark.cpp
  新增 benchmark-only sink、phase lifecycle、periodic sampler、aggregate、CSV writer與 CLI handling

benchmarks/CMakeLists.txt
  新增 telemetry CLI negative test及 public Engine artifact smoke

tests（只有共用 helper被抽出時才新增）
  直接測 phase/sample/CSV helper；否則由 benchmark smoke覆蓋，避免為單一 benchmark建立 production API

docs/order-book-design.md
  不修改；本案沒有改 production architecture或 public contract
```

若單一 benchmark檔案因 helper增加而顯著降低可讀性，可將純 benchmark helper抽到
`benchmarks/engine_tail_telemetry.hpp/.cpp`；不得放入 `src/runtime` 或 `include/order_books`。

預估必要變更約 220--360行：collector／sampler／artifact 150--240行、CLI與summary 40--70行、
CMake smoke／必要 helper tests 30--50行。若超過約400行，應先檢查是否誤納 exporter、通用 tracing、
production config或自動調參。

## 11. 關鍵決策與取捨

### 使用 benchmark downstream sink，而不是重設 production histogram

`MetricsSink` 已能收到每 group sync sample。benchmark sink可以取得 measured raw values，無需公開
`MetricsRegistry` buckets、加入 reset epoch或改 Engine API，影響範圍最小。

### raw sync event加低頻 state snapshot，而不是逐 command trace

sync每 group只有一筆，保存成本低；queue depth若逐 enqueue/dequeue保存會產生數百萬 rows並嚴重改變
hot path。10 ms snapshot足以看 backlog趨勢，再由 calibration量化干擾。

### 先寫記憶體、停止後輸出，而不是在 hot path寫 CSV

同步檔案 I/O會和 WAL競爭相同 filesystem，直接製造待分析的 storage tail。bounded memory與 dropped
validation可避免無界成長，又不汙染 measured I/O。

### 保留 production exporter邊界，而不是在 core repository內建 dashboard

library已有通用 `MetricsSink`。不同部署的 exporter、retention與alerting屬整合責任；本案只補可重現
benchmark artifact，避免引入不必要dependency及thread。

### 根因分析與優化分案

目前 W=2的+7.67%與少數250 ms tail混在一起。先建立可以對齊的證據，下一案才有依據選擇 storage、
group policy或CPU path；在同一變更同時量測與改演算法會失去可信baseline。

## 12. 已知限制與後續方向

- 10 ms state sampling看不到更短的queue spike；它用於趨勢，不是逐事件真相。
- `MetricsSink` timestamp位於sync完成後，與kernel/block-device timestamp只能近似對齊。
- bounded telemetry本身仍有成本，必須通過calibration；不能以「只在benchmark開啟」跳過偏差檢查。
- 本機 `powersave` governor與ext4 `/dev/sdb2`結果不能外推至production SSD／RAID／cloud volume。
- block device若與其他程序共享，`iostat`只能證明裝置同期活動，不能單獨證明因果。
- 若 evidence確認sync/storage tail，下一案才評估storage配置、group policy或I/O API；若 sync相近而
  W=2仍未達gate，則回到prepare CPU收益與resource budget，不應提高lanes碰運氣。
- production canary仍需要部署端MetricsSink exporter；本案的CSV不是production監控系統。

## 13. 設計與實作一致性清單

- [ ] 只修改benchmark／benchmark tests，不修改production WAL、Engine API或RuntimeConfig。
- [ ] telemetry未指定時仍使用NullMetricsSink，既有summary與行為相容。
- [ ] warmup sample不進入measured raw percentile。
- [ ] measured與drain phase有明確邊界，drain不計入throughput。
- [ ] sync與group command各一筆／durable group，count與commands都有validation。
- [ ] queue／publisher lag採固定10 ms snapshot，不保存逐command queue event。
- [ ] hot path不寫檔；artifact只在Engine停止且sampler join後輸出。
- [ ] sample containers有上限；任何drop都讓telemetry輪次無效。
- [ ] summary與CSV可由離線解析互相驗證。
- [ ] telemetry off/on calibration先於正式根因矩陣，偏差門檻為5%。
- [ ] W1/W2配對五輪保留所有慢輪；W4不能替代W2或rollout gate。
- [ ] external strace診斷輪不混入authoritative throughput。
- [ ] 沒有順帶修改fsync policy、group policy、prepare algorithm、Publisher或Completion。
- [ ] 根因不足時結論允許inconclusive，不把correlation寫成causation。
