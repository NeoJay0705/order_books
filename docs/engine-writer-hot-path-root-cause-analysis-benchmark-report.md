# Engine Writer Hot Path 根因分析壓測報告

## 1. 摘要

本報告記錄 `engine_writer_hot_path_profile`、`engine_durable_single_instrument` 與
`wal_write_ceiling` 的受控壓測結果，用來判斷 Engine writer 與 WAL durable path 的相對成本。

本次共完成 35 輪有效正式 throughput run：

- writer hot path：20 輪；group=4,096／256，profile off/on 各五輪；
- Engine authoritative control：5 輪；group=4,096；
- direct WAL control：10 輪；group=4,096、`sync=per_group`，profile off/on 各五輪。

所有有效輪次均通過 workload correctness／replay 驗證，measured phase 均超過 15 秒。

主要結果如下：

| 測試 | Median throughput | 五輪範圍 |
| --- | ---: | ---: |
| Writer group=4,096, profile off | 160,995 commands/s | 160,102–164,453 |
| Writer group=4,096, profile on | 104,677 commands/s | 70,896–153,718 |
| Writer group=256, profile off | 74,159 commands/s | 55,603–78,783 |
| Writer group=256, profile on | 72,413 commands/s | 56,131–72,672 |
| Engine durable control, group=4,096 | 161,450 commands/s | 118,826–163,728 |
| Direct WAL, group=4,096, profile off | 440,665 commands/s | 427,518–443,211 |
| Direct WAL, group=4,096, profile on | 411,391 commands/s | 406,556–414,659 |

結論是：Direct WAL 的 durable ceiling 約 441K commands/s，Engine durable end-to-end 約
161K commands/s，Engine 約為 direct WAL 的 36.6%，兩者相差約 2.73 倍。因此目前不能把
Engine throughput 瓶頸單獨歸因於 `fsync`；WAL 與 Engine integration path 都需要分開處理。

## 2. 測試範圍與負載

### 2.1 Writer hot path

Workload：

```text
--workload=engine_writer_hot_path_profile
--engine-group-delay-us=1000
--engine-producer-lanes=8192
--warmup=10000
```

每個 writer iteration 產生兩筆 command。正式 measured command 數為：

- group=4,096：`iterations=1,680,000`，共 3,360,000 commands；
- group=256：`iterations=800,000`，共 1,600,000 commands。

每個 group/profile 組合執行五輪，profile off/on 使用完全相同的 workload 與 iterations。
off/on 交錯執行，以降低長時間執行造成的溫度與背景負載偏差。

### 2.2 Engine authoritative control

```text
--workload=engine_durable_single_instrument
--engine-group-size=4096
--engine-group-delay-us=1000
--engine-producer-lanes=8192
--iterations=1680000
--warmup=10000
```

此 workload 量測 queue、group commit、WAL append、group `fsync`、matching、invariant
validation 與 durable completion callback 的端到端結果。

### 2.3 Direct WAL control

```text
--workload=wal_write_ceiling
--wal-group-size=4096
--wal-sync=per_group
--iterations=2103
--warmup=100
```

每輪 measured commands 為 `2,103 × 4,096 = 8,613,888`。`per_group` 的 completion
boundary 是 group `fsync` 完成；每輪均重新開啟並 replay WAL。

## 3. 執行環境與可重現性

- 執行日期：2026-09-19；
- CPU：AMD Ryzen 7 3700X，8 cores／16 threads；
- CPU affinity：`taskset -c 2-7`；
- CPU governor：`powersave`；boost enabled；測試期間未修改 host 設定；
- OS：Linux 6.17.0-35-generic x86_64；
- filesystem：ext4，WAL 與 data directory 位於 `/dev/sdb2`；
- compiler：GCC 13.3.0；
- flags：`-std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`；
- binary SHA-256：
  `75f372ff7d37d38a95d4a0cb94f9b76db2af9896ab86a97511d5246b7879ef9a`；
- source HEAD：`b63c27e189835baa67b612385f16a2b019141b53`；
- index diff SHA-256：
  `71eae2ecb096c3a8c210d9f9d2d04e363f67916797e752c2ba740ffacc8b6b3b`。

每一輪使用新的空 data directory，未使用 tmpfs、overlay filesystem，也未清除 page cache。
原始環境紀錄位於：[environment.txt](/home/neojhou/order-books-writer-root-cause-runs-20260919-0104/logs/environment.txt)。

## 4. 正式 throughput 結果

### 4.1 Writer hot path

| Group | Profile | RPS median | RPS range | Elapsed median | p50 median | p99 median | p99.9 median | Max median |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4,096 | off | 160,995 | 160,102–164,453 | 20.870 s | 47.231 ms | 101.883 ms | 303.207 ms | 304.293 ms |
| 4,096 | on | 104,677 | 70,896–153,718 | 32.099 s | 49.867 ms | 465.348 ms | 784.456 ms | 786.985 ms |
| 256 | off | 74,159 | 55,603–78,783 | 21.575 s | 107.891 ms | 208.170 ms | 242.708 ms | 244.031 ms |
| 256 | on | 72,413 | 56,131–72,672 | 22.095 s | 110.230 ms | 210.112 ms | 238.858 ms | 239.371 ms |

所有 writer 正式輪次輸出 `correctness_verified=true`，profile-on 的 profiled input、accepted
與 completion count 均等於 measured commands。

### 4.2 Engine durable control

| 指標 | Median | 五輪範圍 |
| --- | ---: | ---: |
| commands/s | 161,450 | 118,826–163,728 |
| elapsed | 20.811 s | 20.522–28.277 s |
| p50 latency | 47.402 ms | — |
| p99 latency | 113.083 ms | — |
| p99.9 latency | 339.176 ms | — |
| max latency | 340.288 ms | — |
| actual commands/group | 4,092.57 | — |
| WAL throughput | 18.7844 MiB/s | — |

五輪均為 `active_orders=0`、`active_levels=0`、`fsync_mode=per_group`，且 durable WAL
sequence 與 replay 驗證通過。

### 4.3 Direct WAL control

| Profile | RPS median | RPS range | Group p50 median | Group p99 median | Group p99.9 median | Group max median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off | 440,665 | 427,518–443,211 | 7.920 ms | 16.575 ms | 371.320 ms | 809.773 ms |
| on | 411,391 | 406,556–414,659 | 8.488 ms | 17.307 ms | 373.413 ms | 790.022 ms |

Direct WAL 的 off median 為 44.07% 的 1M commands/s 目標；Engine control median 為
16.15% 的目標。所有 WAL 輪次均輸出 `replay_verified=true`。

## 5. Instrumentation bias

依設計使用：

```text
bias = abs(profile_on_median - profile_off_median) / profile_off_median
```

| Workload | Bias | 證據等級 |
| --- | ---: | --- |
| Writer group=4,096 | 34.98% | 不可用細分 phase 百分比作 production 決策 |
| Writer group=256 | 2.35% | 可作主要方向性證據，但仍需注意 I/O range |
| Direct WAL group=4,096 | 6.64% | 只能作方向性 phase attribution |

Writer group=4,096 的 profile-on 結果包含 70,896、97,517、104,677、153,216 與
153,718 commands/s，range 明顯不穩定。這表示逐筆 writer instrumentation 與/或當時
filesystem／排程狀態對結果有顯著影響；不能從其中選取較快的輪次作為代表。

## 6. Phase 觀測

### 6.1 Direct WAL profile-on

五輪 phase share 的 median：

| Phase | Share |
| --- | ---: |
| prepare | 41.04% |
| sync | 29.14% |
| publish | 15.21% |
| plan/copy | 12.27% |
| write | 1.58% |
| rotation | 0.067% |
| lock wait | 0.001% |
| unattributed | 0.686% |

由於 bias=6.64%，以上只能作方向性證據。它顯示 WAL 成本不只有 `fsync`：prepare 是最大
可解釋 phase，sync 次之，publish 與 plan/copy 也不可忽略。

### 6.2 Writer profile-on

這些數值不作 group=4,096 的 production 決策，只保留作後續 instrumentation 設計參考：

| Group | Sync | WAL append | Apply | Admission | Post-apply | Completion enqueue |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4,096 | 44.31% | 22.68% | 15.44% | 5.44% | 6.22% | 4.24% |
| 256 | 63.02% | 15.68% | 8.89% | 3.66% | 4.39% | 2.98% |

group=256 的 sync share 較高，與小 group 增加 group-fsync 固定成本的預期一致；但 group=256
的 throughput 受 I/O outlier 影響，不能單獨用一輪結果推導 SLO。

## 7. 外部觀測

### 7.1 `/usr/bin/time -v`

代表性 writer group=4,096、profile-off run：

- user time：24.30 s；
- system time：6.56 s；
- wall time：31.00 s；
- CPU utilization：99%；
- maximum RSS：1,548,088 KiB（約 1.55 GiB）；
- voluntary context switches：243,021；
- involuntary context switches：16,262；
- exit status：0。

### 7.2 Per-thread CPU

`pidstat -t` 能觀察到 benchmark main process 與 worker TID 的 CPU／system／wait 欄位；代表
run 中 main process 約 99–127% aggregate CPU，個別 worker sample 約 79–87% CPU。此資料用於
確認 worker 是否有 CPU 工作，不將單秒 sample 當作精確長期 CPU share。

### 7.3 perf 限制

`perf stat` 因主機 `perf_event_paranoid=4` 被拒絕，沒有修改 kernel/sysctl 或安全設定；因此
本報告不宣稱 cycles、instructions、cache-miss 或 hardware counter 結果。

## 8. 異常與排除規則

正式結果不因 throughput 較低而刪除。g4096 profile-on r4、g256 off r2、g256 on r5 與
Engine r5 都是 correctness 通過但 throughput 明顯較低的有效輪次，均保留在五輪 range 中。

壓測期間另有一次背景 controller 傳遞環境變數失敗，未啟動 benchmark；其後一次 retry
controller 在 run 中被終止。該 partial data directory
`writer-g4096-on-r2.ApjC6J` 不納入正式統計，也沒有把它當作成功或失敗性能樣本。

完整 raw data 位於：

```text
/home/neojhou/order-books-writer-root-cause-runs-20260919-0104/
```

本次資料目錄約 19 GiB；所有正式輪次的 log、WAL 與 environment metadata 均保留。

## 9. 結論與下一步

1. Direct WAL group=4,096、per-group fsync 的 durable ceiling 約 441K commands/s，尚未接近
   1M/s 目標；但 WAL `fsync` 不是唯一成本，prepare、publish 與 plan/copy 也占顯著比例。
2. Engine durable 約 161K commands/s，只有 direct WAL 的 36.6%；writer profile-off 與
   Engine control 幾乎一致（差約 0.28%），支持主要差距來自 Engine integration path，而非
   benchmark command count 定義不同。
3. group=256 的 throughput 約 74K/s，且 sync share 較高，說明較小 group 會放大 durable
   group 固定成本。
4. Writer group=4,096 profile bias=34.98%，目前不能依 writer 細分 phase 百分比選擇
   parallel prepare、copy optimization 或其他 production change。
5. 下一步應先降低 writer profile 的 sampling／clock 成本，或在權限可用的主機使用外部
   profiler 重測；完成 bias 合格的 profile 後，才依 phase hierarchy 決定下一個 production
   optimization。

本報告只記錄量測結果與根因證據，沒有修改 WAL 格式、durability boundary、Engine ordering、
group-commit defaults、StateMachine、Publisher replay、Completion batching 或公開 API。
