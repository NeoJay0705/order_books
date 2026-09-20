# WAL append publish bookkeeping：Writer tail／排程根因分析設計

## 1. Review 結論

本設計承接：

- `docs/wal-append-publish-bookkeeping-optimization-design.md`；
- `docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md`；
- `docs/wal-append-publish-bookkeeping-optimization-confirmatory-benchmark-report.md`。

既有兩輪結果已足以確認 per-chunk publish bookkeeping 的局部收益，但不足以解釋 Writer g4096
profile-on workload 的 p99／p99.9 與 context-switch guardrail failure：

- confirmatory Writer throughput 提升 6.05%，`wal_publish_ns_per_command` 降低 81.70%，CPU
  seconds/M commands 降低 4.52%；
- Writer p99.9 增加 9.89%，process-wide involuntary context switches/M commands 增加 15.07%；
- authoritative Engine throughput提升 7.41%，p99.9降低0.74%，process-wide involuntary context
  switches降低5.91%，沒有重現相同退化；
- `perf_event_paranoid=4` 使 `perf` counters不可用；現有 `/usr/bin/time -v` 數值是整個process總量，
  不能稱為writer thread的context switches；
- Writer workload目前沒有保留逐group sync tail，無法區分scheduler runnable wait與storage blocking。

因此下一階段是**診斷能力修改加受控重測**，不是再修改WAL演算法。最小必要修改為：

1. 為benchmark涉及的長生命週期thread設定穩定role name；
2. 在Writer benchmark量測期前後讀取Linux `/proc/self/task`，輸出各role的CPU、runqueue wait、
   context switches與migration delta；
3. Writer診斷模式重用既有tail telemetry，保存量測期內每個group的WAL sync latency；
4. 用profile-off／profile-on／diagnostics-on三個case比較相同baseline與candidate。

本階段不得修改per-chunk bookkeeping、WAL format、fsync policy、queue、worker數量或喚醒策略。只有完成
歸因後，才能另立production修正設計。如此可使設計、實作與驗收對「量到什麼」及「尚未證明什麼」有
一致認知。

## 2. 需求理解與合理假設

### 2.1 必須回答的問題

本階段只回答下列問題：

1. Writer workload的process-wide involuntary context-switch增加，實際由load-generator、writer、
   WAL prepare、publisher或completion哪一個thread role貢獻？
2. 退化是否只出現在`--writer-phase-profile=on`，亦即屬於profile instrumentation interaction？
3. writer thread的runqueue wait／migration是否與p99.9惡化同方向？
4. WAL sync p99／p99.9／max或慢sync次數是否與慢輪同時上升？
5. 若上述訊號都不一致，現有資料是否只能分類為host noise／尚未歸因，而不能據此修改production？

### 2.2 必須維持的限制

- baseline與candidate仍只以目標`src/persistence/wal.cpp` production diff區分；診斷程式碼兩邊相同。
- 使用既有g4096、W=2、group delay 1000 us、producer lanes 8192與per-group fsync workload。
- 不改變command、WAL、durability、matching、publisher、completion與recovery語意。
- 診斷預設關閉；關閉時不得讀取`/proc`、建立sampler thread、配置sample buffer或輸出診斷欄位。
- 診斷結果不是production SLO或CI固定RPS gate。
- 保留所有correctness有效的慢輪，不因storage或scheduler outlier排除。
- 不改變Git staging；benchmark artifacts寫在repository外。

### 2.3 合理假設

- 正式效能歸因在Linux部署環境執行；macOS只需維持預設關閉模式可編譯及一般測試可執行。
- Linux procfs提供`/proc/self/task/<tid>/status`與`schedstat`；`sched`中的migration欄位若kernel未提供，
  可明確標成`not measured`，不得填0。
- thread resource delta只需在warmup完成後與measured phase完成後各取一次快照；高頻polling會新增
  scheduler干擾，不是本階段必要條件。
- 既有`EngineTailTelemetry`已能以預先配置的bounded storage收集`wal_sync_latency_us`與
  `wal_group_commands`；Writer模式不需另造同功能telemetry framework。

## 3. 已確認證據與目前缺口

| 項目 | 已確認 | 尚未確認 |
| --- | --- | --- |
| publish bookkeeping | publish及append phase明顯下降 | 不是tail regression的直接證明 |
| authoritative Engine | throughput與主要tail gate通過 | 不能解釋Writer-only退化 |
| `/usr/bin/time -v` | Writer workload process總context switches增加 | 哪個thread role增加 |
| Writer phase profile | sampled phase totals可用 | profile本身是否改變排程結果 |
| WAL sync | Engine有完整tail；Writer只有sampled aggregate | Writer每個group的sync distribution |
| scheduler | host上可重現guardrail failure | runnable wait、migration、per-thread switches |

這些缺口只要求補足量測邊界；沒有證據支持直接調整mutex、condition variable、thread affinity、priority、
group size或fsync。

## 4. 範圍與模組邊界

### 4.1 In scope

- benchmark使用的長生命週期thread role naming：load generator、shard writer、WAL prepare helper、
  event publisher及completion worker；
- Linux-only、benchmark-only的procfs snapshot parser與delta計算；
- Writer benchmark的opt-in thread resource diagnostics；
- Writer benchmark重用既有tail telemetry輸出CSV與summary；
- CLI validation、parser／delta unit tests與小型benchmark smoke test；
- baseline／candidate的root-cause矩陣與報告規格。

### 4.2 Out of scope

- 修改per-chunk publish bookkeeping或將candidate直接標為accepted；
- WAL serialize、write、sync、rotation、replay或on-disk format修改；
- group size、group delay、prepare lane數、queue容量或fsync policy調校；
- thread affinity、scheduler priority、busy polling、spin wait或condition-variable替換；
- Publisher／Completion throughput optimization；
- 高頻system-wide profiler、eBPF、`perf`權限調整或kernel設定修改；
- macOS scheduler counter的等價實作；
- 新public API、runtime config、production metrics schema或第三方dependency；
- 重跑direct WAL、g8192或完整Engine矩陣。

這些項目不是定位本次Writer-only guardrail failure所必需。

## 5. 架構與資料流

### 5.1 正常路徑

診斷關閉時，資料流完全不變：

```text
load generator -> ingress queue -> shard writer
  -> WAL prepare/write/sync -> StateMachine apply
  -> publisher notify -> completion queue
```

thread naming只在各thread啟動入口執行一次，不位於command或group熱路徑。

### 5.2 診斷路徑

```text
warmup完成
  -> 驗證必要thread roles唯一存在
  -> capture procfs snapshot A
  -> begin WAL sync tail collection
  -> run measured phase（原流程）
  -> stop tail collection
  -> capture procfs snapshot B
  -> 依 role 計算 B - A
  -> 輸出resource summary及tail CSV
  -> 執行既有stop／recovery／correctness validation
```

snapshot及CSV整理不得插入每個command。每group新增的工作只來自既有tail telemetry對
`wal_sync_latency_us`與`wal_group_commands`的bounded紀錄，而且只在明確啟用診斷時發生。

## 6. Thread role與核心資料模型

### 6.1 Thread role name

role name必須短於Linux `comm`的15-byte payload限制，且在本單shard benchmark process內可唯一對應：

| role | name | g4096/W=2預期數量 |
| --- | --- | ---: |
| benchmark load generator | `ob-bench` | 1 |
| shard writer | `ob-wr-1` | 1 |
| WAL prepare helper | `ob-wp-1-1` | 1 |
| event publisher | `ob-pub-1` | 1 |
| completion worker | `ob-cmp-1` | 1 |

W=4時可使用`ob-wp-1-1`至`ob-wp-1-3`，但本階段不測W=4。名稱設定失敗不得讓production
runtime失敗；診斷模式若找不到或重複找到必要role，該run必須以診斷無效結束，不得猜測TID。

### 6.2 `ThreadResourceSnapshot`

benchmark-private資料至少包含：

```text
tid
role
cpu_runtime_ns
runqueue_wait_ns
sched_timeslices
voluntary_context_switches
involuntary_context_switches
cpu_migrations                 optional
```

前六項在Linux診斷模式為必要欄位；optional欄位缺失須保留absence。所有counter以unsigned
64-bit解析並檢查overflow。delta要求after不小於before；TID／role集合在兩個snapshot間必須一致。

### 6.3 `ThreadResourceDelta`

每個role輸出raw delta及以下derived欄位：

- CPU ns與CPU seconds/M commands；
- runqueue wait ns與runqueue wait/M commands；
- voluntary／involuntary context switches/M commands；
- migrations/M commands（若可用）；
- scheduler timeslices/M commands。

不得把`measured wall - CPU - runqueue wait`直接命名為I/O wait；它同時包含condition wait、sleep與其他
blocking。若輸出，只能稱為`unclassified_blocked_or_sleep_ns`，並與既有group wait及sync tail一起解讀。

## 7. 主要介面與元件責任

### 7.1 Internal thread-name helper

新增一個小型internal helper，責任只是在目前thread啟動時設定best-effort name：

```cpp
void set_current_thread_name(std::string_view name) noexcept;
```

- 位於internal source，不安裝為public header；
- Linux使用`pthread_setname_np(pthread_self(), ...)`，macOS使用其單參數形式；其他平台為no-op；
- caller只在thread entry以固定字串或bounded temporary組成名稱；這次性組成不得位於command/group
  熱路徑，helper本身不拋例外；
- runtime不依賴設定成功與否維持正確性。

呼叫點只限thread entry：`ShardRuntime::run()`、`completion_run()`、`EventPublisher::run()`與
`PrepareWorkers::worker_loop()`；benchmark main在Writer workload開始前設定`ob-bench`。

### 7.2 Linux procfs snapshot reader

新增benchmark-private reader，例如：

```cpp
Result<ThreadResourceSnapshotSet> capture_thread_resources(
    std::span<const ExpectedThreadRole> expected_roles);

Result<ThreadResourceDeltaSet> subtract_thread_resources(
    const ThreadResourceSnapshotSet& before,
    const ThreadResourceSnapshotSet& after);
```

責任包括：列舉`/proc/self/task`、以`comm`映射role、解析`status`／`schedstat`及可選`sched`、驗證必要
role與counter單調性。它只編入benchmark／test target，不進入`order_books` public API。

### 7.3 Writer benchmark option

新增opt-in CLI概念：

```text
--writer-thread-diagnostics=off|on
--writer-tail-telemetry-output=<path>
```

規則：

- 預設皆為off／unset；
- diagnostics=on只接受Writer hot-path workload，且必須提供telemetry output path；
- Linux procfs不可用、output已存在、parent不可寫或必要role缺失時，在正式measured phase前失敗；
- diagnostics=off時不得出現`thread_resource`或Writer tail診斷輸出；
- 不新增production `EngineConfig`欄位。

### 7.4 Tail telemetry

Writer diagnostics重用`EngineTailTelemetry`的metric collection部分，不啟動其Engine state sampler：

- warmup後呼叫`begin_measured()`；
- measured phase後呼叫`stop_collection()`；
- `summary()`輸出sync count、p50、p99、p99.9、max、總時間與`>25/100/250 ms`次數；
- `write_csv()`保存逐group timestamp、sync latency與group commands；
- `telemetry_dropped_samples`、overflow或CSV write failure使該diagnostic run無效。

不為名稱整齊而重命名或重構既有telemetry class；那會擴大本需求。

## 8. 錯誤處理與有效性

- procfs parse、必要role、counter regression或snapshot集合不一致：輸出明確`error_code`並使process非0；
- optional migration／block-I/O欄位不可用：輸出`not_measured`，不使其他counter失效；
- thread name設定本身為best effort；只有啟用diagnostics時才以role discovery驗證結果；
- diagnostics setup在warmup前完成，第一次snapshot在warmup後、measured phase前完成；setup failure不得留下
  一半正式數據；
- tail buffer須在runtime start前依預期group數bounded reserve，measured phase不得成長超過上限；
- 所有delta及normalization做overflow與除零檢查；
- 原有command count、completion、WAL replay、EngineSeq與book state correctness gate保持不變；
- 不用例外輪的resource counter推翻correctness，兩者分開分類。

## 9. 測試策略

### 9.1 Unit tests

- `status`、`schedstat`及`sched`固定fixture解析；
- 欄位缺失、重複role、非法數字、overflow、counter倒退與TID消失；
- optional欄位缺失保留為absence；
- snapshot delta及per-million normalization；
- thread name長度與role-to-name mapping唯一。

不得以live scheduler counter必須大於0作unit assertion，避免flaky test。

### 9.2 Integration／CLI tests

- Writer diagnostics小型smoke可找到W=2的五個必要role，輸出schema完整並產生非空CSV；
- diagnostics=off維持既有stdout schema，不建立telemetry檔；
- diagnostics用於錯誤workload、缺少output、output已存在及不支援平台時明確拒絕；
- existing Writer profile off/on、parallel prepare、Engine telemetry與persistence tests全部繼續通過；
- Debug、Release及ASan/UBSan編譯／測試通過。效能數字不作CTest gate。

### 9.3 Overhead control

同一binary先跑三組短paired pilot：profile-on diagnostics off與on。若diagnostics-on相對off的median
throughput bias超過5%，或兩者的CPU seconds/M commands差異超過5%，正式resource數據標為
instrumentation-biased，須先縮減telemetry工作；不得挑選有利輪次。thread snapshot本身位於量測邊界，
不計入measured elapsed。

## 10. 正式根因分析矩陣

### 10.1 固定條件

```text
group size                     4,096
WAL prepare workers            2
parallel prepare threshold      4,096
producer lanes                  8,192
group delay                     1,000 us
durability                      per-group fsync
writer profile sample every     16
CPU affinity                    2-7
CPU governor / EPP / boost      保持目前production-like設定且全程不變
paired rounds                   每case 5輪
duration                        每輪至少20秒
```

### 10.2 Cases

| case | phase profile | thread diagnostics | 用途 |
| --- | --- | --- | --- |
| A | off | off | production-like Writer control；判斷退化是否獨立於phase profile |
| B | on | off | 重現既有Writer guardrail與計算profile bias |
| C | on | on | per-thread scheduler及逐group sync歸因 |

baseline與candidate各跑A/B/C五輪，共30個formal runs，交錯順序並在奇偶輪反轉先後。既有15-pair
confirmatory結果保留為candidate acceptance歷史證據；本矩陣不把舊、新絕對median直接合併。

不重跑direct WAL、g8192、Publisher、Completion或authoritative Engine，因為它們無法回答本階段新增的
per-thread歸因問題，且Engine在兩輪既有證據均未重現相同guardrail failure。

## 11. 判讀規則

本階段沒有「讓candidate通過」的gate；輸出是根因分類。每項分類必須同時有多數paired rounds與
median delta同方向，不得只引用單一慢輪。

| 觀測 | 可支持的結論 | 下一步 |
| --- | --- | --- |
| A無退化、B退化，且diagnostics overhead control通過 | profile instrumentation interaction | 修正benchmark量測；不得修改production排程 |
| A/B都退化，writer role runqueue wait、involuntary switches或migration一致增加 | writer scheduler interaction | 另立最小scheduler／wake-up修正設計 |
| 慢輪同時出現sync p99.9／max與writer blocked time增加，runqueue wait未增加 | storage／fsync tail | 以既有fsync證據設計storage-path修正或環境SLO |
| completion／publisher role惡化且queue／lag同方向增加 | async worker contention | 延後到async worker專案，不塞回WAL bookkeeping patch |
| prepare helper CPU／runqueue增加且`wal_prepare_ns`同方向 | prepare scheduling interaction | 針對W=2 prepare協作另立設計 |
| 無role與sync訊號可跨多數輪重現 | host noise／inconclusive attribution | candidate維持provisional；不做推測性production修改 |

`/usr/bin/time` process totals只作交叉檢查；報告不得再將其稱為writer thread counter。相關性不是因果，
若多個role同時惡化，結論須保留為競爭範圍，不得任選一項作根因。

### 11.1 報告必要內容

根因分析報告只需包含：

- source、binary、CPU policy、filesystem、affinity及診斷schema identity；
- A／B／C每輪raw summary與baseline/candidate paired delta；
- C case依thread role列出的CPU、runqueue wait、context switches、timeslices與可用時的migration；
- Writer sync p50／p99／p99.9／max、慢sync次數及telemetry完整性；
- diagnostics overhead pilot結果；
- 依本節decision table得到的一項分類，或明確的`inconclusive attribution`；
- 尚未量到的counter與不能支持的因果敘述。

不需要重複貼出既有direct WAL、g8192及Engine完整raw rows；以文件連結引用即可。

## 12. 關鍵設計決策與取捨

- 使用量測邊界snapshot而非10 ms sampler：足以找出role貢獻，且不新增會影響scheduler的常駐thread。
- 使用thread role name而非依TID建立順序猜測：TID順序不是契約；role name也改善Linux維運可觀測性。
- 重用既有tail telemetry：避免建立重複collector；只使用其已有的bounded metric path。
- 不要求`perf`：目前權限明確不可用；procfs可補足本次必要的per-thread counters。若日後可用`perf`，只作
  confirmatory evidence，不改本設計的correctness結果。
- 不直接修改queue或scheduler：目前只知道process-wide regression，直接調整會混淆根因並可能破壞
  deterministic owner模型。
- 不用本次診斷重寫原acceptance gate：原candidate狀態仍是`provisional, not accepted`，直到另有預先
  定義的production acceptance設計。

## 13. 已知限制與可擴充方向

- procfs為Linux介面；macOS開發可編譯，但正式歸因必須在Linux執行。
- 邊界snapshot能定位每輪role成本，不能把單一p99.9 command精確對齊某次preemption；若本輪仍只縮小
  到兩個候選根因，才考慮另行使用`perf sched`或eBPF，不在本案預先加入。
- thread naming不表示production必須依名稱管理thread affinity或priority。
- sync CSV只說明storage latency共變，不證明block發生在filesystem、device firmware或host層。
- Writer workload與authoritative Engine的load generation不同；本案只定位Writer guardrail，不重新定義
  production SLO。

## 14. 具體修改清單與預估規模

| 檔案／區域 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| internal thread-name helper及build wiring | cross-platform best-effort helper | 35--55 |
| `src/runtime/shard_runtime.cpp` | writer／completion thread entry命名 | 4--8 |
| `src/runtime/event_publisher.cpp` | publisher thread entry命名 | 2--4 |
| `src/persistence/wal.cpp` | prepare helper依lane命名 | 5--10 |
| benchmark procfs snapshot helper | parser、snapshot、delta與summary | 180--260 |
| `engine_writer_profile_benchmark.*` | option、A/B snapshot、tail telemetry及輸出 | 80--130 |
| `order_book_benchmark.cpp`／benchmark CMake | CLI validation與source wiring | 25--45 |
| unit／CLI tests | parser、delta、off-path與smoke | 140--220 |

預估production runtime實質修改約15--25行，主要程式碼都在benchmark-private診斷與測試。若實作需要
改動WAL public API、`EngineConfig`、queue item、durability流程或超過上述邊界，應先回頭更新本設計，
不能以「方便量測」擴大production行為。

## 15. 完成定義

- diagnostics off時行為、輸出與熱路徑保持不變；
- Linux diagnostics可唯一辨識五個必要thread roles並輸出有效前後delta；
- Writer每個measured WAL group都有sync telemetry，沒有drop或overflow；
- profile off/on及diagnostics overhead control可依預定矩陣執行；
- correctness、replay與sanitizer tests通過；
- 報告能依第11節分類，或明確寫`inconclusive attribution`；
- 未修改WAL格式、fsync、queue、worker數、affinity、priority、Publisher／Completion邏輯；
- candidate在完成歸因前仍標為`provisional, not accepted`；
- Git staging在整個實作與測試流程中保持不變。
