# Engine Writer Hot Path 根因分析重測報告

## 1. 摘要

本報告依據 `docs/engine-writer-hot-path-root-cause-analysis-retest-procedure.md` 執行；本輪正式矩陣使用同一份受測 source state 與同一個 Release benchmark artifact。正式矩陣共 35 輪：writer group=4,096、writer group=256、authoritative Engine control、direct WAL 各完成規定輪數；所有輪次程序正常結束，writer correctness 與 WAL replay 均通過。

報告完成後，benchmark-only collector 移除了未被phase切換使用的progress counters。下列效能數據只對應第2節記錄的source與binary hashes，不視為修改後artifact的正式效能驗證；若要以目前source state作正式決策證據，必須依重測操作文件重新執行calibration與formal matrix，不得與本報告輪次拼接。

Sampling calibration 選定 `N=8`。group=4,096 的 profile-on/off throughput bias 為 **0.586%**（低於 5% 門檻），每輪 103 個 sampled groups，故 phase attribution 可作主要證據。group=256 control 的 bias 為 2.580%；direct WAL profile-on/off 的 bias 為 5.100%，僅作方向性證據。

| Workload | Group | Profile | Sample every | RPS median | Range | Bias | Evidence |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| Writer | 4,096 | off | — | 161,853 | 96,388–164,475 | — | diagnostic control |
| Writer | 4,096 | on | 8 | 160,904 | 158,193–163,567 | 0.586% | attribution passed |
| Writer | 256 | off | — | 72,722.7 | 41,043–75,483.4 | — | control |
| Writer | 256 | on | 8 | 74,598.8 | 72,671.1–77,127.3 | 2.580% | control passed |
| Engine durable | 4,096 | authoritative | — | 167,140 | 164,744–167,504 | — | end-to-end control |
| Direct WAL | 4,096 | off | — | 436,994 | 236,187–446,443 | — | WAL control |
| Direct WAL | 4,096 | on | — | 414,709 | 409,756–423,773 | 5.100% | directional only |

結論如下：

1. Writer 的 instrumentation bias 已通過門檻，原先 34.98% 的偏差不再存在。
2. Writer phase 顯示主要成本以 WAL append（含 prepare/plan/copy/publish/write，五輪 share median 37.415%，單輪 30.095–38.506%）及 `StateMachine::apply`（23.064%）為主；WAL prepare 約 1,076 ns/command，其中 payload encode 與 CRC 合計約 65.3%。
3. Direct WAL 的慢輪次與 sync 長尾同時出現，確認 storage sync tail 是 direct WAL throughput outlier 的直接原因；writer off 輪次沒有同時開 phase clock，因此對完整 Engine writer 只能作方向性歸因。
4. Completion worker median service rate 約 2.619M/s，遠高於 writer arrival 約 161K/s；本次沒有 Completion throughput bottleneck 的證據。
5. 唯一選定的下一個 production optimization 是 **bounded parallel WAL prepare**（先以最多 4 個 worker 做設計／prototype gate，不在本次重測中實作）。以目前比例估算，2 workers 理論端到端 uplift 約 10.1%，4 workers 約 15.9%；這只是 Amdahl 上限估算，仍需下一階段 correctness、ordering、WAL boundary 與 latency 驗證。

## 2. 測試身份與環境

- 測試日期：2026-09-19（Asia/Taipei）。
- Host/kernel：Linux `master`, `6.17.0-35-generic`。
- CPU：AMD Ryzen 7 3700X，16 logical CPUs、8 physical cores、2 threads/core；boost enabled。
- CPU affinity：`taskset -c 2-7`。
- Governor：`powersave`（未修改；所有 calibration、formal 與 observation 使用相同設定）。
- Filesystem：`/dev/sdb2`, ext4, `rw,relatime`，WAL 未使用 tmpfs/overlay。
- Formal run 開始時可用空間約 572 GiB；結果目錄完成後仍保留。
- Compiler：GCC 13.3.0。
- C++ flags：`-std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread`。
- Sanitizer flags：`-std=c++20 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer`。
- WAL：`per_group` fsync、group size 4,096；每輪新建空 data directory。
- source identity：HEAD `b63c27e189835baa67b612385f16a2b019141b53`。
- staged diff SHA-256：`71eae2ecb096c3a8c210d9f9d2d04e363f67916797e752c2ba740ffacc8b6b3b`。
- worktree diff SHA-256：`b19a90e0cba000f9bb8470f1aa180e7b2d7a7ef31aca401d30199636002496be`。
- 受測 Release benchmark：`/tmp/order-books-writer-sampling-benchmark-final`。
- benchmark SHA-256：`95771cc39ec82f635e31575d5a20aac2c71cea92baf0ae7dfacfddeeee4a893c`。

CMake/Ninja 在此環境不可用（`cmake`、`ninja` 不在 PATH），因此依相同 source、include、compiler flags 以直接 GCC 命令完成等價建置與測試；此限制不代表修改 CMake 或 staging。

## 3. 測試與建置結果

- Release benchmark：編譯成功，`-Werror` 無警告。
- Release GoogleTest：2/2 通過。
- Debug（`-O0 -g`）GoogleTest：2/2 通過。
- ASan/UBSan（含 leak detection）：2/2 通過，無 sanitizer 錯誤。
- Sampling smoke、`N=1`/大於總 group 數邊界與 invalid CLI 測試通過；invalid sampling option 在 profile 未開啟時正確拒絕。
- 正式矩陣：35/35 有效；writer 每輪 `correctness_verified=true`，WAL 每輪 `replay_verified=true`；sampled writer commands 與 sampled Completion commands 完全相等。
- group=4,096 profile-on 每輪 `profiled_groups=103`（規定至少 50）。

原始 logs 與 WAL data 位於：

`/home/neojhou/engine-writer-retest-final-XQMbfqQp`

該目錄約 22 GiB，包含 environment metadata、兩組 calibration（最終採用第二組）、35 輪 formal logs、外部 observation logs 與每輪 data directory；測試後未自動刪除。

## 4. Sampling 校準

最後採用的 calibration 使用同一 artifact、group=4,096、group delay=1,000 us、producer lanes=8,192、warmup=10,000、measured 2,000,000 commands，採 `off-r1、on-r1、on-r2、off-r2、off-r3、on-r3` ABBA 順序；每輪 elapsed 約 12.1–12.5 秒，符合至少 10 秒的 calibration 條件。較早的 1,680,000-command calibration logs 保留於 raw directory，但不與此組結果混算。N=8 的每輪 sampled groups 為 62，故樣本充足。

| Sample every | Off median RPS | On median RPS | Bias | Sampled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 164,938 | 164,500 | 0.265% | 62 | 選用 |

依程序規則，N=8 已通過，不再測 N=16/32；選擇只依 throughput bias 與 sample sufficiency，沒有依 phase 比例挑選。

## 5. 正式 throughput 與 latency

每個正式 case 五輪；表中 latency 是五輪 per-run 值的 median，range 是五輪 min–max。`p99.9` 與 `max` 特別保留慢輪次，不因吞吐較低而剔除。

| Workload | Group | Profile | RPS median | RPS range | Elapsed median (ms) | p50 median (us) | p99 median (us) | Worst p99.9 (us) | Worst max (us) |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Writer | 4,096 | off | 161,853 | 96,388–164,475 | 20,759.6 | 46,630.4 | 110,451 | 427,806 | 430,565 |
| Writer | 4,096 | on | 160,904 | 158,193–163,567 | 20,882.1 | 47,881.9 | 103,683 | 297,423 | 298,681 |
| Writer | 256 | off | 72,722.7 | 41,043–75,483.4 | 22,001.4 | 108,471 | 200,088 | 10,917,300 | 11,796,600 |
| Writer | 256 | on | 74,598.8 | 72,671.1–77,127.3 | 21,448.1 | 106,825 | 194,014 | 431,719 | 432,423 |
| Engine durable | 4,096 | authoritative | 167,140 | 164,744–167,504 | 20,102.9 | 46,142.4 | 98,002 | 295,983 | 297,549 |
| Direct WAL | 4,096 | off | 436,994 | 236,187–446,443 | 19,711.7 | 7,841.1 group | 16,771.5 group | 541,575.1 group | 944,673.6 group |
| Direct WAL | 4,096 | on | 414,709 | 409,756–423,773 | 20,770.9 | 8,395.6 group | 17,394.2 group | 385,182.1 group | 823,877.1 group |

Authoritative Engine throughput 約為 1M/s 目標的 16.714%；writer profile-off 為 16.185%；direct WAL profile-off 為 43.699%。Writer profile-off 與 authoritative Engine 的 median 差異為 3.163%，可視為相互交叉確認而非替代關係。

## 6. Writer phase（group=4,096，profile-on，N=8）

以下所有 `ns/command` 先在單輪以 sampled commands 為 denominator，再取五輪 median；share 是同一 parent denominator 的 per-run share 再取 median。`writer service` 不包含 Completion worker wall time。

| Phase | ns/command median | min–max | Writer-service share median | 證據 |
| --- | ---: | ---: | ---: | --- |
| admission | 436.8 | 432.7–452.1 | 7.467% | 穩定 |
| WAL append（parent） | 2,239.9 | 1,616.5–2,288.9 | 37.415% | 主要 CPU/WAL 成本 |
| WAL sync | 832.1 | 741.8–889.0 | 14.228% | 同輪 phase-on 較穩定 |
| `StateMachine::apply` | 1,353.8 | 1,328.7–1,388.7 | 23.064% | 穩定且超過 10% |
| publisher notify | 0.590 | 0.565–0.661 | 0.011% | 非 writer 瓶頸 |
| post-apply | 551.8 | 537.4–553.8 | 9.221% | 穩定 |
| completion enqueue | 405.7 | 392.7–419.0 | 6.921% | enqueue 本身非瓶頸 |
| writer unattributed | 以 raw total 計算約 150 ns/command | — | 2.566% | 量測剩餘 |

`writer_service_ns/command` median 為 5,870.0 ns（5,371.5–6,054.6）；`writer_cycle_ns/command` median 為 6,558.1 ns（6,162.1–6,867.4）。

逐輪的 RPS 與 `wal_sync_ns/command`（僅 profile-on，因 profile-off 按設計不取得 phase clocks）如下：

| Run | RPS | `wal_sync_ns/command` |
| --- | ---: | ---: |
| on-r1 | 160,904 | 889.0 |
| on-r2 | 162,185 | 826.9 |
| on-r3 | 158,193 | 861.5 |
| on-r4 | 158,553 | 832.1 |
| on-r5 | 163,567 | 741.8 |

這組 profile-on 沒有與 profile-off 慢輪次同量級的 RPS drop；因此不能只靠這五列宣稱整個 writer 的慢輪次必然由 sync 造成。

## 7. WAL prepare／copy 子階段

Writer sampled phase 的 nested phase 以 WAL prepare 或 WAL append parent 為 denominator；以下同樣採 per-run 後取 median。

| Phase | ns/command median | min–max | Parent share median | 判斷 |
| --- | ---: | ---: | ---: | --- |
| payload encode | 452.9 | 447.5–458.7 | prepare 的 41.834% | 穩定主成本 |
| CRC | 250.4 | 249.4–253.3 | prepare 的 23.180% | 穩定主成本 |
| frame assembly | 198.2 | 196.8–202.2 | prepare 的 18.404% | 穩定 |
| prepare remainder | 180.6 | 173.7–182.6 | prepare 的 16.610% | 穩定 |
| **WAL prepare 合計** | **1,076.5** | **1,069.8–1,096.3** | writer service 的 **18.339%** | 可評估 bounded parallel |
| chunk copy | 28.7 | 25.3–30.0 | append 的一部分 | 非主要成本 |
| plan/copy remainder | 639.5 | 32.5–670.6 | append 的一部分 | run-to-run 兩種量級，需保留觀察 |
| WAL plan/copy 合計 | 668.2 | 57.8–699.7 | append 的 29.832% | 不是下一個唯一選案 |
| publish | 405.3 | 400.7–412.2 | append 的 18.403% | 穩定 |
| write | 40.6 | 39.4–44.3 | append 的 1.869% | 非主要成本 |

獨立 direct WAL profile-on 的五輪結果提供較穩定的 group-level cross-check：prepare share median 41.363%（41.140–42.215%）、plan/copy 12.500%（12.265–12.688%）、publish 14.879%（14.696–14.973%）、write 1.598%（1.571–1.627%）、sync 28.984%（27.707–29.497%）。這些比例不能直接與 writer nested ns/command 相加，但方向一致地顯示 prepare、sync、publish 是主要 WAL 成本。

## 8. Completion worker

Completion 指標只描述 sampled completion items 的 worker service 與 queue residence，不加進 writer service。

| Group | Service RPS median | Queue p50 median | Queue p99 median | Worst p99.9 | Max depth median | 是否持續累積 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,096 | 2.619M/s（2.568–2.914M/s） | 2.687 ms | 5.816 ms | 9.886 ms | 4,085（4,067–4,096） | 未證實 |
| 256 | 本次未將 control 的 per-worker service 作唯一決策依據 | — | — | — | — | 未證實 |

group=4,096 writer arrival 約 161K/s，Completion service median 約為其 16 倍；本次沒有 service rate 低於 arrival rate 的情況。`max_depth` 接近一個 group size 只能表示 burst/backlog 曾出現，沒有時間序列或不同 run length 證明 queue 持續成長，因此不選 Completion batching。

Publisher worker replay、EventSink ACK 與 cursor persistence 不在本次 writer 重測範圍；`publisher_notify` 只代表 writer 呼叫 `notify_publishable()` 的同步成本。

## 9. Sync／storage 長尾分析

Direct WAL profile-off 的兩個慢輪次（236,187、247,276 commands/s）對應 `sync_p99_us` 約 160 ms、`sync_max_us` 約 854–939 ms；正常輪次的 `sync_p99_us` 約 9.7–10.2 ms。profile-on 五輪的 `sync_p50_us` 約 2.505–2.619 ms，`sync_p99_us` 約 9.371–10.594 ms，慢輪次未出現同等長尾。

因此：

- **已證實**：direct WAL throughput outlier 的直接同步變異與 `Wal::sync()` 長尾一致；其 p50 sync 並未顯著變化，差異在 tail。
- **方向性**：Engine writer profile-off 也有低吞吐輪次（96,388/s），但因 off 路徑依設計不收 phase clocks，沒有同一輪的 `wal_sync_ns/command` 可作直接配對；不能把 direct WAL 的因果證據無條件外推到整個 Engine writer。
- **尚未證實**：是否應先改 WAL sync/group commit，或是否能在指定 SLO 下以更大 group 降低固定成本；需要獨立的 sync distribution 與 group-delay sweep。

外部觀測（不納入正式統計）補充：

- `/usr/bin/time -v` profile-off：user 24.37 s、system 6.33 s、wall 25.62 s、CPU 119%、RSS 1,549,964 KiB、voluntary context switches 265,752、involuntary 12,591，exit 0。
- `pidstat -t` 代表 run：process aggregate 平均約 104.86% CPU；最高 worker thread 平均約 73.62%，其餘約 12.06%、12.99%、6.00%。
- `iostat -xz` 觀測期間 device `%util` 約 44.4–70.3%，未顯示穩定 100% 飽和；此結果受 powersave、ext4 與背景 storage scheduling 影響，只作輔助資料。
- `perf stat` 被 `perf_event_paranoid=4` 拒絕；未修改 sysctl/kernel security setting。

## 10. Correctness 與有效性清單

- 每個 writer formal log 均包含 `correctness_verified=true`。
- 每個 direct WAL formal log 均包含 `replay_verified=true`。
- Engine authoritative 五輪均正常 exit，`commands=3,360,000`、`trades=1,680,000`，`active_orders=0`、`active_levels=0`；未將缺少 `correctness_verified` 欄位誤判為失敗。
- 每個 profile-on log 的 `profiled_commands == completion_profiled_commands`。
- group=4,096 profile-on sampled groups：103/輪；group=256 profile-on 也通過 count equality。
- 沒有因 throughput 低而排除任何有效輪次；g=4,096 off-r1、off-r4、g=256 off-r1、off-r4、direct WAL off-r1、off-r4 的慢輪次均保留在統計中。
- 沒有 publisher failure、storage pressure、phase timeout 或 callback error。

逐輪有效性（35/35 均納入統計）：

| Run | Exit | Correctness/replay | Count/profile check | 納入 |
| --- | ---: | --- | --- | --- |
| g4096-off-r1 | 0 | correctness=true | — | yes |
| g4096-off-r2 | 0 | correctness=true | — | yes |
| g4096-off-r3 | 0 | correctness=true | — | yes |
| g4096-off-r4 | 0 | correctness=true | — | yes |
| g4096-off-r5 | 0 | correctness=true | — | yes |
| g4096-on-r1 | 0 | correctness=true | sampled writer=Completion | yes |
| g4096-on-r2 | 0 | correctness=true | sampled writer=Completion | yes |
| g4096-on-r3 | 0 | correctness=true | sampled writer=Completion | yes |
| g4096-on-r4 | 0 | correctness=true | sampled writer=Completion | yes |
| g4096-on-r5 | 0 | correctness=true | sampled writer=Completion | yes |
| g256-off-r1 | 0 | correctness=true | — | yes |
| g256-off-r2 | 0 | correctness=true | — | yes |
| g256-off-r3 | 0 | correctness=true | — | yes |
| g256-off-r4 | 0 | correctness=true | — | yes |
| g256-off-r5 | 0 | correctness=true | — | yes |
| g256-on-r1 | 0 | correctness=true | sampled writer=Completion | yes |
| g256-on-r2 | 0 | correctness=true | sampled writer=Completion | yes |
| g256-on-r3 | 0 | correctness=true | sampled writer=Completion | yes |
| g256-on-r4 | 0 | correctness=true | sampled writer=Completion | yes |
| g256-on-r5 | 0 | correctness=true | sampled writer=Completion | yes |
| engine-control-r1 | 0 | command/trade/active-state expected | — | yes |
| engine-control-r2 | 0 | command/trade/active-state expected | — | yes |
| engine-control-r3 | 0 | command/trade/active-state expected | — | yes |
| engine-control-r4 | 0 | command/trade/active-state expected | — | yes |
| engine-control-r5 | 0 | command/trade/active-state expected | — | yes |
| wal-off-r1 | 0 | replay=true | — | yes |
| wal-off-r2 | 0 | replay=true | — | yes |
| wal-off-r3 | 0 | replay=true | — | yes |
| wal-off-r4 | 0 | replay=true | — | yes |
| wal-off-r5 | 0 | replay=true | — | yes |
| wal-on-r1 | 0 | replay=true | profile groups=2,103 | yes |
| wal-on-r2 | 0 | replay=true | profile groups=2,103 | yes |
| wal-on-r3 | 0 | replay=true | profile groups=2,103 | yes |
| wal-on-r4 | 0 | replay=true | profile groups=2,103 | yes |
| wal-on-r5 | 0 | replay=true | profile groups=2,103 | yes |

## 11. 結論與唯一下一步

### 已證實

1. Deterministic sampling 已消除主要 instrumentation bias：g=4,096 bias 0.586%；fresh N=8 calibration bias 為 0.265%。
2. Writer 的穩定主要成本是 WAL append parent 與 `StateMachine::apply`；publisher notify 和 completion enqueue 本身不是同步瓶頸。
3. WAL prepare 內 payload encode、CRC 是穩定且最大的 nested prepare 子成本；二者合計約 65.3% of prepare。
4. Direct WAL 慢輪次由 sync tail 變異主導，而非 p50 sync 改變。
5. Completion worker service rate 明顯高於 writer arrival；現有資料不支持 Completion batching 為下一案。

### 方向性證據

- Engine writer off 慢輪次與 direct WAL sync tail 在同一 filesystem 上呈現相似長尾現象，但 off writer 沒有 phase clocks，尚不足以完成同輪因果配對。
- `iostat` 未顯示持續 100% device utilization；不能以單次 `%util` 排除 fsync latency tail，也不能推論換硬體後的保證。
- `StateMachine::apply` 約占 writer service 23.064%，值得未來獨立細分，但本次沒有證據顯示它先於 WAL 成為 ceiling。

### 尚未證實

- 更高 group size／group-commit interval 在目標 latency SLO 下的最佳點。
- sync tail 是否來自裝置 queue、kernel writeback、fsync scheduling 或其他外部 I/O。
- bounded parallel prepare 的實際 speedup、CPU contention、ordering 與 failure semantics。

### 唯一選定的 production optimization

選 **bounded parallel WAL prepare** 作為下一案，限制如下：

- 只平行化 prepare 內 payload encode、CRC、frame assembly 等可驗證為 CPU-bound 的工作；不平行化 shared WAL publish、write、fsync 或 StateMachine ownership。
- 先以固定上限 worker（候選 W=2，再評估 W=4）建立設計與 prototype；不得無界建立 thread/task。
- 目前 `P = 1,076.5 / 5,870.0 = 18.339%`；Amdahl 估算 W=2 uplift 約 10.1%，W=4 uplift 約 15.9%，通過至少 10% 的方向性 gate。
- prototype 必須重新驗證 command ordering、EngineSeq、CRC/WAL bytes、fsync boundary、replay、Completion callback，以及 p50/p99/p99.9；若 CPU contention 或 latency SLO 不通過，應回退。

本次沒有修改 durability policy、ordering、WAL format、StateMachine ownership、Publisher worker、public API 或 production defaults。Sync/group-commit sweep 是後續獨立診斷，不與本次唯一 production optimization 同時展開。

## 12. 限制與 artifact

- 結果只代表 AMD Ryzen 7 3700X、powersave governor、ext4 `/dev/sdb2` 與本次 kernel；不能外推到其他 SSD、RAID、cloud block device 或 performance governor。
- CMake/Ninja 不可用，使用直接 GCC 等價命令；完整 CMake/CTest pipeline 仍需在具備工具鏈的環境重跑。
- `perf` 受 `perf_event_paranoid=4` 限制；未降低安全設定。
- profile-off 路徑刻意不取得 phase clocks，因此 writer off 慢輪次只能與 direct WAL sync evidence 作方向性比較。
- Completion queue 只有單一 run length 的 residence/depth 摘要，沒有時間序列；不能宣稱 backlog 會持續成長。
- 報告未把 sampling diagnostic option 變成 production default；sampling 只影響 opt-in benchmark diagnostics。
- 完整 raw logs/data：`/home/neojhou/engine-writer-retest-final-XQMbfqQp`。
