# Engine writer 同步路徑 ceiling／根因分析設計 review 與必要修改

## 1. Review 對象與結論

本文件 review `docs/engine-writer-hot-path-root-cause-analysis-design.md`，並承接其後已完成的
writer profile、bounded parallel WAL prepare、production configuration、WAL sync attribution 與
fsync amortization frontier 工作。新的需求是：**先確認 Engine writer 同一條同步執行路徑上每個
process 的 ceiling 與主要根因，Publisher worker 與 Completion worker 的服務邏輯之後再處理。**

原設計對 production ordering、profile ownership、exclusive phase 邊界及 correctness 的定義仍然
正確；但它不能直接作為本階段實作契約，原因如下：

1. 原報告量測的是 bounded parallel prepare 導入前的 source，不能用舊 phase share 解釋目前程式；
2. 原報告主要回答 phase share，沒有以一致公式列出每個同步 phase 的 derived service ceiling 與
   1M／1.2M headroom；
3. 舊資料已顯示 `StateMachine::apply` 約 1.32 us/command、derived ceiling 約 0.76M/s，且約占
   writer service 22.5%，已符合原設計「低於 1.2M/s 或占比顯著時再細分」的條件；
4. 最新 direct WAL 結果已將 group=8,192 確認為 fsync amortization Pareto point；group=16,384
   只有約 2% uplift 且 Engine case supply-limited，沒有必要再擴大 group；
5. full-state `validate_state()` 不在每個 command/group 的 steady-state 熱路徑；真正每筆執行的是
   `StateMachine::apply()` 內的 touched-state／transition incremental validation，兩者不得混稱。

因此，本階段**不重建 writer profiler、不修改 durability、不調整 downstream workers**。必要修改只有：

- 在既有 opt-in writer profile 中加入 `StateMachine::apply` 的最小子階段歸因；
- 使用現有 phase totals 計算全部同步 phase 的 derived service ceiling；
- 在目前 source 上，以 W=1 純 writer baseline 與 W=2 現有 bounded-prepare 對照，重新量測
  group=4,096／8,192；
- 用既有 isolated workload 交叉驗證 WAL、StateMachine、metrics 與 handoff，不為每個極小 phase
  再建立一套假 production microbenchmark；
- 產出能將 root cause 分成 confirmed、supported 與 unconfirmed 的正式報告。

完成這些修改後，設計與實作對「同步路徑」的認知可一致。原歷史設計與報告保留，不回寫成目前結果。

## 2. 需求理解與合理假設

### 2.1 目標

本階段回答以下問題：

1. 目前單一 shard、單一 instrument、per-group `fsync` 的 Engine writer ceiling 是多少？
2. writer 同步路徑中每個 exclusive phase 的 ns/command、service ceiling、phase share 與 target
   headroom 是多少？
3. bounded parallel prepare 後，第一個低於 1M/s 或缺少 20% headroom 的同步 phase 是哪一個？
4. `StateMachine::apply` 的主要成本是 input/config lookup、OrderBook matching/mutation、state/index
   update、event/result construction、producer result materialization，還是 incremental validation？
5. Engine 與 direct WAL ceiling 的剩餘差距能否由同步 phase 加總解釋？

目標 ceiling 為 1,000,000 commands/s。用來判定是否有足夠餘裕的診斷門檻為 1,200,000
commands/s；這是 20% headroom 的工程門檻，不是 production SLO。

### 2.2 真實同步順序

設計與實作一律使用目前 production ordering：

```text
dequeue / group collection
  -> admission、producer sequence、storage/publisher pressure check
  -> build CommittedCommand（不修改 order book）
  -> Wal::append_batch
       -> prepare / serialize / CRC / frame
       -> plan / copy / publish / write / optional rotation
  -> Wal::sync（per-group fsync）
  -> StateMachine::apply
       -> matching / mutation
       -> result / events
       -> producer result
       -> touched-state incremental validation
  -> notify_publishable（只計 writer 上的通知呼叫）
  -> post-apply result／metrics processing
  -> enqueue completion（只計 writer 上的 enqueue）
```

禁止將 `StateMachine::apply` 畫在 WAL append／sync 之前。成功 completion 仍只能在 WAL durable、apply
成功後進入 Completion queue。

### 2.3 本階段所稱的 ceiling

- **Authoritative Engine ceiling**：profile-off 的完整 durable workload throughput。
- **Derived phase ceiling**：`1e9 / phase_ns_per_command`，只描述該 phase 的服務能力；串行 phase
  ceiling 不可相加，也不可當成獨立 production throughput。
- **Isolated ceiling**：既有正式 component workload 的獨立 throughput，例如 direct WAL 或
  `engine_pipeline_ceiling/state_machine`。
- **Downstream service rate**：Publisher／Completion worker 的消化能力；本階段只作有效性護欄，
  不加入 writer wall-time ceiling。

報告必須標明類型，不能把 derived ceiling、isolated ceiling 與 end-to-end ceiling混為同一數字。

## 3. 範圍與非目標

### 3.1 In scope

- 既有 `engine_writer_hot_path_profile` 的目前 source 重測；
- W=1 與 W=2、group=4,096 與 8,192 的固定矩陣；
- writer exclusive phases、WAL nested phases 與 `StateMachine::apply` nested phases；
- profile off/on bias、actual commands/group、queue/group wait、CPU、context switch 與 tail；
- direct WAL、StateMachine isolated ceiling、metrics 與 runtime handoff 的同環境交叉驗證；
- WAL replay、EngineSeq、results/events、book state 與 durable boundary correctness；
- root-cause report 與唯一下一個同步路徑優化候選。

### 3.2 Out of scope

- Publisher replay、EventSink publish、cursor persistence 或 Publisher worker batching；
- Completion worker batch drain、callback parallelism或queue redesign；
- W=4 production selection或新的 prepare thread pool；
- group=16,384 或更大 group；
- WAL format、CRC coverage、fsync policy、RPO、acknowledgement boundary變更；
- apply-before-fsync、async WAL、multiple in-flight fsync、`io_uring`、direct I/O；
- Snapshot／retention optimization、多 shard／多 instrument scaling；
- 通用 tracing framework、通用 executor、第三方 benchmark dependency；
- 在根因確認前修改 production defaults。

Publisher lag、Completion queue depth仍需觀察。若它們觸發 storage pressure、capacity wait、timeout
或 failure，該輪不能宣稱是純 writer ceiling；但本階段不藉此修改 downstream worker。

## 4. 既有能力與是否需要修改

| 同步 phase | 現有證據來源 | 本階段必要修改 |
| --- | --- | --- |
| group collect／wait | `WriterGroupProfile` | 無；重新量測並分開 active collect 與 wait |
| admission／CommittedCommand prepare | `WriterGroupProfile::admission_ns` | 無；以 derived ceiling 回報 |
| WAL prepare/serialize、plan/copy、publish/write | `WalAppendProfile` | 無；沿用現有 nested profile |
| fsync | `wal_sync_ns`、direct WAL、sync telemetry | 無；沿用最新 attribution 與本輪同步資料 |
| `StateMachine::apply` parent | `apply_ns`、pipeline state-machine workload | 無；重新量測 parent ceiling |
| apply 內部 matching／validation 等 | 尚無 production-path exclusive split | **需新增 opt-in nested profile** |
| Publisher notify 呼叫 | `publisher_notify_ns` | 無；只算通知成本，不測 worker |
| post-apply | `post_apply_ns`、既有 metrics workload | 無；低於門檻時不再拆分 |
| completion enqueue | `completion_enqueue_ns`、runtime handoff | 無；不測 worker 優化 |
| full invariant validation／snapshot | 非 steady-state 每-command path | 不納入本輪 writer ceiling |

只有 `StateMachine::apply` nested attribution 是新的 production-code instrumentation。其他缺口均可由
現有 raw fields 與固定報告公式補足。不得新增 admission-only、notify-only 或 enqueue-only 的假 queue
實作來製造較好看的 isolated RPS。

## 5. StateMachine opt-in 子階段設計

### 5.1 Internal data model

在 `src/domain/state_machine.hpp` 增加 internal-only profile type，名稱可依現有風格微調：

```cpp
struct StateMachineApplyProfile {
  std::uint64_t precheck_ns{};
  std::uint64_t book_apply_ns{};
  std::uint64_t state_update_ns{};
  std::uint64_t output_events_ns{};
  std::uint64_t producer_result_ns{};
  std::uint64_t incremental_validation_ns{};
  std::uint64_t commands{};
  std::uint64_t events{};
  std::uint64_t trades{};
};
```

各 phase 定義如下：

| Field | 包含 | 不包含 |
| --- | --- | --- |
| `precheck` | EngineSeq/config/payload/instrument/order lookup、pre-eviction與command validation | OrderBook mutation |
| `book_apply` | `add_new`／`amend_quantity`／`replace`／`cancel`，包含 matching | StateMachine index/result/event processing |
| `state_update` | terminal/tombstone、order location、active count、committed sequence更新 | event與producer result |
| `output_events` | `CommandResult` materialization與 `append_events()` | producer canonical bytes |
| `producer_result` | `save_producer_result()`，包含 canonical command bytes | transition validation |
| `incremental_validation` | tombstone suffix、post-eviction與 `validate_transition()` | full `validate_state()` |

`apply_ns` 保留為 ShardRuntime parent wall time。報告以：

```text
apply_children = precheck + book_apply + state_update
               + output_events + producer_result + incremental_validation
apply_unattributed = apply_parent - apply_children
```

計算 remainder，並檢查 underflow。任何 phase 因 command branch 不執行時可以為零，測試不得要求
duration 必須大於零。

### 5.2 API 與 normal-path 約束

保留既有：

```cpp
Result<ExecutionOutput> StateMachine::apply(const CommittedCommand& command);
```

新增 internal diagnostic entry，例如：

```cpp
Result<ExecutionOutput> StateMachine::apply_profiled(
    const CommittedCommand& command,
    StateMachineApplyProfile& profile);
```

兩者必須呼叫同一個 private implementation；禁止複製 matching、validation 或 state mutation 邏輯。
normal `apply()` 不得讀 clock、配置 sample、分配 profile object或呼叫 collector。只有已由
`ShardRuntime` 的 deterministic group sampling 選中的 group 使用 `apply_profiled()`。

若 command 走 reject/error branch，已完成的 phase仍累加，並保持原本的 Error／ExecutionOutput。
profile bookkeeping失敗不得改變 command outcome；counter aggregation使用 checked或saturating add。

### 5.3 Writer profile 整合

`WriterGroupProfile` 增加一個 `StateMachineApplyProfile` aggregate。`ShardRuntime` 只在
`profile_sampled=true` 時將 accepted commands逐筆送入 `apply_profiled()`；其他 group仍呼叫原
`apply()`。Benchmark collector沿用 writer-thread ownership，停止並 join後才讀取，不新增 lock或
shared registry。

`engine_writer_profile_benchmark` 輸出每個 apply child 的：

```text
total_ns
ns_per_command
apply_parent_share_percent
writer_service_share_percent
apply_unattributed_ns / ns_per_command / share
commands / events / trades
```

不在 production `MetricsSnapshot`、public `EngineConfig` 或 installed headers 加入這些欄位。

### 5.4 Nested profile 局部 bias control

apply children需要逐command取得多個clock；只比較整體writer profile off/on會因「每N個group才採樣」
而稀釋這項成本，不能證明sampled apply本身沒有被計時扭曲。因此benchmark增加一個diagnostic-only
開關，名稱可依CLI既有風格微調：

```text
--writer-apply-subprofile=off|on
```

規則：

- 預設`off`；
- 只有`engine_writer_hot_path_profile`且`--writer-phase-profile=on`時可以明確指定；
- `off`仍量既有`apply_ns` parent，但呼叫normal `StateMachine::apply()`；
- `on`才在被選中的sampled group呼叫`apply_profiled()`並輸出children；
- 其他workload、writer profile off、空值或未知值在建立data directory前以exit code 2拒絕；
- 不加入production config或環境變數控制。

局部bias使用相同group、W、sampling interval與command budget的paired median：

```text
apply_subprofile_bias =
  abs(apply_parent_ns_per_command(subprofile_on)
      - apply_parent_ns_per_command(subprofile_off))
  / apply_parent_ns_per_command(subprofile_off)
```

`<=5%`時children可作主要歸因，`>5%且<=10%`時降級為directional，`>10%`時不得用children選
optimization；必須降低nested clock密度或改用外部profiler。不得只用整體RPS bias取代這項局部gate。

## 6. Ceiling 與根因計算

### 6.1 每個同步 phase 的統一欄位

每輪以相同 sampled accepted-command denominator計算：

```text
phase_ns_per_command = phase_total_ns / sampled_accepted_commands
derived_phase_ceiling = 1e9 / phase_ns_per_command
target_utilization = 1,000,000 / derived_phase_ceiling
headroom_percent = (derived_phase_ceiling / 1,000,000 - 1) * 100
phase_share = phase_total_ns / writer_service_ns
```

`group_collect` 另回報：

```text
active_collect_ns = group_collect_ns - group_wait_ns
```

不得把 queue/group wait 換算成 CPU ceiling。WAL nested phase以 WAL parent denominator回報，apply
nested phase以 apply parent與writer service兩種 share回報；parent與children不得重複加總。

### 6.2 判定規則

| 條件 | 判定 |
| --- | --- |
| derived／isolated ceiling <1M/s，且跨輪穩定 | confirmed synchronous ceiling candidate |
| ceiling 介於1M與1.2M/s | insufficient headroom，仍需列入候選 |
| writer share >=10%，且A/B或isolated evidence同方向 | supported root cause |
| share <5% 且derived ceiling >=2M/s | stop；不再細分 |
| phase time高但只和慢輪storage tail同時出現 | tail contributor，不宣稱steady-state CPU根因 |
| profile bias >5% | phase share降級為directional |
| profile bias >10%或remainder >10% | attribution invalid，先修量測，不選優化 |

「root cause」至少需要同時具備 phase attribution 與一項獨立／因果證據，例如 isolated ceiling、
受控 W1/W2 差異或同輪 syscall/storage evidence。單一高百分比只能標示 supported candidate。

## 7. 固定正式矩陣

### 7.1 Writer matrix

在相同 source hash、ReleaseBenchmark binary、CPU affinity、filesystem與device執行：

```text
group:             4096, 8192
WAL prepare lanes: 1, 2
parallel threshold:4096
writer profile:    off, on
apply subprofile:  on（只存在於writer profile on case）
formal rounds:     5 per case
measured duration: >=20 seconds per round
instrument/shard:  1 / 1
fsync:             per_group
snapshot:          disabled for measured ceiling
```

W=1 是純同步 writer baseline；W=2 只用來量化目前 bounded prepare 對 writer wait／apply ranking 的
影響，不在本需求調整 worker數量。W=4與group=16,384不執行，因其只屬先前 diagnostic且無助於本次
最小根因判定。

producer lanes需足以維持 backlog，且所有case相同。每輪必須記錄configured與actual commands/group；
若 actual低於configured的90%，標示`supply-limited`，不得宣稱該group的Engine frontier。

在正式矩陣之前，另以group=4,096、W=1固定執行至少三組paired calibration：

```text
writer profile on + apply subprofile off
writer profile on + apply subprofile on
```

兩者使用相同sampling interval、command budget與交錯順序，依第5.4節決定nested attribution證據
等級。Calibration不納入formal throughput aggregate；若局部bias不通過，formal仍可完成top-level
writer attribution，但不能宣稱apply child root cause已確認。

### 7.2 Controls

同一環境執行或引用 identity完全一致的結果：

- public `engine_durable_single_instrument`：authoritative end-to-end ceiling；
- `wal_write_ceiling`：group=4,096／8,192、W=1／W=2 direct WAL control；
- `engine_pipeline_ceiling --pipeline-stage=state_machine`：相同command mix的isolated apply ceiling；
- `engine_pipeline_ceiling --pipeline-stage=metrics`：只在post-apply重新超過10%時作cross-check；
- runtime handoff：只驗證enqueue/handoff有足夠headroom，不優化Completion worker。

已有artifact只有在commit、binary、build flags、CPU affinity、filesystem/device與固定參數完全一致時
才能引用。歷史report可以作背景，不得和目前source的五輪sample合併計算。

### 7.3 Bias 與有效性

每個 `(group, W)` 比較profile off/on median：

```text
bias = abs(on_median - off_median) / off_median
```

- `<=5%`：可作主要attribution；
- `>5%且<=10%`：只作directional；
- `>10%`：該case的phase百分比無效，調高固定sample interval後重跑完整case。

sampling interval必須由預先calibration選定並固定，不能按結果事後更換。
Apply nested profile另依第5.4節做局部bias；兩個bias gate都必須在報告中呈現。

## 8. 正確性、錯誤處理與測試

### 8.1 Unit／integration tests

必要測試只有：

1. `apply()`與`apply_profiled()`對相同command sequence產生完全相同result、events與final state；
2. new／crossing new、amend、replace、cancel與reject branch至少各覆蓋一個；
3. profile counters每次呼叫前清零或正確累加，child sum不大於parent；
4. `commands/events/trades`與ExecutionOutput一致；
5. full `validate_state()`仍只依原snapshot/recovery boundary執行，不因profile新增到每筆command；
6. null/profile-off路徑沒有sample、clock與collector call；
7. apply-subprofile CLI的valid、empty、unknown、wrong-workload與wrong-parent-profile組合；
8. writer aggregate的apply parent、children與remainder不overflow／underflow；
9. WAL replay、EngineSeq continuity、durable head、book empty與completion exactly-once保持通過；
10. Release warnings-as-errors、GoogleTest、ASan／UBSan通過。

duration只能測關係與aggregate一致性，不得在CI斷言非零ns或RPS門檻。

### 8.2 Formal run失敗分類

- result/event/state/replay/counter不一致：`invalid_correctness`；
- profile child超過parent、overflow、sample denominator不一致：`invalid_attribution`；
- profile bias超過門檻：依第7.3節降級或重跑，不得挑快輪；
- publisher pressure、Completion capacity wait、timeout、memory/disk exhaustion：`resource_limited`；
- storage/fsync tail但correctness與資源均有效：保留為valid tail，不得刪除；
- source/binary/environment identity改變：整個矩陣不得合併。

## 9. 報告契約與完成條件

正式報告至少包含：

1. source/index/worktree/binary identity與完整環境；
2. correctness gates、calibration、smoke與formal status；
3. authoritative Engine、writer profile off/on、direct WAL與isolated StateMachine ceiling；
4. 每個top-level writer phase與apply child的五輪ns/command、derived ceiling、share、range；
5. actual commands/group、commands/sync、sync/s、queue/group wait、CPU與context switches；
6. profile bias、writer/apply unattributed remainder；
7. apply-subprofile局部bias與children證據等級；
8. Publisher lag與Completion depth只作有效性護欄的結果；
9. 每一候選的`confirmed`／`supported`／`unconfirmed`證據；
10. 1M/s與1.2M/s headroom差距；
11. 只選一個下一階段同步路徑優化，或在證據不足時明確寫`inconclusive`。

完成條件：

- production ordering、WAL bytes、durability、recovery與public API不變；
- normal `StateMachine::apply()`與profile-off writer無新增clock／sample allocation；
- writer現有exclusive phases與新增apply children能解釋parent，remainder不超過10%；
- 每個同步phase都有derived ceiling與證據等級，但低成本phase不被過度拆分；
- `StateMachine::apply`的主要子成本能被識別，或誠實標示inconclusive；
- Publisher／Completion worker服務邏輯沒有被修改；
- 報告可直接決定下一個且只有一個optimization design。

## 10. 預期修改範圍

必要修改預期落在：

```text
src/domain/state_machine.hpp/.cpp                 apply opt-in profile與exclusive boundaries
src/runtime/writer_profile.hpp                    aggregate apply nested profile
src/runtime/shard_runtime.cpp                     sampled group才呼叫apply_profiled
benchmarks/engine_writer_profile_benchmark.cpp    checked aggregation與輸出
benchmarks/order_book_benchmark.cpp               apply-subprofile CLI與validation
tests/unit或integration state-machine tests       normal/profiled等價與counter驗證
tests/integration/engine_writer_profile_test.cpp  parent/children/remainder契約
benchmarks/CMakeLists.txt                         smoke output contract（若現有regex需補欄位）
```

估計 production/internal code約90～160行，benchmark／CLI約60～110行，tests約80～140行，
總計約230～410行。正式procedure與report另計。若實作明顯超過此範圍，應先檢查是否誤加入新的
benchmark framework、downstream worker優化或StateMachine第二套邏輯。

## 11. 關鍵取捨與限制

- 以derived ceiling覆蓋所有同步phase，可回答headroom而不為微小函式建立失真的for-loop benchmark；
  只有已達門檻的`StateMachine::apply`新增nested profile。
- W=1可看純writer成本，W=2可看目前同步等待bounded helpers後的實際排序；兩者都不代表本階段要
  優化async workers。
- alternating crossing workload只代表目前單一交易對目標；deep book、多maker trade與不同command
  mix可能改變matching/event/validation比例，報告不得外推。
- local ext4與目前CPU結果不能外推到production storage；storage tail保留，但本需求不再重新追查
  fsync底層歸因。
- full invariant validation與snapshot是週期性工作，應另以soak/snapshot測試處理；把它混入每筆
  Engine hot path會同時違反現況與量測目的。
- ceiling workload不是fixed-rate latency SLO。完成同步根因定位後，仍需以另一份設計驗證實際arrival
  rate下的p99/p99.9與downstream drain。
