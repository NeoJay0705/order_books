# StateMachine／WAL Component Ceiling benchmark unstaged review 必要修正

## 1. 結論與修改範圍

目前 unstaged implementation 的整體方向正確，應保留下列既有成果：

- fresh-WAL epoch 的正式 append-return 路徑；
- bounded deterministic latency sampling 與全量 total／max；
- rotation sync、header write／sync、directory sync 的 profile timing；
- measured process counters；
- per-case CV rejection 後繼續收集 matrix；
- production `256 MiB` segment、WAL format、fsync 與 recovery semantics。

但尚有五項會影響結果有效性或違反既有必要修正文件的問題。本文件只定義這五項的最小修正，不加入新的效能
優化、production metrics、benchmark executable、`perf`、`fio` 或 worker 分析。

修改範圍限於：

- `benchmarks/order_book_benchmark.cpp`；
- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- `benchmarks/CMakeLists.txt`及必要的輕量測試；
- 必要時在`src/persistence/wal.cpp`／`wal.hpp`補測試可觀察性，但不得改變production行為；
- 對應設計與操作文件的輸出欄位說明。

## 2. 修正一：rotation attribution只量單一目標group

### 2.1 現況問題

目前runner以一整個segment容量的commands執行`phase_profile=on`：

```text
fresh WAL
  -> small warmup + sync
  -> profile數百萬次append
  -> 其中最後某次append觸發rotation
```

這會造成：

- control與trigger的p50／p99主要描述一般append，不是segment尾端的目標操作；
- trigger round的profile total混入大量非rotation append；
- control與trigger不是位置相近的單一group比較；
- diagnostic被錯誤套用正式case的30秒duration gate；
- 已有一般profile round仍會再次量測大量相同append，造成冗餘I/O與時間。

必要語意應為：

```text
open fresh production-size WAL
  -> warmup
  -> sync warmup
  -> setup-fill（不計時、不profile、不sync）
  -> capture counters/segment position
  -> profile exactly one b1 group
  -> capture counters/segment position
  -> final sync + reopen + replay（量測外）
```

setup-fill必須發生在warmup sync之後，且在trigger group前不得再次sync，才能保留dirty old segment並量到rotation時
的old-segment sync成本。

### 2.2 Benchmark介面

在既有`wal_write_ceiling`增加僅供diagnostic使用的互斥模式：

```text
--wal-rotation-diagnostic=control|trigger
```

最小CLI限制：

- 只允許`--workload=wal_write_ceiling`；
- 只允許`--wal-sync=none`；
- 必須搭配`--wal-phase-profile=on`；
- 本輪runner固定`--wal-group-size=1 --iterations=1`；
- 不得與`--wal-no-rotation-epoch-commands`同時使用；
- 非法組合以明確`error_code`及exit code 2拒絕。

不新增public library API，也不允許正式runner改production segment size。

### 2.3 精確建立control與trigger位置

benchmark使用production `256 MiB` segment，從實際WAL offset與實際frame byte delta計算setup-fill，不使用
calibration平均值猜測是否rotation。

建議做法：

1. WAL建立後記錄header後offset；
2. 執行既有warmup並`Wal::sync()`；
3. 以一筆與目標command相同格式的setup append取得實際frame bytes；
4. setup append仍屬量測外，且之後不得sync；
5. 依`segment_size - active_offset`計算還可容納的完整frame數；
6. setup-fill可使用較大的`append_batch()`降低準備時間，但最後必須停在精確位置；
7. target group永遠只有一筆command。

兩種模式的目標位置：

- `control`：target append前仍至少能容納一個frame；target append後不得rotation；
- `trigger`：target append前剩餘空間小於一個frame；target append必須rotation一次。

不得先執行trigger再從同一WAL取control。兩者各自使用fresh WAL及獨立process，避免rotation或cache state互相污染。

### 2.4 Measured boundary與驗證

只在target group周圍執行：

```text
capture measured process/Dirty/Writeback counters
start outer timer
append_batch_profiled(one command)
stop outer timer
capture segment/counters
```

必要驗證：

- `commands=1`、`groups=1`、`profiled_commands=1`、`profiled_groups=1`；
- control：`measured_segment_rotations=0`且`rotation_triggered=false`；
- trigger：`measured_segment_rotations=1`且`rotation_triggered=true`；
- trigger不得只檢查`>=1`；超過一次同樣是invalid-run；
- target append前後的segment id與offset符合相應模式；
- final sync、reopen、完整replay及sequence/content驗證通過；
- profile subphase只來自該target group。

輸出使用獨立名稱，例如：

```text
wal_rotation_diagnostic case=control|trigger
completion_boundary=append_batch_return
measured_groups=1
```

單樣本的p50／p99／p99.9／max可以相同，但報告必須明示`sample_count=1`，不得將它描述成分布。

### 2.5 Runner修改

`component_run_rotation_attribution()`改為每輪各執行一次control與trigger專用模式，共五輪：

```text
for round in 1..5
  run control --iterations=1
  validate exact zero rotation
  run trigger --iterations=1
  validate exact one rotation
done
```

移除rotation diagnostic的`component_validate_duration()`；30秒gate只適用正式StateMachine、append ceiling及
durable frontier，不適用單次diagnostic。setup-fill所花process wall time仍由GNU time與host observers保留，但不
冒充target group latency。

`derived/component-rotation-attribution.tsv`每輪至少保留：

```text
case
round
rotation_triggered
expected_rotations
measured_rotations
segment_id_before/after
segment_offset_before/after
dirty_bytes_before/after
writeback_bytes_before/after
append_latency_us
rotation_total_us
old_segment_sync_us
header_write_us
header_sync_us
directory_sync_us
replay_verified
```

保留五輪raw rows；跨輪median／min／max由報告彙整，不把diagnostic rows納入append ceiling CV或median。

## 3. 修正二：補齊並強制驗證measured-resource契約

### 3.1 `measured_wal_write_calls`不得為`na`

目前正式durable profile-off會輸出`measured_wal_write_calls=na`，不符合必要resource evidence。

direct-WAL benchmark的measured window內沒有stdout或其他應用層write；Linux`/proc/self/io`的`syscw`可作為實際
write-family syscall數。最小修正為：

- process I/O snapshot有效時，`measured_wal_write_calls=measured_syscw`；
- snapshot無效時輸出`na`，並由正式Linux runner將case判為`invalid-run`；
- profile-on仍保留`profiled_data_write_calls`作WAL data-chunk歸因；
- 文件明確區分`measured_wal_write_calls`是process measured-window write syscall數，
  `profiled_data_write_calls`是WAL profile所計的data chunk write數。

不要為這項需求在production WAL hot path新增atomic counter或常駐metrics。

### 3.2 Runner必要欄位gate

擴充`component_validate_output()`或新增小型`component_validate_measured_resources()`。所有正式WAL case必須讓
以下欄位各出現恰好一次：

- `measured_user_seconds`、`measured_system_seconds`：非負有限數字；
- `measured_voluntary_context_switches`、`measured_involuntary_context_switches`：非負整數；
- `measured_syscw`、`measured_wchar`、`measured_write_bytes`、
  `measured_cancelled_write_bytes`：非負整數，不接受`na`；
- `measured_dirty_bytes_before/after`、`measured_writeback_bytes_before/after`：非負整數；
- `measured_wal_write_calls`：非負整數，且direct-WAL case應等於`measured_syscw`；
- `measured_wal_sync_calls`：append為0，durable為planned groups；
- `latency_sample_stride`、對應sample count：正整數且不超過measured groups。

另驗證正式輸出名稱：

- no-rotation append必須是`wal_append_no_rotation`；
- durable與一般profile必須是`wal_write_ceiling`；
- rotation diagnostic必須是專用名稱及case。

任何必要欄位缺失、重複、`na`、格式錯誤或與plan不一致，都必須立即：

```text
result=invalid-run
reason=<field-specific-reason>
```

這屬counter／artifact failure，不得降級成`rejected-unstable`繼續產生ceiling。

## 4. 修正三：逐epoch驗證planned WAL byte delta

### 4.1 建立精確frame byte plan

no-rotation epoch不能只檢查segment count。每個fresh WAL在warmup前後記錄`Wal::size_bytes()`：

```text
warmup_frame_bytes = size_after_warmup - size_before_warmup
frame_bytes_per_command = warmup_frame_bytes / warmup_commands
```

必要條件：

- warmup commands大於0；
- warmup期間沒有rotation；
- `warmup_frame_bytes % warmup_commands == 0`；
- `frame_bytes_per_command > 0`；
- workload使用相同固定格式command，因此每筆frame size必須一致。

每個epoch在進入measured phase前計算：

```text
planned_epoch_wal_bytes = this_epoch_commands * frame_bytes_per_command
```

並使用checked multiply/add避免overflow。

### 4.2 Measured後精確驗證

每個epoch都要求：

```text
actual_epoch_wal_bytes = ending_wal_bytes - starting_wal_bytes
actual_epoch_wal_bytes == planned_epoch_wal_bytes
segment_count_after == segment_count_before
segment_id_after == segment_id_before
measured rotations == 0
```

最後聚合後再驗證：

```text
measured_wal_bytes == sum(planned_epoch_wal_bytes)
```

輸出新增或校正：

```text
frame_bytes_per_command=<exact integer>
planned_wal_bytes_delta=<exact integer>
wal_bytes_delta=<exact integer>
wal_byte_plan_verified=true
```

runner必須驗證`wal_byte_plan_verified=true`且planned／actual相等。不要把calibration的浮點
`average_wal_bytes_per_command`當成正式exact byte gate。

## 5. 修正四：消除重複欄位並正確聚合counter validity

### 5.1 Dirty／Writeback只輸出一次

no-rotation輸出目前重複：

```text
measured_dirty_bytes_after
measured_writeback_bytes_after
```

只保留boundary snapshot欄位：

```text
measured_dirty_bytes_before
measured_dirty_bytes_after
measured_writeback_bytes_before
measured_writeback_bytes_after
```

`ProcessCounters`的累積結構只聚合CPU、context switch及process I/O delta；Dirty／Writeback是host gauge，不做
跨epoch加總，只保存第一個epoch before與最後一個epoch after。

### 5.2 所有epoch都有效才可輸出有效counter

目前以logical OR聚合`io_valid`，會讓部分epoch讀取失敗時仍輸出不完整總數。改為明確的accumulator初始化語意：

```text
first epoch:
  aggregate = epoch_delta
later epochs:
  aggregate counters += epoch_delta counters
  aggregate.rusage_valid &= epoch_delta.rusage_valid
  aggregate.io_valid &= epoch_delta.io_valid
```

建議在`ProcessCounters`增加`rusage_valid`，使`getrusage()`失敗不會被當成合法的零CPU。`meminfo_valid`由首個before
與末個after snapshot共同決定，不使用OR。

累積uint64 counter仍須checked-add；若overflow則以明確error停止，不輸出飽和值或部分值。

## 6. 修正五：補齊必要測試，不執行重型正式campaign

### 6.1 C++／CTest

新增或擴充輕量測試：

1. no-rotation smoke驗證：
   - output name為`wal_append_no_rotation`；
   - rotation為0；
   - planned／actual WAL bytes相等；
   - resource欄位唯一且不是`na`；
   - replay通過。
2. rotation profile subphase以小segment的WAL測試驗證：
   - control不rotation；
   - trigger恰好rotation一次；
   - rotation、old sync、header write／sync、directory sync counter存在；
   - durability與replay仍正確。
3. bounded sampler使用synthetic latency values測試，不寫大量WAL：
   - group count小於／等於上限時stride為1；
   - group count超過上限時stride大於1；
   - sample count有界；
   - aggregate total、observed與max仍是全量精確值。
4. 新CLI互斥與非法組合各有明確negative test。

若目前helper只存在anonymous namespace，可抽成benchmark-local小型helper/header供測試；不要把sampling helper加入
public library API。

### 6.2 Runner contract

fake benchmark happy path必須輸出完整且唯一的measured-resource與byte-plan欄位。至少新增下列failure scenarios：

- no-rotation仍輸出`wal_write_ceiling`時拒絕；
- required resource欄位缺失、重複或`na`時拒絕；
- WAL write calls與`syscw`不一致時拒絕；
- planned／actual WAL bytes不一致時拒絕；
- rotation control不是0時拒絕；
- rotation trigger不是精確1時拒絕；
- rotation diagnostic意外使用多於一個measured group時拒絕；
- 任一上述failure不得產生`valid-component-ceiling`。

保留既有CV rejected後仍執行fsync/profile matrix的測試；counter或artifact failure仍必須立即停止，兩者不可混用。

### 6.3 實際小型integration gate

在正式campaign前，以新run root執行一次production-size b1 control與trigger，各只執行一輪，確認：

- exact 0／1 rotation；
- target measured group只有1；
- subphase fields完整；
- replay通過；
- 無殘留benchmark／observer process。

這是正確性integration gate，不要求30秒、不計入正式五輪結果，也不與舊run root合併。

## 7. 明確不做

以下不是本輪必要修正：

- 修改production WAL segment size、format、rotation或fsync順序；
- 移除rotation durability；
- 為write-call計數加入production atomic hot-path metric；
- 修改StateMachine、Engine、Publisher或Completion worker；
- 新增`perf`、`fio`、O_DIRECT、io_uring或storage tuning；
- 改變5% CV、3% profile bias或正式case五輪規則；
- 為了通過測試而刪除慢輪、補跑替換或混用run root；
- 重構與上述五項無關的runner或benchmark程式碼。

## 8. 建議實作順序

1. 先修正counter validity、重複輸出及exact WAL byte gate；
2. 實作專用rotation diagnostic benchmark模式；
3. 更新runner的rotation流程及required-field gate；
4. 更新fake benchmark與runner failure contracts；
5. 補C++／CTest的byte plan、rotation subphase及bounded sampler測試；
6. 更新設計／操作文件中rotation measured boundary及欄位定義；
7. build與完整CTest通過後，才執行一輪小型rotation integration gate；
8. review通過後另行決定是否執行正式長時間component campaign。

## 9. 完成條件

修正完成必須同時滿足：

- rotation control／trigger各只量一個target group；
- control精確0 rotation、trigger精確1 rotation；
- setup-fill不在measured/profile boundary內，且trigger前未再次sync；
- durable profile-off的`measured_wal_write_calls`不是`na`；
- 所有required measured-resource欄位唯一、有效並由runner強制驗證；
- 每個no-rotation epoch及聚合後的planned／actual WAL bytes完全一致；
- no-rotation輸出沒有重複欄位，所有epoch counter validity採AND語意；
- bounded sampling、rotation、resource與byte-plan failure contracts均有測試；
- build、完整CTest、runner contract與小型rotation integration gate通過；
- `git diff --check`與`git diff --cached --check`通過；
- staged diff在修正前後完全不變。

預估必要修改量約210至300行。這個估計包含實作與測試，不包含本文件，也不包含任何production效能優化。
