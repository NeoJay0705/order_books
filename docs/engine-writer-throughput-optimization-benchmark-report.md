# Engine Writer Throughput Optimization 壓測報告

## 1. 報告範圍

- 測試日期：2026-09-18
- 設計依據：[engine-writer-throughput-optimization-design.md](engine-writer-throughput-optimization-design.md)
- 目的：驗證 incremental invariant validation、metrics ownership split，以及 durable
  group-commit 的 throughput／latency frontier。
- 範圍：單一 shard、單一 instrument、單一 durable writer；Publisher 與 Completion 不以
  instantaneous 1M commands/s 作為本輪 writer gate。
- Production `RuntimeConfig` 的 `group_commit_max_commands=256` 與
  `group_commit_max_delay=200 us` 未修改；本報告的 group、delay、producer-lane 只透過
  benchmark CLI 覆寫。
- 壓測期間沒有執行 `git add`、`git reset`、`git restore` 或 `git commit`。壓測前 staged
  diff binary hash 為 `cd0a5bc0e520f12643da74f7fe086e51e14e68a56a7b3ddc3ce1b88c160edd7d`。

本報告只反映目前主機、filesystem、compiler、Release flags 與 source 狀態，不能直接視為
正式部署裝置的 SLA。

## 2. 測試環境

| 項目 | 數值 |
| --- | --- |
| OS | Linux 6.17.0-35-generic x86-64 |
| Compiler | GCC 13.3.0 |
| Language | C++20 |
| Build | CMake Release，`-O3 -DNDEBUG` |
| CPU | AMD Ryzen 7 3700X 8-Core Processor |
| CPU threads | 16 |
| WAL filesystem | ext4，`/dev/sdb2` |
| Benchmark executable | `/tmp/order-books-writer-review-build/benchmarks/order_books_benchmark` |
| CTest build directory | `/tmp/order-books-writer-review-build` |

StateMachine 與 metrics workload 使用固定 CPU affinity；durable workload 的每次執行使用獨立
temporary data directory。WAL 為現有格式，completion boundary 為 durable callback，sync mode
為每個 accepted group 一次 `fsync`。

## 3. 參考 SLO 與統計語意

本階段需求尚未提供正式產品 SLO，因此沿用設計文件的暫定 balanced profile：

| 指標 | 暫定門檻 |
| --- | ---: |
| group idle-fill max delay | 1 ms |
| durable completion p99 | <= 20 ms |
| durable completion p99.9 | <= 50 ms |

這些門檻只用於本次 Pareto 判讀，不是通用交易產業標準，也不是 shared CI 的固定效能
gate。group-size sustained-backlog scan 會刻意使 writer 飽和，不能把該 scan 的 tail latency
當成低 concurrency latency。

durable benchmark 的欄位定義如下：

- `commands_per_second`：measured phase 的 accepted durable commands/s。
- `actual_commands_per_group`：`wal_group_commands / wal_group_commits`，不是 configured
  group 上限。
- `wal_mib_per_second`：measured phase 的 WAL bytes delta／elapsed time。
- `p50_us`、`p99_us`、`p99.9_us`、`max_us`：durable completion latency。
- 每列的 throughput 為五輪 median；`range` 為五輪最小至最大值。

## 4. 測試方法

### 4.1 Build、回歸與 correctness

先使用目前 source 建立 Release benchmark，並執行：

```bash
/home/neojhou/.conan2/p/cmake24863797e49b7/p/bin/ctest \
  --test-dir /tmp/order-books-writer-review-build --output-on-failure
```

結果為 **73/73 tests passed**。

### 4.2 StateMachine state-size matrix

以下命令中的 `BENCH` 指向
`/tmp/order-books-writer-review-build/benchmarks/order_books_benchmark`：

代表性命令：

```bash
taskset -c 2 "$BENCH" \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=state_machine \
  --pipeline-batch-size=256 \
  --pipeline-active-orders=N \
  --iterations=45000 --warmup=4000
```

`N` 使用 `0／1000／10000／100000`，每個 case 五輪。每輪 measured commands 為
11,520,000，並檢查 `correctness_verified=true`。

### 4.3 Metrics ownership matrix

```bash
taskset -c 2,3 "$BENCH" \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=metrics \
  --pipeline-batch-size=256 \
  --iterations=80000 --warmup=8000
```

每輪同時輸出 writer-only、separate registries 與 shared registry 三個 case，共五輪；每個
case 的 writer 與 Publisher command count、metric calls、observations 皆由 benchmark 驗證。

### 4.4 Durable group-size scan

代表性命令：

```bash
"$BENCH" --workload=engine_durable_single_instrument \
  --engine-group-size=G --engine-group-delay-us=1000 \
  --engine-producer-lanes=8192 \
  --iterations=ITERATIONS --warmup=WARMUP
```

`G` 掃描 `256／512／1024／2048／4096／8192`。每個 case 五輪，measured phase 以不同
iteration count 校準至約 15 秒：

| group | iterations | warmup |
| ---: | ---: | ---: |
| 256 | 600,000 | 30,000 |
| 512 | 900,000 | 45,000 |
| 1,024 | 1,100,000 | 55,000 |
| 2,048 | 1,200,000 | 60,000 |
| 4,096 | 1,250,000 | 62,500 |
| 8,192 | 1,300,000 | 65,000 |

### 4.5 Producer-lane sweep

對候選 group `4096／8192`，固定 delay=1 ms，測試 lanes
`1／8／64／256／1024／candidate group size`，每個 case 五輪且 measured phase >=10 秒。
這個 sweep 用來觀察 underfilled 到 saturated group，不是新增 rate limiter。

### 4.6 Adaptive delay sweep

根據 group-size 與 lane sweep 的資訊增益，只對 `group=4096` 及 lanes
`1／64／1024／4096` 測試 delay `200／500／1000／2000／5000 us`，每個 case 五輪。這是
設計文件要求的 adaptive subset，不執行沒有資訊增益的完整 Cartesian product；lane=8 與
lane=256 已在 1 ms lane sweep 覆蓋，但沒有再展開五個 delay。

首輪有六個 lane=1024 樣本的 measured phase 小於 10 秒（delay=2000 us 五輪，以及
delay=500 us 一輪），因此提高 iteration count 後重跑。最終統計每個 logical case 都只取
五個有效樣本，不混用被替換的短樣本。

每次 durable invocation 內建驗證：completion submitted/completed count、group counters、
book empty、WAL reopen/replay、durable head、WAL size 與 segment count。測試失敗會以非零
狀態結束。

## 5. StateMachine 結果

每列為五輪 median；latency 單位為 microseconds。

| Active orders | Command-equivalent/s (range) | p50 | p99 | p99.9 | max |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1,128,130 (1,124,900–1,147,520) | 218.019 | 315.021 | 346.510 | 438.283 |
| 1,000 | 1,019,060 (990,360–1,024,740) | 245.180 | 345.828 | 378.660 | 482.765 |
| 10,000 | 999,249 (976,117–1,010,060) | 250.220 | 350.487 | 402.866 | 491.673 |
| 100,000 | 1,023,530 (1,014,340–1,029,400) | 248.606 | 346.370 | 389.621 | 504.616 |

結果未出現與 active state cardinality 成比例的 full-scan 級退化；所有 20 個 run 均回報
`correctness_verified=true`。

## 6. Metrics ownership 結果

每列為五輪 median，latency 單位為 microseconds；throughput 是 command-equivalent/s。

| Case | Throughput (range) | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| writer-only | 1,791,740 (1,778,690–1,814,800) | 135.103 | 207.599 | 249.287 | 1,256.915 |
| separate registries | 1,696,610 (1,668,170–1,707,880) | 135.554 | 212.248 | 254.307 | 1,487.100 |
| shared registry | 370,552 (355,143–429,890) | 609.812 | 1,513.020 | 1,789.130 | 4,315.321 |

separate registry 的 writer median 為 `1,696,613.566636 commands/s`，shared registry 為
`370,552.307171 commands/s`，前者約為後者 **4.58 倍**（約提升 357.86%）。

## 7. Durable group-size 結果

固定 `producer_lanes=8192`、`group_delay=1 ms` 的 sustained-backlog scan。latency 單位為
milliseconds；rotation 是 measured phase 的 WAL segment rotation 次數。

| Configured group | Commands/s (range) | Actual cmd/group | WAL MiB/s | p50 | p99 | p99.9 | max | Rotations | SLO |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 256 | 73.5K (72.0–75.5K) | 255.973 | 8.554 | 107.680 | 202.127 | 237.098 | 241.254 | 0 | fail |
| 512 | 99.6K (96.4–102.9K) | 511.945 | 11.584 | 78.304 | 150.094 | 229.590 | 230.586 | 0 | fail |
| 1,024 | 135.1K (117.6–140.6K) | 1,023.730 | 15.718 | 57.819 | 145.540 | 260.814 | 262.759 | 1 | fail |
| 2,048 | 160.5K (97.8–162.2K) | 2,047.780 | 18.671 | 48.468 | 120.961 | 222.460 | 227.966 | 1 | fail |
| 4,096 | 166.2K (162.6–170.4K) | 4,091.650 | 19.337 | 45.437 | 116.109 | 268.116 | 270.961 | 1 | fail |
| 8,192 | 166.4K (165.2–167.7K) | 6,132.080 | 19.355 | 46.803 | 117.365 | 272.257 | 273.466 | 1 | fail |

4,096 到 8,192 的 median throughput uplift 為約 **0.10%**，低於設計的 5% threshold；依
Pareto 規則應選較小的 4,096。8,192 的 actual commands/group 也沒有達到 configured 上限，
因此不能把 8,192 解讀為每組實際都收滿。

## 8. Producer-lane sweep 結果

固定 `group_delay=1 ms`；latency 單位為 milliseconds。這裡列出五輪 median。

| Group | Lanes | Commands/s | Actual cmd/group | WAL MiB/s | p50 | p99 | p99.9 | max | SLO |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,096 | 1 | 359 | 1.000 | 0.042 | 2.622 | 5.706 | 12.315 | 44.294 | pass |
| 4,096 | 8 | 2.81K | 7.999 | 0.327 | 2.592 | 5.832 | 11.568 | 37.391 | pass |
| 4,096 | 64 | 18.2K | 63.948 | 2.116 | 3.317 | 7.272 | 20.936 | 50.347 | pass |
| 4,096 | 256 | 52.6K | 255.550 | 6.120 | 4.474 | 8.930 | 45.775 | 74.753 | pass |
| 4,096 | 1,024 | 128.2K | 999.746 | 14.912 | 6.828 | 16.446 | 97.644 | 201.570 | fail |
| 4,096 | 4,096 | 159.7K | 3,508.910 | 18.583 | 23.570 | 68.375 | 231.931 | 233.464 | fail |
| 8,192 | 1 | 366 | 1.000 | 0.043 | 2.514 | 5.555 | 11.392 | 44.377 | pass |
| 8,192 | 8 | 2.95K | 8.000 | 0.343 | 2.504 | 5.733 | 15.212 | 41.572 | pass |
| 8,192 | 64 | 18.3K | 63.963 | 2.130 | 3.184 | 7.569 | 21.469 | 43.159 | pass |
| 8,192 | 256 | 53.2K | 255.699 | 6.190 | 4.378 | 8.979 | 45.688 | 78.037 | pass |
| 8,192 | 1,024 | 129.2K | 999.457 | 15.028 | 6.838 | 16.186 | 98.792 | 202.731 | fail |
| 8,192 | 8,192 | 167.1K | 6,206.740 | 19.439 | 46.348 | 131.667 | 284.705 | 285.684 | fail |

套用 4,096／8,192 的 <5% Pareto 規則後，保留 `group=4096, lanes=256, delay=1 ms`；
該 profile 約 52.6K/s，p99=8.93 ms、p99.9=45.8 ms。它仍低於 1M/s，且 delay sensitivity
尚未對 lane=256 展開，因此不直接修改 production default。

## 9. Adaptive delay sweep 結果

固定 `group=4096`。格式為 `p50 / p99 / p99.9 / max`，單位為 milliseconds；每個 case 為
五輪 median。

| Lanes | Delay (us) | Commands/s | Actual cmd/group | p50 / p99 / p99.9 / max (ms) | SLO |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 200 | 435 | 1.000 | 2.281 / 4.971 / 11.081 / 43.285 | pass |
| 1 | 500 | 421 | 1.000 | 2.337 / 5.087 / 11.088 / 42.110 | pass |
| 1 | 1,000 | 372 | 1.000 | 2.563 / 5.357 / 11.750 / 38.994 | pass |
| 1 | 2,000 | 268 | 1.000 | 3.584 / 6.908 / 26.289 / 44.685 | pass |
| 1 | 5,000 | 153 | 1.000 | 6.277 / 10.557 / 19.508 / 53.959 | pass |
| 64 | 200 | 25.1K | 63.871 | 2.388 / 5.492 / 17.928 / 46.199 | pass |
| 64 | 500 | 21.6K | 63.956 | 2.631 / 6.114 / 23.334 / 47.991 | pass |
| 64 | 1,000 | 18.4K | 63.966 | 3.291 / 7.258 / 22.198 / 47.380 | pass |
| 64 | 2,000 | 13.8K | 64.000 | 4.461 / 8.667 / 17.849 / 51.880 | pass |
| 64 | 5,000 | 9.07K | 63.998 | 6.782 / 11.889 / 32.424 / 46.821 | pass |
| 1,024 | 200 | 127.8K | 876.492 | 6.868 / 16.957 / 97.634 / 197.737 | fail |
| 1,024 | 500 | 129.9K | 949.555 | 6.758 / 17.990 / 96.486 / 136.537 | fail |
| 1,024 | 1,000 | 128.3K | 998.046 | 6.814 / 16.465 / 135.623 / 201.133 | fail |
| 1,024 | 2,000 | 115.1K | 1,021.710 | 7.837 / 17.364 / 95.867 / 138.095 | fail |
| 1,024 | 5,000 | 86.0K | 1,023.980 | 10.864 / 21.132 / 70.266 / 96.993 | fail |
| 4,096 | 200 | 163.2K | 3,124.230 | 23.780 / 67.608 / 245.914 / 246.997 | fail |
| 4,096 | 500 | 161.1K | 3,135.140 | 23.583 / 69.063 / 254.317 / 256.432 | fail |
| 4,096 | 1,000 | 159.0K | 3,500.850 | 23.520 / 70.012 / 247.585 / 249.302 | fail |
| 4,096 | 2,000 | 157.3K | 3,788.770 | 23.159 / 65.379 / 210.828 / 221.059 | fail |
| 4,096 | 5,000 | 156.4K | 4,065.480 | 22.996 / 67.871 / 258.775 / 261.407 | fail |

對 saturated lanes=4096，delay 從 200 us 拉高到 5 ms 沒有增加 throughput（163.2K 降至
156.4K），也沒有消除 tail latency；單純提高等待時間不足以接近 1M/s。

## 10. Correctness 與樣本有效性

- Release CTest：73/73 通過。
- StateMachine：20/20 run 回報 `correctness_verified=true`。
- Metrics：5/5 invocation 的 writer-only、separate、shared case 均回報
  `correctness_verified=true`。
- Durable group、lane、delay 矩陣共 200 次 invocation 均成功產生結果並通過 benchmark 內建的
  completion count、group total、WAL reopen/replay 與 durable-head 驗證。
- Delay sweep 有 6 個首輪樣本低於 10 秒；這些樣本沒有納入最終統計，改以提高 iteration 的
  10 次 rerun 取代，確保每個 logical case 都有五個有效樣本。
- `git diff --check` 與 `git diff --cached --check` 通過；測試資料與 WAL 均位於 `/tmp`。

## 11. 結論與瓶頸判讀

1. **StateMachine 不是目前第一瓶頸。** 在 0 至 100K active orders 下仍約 0.999M–1.128M
   command-equivalent/s，沒有看到 full-state scan 隨 state cardinality 放大的退化。
2. **Metrics ownership split 有明確收益。** Separate registry 約 1.697M/s，shared registry
   約 0.371M/s；同一把 registry lock／metric lookup contention 是可重現的成本。
3. **Durable writer 目前 ceiling 約 166K commands/s。** Group 4,096 與 8,192 已平台化，且
   8,192 只增加約 0.1%；在 sustained backlog 下 p99/p99.9 明顯超過暫定 SLO。
4. **提高 delay 不是 1M/s 的解法。** Saturated lane 的 throughput 沒有因 delay 增加而提升，
   queue residence 與 durable completion tail 仍受 backlog、WAL append／sync 與 filesystem I/O
   共同影響。
5. **目前可選的 balanced benchmark profile** 是 `group=4096, lanes=256, delay=1 ms`，約
   52.6K/s 且 p99.9 約 45.8 ms；這只是量測結果，不是 production default。要把它提升到
   1M/s，仍需另案以 syscall／I/O wait／queue depth 分段 profile，區分 WAL encode、CRC、chunk
   copy、write、`fsync` 與 backlog 的實際貢獻，不能只把 fsync 視為已證明的單一根因。

本階段完成的是移除已確認的 StateMachine full-scan 與 shared metrics contention、建立可信的
group-commit frontier；沒有宣稱 durable writer 已達成 1M commands/s。

## 12. 原始結果與限制

原始 benchmark logs 保留於：

- `/tmp/order-books-writer-state.Tnquwx/`
- `/tmp/order-books-writer-metrics.ZRaDey/`
- `/tmp/order-books-writer-durable.q5hvUz/`

限制：

- 結果只代表本機 CPU、kernel、ext4、WAL device、compiler 與 Release flags。
- Sustained-backlog group scan 故意讓 queue 飽和，不代表低 concurrency 的端到端 latency。
- `lane=256` 的 delay sensitivity 沒有展開完整五點；它只在 1 ms lane sweep 中被量測，避免
  沒有資訊增益的 Cartesian product。
- 本輪沒有做 syscall tracing；若要判定 `fsync`、write、CRC 或 encoding 的比例，需另做不
  混入正常 latency 的 profiling run。
- Shared CI 不應以本報告的 RPS 作 hard gate；正式產品 SLO 改變時，應使用相同矩陣重新選擇
  Pareto point。
