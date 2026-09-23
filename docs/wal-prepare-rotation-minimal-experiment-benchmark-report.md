# WAL prepare／rotation 最小實驗報告

## 結論

- Result：`BELOW-TARGET`
- W=2、batch=8192 的 append-return wall throughput 為 `971,985 commands/s`，service
  throughput 為 `1,037,540 commands/s`，均低於 `1.53M commands/s` 參考門檻。
- 相對門檻差距：wall 低 `558,015 commands/s`（`36.47%`），service 低 `492,460 commands/s`
  （`32.19%`）。
- WAL prepare parallel path 確實啟用；measured window 內 CPU 使用集中於部分 affinity CPU，
  而 `sdb` util、queue 與 iowait 均很低，支持 CPU／同步 append 路徑是主要限制的假設。
- 本輪仍不能以 affinity-only observer 唯一歸因到特定函式；需要 perf 或額外分段實驗才能完成
  最終根因歸因。
- 本結果是 `standalone-observation`，不是正式 ceiling、production SLA 或 W=1／2／4 比較。

## 環境與有效性

| 項目 | 結果 |
| --- | --- |
| Run root | `/home/neojhou/wal-prepare-spot-u3wsOsHe` |
| Binary SHA-256 | `cfa7f33aab5572457e9fcef4c6889aff96fd9d5697380c72fe9d23e17a304fa9` |
| CPU affinity | `2-7` |
| WAL device | `sdb` |
| Preflight CPU idle | min `97.78%`，affinity 平均約 `98.80%` |
| Preflight CPU iowait | max `0.71%`，平均約 `0.32%` |
| Preflight disk util | avg `0.090%`，max `0.300%` |
| Preflight disk aqu-sz | avg `0.001`，max `0.010` |
| Preflight free space | 約 `132.0 GiB` |
| 殘留 benchmark／fio／strace | none |
| Exit status | `0` |
| replay | `replay_verified=true` |
| WAL byte plan | `wal_byte_plan_verified=true` |
| Rotation | `measured_segment_rotations=0` |
| Sync calls | `measured_wal_sync_calls=0` |
| Segment size | `4,294,967,296` bytes |
| Parallel prepare | `actual_parallel_prepare_groups=2048`，`actual_prepare_tasks=4096` |
| Marker | start/end 各一次，observer 對齊 measured window |

Preflight 在壓測前通過；沒有因環境 busy 而停止本輪。

## 固定條件

| 項目 | 值 |
| --- | ---: |
| Workload | `wal_write_ceiling` |
| WAL group size | `8192` commands |
| Prepare workers | `2` |
| Parallel prepare threshold | `4096` commands |
| WAL sync | `none` |
| No-rotation epoch | `16,777,216` commands |
| Warmup | `4` groups |
| Measured groups | `2,048` |
| Measured commands | `16,777,216` |

## 壓測結果

| 指標 | 結果 |
| --- | ---: |
| Wall commands/s | `971,985` |
| Service commands/s | `1,037,540` |
| Wall WAL throughput | `113.089 MiB/s` |
| Service WAL throughput | `120.716 MiB/s` |
| Wall measured elapsed | `17,260.8 ms` |
| Service elapsed | `16,170.2 ms` |
| Append p50 | `5,761.093 us` |
| Append p99 | `7,398.172 us` |
| Append p99.9 | `676,690.794 us` |
| Append max | `1,483,040 us` |
| WAL bytes | `2,046,820,352` |
| Average WAL bytes/command | `122` |
| WAL write calls | `2,049` |
| WAL sync calls | `0` |

## 資源觀測

### Measured append window counters

| 指標 | 結果 |
| --- | ---: |
| User CPU seconds | `19.2646` |
| System CPU seconds | `5.66601` |
| Voluntary context switches | `5,680` |
| Involuntary context switches | `703` |
| Dirty bytes before／after | `8,097,792`／`2,053,779,456` |
| Writeback bytes before／after | `0`／`0` |

### Whole-process `/usr/bin/time -v`

這些數值包含 WAL reopen/replay 與清理階段，不能當作純 append window：

| 指標 | 結果 |
| --- | ---: |
| User time | `29.07 s` |
| System time | `16.07 s` |
| CPU utilization | `109%` |
| Maximum RSS | `9,924,056 KiB` |
| Voluntary／involuntary context switches | `5,876`／`1,075` |

### Workload observer

observer 在 marker 之後啟動、marker end 之後停止；共取得 17 個 workload interval：

- affinity CPU 平均 idle：CPU 2=`69.79%`、CPU 3=`33.30%`、CPU 4=`59.80%`、CPU 5=`95.20%`、
  CPU 6=`97.94%`、CPU 7=`90.22%`；最低 `33.30%`，六 CPU 平均約 `74.38%`。
- affinity CPU iowait：所有樣本 `0.00%`。
- `sdb` 平均 util `0.153%`，最高 `1.00%`。
- `sdb` 平均 aqu-sz `0.0047`，最高 `0.04`。
- `sdb` write await 平均約 `0.49 ms`，最高 `2.00 ms`。

測量期間 dirty bytes 從約 `8 MiB` 增加到 `1.91 GiB`，但 writeback bytes 仍為 `0`；因此本輪
沒有 storage saturation 或 fsync 證據。CPU 使用集中於 CPU 2–4，與 append-return throughput
不足及高 p99.9 tail 同時出現，支持 CPU／同步 append 路徑優先調查，但尚不能定位到單一函式。

## 限制與後續

- 本輪只有 W=2 單輪，沒有 W=1 baseline、W=4 candidate 或五輪 CV。
- `sync=none`，本輪不提供 durable fsync ceiling；4 GiB zero rotation 也不回答 256 MiB rotation。
- 若要形成正式 ceiling，才在相同 preflight 與 binary identity 下補做 W=1／W=2／W=4 多輪比較；
  本輪結果已足以把 storage/fsync 從首要假設降級，不應直接修改 production WAL 設定。

原始輸出保留於 run root 的 `logs/`，資料目錄約 `2.0G`；確認不再需要 replay 證據後，才可
刪除該 run root 下明確的 `data/w2-b8192`。
