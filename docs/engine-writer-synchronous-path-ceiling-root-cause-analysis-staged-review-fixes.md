# Engine writer 同步路徑 ceiling／根因分析 staged review 必要修正

## 1. 目的與結論

本文件記錄 `docs/engine-writer-synchronous-path-ceiling-root-cause-analysis-design-review.md`
與目前 staged changes 對照後，在正式根因量測前必須完成的最小修正。

現有實作方向正確：production ordering、WAL durability boundary、Publisher／Completion worker、public
API 與 production defaults 均未改變；新增內容也維持在 opt-in writer diagnostics、benchmark CLI、輸出與
測試範圍內。仍有四項會影響歸因或正式報告正確性的必要調整：

1. reject branch 必須依 exclusive phase 定義歸因，不能整段落入 `precheck` 或 remainder；
2. 零耗時／未執行 phase 不能被輸出為 `0 commands/s` ceiling；
3. apply child 與 remainder 的輸出必須符合 subprofile gate 與報告契約；
4. 正式結果必須輸出 measured-only commands/group 與當輪 Publisher lag validity signals。

本修正不加入新的 benchmark framework、不改變 command mix、不調整 group size、prepare workers、fsync
policy 或任何 production optimization，也不處理 Publisher／Completion worker 的服務邏輯。

## 2. 修正一：讓 reject branch 維持 exclusive phase attribution

### 2.1 問題

`StateMachine::apply_impl()` 以 RAII timer 開始 `precheck`，成功路徑會在進入 `OrderBook` 前停止它；但
目前所有 business reject 仍直接呼叫既有 `reject()`：

```text
precheck timer starts
  -> validation detects a business rejection
  -> reject() builds result/event
  -> reject() updates committed state
  -> reject() saves producer result
  -> reject() evicts tombstones and validates transition
  -> function returns and precheck timer stops
```

因此，book apply 前的 reject 會把 result/event、state update、producer result 與 validation 全算入
`precheck_ns`；若 reject 發生在 book apply 之後，`precheck` 與 `book_apply` 已停止，reject finalization
反而全部落入 `apply_unattributed`。同一段工作會因 reject 發生位置不同而得到不同歸因。

這不影響 command outcome，但違反設計中各 child phase 的 exclusive boundary，會污染包含 reject 的
command mix 與之後的根因判定。

### 2.2 最小修改方法

保留單一 `apply_impl()`，不要複製 reject 或 matching 邏輯。讓 private `reject()` 接受 nullable internal
profile pointer，例如：

```cpp
Result<ExecutionOutput> reject(const CommittedCommand& command,
                               ErrorCode code,
                               StateMachineApplyProfile* profile);
Result<ExecutionOutput> reject(const CommittedCommand& command,
                               ErrorCode code,
                               OrderId order_id,
                               StateMachineApplyProfile* profile);
```

在 `apply_impl()` 取得 `request` 後建立唯一的 local helper：

```cpp
const auto reject_command = [&](const ErrorCode code) {
  precheck.stop();
  return reject(command, code, request.order_id, profile);
};
```

將 `apply_impl()` 中所有 business `return reject(command, code)` 改為
`return reject_command(code)`。`precheck.stop()` 已具 idempotent 語意，因此 book apply 已開始或完成後
再呼叫不會重複累計。Corrupt WAL／snapshot 等直接回傳 `Error` 的路徑維持既有語意，不需強迫產生
business rejection output。

### 2.3 reject 內的 phase 邊界

在同一份 reject implementation 中依序量測：

```text
output_events
  -> construct CommandResult
  -> construct command_rejected Event

state_update
  -> advance last_committed_engine_seq
  -> update logical_retention_time

producer_result
  -> save_producer_result(), including canonical bytes

incremental_validation
  -> evict_tombstones()
  -> validate_transition()
```

`book_apply` 在 validation reject 時維持零；若 `OrderBook` 已執行後才回報 business error，已完成的
`book_apply` 時間保留，reject finalization仍依上述四個 phase 分開累加。

每個 timer target 都使用：

```cpp
profile == nullptr ? nullptr : &profile->field
```

所以 normal `apply()` 不呼叫 clock、不建立 `StateMachineApplyProfile`、不配置 sample，也不呼叫
collector。不要為 reject 另建第二套 `ExecutionOutput` 邏輯或 public API。

### 2.4 測試與驗收

擴充既有 profiled reject unit test：

- normal／profiled reject 的 `CommandResult` 與 events 完全一致；
- `commands == 1`、`events == output.events.size()`、`trades == 0`；
- 不要求任一 duration 大於零，避免 clock resolution 造成 flaky test；
- integration hierarchy 仍驗證 apply children 不大於 parent。

驗收重點是 source boundary 與 outcome 等價，不使用固定 ns 或比例作 functional assertion。

預估修改：`state_machine.hpp/.cpp` 與 unit test 約 45～65 行異動。

## 3. 修正二：零耗時 phase 不得表示成零 ceiling

### 3.1 問題

目前 ceiling helper 將 `ns_per_command <= 0` 轉成：

```text
derived_ceiling_commands_per_second=0
headroom_percent=0
```

零值實際可能表示該 branch 未執行，例如沒有 WAL rotation；也可能是短 smoke 中 timer resolution 無法
觀測。它不代表該 phase 的服務能力是 0。若沿用目前輸出，正式分析套用「ceiling < 1M/s」規則時，
反而會把未執行 phase 選成同步瓶頸。

### 3.2 最小輸出契約

不要以 `0` 或極大 sentinel 假裝存在可比較的 ceiling。每個 ceiling group增加明確 observation field：

```text
<phase>_ceiling_observed=true|false
```

規則如下：

- `total_ns == 0` 或 denominator 為零：只輸出 `ceiling_observed=false`，不輸出該 phase 的
  `derived_ceiling_commands_per_second` 與 `headroom_percent`；
- total與denominator皆有效：輸出 `ceiling_observed=true`、derived ceiling與headroom；
- raw `total_ns` 與 `ns_per_command` 可維持原欄位，方便確認零值原因；
- report parser只對`ceiling_observed=true`的phase套用1M／1.2M門檻。

不要輸出字串`inf`後再依平台解析；explicit observed flag對CMake、shell與後續報告都較穩定。

將相同規則集中在既有 emit helper，避免 top-level、WAL 與 apply child各自實作不同的零值語意。

### 3.3 測試與驗收

在短 benchmark smoke 使用自然為零的 phase，或直接檢查 smoke output：

- 零 phase含`ceiling_observed=false`；
- 不得出現該phase的`derived_ceiling_commands_per_second=0`；
- 有觀測資料的phase含`ceiling_observed=true`與數值ceiling；
- 不對實際RPS或duration設門檻。

預估修改：benchmark output helper與smoke contract約10～20行。

## 4. 修正三：補齊 apply child／remainder 輸出契約

### 4.1 問題

目前 profile summary 無論 `--writer-apply-subprofile=off|on` 都輸出 apply child欄位。off case的
children與counts全為零，並進一步產生無效的零ceiling；這與設計「on才輸出children」不一致。

此外，`apply_unattributed`目前只有total ns，缺少正式報告與10% validity gate所需的：

```text
apply_unattributed_ns_per_command
apply_unattributed_apply_parent_share_percent
apply_unattributed_writer_service_share_percent
```

### 4.2 最小修改方法

先完成既有 checked addition 與 hierarchy validation，再只在subprofile on時計算：

```cpp
const auto apply_unattributed = totals.apply_ns - apply_children;
```

輸出分成兩部分：

1. top-level `apply_ns`、`apply_ns_per_command`、derived ceiling與writer share在profile on但
   subprofile off時仍照常輸出；
2. `apply_precheck`到`apply_incremental_validation`、apply child counters及remainder只在
   `apply_subprofile == true`時輸出。

subprofile on時補齊：

```text
apply_unattributed_ns
apply_unattributed_ns_per_command
apply_unattributed_apply_parent_share_percent
apply_unattributed_writer_service_share_percent
```

分母規則固定為：

- per-command：`totals.apply.commands`；
- apply parent share：`totals.apply_ns`；
- writer service share：`totals.writer_service_ns`。

`apply_children > apply_ns`仍立即回報`profile_hierarchy_invalid`，不得clamp remainder。若subprofile on
但`apply.commands != accepted_commands`，仍回報`apply_profile_count_mismatch`。

### 4.3 測試與驗收

- subprofile-on smoke必須找到六個children、commands/events/trades與三個remainder欄位；
- explicit subprofile-off case仍有top-level apply欄位，但沒有apply child/remainder欄位；
- existing invalid、empty、wrong workload與wrong parent profile測試維持exit code 2；
- 不加入通用output schema或第三方parser，只擴充現有CMake smoke checker即可。

預估修改：benchmark formatting與CMake smoke約15～30行。

## 5. 修正四：輸出 measured-only group occupancy 與 Publisher lag

### 5.1 問題

正式矩陣以actual commands/group是否達configured的90%判斷`supply-limited`。目前summary使用runtime
啟動後的累積`wal_group_commands / wal_group_commits`，其中同時包含warmup與measured phase；短warmup
partial group可能改變正式measured occupancy。

同時，runner已取得`MetricsSnapshot`，但沒有輸出其中的Publisher lag events/bytes/age。正式報告因此
無法依設計證明該輪未觸發downstream pressure validity guard。

### 5.2 measured-only counter delta

warmup完成、measured phase開始前取得一次metrics snapshot：

```cpp
const auto metrics_before_measured = runtime->metrics();
```

measured完成後取得既有snapshot，先檢查：

```text
after.wal_group_commands >= before.wal_group_commands
after.wal_group_commits  >= before.wal_group_commits
```

若counter regressed，回報明確的benchmark validation error並使run失敗。否則計算：

```text
measured_wal_group_commands = after - before
measured_wal_group_commits  = after - before
measured_actual_commands_per_group = commands / commits
```

summary中的正式occupancy欄位改用measured delta。全程snapshot仍保留給`expected_total`、WAL replay與
其他correctness檢查，不要把correctness denominator改成measured-only。

### 5.3 Publisher validity fields

在summary輸出measured結束時snapshot中的：

```text
publisher_lag_events
publisher_lag_bytes
publisher_lag_age_ns
```

這三個欄位只作validity evidence，不加入writer phase加總，也不量Publisher worker service time。非零lag
本身不必直接使functional test失敗；正式報告需結合publisher warning/critical、storage pressure、timeout
與drain結果判斷是否為`resource_limited`。

Completion有效性仍使用既有`completion_max_queue_depth`與queue residence，不新增Completion worker
instrumentation。

### 5.4 測試與驗收

- benchmark smoke輸出measured commands、commits及actual commands/group；
- warmup command數與measured command數刻意不同，確認occupancy使用measured delta；
- summary包含三個Publisher lag十進位欄位；
- replay size、EngineSeq、book empty、completion exactly-once與total metrics correctness仍通過；
- 不對lag固定為零作CI斷言，避免背景worker排程造成flaky test。

預估修改：benchmark runner與smoke約15～25行。

## 6. 必要的等價性測試補強

設計要求normal與profiled command sequence產生相同完整final state。目前測試只比對
`last_committed_engine_seq`、`active_order_count`與`validate_state()`，不足以覆蓋books、order locations、
producer states、tombstones與configuration state。

使用既有canonical snapshot codec比較完整狀態，不新增`ShardState::operator==`：

```cpp
EXPECT_EQ(storage::encode_state(normal.state()),
          storage::encode_state(profiled.state()));
```

`encode_state()`已對unordered collections建立穩定順序，適合用來驗證語意等價。將events/trades一致性
移入共用`apply_pair` helper，使new、crossing new、amend、replace、cancel與reject每個branch都驗證：

```text
profile.events == ExecutionOutput.events.size()
profile.trades == count(EventType::trade)
```

這只是補齊主設計已要求的correctness gate，不擴增production API。預估約10～20行。

## 7. 實作順序

1. 先修正profile-aware reject與exclusive timer boundaries；
2. 補完整state、events與trades等價測試；
3. 修正zero-duration ceiling semantics；
4. 將apply child/remainder輸出置於subprofile-on gate並補齊欄位；
5. 以metrics delta輸出measured-only group occupancy；
6. 補Publisher lag summary與smoke contract；
7. 執行correctness與sanitizer驗證；
8. review通過後才執行正式五輪矩陣與root-cause report。

## 8. 驗證方式

修正完成後依序執行：

```text
git diff --check
git diff --cached --check
ReleaseBenchmark warnings-as-errors build
StateMachine targeted GoogleTest
EngineWriterProfile targeted GoogleTest
benchmark CLI／smoke CTest
ReleaseBenchmark full CTest
ASan／UBSan full correctness CTest
```

確認correctness後，再依主設計執行：

- group=4,096、W=1的apply subprofile off/on paired calibration至少三組；
- group=4,096／8,192、W=1／W=2、profile off/on各五輪；
- 每輪至少20秒且固定sampling interval；
- 同環境direct WAL、isolated StateMachine與authoritative Engine controls；
- 最後才產出root-cause report與唯一下一個同步路徑optimization候選。

本修正文件不授權變更staging；實作與測試期間仍不得執行`git add`、`git reset`、`git restore --staged`
或其他會改變index的操作。

## 9. 修改規模與非目標

預估必要異動：

- reject attribution與StateMachine test：約45～65行；
- zero ceiling與apply output contract：約25～50行；
- measured-only counters與Publisher lag：約15～25行；
- 其餘smoke／完整state等價測試：約20～30行；
- 合計約105～170行異動。

若修正明顯超過此範圍，應先檢查是否誤加入以下非目標：

- Publisher replay、publish、cursor persistence或batching優化；
- Completion worker drain、callback parallelism或queue redesign；
- WAL format、CRC、fsync、RPO或acknowledgement boundary變更；
- 新的prepare worker數、group size或production default；
- 通用profiling／tracing framework、外部benchmark dependency；
- 依尚未完成的根因報告直接修改production hot path。

完成上述四項必要修正並通過correctness gates後，staged implementation才足以產生可用於下一階段決策
的同步路徑ceiling與root-cause證據。
