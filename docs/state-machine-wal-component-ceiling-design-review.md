# StateMachine／WAL Component Ceiling 壓測設計 Review

## 1. Review 結論

本文件 review 下列需求：

- 以固定 command 數量分別量測 `StateMachine::apply()` 各種 command scenario 的 throughput、平均成本與 CPU
  使用量；
- 以固定總 command 數量比較 WAL sequential singleton append 與不同 `append_batch()` 大小；
- 以不同 batch size量測 `append_batch() + fsync()` 的 throughput／latency frontier及I/O行為；
- 每個正式 case 開始前，先確認相關 CPU 或 storage 足夠 idle；
- 只調整 benchmark、runner、測試與文件，不修改 production行為。

需求方向合理。現有 `engine_pipeline_ceiling/state_machine` 與 `wal_write_ceiling` 已提供大部分基礎能力，
不需要新增 production API、WAL format或另一個 benchmark executable。但目前實作不能原樣產生所需結論，
必須做以下四項必要修正：

1. StateMachine stage改為固定 command 數量的整段量測，並補齊可維持有界狀態的 command scenarios；不得把
   計時分組描述成業務 batch。
2. WAL append正式 ceiling與phase profiling分開；正式 ceiling關閉phase profile，避免內部clock sampling污染
   結果。
3. WAL matrix加入singleton baseline，固定append case的總command數；fsync case則依目標執行時間校準，避免
   singleton fsync因固定大量command而超時。
4. 增加不依賴`perf`／`fio`的component-only runner範圍，套用CPU／I/O preflight、affinity、cooldown、重複
   輪次與artifact gate。

完成上述修正後，設計可滿足本次需求。除此以外的完整Engine、Publisher、Completion、網路downstream、
storage tuning與production優化都不屬於本次範圍。

## 2. 需求理解與合理假設

### 2.1 本次必須回答

1. 不含WAL與runtime worker時，各StateMachine scenario的commands/s與CPU成本是多少？
2. 相同總command數下，singleton與不同`append_batch()`大小如何影響append-return throughput？
3. 不同commands/fsync下，durable WAL的commands/s、fsync/s及p50／p99／p99.9／max latency如何變化？
4. 正式case開始前是否有足夠的CPU與storage idle證據？
5. 結果是否可重複，且沒有被observer、fixture counter或環境漂移污染？

### 2.2 名詞與量測邊界

- **StateMachine command ceiling**：從第一筆measured command進入`StateMachine::apply()`所在的scenario loop，
  到固定command數全部完成的component workload throughput。它不包含WAL、Engine queue或worker。
- **append-return ceiling**：`append_batch()`回傳前的direct WAL workload；`sync=none`表示measured loop不主動
  `fsync`，不表示zero-I/O或純記憶體操作。
- **durable WAL frontier**：每組`append_batch(batch_size)`後執行一次`Wal::sync()`，以sync完成作為group
  completion boundary。
- **平均command latency**：`measured_elapsed / measured_commands`。StateMachine固定command測試不宣稱
  per-command p50／p99；沒有逐command sampling便不能從總時間推導percentile。
- **group latency**：WAL單一group的append、sync或兩者合計時間，可輸出p50／p99／p99.9／max。

### 2.3 合理假設

- Linux page cache是實際WAL路徑的一部分；`sync=none`仍可能觸發background writeback及kernel
  dirty-page throttling，因此WAL append也必須監測I/O。
- StateMachine scenario必須使用合法、單調的engine／producer sequence，並讓active orders與資料結構大小
  維持有界，避免把state持續膨脹誤判為command成本。
- 正式結果使用ReleaseBenchmark binary、固定CPU affinity、相同filesystem與相同binary hash。
- 目前host不能使用`perf`不妨礙CPU usage量測，但本次不能據此做函式級CPU hotspot歸因。
- 固定command數適合StateMachine與append-return比較；singleton fsync若使用相同大量command可能耗時過長，
  因此durable matrix允許各case依目標時間校準command數。

## 3. Scope

### 3.1 In scope

- 擴充既有`engine_pipeline_ceiling/state_machine` scenario；
- 調整既有`wal_write_ceiling`的執行矩陣與ceiling/profile邊界；
- component-only affinity runner、idle gate、artifact與報告；
- correctness、counter、replay、duration、CV與observer overhead驗證；
- benchmark及runner的單元／整合測試。

### 3.2 Out of scope

- Engine ingress／admission、group-commit delay或end-to-end completion latency；
- Publisher、Completion worker及其queue／batching優化；
- 真實network、broker、downstream或EventSink成本；
- production defaults、WAL格式、matching語意或public API變更；
- `fio` storage ceiling、raw device benchmark、drop cache、sysctl、CPU governor、IRQ或filesystem tuning；
- 直接承諾1M commands/s或將component ceiling當成production SLA。

Admission若仍需分析，沿用既有Writer profile的admission phase；本次不建立模擬成功admission的新
microbenchmark，避免複製Engine內部邏輯。

## 4. 現況與必要差距

| 現有能力 | 可保留內容 | 必要差距 |
| --- | --- | --- |
| `engine_pipeline_ceiling/state_machine` | 真實StateMachine、crossing workload、correctness | 只有crossing new order；以group取樣且計時含測試輔助工作 |
| `wal_write_ceiling --wal-sync=none` | direct WAL、batch append、replay驗證 | runner缺singleton；profile固定開啟；headline wall time含fixture建立 |
| `wal_write_ceiling --wal-sync=per_group` | append／sync／total percentile及WAL counter | batch matrix缺singleton；未依slow fsync case校準執行量 |
| `run_engine_writer_diagnostics.sh` | identity、affinity、preflight、observer、artifact | full scope要求perf/fio；affinity-only流程又排除component/direct WAL |

不需要移除既有workload，也不建立第二套StateMachine或WAL實作。必要工作是在既有benchmark中收斂量測語意，
並讓runner能只執行這三類component case。

## 5. 設計與模組邊界

```text
component-only campaign runner
  ├─ identity / binary / tool gate
  ├─ CPU-only preflight
  │    └─ StateMachine scenarios
  ├─ CPU + storage preflight / cooldown
  │    ├─ WAL append-return matrix
  │    └─ WAL durable fsync matrix
  ├─ external observers
  │    ├─ GNU time
  │    ├─ mpstat
  │    └─ iostat
  └─ run root outside repository
       ├─ raw stdout/stderr
       ├─ preflight/monitor/time artifacts
       ├─ normalized rows
       └─ summary inputs
```

Benchmark binary只負責執行component workload、量測component邊界與驗證correctness。環境判定、CPU affinity、
重複輪次、CV與artifact完整性由runner負責。兩者不得互相複製責任。

## 6. StateMachine 固定 command 測試

### 6.1 Scenario

只加入能覆蓋現有command type且維持有界state的四個scenario：

| Scenario | Measured循環 | 必要驗證 |
| --- | --- | --- |
| `new_crossing_pair` | resting sell後以buy成交 | 每兩command一筆trade；active orders回到baseline |
| `new_resting_cancel` | new resting order後cancel | order建立後終止；active orders回到baseline |
| `amend_quantity` | 對既有resting order在兩個合法quantity間切換 | order維持active；最終quantity符合預期 |
| `replace_order` | 對既有resting order在兩個不成交price間切換 | order維持active；price／priority結果符合預期 |

scenario setup與warmup不計入measured時間。每筆command仍由`StateMachine::apply()`逐筆處理；不存在
StateMachine業務batch。`--pipeline-active-orders`固定為同一個reported baseline；amend／replace scenario
需要的target order由scenario setup建立，不得讓measured loop持續增加active orders。

### 6.2 計時與輸出

正式case以單一outer timer包住固定command數的scenario loop，不在每筆command前後讀clock。每輪至少輸出：

- scenario、warmup commands、measured commands；
- elapsed milliseconds、commands/s、average ns/command；
- trades、events、active orders、active levels及last engine sequence；
- `completion_boundary=state_apply_return`；
- `latency_scope=run_average`；
- `correctness_verified=true`。

command建立與必要的result consumption若無法在不大量預建fixture的情況下完全移出loop，結果必須命名為
`state_machine_scenario_ceiling`，不得宣稱為純函式instruction ceiling，也不得人工扣除估算成本。

### 6.3 固定工作量

四個scenario使用相同measured command數；數量必須是所有scenario cycle長度的公倍數。先以短calibration選出
可讓最快scenario執行至少30秒且最慢scenario不超過case timeout的固定數量，寫入frozen plan後不得逐case
調整。

為保留既有CLI，component runner固定使用`--pipeline-batch-size=2`，並令
`iterations = measured_commands / 2`；這裡的2只用來換算既有counter，不是StateMachine batch。實作必須把
現有每group timer改成單一measured phase outer timer，不能因保留CLI而繼續輸出兩筆command一組的latency
percentile。

正式順序每輪正反交替，共五輪。各scenario以五輪commands/s median作結果，CV必須不超過5%。

## 7. WAL append-return 測試

### 7.1 Matrix與資料流

固定總command數，使用production batch API。正式append ceiling以fresh-WAL epoch切分，避免 measured
window跨越segment rotation：

```text
prepared command stream
  → partition by batch size
  → Wal::append_batch(span)
  → append return
  → next batch
  → final Wal::sync() outside measured boundary
  → reopen + replay verification
```

每個epoch沿用production `256 MiB` segment size，warmup後先sync；epoch command數依實際frame bytes、segment
header及安全餘裕計算，並驗證`measured_segment_rotations=0`。epoch結束後完成final sync、reopen/replay並
釋放WAL，再建立下一個fresh WAL；production segment size、rotation與durability順序不變。正式輸出名稱為
`wal_append_no_rotation`，completion boundary仍是`append_batch_return`。另執行b1 rotation-control與
rotation-trigger diagnostic各五輪，輸出old segment sync、new header write/sync、directory sync與rotation
latency；這些diagnostic rows不併入append ceiling。

rotation diagnostic只使用`--wal-rotation-diagnostic=control|trigger`：它限制為
`--workload=wal_write_ceiling`、`--wal-sync=none`、`--wal-phase-profile=on`、`--wal-group-size=1`及
`--iterations=1`，並禁止與no-rotation epoch同時使用。每個mode在warmup sync後，以實際frame byte delta做
setup-fill；setup-fill不計時、不profile、不sync，measured window只包含一個target group。control必須精確
0次rotation，trigger必須精確1次rotation，兩者都要final sync、reopen及完整replay驗證。輸出名稱固定為
`wal_rotation_diagnostic`，並明示`rotation_triggered`、`wal_byte_plan_verified`與sample count；單一target
group的p50/p99/p99.9可以相同，但不得當作分布。

必要batch matrix為：

```text
1, 256, 1024, 4096, 8192
```

`batch=1`就是production batch API的sequential singleton baseline，不另測非主要路徑的`Wal::append()`。
總command數必須是8192的倍數，所有case相同；warmup、segment header與final sync不計入measured時間。
先用短calibration選出能讓最快batch執行至少30秒、最慢batch不超過case timeout且符合disk budget的固定
command數，之後凍結於plan。

### 7.2 Ceiling與profile分離

- **ceiling rounds**：`sync=none`、`phase_profile=off`，五輪，作正式append-return結果。
- **profile rounds**：每個batch size至多一輪`phase_profile=on`，只提供prepare、plan/copy、write、publish等
  分段證據，不列入ceiling median。
- 選一個代表case執行profile off/on overhead check；throughput bias超過3%時，profile資料只能作定性參考。

`run_wal_groups()`必須將group service起點移到command fixture建立完成之後，使append latency及
append+sync group total不含fixture建立。整個process wall time仍保留，用來計算observer與CPU成本；正式component
ceiling則使用measured group service time，兩者不得混為同一欄位。

正式append結果至少輸出service commands/s、workload wall commands/s、MiB/s、average WAL bytes/command、
append group percentile、write call數、segment rotation數、service／wall elapsed及replay驗證，並標示
`latency_sample_stride`、sample count、measured process/I/O counters與Dirty/Writeback gauge。percentile只代表
固定stride sample，max與total由全量measured groups累積。欄位必須明示
`completion_boundary=append_batch_return`，不可把含fixture的workload wall rate命名為純append call ceiling。
正式WAL輸出必須同時提供且只提供一次`measured_rusage_valid`、`measured_io_valid`及
`measured_meminfo_valid`。Linux正式runner要求三者皆為true，且要求
`measured_user_seconds`、`measured_system_seconds`、context switches、`measured_syscw`、`measured_wchar`、
`measured_write_bytes`、`measured_cancelled_write_bytes`、Dirty/Writeback before/after及
`measured_wal_write_calls`為非負數字；`measured_wal_write_calls`必須等於`measured_syscw`，
`measured_wal_sync_calls`則必須等於planned sync groups。缺欄位、重複、`na`或counter不一致直接是
`invalid-run`，不得降級為unstable case。

`measured_wal_write_calls`是`/proc/self/io` measured window內的write-family syscall數；
`profiled_data_write_calls`則是WAL phase profile記錄的data-chunk write數，兩者來源與語意不同，
不得互換或用其中一者代替另一者。

no-rotation epoch另外以warmup實測的固定frame size建立byte plan：每個epoch都驗證
`ending_wal_bytes - starting_wal_bytes == planned_epoch_wal_bytes`、segment count/id不變且rotation為0，
最後再驗證所有epoch actual bytes等於planned bytes總和。輸出名稱為`wal_append_no_rotation`，並包含
`frame_bytes_per_command`、`planned_wal_bytes_delta`及`wal_byte_plan_verified=true`。

## 8. WAL durable fsync frontier

### 8.1 Matrix與完成邊界

沿用相同batch matrix：

```text
append_batch(batch_size)
  → Wal::sync()
  → group completed
```

每個case先短calibration，再將正式工作量凍結為：

- measured時間目標30至60秒；
- 至少1,000個sync samples；
- 不超過case timeout及預先計算的disk budget。

不同batch size可有不同command總數，因為本階段比較的是穩態rate與latency，不以相同command數換取singleton
case的過長執行時間。

### 8.2 必要metrics

- commands/s、commands/fsync、fsync/s；
- append p50／p99／p99.9／max；
- sync p50／p99／p99.9／max；
- group total p50／p99／p99.9／max；
- WAL bytes/s、bytes/command、data write calls及segment rotations；
- GNU time user/system seconds、CPU seconds/1M commands；
- iostat write bytes/s、write IOPS、await、aqu-sz與util。

CPU摘要統一計算：

```text
average_process_cpu_percent = (user_seconds + system_seconds) / wall_seconds * 100
cpu_seconds_per_million     = (user_seconds + system_seconds) / commands * 1,000,000
```

WAL可能啟用prepare helper，因此process CPU可超過100%；報告不得把它截斷成單core百分比。

正式frontier使用`phase_profile=off`。需要subphase時只跑獨立profile round，不得以profile-on throughput替代
ceiling。

## 9. Environment與執行控制

### 9.1 共通條件

- ReleaseBenchmark build及相關CTest先通過；
- run root位於repository外、與目標WAL相同filesystem；
- benchmark CPUs與observer CPUs不重疊；
- 每個正式case使用新的空data directory；
- binary SHA-256、HEAD、index、worktree與untracked identity在campaign前後一致；
- runner不得修改sysctl、governor、IRQ、cgroup、filesystem或Git staging。

### 9.2 StateMachine preflight

StateMachine case只以CPU idle作停止條件：

- benchmark CPU平均idle的最小值至少90%；
- iowait p95不超過5%；
- 無殘留benchmark或observer process。

disk數據仍可記錄為環境證據，但不因單獨disk queue spike拒絕CPU-only case。

### 9.3 WAL preflight與cooldown

每個WAL正式case前固定cooldown 60秒，再執行一次30秒preflight：

- benchmark CPU平均idle的最小值至少90%；
- iowait p95不超過5%、絕對max不超過10%；
- 目標device util平均不超過5%；
- aqu-sz p95不超過0.25、max不超過0.50；
- 無殘留benchmark、mpstat、iostat、fio或perf process。

`/proc/meminfo`的Dirty／Writeback只在cooldown前後記錄為佐證，不另設未經驗證的絕對門檻；正式idle gate以
上述30秒iostat條件判定。

preflight只執行一次；失敗時分類為`preflight-busy`並停止整次campaign，不得反覆抽樣直到偶然通過。

## 10. Runner與介面修改

### 10.1 既有binary

沿用`order_books_benchmark`及既有workload名稱：

- `engine_pipeline_ceiling --pipeline-stage=state_machine`；
- `wal_write_ceiling --wal-sync=none|per_group`。

StateMachine只新增benchmark CLI選項：

```text
--pipeline-command-scenario=
  new_crossing_pair|new_resting_cancel|amend_quantity|replace_order
```

不新增public library API。既有`--iterations`／`--pipeline-batch-size`可在內部換算固定command數，但輸出必須
明確列出最終`measured_commands`，runner以該counter驗證frozen plan。

### 10.2 既有runner

在`run_engine_writer_diagnostics.sh`增加互斥scope：

```text
--scope=full       # 保留既有行為
--scope=component  # 本文件定義的StateMachine/WAL campaign
```

component scope不得要求`perf`權限或`fio-bs`，也不得執行Engine frontier、tail、Publisher、Completion、
writer profile、perf record或fio。這比新增第二套runner更能避免identity、preflight及artifact邏輯分裂。

成功完成replay與artifact驗證、且runner的schema、counter、position與duration gates均通過的case，才可刪除
runner自己建立的該case WAL data directory，只保留stdout、counter、monitor、time與normalized artifacts；
binary exit 0但語意驗證失敗的case仍須保留data供診斷。清理必須使用已解析且位於本次run root下的精確case
路徑，不接受glob、空變數或repository內路徑。

## 11. 錯誤處理與結果分類

| 分類 | 條件 | 結果用途 |
| --- | --- | --- |
| `valid-component-ceiling` | correctness、identity、duration、CV、artifact全部通過 | 可報component ceiling |
| `preflight-busy` | CPU或WAL storage idle gate不通過 | 不產生正式結果 |
| `observer-biased` | observer/profile bias超過3% | 不使用受污染數據 |
| `collection-complete-results-partial` | 單一或多個case CV超過5%，但其他matrix已完整收集 | 保留rejected raw rows，不宣稱ceiling或Pareto |
| `environment-unstable` | 舊版流程以環境名義停止或環境counter明顯漂移 | 依具體證據分類，不把workload rotation I/O泛稱外部繁忙 |
| `invalid-run` | counter、replay、scenario final state、artifact或identity錯誤 | 修正後以新run root重跑 |

任一command失敗、unexpected result、sequence不連續、WAL replay不一致或sync失敗都應立即停止該campaign；
不得忽略失敗command後繼續計算RPS。

## 12. 測試策略

### 12.1 C++測試

- 四個StateMachine scenario的command count、result、event／trade及final-state correctness；
- scenario command數不可整除cycle時回報明確CLI／setup錯誤；
- WAL batch=1與既有batch的counter、sync samples、bytes及replay correctness；
- `sync=none`沒有measured sync samples；`per_group`的sync samples等於measured groups；
- profile off/on輸出schema及必要counter一致。

### 12.2 Runner contract測試

- component scope不要求perf、fio或fio block size；
- scope互斥且不執行scope外phase；
- CPU-only與WAL preflight使用各自門檻；
- frozen command plan、正反輪序、CV、bias及terminal classification正確；
- busy preflight、benchmark failure、artifact缺失與identity改變均安全停止；
- 測試模式只使用小工作量及fake observer，不更改host policy或Git index。

### 12.3 實際執行順序

實作階段只完成程式、測試與可編譯驗證；經review後才執行正式campaign。正式順序固定為：

1. build／CTest及identity；
2. initial environment preflight；
3. StateMachine calibration、frozen plan、四scenario各五輪；
4. WAL append calibration、五batch sizes各五輪及必要profile rounds；
5. WAL fsync calibration、五batch sizes各五輪及必要profile rounds；
6. identity after、artifact validation與報告。

## 13. 報告格式與接受標準

結果另寫入：

```text
docs/state-machine-wal-component-ceiling-benchmark-report.md
```

報告至少包含：

- binary／repository identity、host、filesystem、CPU affinity與evidence限制；
- initial及per-case preflight摘要；
- StateMachine各scenario五輪、median、CV、平均ns/command及CPU seconds/M；
- WAL append各batch五輪、median、CV、MiB/s、write calls與rotation；
- WAL fsync各batch五輪、commands/fsync、fsync/s、latency tail與iostat；
- profile off/on bias；
- append no-rotation與rotation attribution分開的結果，以及每個case的accepted／rejected status；
- measured process CPU／I/O counters與host/device time series分開的證據；
- 哪一個batch是append ceiling、哪一個batch是durable throughput／latency Pareto候選；
- 明確聲明component結果不能替代full Engine sustainable ceiling。

接受標準：

- 所有formal case measured duration至少30秒；
- 每個正式結果五輪CV不超過5%；
- observer／profile bias不超過3%；
- 所有counter、final state及WAL replay正確；
- StateMachine固定command數一致；
- append固定command數一致；
- fsync每case至少1,000個sync samples；
- campaign前後identity一致且staging未被改變。

## 14. 關鍵取捨

1. **固定command數而非StateMachine batch**：符合逐command業務語意並降低計時干擾；代價是只報平均
   command latency，不製造無法支持的percentile。
2. **append固定總commands、fsync按時間校準**：append比較維持相同工作量；durable測試避免singleton case
   不合理地長時間執行。
3. **phase off作ceiling、phase on作歸因**：保留分段證據，但不讓clock sampling污染正式數字。
4. **沿用既有binary與runner**：避免再建立一套診斷框架；只增加一個bounded scope與一個scenario選項。
5. **不要求perf／fio**：本次可回答component rate、latency與CPU／I/O usage；無call graph與storage control時
   不宣稱函式級或硬體級根因。

## 15. 已知限制與後續方向

- StateMachine scenario loop仍可能包含少量command fixture與result consumption成本；報告必須以scenario
  ceiling命名，不能人工扣除。
- append-return受page cache容量、dirty throttling與filesystem影響，不是DRAM bandwidth benchmark。
- direct WAL不包含Engine admission、queue、StateMachine、metrics、Publisher或Completion成本。
- fsync結果只代表本次host與filesystem；不能直接外推到其他storage或production SLA。
- 若component ceiling顯著高於full Engine，再依證據決定是否分析admission、metrics或async workers；本次不預先
  實作這些擴充。
- 若未來取得perf權限或專用host，可在新campaign加入call graph與same-path fio，但不得與本次artifact混用。

## 16. 設計與實作一致性檢查表

實作者完成後必須逐項確認：

- 沒有修改production matching、WAL durability或public API語意；
- 沒有新增benchmark executable或重複StateMachine／WAL實作；
- StateMachine只有四個必要scenario，沒有額外workload matrix；
- append與fsync數據分表且completion boundary明確；
- profile-on數據沒有進入ceiling median；
- component scope沒有啟動Engine／Publisher／Completion worker；
- CPU-only與WAL preflight沒有誤用同一組storage gate；
- 報告沒有把平均latency寫成percentile，也沒有把component ceiling寫成full Engine ceiling。

符合此檢查表，即代表設計與後續實作對需求的認知一致，且沒有超出本次必要範圍。
