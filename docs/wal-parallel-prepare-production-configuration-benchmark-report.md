# WAL parallel prepare production configuration 壓測結果

日期：2026-09-19。測試依
`docs/wal-parallel-prepare-production-configuration-benchmark-procedure.md` 執行；沒有執行任何
會改變 Git staging 的操作。

## 1. 摘要與最終判定

| Gate | 結果 | 判定 |
| --- | --- | --- |
| ReleaseBenchmark | 109/109 passed | pass |
| Debug | 87/87 passed | pass |
| ASan/UBSan | 87/87 passed；無 sanitizer error | pass |
| W2 median uplift | 157,880 -> 169,982 commands/s，+7.67% | **fail；要求 +10%** |
| W2 paired p99 regression | 2/5 輪超過 10% | pass；失敗門檻為 >=3/5 |
| W2 parallel path | 每輪 919 groups / 1,839 tasks | pass |
| W2 fallback path | 每輪 0 groups | pass |
| Replay／durable head | 20/20 輪由 benchmark validation 通過 | pass |
| Queue／publisher lag canary | not run | incomplete |
| Sync p99/p99.9/max time-series | not available；CLI 僅有 full-run p99 | incomplete |
| CPU／context-switch budget | 未定義 deployment budget | not evaluated |
| Rollback restart | not run | incomplete |

最終結論：**reject W=2 rollout，維持 W=1 default**。

W=2 確實觸發 bounded parallel prepare，並保留 WAL bytes、replay、durable head 與 completion
correctness；但五輪 median throughput uplift 只有 7.67%，未達設計的 +10% production acceptance
gate。W=4 僅作 diagnostic，不能取代 W=2 gate。由於 controlled gate 已失敗，依程序沒有執行
deployment canary，也不能宣稱 production acceptance 或達到 1M commands/s。

## 2. Artifact 與環境

| 項目 | 值 |
| --- | --- |
| RUN_ROOT | `/home/neojhou/wal-prepare-production-qq3Pf4tM` |
| Host／kernel | `master`, Linux `6.17.0-35-generic` |
| CPU | AMD Ryzen 7 3700X；8 cores / 16 logical CPUs |
| CPU affinity | `taskset -c 2-7` |
| Governor | `powersave` |
| Filesystem | `/dev/sdb2`, ext4, `rw,relatime` |
| Available space at start | 約 477 GiB |
| Compiler／build | GCC 13.3.0，C++20，ReleaseBenchmark |
| HEAD | `5bc6f85073b532267e77d57d8649900711b720c9` |
| Index diff SHA-256 | `e34c8091a64a1e872edf771d0d28f15eb3fe87be2a3b3ac6094bf626b2015966` |
| Worktree tracked diff SHA-256 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| Procedure SHA-256 | `f9e0180319df684d4f7a2c174e9d67b9f27c4b846082273dcbc0c97187ff5f33` |
| Benchmark binary SHA-256 | `8e1d8cbecac75671004040864f0a3a371a4ddb21a09336cda7e68d7c1e6a9e49` |
| Tool limitation | 使用 `/tmp/order_books-tools/bin/` 的 cmake/ninja/ctest wrapper |

正式矩陣前與完成後的 source/binary identity 相同。測試 artifact 約 9.0 GiB；所有 case 使用
獨立 data directory，慢輪與 storage tail 均保留。

## 3. Build、correctness 與 smoke

### 3.1 Build/test gate

- ReleaseBenchmark：`100% tests passed out of 109`。
- Debug：`100% tests passed out of 87`。
- ASan/UBSan：`100% tests passed out of 87`，未出現 sanitizer error。
- TSan：本次未執行獨立 TSan build；因此不宣稱 TSan 通過。

### 3.2 Smoke

固定參數為 group=4,096、delay=1,000 us、producer lanes=8,192、warmup=1,000、iterations=10,000。

| Case | RPS | p99 us | Parallel groups | Prepare tasks | Sync full-run p99 us | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| W1 | 156,429 | 52,638.4 | 0 | 5 | 5,000 | 0 |
| W2 candidate | 168,661 | 48,890.6 | 4 | 9 | 5,000 | 0 |
| W2 fallback | 160,587 | 54,026.7 | 0 | 6 | 5,000 | 0 |
| W4 | 137,317 | 77,232.5 | 4 | 17 | 50,000 | 0 |

所有 smoke summary 都完成 durable callback、stop、WAL reopen/replay 與 counter validation。

## 4. Pilot 與正式參數

Pilot 使用每 case 400,000 iterations、warmup=10,000；正式 iterations 依目標 20 秒的公式取
四個 case 的最大值。

| Case | Pilot elapsed ms | Pilot RPS | Calculated iterations |
| --- | ---: | ---: | ---: |
| W1 | 4,837.59 | 165,372 | 1,653,716 |
| W2 candidate | 4,625.37 | 172,959 | 1,729,591 |
| W2 fallback | 5,318.96 | 150,405 | 1,504,053 |
| W4 | 4,247.30 | 188,355 | 1,883,550 |

共同正式設定為 `FORMAL_ITERATIONS=1,883,550`、warmup=10,000；每輪 measured commands 為
3,767,100。所有正式輪 elapsed 均 >=15 秒。

固定參數：WAL `per_group` sync、group size=4,096、group delay=1,000 us、producer lanes=8,192、
candidate threshold=4,096、fallback threshold=8,192、單一 instrument／shard。

## 5. Controlled benchmark aggregate

Latency 單位為 us；RPS 與 latency 是五輪 median，p99.9/max 是五輪 worst；sync 欄位是
`wal_sync_full_run_p99_us`，包含 warmup。

| Case | RPS median | RPS min--max | Uplift vs W1 | p50 median | p99 median | Worst p99.9 | Worst max | Sync full-run p99 range |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| W1 | 157,880 | 100,426--159,920 | baseline | 47,949.8 | 120,513 | 431,713 | 433,865 | 25,000--250,000 |
| W2 candidate | 169,982 | 96,124.3--176,029 | **+7.67%** | 44,433.6 | 109,542 | 921,199 | 923,194 | 25,000--250,000 |
| W2 fallback | 159,031 | 158,035--159,707 | +0.73% | 47,860.6 | 106,210 | 388,841 | 390,201 | 25,000 |
| W4 diagnostic | 179,456 | 108,338--181,968 | +13.67% | 41,559.3 | 112,884 | 543,060 | 545,102 | 25,000--250,000 |

### 5.1 W1/W2 paired p99

| Round | W1 p99 us | W2 p99 us | W2 > W1 × 1.10 |
| ---: | ---: | ---: | --- |
| r1 | 115,766 | 109,542 | no |
| r2 | 120,513 | 405,901 | yes |
| r3 | 114,504 | 435,697 | yes |
| r4 | 344,488 | 101,192 | no |
| r5 | 158,226 | 101,023 | no |

W2 有 2/5 輪超過 10% p99 regression，低於失敗門檻 3/5；但 r2/r3 同時伴隨明顯 storage
tail，不能將 p99 改善解讀成穩定 latency improvement。

## 6. 路徑與資源成本

`/usr/bin/time -v` 數值涵蓋整個 benchmark process（包含 stop/replay），CPU 可超過 100% 是因
parallel workers 與 writer thread 同時執行。

| Case | Parallel groups | Prepare tasks | Commands/group | CPU median | Vol CS median | Invol CS median | Max RSS median KiB |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| W1 | 0 | 920--921 | 4,090.23--4,094.67 | 120% | 301,983 | 6,947 | 1,591,700 |
| W2 candidate | 919 | 1,839 | 4,094.67 | 128% | 280,261 | 7,221 | 1,662,560 |
| W2 fallback | 0 | 920--921 | 4,090.23--4,094.67 | 120% | 326,454 | 8,348 | 1,604,464 |
| W4 diagnostic | 919 | 3,677--3,678 | 4,090.23--4,094.67 | 134% | 284,831 | 6,613 | 1,594,368 |

W2 candidate 相對 W1 的 process CPU median 增加約 8 percentage points，RSS median 增加約 70,860
KiB；但本次沒有預先定義 deployment CPU budget，因此只記錄，不判定資源 gate 通過。

每一正式輪的 measured WAL invariants 一致：`wal_bytes_delta=459,586,222`、
`wal_group_commands=3,767,100`、`segment_count=2`、`measured_segment_rotations=1`，且所有成功
summary 都是 `fsync_mode=per_group completion_boundary=durable_callback`。

## 7. 正式輪次明細

| Case | Round | RPS | p99 us | p99.9 us | Max us | Elapsed ms | Sync full-run p99 us | Groups/tasks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| W1 | r1 | 157,880 | 115,766 | 384,821 | 387,392 | 23,860.6 | 25,000 | 0/921 |
| W2 | r1 | 173,615 | 109,542 | 352,822 | 354,681 | 21,698.0 | 25,000 | 919/1,839 |
| fallback | r1 | 158,139 | 117,685 | 388,841 | 390,185 | 23,821.4 | 25,000 | 0/920 |
| W4 | r1 | 181,968 | 119,606 | 386,235 | 387,602 | 20,702.0 | 25,000 | 919/3,677 |
| W2 | r2 | 96,124 | 405,901 | 921,199 | 923,194 | 39,189.9 | 250,000 | 919/1,839 |
| W4 | r2 | 180,687 | 121,198 | 405,918 | 406,706 | 20,848.8 | 25,000 | 919/3,677 |
| W1 | r2 | 156,909 | 120,513 | 407,292 | 409,375 | 24,008.2 | 25,000 | 0/921 |
| fallback | r2 | 158,035 | 102,315 | 368,628 | 369,681 | 23,837.2 | 25,000 | 0/920 |
| fallback | r3 | 159,707 | 106,210 | 336,639 | 337,947 | 23,587.5 | 25,000 | 0/921 |
| W1 | r3 | 159,920 | 114,504 | 411,611 | 412,524 | 23,556.1 | 25,000 | 0/921 |
| W4 | r3 | 179,425 | 110,811 | 393,643 | 396,596 | 20,995.3 | 25,000 | 919/3,677 |
| W2 | r3 | 104,622 | 435,697 | 660,546 | 663,887 | 36,006.8 | 250,000 | 919/1,839 |
| W4 | r4 | 179,456 | 112,884 | 422,551 | 424,863 | 20,991.7 | 25,000 | 919/3,678 |
| fallback | r4 | 159,162 | 101,005 | 316,882 | 317,986 | 23,668.3 | 25,000 | 0/920 |
| W2 | r4 | 176,029 | 101,192 | 358,555 | 362,656 | 21,400.4 | 25,000 | 919/1,839 |
| W1 | r4 | 100,426 | 344,488 | 431,713 | 433,865 | 37,511.3 | 250,000 | 0/921 |
| W1 | r5 | 159,206 | 158,226 | 394,944 | 395,671 | 23,661.7 | 25,000 | 0/920 |
| W4 | r5 | 108,338 | 396,667 | 543,060 | 545,102 | 34,771.6 | 250,000 | 919/3,677 |
| W2 | r5 | 169,982 | 101,023 | 391,393 | 392,224 | 22,161.8 | 25,000 | 919/1,839 |
| fallback | r5 | 159,031 | 115,935 | 386,304 | 390,201 | 23,687.9 | 25,000 | 0/920 |

## 8. Canary、rollback 與限制

Deployment canary 未執行，原因是 controlled W=2 median uplift 已低於 +10% gate，且目前 repository
benchmark 使用 `NullMetricsSink`，沒有可直接收集 queue／publisher lag time-series 的 exporter。
因此沒有填造 fixed-rate lag、sync p99.9/max 或 deployment CPU budget 結果。

本次結果可確認：

- public Engine path 的 W=2 設定確實生效，parallel groups/tasks 及 fallback counter 符合預期；
- WAL bytes、group commands、segment rotation、completion boundary 與 replay/durable validation
  在所有輪次一致；
- parallel prepare 在此 host/storage/workload 有 throughput 方向性收益，但 median 未達 production
  gate，且 storage tail 造成 W2 r2/r3 與 W4 r5 的顯著變異；
- `wal_sync_full_run_p99_us` 是包含 warmup 的 bounded histogram，不能當作 measured-window
  sync p99/p99.9/max。

本次未執行 rollback restart，因 W=2 未通過 rollout gate；W=1 仍是 library/deployment default。

## 9. 結論與下一步

本次 production-configuration benchmark **不通過 W=2 throughput acceptance**：+7.67% < +10%。
建議維持 W=1，保留本次 raw artifacts，另行調查 storage tail、CPU budget 與 group/threshold 在
實際 arrival pattern 下的收益；在沒有新的 controlled benchmark 通過 gate 前，不啟動 W=2 canary。

若後續重新評估，必須使用相同矩陣重新取得五輪 median，並補足 production MetricsSink/exporter
的 queue、publisher lag 及 sync p99/p99.9/max time-series；不得只重跑最快輪或以 W=4 結果替代 W=2。
