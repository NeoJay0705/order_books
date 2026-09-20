# WAL append publish bookkeeping 最小化：post-benchmark 設計 review

## 1. Review 範圍與結論

本文件 review 下列內容的一致性：

- `docs/wal-append-publish-bookkeeping-optimization-design.md`；
- `docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md`；
- `docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md`；
- commit `ea72a31` 中 `src/persistence/wal.cpp` 與
  `tests/integration/persistence_test.cpp` 的 production/test 修改。

結論分成兩部分：

1. **功能設計與實作相符。** Production 修改確實只把 byte counters 與 terminal
   `WalPosition` 從 per-record 更新提升為 per-chunk 更新；record cache 仍逐筆、依序 publish，
   segment rotation、write、sync、durability 與 public API 均未改變。現有測試也覆蓋最後位置、
   segment offset、WAL size、rotation durability boundary 與 replay。這部分不需要再擴大程式碼。
2. **效能驗收狀態與 repository 狀態不一致。** 報告寫明 `retain candidate: no`，且既定
   tail/resource guardrail 已失敗，但 candidate 已存在於目前 HEAD。依原先預先定義的規則，不能把它
   描述為已通過或已保留的 production optimization。

因此，目前必要工作不是開始下一個 StateMachine optimization，也不是擴充 WAL patch，而是先修正
驗收契約與報告中的矛盾，並讓 production source 回到一個有明確狀態的版本。

## 2. 需求理解與合理假設

### 2.1 必須維持的需求

- 不改變 WAL bytes、record ordering、segment boundary、CRC、EngineSeq、durability 與 replay。
- 不改變 group size、group delay、fsync policy、prepare worker、Publisher 或 Completion。
- 效能候選只有在原設計第 9.3 節全部 gate 通過時才可標記為 `pass / retain`。
- 方向性 workload 不得反過來成為主要 acceptance gate。
- 已取得的慢輪、storage tail 與 context-switch 結果不得因不利於 candidate 而刪除。
- 在本 review 階段不修改 production code、不重跑 benchmark，也不改變 Git staging。

### 2.2 合理假設

- benchmark report 與其外部 raw artifacts 是本輪量測的固定歷史紀錄，不回寫原始數據。
- 原設計先於正式量測完成，其 acceptance gate 是權威規則；不得在看到結果後放寬 gate。
- g8192 writer 在原設計中明確只是 phase 方向性資料；g8192 direct WAL 則仍是 regression guardrail。
- `powersave` governor 與 storage variability 可能放大變異，但它們已如實存在於本輪正式結果，不能讓
  一個已完成且其他 validity 條件成立的失敗 gate 自動失效。

## 3. 已確認符合設計的部分

### 3.1 架構與模組邊界

外部資料流沒有改變：

```text
prepare -> plan/copy -> write chunk -> publish cached records
        -> publish chunk terminal position -> fsync -> StateMachine apply
```

`Wal::append_prepared_unlocked()` 仍是唯一被修改的 production 邊界。實作沒有新增 thread、lock、
allocation、configuration、public API 或 metrics schema，也沒有把所有 chunks 延後到 batch 結束才一次
publish。

### 3.2 核心狀態與資料流

每個成功寫入的 chunk 仍依序執行：

1. 將該 chunk 的 `CachedRecord` 逐筆移入 `records_`；
2. 以 `chunk.bytes.size()` 一次增加 `active_bytes_` 與 `size_bytes_`；
3. 以 chunk 最後一筆 record 建立 terminal `WalPosition`；
4. 更新 `last_appended_position_` 並設定 `active_dirty_`。

這與設計要求的 per-chunk failure boundary 一致。後續 chunk 失敗時，不會回滾先前成功 chunk；這仍是
既有 fail-stop 語意。

### 3.3 介面、錯誤與測試

- `Wal` public API 未改變。
- `write_all()` 失敗前後的 publication 邊界未擴大。
- rotation integration test 已增加最後 EngineSeq、segment、offset、WAL size、sync 前後 durable
  position 與 replay assertions。
- Debug、ASan/UBSan、baseline/candidate Release suites 與 70 個 formal process 均成功。

目前沒有證據支持新增 fault-injection framework、替換 container 或重構 `Wal`；這些都超出本需求。

## 4. 必要修正

### 4.1 統一 g8192 writer 的 gate 身分

**問題**

原設計第 9.1 節規定 g8192 writer 只作 phase 方向性資料；但 procedure 第 11、13 節要求每一個 formal
profile-on run 都必須有至少 50 個 `profiled_groups`。因此 g8192 的 41--45 個樣本被用來否決整份
formal matrix，與原設計的 scope 不一致。

**必要修正**

- `profiled_groups >= 50` 只適用於用來判定 15% publish reduction 與 append direction 的
  g4096 writer profile-on runs。
- g8192 writer 繼續收集方向性資料；若樣本不足或 profile bias 超標，該段只標成
  `directional / insufficient samples`，不得讓 g4096 primary、direct WAL 或 Engine gate 失效。
- g8192 direct WAL 的 <=3% regression gate維持不變，因為它不是 profile-on 方向性 workload。
- sampling calibration 必須以 g4096 baseline/candidate 的 off/on bias 選定共同 `SAMPLE_EVERY`；
  g8192 bias只決定其方向性 phase資料是否可解讀，不決定整體有效性。

這是 scope 修正，不是看到結果後放寬 primary acceptance gate。

### 4.2 依既定 guardrail 更正結果分類

**問題**

移除不必要的 g8192 validity gate後，本輪不是 `inconclusive`。g4096 primary 樣本數、calibration、
correctness、identity、direct WAL 與 authoritative Engine 都有效，但以下既定 performance guardrail
失敗：

- writer g4096 p99.9 median：`414,024 -> 418,753 us`，惡化 1.14%；paired runs為 3/5 惡化；
- writer g4096 involuntary context switches：`2,705.48 -> 3,483.52 / M commands`，惡化
  28.75%，paired runs為 5/5 惡化。

procedure 第 14 節規定，資料有效但任一必要 performance gate 失敗時，結論是
`no material gain / do not retain`，而不是 `inconclusive` 或 `pass`。

**必要修正**

- report 結論改為 `performance guardrail failed / do not retain`；可保留 publish、append、direct WAL
  與 Engine 的 observed improvement，但不得稱為已接受結果。
- p99.9 paired worsening 修正為 3/5；原文的 4/5 與 Appendix A 不一致。
- 不因 p99.9 只惡化 1.14% 而事後新增容忍值；原 gate 沒有這個例外。

### 4.3 修正 Appendix A 的 Engine 欄位錯位

**問題**

Appendix A 宣稱 writer/Engine 的 `max/group fill` 欄為 actual commands/group，但 Engine rows 的
`42.xx` 實際是 CPU seconds；後續欄位因此整列左移。正文中的 Engine group fill 約 4,092/group，
與附錄呈現不一致。

**必要修正**

- 將 latency max、actual commands/group、CPU seconds、voluntary 與 involuntary context switches
  拆成獨立欄位；所有 workload 使用相同欄數。
- 若 raw summary沒有某欄，填 `not measured`，不得移動後續欄位。
- 重新由 raw files產生 Engine五輪 rows，並核對正文 median可追溯到附錄。

此項只修正報告，不重跑 benchmark。

### 4.4 補足 WAL byte-for-byte correctness 證據

**問題**

原設計要求 baseline/candidate WAL bytes逐位元不變；目前 report證明了 replay、WAL size與 ordering，
但沒有列出相同 deterministic workload所產生 segment files的 byte hash或 `cmp` 結果。Replay成功與
size相同不能取代 byte identity證據。

**必要修正**

- 在 procedure 增加一個 correctness control：baseline與candidate各自用相同固定 commands、shard、
  segment size與prepare設定產生包含同 segment及rotation的WAL。
- close/sync後依 segment順序比較檔名、file size與SHA-256；任一差異即為 correctness failure。
- 將比較 manifest與diff保存在RUN_ROOT，report只記錄結果與artifact路徑。
- 不需要對數十GiB formal data逐檔互相比較；一個涵蓋不 rotation及跨 segment rotation的 deterministic
  control足以驗證本修改所聲稱的不變量。

這個 control 已執行：未 rotation 與跨 segment rotation 的 baseline/candidate manifest diff 均為空，
segment filename、size 與 SHA-256 逐項相同；artifact 保存在
`/home/neojhou/wal-publish-bookkeeping-wQgSFDz7/logs/byte-identity/`。因此 byte-for-byte gate 的
直接證據已補足。

### 4.5 讓 repository 狀態符合保留結論

**問題**

目前 HEAD `ea72a31` 已包含 candidate production code，但同一commit內的報告明確寫
`retain candidate: no`。這使讀者無法判斷目前程式碼是正式接受版本或未通過的實驗版本。

**必要決策與作法**

依既定 gate，本輪 candidate不得標記為正式保留。最小且可稽核的作法是：

1. 先完成本節所列文件更正與 byte-identity correctness control；
2. 若不另立一份預先定義的 confirmatory experiment，使用新的 revert commit只還原
   `src/persistence/wal.cpp` 與相依測試調整，保留設計、procedure與report作歷史證據；
3. 不改寫、squash或刪除原commit，讓candidate與拒絕原因可追溯；
4. 若要保留 candidate供確認測試，必須在文件與branch/release狀態明確標為
   `provisional, not accepted`，不得繼續在其上建立下一個production optimization後才補驗收。

直接把 report 的 `retain candidate: no` 改成 `yes` 不符合原設計，也不是允許的修正。

## 5. 是否需要重測

只為了把 g8192 writer從41--45個樣本增加到50個，**不需要重跑完整矩陣**；該要求不應是整體
acceptance gate。

目前有效資料已足以依原規則判定 performance guardrail失敗。若專案仍希望確認 context-switch結果是否
由CPU governor或host noise造成，必須另寫一份預先固定的 confirmatory procedure，而不是在原報告中
補跑較有利的輪次。該procedure至少需要：

- baseline與candidate由同一source identity建立，只有目標production diff；
- 固定CPU affinity與governor，記錄frequency、SMT、background load與filesystem狀態；
- g4096 writer profile off/on、direct WAL g4096及authoritative Engine g4096各五個完整A/B paired rounds；
- g4096每個profile-on run至少50個profiled groups；
- 保留所有慢fsync與storage-tail輪次；
- 預先定義如何合併本輪與confirmatory evidence。若兩輪結論相反，結果應為不穩定／不保留，不能只採用
  較新的有利結果；
- 保留第4.4節WAL byte-identity control的manifest與空diff作為驗收證據。

這個confirmatory experiment是獨立後續工作，不應塞回目前production patch。

## 6. 技術選型與取捨

- 保留per-chunk bookkeeping：程式碼改動小、目標phase下降80.49%，機制與量測方向一致。
- 不擴充成prepared-batch representation、`writev`或新container：目前沒有必要證據，且會混淆歸因。
- 不把g8192方向性profile升格為gate：它不參與primary決策，且實際group fill不足。
- 不事後放寬context-switch guardrail：避免benchmark結果驅動驗收規則。
- 不直接開始StateMachine optimization：否則新的baseline會包含一個尚未被接受的WAL變更，後續效能
  歸因無法保持單一變因。

## 7. 已知限制與後續方向

- `wal_publish_ns_per_command`降低80.49%是穩定且可信的機制證據，但它不等於整體candidate已通過。
- `plan/copy` 在未修改其程式碼的情況下降低74.73%，顯示phase數值仍受跨group/cache/frequency或host
  狀態影響；不得把這項觀測宣稱為本patch的直接收益。
- p99.9受storage tail影響且五輪range很寬；原gate仍要求保留這項風險，而不是用median幅度小來忽略。
- Publisher lag與Completion worker不在本案範圍，不應成為接受或拒絕per-chunk bookkeeping的根因。

完成本review的必要狀態整理後，下一份獨立設計才可處理
`StateMachine::apply`／invariant validation的root-cause attribution。該設計應以最終被接受的WAL版本作
baseline，先量測precheck、book lookup、matching、event/result construction與validation，不直接平行化
StateMachine mutation owner。

## 8. 必要修改清單與完成定義

| 檔案／狀態 | 必要修改 | 預估規模 |
| --- | --- | ---: |
| design addendum或原設計第9節 | 明定g4096 profile sample gate與g8192方向性身分 | 8--15行 |
| benchmark procedure | 縮小sample validity範圍；增加WAL byte-identity control | 25--45行 |
| benchmark report | 更正result、3/5 paired count、Engine附錄欄位與byte證據狀態 | 20--40行 |
| production source狀態 | 依`do not retain`結論revert，或在confirmatory experiment前明確標成provisional | 約13行production diff；不得順帶重構 |

完成定義：

- 文件對g8192 writer的身分只有一種說法；
- 結論依預先定義的gate分類，不以程式已commit為由改寫結果；
- Appendix A與raw artifacts欄位可逐項對應；
- WAL byte identity有直接證據；
- repository中的candidate狀態與`retain/do not retain`結論一致；
- 沒有新增WAL格式、API、runtime config、worker、metrics或與本案無關的optimization。
