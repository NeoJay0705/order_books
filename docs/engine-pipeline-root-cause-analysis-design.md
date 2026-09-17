# Engine Pipeline 根因分析設計審查與必要修改

## 1. 文件目的

本文件承接 `engine_pipeline_ceiling` 的 correctness／measurement smoke 結果，定義下一階段
確認單一 shard、單一 instrument pipeline 瓶頸根因所需的最小修改與正式量測方法。

本階段的交付物是可重現的根因證據與後續優化設計輸入，不是直接修改 production 熱路徑，
也不宣稱選出最佳 batch、queue 或 cursor persistence 參數。

## 2. 審查結論

現有 `docs/engine-pipeline-ceiling-benchmark-design.md` 符合「建立各階段 ceiling 診斷入口」的
需求，但目前 `docs/engine-pipeline-ceiling-benchmark-test-report.md` 只完成短時間 smoke，尚
不足以確認根因或選擇最佳設定：

- `StateMachine::apply` 已提供必要控制組；目前短測超過 1M commands/s，正式分析中只需保留
  為 writer CPU 成本對照，不需增加功能。
- invariant stage 已可調 active orders，足以確認完整驗證成本是否隨 state size 成長；不需
  修改 production validation。
- metrics 現有 `writer_only` 與 shared-registry contention 並非同等工作量控制，無法把兩執行緒
  的一般 CPU 成本和共享 registry lock 成本分開。
- runtime handoff 固定使用 1,024 producer lanes，只能得到合併 ceiling，無法判斷吞吐是否受
  concurrency／queue saturation 影響。
- Publisher stage 已使用正式 worker 和 durable cursor，配合長時間 backlog、syscall shape
  與 block-device 指標即可先完成歸因；不需要為量測加入可關閉 cursor durability 的 test hook。
- 現有執行環境 `kernel.perf_event_paranoid=4`，不能把 `perf` 成功視為必要條件，也不得為測試
  修改主機安全設定。

因此下一階段只新增兩個 benchmark-only 控制：

1. metrics 的雙 worker／separate registries 對照組；
2. runtime handoff 的可設定 producer lane 數量。

其餘工作是正式長測、外部觀測與結果報告。不得在同一變更中實作 incremental invariant、
typed metrics、lock-free queue、Completion batching 或 Publisher cursor batching。

## 3. 需求理解與合理假設

### 3.1 必須回答的問題

1. 完整 invariant validation 在 `0／1,000／10,000／100,000` active orders 下的固定成本與
   state-size scaling 為何？攤提到現有 group size 後占 1 microsecond/command writer budget
   多少？
2. Metrics 下降主要來自每次 `observe()` 本身，還是兩個 worker 共用 registry 的同步競爭？
3. Runtime handoff 約 400k callbacks/s 是低 concurrency、queue／condition-variable 協調，
   Completion worker，還是 benchmark producer 供給不足所造成？
4. Publisher drain 的主要 wall time 是否與每 command cursor file／directory durability 操作
   成線性關係？Publisher 能否在長 backlog 下維持穩定 drain rate？
5. 哪些根因已有足夠證據可另案設計 production 優化，哪些仍只能標示為瓶頸範圍？

### 3.2 假設與邊界

- 效能目標仍為單一交易對 1,000,000 commands/s。
- 正式結果只在 Linux Release build、相同 CPU set、相同 filesystem／device 上比較。
- 每個正式組合至少執行五次；CPU stage measured phase 至少 5 秒，I/O／worker stage 至少
  10 秒或足以觀察穩態。
- 五次結果使用 throughput median、run-to-run range、worst p99／p99.9；單次最佳值不作結論。
- 目前沒有 completion latency SLO，因此本階段不選擇新的 production default，也不使用
  「最大吞吐」代替最佳設定。
- `perf` 是可選的歸因證據；無權限時記錄限制，改用受控對照、`strace`、`iostat` 與
  `/usr/bin/time -v`，不能省略 baseline correctness。

## 4. 架構與模組邊界

分析沿用既有資料流，不建立第二套元件：

```text
order_books_benchmark
  -> engine_pipeline_ceiling stage
       -> StateMachine / validate_state / MetricsRegistry
       -> public Engine -> ShardRuntime -> Completion worker
       -> EventPublisher -> WAL reader -> EventSink -> cursor persistence

external runner/operator
  -> fixed environment and five-run matrix
  -> optional perf / strace / iostat / time
  -> raw output
  -> root-cause report and optimization decision
```

Production domain、WAL、Engine、Publisher、Completion worker 與 durability semantics 均不
修改。Benchmark 只負責建立同工作量控制組、輸出必要計數並驗證 correctness；外部工具負責
CPU、syscall 與 device 層歸因。

## 5. 必要修改

### 5.1 Metrics 加入 separate-registry 控制組

在 `benchmarks/pipeline_ceiling_benchmark.cpp` 的 metrics stage 保留現有：

```text
writer_only
writer_publisher_contended          # 兩 worker 共用一個 registry
```

並新增：

```text
writer_publisher_separate_registries
```

新 case 必須與 shared case 使用完全相同的：

- writer metric mix 與 Publisher metric mix；
- commands per worker、batch size、warmup、start barrier；
- 兩個 OS threads 與 wall-time completion boundary；
- `NullMetricsSink`；
- correctness count 驗證。

唯一差異是兩個 worker 各自持有一個 `MetricsRegistry`。因此：

```text
shared vs separate 差值 = shared registry contention 候選成本
writer_only vs separate = 增加第二 worker、不同 metric mix 與排程的成本
```

每個雙 worker case 除 aggregate observations/s 外，新增：

```text
writer_elapsed_ms
publisher_elapsed_ms
writer_commands_per_second
publisher_commands_per_second
```

worker elapsed 從共同 barrier release 到該 worker 完成 measured loop；aggregate elapsed 仍從
barrier release 到兩個 worker 都 join。不得相加兩個 worker elapsed 計算 throughput。

### 5.2 Runtime handoff 加入 producer-lane 控制

新增 benchmark-only CLI：

```text
--pipeline-producer-lanes=N
```

規則：

- 預設 `1024`，維持現有行為；
- 必須大於零，且不得大於 benchmark 使用的 ingress capacity `65,536`；
- 只影響 `runtime_handoff`（`all` 會傳入）；
- 不改 `RuntimeConfig` production default；
- summary 輸出實際 `producer_lanes=N`。

將目前固定的 `kProducerLanes` 改為由 `PipelineBenchmarkOptions` 傳入 `HandoffState`。每 lane
仍維持 single-in-flight，callback 後才能歸還，不能用同一 Producer stream 製造多筆 in-flight
來提高數字。

正式 lane matrix：

```text
1, 8, 64, 256, 1024
```

先固定 `engine-group-size=256`、`engine-group-delay-us=200` 掃 lanes。只有 throughput 已在某個
lane 數形成平台後，才在該 lane 數補測 group size `64／256／1024`；不執行沒有資訊增益的
完整二維排列。

### 5.3 不需要修改的項目

- `state_machine`：只作 CPU ceiling 對照。
- `invariant_validation`：既有 `--pipeline-active-orders` 和
  `--pipeline-batch-size` 已足夠；batch size 只影響攤提值，不需為同一 state 重跑所有 batch。
- `publisher_drain`：既有 `iterations × batch_size` 可控制 backlog，且已驗證 durable cursor；
  不新增 cursor-disable、mock cursor 或 test-only production API。
- `include/order_books/*`、`src/domain/*`、`src/persistence/*`、`src/runtime/*`。
- WAL／Snapshot format、Conan dependencies、production config、CI performance threshold。

## 6. CLI、資料與輸出契約

`PipelineBenchmarkOptions` 新增 `producer_lanes`。一般 benchmark options 加入對應欄位與
`pipeline_options_set` 處理；usage 與 README 僅補一個選項說明及一個 root-cause 範例。

Metrics 三個 case 必須使用不同 `case=` 值，且保留：

```text
correctness_verified=true
latency_scope=metric_group|concurrent_worker_group
contention_mode=single_worker|separate_registries_two_workers|shared_registry_two_workers
writer_metric_calls_per_command=11
publisher_metric_calls_per_command=5
```

任何 count mismatch、worker failure、timeout 或 arithmetic overflow 都不得輸出成功 summary。
Runtime handoff 必須繼續驗證 queued count、exactly-once callback、identity、預期 admission error
與 producer stream ordering。

本階段沒有新的 domain data model。新增資料只存在於 benchmark options、worker timing result
與文字輸出，不進入 public library ABI 或持久化格式。

## 7. 正式根因分析方法

### 7.1 共通環境

每次報告至少記錄：

```text
source revision
compiler and flags
CPU model / fixed CPU set / governor
kernel
filesystem and mount options
block device
perf_event_paranoid
benchmark command
raw successful and failed outputs
```

使用 Release build；sanitizer 只做 correctness。CPU affinity 由外部 `taskset` 管理，CPU stage
使用固定 core，雙 worker／Engine／Publisher 使用固定且足夠的 CPU set。不得把不同 CPU set
的結果直接計算 uplift。

### 7.2 Invariant validation

固定 batch size 256，依序測：

```text
active orders: 0, 1,000, 10,000, 100,000
repetitions:   5 per point
duration:      >= 5 seconds measured per run
```

報告 validation median／worst p99／p99.9、validations/s，以及 `validation_time / 256` 的
amortized ns/command。其他 batch size 的攤提值由同一 validation cost 計算，不重複執行相同
工作。

只有 state size 增加時成本趨勢可重現，且攤提成本在目標 writer budget 中占主要比例，才把
完整 invariant scan 列為已確認的 production 優化候選。短於 5 秒或只有單一 state size 的
結果只能標為初步觀察。

### 7.3 Metrics

固定 batch size 256，使每 case measured phase 至少 5 秒，對以下三組各執行五次：

```text
writer_only
writer_publisher_separate_registries
writer_publisher_contended
```

判讀：

- shared 明顯低於 separate，且五次 run range 不重疊：確認共享 registry contention。
- shared 與 separate 接近、兩者都低：主要成本是每次 observe 的 lookup／histogram／sink
  呼叫，而不是跨 worker lock contention。
- 只有其中一個 worker rate 低：後續優先分析該 worker 的 metric mix，不用 aggregate rate
  掩蓋不平衡。

「明顯」用 median 至少 20% 差異且五次 range 不重疊作為本報告的實驗判定門檻；它是根因
歸因門檻，不是 CI performance gate。

若 `perf` 可用，再以相同 command 執行一次 `perf record -g`，確認 call stack 是否集中於
`MetricsRegistry::observe`、mutex 或 unordered-map；profiler run 不參與 throughput uplift。

### 7.4 Runtime handoff／Completion

依 5.2 的 lane matrix 執行，每點至少五次且 measured phase 至少 10 秒，記錄：

- callbacks/s、p50／p99／p99.9／max；
- voluntary／involuntary context switches 與 user／system CPU time；
- `strace -f -c -e trace=futex` 的 futex calls／time；
- 可用時的 `perf stat` 與 `perf record -g`。

判讀：

- lanes 增加時 throughput 上升後形成平台、latency 持續上升：確認合併 worker／queue capacity
  ceiling，不是 producer 供給不足。
- 單 lane 已接近平台：固定 per-command dispatch／callback 成本是主要候選。
- futex、context switch 或 call stack 證據集中於 completion queue：才另案評估 batch drain／
  queue implementation。
- 若外部工具無法把 ingress、writer admission 與 Completion worker 分開，本階段必須明確
  報告「合併 ceiling 已確認、內部根因未確認」，不能由 source inspection 宣稱百分比。

### 7.5 Publisher drain

在部署目標 filesystem 建立新的空 data directory，先用小 backlog 驗證，再以可讓 measured
drain 至少持續 10 秒的 backlog 執行五次。另選一個代表組合執行：

```bash
/usr/bin/time -v <publisher command>
strace -f -c -e trace=openat,write,close,fsync,fdatasync,rename <publisher command>
iostat -xz 1
```

`strace` run 只分析 syscall shape，不與正常 baseline 比 throughput。報告至少將 `rename`、
`fsync/fdatasync` 與 commands 數正規化，並說明 fixture 建置也在 process lifetime 內，不能將
所有 open／write 都歸因於 measured drain。

判讀：

- backlog 增加時 drain time 近似線性、rate 穩定，表示取得 sustainable ceiling；短 burst
  startup cost 不再主導。
- cursor-related rename 約為每 command 一次，且 sync／device wait 證據隨 command 數成長：
  逐筆 cursor durability 是已支持的主要候選。
- 即使 syscall 證據支持 cursor 成本，真正的 causal uplift 仍需下一份 cursor group
  persistence 設計完成後，用相同 workload 作 before／after 才算最終確認。
- Publisher drain rate 低於 durable writer rate時，端到端系統不得宣稱該 writer rate 可持續。

## 8. 錯誤處理與測試策略

### 8.1 自動化 correctness

- GCC／Clang Release build，warnings-as-errors。
- 現有 GoogleTest 全部通過。
- ASan／UBSan 通過；sanitizer 結果不作效能比較。
- pipeline `all` smoke 使用 `--pipeline-producer-lanes=8`，確認新 CLI 傳遞與三個 metrics case
  都輸出 `correctness_verified=true`。
- CLI 的 lane `0`、非數字及大於 `65,536` 必須以 exit code 2 和 machine-readable CLI error
  拒絕。
- shared／separate registry case 都在輸出 summary 前驗證兩側 observation counts。

不新增重複測試 production Engine／Publisher semantics 的 unit test；既有 integration tests
繼續負責 exactly-once completion 與 durable cursor contract。

### 8.2 正式效能結果有效性

以下任一情況使該 run 無效，但保留 raw log：

- `correctness_verified` 不是 true；
- timeout、worker failure、cursor mismatch 或 queue rejection；
- measured duration 未達本文件下限；
- filesystem case 重用非空 data directory；
- CPU affinity、build type 或 source revision 與同組其他 run 不同；
- 把 profiler／strace run 混入未插樁 baseline median。

工具不可用時記錄 `unavailable` 與原因，不修改 kernel security policy，也不以猜測補上指標。

## 9. 根因確認與後續優化門檻

根因報告必須將結論分為：

```text
confirmed ceiling          # 長測可重現的 stage capacity
supported cause            # 有受控對照或 syscall/profile 證據
unconfirmed hypothesis     # 只有 source inspection 或單次 smoke
```

後續 production 優化只在 supported cause 以上另案設計：

- invariant scaling：設計保持完整 correctness 的 incremental validation；
- metrics shared contention：評估 typed metric IDs、writer ownership 或分片 aggregation；
- Completion queue ceiling：評估 batch drain，不能直接跳到 lock-free queue；
- cursor persistence：設計 batch size、max interval、confirmed／durable cursor 語意、crash
  duplicate window、retention watermark 與 final flush。

沒有 completion latency SLO 前，只能產出 throughput／latency Pareto frontier，保留現有
production defaults；本階段不得宣稱任何參數是「最佳」。

## 10. 檔案修改清單與預估

必要修改：

```text
benchmarks/pipeline_ceiling_benchmark.hpp       2～5 行
benchmarks/pipeline_ceiling_benchmark.cpp      80～140 行
benchmarks/order_book_benchmark.cpp            15～30 行
README.md                                       8～15 行
docs/order-book-design.md                       8～15 行
.github/workflows/ci.yml                         1～4 行
```

預估 production code 變更為 `0` 行，總修改約 `114～209` 行，不含本設計與後續實測報告。
若實作需要修改 `src/`、加入通用 benchmark framework、增加 duration／JSON subsystem，或開始
實作被測元件優化，應停止並重新審查範圍。

## 11. 已知限制與擴充方向

- Producer-lane sweep仍只能定位 runtime handoff 合併 ceiling；沒有 profiler 證據時不能精確
  分配 ingress、writer與Completion worker百分比。
- Immediate-success sink不代表真實 broker；本階段只分析本機 Publisher replay與cursor I/O。
- `strace`會放大 syscall latency，只能分析形狀與次數。
- `iostat`是device-level統計，若裝置同時承載其他工作，必須標記結果受污染。
- 單一 crossing fixture不涵蓋multi-match event fan-out；目前需求只要求單一交易對既有基準，
  不擴充 workload matrix。
- 若完成本文件後仍無法在不修改production hot path的情況下區分runtime內部成本，再另案設計
  可關閉且預設停用的低成本診斷，而不是在本階段預先加入。

## 12. 設計與實作一致性規則

- 實作只加入5.1與5.2的兩個控制，不順手優化被測元件。
- shared與separate metrics case除registry ownership外必須完全相同。
- producer lanes只控制並行producer streams，不改single-in-flight contract。
- 所有throughput使用measured wall elapsed；worker elapsed不得相加。
- Publisher成功邊界仍是stop/join後reopen驗證durable cursor等於head。
- smoke、baseline、profile三種結果必須分開命名與報告。
- 實作若發現上述控制不足，先更新本文件說明原始設計、問題、最小調整與影響，再修改程式。

## 13. Definition of Done

- 新metrics separate-registry case可執行，counts、worker rate與aggregate rate語意正確。
- `--pipeline-producer-lanes`可設定、驗證並出現在runtime summary，預設行為維持1,024 lanes。
- production source、public API、持久化格式、durability與ordering均未改變。
- correctness smoke、完整GoogleTest、Release warnings-as-errors與ASan／UBSan通過。
- 四個不足stage完成本文件規定的五輪長測或清楚記錄工具／環境阻礙。
- 根因報告對每項標示confirmed ceiling、supported cause或unconfirmed hypothesis，不把短測當
  穩態結論。
- 後續優化項目由證據觸發，且不在本階段選擇新的production預設值。
