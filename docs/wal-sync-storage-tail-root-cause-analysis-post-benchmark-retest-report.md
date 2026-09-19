# WAL sync／storage tail post-benchmark 重測報告

## 1. 摘要與結論

本報告記錄 `docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-retest-procedure.md` 的
Phase A 結果。9 輪 component artifact 與 summary／CSV consistency 全部通過，但 sampler gate 失敗，
因此依規則沒有執行 Phase B，也沒有執行正式十輪矩陣。

| 項目 | 結果 | 判定 |
| --- | --- | --- |
| ReleaseBenchmark | 120/120 | pass |
| Debug | 92/92 | pass |
| ASan/UBSan | 92/92 | pass |
| targeted telemetry | 6/6 | pass |
| component artifact consistency | 9/9 | pass |
| collector bias | off 152,577；collector 151,223；0.887% | pass，`<=5%` |
| sampler increment | collector 151,223；full 170,835；12.969% | fail，`>5%` |
| full bias | off 152,577；full 170,835；11.966% | fail，`>5%` |
| Phase B calibration | not run | 依 gate 停止 |
| 正式矩陣 | 0/10 | not run，並非漏跑 |
| application sync tail | supported at application-level correlation | 尚不能定位 device／syscall |
| device/storage | inconclusive | 有同期粗粒度 tail，但無精確 timestamp 對齊 |
| filesystem/syscall | not run | gate 失敗，未執行 strace |
| queue/publisher pressure | inconclusive | 只有 W2 full state，未形成正式矩陣證據 |
| parallel-prepare interaction | inconclusive | 沒有 W1/W2 正式配對矩陣 |

collector-only 相對 off 的 median bias 僅 0.887%，目前沒有證據顯示 raw sync／group collector 本身造成
主要退化。full 相對 collector 的 12.969% 差異超過量測 gate；但 full 在本次 median 反而較快，不能
直接把差異宣稱為 sampler 的固定 overhead，也不能排除 storage tail 與輪次順序交互影響。

本次最小結論是：WAL application-level sync latency tail 仍與低吞吐輪次同時出現；然而 full telemetry
尚未通過 calibration，不能用這組結果產生正式 p50／p99／p99.9 aggregate，也不能進行 production
durability、group delay 或 prepare default 變更。

## 2. 執行身份與環境

- 量測日期：2026-09-19（Asia/Taipei）。
- Run root：`/home/neojhou/wal-sync-tail-post-review-cmo5BD7h`。
- Host/kernel：Linux `6.17.0-35-generic`，x86_64。
- CPU：AMD Ryzen 7 3700X，8 cores／16 threads，boost enabled。
- CPU affinity：`taskset -c 2-7`。
- Governor：`powersave`。
- Filesystem：`/dev/sdb2`、ext4、`rw,relatime`；約 468 GiB available。
- Compiler/build：GNU C++ 13.3.0、ReleaseBenchmark、C++20、`-m64 -O3 -DNDEBUG`。
- Workload：單一 instrument、單一 shard、`engine_durable_single_instrument`。
- Fixed parameters：group size 4096、group delay 1000 us、producer lanes 8192、parallel threshold 4096、
  warmup 10000、W=2。
- Source HEAD：`10471350f190282b42e441d76c40e641461c59a6`。
- index diff SHA-256：`642c528bc70586ac40d1f269aef73f0a5aeeeea50e8364c2edff8de0c2b8f00a`。
- worktree diff SHA-256：`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
- retest procedure SHA-256：`46f9fa916e6e094ff667ac6970025714fa8ef379d1f56c21461b0027306e4748`。
- benchmark binary SHA-256：`55c8336d34bba0a730bf42a7c5f0721c71935a4fc7ca235786bcfab600e8162f`。
- Tools：`/usr/bin/time`、`taskset`、`iostat`、`pidstat`；均可用。
- 外部監測：所有 9 輪均有 iostat 與 process-I/O artifact。

`source-identity-before.txt` 與 `source-identity-after.txt` 的 `diff` 為空，身份一致性通過。after identity
是在建立本報告前擷取；本報告本身不包含在該次 before／after 比對中。全程沒有執行會改變 staging 的命令。

另有一個未納入統計的操作 invalid attempt：`/home/neojhou/wal-sync-tail-post-review-J7nnjdHY` 的
benchmark process 全部 exit 0，但執行端 summary parser quoting 錯誤，候選 iterations 變成 `inf`；該
run root 保留，沒有覆寫或合併至本報告。

## 3. Correctness、smoke 與 pilot

### 3.1 Correctness

ReleaseBenchmark、Debug、ASan/UBSan 全量測試與 targeted telemetry CTest 均通過。ReleaseBenchmark 的
telemetry full／collector-only smoke 已驗證 drain boundary、summary／CSV first-last、collector-only 不含
state row，以及 `telemetry_dropped_samples=0`。

### 3.2 Component pilot

三種模式均使用 W=2、相同參數與外部監測，pilot 為 400,000 iterations：

| Mode | Elapsed ms | RPS | Candidate iterations for 60 s |
| --- | ---: | ---: | ---: |
| off | 4,727.38 | 169,227 | 5,076,808 |
| collector | 4,563.93 | 175,287 | 5,258,626 |
| full | 4,642.63 | 172,316 | 5,169,484 |

三者取最大值，Phase A 使用共同 `COMPONENT_ITERATIONS=5,258,626`。pilot 不納入 bias。

## 4. Phase A component calibration

### 4.1 逐輪 workload 與 sync 統計

每輪均為 5,258,626 iterations、10,517,252 commands；elapsed 均至少 60 秒，且 exit status 為 0。
Sync total 單位為 ms；off 沒有 collector，因此填 `N/A`。

| Round/mode | RPS | Elapsed ms | Cmd p99 / p99.9 / max us | Sync p99 / max us | Sync total ms | >25 / >100 / >250 ms | Drain rows |
| --- | ---: | ---: | --- | --- | ---: | --- | ---: |
| r1/off | 173,489 | 60,621.8 | 84,546.5 / 526,323 / 819,123 | N/A | N/A | N/A | N/A |
| r1/collector | 151,223 | 69,548.2 | 341,702 / 532,382 / 805,459 | 151,462 / 197,858 | 16,409.875 | 68 / 56 / 0 | 0 |
| r1/full | 172,094 | 61,113.3 | 90,181.3 / 541,574 / 805,302 | 11,236 / 322,552 | 8,206.940 | 8 / 1 / 1 | 3 |
| r2/full | 150,495 | 69,884.2 | 317,861 / 589,026 / 818,044 | 134,405 / 297,980 | 16,160.288 | 71 / 57 / 1 | 2 |
| r2/off | 152,577 | 68,930.6 | 310,998 / 578,342 / 842,996 | N/A | N/A | N/A | N/A |
| r2/collector | 171,941 | 61,167.7 | 83,274.9 / 586,189 / 810,160 | 12,281 / 50,998 | 7,938.984 | 7 / 0 / 0 | 0 |
| r3/collector | 146,986 | 71,552.7 | 301,763 / 588,985 / 834,942 | 134,862 / 213,246 | 17,521.673 | 86 / 55 / 0 | 0 |
| r3/full | 170,835 | 61,563.9 | 87,768.6 / 643,263 / 958,236 | 11,752 / 59,804 | 8,091.638 | 10 / 0 / 0 | 3 |
| r3/off | 146,227 | 71,924.0 | 304,731 / 656,549 / 944,887 | N/A | N/A | N/A | N/A |

### 4.2 State／drain evidence

Full mode measured queue and drain values were:

| Case | Queue max | Drain rows | Drain first events / bytes / age ns | Drain last events / bytes / age ns |
| --- | ---: | ---: | --- | --- |
| r1/full | 8,192 | 3 | 7,379,931 / 900,351,582 / 42,904,811,416 | 7,379,931 / 900,351,582 / 42,904,811,416 |
| r2/full | 8,188 | 2 | 7,493,734 / 914,235,548 / 43,877,228,079 | 7,493,734 / 914,235,548 / 43,877,228,079 |
| r3/full | 8,192 | 3 | 7,455,905 / 909,620,410 / 43,874,670,062 | 7,455,905 / 909,620,410 / 43,874,670,062 |

所有 full row 的 summary first／last 與 CSV 第一／最後 drain row 完全一致；collector-only 的 state／drain
row count 均為 0。Drain lag 在這三輪沒有顯示收斂，然而這只是 W=2 component 輪，不能替代正式 queue/
publisher pressure 判定。

### 4.3 RPS median 與 component gate

| Mode | RPS values | Median | Min--max |
| --- | --- | ---: | ---: |
| off | 173,489；152,577；146,227 | 152,577 | 146,227--173,489 |
| collector | 151,223；171,941；146,986 | 151,223 | 146,986--171,941 |
| full | 172,094；150,495；170,835 | 170,835 | 150,495--172,094 |

```text
collector bias = abs(151,223 - 152,577) / 152,577 = 0.887%
sampler increment = abs(170,835 - 151,223) / 151,223 = 12.969%
full bias = abs(170,835 - 152,577) / 152,577 = 11.966%
```

collector bias 通過；sampler increment 與 full bias 失敗。由於 full median 高於 collector median，這個
gate 只能表示 instrumentation calibration 不合格，不能單獨證明 sampler 導致固定方向的 throughput
下降。

## 5. 外部資源與 storage 觀察

### 5.1 Process resources

`/usr/bin/time -v` 與每輪 process-I/O monitor 均有輸出。CPU 百分比是 wrapper process 的 wall-time
比例，不是單核 utilization；RSS 單位為 KiB。

| Case | CPU | Max RSS | Voluntary CS | Involuntary CS | Process-I/O lines |
| --- | ---: | ---: | ---: | ---: | ---: |
| r1/off | 129% | 4,498,068 | 895,435 | 29,948 | 78 |
| r1/collector | 113% | 4,483,252 | 856,420 | 32,559 | 88 |
| r1/full | 129% | 4,549,248 | 778,895 | 14,694 | 78 |
| r2/off | 116% | 4,497,088 | 922,658 | 30,471 | 87 |
| r2/collector | 129% | 4,495,600 | 912,616 | 25,998 | 79 |
| r2/full | 114% | 4,488,904 | 834,799 | 21,385 | 88 |
| r3/off | 111% | 4,618,552 | 896,894 | 21,976 | 90 |
| r3/collector | 110% | 4,486,692 | 892,077 | 32,780 | 91 |
| r3/full | 128% | 4,549,260 | 829,833 | 22,950 | 79 |

### 5.2 Device evidence

同輪 1 秒 iostat 的 `/dev/sdb2` 所在 `sdb` sample 範圍如下；CSV 沒有 absolute epoch anchor，因此不能把
任一 sync sample 指派給特定 iostat 秒。

| Aggregate | r_await max ms | w_await max ms | aqu-sz max | %util max |
| --- | ---: | ---: | ---: | ---: |
| 9 component runs | 39.72 | 43.54 | 10.31 | 101.50 |

slow component runs 同時出現較高 application sync total、iowait／await／queue 的觀察，但監測解析度與
application timeline 不足以建立 syscall 或 block-device 的因果鏈。因此 device/storage 維持
`inconclusive`，不能寫成已證明的 device bandwidth ceiling。

## 6. 未執行與限制

- Phase B W1/W2 off-full calibration：not run。
- 正式 W1/W2 五輪矩陣：0/10，not run。
- 獨立 `strace`：not run；不能宣稱慢 sample 對應 `fsync`、`fdatasync` 或 write syscall。
- `perf stat`：not run。
- Workload 是單機 closed-loop、單一 instrument／shard，沒有 production fixed-rate arrival。
- 10 ms application state 與 1 秒外部監測解析度不同，不能精確對齊單筆 sync 與 device sample。
- 只測 W=2，不能判定 W=1/W=2 parallel-prepare interaction。
- Full sampler calibration 未通過，full telemetry 結果不可作正式效能 aggregate。

## 7. 後續必要行動

本次不應進入正式矩陣，也不應修改 production durability、fsync、group delay 或 prepare default。下一步
只應針對 sampler calibration failure 設計最小診斷：

1. 保留本次 9 輪與所有 storage tail artifact；不以新輪覆蓋。
2. 釐清 full state sampler 與 collector-only 的 12.969% median 差異，至少確認 sampler 是否造成
   metrics contention 或改變 tail 分布。
3. 若需要修改 telemetry instrumentation，先更新設計與 procedure，再重新執行 Phase A 三模式 gate。
4. 只有三個 component bias 全部 `<=5%`，才可執行 W1/W2 calibration 與正式十輪矩陣。

