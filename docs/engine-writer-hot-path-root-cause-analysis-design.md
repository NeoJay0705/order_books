# Engine Writer Hot Path 根因分析設計審查與必要修改

## 1. 文件目的

本文件承接以下已完成的正式量測：

- `engine-writer-throughput-optimization-benchmark-report.md`；
- `wal-durable-path-root-cause-analysis-report.md`；
- `publisher-cursor-group-persistence-benchmark-report.md`。

目前單一 shard、單一 instrument 的完整 Engine durable median 約為 162,780 commands/s；相同
group=4,096 的 direct WAL durable median 約為 412,817 commands/s。換算後，完整 Engine 約需
6.14 us/command，其中 direct WAL 約需 2.42 us/command，尚有約 3.72 us/command 的非 WAL
合併成本未被同一次 production-like run 分配到 admission、apply、post-apply metrics 與
Completion handoff。

WAL 現有 coarse profile 顯示 group=4,096 時 `prepare`、`plan_copy`、`publish` 與 `sync` 分別約占
40.5%、13.0%、14.5% 與 29.6%，但 group=4,096、`sync=none` 的 profile-on throughput 相對
profile-off 下降約 9.1%。因此 phase share 只能作方向性歸因，仍不足以直接決定 parallel prepare、
buffer redesign 或 record-index redesign。

本階段的目標是以最小的 opt-in 診斷修改完成兩件事：

1. 在真正的 `ShardRuntime` writer 與 Completion worker 上分配 Engine 非 WAL 成本；
2. 在既有 WAL production implementation 中細分 `prepare` 與 `plan_copy`，判定 CPU、CRC、
   framing 或 memory copy 哪一類工作值得另案優化。

本文件是診斷實作與正式量測契約。交付物是根因報告及下一階段 production optimization 的選擇，
不是在證據完成前直接加入 thread pool，也不承諾本階段達成 1,000,000 durable commands/s。

## 2. Review 結論

### 2.1 已經能確定的事項

- incremental validation 後的 `StateMachine` 在既有 controlled workload 約為
  0.999M–1.128M command-equivalent/s，並非目前 162.8K/s 的單一第一根因；但它接近長期目標，
  尚不能視為有充足 headroom。
- metrics ownership split 的 writer ceiling 約為 1.697M command-equivalent/s；shared registry
  contention 已處理，本階段不再設計新的 metrics framework。
- runtime handoff 目前最高約為 944K callbacks/s，但該數字合併 ingress、admission rejection、
  Completion queue 與 callback，無法把成本分配給其中一項。
- direct WAL 是 writer 同步路徑中 ceiling 最低的已知單項，但完整 Engine 比 direct WAL 再慢約
  2.54 倍；兩者不是互斥瓶頸，而是串行成本累加。
- Publisher worker 的持續吞吐仍低於 writer，但需求已明確決定延後其 production 優化。本階段只
  保留 lag／pressure correctness，不能讓 Publisher 工作混入 writer phase share。

### 2.2 必要修改

為回答尚未解決的問題，必要修改只有：

1. 新增 internal-only、opt-in 的 writer group profile collector，量測 group collection、
   admission、WAL append、WAL sync、apply、Publisher notify、post-apply processing 與 completion
   enqueue；Completion worker 的 queue residence 與 callback service time獨立回報。
2. 擴充既有 `WalAppendProfile`，在不複製 encoder／CRC／append algorithm 的前提下，將
   `prepare` 細分為 payload encode、CRC 與 frame assembly，並從 `plan_copy` 中獨立回報
   contiguous chunk copy。
3. 新增一個使用正式 `ShardRuntime`、只供歸因的 benchmark workload，並以既有 public
   `engine_durable_single_instrument` 作 authoritative end-to-end control。
4. 建立 profile off/on bias check、五輪長測與 correctness／replay 驗證，產出可決定後續優化的
   報告。

不修改 WAL format、durability boundary、Engine ordering、group defaults 或 public API。也不在本
階段加入 parallel prepare、lock-free queue、buffer pool、async WAL 或 Publisher 優化。這些項目
只有通過第 11 節的決策門檻後才能另案設計。

## 3. 需求理解與合理假設

### 3.1 必須回答的問題

1. 完整 writer 的非 WAL 約 3.72 us/command 主要落在 admission、apply、post-apply metrics、
   completion enqueue，還是 Completion worker？
2. WAL `prepare` 的主要成本是 command payload encoding、CRC32C、frame assembly，還是未被子階段
   解釋的 allocation／其他工作？
3. `plan_copy` 中有多少時間花在 contiguous chunk allocation／copy，而不是 metadata 與 segment
   planning？
4. writer 與 Completion worker 的 service rate 是否低於 arrival rate，queue residence／depth
   是否隨測試時間持續增加？
5. 若 prepare 主要是可平行的 CPU work，加入固定 CPU 上限的 parallel prepare 是否具有至少可見的
   Amdahl 理論收益；若不是，下一階段應選擇 copy／allocation、WAL index 或 Engine post-processing？

### 3.2 不變條件

- 目標情境仍是單一 shard、單一 instrument、單一 ordered writer。
- 成功 completion 不得早於同 group 的 WAL append 與 `fsync` 成功。
- EngineSeq、producer sequencing、matching determinism、event ordering、WAL bytes、CRC coverage、
  recovery 與 RPO=0 語意不變。
- `StateMachine::apply` 維持單一 deterministic mutation owner；本階段不平行 apply。
- Publisher 仍由獨立 worker replay durable WAL；writer profile 只量 `notify_publishable()` 呼叫，
  不把實際 replay、sink ACK 或 cursor persistence 算入 writer service time。
- Snapshot interval 在 ceiling workload 中維持停用；segment rotation仍保留並獨立回報。
- authoritative throughput 使用 profile-off；profile-on 只用於成本歸因。
- 正式結果使用至少五輪 median、range 與 worst tail，不以單次最佳值下結論。

### 3.3 名詞與量測層級

本文件的「stage」是同一 process 內的 pipeline 階段，不代表 OS process。

量測分成三層，報告不得跨層重複相加：

```text
end-to-end Engine durable                     # authoritative throughput/latency
  writer cycle / writer service               # ShardRuntime attribution
    WAL append / sync                          # writer 的 child phase
      WAL prepare / plan / copy / write/...    # WAL child phase

Completion worker service                     # 與 writer 並行，獨立 denominator
Publisher worker                              # 本階段不作 phase profile
```

## 4. 範圍與非目標

### 4.1 In scope

- `ShardRuntime` internal writer／Completion opt-in phase profile；
- existing `WalAppendProfile` 的 prepare 與 copy 子階段；
- 使用正式 admission、WAL、StateMachine、metrics、Completion queue 的診斷 workload；
- profile off/on、direct WAL 與 public Engine durable 的同環境對照；
- queue depth／residence、group size、phase totals、CPU utilization 與 context-switch evidence；
- unit、integration、CLI、replay、sanitizer、Release build 與 bias tests；
- 根因分析報告與下一階段決策。

### 4.2 Out of scope

- parallel WAL prepare 或新的 thread pool；
- CPU affinity／worker count 的 production config；
- WAL record／segment format、checksum coverage或recovery規則；
- `writev`、`io_uring`、AIO、direct I/O、mmap、async／multiple in-flight sync；
- apply-before-fsync、降低 sync 頻率或任何 RPO 變更；
- buffer pool、custom allocator、新的 cached-record representation；
- lock-free ingress／Completion queue或Completion batch drain；
- Publisher replay、EventSink、cursor persistence 或 replay snapshot 優化；
- Snapshot、retention、多 shard／多 instrument scaling；
- production group size／delay default 變更；
- 通用 tracing framework、第三方 telemetry dependency 或 shared CI RPS gate。

## 5. 架構與資料流

### 5.1 Production path

Production path 不改變：

```text
Engine::submit
  -> ShardRuntime ingress queue
  -> collect group
  -> admission / producer checks / pressure checks
  -> build CommittedCommand
  -> Wal::append_batch
       -> payload encode
       -> frame version / CRC / framing
       -> metadata and segment plan
       -> contiguous chunk copy
       -> optional rotation
       -> write_all
       -> in-memory WAL index publication
  -> Wal::sync
  -> StateMachine::apply
  -> notify_publishable
  -> result / metrics processing
  -> Completion queue
  -> Completion worker callback
```

Normal `Engine::open()` 不安裝 profile collector，新增路徑不得取 clock、配置 profile sample 或呼叫
profile callback。既有 production output 與 error semantics 維持不變。

### 5.2 Diagnostic path

新增 workload 暫定命名為：

```text
--workload=engine_writer_hot_path_profile
--writer-phase-profile=off|on
```

profile runner 直接使用 internal `ShardRuntime::open()`，只省略 public `Engine` 的 routing lookup；從
`ShardRuntime::submit()`、正式 queue、writer、WAL、StateMachine 到 Completion callback 均使用同一
production implementation。它是 attribution result，不取代 public
`engine_durable_single_instrument` 的 authoritative throughput。

```text
profile-off direct ShardRuntime run   -> diagnostic control throughput
profile-on direct ShardRuntime run    -> phase attribution
public Engine durable run             -> authoritative end-to-end control
direct WAL run                        -> WAL-only control
```

不得建立簡化 queue、memory-only WAL、mock StateMachine、假的 Completion dispatcher 或第二套
encoding／CRC implementation。

## 6. Internal Profile 介面與責任

### 6.1 Writer group sample

在 `src/runtime/` 增加 internal-only profile types；名稱可依既有風格微調：

```cpp
struct WriterGroupProfile {
  std::uint64_t input_commands{};
  std::uint64_t accepted_commands{};
  std::uint64_t writer_cycle_ns{};
  std::uint64_t writer_service_ns{};
  std::uint64_t group_collect_ns{};
  std::uint64_t group_wait_ns{};
  std::uint64_t admission_ns{};
  std::uint64_t wal_append_ns{};
  std::uint64_t wal_sync_ns{};
  std::uint64_t apply_ns{};
  std::uint64_t publisher_notify_ns{};
  std::uint64_t post_apply_ns{};
  std::uint64_t completion_enqueue_ns{};
  storage::WalAppendProfile wal;
};
```

`group_collect_ns` 是取得第一筆 command 後至 batch 完成的 wall time；`group_wait_ns` 只包含為等待
更多 command 而阻塞在 condition variable 的時間。兩者可同時回報，但報告 writer active work 時
必須使用 `group_collect_ns - group_wait_ns`，不得把 parent 與 child 重複相加。

`writer_service_ns` 是進入 admission 至 completion enqueue 完成的外層實測 wall time；
`writer_cycle_ns` 是取得第一筆 command 後至同一 group completion enqueue 完成的外層 wall time。
兩者不能由 child phase 反推，否則無法計算 unattributed remainder。

`WriterGroupProfile` 只對至少一筆 accepted command 且 WAL append／sync 成功的 group 納入正式
phase aggregation。全部被 admission reject 的 group 另外計數，不得與 durable group混合。

### 6.2 Completion sample

Completion worker 與 writer 並行，因此使用獨立資料模型與 denominator：

```cpp
struct CompletionProfile {
  std::uint64_t completions{};
  std::uint64_t queue_residence_ns{};
  std::uint64_t callback_service_ns{};
  std::uint64_t max_queue_depth{};
};
```

profile-on 時需要把 enqueue timestamp帶到 Completion worker。`queue_residence_ns` 是 enqueue 至
Completion worker 取出的時間；`callback_service_ns` 只包覆 callback 本身。正常路徑不得為此額外
取 clock或增加queue item storage。

為避免永久放大 production completion queue item，實作應只在 profile-on 時以 diagnostic wrapper
包住原 CompletionHandler，capture enqueue timestamp並在 Completion thread量測；profile-off 繼續
存放原本的 result／handler pair。Wrapper必須在原callback丟出exception時仍結束service sample，
再由既有`completion_callback_error`邊界處理，不能改變exception isolation語意。

### 6.3 Collector ownership

使用 nullable internal collector／observer，由 diagnostic runner 擁有，生命週期長於
`ShardRuntime`：

- writer thread 只寫 writer-owned samples；
- Completion thread 只寫 completion-owned samples；
- runner 只在 `ShardRuntime::stop()`／join 後讀取；
- 不經過 `MetricsRegistry`，避免重新引入 shared-registry contention；
- observer 方法必須 `noexcept`，profile failure 不得改變 Engine command outcome；
- 不加入 public `EngineConfig`、`MetricsSnapshot`、`CommandResult` 或 installed headers。

`ShardRuntime::open()` 是 internal API，可接受預設為 null 的 collector pointer，或提供 internal
overload。`Engine::open()` 必須沿用 null 路徑。禁止以 global singleton、環境變數或
`dynamic_cast<MetricsSink*>` 偷渡 profile control。

Collector若需配置sample storage，必須在runner啟動前reserve；observer內仍應捕捉配置／aggregation
例外並只設定diagnostic-invalid flag，不能因`noexcept` callback而terminate process。

## 7. Writer Phase 邊界

各 phase 必須 exclusive；巢狀 WAL child phases 除外，報告時使用獨立 denominator。

| Phase | 包含 | 不包含 |
|---|---|---|
| `group_collect` | 第一筆 dequeue 後的 batch collection、queue depth observation、idle-fill wait | 第一筆 command 的 ingress residence、command processing |
| `group_wait` | condition-variable wait for more commands | active dequeue／vector work |
| `admission` | producer lookup、duplicate／sequence checks、storage／Publisher pressure check、`CommittedCommand` construction、accepted/result slot準備 | queue wait、WAL |
| `wal_append` | 正式 `Wal::append_batch_profiled()` call | group sync |
| `wal_sync` | 正式 `Wal::sync()` call | rotation-triggered sync |
| `apply` | 全部 accepted commands 的 `StateMachine::apply` 與 output vector construction | post-apply metrics |
| `publisher_notify` | failed check 與 `notify_publishable()` | Publisher worker replay／sink／cursor |
| `post_apply` | result move、event trade count、runtime metrics、active-state gauge與snapshot counter更新 | completion enqueue |
| `completion_enqueue` | `dispatch_results()` 內 completion queue capacity wait、push 與 notify | Completion worker callback |

Snapshot／retention 在正式 ceiling workload 中停用。若因設定錯誤實際觸發，該 run 必須標示無效，
不能把 snapshot time塞入 unattributed remainder。

報告至少輸出：

```text
writer_cycle_total_ns
writer_service_total_ns
profiled_groups
profiled_input_commands
profiled_accepted_commands
各 phase total / per-command / writer-service share
writer_unattributed_ns / share
completion queue residence / callback service / max depth
```

Completion queue residence與callback service至少回報per-completion p50／p99／p99.9／max及整體
service rate；累計nanoseconds不能直接當作與writer wall time可相加的duration。

其中：

```text
writer_children = admission + wal_append + wal_sync + apply
                + publisher_notify + post_apply + completion_enqueue

writer_service_unattributed = writer_service_ns - writer_children
writer_cycle_unattributed   = writer_cycle_ns - group_collect_ns - writer_service_ns
```

`unattributed` 只能由外層 cycle／service wall time減去 exclusive children 計算，必須檢查 underflow。

## 8. WAL 子階段擴充

### 8.1 Data model

在既有 internal `WalAppendProfile` 增加：

```cpp
std::uint64_t payload_encode_ns{};
std::uint64_t crc_ns{};
std::uint64_t frame_assembly_ns{};
std::uint64_t chunk_copy_ns{};
std::uint64_t payload_bytes{};
```

既有 `prepare_ns` 與 `plan_copy_ns` 保留為 parent phase，維持先前報告相容性。新增欄位的語意：

| Field | 包含 | 備註 |
|---|---|---|
| `payload_encode_ns` | `encode_committed_command()` | 累加每筆 profile-only sample |
| `crc_ns` | 對 version+payload body 執行 `crc32c()` | checksum coverage 不變 |
| `frame_assembly_ns` | version／CRC／length 寫入及 payload/body vector insert | 包含相關 allocation/copy，不宣稱是純 allocation time |
| `chunk_copy_ns` | chunk buffer allocation/reserve 與 frame bytes insert | 是 `plan_copy_ns` 的 child |
| `payload_bytes` | encoded command payload bytes 總數 | 與完整 `frame_bytes` 分開 |

因 allocator 呼叫發生在 `BinaryWriter`／`std::vector` 內，本階段不得輸出虛構的
`allocation_ns`。若 encode、CRC、frame assembly 與 parent remainder仍無法解釋 prepare，報告只能
標示 `prepare_remainder`，後續再決定是否需要 heap profiler。

### 8.2 Implementation constraints

- `encode_frame()` 與 prepare helper 接受 nullable profile pointer，normal path不得取 clock；
- 不複製 `encode_committed_command()`、`crc32c()` 或 framing 邏輯；
- profile-only timing 可以逐 record 累加，但必須執行 profile off/on bias check；
- `prepare_ns` 包含其 children與profile bookkeeping，children sum 不得硬要求等於 parent；
- `chunk_copy_ns` 可以按 chunk 累加，不得為量測改變 chunk boundary 或 write count；
- counters 必須使用 checked／saturating aggregation，不能發生 unsigned wrap；
- normal 與 profiled append 必須產生完全相同 WAL bytes、position、write count 與 replay result；
- partial write、rotation、sync failure 與 fail-stop semantics 不變。

## 9. Benchmark 與正式量測方法

### 9.1 CLI

新增：

```text
--workload=engine_writer_hot_path_profile
--writer-phase-profile=off|on
```

規則：

- profile option 預設 `off`；
- parser 必須記錄是否由使用者明確指定；其他 workload 明確指定時以 exit code 2 拒絕；
- 空值、未知值、group／lane 為零或超過 ingress capacity 時，在建立 data directory 前拒絕；
- 沿用既有 `--engine-group-size`、`--engine-group-delay-us`、
  `--engine-producer-lanes`、`--iterations`、`--warmup` 與 data-directory 規則；
- 不增加 production config default。

### 9.2 Workload correctness

工作負載維持既有 single-instrument alternating sell／buy、每 producer lane single-in-flight。每輪
必須驗證：

- submitted、accepted、durable commands 與 callbacks 數量一致；
- completion exactly once、identity正確且 status committed；
- trade count、book empty、active orders／levels符合預期；
- actual commands/group、group commits 與 WAL bytes合理；
- WAL reopen／replay count、EngineSeq continuity、durable head正確；
- collector 的 accepted commands、WAL profiled commands 與 runtime counters一致；
- Completion queue 在 stop 前 drain，沒有 callback error；
- Publisher failure／storage pressure／timeout 的 run 不納入效能結果。

### 9.3 最小正式矩陣

在同一 source、Release build、CPU affinity、filesystem 與 device 上執行：

1. `engine_writer_hot_path_profile`：group=4,096、delay=1 ms、lanes=8,192，profile off/on 各五輪；
2. 同一 profile workload：group=256、delay=1 ms、lanes至少256，profile off/on 各五輪，用來確認
   小 group 下 phase ordering是否改變；
3. `engine_durable_single_instrument`：group=4,096 的既有五輪 authoritative control；
4. `wal_write_ceiling`：group=4,096、`sync=per_group` 的既有 off/on control；
5. `/usr/bin/time -v` 與 per-thread CPU observation各一個代表性 profile-off run；`perf` 只在權限
   可用時作輔助，不是完成條件。

每個正式 throughput run 的 measured phase至少 15 秒並使用 fresh data directory。若既有結果與
source hash、build flags、CPU／filesystem 條件完全相同，可以引用；否則必須重跑，不能跨 commit
拼接 phase share。

### 9.4 Instrumentation bias

對相同 workload 計算：

```text
bias = abs(profile_on_median - profile_off_median) / profile_off_median
```

- bias <= 5%：phase share 可作主要歸因證據；
- bias > 5%：authoritative throughput只採 off，phase share降級為 directional evidence；
- bias > 10%：不得用細分 phase百分比決定 production optimization，必須降低 sampling／計時成本或
  改用外部 profiler後重測。

逐 record clock 很可能提高 bias，因此 benchmark 可採 deterministic sampling，例如每固定 N 個
group詳細量測一次；若採樣，N 必須固定輸出，coarse writer totals與detailed WAL totals不得使用不同
denominator卻報成同一 share。不得事後挑選較快樣本。

## 10. 錯誤處理與測試策略

### 10.1 Unit tests

- profile object 每次呼叫前清零；
- normal／profiled WAL append 的 bytes、positions、write counts與replay相同；
- `payload_bytes <= frame_bytes`，profile counters／durations不 overflow；
- `chunk_copy_ns` 不大於含 bookkeeping誤差的 `plan_copy_ns`；測試不得要求 duration非零；
- empty batch、oversized record、rotation與reopen維持既有錯誤；
- collector null時不建立 samples，profile on時 group／command counts正確；
- Completion profile exactly-once，callback throw仍維持既有錯誤隔離。

### 10.2 Integration tests

- profile off/on 使用相同 commands得到相同 results、events、WAL replay與durable head；
- admission reject不得被計入 durable group phase；
- group=1／小型 batch的 smoke不除以零；
- Completion queue residence與callback count對齊；
- publisher notification仍只在 WAL durable且apply成功後發出；
- CLI valid、empty、unknown、wrong-workload、zero／over-capacity cases；
- Release warnings-as-errors、GoogleTest、ASan／UBSan。

Performance threshold不放入shared CI；CI只驗證輸出欄位、counts與correctness。正式RPS由受控主機
手動執行並寫入報告。

### 10.3 Profile failure policy

Profile collector是診斷功能，不得讓正常command失敗。Collector method必須`noexcept`；若sample
aggregation無法表示或內部一致性檢查失敗，benchmark在測試結束時以非零狀態回報
`writer_profile_invalid`，但不得改寫已完成command的business result。

## 11. 後續優化決策門檻

本階段報告只能依以下規則選擇下一案，不能同時實作所有候選：

### 11.1 Parallel prepare

只有同時滿足下列條件才設計 bounded parallel prepare：

- WAL prepare在profile hierarchy中仍是主要成本；
- payload encode + CRC 是 prepare 的主要可解釋部分，而不是 frame/chunk copy或未知remainder；
- profile-off resource evidence顯示writer有CPU工作且主機有可分配核心；
- 依Amdahl估算，2～4個worker的理論end-to-end uplift至少10%；
- group大小足以攤平task dispatch成本；小group必須保留single-thread fallback。

後續設計才決定固定worker pool、`wal_prepare_worker_count`上限、threshold、affinity、ordering、
deterministic error selection與shutdown。不得在本診斷變更中預留通用executor framework。

### 11.2 Copy／buffer path

若frame assembly或chunk copy占writer service的比例高於prepare中的encode／CRC，下一案優先評估
減少中間vector與第二次copy；不能先用更多CPU平行複製。任何zero-copy／scatter-gather方案都必須
維持partial-write處理、segment boundary與WAL bytes。

### 11.3 WAL in-memory publication

若`publish`在不同run length下持續占顯著比例，且成本隨`records_`大小增加，下一案才評估record
index representation／reserve policy。若占比穩定且低於主要phase，不修改資料模型。

### 11.4 Engine non-WAL path

- admission或post-apply若占writer service >=10%，再對該stage做一次小範圍sub-profile；
- apply若占比顯著或production-like ceiling低於1.2M/s，再細分book lookup、matching、events與
  incremental validation；
- completion service rate低於writer arrival rate或queue depth持續增長，才另案設計batch drain；
- 單一stage若低於5%且isolated ceiling有至少2倍headroom，不再深入instrument。

百分比是決策門檻，不是production SLO；若profile bias超標，必須使用absolute time與外部證據交叉
確認。

## 12. 關鍵取捨

### 12.1 為何不直接實作 parallel prepare

目前只知道coarse `prepare`占比，尚不知道encode／CRC與allocation／copy比例。Thread pool會增加
排程、同步、shutdown、CPU配置與錯誤排序複雜度；若真正主因是memory copy或allocator contention，
平行化可能增加CPU用量卻不提高throughput。因此先完成細分是必要成本，不是延後已確認的修正。

### 12.2 為何 profile workload 直接使用 ShardRuntime

public `Engine`沒有internal profile hook。把benchmark collector加入`EngineConfig`或public metrics
會形成不必要的API契約；dynamic cast、global flag或環境變數則難以測試。直接使用internal
`ShardRuntime`可以保留真正queue、writer、WAL、apply與Completion code，同時讓public Engine
control負責檢查routing層與端到端差異。

### 12.3 為何 Completion 與 writer 分開計算

兩者在不同threads並行執行。把callback time加進writer phase share會重複計算wall time，也會把
queue residence誤認為CPU service。正式報告同時呈現writer service與Completion service／lag，
但不把兩者duration直接相加。

### 12.4 為何不處理 Publisher

Publisher確實是目前系統sustainable throughput限制之一，但使用者已明確指定延後優化。Writer
profiling仍保留notify time、lag與pressure correctness，以避免Publisher failure污染結果；不量
Publisher replay子階段，也不修改cursor policy。

## 13. 已知限制與可擴充方向

- detailed WAL timing需要逐record clock時可能有明顯bias；bias超標時只能提供方向性證據。
- direct `ShardRuntime` runner不包含public Engine routing lookup，因此必須保留public Engine control。
- single-instrument alternating crossing workload沒有代表deep-book、多trade或多instrument成本；
  本階段只回答既定單一交易對目標。
- local ext4結果不能外推至production SSD、RAID或cloud block device。
- `perf_event_paranoid`可能阻止hardware counters；權限不足必須如實記錄，不能推測cache miss或IPC。
- Snapshot在ceiling workload停用，rotation只反映偶發tail；production週期性工作需另做soak test。
- 若parallel prepare後WAL不再是第一瓶頸，可沿用同一profile hierarchy重新定位；不預先抽象成通用
  pipeline scheduler。

## 14. 預期檔案與修改規模

實作者可依現有檔案組織微調，但必要責任預期落在：

```text
src/runtime/writer_profile.hpp                 新增 internal profile types／collector contract
src/runtime/shard_runtime.hpp/.cpp             opt-in phase boundaries與Completion sample
src/persistence/wal.hpp/.cpp                   prepare／CRC／frame／chunk-copy子階段
benchmarks/engine_writer_profile_benchmark.*   diagnostic runner與aggregation
benchmarks/order_book_benchmark.cpp            CLI dispatch／共用options（或等價薄入口）
benchmarks/CMakeLists.txt                       CLI／smoke tests
tests/integration/persistence_test.cpp          WAL profile correctness
tests/integration/engine_pipeline_ceiling_test.cpp
                                                writer／Completion profile correctness
```

估計production/internal code約180～300行修改，benchmark與aggregation約250～400行，tests約
120～200行，總計約550～900行。若實作需要明顯超過此範圍，應先檢查是否引入了通用executor、
telemetry framework或production optimization；那些都超出本設計。

## 15. 完成條件

- normal path無新增clock／sample allocation，public API與durability semantics不變；
- profile runner使用正式ShardRuntime、WAL、StateMachine與Completion worker；
- writer與Completion phase邊界符合本文件且沒有重複計算；
- WAL prepare／plan-copy子階段與parent counters一致，profiled／normal WAL結果相同；
- CLI、unit、integration、Release warnings-as-errors、ASan／UBSan通過；
- profile off/on與authoritative controls各完成五輪有效長測；
- 報告揭露bias、source hash、環境、throughput、latency、phase absolute time／share、CPU與queue證據；
- 報告依第11節只選擇一個有證據支持的下一階段production optimization；
- 不包含parallel prepare、Publisher優化、queue redesign或其他out-of-scope變更。
