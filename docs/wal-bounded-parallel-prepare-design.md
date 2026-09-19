# WAL bounded parallel prepare 設計

## 1. Review 結論

本設計承接 `engine-writer-hot-path-root-cause-analysis-post-cleanup-retest-report.md`
的量測結果。現有 Engine durable 中位數約 164K commands/s；writer service 約
5.857 us/command，其中 WAL prepare 約 1.061 us/command、占 18.1%，且 payload
encode 與 CRC 合計占 prepare 約 65.5%。以 Amdahl 估算，四個 prepare lane 的理論
end-to-end 上限約為 +15.7%，符合進入 prototype 的 10% 門檻，但遠不足以單獨達成
1M commands/s。

因此下一案只做 **bounded parallel WAL prepare prototype**。它的目的不是宣稱已解決
1M/s，而是回答以下單一問題：在維持完全相同的 WAL bytes、ordering、durability 與
recovery 語意下，W=2／W=4 是否能在 group=4,096 的完整 writer durable path 取得至少
10% 的實測吞吐改善，且沒有不可接受的 latency 或 CPU contention。

此設計符合目前需求，且只包含驗證該問題所必需的變更。它不包含公開 production
設定、async WAL、fsync policy、StateMachine、Publisher、Completion queue 或通用
executor 的修改。prototype 通過後，是否公開設定與採用何種預設值必須另案決定。

## 2. 需求理解與合理假設

### 2.1 必須滿足

1. `CommittedCommand` 的輸入順序、`EngineSeq` 與產生的 WAL record 順序完全不變。
2. W=1 與 W=2／W=4 對相同 command batch 必須產生逐 byte 相同的 WAL segment。
3. WAL append、segment rotation、write、publication 與 `fsync` 仍由原本單一持有
   WAL mutex 的 caller 執行；只平行化不修改共享 WAL 狀態的 record preparation。
4. 任一 prepare task 失敗時，不得 write、rotate、publish record 或推進 durable
   position；回報輸入順序中最早的 error。
5. prepare worker 數量有固定上限；W 表示包含 caller 在內的總 prepare lane 數，
   所以 W=4 最多建立三個背景 worker。
6. batch 小於 threshold 時走既有單執行緒 prepare，避免 dispatch 成本放大小 batch
   以外的 latency。
7. worker 在 `Wal` open 時建立並重複使用；不得每 group 建立 thread，也不得形成
   無界 task queue。
8. prototype 預設 W=1，現有 public `Engine` 行為、API、production default 與資源用量
   不變。

### 2.2 合理假設

- `append_batch()` 在返回前，caller 提供的 command span 仍有效；worker 只讀取該
  span，並在函式返回前全部完成。
- 同一個 `Wal` 同時最多有一個 prepare job。既有 WAL mutex 在 prepare、append 與
  publish 全程持有，維持跨 caller 的 append 原子順序。
- `encode_committed_command()`、CRC 與 frame assembly 只依 command 與固定格式運算，
  不存取共享可變 WAL 狀態，適合在不同 record 間平行執行。
- 本階段在同一台六 CPU affinity 的測試環境比較 W=1／2／4；結果不能直接外推至
  不同 CPU、NUMA、storage 或 scheduler。

## 3. 明確不在範圍內

- 不修改 WAL record／segment format、CRC coverage、replay 或 retention schema。
- 不改 `append -> fsync -> apply -> completion` durability boundary，也不降低 RPO=0。
- 不平行化 segment planning、chunk copy、publish、write、rotation 或 `fsync`。
- 不平行化 `StateMachine::apply`，不改 single-instrument deterministic mutation owner。
- 不修改 Publisher replay、cursor persistence、Completion worker 或 ingress queue。
- 不新增 lock-free queue、work stealing、通用 thread pool、coroutine 或第三方 dependency。
- 不新增 thread affinity、scheduler priority 或自動依硬體核心數選擇 worker。
- 不修改公開 `RuntimeConfig`，也不在本案選定 production worker／threshold default。
- 不同時執行 group-size／group-delay／fsync policy 優化；那是獨立診斷，以免無法歸因。

## 4. 現況與必要修改

目前 `Wal::append_batch_unlocked()` 在 WAL mutex 內循序執行：

```text
commands
  -> prepare_records_unlocked()       # encode + CRC + frame，逐筆
  -> append_prepared_unlocked()       # plan/copy + rotate/write + publish
  -> Wal::sync()                      # ShardRuntime 之後呼叫
```

必要修改只有四組：

1. 在 internal `Wal` 加入固定、可停止、一次只處理一個 batch 的 prepare workers。
2. 讓 `prepare_records_unlocked()` 依 internal options 選擇 sequential 或 bounded
   parallel，但兩條路徑共用同一個 `encode_frame()`。
3. 調整 internal profile 語意，使 parallel task time 不會被錯誤地拿來與 wall-time
   parent 相減。
4. 擴充既有 writer／direct-WAL benchmark 與測試，以 W=1／2／4 驗證 bytes、replay、
   throughput、latency、CPU 與 fallback。

## 5. 架構與模組邊界

### 5.1 資料流

```text
Shard writer（持有 WAL mutex）
  |
  |-- batch < threshold、W=1 或只有一個非空 range
  |     `-- caller 依輸入順序 prepare 全部 records
  |
  `-- batch >= threshold 且 W>1
        |-- 將輸入切成最多 W 個連續 range
        |-- caller 處理其中一個 range
        |-- 最多 W-1 個長生命週期 worker 各處理一個 range
        `-- join barrier
              |-- 依原始 range／record 順序合併 PreparedRecord
              |-- 依輸入 index 選擇最早 error
              `-- 成功後才進入既有 append_prepared_unlocked()

既有單 writer 繼續執行
  -> segment plan/copy
  -> rotate/write
  -> publish in-memory index
  -> unlock append
  -> fsync
  -> StateMachine::apply
```

平行區段沒有 file descriptor、`records_`、segment metadata、durable position、Publisher
或 StateMachine 的存取權。

### 5.2 元件責任

#### `WalPrepareOptions`（internal）

```cpp
struct WalPrepareOptions {
  std::size_t lane_count{1};
  std::size_t min_parallel_commands{256};
};
```

- `lane_count` 是包含 caller 的總 lane 數，只接受 1、2、4。
- `min_parallel_commands` 必須大於 0，只在 `lane_count > 1` 時生效。
- 這個 type 位於 `src/persistence/wal.hpp`，不是 installed public API。
- `Wal::open()` 接受具預設值的 internal options；所有既有 caller 不傳參數時維持
  W=1。

`256` 只是 prototype 的可覆寫起始 threshold，不是 production 決策。正式 benchmark
必須同時量 group=256 與 group=4,096，報告 crossover；若 W>1 在 256 不具收益，後續
production design 必須提高 threshold。

#### `Wal::PrepareWorkers`（private implementation）

- 以 private incomplete type 加 `std::unique_ptr<PrepareWorkers>` 放在 `Wal`，實作留在
  `wal.cpp`，避免把同步細節擴散到其他模組。
- W=1 時不配置 worker object、不建立背景 thread。
- W>1 時只建立 `lane_count - 1` 個 `std::jthread`。
- 只容納當前一個 job；不得提供一般化 `submit()` queue 給其他模組。
- job 包含唯讀 command span、連續 range、每 lane result、generation 與完成計數。
- destructor／stop 先停止並 join workers，再關閉 WAL descriptor。

#### `prepare_records_unlocked()`

- 保留現有函式作為唯一入口。
- sequential 與 parallel lane 都呼叫同一個 `encode_frame()`；不得複製 binary codec、
  CRC 或 record-size validation。
- parallel path 依 record 數平均切割連續 range，range 數不超過 command 數或 W。
- 每 lane 只寫自己的 result；join 完成後 caller 依 range 起始 index 合併，確保輸出
  順序與 sequential path 相同。

#### `ShardRuntime`

- `ShardRuntime::open()` 最後增加具預設值的 internal `WalPrepareOptions` 參數，並原樣
  傳給 `Wal::open()`。
- public `Engine::open()` 不傳 options，仍使用 W=1。
- writer benchmark 可直接開 `ShardRuntime` 並注入 W=1／2／4；不得將 prototype knob
  加入 public `RuntimeConfig`。

## 6. 並行、記憶體與生命週期契約

### 6.1 Partition 與 ordering

- 對 N records 與 L lanes，建立 `min(N, L)` 個非空、連續且不重疊的 ranges。
- range 大小差距最多一筆；range 與 record 均依原輸入順序編號。
- `min(N, L) == 1` 時不啟動 parallel job，直接由 caller sequential prepare。
- 每 lane 可使用自己的 `std::vector<PreparedRecord>` 並預先 reserve range size。
- caller 在所有 lanes 完成後，依 range index move 到最終 vector；不得按完成時間合併。

### 6.2 同步

- caller 發佈 job 前完整初始化 command span、ranges 與 result slots；worker 取得 job
  必須具 acquire 語意。
- worker 完成 result 後，以 release 語意更新完成狀態並通知 caller。
- caller 等待全部 lanes 完成後才讀 results；不得 busy-spin 無上限。
- 只允許一個 active generation，worker 必須辨識 generation，避免 shutdown／下一批
  誤讀前一批狀態。
- callback、WAL I/O 或外部 user code 不在 prepare worker 上執行。

實作可使用 `std::mutex`、`std::condition_variable_any` 與 `std::jthread`；本設計不要求
自製 lock-free primitive。

### 6.3 記憶體上限

- 記憶體仍為 O(batch commands + encoded bytes)。
- worker queue 深度固定為一，不保留跨 batch commands 或 frames。
- 最終 ordered vector 透過 move 合併 lane-local records，不複製 frame bytes。
- 不在本案加入 buffer pool；若 allocation 抵銷平行收益，應以 benchmark 判定 prototype
  不通過，而不是擴大本次範圍。

### 6.4 Shutdown

- 正常 Engine stop 先 join shard writer，因此 `Wal` 銷毀時不應有新的 append caller。
- `PrepareWorkers::stop()` 設定停止狀態、通知全部 worker 並 join；不可 detach。
- 建立任一背景 thread 失敗時，`Wal::open()` 回傳 `wal_failure`，已建立的 worker 由 RAII
  停止；不得半套降級成與要求不同的 lane count。

## 7. 錯誤與 durability 語意

1. worker 只建立尚未 publish 的 `PreparedRecord`；所有 lane 成功前不得呼叫
   `append_prepared_unlocked()`。
2. 多個 lane 回報 error 時，以最小 command input index 的 error 為結果，不依 thread
   完成順序決定。
3. prepare error 後 `size_bytes_`、`active_bytes_`、`records_`、last appended／durable
   position、segment files 與 dirty flag 全部維持呼叫前狀態。
4. prepare 成功後的 rotate、partial write、sync failure 與 fail-stop 行為沿用既有實作；
   本案不改既有 I/O error semantics。
5. worker function 不得讓 exception 穿越 thread entry。worker 以 `std::exception_ptr`
   保存 exception；全部 lanes join 後由 caller 依最早 input index重新拋出，維持既有
   sequential allocation／codec exception語意並避免 `std::terminate`。thread 建立或
   synchronization setup失敗則由 `Wal::open()` 轉成 `wal_failure`。

## 8. Profile 語意的必要調整

現有 `prepare_ns` 是 caller 看到的 wall time，而 payload encode、CRC、frame assembly
是逐筆 elapsed time 的合計。parallel path 中 children 可重疊，總和可能大於 wall
parent，因此現有 `children <= prepare_ns` 驗證不能沿用。

在 internal `WalAppendProfile` 增加：

```cpp
std::uint64_t prepare_task_ns{};          // 各 lane task elapsed time 的總和
std::uint64_t parallel_prepare_groups{};  // 本 sample 使用 parallel path 的 group 數
std::uint64_t prepare_tasks{};            // 實際非空 ranges 數
```

語意如下：

- `prepare_ns`：完整 prepare 的 caller wall time，包含 dispatch、wait、merge 與 profile
  bookkeeping；它仍是計算 writer latency／Amdahl 的數字。
- `prepare_task_ns`：所有非空 lane task elapsed time 的合計；它不是 process CPU time，
  也不得與 `prepare_ns` 相加。
- encode／CRC／frame assembly counters：各 lane 使用 local profile 收集，join 後以
  saturating add 合併；不得讓多個 thread 寫同一 counter。
- prepare task remainder：`prepare_task_ns - (encode + CRC + frame assembly)`；只有這個
  hierarchy 要求不得 underflow。
- parallel dispatch／wait overhead 只由 `prepare_ns` 與吞吐結果反映；本案不再增加
  dispatch、barrier 等細碎 clocks，避免 instrumentation 反過來改變 prototype。

Writer profile 與 direct-WAL profile aggregation、輸出與測試必須改以
`prepare_task_ns` 驗證 child hierarchy。舊的 `wal_prepare_ns` output key保留為 wall
time；新增 `wal_prepare_task_ns`、`wal_parallel_prepare_groups` 與
`wal_prepare_tasks`。報告不得把 aggregate task time解讀成 end-to-end latency。

## 9. Benchmark 設計

### 9.1 Internal CLI

既有 benchmark 增加：

```text
--wal-prepare-workers=1|2|4
--wal-parallel-prepare-min-commands=N
```

- 只允許 `wal_write_ceiling` 與 `engine_writer_hot_path_profile` 使用。
- 未指定時 W=1，確保既有 workload 行為不變。
- 0、非數字、不支援 worker count、threshold=0，以及其他 workload 誤用都必須明確
  non-zero exit 並輸出穩定 error code。
- summary 必須輸出 worker count、threshold、實際 parallel groups／tasks，使報告能證明
  測到的確實是目標路徑。

### 9.2 Correctness matrix

至少包含：

- W=1／2／4，相同 commands 與 segment size；逐檔比較所有 WAL segment bytes。
- command 數不能被 lane 整除，以及 command 數小於 lane 數。
- batch 小於 threshold 時 W=2／4 必須走 sequential fallback。
- 大 batch 必須實際使用期望的非空 task 數。
- 跨 segment rotation 的 byte identity、position、size、write count 與 replay ordering。
- append + sync + close + reopen + replay 後 commands、EngineSeq 與 durable head 一致。
- 重複 open／append／stop，確認 worker 可正常 join，沒有 hang 或 leaked thread。
- normal 與 profiled parallel path 產生相同 bytes；profile counter 無 overflow／underflow。

不得只比較 decoded commands；byte-for-byte 比較是避免 parallel merge 改變 record 或
segment boundary 的必要驗證。

### 9.3 Performance matrix

所有正式 case 使用相同 Release binary、固定 affinity、相同 workload、每輪新空目錄，
每 case 五輪且 measured phase至少 15 秒。低吞吐與 fsync tail 有效輪次不得事後刪除。

1. writer durable，group=4,096：W=1／2／4，profile-off，作 end-to-end gate。
2. direct WAL per-group fsync，group=4,096：W=1／2／4，profile-off，分離 WAL ceiling。
3. writer group=256：W=1／2／4，確認 dispatch crossover 與 small-group 風險。
4. W=1／4 profile-on：先校準 sample interval，使各自 off/on median bias <=5%；若 N=8
   不通過，依序測 N=16、32，不得事後依 phase share挑 N。
5. public `engine_durable_single_instrument` W=1 control，確認 prototype injection 沒有改變
   public path。

執行順序交錯 W=1／2／4，避免 storage temperature 與 background writeback 全落在同一
配置。每輪至少回報：

- commands/s、elapsed、p50、p99、p99.9、max；
- prepare wall ns/command、task ns/command、append、sync 與 writer service；
- CPU utilization、voluntary／involuntary context switches、最大 RSS；
- actual commands/group、parallel groups／tasks、WAL bytes、rotations、write calls；
- correctness、replay、durable head 與 exit status；
- sync p50／p99／max，將 storage tail與 prepare CPU收益分開。

## 10. 驗收與停止條件

### 10.1 Mandatory correctness gate

任一 W=2／4 case 發生 WAL byte、ordering、segment boundary、replay、durable head、callback
count、sanitizer、hang 或 shutdown 不一致，prototype 立即不通過，不得用吞吐提升抵銷。

Release、Debug、ASan／UBSan 既有完整 tests 必須通過；TSan 若環境可用則另跑，但不為
本案新增共享 CI performance gate。

### 10.2 Performance gate

- 主要 gate：group=4,096 writer profile-off 的 W=4 五輪 median RPS 相對 W=1 至少 +10%。
- W=2／W=4 的結果都要保留；不得只報最佳 worker count。
- p50／p99、最差 p99.9／max、CPU 與 context switches必須完整報告。若 W=4 在多數配對輪次
  穩定造成超過 10% 的 p99 regression，或使 Publisher／Completion 出現持續 backlog，則不
  建議成為 production default，即使 throughput 通過。
- direct WAL 只用來解釋收益位置；其 profile bias >5% 時，nested share只能作方向性證據。
- 理論 +15.7% 不是驗收結果；實測未達 10% 時停止此方向，不追加 buffer pool、lock-free
  queue 或更多 worker 來挽救本案。

### 10.3 通過後的決策

- 通過：另案設計公開或 deployment-only configuration、選定 threshold/default、文件化 CPU
  budget，並在目標 Linux storage 上重測。
- 不通過：保留 W=1 production path，移除或不啟用 prototype；下一個最小診斷是細分
  `StateMachine::apply`，不是同案再擴充架構。
- 無論通過與否，本案都不宣稱達成 1M/s。W=4 理論上約只把 160K/s提高至 185K/s；後續仍需
  處理 apply 與 durable WAL ceiling。

## 11. 測試策略

### 11.1 Unit／integration

- `persistence_test.cpp`：options validation、W=1/W=4 byte identity、uneven ranges、threshold
  fallback、rotation、profile hierarchy、reopen/replay 與 repeated shutdown。
- `engine_writer_profile_test.cpp`：profile task fields/hierarchy、completion counts與 writer
  correctness一致；W=4 的 parallel group/task counter 與 options 傳遞由
  `persistence_test.cpp` 的 direct-WAL 整合案例及 benchmark smoke 覆蓋。
- 既有 persistence、recovery、publisher tests 全量 regression，不改 expected WAL format。
- benchmark CLI tests：合法 W=1/2/4 與所有非法／錯誤 workload 組合。

測試不得依 duration counter 必須非零，也不得以固定 RPS 作一般 CI assertion。正式吞吐 gate
只在操作文件定義的 controlled benchmark 執行。

### 11.2 Failure／race 檢查

- 驗證 prepare 失敗前不產生 WAL state mutation；若現有固定 command schema 無法自然製造
  record-size error，不為此新增 production fault-injection framework。options validation、
  ordered merge 的 byte/replay結果與既有可達錯誤測試覆蓋目前可觀察的失敗契約；最早
  input-index 的選擇由實作中的 ordered scan 保持 deterministic。
- ASan／UBSan 檢查生命週期與 overflow；可用時以 TSan檢查 job publication、result visibility
  與 shutdown。TSan 的低吞吐不納入 capacity結果。

## 12. 檔案層級實作指引

```text
src/persistence/wal.hpp
  WalPrepareOptions、WalAppendProfile必要欄位、PrepareWorkers private declaration

src/persistence/wal.cpp
  bounded worker生命週期、range partition、parallel prepare、ordered merge與profile aggregation

src/runtime/shard_runtime.hpp/.cpp
  internal options傳遞；public Engine仍走預設W=1

benchmarks/engine_writer_profile_benchmark.hpp/.cpp
  writer options、summary、parallel profile hierarchy與結果欄位

benchmarks/order_book_benchmark.cpp
  CLI parse/validation，writer與direct-WAL options傳遞

benchmarks/CMakeLists.txt
  只增加必要的CLI負面測試

tests/integration/persistence_test.cpp
  bytes/order/rotation/replay/fallback/profile/shutdown測試

tests/integration/engine_writer_profile_test.cpp
  ShardRuntime整合與counter一致性
```

不需要新增 public header、dependency、WAL migration、通用 worker library或新的 production
service。預估 production/internal code約 220～340 行、benchmark／CLI約 100～180 行、tests約
180～280 行；若實作顯著超出此範圍，應先檢查是否引入了本設計排除的抽象或功能。

## 13. 關鍵設計決策與取捨

### 長生命週期 worker，而非每 batch 建 thread

prepare 每筆約 1 us；每 group 建立 thread 的固定成本足以吃掉預期收益。固定 workers是此
prototype成立的必要條件，但只服務單一 WAL，不抽象成全系統 executor。

### caller 參與工作

W 表示總 CPU lane，caller 處理一個 range，可少一個 thread、減少一次 handoff，並讓 W=4 的
CPU budget可明確解讀為最多四個同時 prepare lanes。

### WAL mutex仍涵蓋 prepare

純 prepare 理論上可在 mutex 外執行，但那需要 append reservation／ticket與跨 caller ordering
協定，會擴大 correctness surface。現有 writer只有單一 append owner；保留 lock boundary是本案
最低風險作法，也不比現況增加 Publisher read blocking時間。

### internal options，不先擴 public config

目前只有 Amdahl 上限，尚無實測收益、threshold或跨機器 CPU budget。先讓 benchmark可注入
W=2／4，public Engine維持 W=1，避免將未驗證 knob變成向使用者承諾的 API。

### 不將 task elapsed 誤稱 CPU time

`steady_clock` 量到的是每 lane elapsed，不是 scheduler thread CPU time。報告以 aggregate task
time描述 encode／CRC構成，以外部 process CPU觀測資源成本；兩者不可混用。

## 14. 已知限制與可擴充方向

- 單一 WAL 專用 worker會增加每 shard thread數；prototype通過後仍需評估多 shard 的 thread
  budget，不能直接把每 shard W=4套用到所有 deployment。
- allocator、cache bandwidth或 SMT contention可能使實際收益遠低於 Amdahl估算；本案不預設
  根因一定是算力不足。
- fsync tail仍可掩蓋 CPU改善，必須同輪記錄 sync distribution，但不可藉此改 durability policy。
- 達成 1M/s 還需要後續處理 `StateMachine::apply` 與 WAL durable ceiling；本設計只驗證第一個
  有界、可回退的 CPU 優化。
- 若 prototype通過，未來可以另案決定 public config、shared per-process CPU budget或 shard間
  worker共享；這些都不是本次實作的預留需求。

## 15. 設計與實作一致性清單

- [ ] public `RuntimeConfig`、WAL format與production default未改變。
- [ ] W=1不建立背景 prepare thread，並走既有 sequential algorithm。
- [ ] W=2／4只平行 encode、CRC與frame assembly。
- [ ] W包含caller，背景 thread數為W-1且永不超過3。
- [ ] 小 batch依threshold確實fallback，沒有空task。
- [ ] output依input index合併，不依完成順序。
- [ ] 全部prepare成功前沒有WAL共享狀態或檔案 mutation。
- [ ] W=1與W=4跨rotation產生逐byte相同segments並可replay。
- [ ] worker local profile無data race，child hierarchy改用`prepare_task_ns`。
- [ ] shutdown可喚醒並join全部worker，沒有detach或hang。
- [ ] benchmark同時報W=1／2／4、低吞吐有效輪次、latency與CPU成本。
- [ ] W=4只有實測writer median提升至少10%才進入下一個production design。
- [ ] 沒有順帶修改fsync、apply、Publisher、Completion、queue或加入通用executor。
