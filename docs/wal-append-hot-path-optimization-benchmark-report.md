# WAL Append Hot-path Optimization 壓測報告

## 1. 報告範圍

- 測試日期：2026-09-17
- 測試 source：working tree 的 WAL active descriptor、`append_batch()` 與 geometric `records_` capacity growth 修正
- baseline source：`HEAD` 在 `/tmp` 的獨立副本；未修改 repository index
- baseline executable：使用先前由該 `HEAD` 副本成功建立並驗證的 `/tmp/order_books_base_wal`；本輪未採用重編失敗產物
- 目的：以相同參數比較 batch append、group `fsync`、durable engine 與 segment rotation
- 範圍：單一 shard、單一 instrument、單一 WAL writer
- 本輪壓測未執行會改變 Git staging 的操作；fixed source 修正已在壓測前完成
- benchmark 使用唯一的 `/tmp` 暫存目錄，完成後由 workload 清理

本環境沒有 CMake，因此 benchmark 以目前 source 使用 GCC 直接編譯；結果不使用 repository
內可能過期的 executable。

## 2. 測試環境

| 項目 | 數值 |
| --- | --- |
| OS | Linux 6.17.0-35-generic x86-64 |
| Compiler | GCC 13.3.0 |
| Language | C++20 |
| Build | Release-style `-O2 -DNDEBUG` |
| CPU | AMD Ryzen 7 3700X，8 cores／16 threads |
| WAL filesystem | ext4，`/dev/sdb2` |
| CPU pinning | 未綁定 |
| WAL segment size | 256 MiB |
| WAL record fixture | 固定 single-instrument `NewOrder`，約 122 bytes／command |

結果只代表上述開發機、kernel、filesystem、compiler 與 source 狀態，不能直接視為正式
部署裝置的 SLA。

## 3. 負載情境與完成邊界

### 3.1 WAL ceiling 矩陣

執行 workload：

```text
--workload=wal_write_ceiling
```

- `per_group`：每組 `append_batch()` 完成後呼叫一次 `Wal::sync()`；完成邊界為 group fsync。
- `none`：不執行顯式 group sync；完成邊界為 append return，只作 append path 對照，不代表 durable throughput。
- group size：1、16、64、256、512、1024。
- 每個 group size：100 warmup groups、1,000 measured groups、baseline 與 fixed 各重複 5 次。
- command construction、frame encoding、CRC、batch append 都位於 append phase 計時範圍。

每次成功執行都會再完成 final sync、WAL reopen、replay、command identity、EngineSeq 與
record count 驗證。

### 3.2 Durable engine

執行 workload：

```text
--workload=engine_durable_single_instrument
```

- 1 shard、1 instrument、1 order book。
- 1,024 producer lanes，每 lane 同時最多 1 筆 in-flight command。
- group commit 上限 256 commands，最大 delay 200 microseconds。
- 每個 iteration 提交 2 筆 crossing new-order，預期完成 1 trade。
- 100 warmup iterations、1,000 measured iterations；baseline 與 fixed 各重複 5 次。
- completion boundary 為 durable callback。

### 3.3 Segment rotation

- `per_group`、group size 256。
- 100 warmup groups、11,000 measured groups；baseline 與 fixed 各重複 5 次。
- measured commands：2,816,000，WAL delta 343,552,022 bytes，超過 256 MiB。
- 目的：驗證跨 segment rotation、延遲尾端與 replay correctness。

## 4. 監測點

WAL workload 記錄：

- commands/s、target attainment、WAL MiB/s、平均 WAL bytes／command；
- append group p50／p99／p99.9／max；
- sync p50／p99／p99.9／max；
- group total p50／p99／p99.9／max；
- WAL bytes delta、segment count、measured rotations；
- completion boundary 與 `replay_verified`。

Durable engine 記錄：

- commands/s、trades/s；
- completion latency p50／p99／p99.9／max；
- WAL bytes、active orders、active price levels；
- group size、group delay、fsync mode 與 completion boundary。

外部 syscall trace 使用：

```text
strace -f -c -e trace=openat,write,close,fsync
```

## 5. WAL `per_group` 結果

每列為相同參數下五次執行的 median；latency 欄位依序為 group-total p50、五次中最大的
group-total p99（worst p99）、group-total p99.9 與 max，單位為 microseconds。

| Group | Commands/run | Baseline throughput / target % | Baseline p50 / worst p99 / p99.9 / max (us) | Baseline MiB/s | Fixed throughput / target % | Fixed p50 / worst p99 / p99.9 / max (us) | Fixed MiB/s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,000 | 799.45 / 0.079945% | 1,188.7 / 2,878.81 / 4,795.51 / 9,402 | 0.0930146 | 818.394 / 0.0818394% | 1,166.86 / 3,210.15 / 5,258.45 / 10,076.4 | 0.0952188 |
| 16 | 16,000 | 11,889.1 / 1.18891% | 1,230.88 / 4,795.8 / 4,228.49 / 9,942.35 | 1.38328 | 11,694.5 / 1.16945% | 1,276.05 / 2,449.92 / 3,669.19 / 10,740.4 | 1.36064 |
| 64 | 64,000 | 31,569.8 / 3.15698% | 1,995.44 / 5,202.13 / 12,226.4 / 12,275.4 | 3.67309 | 40,051.1 / 4.00511% | 1,389.57 / 5,565.47 / 9,334.63 / 25,753.2 | 4.65988 |
| 256 | 256,000 | 84,465.5 / 8.44655% | 2,885.02 / 5,847.37 / 30,536.7 / 34,402.4 | 9.82741 | 125,841 / 12.5841% | 1,897.49 / 4,986.68 / 17,488.2 / 24,117.2 | 14.6413 |
| 512 | 512,000 | 105,789 / 10.5789% | 4,603.48 / 7,943.05 / 33,847.3 / 56,109.7 | 12.3084 | 192,153 / 19.2153% | 2,360.66 / 9,078.59 / 38,751.2 / 47,536.5 | 22.3566 |
| 1,024 | 1,024,000 | 135,804 / 13.5804% | 7,257.21 / 114,242 / 59,004.7 / 109,531 | 15.8006 | 294,964 / 29.4964% | 3,187.03 / 11,385.2 / 65,459.3 / 91,966.9 | 34.3185 |

Fixed group=256 的 throughput 相對 baseline median 約提升 49.0%；這是同參數比較，不代表
已達到一百萬 commands/s。

### 5.1 Fixed group size 256 詳細監測點

下表為 fixed 五次結果的 median，單位為 microseconds；`worst p99` 為五次中的最大值：

| Metric | p50 median | p99 median | worst p99 | p99.9 median | max median |
| --- | ---: | ---: | ---: | ---: | ---: |
| append group | 323.256 | 495.439 | 506.049 | 15,769.5 | 23,428.3 |
| sync | 1,629.33 | 2,649.61 | 5,086.48 | 10,162.5 | 13,729.1 |
| group total | 1,972.48 | 5,179.22 | 6,800.02 | 17,510.4 | 25,612.2 |

## 6. WAL append-only 對照

`sync=none`、group size 256、100 warmup、1,000 measured groups、baseline 與 fixed 各 5 次：

| Metric | Baseline median | Fixed median | Fixed worst p99 |
| --- | ---: | ---: | ---: |
| Throughput | 196,803 commands/s | 668,630 commands/s | — |
| Target attainment | 19.6803% | 66.863% | — |
| WAL throughput | 22.8977 MiB/s | 77.7939 MiB/s | — |
| Append/group total p50 | 1,216.25 us | 311.193 us | — |
| Append/group total p99 | 1,697.63 us | 508.263 us | 508.263 us |
| Append/group total p99.9 | 14,942.6 us | 16,053.3 us | — |
| Append/group total max | 27,383.2 us | 23,161.2 us | — |
| Completion boundary | append return | append return | — |

這不是 durable capacity，只用來隔離顯式 group sync 的成本。

## 7. Durable engine 結果

單一 shard、單一 instrument、group size 256；以下為 baseline 與 fixed 各 5 次的 median，
`worst p99` 為五次 completion p99 的最大值：

| Metric | Baseline median | Fixed median | Fixed worst p99 |
| --- | ---: | ---: | ---: |
| Commands/s | 60,430.3 | 71,402.6 | — |
| Trades/s | 30,215.1 | 35,701.3 | — |
| Completion p50 | 15,922 us | 12,992.1 us | — |
| Completion p99 | 16,712.1 us | 14,682 us | 15,642.8 us |
| Completion p99.9 | 16,727.8 us | 14,686.8 us | — |
| Completion max | 16,728.2 us | 14,687.2 us | — |
| WAL bytes | 268,422 | 268,422 | — |
| Active orders / levels | 0 / 0 | 0 / 0 | — |

Fixed durable engine throughput 相對 baseline median 約提升 18.2%。

## 8. Segment rotation 結果

使用設計指定的 `per_group`、group size 256、100 warmup、11,000 measured groups，五次執行。
每次 measured WAL delta 都是 343,552,022 bytes、兩個 segments、一次 rotation，且
`replay_verified=true`。

| Metric | Baseline median | Fixed median | Fixed worst p99 |
| --- | ---: | ---: | ---: |
| Measured commands | 2,816,000 | 2,816,000 | — |
| Throughput | 82,140.8 commands/s | 124,879 commands/s | — |
| Target attainment | 8.21408% | 12.4879% | — |
| WAL throughput | 9.55694 MiB/s | 14.5295 MiB/s | — |
| Append group p50 | 1,347.07 us | 323.106 us | — |
| Append group p99 | 1,698.69 us | 467.035 us | 481.964 us |
| Append group p99.9 | 2,472.13 us | 3,517.24 us | — |
| Append group max | 209,569 us | 253,230 us | — |
| Sync p50 | 1,616.02 us | 1,590.93 us | — |
| Sync p99 | 4,325.34 us | 4,122.99 us | 4,299.06 us |
| Sync p99.9 | 12,180.4 us | 9,978.3 us | — |
| Sync max | 46,210 us | 41,842.3 us | — |
| Group total p50 | 2,988.8 us | 1,929.11 us | — |
| Group total p99 | 5,852.1 us | 4,558.94 us | 4,761.82 us |
| Group total p99.9 | 26,320.7 us | 24,359.2 us | — |
| Group total max | 210,873 us | 255,214 us | — |

Fixed rotation throughput 相對 baseline median 約提升 52.0%。這是同參數比較；CPU 未綁定，
尾延遲仍可能受主機背景負載影響。

## 9. Syscall trace

為與 baseline 可比較，使用 100 warmup、1,000 measured、group size 256、`per_group`，並將
benchmark stdout 導向 `/dev/null`：

| Syscall | Baseline calls | Fixed calls |
| --- | ---: | ---: |
| `openat` | 282,717 | 16 |
| `write` | 281,602 | 1,102 |
| `close` | 282,717 | 16 |
| `fsync` | 1,104 | 1,104 |

Fixed data writes 約為 warmup／measured group 與 header 的數量，而不是 command 數量；
`openat`／`close` 不再逐 command 增長。總 syscall 仍包含 dynamic loader、directory sync、
WAL reopen/replay 與 benchmark lifecycle。

## 10. Correctness 驗證

- baseline 與 fixed 矩陣每一個 WAL benchmark run 都通過 `replay_verified=true`。
- `--workload=all --iterations=5 --warmup=1` smoke 通過，包含 Engine、既有 workloads、WAL
  與 recovery。
- Release GoogleTest：41/41 通過。
- ASan + UBSan + leak detection：41/41 通過。
- 壓測期間未執行改變 staging 的操作。

## 11. 瓶頸分析

### 11.1 已確認的改善

- geometric capacity growth 消除了長 WAL 中每 batch 搬移全部 `records_` 的退化。
- `per_group` group=256 median 由 84.5k 提升至 125.8k commands/s；rotation median 由 82.1k
  提升至 124.9k commands/s。
- `sync=none` median 由 196.8k 提升至 668.6k commands/s，表示 append path 的 per-batch
  allocation／copy 成本原先也是主要限制。
- syscall trace 中，fixed batch data write 隨 group 數量增加，而不是隨 command 數量增加；
  active descriptor 沒有在每筆 command 或每個 group 重新 open／close。

### 11.2 新壓測揭露的主要成本

修正後 group=256 rotation 的 append p50 約 0.323 ms、sync p50 約 1.591 ms；durable
path 的固定成本仍主要來自 group `fsync`，但 append、encoding、CRC、cached-record copy
與 filesystem write 仍共同構成剩餘差距。

目前 [wal.cpp](../src/persistence/wal.cpp) 已不再每次 batch 執行精確 capacity reserve：

```cpp
if (required_capacity > records_.capacity()) {
  records_.reserve(std::max(required_capacity, grown_capacity));
}
```

因此目前可作出的證據化結論是：

1. WAL active descriptor／batch write 的第一階段優化已生效。
2. `records_` 精確 reserve 是本次 staged implementation 的主要長測退化根因，已以幾何成長
   修正並透過同參數 before/after 矩陣驗證。
3. `fsync` 仍是 durable group 的固定成本；一百萬 commands/s 仍不是本切片完成條件，後續
   優化必須等新的 profile 證明 codec、CRC、copy 或 storage device 是主要限制後另案設計。

## 12. 限制

- CPU 未綁定，durable engine 重複測試有明顯背景負載變異。
- rotation 使用單一固定 NewOrder fixture，不代表所有 payload 大小或 filesystem 的 SLA。
- syscall trace 只用於驗證 syscall shape，會改變正常 latency。
- `records_` 長期保留 decoded command；其記憶體與 allocation 成本仍未優化。
- 1,000,000 commands/s 不是本輪完成條件；目前結果不能宣稱達標。
