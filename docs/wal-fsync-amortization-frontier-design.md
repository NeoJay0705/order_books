# WAL fsync 攤提上限設計審查與必要工作

## 1. Review 結論

本設計接續下列已完成工作：

- `docs/engine-writer-throughput-optimization-benchmark-report.md`；
- `docs/wal-bounded-parallel-prepare-benchmark-report.md`；
- `docs/wal-parallel-prepare-production-configuration-benchmark-report.md`；
- `docs/wal-sync-final-attribution-benchmark-report.md`。

現有證據已足以確認：

1. WAL data `write` 不是主要 tail；慢 durable interval 主要落在 WAL `fsync` syscall；
2. filesystem／kernel／device 的下一層責任尚無穩定證據，不得直接宣稱硬體故障；
3. 舊有 group-size scan 已測過 256--8,192，且 W=1 在 4,096 後平台化；
4. bounded parallel prepare 後，W=2／W=4 在 group=4,096 有方向性收益，但尚未與更大的
   group size 做同一固定矩陣；
5. delay scan 已證明 saturated workload 將 delay 從 200 us 提高至 5 ms 不會增加 throughput。

因此，下一步必要工作不是重做舊矩陣，也不是再增加 fsync 監測，而是回答一個尚未被量測的問題：

> bounded parallel prepare 生效後，提高 durable group size 是否能以較少的 fsync/command 提高
> 單一 instrument writer ceiling，且收益是否足以支持後續 production candidate？

本階段只建立固定、可重現的 ceiling frontier。現有 benchmark、telemetry 與 runtime configuration
已具備所需能力，**不需要修改 production C++ 程式碼或 public API**。必要產物只有本設計、後續獨立
操作文件與結果報告。若結果未通過 gate，不得為了追求較高數字而臨時增加 group、修改 durability
或更換 sync primitive。

## 2. 需求理解與合理假設

### 2.1 目標

本階段必須回答：

1. W=1、W=2、W=4 在 group=4,096／8,192／16,384 的 direct WAL 與完整 Engine durable
   throughput、commands/fsync 與 sync tail 分別是多少？
2. W=2／W=4 是否將先前 W=1 的 4,096 plateau 往後推移？
3. Engine 與 direct WAL ceiling 的差距是否仍顯著，避免把所有剩餘差距都錯歸因於 fsync？
4. 若較大 group 提升 throughput，代價是多少 durable completion latency、記憶體與 CPU？
5. 在固定 gate 下，是否存在值得另案評估的 production candidate？

### 2.2 非目標

本階段不宣稱達到 production SLA，也不以 ceiling workload 直接選定 production default。特別不做：

- 不修改 `fsync` 次數以外的 durability 語意，也不跳過 per-group sync；
- 不改用 `fdatasync`、`O_DSYNC`、`O_DIRECT`、io_uring 或非同步 acknowledgement；
- 不修改 WAL format、segment size、rotation、recovery 或 RPO=0 boundary；
- 不再加入 strace、eBPF、block trace 或 production hot-path telemetry；
- 不重新掃描已證明無 throughput 收益的 saturated delay 200--5,000 us；
- 不優化 Publisher／Completion worker；
- 不將 W=4 diagnostic 結果當成 production configuration acceptance；
- 不修改 `RuntimeConfig` default、`WalPrepareOptions` default 或文件中的 production 建議值；
- 不建立通用 benchmark framework、JSON schema 或新的第三方依賴。

這些工作不是回答 fsync 攤提 frontier 所必需，納入會混淆單一變因或超出需求。

### 2.3 工作負載假設

- 只測一個 instrument、一個 shard，符合目前單一交易對 ceiling 目標。
- ceiling run 使用持續 backlog；其 latency 是飽和情境的 durable completion latency，不代表
  production fixed-rate arrival。
- `group_commit_max_commands` 是上限，必須以 `actual_commands_per_group` 判斷實際 batch fill。
- W 表示 WAL prepare lanes，包含 caller；W=1 是 production baseline，W=2 是現有 candidate，
  W=4 只作 ceiling diagnostic。
- 報告延用既有工程參考線：durable completion p99 <=20 ms、p99.9 <=50 ms，但只作標示。
  Saturated closed-loop queue latency不能取代fixed-rate production SLO驗證，也不影響ceiling數據是否有效。

## 3. 系統邊界與資料流

```text
producer lanes
  -> Engine ingress queue
  -> shard writer collects up to configured group
  -> admission / EngineSeq assignment
  -> WAL prepare (W=1 / W=2 / W=4)
  -> append one durable group
  -> fsync once for the group
  -> StateMachine::apply in EngineSeq order
  -> invariant validation
  -> completion handoff

comparison control:
fixture -> WAL prepare -> append -> fsync -> replay verification
```

固定比較包含兩條既有路徑：

- `engine_durable_single_instrument`：量測完整 shard writer durable path；
- `wal_write_ceiling`：量測 direct WAL prepare／append／fsync ceiling。

兩者使用相同 group size、prepare lanes、parallel threshold、filesystem 與 per-group fsync。兩條
workload 的 latency 定義不同：Engine 是 command durable completion latency；direct WAL 是 group
latency。報告不得把兩者 latency 放在同一欄作數值比較。

## 4. 技術方案與必要修改

### 4.1 重用既有能力

目前已存在且足夠的介面：

```text
--workload=engine_durable_single_instrument|wal_write_ceiling
--engine-group-size=N
--engine-group-delay-us=N
--engine-producer-lanes=N
--wal-group-size=N
--wal-sync=per_group
--wal-prepare-workers=1|2|4
--wal-parallel-prepare-min-commands=N
--engine-tail-telemetry-output=<path>
--engine-tail-state-sampling=off
```

Engine summary 已輸出 commands/s、completion p50/p99/p99.9/max、actual commands/group、
WAL MiB/s、group commits、parallel groups/tasks、rotation 與 correctness；collector-only telemetry
已輸出 measured sync p50/p99/p99.9/max、total duration 與 >25/>100/>250 ms count。

Direct WAL summary 已輸出 append、sync、group total latency、WAL bytes、parallel path counters 與
replay verification。因此不新增 summary 欄位、不新增 metrics、不新增 runtime option。

### 4.2 唯一必要新增產物

實作階段只新增：

1. `docs/wal-fsync-amortization-frontier-benchmark-procedure.md`：固定命令、輪序、pilot、
   artifact validation、統計與報告格式；
2. 執行後的 `docs/wal-fsync-amortization-frontier-benchmark-report.md`。

若實作者發現上述 CLI 或輸出實際缺少設計列出的資料，必須先記錄設計偏差；只允許補足缺失的
benchmark-only validation／output 與對應小型測試，不得藉此修改 production path。

## 5. 固定實驗矩陣

### 5.1 共同設定

所有正式輪固定：

```text
instrument/shard             1 / 1
sync mode                    per_group fsync
group delay                  1,000 us
group sizes                  4,096 / 8,192 / 16,384
prepare lanes W              1 / 2 / 4
parallel prepare threshold   4,096
producer lanes               16,384（Engine 全 case 固定）
tail telemetry               collector-only；state sampling off
CPU affinity                 同一組 CPU
filesystem / WAL device      同一 filesystem 與 device
build                        同一 ReleaseBenchmark binary
```

固定 16,384 producer lanes 是為了讓各 group 有相同 ingress concurrency 上限，避免 group 增加時
同時改變 producer supply。若 setup 因明確的記憶體或既有 queue capacity 限制失敗，整個 16,384
group column 標為 resource-limited；不得只降低其中某個 case 的 lanes 後混入主矩陣。

### 5.2 Case 數量

主矩陣固定為：

```text
2 workloads x 3 group sizes x 3 W values x 5 formal rounds = 90 runs
```

W=4 必須標為 diagnostic。W=2 是否可成為 production candidate 由第 8 節 gate 判定；不得用 W=4
取代 W=2 gate。

### 5.3 Pilot 與正式持續時間

- 每個 workload/group/W 先做一次未納入統計的 smoke，驗證 CLI、parallel/fallback path 與 replay。
- 以有限 pilot 推算共同負載預算，使同一 workload 的最慢與最快 case 都能在 timeout 內完成，且每個
  正式 run 的 measured phase 至少 20 秒。Engine 的 `iterations` 是 crossing-pair 數；direct WAL
  的 measured commands 是 `iterations * group_size`，兩者不能共用同一個 iterations 值。
- Engine 同一 workload 的正式 case 使用相同 `iterations`；direct WAL 使用固定的 command budget，
  並選為 16,384 的倍數，再依 group size 換算每個 case 的 iterations，確保 4,096／8,192／16,384
  case 的 measured command count 相同。不得為較慢 case 任意減少負載。
- 每個 case五輪；輪序在 procedure 預先固定並交錯 W/group，避免單向時間漂移。
- 正式矩陣開始後，不因結果好壞追加輪次或改變參數。

## 6. 核心資料模型與統計

每輪至少記錄：

```text
identity:
  workload, group_size, W, threshold, producer_lanes, delay_us

throughput:
  commands_per_second, elapsed_ms, wal_mib_per_second

amortization:
  wal_group_commands, wal_group_commits
  actual_commands_per_group
  syncs_per_second = wal_group_commits / measured_seconds
  commands_per_sync = wal_group_commands / wal_group_commits

latency:
  Engine durable completion p50/p99/p99.9/max
  direct WAL group p50/p99/p99.9/max
  measured fsync p50/p99/p99.9/max
  measured fsync >25/>100/>250 ms counts

path proof:
  actual_parallel_prepare_groups, actual_prepare_tasks
  segment rotations, WAL byte delta
  correctness_verified or replay_verified

resources:
  process CPU, max RSS, voluntary/involuntary context switches
```

五輪 aggregate 使用 median throughput、median latency、min--max throughput；p99.9/max 另外回報
五輪 worst value，不能只用 median 隱藏 storage tail。每個 aggregate 必須保留五輪明細與原始 stdout。

不將 traced run 納入本矩陣。最終 syscall attribution 已完成，再使用 strace 只會增加 observer effect，
沒有本階段所需的新資訊。

## 7. Correctness、錯誤與有效性

### 7.1 執行前 gate

- ReleaseBenchmark 全測試通過；
- Debug 全測試通過；
- ASan/UBSan 全測試通過；
- targeted benchmark smoke 通過；
- `git diff --check` 與 `git diff --cached --check` 通過；
- 記錄 HEAD、cached diff hash、tracked worktree diff hash 與 binary SHA-256。

上述 gate 不要求修改或清空既有 staging。

### 7.2 每輪有效性

Engine run 只有同時符合下列條件才有效：

- exit status 0，未 timeout；
- measured commands、group commands、completion count一致；
- group commits >0，sync sample count等於 group commits；
- W=2／W=4 在 group >= threshold 時 parallel groups >0，W=1 為0；
- WAL reopen/replay、durable head與 expected EngineSeq驗證成功；
- telemetry無 dropped sample、overflow或 sampler error；
- measured phase >=20 秒。

Direct WAL run 另要求 WAL byte delta一致、replay verified、sequence完整且 sync count符合 group數。

### 7.3 失敗處理

- correctness、replay、counter或telemetry validation失敗：該 case為 invalid，不得納入效能結論；
- timeout或資源不足：保留 artifact並標示 resource-limited，不得偷偷縮小 workload；
- storage tail：屬真實結果，保留且納入 worst-tail，不得重跑取代；
- setup錯誤且尚未開始 measured phase：允許修正命令後從整個固定矩陣重新開始，並記錄原因；
- 正式 run 中途發現 binary/source identity改變：整個矩陣無效。

## 8. 判定規則

### 8.1 Ceiling frontier

對每個 workload與 W分別建立 group Pareto frontier：

- group增加後 median throughput uplift <5%，視為已平台化；
- actual commands/group < configured group的90%，不得宣稱該 group 已充分測得；
- 報告選擇「距同一 W 最大 median throughput 5% 內的最小 group」作 ceiling Pareto point；
- tail、CPU、RSS 必須並列，不得只排序 RPS。

### 8.2 Fixed-rate canary candidate gate

只有 W=2 可以進入「值得另案做 fixed-rate canary」的 candidate 判定，且必須同時符合：

1. 相對同 group W=1 的五輪 median throughput uplift >=10%；
2. 相對 `group=4,096, W=1` 的五輪 median throughput uplift >=10%；
3. 五個配對輪中，completion p99 超過對照 10%的輪數少於3；
4. correctness、replay、parallel-path及resource validity全部通過；
5. benchmark完成bounded queue、全部completion及publisher drain，沒有未完成工作。

上述 gate通過只代表可以另立fixed-rate canary設計；它不是production acceptance。Production p99/p99.9、
publisher lag time-series、CPU budget與rollback必須在該canary中驗證，本階段不提前實作。

若只有 W=4 通過 throughput gate，結論仍是 diagnostic ceiling improvement，不是 production acceptance。

### 8.3 一百萬 commands/s 解讀

- 若 direct WAL 任一有效 case仍低於1M commands/s，完整 Engine在相同單 writer、durability與storage
  條件下沒有達到1M的證據；報告列出百分比差距，不宣稱增加Engine層調參即可達成。
- 若 direct WAL達到但 Engine未達到，下一階段才回到 Engine/WAL之外的剩餘 service gap。
- 若兩者都達到，仍必須通過 production latency與fixed-rate canary後才能成為部署結論。

## 9. 驗收條件

本設計完成需具備：

- 固定90輪矩陣或明確、可重現的resource-limited結果；
- 所有有效輪的correctness/replay與path proof；
- direct WAL與Engine各自的group/W frontier；
- commands/fsync、sync/s與measured fsync tail；
- 五輪明細、aggregate、worst tail與raw artifact位置；
- fixed-rate canary candidate gate逐項pass/fail；
- 對1M commands/s差距的量化結論；
- 明確說明production defaults是否保持不變。

若沒有candidate通過，正確結果是維持現況並停止group擴張；不得自動進入fdatasync、放寬durability
或改動filesystem設定。

## 10. 關鍵決策與取捨

| 決策 | 選擇 | 理由 |
| --- | --- | --- |
| 下一步 | parallel prepare後的fsync amortization frontier | 這是現有證據唯一尚未覆蓋的group維度 |
| Delay | 固定1 ms | saturated delay scan已完成，重測無資訊增益 |
| Sync primitive | 保持fsync | 符合既有durability contract並維持單一變因 |
| Instrumentation | 重用collector-only telemetry | 已驗證bias且資料足夠，不再增加observer effect |
| W=4 | diagnostic only | 既有production設計只允許W=2 candidate |
| Default | 本階段不改 | ceiling workload不能直接代表production arrival |
| 失敗輪 | 保留 | storage tail是被測系統的一部分，不可挑選最快輪 |

## 11. 已知限制與可擴充方向

- 結果只適用於報告記錄的host、CPU、kernel、filesystem、storage、compiler與build flags。
- Closed-loop saturation可量測ceiling，但不能建立production arrival-rate latency曲線。
- p99.9在sync sample較少時解析度有限，因此同時保留threshold counts與max。
- 更大group增加unacknowledged commands、queue residence與記憶體，不改變RPO=0但會增加故障時重送量。
- 單一instrument必須保序；本設計不以多shard水平擴充掩蓋單一writer ceiling。
- 若本矩陣平台化且direct WAL仍低於目標，後續只能另案評估storage/fsync policy或更大架構變更；
  任何durability語意變更都必須先更新需求與crash-consistency設計。

## 12. 實作認知對齊

本設計中的「實作」是建立並執行固定benchmark procedure，不是修改Engine或WAL production code。
實作者不得因一般實作階段模板提到測試檔案，就創造沒有需求的C++抽象或測試target。既有CLI、summary、
telemetry與correctness tests已覆蓋本階段所需能力；先編譯與執行既有tests，再由獨立procedure產生
新證據，才是與本設計一致的最小實作。

若實際檢查發現既有能力與第4.1節不符，允許的調整只有：

1. 補足缺少的benchmark-only欄位或validation；
2. 為該缺口新增最小unit/integration smoke；
3. 在報告中記錄原設計、實際問題、調整與取捨。

不得在同一變更中修改production default、durability boundary、WAL format或sync primitive。

## 13. 設計調整紀錄

原始設計草稿以「同一 workload 共用 iteration count」描述正式矩陣；檢查既有 benchmark 後發現
`engine_durable_single_instrument` 的 measured commands 是 `iterations * 2`，而 `wal_write_ceiling`
的 measured commands 是 `iterations * wal_group_size`。若直接共用 400,000 iterations，16,384 group
會產生數十億 direct-WAL commands，無法在預定 timeout 內完成，也不能形成公平的 group-size 比較。

必要調整：

- Engine 仍使用同一 `ENGINE_FORMAL_ITERATIONS`；
- direct WAL 改用固定且可被 16,384 整除的 `WAL_FORMAL_COMMANDS`；
- 每個 direct-WAL case 以 `WAL_FORMAL_COMMANDS / group_size` 換算 iterations。

這不改變 production path、WAL 語意或統計定義；代價是不同 group case 的 CLI iterations 不同，報告
必須改以 measured command delta 驗證相同負載，而不是比較 iterations 數。這是為了避免無意的超長
run 與 group-size 造成的 workload confounding，屬本設計必要且局部的修正。
