# Publisher Cursor Group Persistence 壓測報告

## 1. 測試目的與範圍

本報告驗證 `docs/publisher-cursor-group-persistence-design.md` 的 Publisher drain
與 cursor group persistence 行為，重點是：

- baseline、default、ceiling 三種 cursor persistence policy 的 throughput 差異；
- `confirmed_cursor`、`durable_cursor`、WAL head 與 reopen durability；
- cursor persistence 次數與實際 commands-per-persist；
- time-trigger、pipeline smoke、sanitizer regression 與 cursor filesystem syscall 形狀。

Publisher drain 使用 benchmark 內的 immediate-success `CountingPublisherSink`，因此結果是
Publisher replay、event construction、sink ACK 與 cursor persistence 的上限，不代表實際
downstream network publisher 的吞吐量。

## 2. 測試環境

| 項目 | 值 |
|---|---|
| 平台 | Linux 6.17.0-35-generic x86_64 |
| CPU threads | 16 |
| CPU affinity | `taskset -c 2-3` |
| Filesystem | ext4，`/dev/sdb2` |
| Compiler | GCC 13.3.0 |
| Build type | Release |
| Source | `HEAD=83bcceb0c9d61d2e05564f69ee0512c6f5df924d` 加目前 worktree changes |
| Binary | `build/ReleaseBenchmark/benchmarks/order_books_benchmark` |
| Persistence delay | `--publisher-cursor-persist-max-delay-us=1000` |
| Pipeline batch size | `256` |

正式測試使用新的空 data directory。原始輸出保留於：
`/tmp/order_books_publisher_retest.SXLXnV/logs/`；strace 原始檔為
`/tmp/order_books_publisher_retest.SXLXnV/trace/default.trace`。

## 3. 測試命令

建置：

```bash
cmake --preset release-benchmark
cmake --build --preset release-benchmark --parallel 2
```

Publisher drain 的共同參數為：

```text
--workload=engine_pipeline_ceiling
--pipeline-stage=publisher_drain
--pipeline-batch-size=256
--publisher-cursor-persist-max-delay-us=1000
```

正式矩陣如下：

| Case | `max_commands` | measured commands | iterations | warmup | 輪數 |
|---|---:|---:|---:|---:|---:|
| baseline | 1 | 5,120 | 20 | 4 | 5 |
| default | 256 | 2,048,000 | 8,000 | 100 | 5 |
| ceiling | 1,024 | 2,048,000 | 8,000 | 100 | 5 |

512,000 commands 的 ceiling calibration 為 3,811.78 ms、134,320 commands/s；因此
default／ceiling 長測使用 2,048,000 commands，使每輪 measured drain 超過 10 秒。
baseline 在 5,120 commands 已需約 14 秒；若使用相同 2,048,000 commands，估計單輪會接近
96 分鐘，故未納入本次五輪長測。

## 4. 正式矩陣 raw results

每一行均為 benchmark 的 measured output；所有行均有
`correctness_verified=true` 與 `durable_cursor_verified=true`。

### 4.1 Baseline：`max_commands=1`

| Run | commands/s | elapsed ms | successful persists | commands/persist |
|---:|---:|---:|---:|---:|
| 1 | 344.508 | 14,861.8 | 5,120 | 1.000000 |
| 2 | 367.005 | 13,950.8 | 5,120 | 1.000000 |
| 3 | 361.872 | 14,148.7 | 5,120 | 1.000000 |
| 4 | 353.699 | 14,475.6 | 5,120 | 1.000000 |
| 5 | 344.779 | 14,850.1 | 5,120 | 1.000000 |

### 4.2 Default：`max_commands=256`

| Run | commands/s | elapsed ms | successful persists | commands/persist |
|---:|---:|---:|---:|---:|
| 1 | 80,089.6 | 25,571.4 | 8,000 | 256.000000 |
| 2 | 76,279.7 | 26,848.6 | 8,001 | 255.968004 |
| 3 | 76,794.9 | 26,668.4 | 8,001 | 255.968004 |
| 4 | 76,163.4 | 26,889.6 | 8,001 | 255.968004 |
| 5 | 74,187.2 | 27,605.8 | 8,001 | 255.968004 |

### 4.3 Ceiling：`max_commands=1024`

| Run | commands/s | elapsed ms | successful persists | commands/persist |
|---:|---:|---:|---:|---:|
| 1 | 138,927 | 14,741.6 | 3,764 | 544.102019 |
| 2 | 131,338 | 15,593.4 | 3,840 | 533.333333 |
| 3 | 134,873 | 15,184.7 | 3,814 | 536.969061 |
| 4 | 140,176 | 14,610.2 | 3,796 | 539.515279 |
| 5 | 135,907 | 15,069.1 | 3,824 | 535.564854 |

## 5. 統計結果

| Case | throughput median | throughput range | elapsed median | target attainment |
|---|---:|---:|---:|---:|
| baseline | 353.699/s | 344.508–367.005/s | 14,475.6 ms | 0.0354% |
| default | 76,279.7/s | 74,187.2–80,089.6/s | 26,848.6 ms | 7.62797% |
| ceiling | 135,907/s | 131,338–140,176/s | 15,069.1 ms | 13.5907% |

相對改善：

- default 相對 baseline：`215.663x`，約 `+21,466.28%`；
- ceiling 相對 default：`1.782x`，約 `+78.17%`；
- ceiling 相對 baseline：`384.245x`，約 `+38,324.48%`。

baseline 與 optimized cases 的 measured backlog 不同，因此上述倍率是 normalized throughput
比較，不是完全相同 backlog 的 paired latency 實驗。

## 6. Persistence 與 durability 結果

- baseline 完成 5,120 commands，5,120 次 persistence，維持逐筆行為。
- default 完成 2,048,000 commands，約 8,000 次 persistence，commands/persist 約 256。
- ceiling 完成 2,048,000 commands，約 3,800 次 persistence，commands/persist 約 537。
- 所有正式輪次均滿足：

  ```text
  sink_calls == measured_commands
  sink_events == measured_commands * 2
  confirmed_cursor == durable_cursor == cursor_head
  durable_cursor_verified=true
  ```

ceiling 沒有達到名義上的 1,024 commands/persist，原因是 1 ms time trigger 常在 count
threshold 前觸發；但它仍明顯高於逐筆 persistence，沒有退化成 baseline 行為。

## 7. 其他驗證

### 7.1 Time trigger

```text
EventPublisherTest.TimeTriggerFlushesWithoutAnotherNotification: PASS
```

測試在 `stop()` 前觀察 durable cursor 已前進，避免由 clean-stop final flush 掩蓋 timer
行為。

### 7.2 Pipeline smoke

`--pipeline-stage=all --iterations=20 --warmup=4 --pipeline-batch-size=64` 通過；其中
Publisher drain 為 1,280 commands、80,060.9 commands/s、5 次 persistence，且
`durable_cursor_verified=true`。

### 7.3 Correctness regression

- ReleaseBenchmark CTest：52/52 通過，總時間 0.43 秒。
- ASan/UBSan CTest：52/52 通過，總時間 2.64 秒。

### 7.4 Representative syscall trace

default policy、5,120 commands 的代表 run 結果：

| syscall | 次數 |
|---|---:|
| `openat` | 101 |
| `write` | 51 |
| `fsync` | 54 |
| `fdatasync` | 0 |
| `rename` | 24 |
| `renameat` | 0 |
| `renameat2` | 0 |

trace 中可直接看到 cursor persistence 的 temp-file `fsync`、`rename` 及 event-replay
directory `fsync`。上述總數包含 WAL fixture 建立、Publisher measured run 與 reopen，不能
全部歸因於 cursor persistence。

## 8. Latency 解讀限制

目前 `publisher_drain` 每次 process 只有一個完整 backlog drain sample，因此輸出的
`group_p50_us`、`group_p99_us`、`group_p99.9_us` 與 `group_max_us` 在同一輪會相同；它們是
full-backlog elapsed，不是逐 command 或逐 cursor group latency。正式結論以五輪 throughput、
elapsed range 與 persistence count 為主，不將這些欄位宣稱為 per-command tail latency。

另外，5,120-command paired short probes 曾出現 1.36–4.70 秒的 filesystem/cache outlier；
因此未用短 probe 取代長測統計。

## 9. 結論

cursor group persistence 確實大幅降低 cursor persistence 次數，default median throughput
約為 baseline 的 216 倍，ceiling 再提高約 78%。在本環境與 immediate sink 下，ceiling median
約 135.9K commands/s，仍未達 1M/s 目標；主要可觀察限制仍是 cursor file 與 directory
`fsync`，且 1 ms time trigger 限制了 1,024 count policy 的實際 group size。

本次結果已證明 correctness、durable reopen 與 group persistence 有效，但尚未滿足「所有
policy 使用相同 2,048,000-command backlog 且每輪超過 10 秒」的嚴格 paired benchmark 條件。
若要完成該條件，需安排 baseline 的長時間 overnight run，或先正式決定以每 case 各自達到
10 秒的 normalized throughput protocol 取代相同 backlog 要求；本報告不以現有結果假裝已完成
該項驗收。

本次測試沒有修改 production source，也沒有執行任何會改變 Git staging 的操作。
