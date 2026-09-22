# StateMachine／WAL Component Ceiling 壓測必要修正

## 1. 結論

`docs/state-machine-wal-component-ceiling-benchmark-report.md` 的 StateMachine 資料可作為穩定的
partial observation；WAL append `batch=1` 則不能作為有效 ceiling，但失敗原因不是 WAL correctness。

目前 `sync=none` 的 measured window 寫入約 3.78 GB，跨越 14 次 256 MiB segment rotation。
`Wal::append_batch()` 在 rotation 時會同步 dirty 舊 segment，建立新 segment時也會同步新檔案與
directory。因此 `append_batch_return` 同時包含一般 append、background writeback／dirty throttling與
rotation durability。單次最大 latency達10至16秒，與一般append的p99約5 microseconds不是同一種成本。

下一輪不能直接原樣重跑。必須先完成本文的必要修正，將下列三種量測分開：

1. 不跨segment的append-return ceiling；
2. segment rotation的成本與tail歸因；
3. `append_batch() + Wal::sync()`的durable frontier。

修正只涉及benchmark、component runner、測試與文件；不得改變production WAL的segment size、rotation、
fsync、WAL format或recovery semantics。

## 2. 已確認的問題與證據

### 2.1 `sync=none`不等於沒有fsync

現行`Wal::append_prepared_unlocked()`在segment空間不足時會：

```text
dirty active segment
  -> sync_active_unlocked()
  -> create_segment()
       -> write segment header
       -> fsync(new segment)
       -> fsync(WAL directory)
  -> write current records
  -> append_batch() returns
```

所以現有`completion_boundary=append_batch_return`語意本身沒有造假，但它不是「無durability I/O的
append ceiling」。`sync_samples=0`只能表示benchmark沒有在每個group後額外呼叫`Wal::sync()`。

### 2.2 本輪是workload自己產生I/O壓力

五輪case開始前的CPU與storage preflight都通過，但正式workload每輪寫入約3.78 GB並rotation 14次。
最慢的round 4同時具有：

- 最低service throughput：221,545 commands/s；
- 最低whole-process CPU：70.86%；
- 最高write await p95：499.12 ms；
- 最高average device util：29.684%；
- 單次append max：15.382秒。

因此`environment-unstable`只適合作為campaign validity結果，不適合描述根因。現有證據支持
`rotation/writeback suspected`，但因ceiling round沒有rotation phase timing，尚不能報精確占比。

### 2.3 單一CV gate過早截斷證據收集

runner在`batch=1`五輪CV為10.102%後立即停止，導致`batch=256/1024/4096/8192`、durable matrix與
profile rounds全部沒有執行。這使一個case的tail變異阻止其他獨立問題的診斷，也無法判斷batching是否
降低固定成本。

### 2.4 現行resource欄位不是measured-phase資源

GNU `time`與外部`iostat`涵蓋setup、warmup、measured append、final sync、reopen、完整replay與清理。
因此約10 GiB max RSS、whole-process CPU與iostat不能直接歸屬於append measured phase。報告已正確標成
whole-process資料，但下一輪若要判斷CPU／I/O瓶頸，仍需要measured-phase counters。

## 3. 必要修正一：拆分量測語意

### 3.1 Append-return ceiling

正式append ceiling必須保證每個measured epoch不發生rotation：

```text
create fresh WAL using production segment size
  -> warmup
  -> sync warmup outside measured boundary
  -> capture starting position/counters
  -> append fixed commands without crossing segment boundary
  -> capture ending counters
  -> end measured boundary
  -> final sync and replay outside measured boundary
```

不能藉由放大production segment size來避開rotation，因為那會改變被測設定。應在benchmark內把同一正式
round切成數個fresh-WAL epochs；每個epoch的command數由實際frame bytes與segment剩餘空間計算，並保留
至少一個最大frame的安全餘裕。各batch size使用相同總command數與相同epoch規則。

每個epoch必須驗證：

- measured rotations等於0；
- measured commands與WAL byte delta符合計畫；
- final sync成功；
- reopen/replay內容與sequence正確。

正式輸出名稱改為`wal_append_no_rotation`，completion boundary仍為
`append_batch_return`。文件必須明示它包含serialization、CRC、index bookkeeping、`write()`與可能的
page-cache writeback，但不包含segment rotation和顯式group fsync。

### 3.2 Rotation attribution case

新增diagnostic case，不列入append ceiling median：

1. 使用production 256 MiB segment；
2. warmup後先sync，讓起始狀態可判定；
3. 寫到距segment尾端不足一個planned group；
4. profile下一個必定觸發rotation的`append_batch()`；
5. 另取相同batch size、不觸發rotation的control append；
6. 各執行五輪，輸出median、p99／max與所有raw rows。

必要欄位：

- `rotation_triggered=true|false`；
- append total、prepare、plan/copy、rotation、write與publish latency；
- rotation內的old-segment sync、new-header sync與directory sync latency；
- rotation前後segment id、offset、Dirty／Writeback；
- correctness與replay結果。

為得到三種sync的明確歸因，可只在既有benchmark diagnostic/profile路徑細分rotation timing；production
`append_batch()`與durability順序不得改動。這些clock reads只出現在diagnostic case，不得放入正式ceiling
round。

### 3.3 Durable frontier

既有durable matrix保留：

```text
append_batch(batch_size)
  -> Wal::sync()
  -> group completed
```

batch仍為`1, 256, 1024, 4096, 8192`。正式結果報commands/s、commands/fsync、fsync/s，以及append、
sync、group-total的p50／p99／p99.9／max。rotation若在durable case發生，需另外計數，但不從production
路徑排除；因每個group已sync，這才是production durability語意下的rotation成本。

## 4. 必要修正二：避免量測資料結構放大記憶體壓力

現行`batch=1`會建立約31M個`append_ns`與31M個`total_ns`樣本；`sync=none`時兩者語意幾乎相同。
另外，單一WAL保留全部cached records，完整replay也會建立大型vector。

必要調整：

1. `sync=none`只保留append latency distribution，不建立重複的total sample vector；
2. latency distribution改用有界histogram或有界、固定規則的deterministic sampling，並永遠另外保存精確max；
3. 若使用sampling，輸出sampling stride、sample count與coverage，不能把sample percentile描述成全量精確值；
4. 以第3.1節的fresh-WAL epochs限制單一WAL cache與單次replay的record數；
5. 每個epoch結束後才釋放WAL與replay資料，下一epoch不得與上一epoch同時保留完整records。

不應為benchmark需求改掉production WAL的in-memory index。目標是避免benchmark自己額外保留數千萬筆重複
latency samples，並以epoch限制validation的峰值工作集。

## 5. 必要修正三：取得measured-phase資源證據

在measured append或durable loop的前後，於benchmark process內擷取低成本累積counter：

- process user/system CPU time；
- voluntary／involuntary context switches；
- Linux `/proc/self/io`的`syscw`、`wchar`、`write_bytes`與`cancelled_write_bytes`；
- `/proc/meminfo`的Dirty與Writeback；
- WAL bytes、write calls、rotations與sync calls。

輸出必須使用`measured_*`前綴，與GNU time、mpstat、iostat的`whole_process_*`／`host_window_*`分開。
外部iostat仍保留，因process counters不能取代device await、queue與util；報告歸因時需同時呈現：

```text
measured process counters
  + host/device time series
  + WAL rotation/sync counters
```

不新增常駐production metrics，也不要求perf或fio才能完成component matrix。perf只在CPU仍為候選根因時做
後續profile；fio只在需要比較裝置能力上限時執行，不能阻止本次component collection。

## 6. 必要修正四：CV只判定case，不截斷matrix

runner需將每個case的collection與acceptance分開：

```text
run five rounds
  -> verify counters/replay/artifacts
  -> calculate CV
  -> record accepted or rejected
  -> continue next batch
```

規則如下：

- preflight失敗、binary failure、counter/replay failure或artifact缺失仍立即安全停止；
- 單一case `CV > 5%`時標記該case為`rejected-unstable`，但繼續其他batch與diagnostic cases；
- 不得刪除慢輪、補跑替換某一輪或混合不同run root；
- matrix完成後，只有所有必要case都accepted才能輸出`valid-component-ceiling`；
- 有case被拒絕但collection完整時輸出`collection-complete-results-partial`；
- 報告可以呈現rejected raw data，但不得用它選production Pareto或宣稱正式ceiling。

這項修正不放寬5%門檻，只避免一個case阻止其他必要證據被收集。

## 7. 必要修正五：校正報告與判定文字

更新設計、操作文件與報告模板，使結果固定分成：

1. StateMachine scenario ceiling；
2. no-rotation append-return ceiling；
3. rotation attribution；
4. durable fsync frontier；
5. whole-process與measured-phase資源證據；
6. case validity及campaign collection status。

現有報告的`environment-unstable`保留作歷史結果，但根因文字應補充：

> Case在執行前preflight通過；CV失敗與workload內14次segment rotation及其durability I/O高度相關。
> 現有資料尚未量得rotation subphase精確占比，因此不得宣稱唯一根因。

不得再使用下列模糊敘述：

- 把`sync=none`稱為純記憶體append；
- 把whole-process RSS／CPU稱為append hot-path資源；
- 把preflight通過後的workload自生I/O壓力統稱為外部環境繁忙；
- 因append b1 rejected而宣稱其他batch或fsync case也不合格。

## 8. 檔案層級修改

### `benchmarks/order_book_benchmark.cpp`

- 將WAL append正式round切成不跨segment的fresh-WAL epochs；
- 增加rotation diagnostic輸出與三種rotation sync subphase；
- 移除`sync=none`重複的total latency samples，改為有界latency資料結構；
- 增加measured-phase process／I/O counters；
- 保留final sync與replay在measured boundary之外。

### `benchmarks/run_engine_writer_diagnostics.sh`

- 產生並凍結epoch plan；
- 在append matrix後執行rotation attribution，再執行durable matrix；
- 將CV failure改為per-case rejection，完整收集matrix後才決定campaign結果；
- 驗證每個no-rotation epoch的rotation counter必須為0；
- 彙整case status、measured resources與rotation raw rows。

### `benchmarks/test_engine_writer_diagnostics_runner.sh`

- 覆蓋CV rejected但後續matrix仍執行；
- 覆蓋preflight／correctness／artifact failure仍立即停止；
- 覆蓋no-rotation counter、epoch aggregation與partial-result分類；
- 驗證不得以補跑取代慢輪。

### `benchmarks/CMakeLists.txt`與相關CTest

- 增加no-rotation epoch、rotation diagnostic、bounded sampling與measured counter contract測試；
- 保留現有append、fsync、profile與replay correctness測試。

### 文件

- 更新`state-machine-wal-component-ceiling-design-review.md`的量測邊界與validity分類；
- 更新`state-machine-wal-component-ceiling-benchmark-procedure.md`的執行及報告格式；
- 在既有benchmark report補上第7節的精確根因文字，不改寫或隱藏原始失敗結果。

## 9. 明確不做

以下均非本輪必要修正：

- 改變production WAL segment size或format；
- 移除rotation時的fsync或directory sync；
- 引入`O_DIRECT`、io_uring、mmap或另一套WAL；
- 修改Engine、Publisher或Completion worker；
- 調整CPU governor、IRQ、sysctl、cgroup或filesystem mount options；
- 為了讓CV通過而提高門檻、刪除慢輪或只選較快round；
- 在完成正確量測前進行WAL production optimization。

## 10. 完成條件與執行順序

實作完成後依序驗證：

1. build與全部相關CTest通過；
2. runner dry-run證明scope、epoch plan、case order與failure semantics；
3. 小型integration run證明no-rotation counter為0、rotation diagnostic恰好觸發預期rotation；
4. 新run root執行完整正式campaign；
5. matrix所有case都有五輪raw data、資源證據、correctness與case status；
6. 只有accepted case可產生ceiling或Pareto結論。

舊run root只保留作失敗證據，不與新campaign合併。完成以上量測後，再依證據決定是否需要修改production
WAL；目前沒有足夠依據直接修改production path。
