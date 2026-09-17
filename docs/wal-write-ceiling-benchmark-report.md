# WAL Write Ceiling 壓測報告

## 1. 報告範圍

- 測試日期：2026-09-17
- source：目前 working tree 的 `wal_write_ceiling` workload 實作
- 目的：在進行百萬 commands/s 優化前，量測目前 WAL append 與 durable group commit 上限
- 測試範圍：單一 WAL writer、單一 shard、單一 instrument fixture
- 本報告不代表 Engine queue、matching、publisher 或 completion 的端到端吞吐
- 測試資料目錄由 benchmark 建立於 `/tmp`，成功後自動清除
- 本報告未執行或改變 Git staging

目前環境沒有 `cmake`／Ninja executable，因此測試 binary 使用目前 source 以 GCC 13.3.0
直接編譯；測試與 benchmark 均未使用 repository 內可能過期的 executable。

## 2. 測試環境

| 項目 | 數值 |
| --- | --- |
| OS | Linux 6.17.0-35-generic x86-64 |
| Compiler | GCC 13.3.0 |
| Language | C++20 |
| Build | Release-style `-O2 -DNDEBUG`；sanitizer tests 另以 ASan/UBSan 編譯 |
| CPU | AMD Ryzen 7 3700X，8 cores／16 threads |
| WAL filesystem | ext4，`/dev/sdb2` |
| WAL device | Crucial BX500 SATA SSD（目前開發機） |
| CPU pinning | 未綁定 |
| Segment size | 256 MiB |
| Record fixture | 單一 instrument、固定 NewOrder、平均 WAL bytes 約 122／command |

這些結果只代表上述機器、kernel、filesystem、compiler 與 source 狀態。正式 Linux
部署機器必須重新建立 baseline。

## 3. Workload 與完成邊界

### 3.1 WAL workload

使用：

```text
--workload=wal_write_ceiling
```

每筆 command 使用正式 `Wal::append()`，包含：

```text
CommittedCommand construction
  -> binary encode
  -> CRC32C
  -> Wal::append()
     -> open
     -> write
     -> close
```

`per_group` 模式每組追加完成後呼叫一次 `Wal::sync()`；`none` 模式不呼叫顯式 group
sync，只量測 append-return path。segment rotation 由 `Wal::append()` 內部觸發的必要
sync 仍會計入 append path。

### 3.2 設定

| 參數 | 值 |
| --- | --- |
| Shard | 1 |
| Instrument | 1 |
| Segment | 256 MiB |
| Warmup | 各矩陣 100 groups |
| Measured | 各矩陣 1,000 groups；rotation run 11,000 groups |
| Group sizes | 1、16、64、256、512、1024 |
| Sync modes | `per_group`、`none` |
| Input concurrency | 單執行緒、單 WAL instance |
| Snapshot／matching／publisher | 不包含 |

`iterations` 在此 workload 表示 measured groups；measured commands 為
`iterations × wal_group_size`。

### 3.3 計時邊界

Measured phase 從第一個 measured group 開始前取得 `steady_clock`，到最後一個 measured
group 完成 append／sync 後結束。Warmup、final sync、reopen、replay 與 correctness
validation 不計入 throughput。

成功輸出前必須完成：

- measured command 數量驗證；
- final `Wal::sync()`；
- WAL reopen；
- replay record 數量與連續 `EngineSeq` 驗證；
- instrument、command type 與 command identity 驗證。

## 4. 監測點

Benchmark 輸出：

- `commands_per_second`
- `target_attainment_percent`（相對 1,000,000 commands/s）
- `wal_mib_per_second`
- `average_wal_bytes_per_command`
- `append_group_p50/p99/p99.9/max_us`
- `sync_p50/p99/p99.9/max_us`
- `group_total_p50/p99/p99.9/max_us`
- `wal_bytes_delta` 與累計 `wal_bytes`
- `segment_count` 與 `measured_segment_rotations`
- `completion_boundary`
- `replay_verified`

每個 group 只取 append、sync 與 total timestamp；沒有對每筆 command 額外呼叫 clock，
避免量測本身污染百萬級 append path。

外部診斷：

- `strace -f -c -e trace=openat,write,close,fsync`
- `perf stat` 嘗試量測 cycles、instructions、context switches、page faults

## 5. 功能與 sanitizer 驗證

| 測試 | 結果 |
| --- | --- |
| Release-style GoogleTest | 36/36 passed，9 suites |
| ASan + UBSan + leak detection | 36/36 passed |
| `wal_write_ceiling` per-group smoke | passed，8 commands，replay verified |
| `wal_write_ceiling` append-only smoke | passed，8 commands，replay verified |
| `--workload=all` smoke | passed，Engine、既有 workloads、WAL、recovery 全部成功 |
| 256 MiB rotation run | passed，2 segments，1 measured rotation，replay verified |
| `git diff --check` | passed |

## 6. Durable WAL group-size 結果

每列為單次 Release-style run，100 warmup groups、1,000 measured groups；未使用 CPU
pinning。數字包含目前逐筆 `open/write/close` 與每組 `fsync`。

| Sync mode | Group size | Commands | Throughput (commands/s) | WAL MiB/s | Target attainment | p50 group total (us) | p99 group total (us) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `per_group` | 1 | 1,000 | 1,057 | 0.123 | 0.106% | 916.6 | 1,974.3 |
| `per_group` | 16 | 16,000 | 12,577 | 1.463 | 1.258% | 1,088.9 | 3,537.5 |
| `per_group` | 64 | 64,000 | 39,625 | 4.610 | 3.963% | 1,405.1 | 3,147.6 |
| `per_group` | 256 | 256,000 | 81,756 | 9.512 | 8.176% | 3,017.8 | 5,756.2 |
| `per_group` | 512 | 512,000 | 105,644 | 12.292 | 10.564% | 4,603.2 | 7,629.8 |
| `per_group` | 1024 | 1,024,000 | 131,381 | 15.286 | 13.138% | 7,250.8 | 12,363.0 |

### 6.1 Append-only 對照

| Sync mode | Group size | Commands | Throughput (commands/s) | WAL MiB/s | Target attainment | Completion boundary |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `none` | 256 | 256,000 | 189,306 | 22.025 | 18.931% | append return |

`none` 仍然會包含 WAL append 內部的 segment rotation sync；它不是 durable throughput。

### 6.2 直接觀察

- 在測試矩陣中，最高 durable throughput 是 group 1024 的 **131,381 commands/s**，仍
  只有目標的 **13.14%**。
- 目前設計預設 group 256 的 durable throughput 是 **81,756 commands/s**，約為目標
  的 **8.18%**，距離目標約差 **12.2 倍**。
- group 256 的 append-only 對照為 **189,306 commands/s**，即使排除顯式 group sync，
  仍只有目標的 **18.93%**，距離目標約差 **5.3 倍**。
- group size 增大可提升吞吐，但在本次 1024 仍未接近百萬；這只能說明較大的 group
  有效，不能直接改寫 production 預設。

## 7. Segment rotation 結果

設定：`per_group`、group 256、100 warmup groups、11,000 measured groups，共
2,816,000 measured commands，WAL measured delta 約 343.5 MB。

| 項目 | 數值 |
| --- | ---: |
| Throughput | 83,060 commands/s |
| Target attainment | 8.306% |
| Segment count | 2 |
| Measured rotations | 1 |
| p50 group total | 2,915.1 us |
| p99 group total | 5,579.1 us |
| p99.9 group total | 29,206.2 us |
| Maximum group total | 250,891.5 us |
| Replay | verified |

與沒有 rotation 的 group 256 單次結果（81,756 commands/s）相比，整體吞吐沒有顯著
提升或崩潰；但 maximum latency 出現約 250 ms rotation／I/O spike，rotation 對尾延遲
仍有實際影響。

## 8. Syscall 診斷結果

使用 100 warmup groups、1,000 measured groups、group 256、`per_group`，並透過 `strace`
收集。因 instrumentation 會放大 latency，以下只用於 syscall 結構，不與正常 run 的
throughput 混用。

| Syscall | Calls | Traced time | Time share |
| --- | ---: | ---: | ---: |
| `openat` | 282,717 | 1.591 s | 38.22% |
| `write` | 281,602 | 1.346 s | 32.33% |
| `close` | 282,717 | 1.143 s | 27.45% |
| `fsync` | 1,104 | 0.083 s | 2.00% |
| Total | 848,140 | 4.164 s | 100.00% |

在 traced syscall time 中，`openat + write + close` 合計 **97.99%**，`fsync` 約
**2.00%**。這支持目前逐筆檔案操作是主要限制候選；它不是完整 CPU／wall-time 歸因，
因為 strace 本身會改變執行時間。

`perf stat` 因主機 `perf_event_paranoid=4` 無權限執行而失敗；沒有修改 kernel 設定或
要求額外權限。

## 9. 結論

目前已取得的證據顯示：

1. 目前 WAL implementation 在 production group 256 的實測 durable 上限約為
   **82k commands/s**。
2. 將診斷 group size 提高到 1024，實測約 **131k commands/s**，仍遠低於一百萬。
3. 即使不執行顯式 group sync，現有 append path 約 **189k commands/s**，因此問題不只
   是 `fsync`；逐筆 `open/write/close`、codec／CRC、WAL index 與檔案操作成本都必須納入。
4. `fsync` 在正常 per-group matrix 中是每組約 1～2 ms 的成本，但 syscall trace 顯示
   本次現有 implementation 的 traced syscall time 主要消耗在逐筆檔案操作。
5. Segment rotation 不影響本次長測的平均 throughput 結論，但會造成明顯尾延遲 spike。

因此，目前尚不能宣稱「fsync 是唯一根因」，也不能用 CPU threads 增加或更大 group size
直接宣稱可達一百萬。下一階段應先針對已量化的逐筆 `open/write/close` 與 WAL append
路徑設計最小優化，並以相同 workload 重新量測；任何 production 修改前仍需保留
durability 與 replay correctness 驗證。

## 10. 限制與可比性

- 每個 group size 目前為單次實測，尚未形成五次重複的中位數／最差 p99 baseline。
- CPU 未綁定，頻率與背景負載可能影響單次數據。
- `perf stat` 未能取得硬體 counters。
- `none` 模式不是 durable 服務能力，只是 append path 對照。
- 固定 NewOrder fixture 不涵蓋不同 payload 大小。
- `Wal` 會在記憶體保存 cached records；長測 RSS 成長是目前實作成本的一部分。
- 測試結果只代表目前 Linux 開發機，不等同正式部署磁碟 SLA。

正式 baseline 建議在實際 Linux WAL device 上，對 group 256、512、1024 各重複至少五次，
同時記錄 CPU、disk utilization、await、RSS 與 syscall／block I/O 資料，再決定下一個
production 優化項目。
