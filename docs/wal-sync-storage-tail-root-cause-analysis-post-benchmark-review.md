# WAL sync／storage tail 根因分析：壓測後設計 review 與必要修正

## 1. Review 範圍與結論

本文件 review：

- `docs/wal-sync-storage-tail-root-cause-analysis-design.md`；
- `docs/wal-sync-storage-tail-root-cause-analysis-benchmark-procedure.md`；
- 目前對應的 benchmark telemetry 實作與測試；
- `/home/neojhou/wal-sync-tail-analysis-dFGqFZVA` 的實際量測產物。

原設計的架構邊界正確，且沒有超出需求：telemetry 只存在 benchmark target，production WAL、Engine
public API、durability boundary、group policy、prepare default、Publisher 與 Completion 都沒有改動。量測也已
證明慢輪的直接瓶頸是 WAL sync wait：兩個 W=2 慢輪新增的 sync time分別可解釋約 94.65% 與
98.92% 的額外 elapsed time。

但需求尚未完整達成，原因不是缺少 WAL 優化，而是目前證據仍無法回答 sync tail 的下一層根因：

1. W=2 telemetry off/on median差異約40.25%，超過5% calibration gate；正式十輪矩陣依法停止。
2. 現有off/on比較同時改變raw metric collector與10 ms state sampler，無法判斷偏差來自哪一部分，
   也無法排除只是間歇性storage tail剛好集中在telemetry-on輪次。
3. 多數輪次沒有drain state row，summary輸出`drain_*_last=na`，無法可靠判斷publisher backlog在
   `Engine::stop()`期間是否收斂。
4. 唯一有`iostat`／process I/O的observed輪沒有重現慢tail，因此不能將application sync tail歸因給
   device、filesystem或syscall。

因此只需要三組修正：拆分telemetry成本、補齊deterministic drain boundary snapshot，以及定義
calibration失敗後的固定診斷與報告流程。除此之外不應修改production或開始效能優化。

## 2. 需求理解與不變條件

### 2.1 本次修正必須回答

1. `wal_sync_latency_us`／`wal_group_commands` collector本身是否造成可量測的throughput regression？
2. 10 ms `Engine::metrics()` sampler是否造成額外contention？
3. W=2慢輪若再次發生，是否同時出現device await／queue／utilization或`fsync` syscall tail？
4. measured輸入停止後，publisher lag在drain開始與`Engine::stop()`完成時各是多少？

### 2.2 保持不變

- 單一instrument、單一shard、public Engine durable path。
- 每筆command仍在WAL append及當次group `fsync`成功後完成durable callback。
- `fsync`、group size／delay、WAL record、replay、RPO=0與completion語意不變。
- W=1仍是production default；不因本輪診斷自動啟用W=2。
- telemetry預設關閉；未指定telemetry option時行為與輸出不變。
- 所有新增控制只屬於benchmark CLI，不加入`RuntimeConfig`或public header。
- 不以重跑到結果好看為止；所有預先排定輪次與outlier都保留。

## 3. 必要修正一：拆分 collector 與 state sampler 成本

### 3.1 問題

目前只有兩種狀態：

```text
telemetry off = NullMetricsSink，沒有state sampler
telemetry on  = raw sync/group collector + 10 ms state sampler
```

W2 calibration失敗時，這個二分法無法辨識是writer callback的clock／mutex／record成本、sampler的
`Engine::metrics()`成本，或間歇性storage tail。直接把10 ms改成另一個數字只是在調參，不能證明根因。

### 3.2 最小介面

新增一個benchmark-only option：

```text
--engine-tail-state-sampling=on|off
```

規則：

- 預設為`on`，維持既有telemetry完整行為。
- 只有指定`--engine-tail-telemetry-output`且workload為
  `engine_durable_single_instrument`時才允許使用。
- `off`只停用periodic state sampler；raw `wal_sync_latency_us`與`wal_group_commands` collector、
  measured phase、CSV輸出及aggregate validation仍保持啟用。
- summary新增`tail_state_sampling=on|off`；CSV維持既有固定header，不加入metadata row，artifact透過同一
  summary的`tail_telemetry_file`配對識別，避免破壞離線schema validator。
- `off`是calibration／diagnostic模式，不可用於正式storage correlation矩陣。
- 不新增任意sampling interval knob；在確認sampler確實是偏差來源前，不預先選擇10、25或50 ms。

這會形成三個可比較模式：

| 模式 | Raw collector | State sampler | 用途 |
| --- | --- | --- | --- |
| telemetry off | 否 | 否 | 原始baseline |
| collector-only | 是 | 否 | 隔離writer metric callback成本 |
| full telemetry | 是 | 是，10 ms | 隔離periodic `Engine::metrics()`增量成本 |

### 3.3 元件責任與資料語意

- `BenchmarkOptions`只解析與驗證CLI，不把設定傳入production config。
- `EngineTailTelemetry`仍負責raw collector與CSV；只有`start_sampler()`是否呼叫由benchmark controller決定。
- collector-only CSV必須包含`sync`與`group_commands` rows；沒有`state` row是該模式的合法結果。
- full telemetry仍要求至少一個measured state row及第4節定義的drain boundary rows。
- `telemetry_dropped_samples`、sync/group count、command sum與CSV/summary一致性規則不變。

### 3.4 測試

- CLI接受`on`／`off`，拒絕未知值、缺少output path及錯誤workload。
- collector-only smoke驗證sync/group rows與aggregate正確，且沒有state row。
- full smoke維持既有schema檢查；unit test固定state sampling仍為10 ms設計語意。
- telemetry完全關閉時summary與既有行為不變。

預估必要修改：production code 0行；benchmark與測試約45--75行。

## 4. 必要修正二：保證 drain 起訖快照存在

### 4.1 問題

`begin_drain()`到`Engine::stop()`完成可能短於10 ms。若sampler沒有剛好醒來，CSV不會有drain row，
目前多數實際輪次因而輸出`drain_publisher_lag_*_last=na`。這與「判斷停止輸入後backlog是否收斂」的
需求不一致。

### 4.2 最小修法

不提高sampling頻率，也不新增production metric。重用controller已取得的snapshot並增加兩個明確邊界：

```text
measured durable completion
  -> begin_drain()
  -> Engine::metrics(shard)              # drain-start
  -> telemetry.record_drain_snapshot(..., drain_start)
  -> Engine::stop()                       # periodic sampler仍可在此期間取樣
  -> telemetry.stop_sampler()
  -> Engine::metrics(shard)              # drain-end，Engine物件尚未reset
  -> telemetry.record_drain_snapshot(..., drain_end)
  -> telemetry.stop_collection()
  -> write artifact
```

`record_drain_snapshot()`只放在`benchmarks/engine_tail_telemetry.*`，接收既有`MetricsSnapshot`與
`drain_start`／`drain_end` boundary標記；它不得反向呼叫Engine。boundary標記只存在collector的內部
record，不改CSV schema。drain-start可重用目前已存在的`final_metrics`呼叫，因此不增加measured hot
path成本。drain-end是在`Engine::stop()`完成後取得，不納入throughput。

若stop後metrics不可取得，該輪回報telemetry validation failure並保留artifact，不以`na`假裝完成。
early-return仍維持先停止sampler、再停止Engine的安全順序；上述流程只適用正常成功路徑。

### 4.3 Summary 與驗證

為避免只有`last`而無法判斷變化，summary新增：

```text
drain_state_sample_count=<n>
drain_publisher_lag_events_first=<n>
drain_publisher_lag_bytes_first=<n>
drain_publisher_lag_age_ns_first=<n>
drain_publisher_lag_events_last=<n>
drain_publisher_lag_bytes_last=<n>
drain_publisher_lag_age_ns_last=<n>
```

full telemetry成功輪必須至少有drain-start與drain-end兩筆state row，且`last`來自排序後最後一筆
drain row。collector-only模式不要求state或drain aggregate。

### 4.4 測試

- unit test驗證無periodic tick時仍能寫出兩筆drain boundary rows。
- integration smoke驗證full telemetry的drain first／last不是`na`且與CSV一致。
- stop後snapshot失敗時benchmark非零結束，且不掩蓋既有Engine stop error。
- 不對lag一定下降作functional assertion；Publisher是否追上是量測結果，不是測試固定條件。

預估必要修改：benchmark helper/controller與測試約45--70行。

## 5. 必要修正三：校準失敗後的固定診斷與報告流程

### 5.1 先交付本次 blocked report

依既有操作規格建立：

```text
docs/wal-sync-storage-tail-root-cause-analysis-benchmark-report.md
```

報告必須明確記錄：

- ReleaseBenchmark 115/115、Debug 90/90、ASan/UBSan 90/90通過；
- W1 bias約1.64%通過；W2 bias約40.25%失敗；
- 正式矩陣為0/10，原因是依gate停止，不是漏跑；
- W2慢輪額外elapsed約94.65%／98.92%可由新增sync wait解釋；
- device/storage、filesystem/syscall及parallel-prepare interaction仍為`inconclusive`；
- observed正常輪只證明當輪沒有bandwidth saturation，不能反證慢輪沒有storage tail；
- raw artifact位置與未執行strace的限制。

不得填造正式aggregate、把calibration當正式五輪，或宣稱telemetry本身造成40.25%退化。

### 5.2 Component calibration

實作第3節後，先只對W=2執行三模式診斷。每種模式三輪、每輪measured至少60秒；使用同一binary、
parameters、CPU affinity與filesystem，順序預先固定並平衡位置：

```text
r1: off -> collector-only -> full
r2: full -> off -> collector-only
r3: collector-only -> full -> off
```

三種模式都使用相同的低頻外部`iostat -xz 1`、process I/O與`/usr/bin/time -v`，因此外部監測成本不會
只落在某一模式。所有輪次都保留；不得因發生tail而重跑或排除。

計算：

```text
collector bias = |median(collector-only RPS) - median(off RPS)| / median(off RPS)
sampler increment = |median(full RPS) - median(collector-only RPS)| /
                    median(collector-only RPS)
full bias = |median(full RPS) - median(off RPS)| / median(off RPS)
```

三者都必須<=5%，full telemetry才可進入正式矩陣。若任一失敗，先依失敗component另案處理；不得直接
降低durability、提高group delay或改prepare lanes來讓數字通過。

60秒是針對本次20秒輪次被少數13--16秒tail主導所做的必要調整；它降低單次間歇tail對median的支配，
但不會刪除tail evidence。

### 5.3 Root-cause evidence

若component calibration通過，重新執行原W1/W2 full-telemetry calibration及正式五輪矩陣。正式輪都必須
啟用外部監測並包含deterministic drain boundary。若慢輪重現：

- application層以measured sync total與tail count解釋elapsed差異；
- device層只以同輪`iostat`／process I/O作粗粒度correlation；
- syscall層以獨立`strace`診斷輪確認慢呼叫是否為`fsync`／write；
- `strace`絕對RPS與latency不得納入正式aggregate。

若固定矩陣沒有重現慢輪，結論只能是「本次環境未重現」；不得把先前tail刪除，也不得宣稱storage問題
已消失。

預估必要修改：操作文件與報告約80--130行，不涉及production code。

## 6. 錯誤處理與失敗語意

- CLI組合錯誤回傳2並輸出穩定error code。
- collector-only不得因state row為0而失敗；full telemetry缺少measured或drain boundary row必須失敗。
- 任一metrics snapshot、CSV write、aggregate、drop或sampler錯誤都使該輪無效，但不得影響WAL durability。
- Engine/WAL原始錯誤優先；telemetry錯誤只能追加，不可覆蓋原錯誤。
- observed工具缺失時device/storage判定為`incomplete`，不能默默退化成無監測正式輪。

## 7. 必要性與範圍檢查

| 項目 | 是否必要 | 理由 |
| --- | --- | --- |
| collector/sampler拆分 | 是 | 現有W2 gate失敗且無法判定量測偏差來源 |
| deterministic drain boundaries | 是 | 實際輸出`na`，無法回答既定backlog需求 |
| 固定component calibration | 是 | 防止重跑挑結果，並取得慢輪同期外部證據 |
| blocked benchmark report | 是 | 原設計明定交付物，目前尚未建立 |
| 修改`fsync`或group policy | 否 | 尚未證明下一層根因，會破壞baseline |
| 改WAL format／durability | 否 | 與診斷無關且超出需求 |
| 調整W=2 production default | 否 | 正式矩陣尚未完成 |
| 優化Publisher／Completion | 否 | backlog存在但未證明造成sync tail |
| 新增production tracing/exporter | 否 | benchmark-local evidence已足夠 |
| 引入第三方benchmark/telemetry library | 否 | 增加依賴但不改善歸因能力 |

必要程式與測試修改合計約90--145行；文件與報告約80--130行。若程式修改明顯超過約170行，應先檢查
是否誤納sampling framework、production config、storage abstraction或效能優化。

## 8. 設計與實作最終一致性清單

- [ ] telemetry off仍完全不建立collector或sampler。
- [ ] collector-only只收sync/group raw samples，不啟動state sampler。
- [ ] full telemetry維持10 ms periodic state sampling。
- [ ] summary與artifact明確記錄state sampling模式。
- [ ] full成功輪必定包含drain-start與drain-end snapshot，不再以`na`表示正常快速stop。
- [ ] measured throughput不包含drain、stop、CSV或replay時間。
- [ ] 三模式診斷使用同一binary、參數、CPU與filesystem，且順序在執行前固定。
- [ ] calibration未通過時不執行正式矩陣、不產生production結論。
- [ ] 慢輪與outlier全部保留，沒有重跑取代。
- [ ] device／filesystem／syscall結論只在對應外部證據存在時成立。
- [ ] 沒有修改production WAL、Engine API、RuntimeConfig、Publisher或Completion。
- [ ] 沒有執行任何會改變staging的操作。

完成上述三組修正後，設計、benchmark行為、artifact語意、停止條件與報告結論才會一致。下一個實作階段
應只完成本文件定義的量測修正；在新的證據成立前，不應開始WAL或storage效能優化。
