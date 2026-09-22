# StateMachine／WAL Component Ceiling 壓測報告

## 結論

- **Result：`preflight-busy`／`collection-incomplete`，不是有效 campaign。**
- 本次依操作文件執行兩輪完整 campaign；兩輪都在 WAL append-return 矩陣的 per-case
  storage preflight 停止，沒有產生 `valid-component-ceiling`。
- 最新 run root：
  `/home/neojhou/engine-writer-unified-campaign-40jKMTMa`。
  重複 run root：`/home/neojhou/engine-writer-unified-campaign-FtaaiOa0`。
- 最新輪完成 calibration 14/14、StateMachine 20/20、WAL append 11/25；未執行
  rotation attribution、durable fsync 25 cases 或 profile/control 11 cases。
- 另完成一個 `batch=4096` durable fsync 單輪補充觀測；它不併入正式 5-round
  frontier 或 ceiling 判定。
- StateMachine 的數字與已完成 append case 只能作 partial observations，不能宣稱為
  component ceiling、production limit 或 1M commands/s SLA。
- 兩次停止原因都只有單一 `disk_aqu_max` 超過門檻：第一次 `0.89`，第二次 `0.70`；
  同時 CPU idle、iowait、disk util 與 `disk_aqu_p95` 都合格。

## 1. 測試範圍與量測邊界

本次依據 `docs/state-machine-wal-component-ceiling-design-review.md` 與
`docs/state-machine-wal-component-ceiling-benchmark-procedure.md`，只執行：

1. 固定 command 數量的四種 StateMachine scenario；
2. `Wal::append_batch()` 的 batch `1／256／1024／4096／8192` 規劃矩陣，
   `sync=none`；
3. 預計執行但未到達的 `append_batch() + Wal::sync()` durable matrix；
4. CPU affinity、CPU／storage preflight、GNU `time`、`mpstat` 與 `iostat`。

StateMachine completion boundary 是 `state_apply_return`，只報 run-average
`average_ns_per_command`，不虛構 p50/p99。append completion boundary 是
`append_batch_return`；`sync=none` 仍受 page cache、writeback 與 filesystem 影響，
不是純記憶體 benchmark。

本次沒有啟動完整 Engine frontier、Publisher、Completion、`perf`、`fio` 或
network/downstream，也沒有修改 production code、sysctl、CPU governor、IRQ、cgroup
或 Git index。

## 2. Identity 與環境

| 項目 | 最新 run |
| --- | --- |
| Run root | `/home/neojhou/engine-writer-unified-campaign-40jKMTMa` |
| Console log | `/home/neojhou/state-machine-wal-component-campaign-fgPxMwWh.log` |
| 重複 run root | `/home/neojhou/engine-writer-unified-campaign-FtaaiOa0` |
| 執行時間 | 2026-09-22 16:02（`logs/environment-before.txt`） |
| HEAD | `659c4abba82a02e9b3acc1b83722278cd14d3048` |
| cached diff SHA-256 | `be5aeaeba014c1e28337785203bdb4fa1d20d0d3c7eaee98225b7043593ebc6d` |
| worktree diff SHA-256 | `37d1d33c198a7d672ae4ef2e3d92a907568095bf99b0903c0cb3dd480086438e` |
| untracked manifest SHA-256 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| Benchmark binary SHA-256 | `6d338f4503a34f0068fea46debadb7697f33b8db1d4c94b1807b63047785bafe` |
| Host | AMD Ryzen 7 3700X，16 logical CPUs，SMT enabled |
| Kernel | Linux `6.17.0-35-generic` |
| Filesystem | `/` on `/dev/sdb2`，ext4，block device `sdb` |
| Benchmark affinity | CPUs `2-7` |
| Observer affinity | CPUs `0-1` |
| Preflight CPU list | `2,3,4,5,6,7` |
| CPU policy | CPUs 2-7 `powersave`，EPP `balance_performance`，boost `1` |

`logs/repository-identity-before.txt` 與 `repository-identity-current.txt` 一致，
binary SHA-256 一致；runner 因 preflight-busy 停止，沒有完整 after-campaign identity
artifact。cached hash 在測試前後仍一致。

## 3. Build、idle 與 validity gates

| Gate | 結果 | 證據／說明 |
| --- | --- | --- |
| Release build | PASS | `release-benchmark` binary 已建立並凍結 SHA-256 |
| CTest | PASS | 154/154 tests passed |
| Runner contract／component dry-run | PASS | scope isolation、identity、observer contract 通過 |
| 外部 idle screening（首輪） | PASS | idle min `97.2947%`、iowait p95/max `0/0%`、sdb util avg `0.0967%`、aqu-sz p95/max `0.01/0.08` |
| 外部 idle screening（重跑前） | PASS | idle min `93.94%`、iowait p95/max `0/1.01%`、sdb util avg `0.1033%`、aqu-sz p95/max `0.01/0.07` |
| Calibration | PASS | 最新輪 14/14；不套用 formal duration gate |
| StateMachine formal cases | PASS | 20/20；correctness 全部通過，CV `0.963–1.387%` |
| WAL append formal cases | INCOMPLETE | 最新輪 11/25；b1、b256 五輪，b1024 僅 r1 |
| Failed per-case preflight（首輪） | STOP | `append-r4-b1024`：CPU idle `97.4927%`、iowait p95/max `0/1%`、util `0.11%`、aqu p95/max `0.01/0.89` |
| Failed per-case preflight（重跑） | STOP | `append-r2-b1024`：CPU idle `97.524%`、iowait p95/max `0/0%`、util `0.40%`、aqu p95/max `0.01/0.70` |
| Durable fsync matrix | NOT RUN | 兩輪均在 append storage gate 前停止 |
| Profile/control | NOT RUN | 未建立 profile bias 證據 |
| Final classification | **FAIL／STOP** | `logs/result.txt`: `result=preflight-busy` |

停止不是 benchmark command、counter、replay 或 parser failure；runner 依文件在
storage gate 失敗時安全停止並保留 preflight JSON、summary、stdout、status 與 time。

## 4. Frozen plan 與完成度

| Kind | Batch／scenario | Planned formal cases | 最新輪完成 |
| --- | --- | ---: | ---: |
| StateMachine | 4 scenarios × 5 rounds | 20 | 20 |
| WAL append-return | `1/256/1024/4096/8192` × 5 rounds | 25 | 11（另有 b1024 r1） |
| Rotation attribution | control／trigger | 10 | 0 |
| WAL durable fsync | `1/256/1024/4096/8192` × 5 rounds | 25 | 0 |
| Profile／control | append、fsync、representative control | 11 | 0 |

最新版 frozen plan 的 StateMachine 固定 `66,774,800` measured commands/case；append
固定 `30,564,352` commands/case，預估 WAL bytes 約 `3,731,849,216`/case；fsync
計畫已凍結但未執行。

## 5. StateMachine 結果（partial observations）

以下均為最新 run 的五輪中位數與範圍，completion boundary 為
`state_apply_return`，不是整條 Engine path 的 ceiling。

| Scenario | Commands/case | RPS median | RPS min–max | Avg ns/cmd median | CV | Correctness |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `new_crossing_pair` | 66,774,800 | 1,076,970 | 1,056,860–1,088,270 | 928.533 | 0.963% | 5/5 true |
| `new_resting_cancel` | 66,774,800 | 1,438,170 | 1,404,260–1,449,490 | 695.327 | 1.158% | 5/5 true |
| `amend_quantity` | 66,774,800 | 1,761,760 | 1,713,460–1,783,120 | 567.615 | 1.377% | 5/5 true |
| `replace_order` | 66,774,800 | 1,417,440 | 1,402,010–1,458,500 | 705.498 | 1.387% | 5/5 true |

GNU `time` 的 whole-process CPU 約 99%，涵蓋 setup、warmup、measured loop、replay
與清理；不能直接解讀為純 matching CPU。這些數值雖高於 1M，因完整 component
campaign 無效，不能宣稱為 accepted ceiling。

## 6. WAL append-return 結果（partial）

所有已完成 append case 都有 `wal_byte_plan_verified=true`、`measured_segment_rotations=0`
與 `replay_verified=true`。五輪 batch 的統計只有在五輪且 CV 合格時才可作候選；整個
append matrix 未完成，故下表全部標為 partial。

| Batch | Completed rounds | Wall RPS median | Service RPS median | Append p50／p99／p99.9 (us) | Worst max (us) | CV | Validity |
| ---: | ---: | ---: | ---: | --- | ---: | ---: | --- |
| 1 | 5/5 | 334,971 | 357,892 | 2.455／5.380／9.848 | 161,000 | 0.900% | partial |
| 256 | 5/5 | 755,678 | 788,575 | 265.237／324.899／3,762.492 | 171,890 | 1.300% | partial |
| 1024 | 1/5 | 745,069 | 777,923 | 1,040／1,398／88,780.747 | 200,554 | n/a | rejected/incomplete |
| 4096 | 0/5 | — | — | — | — | — | not run |
| 8192 | 0/5 | — | — | — | — | — | not run |

Append process 的 whole-process CPU 約 79–87%，最大 RSS 約 1.2 GiB；每個已完成
case 約寫入 3.73 GiB。這些是 process／device evidence，不是 fsync ceiling，也不能
用來推導裝置理論頻寬。

### 6.1 單獨 batch=4096 補充觀測（非 formal case）

為補足正式矩陣在 `batch=4096` 前停止的資料，另以新 run root 執行一次單獨
`sync=none`、`append_batch_return` 測試。這次只作為診斷觀測，不併入 5-round
CV、frontier 或 ceiling 判定。

| 項目 | 結果 |
| --- | --- |
| Run root | `/home/neojhou/state-machine-wal-append-b4096-FYdvfunn` |
| Workload | `wal_write_ceiling`，`batch=4096`，`sync=none`，無 rotation |
| Warmup／測量 | 6 groups／7,462 groups，30,564,352 measured commands |
| Preflight | PASS；CPU idle min `93.00%`、iowait p95/max `0/1%`、sdb util avg `0.15%`、aqu-sz p95/max `0.02/0.06` |
| Wall／service RPS | `772,741`／`808,059` commands/s |
| WAL 寫入量 | `89.91`／`94.02` MiB/s（wall／service） |
| Append group latency | p50／p99／p99.9 `4,187.030`／`24,475.595`／`153,972.958` us；max `162,562` us |
| WAL sync／rotation | sync calls `0`；segment rotations `0`；`replay_verified=true` |
| Process resource | GNU `time` whole-process CPU `68%`、max RSS 約 `1.19 GiB`；hot-loop measured user/system `31.3344/8.209` s |
| Workload elapsed | wall `39.5532` s；service `37.8244` s |

壓測期間的裝置觀測（完整 monitor window；包含前後 idle 視窗）為：write
throughput avg/p95/max `34,780.724/197,776/258,584 KiB/s`、write await
avg/p95/max `344.088/1,703/5,024.1 ms`、aqu-sz avg/p95/max
`3.147/15.68/33.72`、util avg/p95/max `26.274/101.52/135.98%`。目標 affinity
CPU 中，CPU4 與 CPU7 的 workload window 出現高 iowait；這表示本次 append-return
受到 writeback／storage wait 的明顯影響，但 monitor 是 host/device 層級資料，不能
單獨證明唯一根因或裝置理論上限。`sync=none` 且 sync calls 為 0，因此本觀測不代表
durable fsync throughput。

由於只有一輪、沒有 formal duration gate 與 CV 證據，本結果分類為
`standalone-observation`，不能取代未完成的 25-case append 矩陣，也不能宣稱
`valid-component-ceiling`。

### 6.2 單獨 batch=8192 補充觀測（非 formal case）

在相同的 append-return 方法下另測一次 `batch=8192`。正式有效的觀測使用較保守的
no-rotation epoch budget；前兩個 root 不納入結果：第一個 root 的 preflight parser
欄位位置錯誤，第二個 root 雖通過 preflight，但 epoch budget 太接近 segment 上限而
觸發 rotation。

| 項目 | 結果 |
| --- | --- |
| Run root | `/home/neojhou/state-machine-wal-append-b8192-s7zrub` |
| Workload | `wal_write_ceiling`，`batch=8192`，`sync=none`，無 rotation |
| Warmup／測量 | 6 groups／3,731 groups，30,564,352 measured commands |
| Epoch budget | `2,048,000` commands；15 epochs |
| Preflight | PASS；CPU idle min `91.92%`、iowait p95/max `0/0%`、sdb util avg `1.103%`、aqu-sz p95/max `0.09/0.48` |
| Wall／service RPS | `755,832`／`790,973` commands/s |
| WAL 寫入量 | `87.94`／`92.03` MiB/s（wall／service） |
| Append group latency | p50／p99／p99.9 `8,839.547`／`69,031.639`／`147,515.856` us；max `150,495` us |
| WAL sync／rotation | sync calls `0`；segment rotations `0`；`replay_verified=true` |
| Workload elapsed | wall `40.438` s；service `38.6414` s |
| Process resource | GNU `time` whole-process CPU `90%`、max RSS `893,216 KiB`（約 872 MiB）；user/system `50.63/22.76` s |

工作期間裝置觀測（完整 monitor window，包含前後 idle 視窗）為：write throughput
avg/p95/max `46,400.7/244,104/249,024 KiB/s`、write await avg/p95/max
`22.87/99.22/111.72 ms`、aqu-sz avg/p95/max `1.10/5.84/6.04`、util
avg/p95/max `9.58/47.7/50.4%`。目標 affinity CPU 的 workload iowait p95/max 為
`0/0%`。這是 append-return 的非 durable 觀測；`sync_calls=0`，不代表 fsync
throughput，也不提供唯一 storage 根因或硬體理論上限。

本結果只有一輪，分類為 `standalone-observation`；不能併入正式 CV、frontier 或
ceiling 判定。兩個未納入 root 為：

- `/home/neojhou/state-machine-wal-append-b8192-6Tp1Sh`：preflight parser 欄位誤判，未執行 workload；
- `/home/neojhou/state-machine-wal-append-b8192-vYghq7`：preflight 通過，但 no-rotation epoch budget 過大而觸發 rotation。

## 7. WAL durable fsync 結果

正式 25-case durable fsync matrix 未執行，因此沒有完整的 durable Pareto candidate。
但另有 `batch=4096` 與 `batch=8192` 的單輪補充觀測，列於下表與 7.1／7.2；它們
不能代表正式 5-round frontier。

| Batch | Planned rounds | Completed rounds | Wall／service RPS | Sync p50／p99／p99.9／max (us) | Wall／service fsync/s | Status |
| ---: | ---: | ---: | --- | --- | ---: | --- |
| 1 | 5 | 0 | — | — | — | NOT RUN |
| 256 | 5 | 0 | — | — | — | NOT RUN |
| 1024 | 5 | 0 | — | — | — | NOT RUN |
| 4096 | 5 | 1 | 528,148／545,188 | 1,876.969／9,072.994／12,588.446／52,754.945 | 128.94／133.10 | standalone |
| 8192 | 5 | 1 | 583,173／603,148 | 2,847.935／10,723.489／14,051.092／15,550.641 | 71.19／73.63 | standalone |

表中的 `—` 代表沒有執行或沒有可驗證的 durable measurement；append-return 的
`sync_calls=0` 補充觀測不能填入此表，也不能當作 fsync 結果。

### 7.1 單獨 batch=4096 durable fsync 補充觀測（非 formal case）

Run root：`/home/neojhou/wal-fsync-b4096-SZIzgq`。測試使用
`wal-sync=per_group`、`group_size=4096`、1,000 measured groups（4,096,000
commands），completion boundary 為 `group_fsync`。preflight 通過：CPU idle min
`93.00%`、iowait p95/max `0/0%`、sdb util avg `1.116%`、aqu-sz p95/max
`0.05/0.48`。

主要結果如下：

- append p50／p99／p99.9／max：`4,397.235`／`10,457.499`／`238,741.361`／`355,964.264` us；
- sync p50／p99／p99.9／max：`1,876.969`／`9,072.994`／`12,588.446`／`52,754.945` us；
- durable group total p50／p99／p99.9／max：`6,279.284`／`16,966.866`／`240,824.006`／`357,900.835` us；
- wall／service RPS：`528,148`／`545,188` commands/s；wall／service fsync/s：`128.94`／`133.10`；
- WAL throughput：`61.45`／`63.43` MiB/s；`sync_samples=1000`、`measured_wal_sync_calls=1000`；
- `rotation_scope=rotation_inclusive`、`measured_segment_rotations=1`、`replay_verified=true`。

GNU `time` whole-process CPU 為 `83%`、max RSS `1,522,428 KiB`（約 1.45 GiB）；
user/system time 為 `6.67/3.31` s。完整 monitor window 的 sdb write
throughput avg/p95/max 為 `43,481/77,440/77,440 KiB/s`，write await
avg/p95/max 為 `0.523/1.01/1.01 ms`，aqu-sz avg/p95/max 為 `0.27/0.48/0.48`，
util avg/p95/max 為 `19.02/29.38/29.38%`。

這是一輪成功且 counters/replay 通過的 durable 觀測，但沒有 5-round CV、完整 batch
比較或 fio control；因此不能宣稱 durable ceiling、fsync 硬體上限或 production SLO。

### 7.2 單獨 batch=8192 durable fsync 補充觀測（非 formal case）

Run root：`/home/neojhou/wal-fsync-b8192-XHljfW`。測試使用
`wal-sync=per_group`、`group_size=8192`、1,000 measured groups（8,192,000
commands），completion boundary 為 `group_fsync`。preflight 通過：CPU idle min
`94.06%`、iowait p95/max `0/0%`、sdb util avg `1.196%`、aqu-sz p95/max
`0.06/0.48`。

主要結果如下：

- append p50／p99／p99.9／max：`8,154.115`／`21,153.226`／`450,296.516`／`745,787.415` us；
- sync p50／p99／p99.9／max：`2,847.935`／`10,723.489`／`14,051.092`／`15,550.641` us；
- durable group total p50／p99／p99.9／max：`11,041.253`／`24,038.160`／`453,174.497`／`748,872.554` us；
- wall／service RPS：`583,173`／`603,148` commands/s；wall／service fsync/s：`71.19`／`73.63`；
- WAL throughput：`67.85`／`70.18` MiB/s；`sync_samples=1000`、`measured_wal_sync_calls=1000`；
- `rotation_scope=rotation_inclusive`、`measured_segment_rotations=3`、`replay_verified=true`。

GNU `time` whole-process CPU 為 `86%`、max RSS `2,745,728 KiB`（約 2.62 GiB）；
user/system time 為 `13.00/6.38` s。完整 monitor window 的 sdb write throughput
avg/p95/max 為 `44,307/86,424/86,504 KiB/s`，write await avg/p95/max 為
`0.745/1.02/1.07 ms`，aqu-sz avg/p95/max 為 `0.191/0.33/0.48`，util
avg/p95/max 為 `14.52/26.3/29.38%`。

這是一輪成功且 counters/replay 通過的 durable 觀測，但沒有 5-round CV、完整 batch
比較或 fio control；因此不能宣稱 durable ceiling、fsync 硬體上限或 production SLO。

## 8. Storage 與根因證據

兩次停止的共同模式是：

1. CPU idle 大於 97%，iowait p95 為 0%，平均 disk util 小於 0.4%；
2. `disk_aqu_p95=0.01`，只有單一最大 sample 分別為 `0.89` 與 `0.70`；
3. 停止點都在大量 append case 之後的 batch=1024 preflight；
4. 當時沒有其他 benchmark/observer process 殘留，停止後目前 disk 也恢復近乎 idle。
5. 獨立的 batch=4096 觀測在通過 preflight 後仍出現高 await、aqu-sz 與目標 CPU
   iowait，支持 storage/writeback wait 是 append-return tail 的重要放大因素；但因為
   只有單輪且未執行 fsync／fio 對照，仍不足以建立唯一根因或硬體頻寬上限。

這足以確認 runner 觀測到短暫 storage queue outlier，並在單獨 batch=4096 觀測中看到
實際 workload 的 storage wait；但仍不足以區分 filesystem writeback、裝置排程或
其他 host-level I/O 來源；故根因分類為
**`inconclusive-attribution`**。不能把 outlier 直接宣稱為 WAL append 的固定瓶頸，
也不能把 StateMachine 與 append partial 數字外推到完整 Engine。

## 9. 下一步與限制

本報告不提出 production code/configuration 修改。要取得有效 component ceiling，需先
在操作文件中處理單一 `aqu-sz max` outlier（例如要求連續視窗超標才阻塞，並保留 max
作為診斷欄位；CPU、iowait、util 與 aqu-sz p95 gate 仍保留），再以新 run root 重跑
完整矩陣。不能拼接兩次 partial run，也不能以本報告的 partial rows 選 ceiling。

只有 runner 輸出 `collection_status=complete` 與 `result=valid-component-ceiling`，且
formal case、CV、profile bias、counter/replay、rotation、fsync sample 與 identity gates
全部通過時，才可產生正式 RPS／latency frontier 和後續 optimization design。

## 10. Artifact 索引

- 最新 run root：`/home/neojhou/engine-writer-unified-campaign-40jKMTMa`
- 最新停止 summary：
  `preflight/component-append-r2-b1024-summary.txt`
- 最新停止 iostat JSON：
  `preflight/component-append-r2-b1024-iostat.json`
- 首輪失敗 summary：
  `/home/neojhou/engine-writer-unified-campaign-FtaaiOa0/preflight/component-append-r4-b1024-summary.txt`
- 首輪 result：
  `/home/neojhou/engine-writer-unified-campaign-FtaaiOa0/logs/result.txt`
- batch=4096 standalone root：
  `/home/neojhou/state-machine-wal-append-b4096-FYdvfunn`
- batch=8192 standalone root：
  `/home/neojhou/state-machine-wal-append-b8192-s7zrub`
- batch=8192 invalid roots：
  `/home/neojhou/state-machine-wal-append-b8192-6Tp1Sh`、
  `/home/neojhou/state-machine-wal-append-b8192-vYghq7`
- batch=4096 durable fsync root：
  `/home/neojhou/wal-fsync-b4096-SZIzgq`
- batch=8192 durable fsync root：
  `/home/neojhou/wal-fsync-b8192-XHljfW`
