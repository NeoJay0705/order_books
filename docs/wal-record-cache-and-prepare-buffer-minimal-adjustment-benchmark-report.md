# WAL record cache／prepare buffer 最小調整壓測報告

## 結論

- Result：`VALID`，但屬於單輪 standalone observation。
- append-return wall throughput 為 `1,257,690 commands/s`，service throughput 為
  `1,388,160 commands/s`；超過 1M target，但低於 `1.53M commands/s` 參考門檻。
- 第二點 lane-contiguous prepare buffer 的 candidate spot run service throughput 為
  `1,780,090 commands/s`，高於第一點單輪結果約 `28.2%`；此比較不是 paired baseline／candidate，
  只能視為方向性改善。
- correctness gate 通過：`replay_verified=true`、`wal_byte_plan_verified=true`、0 rotation、0 WAL sync。
- 第二點 run 的 preflight 合格且沒有外部 benchmark／fio／strace；workload 本身仍造成 CPU 與 page-cache
  write activity，因此不能把本輪視為純 CPU ceiling，也不能單靠本輪證明 prepare buffer 的因果收益。
- 本報告不能宣稱 production ceiling；第二點是否正式保留仍需同環境 baseline／candidate 配對。

## 環境與有效性

| 項目 | 結果 |
| --- | --- |
| Run root | `/home/neojhou/wal-record-cache-spot-HMGA9mNU` |
| Git HEAD | `ebc463b1b3fb1176e08c8aa785b81a164b78e4d6` |
| Binary SHA-256 | `aaba2645dacfea5f3de792b6fd6c6a0aecf5cff1587bfc366d9cf2a564738ad4` |
| CPU affinity | `2-7` |
| WAL filesystem | `/dev/sdb2`，ext4 |
| Preflight CPU idle | 約 `97.7%` 至 `99.1%`（affinity CPU） |
| Preflight iowait | `0%` |
| Preflight disk activity | `%util`／`aqu-sz` 未持續超標；僅有小幅瞬間活動 |
| Preflight available space | `139,701,596,160` bytes（約 `131 GiB`） |
| Residual benchmark/fio/strace | 無 |
| Exit status | `0` |
| Replay | `true` |
| WAL byte plan | `true` |
| Measured rotations | `0` |
| Measured WAL sync calls | `0` |
| Parallel prepare groups / tasks | `2,048 / 4,096` |

## 壓測設定

| 項目 | 值 |
| --- | ---: |
| Workload | `wal_write_ceiling` |
| Group size | `8,192` |
| Prepare workers | `2` |
| Parallel prepare threshold | `4,096` |
| Sync | `none` |
| Segment size | `4 GiB` |
| Warmup groups | `4` |
| Measured groups / commands | `2,048 / 16,777,216` |
| WAL bytes | `2,046,820,352` |
| WAL bytes / command | `122` |

## 壓測結果

| 指標 | 結果 |
| --- | ---: |
| Wall commands/s | `1,257,690` |
| Service commands/s | `1,388,160` |
| Wall WAL MiB/s | `146.331` |
| Service WAL MiB/s | `161.510` |
| Append p50 | `5,911.768 us` |
| Append p99 | `6,838.376 us` |
| Append p99.9 | `9,374.613 us` |
| Append max | `22,912.8 us` |
| Measured elapsed | `13,339.7 ms` |
| Service elapsed | `12,086 ms` |

## 資源觀測

### Benchmark measured rusage

| 指標 | 結果 |
| --- | ---: |
| User CPU seconds | `19.0341` |
| System CPU seconds | `1.81047` |
| Voluntary context switches | `5,983` |
| Involuntary context switches | `580` |
| Measured write bytes | `2,046,824,448` |
| Dirty bytes after | `2,025,811,968` |
| Writeback bytes delta | `0` |

### Workload observer

Observer 覆蓋 setup、measured window 與 replay，以下數字作為資源佐證，不是 phase attribution：

| 指標 | 結果 |
| --- | ---: |
| Affinity CPU 平均 idle | `78.278%` |
| Affinity CPU 最低 sample idle | `0%` |
| 平均 iowait | `1.777%` |
| 最大 iowait sample | `95.96%` |
| `sdb` 平均 `aqu-sz` | `1.375` |
| `sdb` 最大 `aqu-sz` | `12.620` |
| `sdb` 平均 `%util` | `11.251%` |
| `sdb` 最大 `%util` | `98.000%` |

GNU `/usr/bin/time -v` 全程（含 setup/replay）另記錄：user `29.32 s`、system `9.62 s`、elapsed
`35.26 s`、CPU `110%`、max RSS `9,566,996 KiB`。這些不可直接與 measured window latency
混用。

## 判讀

相較歷史非配對結果（append p99.9 `676,690.794 us`、max `1,483,040 us`），本輪 tail 數字較低，
但 p50 `5,911.768 us` 並未低於歷史 `5,761.093 us`。由於兩者不是同一環境的 paired baseline，
這只能記為方向性觀察，不能計算正式改善百分比或歸因於 record-cache container change。

本輪 CPU 與磁碟數據表示：

- `sync=none` 與 0 rotation 已排除 WAL `fsync`／rotation path；
- append-return 仍同時受到 prepare／serialize CPU 與 page-cache write／background storage activity
  影響；
- 目前證據不足以在 CPU-bound 與 writeback-bound 之間做唯一歸因。

## 第二點：lane-contiguous prepare buffer candidate spot run

這一節記錄第二點 unstaged candidate 的單輪結果；前述第一點資料保留作為方向性參考，兩者不是
同一 run 的 paired baseline／candidate。

### 環境與有效性

| 項目 | 結果 |
| --- | --- |
| Run root | `/home/neojhou/wal-prepare-buffer-spot-FLhvtarp` |
| Binary SHA-256 | `65025090dbf7279119200c7e727bce7fec318ec2dd3b5e47fba523be92f46d19` |
| CPU affinity | `2-7` |
| Preflight CPU idle | 約 `97.6%` 至 `99.4%` |
| Preflight iowait | `0%` |
| Preflight disk activity | sdb `%util` 約 `0--0.5%`、`aqu-sz` 約 `0--0.03` |
| Preflight available space | 約 `128 GiB` |
| Residual benchmark/fio/strace | 無 |
| Exit status | `0` |
| Replay / WAL byte plan | `true / true` |
| Measured rotations / sync calls | `0 / 0` |
| Parallel prepare groups / tasks | `2,048 / 4,096` |
| WAL SHA-256 | `2490a39765c288d5d40858d82ca6447fbc951cd2e2c656dcd90406859b8c6278` |

### 壓測設定與結果

| 指標 | 結果 |
| --- | ---: |
| Workload | `wal_write_ceiling` |
| Group／prepare workers／threshold | `8,192 / 2 / 4,096` |
| Sync／segment size | `none / 4 GiB` |
| Warmup／measured groups | `4 / 2,048` |
| Measured commands | `16,777,216` |
| Wall commands/s | `1,586,540` |
| Service commands/s | `1,780,090` |
| Append p50／p99／p99.9／max | `4,602.928 / 5,922.420 / 8,944.826 / 21,540.5 us` |
| Measured WAL bytes | `2,046,820,352` |
| Measured user／system CPU seconds | `14.9117 / 1.82237` |
| Measured voluntary／involuntary context switches | `5,736 / 658` |

### 第二點資源觀測與判讀

Observer 覆蓋 setup、measured window 與 replay，數據是資源佐證，不是 phase attribution：

| 指標 | 結果 |
| --- | ---: |
| Affinity CPU 平均 idle／最低 sample idle | `78.912% / 0%` |
| Workload iowait | `0%` |
| sdb 平均／最大 `%util` | `15.431% / 97.900%` |
| sdb 平均／最大 `aqu-sz` | `1.667 / 12.940` |
| GNU time 全程 user／system／elapsed | `24.90 / 9.34 / 32.08 s` |
| GNU time max RSS | `9,567,284 KiB` |

相較第一點單輪結果，第二點 spot run 的 service throughput 約提升 `28.2%`，append p50／p99 分別
降低約 `22.1%`／`13.4%`；p99.9 與 max 分別降低約 `4.6%`／`6.0%`。結果支持「prepare buffer
representation 可能改善正常路徑成本」的方向，但尚未排除 run-to-run 環境差異。

## 限制與下一步

- 沒有 baseline／candidate 同環境配對，因此不能決定是否保留 `deque` 修改。
- 第二點只有 candidate spot run，沒有同環境 paired baseline，因此不能以本報告單獨決定正式保留
  prepare buffer 修改。
- 沒有執行 perf、fio 或 `fsync` 實驗；本報告不延伸到 storage 理論頻寬或 durable ceiling。
- 若要做正式保留決策，下一輪只需在相同 affinity、filesystem、build flags 與 workload 下執行
  baseline／candidate 各一輪，並比較 WAL bytes identity、throughput、p99.9、max、CPU 與磁碟觀測。

原始 stdout、GNU time、preflight、observer 與 WAL artifacts 均保留於 run root。
