# WAL bounded parallel prepare 壓測結果

日期：2026-09-19。測試依 `docs/wal-bounded-parallel-prepare-benchmark-procedure.md`
執行；未執行任何會改變 Git staging 的操作。

## 1. 摘要與最終判定

主要 gate 是 writer group=4,096、profile-off、W=4 相對 W=1 的五輪 median
throughput。結果如下：

| Gate | 結果 |
| --- | --- |
| Writer g4096 W=1 median | 151,044 commands/s |
| Writer g4096 W=2 median | 173,129 commands/s（+14.62%） |
| Writer g4096 W=4 median | 184,215 commands/s（+21.96%） |
| Writer g4096 W=4 uplift gate | 通過（門檻 +10%） |
| Correctness／replay | 所有納入輪次通過 |

W=4 的 p99 median 為 116,051 us，低於 W=1 的 271,387 us；但 W=4 第 5
輪出現真實 storage tail（967,789 us），不能把 tail 排除或宣稱 latency 已完全改善。
W=4 的 p99 在五個配對輪次中有兩輪超過 W=1 的 110%，因此沒有證據顯示穩定的
p99 regression；仍應以 production configuration 試驗確認。

Completion queue 的 sampled depth 約 4,091--4,096，queue p99.9 約 7--11 ms，
沒有觀察到隨輪次持續增加的現象。不過目前 benchmark 沒有 publisher lag time-series，
不能宣稱 publisher 永無短暫 backlog。

**結論：通過 prototype gate，進入獨立 production configuration 設計。**

這只證明 bounded parallel prepare 在目前測試機、group=4,096、per-group fsync
下有 prototype throughput uplift；不可解讀為已達到 1M commands/s。Production 設計仍須
明確配置 CPU budget、queue/backpressure、publisher lag 監測與 rollback 條件。

## 2. Artifact 與環境

| 項目 | 值 |
| --- | --- |
| Host／kernel | `master`, Linux `6.17.0-35-generic` |
| CPU | AMD Ryzen 7 3700X，16 logical／8 physical，boost enabled |
| CPU affinity | `taskset -c 2-7`；governor `powersave` |
| Filesystem | `/dev/sdb2`, ext4, `rw,relatime`；非 tmpfs／overlay |
| Compiler／flags | GCC 13.3.0，C++20，Release `-O3 -DNDEBUG -Wall -Wextra -Wpedantic` |
| HEAD | `7c20b39e58fd086f87752147a2badd466ad9e93d` |
| Index diff SHA-256 | `3d54664670939a09760de9fede6921d98246054e84b096b4228171b489f81322` |
| Worktree diff SHA-256 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| Procedure doc SHA-256（identity check 前） | `475557743eb499aabf043434e32b5b876a93fc119f19696b8a469260e16548f4` |
| Benchmark binary SHA-256 | `73495d2b8399ea98ac809bc93abc121afa2c0ec92df6953030123c5311a55e6b` |
| RUN_ROOT | `/home/neojhou/wal-parallel-prepare-benchmark-dLqtCe4z` |
| 可用空間（開始時） | 約 531 GiB |
| 工具限制 | 系統 PATH 沒有 cmake／ninja／ctest，使用既有 `/tmp/order_books-tools/bin/` wrappers；未改 sysctl 或 I/O 設定 |

Final identity check 在建立本報告前完成；HEAD、index/worktree diff、procedure doc 與
benchmark binary 均與開始正式矩陣時相同。本報告檔案本身是在 identity check 後建立，
因此不納入上述 worktree identity。

## 3. Build、correctness、smoke 與 calibration

### 3.1 Build/test gate

- Release CTest：`102/102` passed。
- Debug CTest：`80/80` passed。
- ASan/UBSan CTest：`80/80` passed，未出現 sanitizer error。
- TSan：本次沒有可用的獨立 TSan build；既有環境曾回報
  `FATAL: ThreadSanitizer: unexpected memory mapping`，所以不宣稱 TSan 通過，也不將它混入效能結論。

### 3.2 Smoke

| W | RPS | actual parallel groups | actual tasks | correctness |
| ---: | ---: | ---: | ---: | --- |
| 1 | 161,005 | 0 | 6 | true |
| 2 | 193,778 | 5 | 10 | true |
| 4 | 195,510 | 5 | 20 | true |

### 3.3 Sampling calibration（N=8）

依 ABBA 順序各三輪；`profiled_groups=104` 且
`profiled_commands == completion_profiled_commands`。

| W | Sample every | Off median RPS | On median RPS | Bias | Profiled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 8 | 159,699 | 159,063 | 0.398% | 104 | pass，採 N=8 |
| 4 | 8 | 179,994 | 179,647 | 0.193% | 104 | pass，採 N=8 |

N=16／N=32：`not run`，因 N=8 已符合 bias <=5%。W=4 calibration 的 on-r2
雖有一次 fsync tail outlier（102,120 RPS），仍保留在 calibration raw data；median bias
仍低於門檻。

## 4. Pilot 與固定參數

| Workload | Group | Pilot iterations | W1/W2/W4 elapsed ms | Formal iterations | 最短 formal elapsed ms |
| --- | ---: | ---: | --- | ---: | ---: |
| Writer | 4,096 | 400,000 | 4,995.3 / 4,660.84 / 4,341.12 | 1,700,000 | 18,303.1 |
| Writer | 256 | 400,000 | 11,446.6 / 10,410.6 / 10,998.5 | 800,000 | 20,280.6 |
| Direct WAL | 4,096 | 300 groups | 3,069.39 / 2,536.90 / 9,251.66 | 2,400 groups | 17,533.3 |

Direct WAL 曾先以 700 與 1,000 groups 試跑，但 W=1 elapsed 分別約 13.7 秒與
9.9 秒，未達 15 秒；該批資料全部排除。依程序以 pilot 公式重新選定 2,400 groups，
從 r1 重跑完整矩陣。

所有正式輪固定：group delay 1,000 us、producer lanes 8,192、threshold 256、
per-group fsync、W=1/2/4、每 case 五輪、全新 data directory、CPU 2--7。

## 5. 正式 profile-off throughput 與 latency

Latency 單位為 us；Direct WAL 欄位是 **group latency**，不是 command latency。
`p50`／`p99` 為五輪 median；`p99.9`／`max` 為五輪最差值。

| Workload | Group | W | RPS median | RPS min--max | Uplift vs W1 | p50 median | p99 median | Worst p99.9 | Worst max |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Writer | 4,096 | 1 | 151,044 | 106,583--162,108 | baseline | 48,144.6 | 271,387 | 487,383 | 491,104 |
| Writer | 4,096 | 2 | 173,129 | 98,827.2--178,663 | +14.62% | 43,298.3 | 100,614 | 578,066 | 581,957 |
| Writer | 4,096 | 4 | 184,215 | 62,204.9--185,761 | +21.96% | 40,680.7 | 116,051 | 1,371,130 | 1,372,690 |
| Writer | 256 | 1 | 74,406.5 | 69,353.7--78,893.3 | baseline | 107,195 | 202,968 | 490,453 | 511,333 |
| Writer | 256 | 2 | 76,268.1 | 55,829.7--77,559.7 | +2.50% | 105,808 | 209,298 | 6,412,650 | 6,711,140 |
| Writer | 256 | 4 | 77,406.5 | 61,468.8--78,186.8 | +4.03% | 104,857 | 200,073 | 3,796,210 | 3,816,190 |
| Direct WAL | 4,096 | 1 | 417,094 | 416,281--419,235 | baseline | 5,828.234 (group) | 17,200.398 (group) | 377,532.780 (group) | 838,975.667 (group) |
| Direct WAL | 4,096 | 2 | 496,622 | 461,440--501,180 | +19.07% | 4,422.366 (group) | 14,645.360 (group) | 376,858.941 (group) | 828,251.016 (group) |
| Direct WAL | 4,096 | 4 | 469,582 | 294,396--560,670 | +12.58% | 3,431.047 (group) | 95,048.294 (group) | 603,370.348 (group) | 830,203.691 (group) |

Public Engine W=1（不帶 prototype options）另列，五輪 RPS median 103,887、範圍
99,799.7--158,456；p50 median 48,426.9 us、p99 median 451,291 us、worst p99.9
1,178,300 us、worst max 1,179,770 us。它不是 internal writer profile 的同一 workload，
沒有混入上述 gate。

## 6. 實際路徑與資源成本

`/usr/bin/time -v` 的 CPU 可超過 100%，因為 W=2／4 有多執行緒。數值為五輪
median（括號為 min--max）。

| Workload／group | W | Actual commands/group | Parallel groups（五輪） | Prepare tasks（五輪） | CPU % | Vol CS | Invol CS | Max RSS KiB |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| Writer g4096 | 1 | 4,086.02--4,090.91 | 0（每輪） | 831（每輪） | 113（80--122） | 307,750（284,218--350,829） | 7,323（5,522--10,969） | 1,541,548（1,443,972--1,557,120） |
| Writer g4096 | 2 | 4,086.02--4,090.91 | 831（每輪） | 1,662（每輪） | 129（78--130） | 253,008（241,070--268,367） | 12,209（8,814--17,118） | 1,547,772（1,536,532--1,554,696） |
| Writer g4096 | 4 | 4,086.02--4,090.91 | 831（每輪） | 3,324（每輪） | 134（46--136） | 233,919（220,187--273,213） | 8,893（4,675--14,790） | 1,505,964（1,448,648--1,604,872） |
| Writer g256 | 1 | 255.965 | 0（每輪） | 6,250（每輪） | 64（40--67） | 166,943（152,042--170,380） | 6,828（5,667--13,410） | 943,436（899,376--944,420） |
| Writer g256 | 2 | 255.965 | 6,250（每輪） | 12,500（每輪） | 66（37--68） | 194,694（186,116--198,891） | 5,879（2,974--8,786） | 944,640（892,776--945,576） |
| Writer g256 | 4 | 255.965 | 6,250（每輪） | 25,000（每輪） | 68（40--69） | 228,998（222,569--236,908） | 5,042（2,875--7,285） | 942,128（941,652--943,676） |
| Direct WAL g4096 | 1 | 4,096 | 0（每輪） | 2,400（每輪） | 80（58--81） | 9,269（9,217--9,319） | 1,254（1,153--1,412） | 4,232,224（4,232,152--4,232,404） |
| Direct WAL g4096 | 2 | 4,096 | 2,400（每輪） | 4,800（每輪） | 77（62--94） | 16,392（16,362--16,480） | 2,055（1,725--2,120） | 4,232,404（4,232,056--4,232,436） |
| Direct WAL g4096 | 4 | 4,096 | 2,400（每輪） | 9,600（每輪） | 67（48--101） | 30,518（30,203--30,999） | 2,431（1,621--2,646） | 4,232,576（4,232,468--4,232,696） |

WAL／rotation invariants：

- Writer g4096 每輪 `wal_size_bytes=417,240,044`、約 836--837 commits、
  `wal_group_commands=3,420,000`；W=2／4 的 actual parallel groups/tasks 分別為
  831/1,662 與 831/3,324。
- Writer g256 每輪 `wal_size_bytes=197,640,022`、6,329 commits、
  `wal_group_commands=1,620,000`；W=2／4 的 groups/tasks 為 6,250/12,500 與
  6,250/25,000。
- Direct WAL 每輪 `wal_bytes_delta=1,199,308,888`、4 segment rotations、
  `replay_verified=true`；W=2／4 每輪 groups/tasks 為 2,400/4,800 與 2,400/9,600。
- Direct WAL profile-on 每輪 `profiled_groups=2,400`、`profiled_commands=9,830,400`、
  `profiled_data_write_calls=2,404`，與 replay 結果一致。

## 7. Profile-on phase 診斷

### 7.1 Writer g4096（ns/command，五輪 median）

| W | Profile bias | Writer service | Prepare wall | Prepare task* | Encode | CRC | Frame assembly | Plan/copy | WAL write | Publish | Sync | StateMachine apply | Publisher notify | Completion enqueue |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.398% | 5,934.08 | 1,158.68 | 1,158.20 | 528.244 | 250.339 | 199.452 | 654.168 | 40.533 | 425.904 | 799.134 | 1,359.40 | 0.688 | 397.872 |
| 4 | 0.193% | 4,714.54 | 388.528 | 1,169.10 | 555.490 | 259.068 | 209.400 | 198.655 | 41.242 | 420.782 | 732.238 | 1,367.11 | 0.661 | 370.695 |

\* `Prepare task` 是平行 task elapsed 的重疊總和，不是 end-to-end latency，也不可與
`Prepare wall` 相加。W=1／W=4 的 profile hierarchy 均符合
`profiled_commands == completion_profiled_commands`。

Completion sampled metrics：

| W | Completion service RPS | Queue p50 us | Queue p99 us | Queue p99.9 us | Queue max us | Max depth |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2,582,550 | 2,891.2 | 5,861.68 | 6,992.98 | 7,238.43 | 4,096 |
| 4 | 2,540,030 | 2,994.64 | 7,404.67 | 8,734.30 | 8,809.73 | 4,096 |

`publisher_notify` 是 Engine writer 發出通知的成本，不是 Publisher worker service time；
本 benchmark 沒有直接輸出 publisher lag time-series。

### 7.2 Direct WAL g4096（phase share 與 ns/command）

Direct WAL 的 phase totals 以 9,830,400 measured commands 換算 ns/command；sync 與其他
phase仍是 group 內部觀測，不能當作單 command latency。

| W | Profile bias | Prepare wall | Prepare task* | Plan/copy | Write | Publish | Sync | Prepare share | Plan share | Write share | Publish share | Sync share |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8.27% | 1,072.76 | 1,072.64 | 264.58 | 38.20 | 370.09 | 805.57 | 41.95% | 10.07% | 1.46% | 14.30% | 31.52% |
| 4 | 16.55% | 372.22 | 1,181.43 | 268.95 | 38.44 | 370.74 | 680.42 | 21.60% | 15.17% | 2.17% | 21.17% | 38.84% |

\* Direct WAL `Prepare task` 同樣是平行 lanes elapsed 總和。Direct WAL profile-on/off
bias 分別為 W=1 8.27%、W=4 16.55%，均超過 5%，所以這些 nested phase 只能作方向性
歸因證據；throughput gate 沒有使用 profile-on。

## 8. 正式輪次有效性

所有下列 final formal runs 均 exit 0，writer 為 `correctness_verified=true`，direct WAL
為 `replay_verified=true`，elapsed >=15,000 ms，並納入統計。`r4-retry`／`r3-retry`
只是因為同名資料目錄或命令 setup 失敗後使用唯一 data directory，不是排除輪次。

### 8.1 Writer g4096 profile-off

| Run | Elapsed ms | RPS | p99 us | Actual groups/tasks | 納入 |
| --- | ---: | ---: | ---: | --- | --- |
| W1-r1 | 25,610.2 | 132,759 | 350,635 | 0 / 831 | yes |
| W2-r1 | 34,403.5 | 98,827.2 | 435,471 | 831 / 1,662 | yes |
| W4-r1 | 18,456.7 | 184,215 | 104,614 | 831 / 3,324 | yes |
| W4-r2 | 18,303.1 | 185,761 | 116,051 | 831 / 3,324 | yes |
| W1-r2 | 31,900.0 | 106,583 | 347,338 | 0 / 831 | yes |
| W2-r2 | 19,638.6 | 173,129 | 100,049 | 831 / 1,662 | yes |
| W2-r3 | 31,099.9 | 109,325 | 515,640 | 831 / 1,662 | yes |
| W4-r3 | 19,161.4 | 177,440 | 122,055 | 831 / 3,324 | yes |
| W1-r3 | 20,973.7 | 162,108 | 94,886.9 | 0 / 831 | yes |
| W1-r4-retry | 22,509.9 | 151,044 | 271,387 | 0 / 831 | yes |
| W4-r4-retry | 18,351.8 | 185,267 | 98,336.2 | 831 / 3,324 | yes |
| W2-r4-retry | 19,480.9 | 174,530 | 100,614 | 831 / 1,662 | yes |
| W4-r5 | 54,658.1 | 62,204.9 | 967,789 | 831 / 3,324 | yes |
| W2-r5 | 19,030.2 | 178,663 | 88,109 | 831 / 1,662 | yes |
| W1-r5 | 21,451.3 | 158,499 | 111,602 | 0 / 831 | yes |

### 8.2 Writer g256 profile-off

| Run | Elapsed ms | RPS | p99 us | Actual groups/tasks | 納入 |
| --- | ---: | ---: | ---: | --- | --- |
| W1-r1 | 23,070.1 | 69,353.7 | 359,740 | 0 / 6,250 | yes |
| W2-r1 | 20,629.3 | 77,559.7 | 190,535 | 6,250 / 12,500 | yes |
| W4-r1 | 20,670.1 | 77,406.5 | 168,668 | 6,250 / 25,000 | yes |
| W1-r2 | 21,859.0 | 73,196.3 | 202,968 | 0 / 6,250 | yes |
| W2-r2 | 20,837.7 | 76,783.9 | 209,298 | 6,250 / 12,500 | yes |
| W4-r2 | 20,463.8 | 78,186.8 | 195,095 | 6,250 / 25,000 | yes |
| W4-r3 | 26,029.5 | 61,468.8 | 223,359 | 6,250 / 25,000 | yes |
| W2-r3 | 20,978.6 | 76,268.1 | 220,981 | 6,250 / 12,500 | yes |
| W1-r3 | 21,261.4 | 75,253.7 | 172,964 | 0 / 6,250 | yes |
| W1-r4 | 21,503.5 | 74,406.5 | 207,093 | 0 / 6,250 | yes |
| W2-r4 | 28,658.6 | 55,829.7 | 286,228 | 6,250 / 12,500 | yes |
| W4-r4 | 20,591.0 | 77,703.7 | 203,210 | 6,250 / 25,000 | yes |
| W4-r5 | 21,546.4 | 74,258.4 | 200,073 | 6,250 / 25,000 | yes |
| W2-r5 | 21,281.7 | 75,181.8 | 191,729 | 6,250 / 12,500 | yes |
| W1-r5 | 20,280.6 | 78,893.3 | 191,488 | 0 / 6,250 | yes |

### 8.3 Direct WAL g4096 profile-off

| Run | Elapsed ms | RPS | p99 group us | Actual groups/tasks | Replay | 納入 |
| --- | ---: | ---: | ---: | --- | --- | --- |
| W1-r1 | 23,614.4 | 416,288 | 17,497.511 | 0 / 2,400 | true | yes |
| W2-r1 | 19,689.2 | 499,278 | 14,645.360 | 2,400 / 4,800 | true | yes |
| W4-r1 | 17,790.6 | 552,561 | 13,829.347 | 2,400 / 9,600 | true | yes |
| W4-r2 | 20,934.4 | 469,582 | 95,048.294 | 2,400 / 9,600 | true | yes |
| W1-r2 | 23,614.8 | 416,281 | 16,692.749 | 0 / 2,400 | true | yes |
| W2-r2 | 19,614.5 | 501,180 | 14,984.283 | 2,400 / 4,800 | true | yes |
| W2-r3 | 19,794.5 | 496,622 | 14,633.754 | 2,400 / 4,800 | true | yes |
| W4-r3 | 17,533.3 | 560,670 | 14,950.708 | 2,400 / 9,600 | true | yes |
| W1-r3 | 23,566.6 | 417,133 | 16,834.393 | 0 / 2,400 | true | yes |
| W1-r4 | 23,448.4 | 419,235 | 17,200.398 | 0 / 2,400 | true | yes |
| W2-r4 | 21,303.7 | 461,440 | 19,385.887 | 2,400 / 4,800 | true | yes |
| W4-r4 | 24,032.9 | 409,040 | 196,765.625 | 2,400 / 9,600 | true | yes |
| W1-r5 | 23,568.8 | 417,094 | 18,026.457 | 0 / 2,400 | true | yes |
| W2-r5 | 19,855.2 | 495,104 | 14,520.279 | 2,400 / 4,800 | true | yes |
| W4-r5 | 33,391.8 | 294,396 | 167,527.826 | 2,400 / 9,600 | true | yes |

### 8.4 Profile-on 與 public control

Writer profile-on（W1/W4 各五輪）與 direct WAL profile-on（W1/W4 各五輪）全部 exit 0、
sample/replay counter 通過且納入 phase 診斷；public Engine W1 五輪全部 exit 0 且 elapsed
>=15 秒，納入 regression control。其 raw log 檔名與完整 `/usr/bin/time -v` 檔案均位於
RUN_ROOT；profile-on 的逐輪摘要如下：

| Case | RPS min--max | elapsed ms min--max | correctness/replay |
| --- | ---: | ---: | --- |
| Writer g4096 on W1 (5) | 155,512--159,752 | 21,283.0--21,863.3 | true |
| Writer g4096 on W4 (5) | 177,759--183,128 | 18,566.2--19,127.0 | true |
| Direct WAL g4096 on W1 (5) | 248,979--396,629 | 24,784.9--39,482.8 | true |
| Direct WAL g4096 on W4 (5) | 538,479--560,458 | 17,539.9--18,255.9 | true |
| Public Engine W1 (5) | 99,799.7--158,456 | 21,457.1--34,068.2 | process exit 0 |

排除項目：

- writer g4096 第 4 輪初次命令誤帶 `--writer-profile-sample-every` 到 profile-off，三個
  setup 立即回報 CLI error，沒有建立測量資料；以正確命令與唯一 retry directory 完成有效輪。
- direct WAL 的 700-group 與 1,000-group 初始矩陣因 elapsed <15 秒而全部排除；沒有與
  2,400-group 正式統計混合。
- 一次同名 data directory 殘留造成 setup error 的嘗試僅保留為操作紀錄；有效 retry 使用
  新目錄，未重用任何已寫入的 data directory。

## 9. Gate 與後續決策

| Gate | 門檻 | 結果 | Pass/Fail |
| --- | --- | --- | --- |
| Correctness | 所有 W 的 WAL bytes/order/replay/shutdown 一致 | writer correctness true；direct WAL replay true；bytes/rotations 一致 | Pass |
| Writer g4096 W4 uplift | >=10% | +21.96% median RPS | Pass |
| Writer W4 p99 | 多數配對輪不得穩定退化 >10% | 2/5 配對超過 110%；非穩定 regression，但 r5 有 967,789 us 真實 tail | Pass with tail warning |
| Completion backlog | 不得隨 run 持續累積 | sampled queue depth 未隨輪次上升；publisher lag 未直接量測 | Directional only |
| Profile bias | W1、W4 各 <=5% 才作主要 phase 證據 | Writer 0.398%／0.193%；Direct WAL 8.27%／16.55% | Writer pass；WAL directional only |

### 結論

bounded parallel prepare 在 group=4,096 的 prototype gate 通過，W=2/W=4 具有可重現的
median throughput uplift，且 correctness／replay 不變；group=256 的 uplift 僅約 2.5--4.0%，
顯示小 group 的固定成本與 storage tail 會吃掉平行化收益。Direct WAL 的 W=4 median 約
469.6k commands/s，仍低於 1M，並受 fsync tail 明顯影響。

下一步只能另案設計 production configuration：先固定 CPU 配額與 group policy，加入
publisher lag／queue depth time-series、fsync tail alert、backpressure 與 rollback，
再在目標硬體與實際 workload 重測。不能因本報告的 prototype gate 通過，就直接把 W=4
設為預設或宣稱達到 1M commands/s。

## 10. Raw artifacts

所有 benchmark stdout、status、`/usr/bin/time -v` 與每輪 data directory 位於：

`/home/neojhou/wal-parallel-prepare-benchmark-dLqtCe4z`

本報告未刪除慢輪或任何有效 raw artifact。
