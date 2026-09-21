# Engine Writer 統一 ceiling／根因分析設計 Review

## 1. Review 對象與結論

本文件 review：

- `docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-design.md`；
- `docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-report.md`；
- `docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-overhead-retest-report.md`；
- 現有 `engine_pipeline_ceiling`、`wal_write_ceiling`、
  `engine_writer_hot_path_profile` 與 `engine_durable_single_instrument` benchmark。

原設計若目標只是在既有 Writer benchmark 中區分 thread role 與 sync tail，內容大致完整；但它不再符合
目前需求：一次取得單 instrument／單 shard Engine Writer 同步路徑的 sustainable ceiling，並以 CPU
profile、階段時間、I/O 與 storage baseline 提供可驗證的瓶頸證據。原設計有四個必要修正：

1. 不再把自行解析 `/proc` 與逐 group CSV telemetry 當成主要歸因工具；前次 B/C 已出現 31.01%
   throughput bias，該組資料無法作 production 歸因。
2. 不再為每個新疑問建立一個 C++ benchmark。現有 workload 已覆蓋 StateMachine、invariant、metrics、
   runtime handoff、WAL append／sync、Writer phase 與完整 durable Engine，應由單一外部 campaign 組合。
3. 將 `perf stat`／`perf record`、`mpstat`／`iostat` 與相同 durability pattern 的 file-backed `fio`
   納入同一次診斷；若 benchmark host 不允許 CPU profiling，應標記環境不合格，而不是再加入侵入式
   production-side telemetry 代替。
4. 清除只服務已失敗診斷方案的程式與中間文件；保留仍能持續用於 profiling、correctness 或
   regression 的最小設施。

這是 benchmark／診斷與清理需求，不修改 matching、WAL format、durability、group-commit、publisher、
completion 或 recovery 語意，也不承諾本次直接達到 1M commands/s。

## 2. 需求理解與合理假設

### 2.1 本次必須回答

1. 目前單 instrument、單 shard 的 Engine Writer sustainable commands/s ceiling 是多少？
2. ceiling 隨 group size／delay 的 throughput、p50、p99、p99.9 與 max frontier 如何變化？
3. Writer 同步路徑中 admission、WAL prepare、append/write、fsync、StateMachine apply、validation、
   metrics／post-apply、publisher notify 與 completion enqueue 各占多少？
4. 主要限制是單 Writer CPU、prepare helper CPU、metrics、storage write bandwidth、durable flush rate、
   scheduler contention，還是尚未歸因？
5. Engine 實際 WAL bytes/s 與 fsync/s，相對於裝置宣告上限及同 path、同 durability pattern 的
   empirical storage ceiling 有多少利用率？
6. 現有 benchmark-only diagnostics 是否提供不可替代的證據；若沒有，應移除哪些？

### 2.2 Scope

In scope：

- Linux 專用效能 campaign；
- 單 instrument、單 shard、crossing pair workload；
- Writer 同步路徑以及 async workers 對 Writer 的 CPU／scheduler 干擾；
- 既有 benchmark 的統一執行、artifact identity、環境 gate、raw evidence 與一份結論報告；
- 移除本輪未證明有效且可由 OS 工具取代的診斷碼；
- 保留既有 correctness、replay 與 durability gate。

Out of scope：

- Publisher／Completion worker 的演算法或 batching 優化；
- production defaults、thread priority／affinity、I/O scheduler、filesystem 或 kernel tuning；
- 新 public API、WAL format、第三方 C++ dependency或新的 matching workload；
- 將觀測到的 ceiling 宣稱為 production SLA；
- 直接以 raw block device 執行破壞性 storage benchmark。

### 2.3 假設與邊界

- 1M commands/s 是容量目標，不是需求規格目前承諾的 pass/fail gate；本次先提供差距與根因。
- 正式數據必須在 Linux、專用或可隔離 host、可識別 WAL block device 的環境取得。
- benchmark CPU 與 observer CPU 必須分離；所有比較使用相同 binary hash、CPU policy、filesystem、
  mount options 與 workload seed。
- `perf_event_paranoid`／capability 必須允許取得 cycles、instructions 與 call graph；不可用時結果為
  `environment-blocked`，不得以自行加入 hot-path logging 取代。
- 裝置 datasheet bandwidth／IOPS只作 sanity bound。對 group fsync 更重要的是相同 filesystem、buffered
  write、write size與每 group `fsync` 的 empirical ceiling。
- async workers本輪不要求 1M/s，但 measured window 內 Publisher／Completion backlog不得無界增加；
  否則完整 Engine 數字不是 sustainable Writer ceiling。

## 3. 為何原設計不足

原設計刻意只回答 Writer-only guardrail，並明確排除 `perf` 權限調整、完整 Engine、direct WAL 重測、
group-size 調校與 storage-path baseline。這些限制與原目標一致，但無法回答目前需求。

現有 Writer diagnostics 又把兩類工作綁在同一 case：

- measured phase 前後兩次 `/proc/self/task` snapshot；
- 每個 measured WAL group 的 `wal_sync_latency_us` 與 `wal_group_commands` 記錄。

其中 `EngineTailTelemetry::observe()` 在每次 metric 上讀時鐘、取得 mutex 並寫入 vector。B/C 只證明整組
diagnostics 與未受控環境存在 31.01% bias，無法判定是 snapshot、tail collector或host noise。繼續為它
增加第四種 case只會擴大矩陣，不能提高 production 根因可信度。

另一方面，現有 benchmark 已有必要 component：

| 既有 workload／stage | 可回答的問題 |
| --- | --- |
| `engine_pipeline_ceiling/state_machine` | matching／apply CPU ceiling |
| `engine_pipeline_ceiling/invariant_validation` | full validation ceiling |
| `engine_pipeline_ceiling/metrics` | registry單獨、分離與 contention成本 |
| `engine_pipeline_ceiling/runtime_handoff` | ingress／admission／completion handoff ceiling |
| `wal_write_ceiling` | WAL append-return與per-group fsync ceiling |
| `engine_writer_hot_path_profile` | Writer主執行緒非重疊phase accounting與WAL subphase |
| `engine_durable_single_instrument` | 公開 Engine end-to-end durable throughput／latency |

所以必要工作是編排與外部證據，不是新增另一套 production instrumentation。

## 4. 診斷架構與模組邊界

```text
campaign runner（repository script）
  ├─ identity/environment/preflight gate
  ├─ existing benchmark binary
  │    ├─ component ceilings
  │    ├─ WAL append/durable ceilings
  │    ├─ sampled Writer phase accounting
  │    └─ full durable Engine frontier
  ├─ low-overhead observers（獨立 CPU）
  │    ├─ perf stat
  │    ├─ mpstat
  │    └─ iostat
  ├─ separate attribution runs
  │    └─ perf record/report
  ├─ storage control
  │    └─ file-backed fio on the same filesystem
  └─ run root outside repository
       ├─ manifest
       ├─ raw logs
       ├─ normalized rows
       └─ final report inputs
```

正式 ceiling run只使用已通過 overhead gate 的 low-overhead observers，且 EngineTailTelemetry 預設關閉，
避免 benchmark-side collector 污染 production-like ceiling。`perf record`、tail telemetry attribution 與
fio各自在獨立 attribution/control run執行，不把其絕對RPS混入正式ceiling median。

Production library不負責啟動perf、iostat、fio或寫CSV。campaign artifacts不得寫入source tree，也不得
改變Git staging。

## 5. Ceiling 與證據模型

### 5.1 Ceiling 定義

`observed sustainable ceiling` 是預先定義矩陣中：

- correctness、replay及durability全部通過；
- measured duration至少30秒；
- 無submit error、timeout、telemetry loss或counter regression；
- Publisher／Completion backlog在drain後歸零，且measured window沒有持續線性成長；
- 重複輪次median穩定；
- 由completed commands計算的最高median commands/s。

同時報告Writer service RPS與completed RPS；兩者不可互相替代。若Writer service快但Completion無法消化，
只能稱為Writer瞬時ceiling，不得稱為sustainable Engine ceiling。

正式ceiling使用tail-off Engine case；tail-on僅在候選case執行三組off/on attribution。tail-on相對tail-off
的RPS或total CPU seconds/M commands bias超過3%時，tail資料只能定性使用，且tail-on RPS不得納入ceiling
median。tail attribution須同時檢查dropped samples、measured backlog slope與drain終值。

### 5.2 理論與實測上限

報告至少計算：

```text
cpu_writer_ceiling_rps = 1e9 / writer_cpu_ns_per_command
storage_bandwidth_ceiling_rps = effective_write_bytes_per_second / wal_bytes_per_command
storage_flush_ceiling_rps = effective_durable_flushes_per_second * commands_per_fsync
```

WAL可達上限取上述與prepare helper capacity中的最小值。CPU helper為平行工作，不能把所有thread CPU
time直接加總成Writer wall time；主執行緒非重疊phase用於accounting，per-thread CPU用於資源需求。

`effective_*`優先使用相同path與durability pattern的fio結果；vendor或cloud provisioned limit另列來源與
日期。若無權威裝置上限，欄位記為`unknown`，不得自行猜值。

### 5.3 根因證據標準

任何primary bottleneck至少需要兩種獨立證據，且跨多數重複輪次同方向：

| 分類 | 必要證據 |
| --- | --- |
| Writer CPU | Writer CPU接近單core容量；perf hotspot與phase accounting指向同一區域；storage未接近effective ceiling |
| prepare CPU | helper CPU飽和或runqueue增加；prepare phase及perf hotspot一致；增加既有lane control才有預期方向 |
| metrics | perf hotspot落在MetricsRegistry／map／mutex；metrics component ceiling及Writer post-apply占比一致 |
| storage bandwidth | 實際bytes/s接近same-path write ceiling；append/write phase主導；flush rate仍有餘裕 |
| durable flush | sync wall share與tail主導；commands/fsync × empirical flush/s可預測Engine ceiling |
| scheduler／host | runqueue、migration或host activity與慢輪共變，但CPU／storage isolated ceiling無同樣限制 |

只有單一phase timer、單一perf sample、device util或datasheet數字時，結論必須是`inconclusive`。

## 6. 統一測試矩陣

### 6.1 Artifact與環境 gate

1. ReleaseBenchmark binary完成Debug、ASan／UBSan、Release CTest與小型smoke後凍結SHA-256。
2. 記錄commit、cached/worktree diff hash、compiler、flags、OS、kernel、CPU topology、NUMA、governor、
   EPP、boost、IRQ、filesystem、mount、block topology、device model、scheduler及storage limit來源。
3. 每組pair前執行固定30秒preflight；CPU idle、iowait、device util與queue超限即停止整次campaign。
4. preflight不得反覆執行直到偶然通過；環境失敗後必須移除外部工作並建立新run root。

門檻沿用已定義的idle gate；若專用硬體特性需要調整，只能在正式執行前寫入procedure，不能看到結果後
修改。

### 6.2 外部 observer overhead gate

使用完整durable Engine固定case，以`off,on / on,off / off,on`交錯執行observer off/on各三輪。on包括正式
run會使用的`perf stat`、`mpstat`與`iostat`，observer固定在非benchmark CPU。CPU成本使用user+system
seconds/M completed commands。若median throughput或CPU seconds/M commands bias
任一超過3%，停止並降低observer頻率；不得進入正式矩陣。

### 6.3 Component與WAL matrix

- `state_machine`、`invariant_validation`、`metrics`及`runtime_handoff`各執行五輪；
- `wal_write_ceiling`以group size 256、1,024、4,096、8,192分別執行`sync=none`與
  `sync=per_group`，固定現行production candidate的prepare lane設定；
- 每輪輸出commands/s、CPU seconds/M commands、bytes/command、bytes/s、writes/group、fsync/s與
  latency percentiles；
- correctness、WAL reopen及replay仍是必要gate。

Publisher drain只作async interference/reference，不列為本輪優化目標。

### 6.4 Full Engine frontier

固定單instrument／單shard／crossing pair／producer lanes 8,192／W=2，掃描：

```text
group size   256, 1,024, 4,096, 8,192
group delay  200 us, 1,000 us
```

正式矩陣前對八個case各執行一次observer-off、tail-off calibration，以固定40秒目標及10%安全係數
產生`engine-iteration-plan.tsv`。calibration不列入ceiling、latency或observer median；若plan預估時間
超過case timeout budget，正式矩陣在開始前停止。

八個case先各三輪形成frontier；先排除CV超過5%、duration不足、artifact不完整或counter無法驗證的case，
再依completed RPS median由高到低執行tail off/on sustainability gate。第一個通過者才是observed
sustainable ceiling。production default、既有g4096/d1000 case與該case再各確認五輪。若兩個身份重合只
執行一次，不補造重複case。因需求尚未定義可接受latency，本輪只報Pareto frontier，不自行改production
default。

Runner 對每輪使用 frozen formal iteration plan，要求 `commands == formal_iterations * 2`、
`wal_group_commands == commands`、`wal_group_commits > 0`、`wal_bytes_delta > 0` 且 measured duration
至少 30 秒；缺少 status、GNU time、normalized row 或 observer artifact 時，以明確 reason 排除。校準
另以十進位字串精確驗證 `calibration_commands == calibration_iterations * 2`，formal iterations 使用向上
取整，且正式 command count 必須通過 uint64 overflow check；不把校準列入任何正式 median。duration 不足
會排除該 case，不會在 selection 前中止 frontier。`frontier-selection.tsv` 僅保存固定六欄的每 case
valid/excluded row，`frontier-selection.txt` 另保存 tail rejection、selected sustainable case，以及
default、g4096/d1000 與 selected case 的唯一 tail comparison；rejected candidate 的
`tail-gate-<case>.summary` 與 validation artifact 不會被後續 candidate 覆寫，已執行的 tail attribution
會由 comparison 重用而不重跑。tail attribution wrapper 在所有 return path 恢復原本 observer mode；
backlog/drain/slope 與 bias rejection 使用不同 status，malformed numeric CSV、artifact 或解析錯誤則終止
為 `invalid-run`。

### 6.5 Writer phase與CPU profile

- 對production default、g4096/d1000與ceiling case執行profile-off／sampled-profile-on配對；iterations
  使用同一份前置 calibration plan；
- sampled profile只保留既有非重疊Writer phases與WAL subphases，不開啟逐group tail CSV；
- profile bias超過3%時，phase數字只能作定性輔助，不能作百分比歸因；
- 對同三個case執行獨立`perf stat`；
- 對ceiling case執行至少一輪獨立`perf record -g`並保存`perf report --stdio`；
- profiling artifact沿用ReleaseBenchmark frame-pointer設定，記錄binary hash，不將profile run的RPS併入
  ceiling median。

### 6.6 Storage control

fio只可對run root內新建、已驗證不含使用者資料的檔案執行，不可指定raw block device。設定須匹配：

- 相同filesystem與mount；
- buffered sequential write；
- 由WAL實測bytes/group得到的write size；
- queue depth 1；
- 每次group write後fsync；
- 足夠duration與file size避免只量到短暫cache burst。

至少覆蓋production default、g4096/d1000與ceiling case的bytes/group。另可記錄純sequential write
bandwidth作背景資料，但它不能取代fsync-matched ceiling。

## 7. 必要程式調整與清理

### 7.1 保留

- `src/support/thread_name.*`及Writer、prepare、publisher、completion thread-entry命名：一次性成本，
  可讓perf／pidstat對應role，不在hot path。
- 既有sampled `WriterProfileCollector`、`WalAppendProfile`與profile-off正常路徑。
- `EngineTailTelemetry`供既有durable Engine tail benchmark使用。
- `EngineTailTelemetry`容量邊界防護與對應unit test。
- 既有`engine_pipeline_ceiling`、`wal_write_ceiling`、Writer與durable Engine workloads。
- production `MetricsRegistry`功能：需求規格明確要求operational observability；本輪只量測成本。

### 7.2 移除

以下只服務已失敗的Writer B/C diagnostics，且可由外部工具取代：

- `benchmarks/thread_resource_telemetry.cpp`；
- `benchmarks/thread_resource_telemetry.hpp`；
- `tests/unit/thread_resource_telemetry_test.cpp`；
- `benchmarks/check_writer_diagnostics_output.cmake`；
- `benchmarks/check_writer_thread_diagnostics.cmake`；
- `--writer-thread-diagnostics`及`--writer-tail-telemetry-output` CLI；
- `WriterProfileBenchmarkOptions`中的對應欄位；
- Writer workload中的procfs snapshot、role completeness、tail collector、CSV與summary路徑；
- 對應CMake source／test wiring。

不得刪除整個`EngineTailTelemetry`，因為它仍服務既有durable Engine benchmark。不得因metrics可能昂貴就
刪除需求規格要求的production metrics；若統一campaign證明metrics為primary bottleneck，再另立最小
typed／shard-local／per-group aggregation優化設計。

### 7.3 文件收斂

在本review已保存必要事實且清理完成後，下列尚未形成有效production歸因的中間文件不應作為公開專案的
長期主要文件：

- 已由本review取代的Writer tail root-cause design；
- Writer tail staged-review fixes；
- instrumentation-biased benchmark procedure/report；
- preflight-busy overhead-retest procedure/report。

實作時可移除這些pending中間文件，或移至明確的historical evidence目錄；不可讓讀者誤認為其中數字是
accepted baseline。長期只保留本review、統一campaign procedure與一份最終report。

## 8. Campaign runner責任

新增一個repository-owned runner，例如：

```text
benchmarks/run_engine_writer_diagnostics.sh
```

它只負責：工具與權限precheck、identity capture、建立repository外run root、依固定matrix執行、監測
process lifecycle、保存exit status與raw logs、檢查完整性。它不得：

- 編輯production config或source；
- 自動改governor、sysctl、scheduler或mount；
- 自動安裝工具或以sudo提升權限；
- 依中途結果改變正式case；
- 刪除失敗或慢輪；
- 自動宣稱root cause。

runner使用`set -euo pipefail`、每輪唯一data directory、明確timeout與trap清理observer。報告計算可使用
POSIX工具與已列為procedure prerequisite的`jq`；不新增production dependency。

## 9. 錯誤與結果分類

整次campaign只允許以下terminal classification：

| result | 條件 |
| --- | --- |
| `environment-blocked` | perf權限、CPU隔離、storage identity或必要工具不符合 |
| `preflight-busy` | 任一正式pair前的idle gate失敗 |
| `observer-biased` | 外部observer overhead超過3% |
| `invalid-run` | correctness、replay、durability、timeout、counter或artifact identity失敗 |
| `environment-unstable` | 有效輪次跨round變異超過預定門檻且與host/storage activity一致 |
| `valid-attribution` | ceiling有效且primary bottleneck符合兩種獨立證據 |
| `inconclusive-attribution` | ceiling可量，但沒有候選符合根因證據標準 |

任何一種blocked／invalid結果都不得轉譯為production regression或瓶頸。

## 10. 測試與驗證策略

### 10.1 清理後build/test

- Debug CTest；
- ASan／UBSan CTest；
- ReleaseBenchmark build及既有benchmark smoke；
- Writer profile off/on、WAL ceiling、pipeline stage與durable Engine CLI tests；
- macOS仍可編譯正常library/test target，Linux-only runner不加入預設build。

刪除Writer diagnostics後，必須確認不存在殘留CLI help、CMake test、source include或stdout schema期待。

### 10.2 Runner tests

不以CI執行長時間效能矩陣。只需shell syntax check及可注入fake command的小型測試，覆蓋：

- preflight fail會停止且不啟動benchmark；
- child／observer exit會被wait並記錄；
- timeout與non-zero status分類；
- run root不可位於repository；
- artifact hash改變會停止；
- 不執行任何Git index mutation。

Contract test 以 `ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1` 接受 repository 外 fake PATH 與縮短的
duration／round／case override；未啟用 test mode 時，任何 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` override
都在建立 run root 前拒絕。fake test 可為 observer/tail bias 使用只限 test mode 的 acceptance limit，
正式 runner 仍固定 3%。測試需驗證 exact calibration count/ceil、frontier counter/artifact rejection、
tail candidate fallback／drain／positive-slope／bias summary、exact perf event list、每個 failure 的
result/reason/invocation contract、machine-readable environment-blocked，以及 observer process 的
PID/status/wait evidence；不啟動真實 perf、fio 或長時間 campaign。

### 10.3 數據有效性

- frontier scan每case三輪；root-cause與confirmatory case五輪；
- case順序固定交錯並在round間反轉，避免永遠讓同一case承受thermal／storage drift；
- median RPS的CV或robust spread超過5%時不作root-cause判定；
- sampled Writer非重疊phase合計應解釋至少90% writer service wall time；不足部分明列
  `unaccounted`，不得分配給任意phase；
- perf sample、phase share與component/full ceiling使用相同command mix，但各自結果保持不同語意。

## 11. 最終報告必要內容

1. artifact與environment manifest；
2. preflight與observer overhead gate；
3. component、WAL及full Engine ceiling表；
4. group size／delay throughput-latency Pareto frontier；
5. Writer phase accounting與unaccounted share；
6. perf stat、top call stacks及per-thread CPU；
7. Engine實際WAL bytes/s、fsync/s、commands/fsync；
8. device declared limit、same-path fio result及Engine/fio ratio；
9. async backlog是否bounded；
10. 一項terminal classification與支持／反證；
11. 下一個production change，或明確寫不應修改production。

報告不得用單輪max RPS當ceiling，不得把vendor sequential bandwidth直接當fsync ceiling，也不得將
component microbenchmark RPS稱為end-to-end Engine RPS。

## 12. 關鍵決策與取捨

- 使用外部OS profiler而非再造scheduler telemetry：可取得call graph與hardware counters，且不污染
  production hot path；代價是benchmark host必須提供權限。
- 保留sampled phase profile：perf能指出函式成本，但無法單獨表達group-commit語意邊界；兩者互補。
- fio使用普通檔案而非raw device：結果包含filesystem語意且不破壞資料，更接近目前WAL path；不宣稱是
  裝置裸硬體極限。
- 不在本輪優化async workers：只要求其backlog bounded並監控contention，維持當前Engine範圍。
- 不刪production metrics：它是明確需求；先量化成本，再以證據決定是否改為typed/per-group更新。
- 一個campaign仍包含多個受控run；「一次診斷」表示一次凍結artifact、一次預定矩陣、一份報告，不表示
  把所有侵入式工具同時掛在同一process。

## 13. 已知限制與可擴充方向

- shared host若持續無法通過preflight，必須改用專用host／storage；降低門檻不是解法。
- fio無法代表device firmware所有tail行為，仍須與Engine sync latency及block-device時序交叉驗證。
- CPU profile可定位目前primary hotspot；修正後可能顯露下一個瓶頸，仍需一次confirmatory rerun，但不需
  再設計新的監測架構。
- 本輪只有crossing pair；其他order type與large-book workload屬容量／業務workload驗證，不應塞入本次
  Writer根因分析。
- 如果metrics成為primary bottleneck，後續設計必須維持`Order-book-spec.md`第17節的可觀測性契約。

## 14. 必要修改清單與預估規模

| 區域 | 修改 | 預估變動 |
| --- | --- | ---: |
| Writer diagnostics CLI／benchmark path | 移除procfs與逐group tail整合 | 刪除120--190行 |
| procfs helper、tests、CMake checks | 完整移除 | 刪除520--650行 |
| thread naming | 保留現有helper與thread-entry calls | 0--10行調整 |
| `EngineTailTelemetry`容量防護 | 保留既有fix與test | 0行 |
| campaign runner | 固定矩陣、calibration、tail attribution、validity gate、preflight與artifact capture | 新增450--520行 |
| 統一procedure | 前置條件、命令入口、calibration、gate與報告schema | 新增200--240行 |
| 中間診斷文件 | 移除或移入historical evidence | 刪除約2,287行 |

預期production演算法修改為0行；production source只保留約50行thread naming設施。實作若需要新增新的
WAL API、EngineConfig、queue、worker或hot-path telemetry，代表超出本設計，必須先重新review。

## 15. 完成定義

- 已移除未證明有效的Writer procfs／逐group telemetry整合，沒有殘留build或CLI路徑；
- thread naming、sampled phase profile及既有component benchmarks保留且測試通過；
- 統一runner不修改source、production設定或Git index，且失敗時完整清理child processes；
- dedicated Linux環境可用一個命令產生完整run root；
- report同時包含ceiling、latency frontier、CPU profile、I/O、fio與phase accounting；
- bottleneck結論符合兩種獨立證據，否則明確標為`inconclusive-attribution`；
- 沒有因本次診斷直接改動production defaults或宣稱達成尚未定義的SLO；
- 後續只需針對已證明的primary bottleneck設計最小production修正，並用相同campaign做一次
  confirmatory rerun。
