# WAL Durable Path 根因分析壓測報告

## 1. 結論摘要

本報告依據 `docs/wal-durable-path-root-cause-analysis-design.md` 的量測契約，對單一
shard、單一 instrument 的 WAL durable path 及 Engine durable path 進行受控壓測。

- 直接 WAL 的 `sync=per_group` 最高五輪 median 為 **465,991 commands/s**（group 8,192），
  約為 1,000,000 commands/s 目標的 **46.6%**；因此本次未達成百萬 durable commands/s。
- `sync=none` 的最高 median 為 544,133 commands/s。這是 append-return ceiling，不能當成
  RPO=0 的 durable throughput。
- group=4,096 的 instrumentation bias check 不通過：profile-on median 比 profile-off 低
  約 **9.1%**（494,474 vs. 544,133 commands/s）。因此 authoritative ceiling 採 profile-off，
  phase share 僅作診斷估計，不用 profile-on 數字宣稱 production ceiling。
- direct WAL group=4,096、`per_group` 的 median 為 412,817 commands/s；Engine durable
  median 為 162,780 commands/s，約相差 **2.54 倍**，五輪 throughput range 也不重疊。這支持
  WAL 之外仍有 writer integration 成本。
- 沒有任何 WAL phase 在可維持較大 group 的情況下持續占 append+sync wall time 過半：小 group
  的 sync 占比很高，大 group 則由 prepare、sync、publish 及其他工作共同構成。結論是
  **混合瓶頸**，不能把 `fsync` 宣稱為唯一根因。
- p99.9 tail 明顯受 storage/rotation outlier 影響；group=4,096、`per_group` 的 median
  約 379 ms，group=8,192 約 507 ms。若暫以 p99 <= 20 ms、p99.9 <= 50 ms 作參考，較大
  group 的 throughput 增益伴隨不可接受的 tail，不能只靠拉高 batch 參數解決。

## 2. 範圍與測試版本

本次只量測既有實作，不改變 WAL format、RPO=0 semantics、group-commit default、Engine
queue 或 Publisher 行為。測試包含：

1. `wal_write_ceiling` 的 `sync=none`／`sync=per_group` 矩陣，以及 profile off/on 對照。
2. `engine_durable_single_instrument` 的 group=4,096 對照。
3. `strace`、`/usr/bin/time`、`iostat` 及可用性檢查用的 `perf`。
4. Release correctness tests 與 ASan/UBSan tests。

測試基準為 HEAD `e72760ae368856b9316609231a98b8d08aca4532` 加上當時工作樹中的 staged changes。
本報告產生於 2026-09-18。

## 3. 執行環境

| 項目 | 實際值 |
| --- | --- |
| Compiler | GCC 13.3.0 (`c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1)`) |
| Build | Release；sanitizer correctness 為 Debug + ASan/UBSan |
| CPU | AMD Ryzen 7 3700X，8 cores / 16 logical CPUs |
| CPU affinity | `taskset -c 2-7` |
| Kernel | Linux 6.17.0-35-generic x86_64 |
| WAL filesystem | `/dev/sdb2`、ext4、`rw,relatime`，WAL 使用同一裝置 |
| WAL segment | 256 MiB (`268435456` bytes) |
| Cache/host policy | 未清 page cache；未修改 governor、mount option、I/O scheduler 或 kernel |
| perf permission | `perf_event_paranoid=4`，無法取得 perf counters |

每個 authoritative case 使用 fresh empty data directory、warmup 後進入 measured phase、
五輪重複。測試以 `seed=1` 執行；每輪 measured phase 約 15 秒以上。第一個
`g256-none-off` case 使用獨立明確 data directory，其餘 case 由 benchmark 以 `TMPDIR` 建立
並清理；所有目錄均為空目錄，不重用先前 WAL。

## 4. 建置、正確性與執行方法

Release benchmark binary 為：

```text
/tmp/order-books-wal-profile-build-tests/benchmarks/order_books_benchmark
```

WAL 矩陣每一輪的 workload 形狀為：

```text
--workload=wal_write_ceiling
--wal-phase-profile=off|on
--wal-sync=none|per_group
--wal-group-size=<group>
--warmup=<calibrated warmup>
--iterations=<calibrated measured groups>
```

所有正式 WAL case 均使用 100 個 warmup groups、五輪；profile on 與 off 使用相同 group count。
為讓 measured phase 達到約 20 秒，使用下列 iteration：

| sync | group=256 | group=1,024 | group=4,096 | group=8,192 |
| --- | ---: | ---: | ---: | ---: |
| none | 47,254 | 12,104 | 2,650 | 未正式執行 |
| per_group | 8,272 | 6,176 | 2,103 | 1,139 |

`none` 在 1,024→4,096 的 throughput uplift 已低於 5%，依設計規則停止後續 group=8,192；
group=4,096 仍完整執行，供 instrumentation bias check 使用。pilot 的 group=8,192、none
僅用於確認停止條件，不列入正式矩陣。

Engine 對照每輪使用 1,680,000 measured iterations（3,360,000 commands、1,680,000 trades）、
warmup=10,000、group=4,096、group delay=1 ms、producer lanes=8,192，執行五輪。

Release CTest 為 **82/82 passed**；正確設定後的 ASan/UBSan CTest 亦為 **82/82 passed**。
所有正式 WAL log 都有 `replay_verified=true`。`git diff --check` 與
`git diff --cached --check` 通過；本次沒有執行會改變 index 的操作。Clang 未安裝，故本報告
不宣稱 Clang build 通過。

## 5. WAL 壓測結果

以下 throughput 的 median、p50/p99/p99.9/max 都是五輪結果的 median；`throughput range`
是五輪的最小值至最大值。時間單位為 microseconds。`sync=none` 的 completion boundary
是 append return，`sync=per_group` 才是 group fsync durable boundary。

| group | sync | profile | groups/輪 | commands/s median | throughput range | group p50 | group p99 | group p99.9 | group max |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | none | off | 47,254 | 435,224 | 235,361–568,171 | 344.636 | 488.175 | 732.303 | 6,139,799.723 |
| 256 | none | on | 47,254 | 538,932 | 347,058–552,062 | 356.318 | 505.588 | 791.273 | 860,584.028 |
| 1,024 | none | off | 12,104 | 540,102 | 358,637–550,111 | 1,405.625 | 1,949.244 | 60,502.112 | 1,065,587.392 |
| 1,024 | none | on | 12,104 | 534,718 | 411,597–564,364 | 1,411.687 | 1,923.716 | 61,496.898 | 1,068,039.220 |
| 4,096 | none | off | 2,650 | 544,133 | 416,263–561,196 | 5,645.988 | 7,718.324 | 562,089.194 | 828,308.270 |
| 4,096 | none | on | 2,650 | 494,474 | 277,997–567,963 | 5,652.059 | 8,043.132 | 573,294.572 | 1,013,192.262 |
| 256 | per_group | off | 8,272 | 126,114 | 114,725–132,645 | 1,938.566 | 4,479.238 | 26,412.279 | 176,630.295 |
| 256 | per_group | on | 8,272 | 125,239 | 58,620.6–128,456 | 1,930.350 | 4,448.113 | 25,853.912 | 178,396.722 |
| 1,024 | per_group | off | 6,176 | 273,749 | 268,528–278,015 | 3,389.958 | 6,582.104 | 51,234.853 | 481,836.326 |
| 1,024 | per_group | on | 6,176 | 282,485 | 226,147–290,739 | 3,346.706 | 6,601.319 | 47,040.505 | 481,119.366 |
| 4,096 | per_group | off | 2,103 | 412,817 | 233,306–417,290 | 8,363.623 | 18,927.501 | 379,402.791 | 841,891.647 |
| 4,096 | per_group | on | 2,103 | 416,082 | 365,145–418,472 | 8,376.366 | 17,911.815 | 379,058.589 | 843,117.846 |
| 8,192 | per_group | off | 1,139 | 465,991 | 260,801–469,627 | 15,155.045 | 32,320.070 | 507,146.643 | 755,748.651 |
| 8,192 | per_group | on | 1,139 | 469,077 | 462,101–471,579 | 15,142.722 | 27,034.099 | 518,508.833 | 751,693.723 |

### 5.1 Group size 與暫定 latency 參考值

以 profile-off 的 durable rows 為 authoritative 結果：

- 256→1,024 的 `per_group` median 從 126,114 增至 273,749 commands/s，約 +117%。
- 1,024→4,096 約 +50.8%。
- 4,096→8,192 約 +12.9%，但 p99 從 18.9 ms 增至 32.3 ms，p99.9 從 379 ms 增至 507 ms。
- 以設計中的暫定 p99 <= 20 ms、p99.9 <= 50 ms 作參考，group=256 尚符合兩項、group=1,024
  的 p99.9 約 51.2 ms 已略超過，group=4,096 與 8,192 都不符合 p99.9；8,192 連 p99 也超過。

因此「提高 group size 以攤薄固定成本」在 throughput 上有效，但它會把 group 等待及 tail
放大；在尚未定義正式 latency SLO 前，不應直接把 8,192 設成 production default。

## 6. Profile phase 結果

以下是 profile-on 五輪 share 的 median。share denominator 是 WAL append call 加上 group
sync 的總時間，不包含 fixture construction；`sync=none` 沒有 sync sample，故為 `na`。lock
wait 與 unattributed remainder 未列在表內，不能把表內數字直接當成 100%。

| group | sync | prepare | plan/copy | rotation | write | publish | sync |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | none | 51.256% | 13.231% | 11.944% | 4.366% | 16.456% | na |
| 1,024 | none | 50.917% | 15.078% | 11.777% | 2.977% | 18.255% | na |
| 4,096 | none | 47.076% | 12.380% | 11.931% | 2.594% | 16.943% | na |
| 256 | per_group | 11.960% | 3.393% | 0.000% | 1.228% | 4.333% | 78.805% |
| 1,024 | per_group | 27.174% | 7.014% | 0.026% | 1.453% | 9.803% | 54.060% |
| 4,096 | per_group | 40.513% | 13.044% | 0.070% | 1.640% | 14.481% | 29.603% |
| 8,192 | per_group | 45.774% | 11.965% | 0.082% | 1.752% | 16.396% | 23.279% |

解讀如下：

- 小 group 的 sync share 過半（256 為 78.8%、1,024 為 54.1%），所以 sync/storage 是重要
  候選，但 device 指標沒有同步顯示飽和，不能判定為唯一根因。
- 4,096 與 8,192 的 prepare 分別約 40.5% 與 45.8%，是主要 userspace 成本，但未達設計
  「單一 phase 過半」的 confirmed 門檻。
- 大 group 沒有任何單一 phase 過半；prepare、sync、publish、plan/copy 的累積成本才是
  主要解釋。
- phase profile 因 bias check 不通過，只作 attribution；不使用 profile-on throughput 作
  production ceiling。

## 7. Engine durable 對照

五輪 Engine run 的固定 workload 為 3,360,000 commands、group=4,096、group delay=1 ms、
producer lanes=8,192。

| 指標 | median | 五輪 range |
| --- | ---: | ---: |
| commands/s | 162,780 | 161,361–166,742 |
| p50 latency (us) | 47,141.1 | 45,332.9–47,456.0 |
| p99 latency (us) | 102,324 | 93,872.1–114,844 |
| p99.9 latency (us) | 300,388 | 289,050–359,065 |
| max latency (us) | 302,909 | 289,933–361,097 |
| actual commands/group | 4,092.57 | 4,087.59–4,092.57 |
| WAL MiB/s | 18.9391 | 18.7741–19.4001 |

Engine 的 actual commands/group 接近 4,096，並非因 group 明顯填不滿造成表面差異。與 direct
WAL group=4,096、`per_group` 的 412,817 median 相比，Engine 只有約 39.4%，且兩者五輪
throughput range 不重疊。依設計規則，這支持 queue/admission/apply/completion 等 WAL 外的
writer integration 成本；不能把 162,780 直接歸因為 `fsync`。

## 8. 外部觀測

外部工具只用於歸因，沒有用來產生 authoritative throughput，因為工具本身會改變 wall time。
代表性 case 為 group=4,096、`sync=per_group`、warmup=100、measured groups=1,000。

### 8.1 strace

`strace -f -c -e trace=write,fsync,fdatasync,openat,close` 的 benchmark summary 為 263,927
commands/s、30.7075 MiB/s；統計到 1,106 次 write、1,110 次 fsync、40 次 close、52 次
openat（其中 12 次 error），共 2,308 次 syscall、0.077524 秒 traced time。

在 traced syscall time 中，write 約 65.09%、fsync 約 32.87%、close 約 1.50%、openat 約
0.54%。這表示目前 active-descriptor 實作的 syscall shape 以 write/fsync 為主；open/close
不是每一筆 command 都發生，因此不支持「每筆 WAL operation 都 open/write/close」作為目前
根因。strace 的比例不能直接轉換成 wall-time CPU 或 device time。

### 8.2 `/usr/bin/time -v`

summary 為 413,751 commands/s、48.1392 MiB/s；user time 8.81 s、system time 4.39 s、
wall time 16.50 s、CPU 80%。最大 RSS 約 2.2 GiB，voluntary context switches 4,104、
involuntary 594，filesystem outputs 1,082,432。這支持 userspace/kernel work 存在，但不
足以將成本細分成 codec、copy 或 fsync。

### 8.3 iostat

`iostat -xz 1 20` 同期觀測到 summary 約 416,948 commands/s、48.5112 MiB/s。WAL 所在
`sdb` 的觀測範圍約為：device `%util` 50.2–67.3%、`w_await` 0.4–1.02 ms、`aqu-sz`
0.53–1.75；同時有 replay 造成的讀取流量。裝置沒有呈現接近 100% utilization 或持續高
queue depth，因此目前沒有「純粹 device bandwidth/queue saturation」的證據。

### 8.4 perf

`perf stat -d` 以 `perf_event_paranoid=4` 執行後 exit code 255，權限不足。沒有修改 kernel
安全設定，也沒有偽造 call-stack 或 CPU counter 結果。

## 9. Instrumentation bias check

必要的 group=4,096、`sync=none` 五輪結果如下：

| profile | commands/s median |
| --- | ---: |
| off | 544,133 |
| on | 494,474 |

profile-on 相對 profile-off 下降：

```text
(544,133 - 494,474) / 544,133 = 約 9.1%
```

超過設計規定的 5% 門檻。因此：

1. authoritative ceiling 只採 profile-off。
2. phase share 是診斷估計，不用 profile-on throughput 與 off 直接比較宣稱優化收益。
3. 其他 profile-on row 的小幅 uplift（例如 group=256）視為 run-to-run noise，不能當成
   instrumentation 帶來的效能改善。

## 10. 根因判定

依設計第 9 節的門檻，證據強度如下：

| 候選 | 判定 | 證據 |
| --- | --- | --- |
| sync/storage | supported candidate，未確認唯一根因 | 小 group sync share 78.8%/54.1%，但 iostat utilization 約 50–67%、await 約 0.4–1.02 ms，外部證據不一致 |
| prepare（encode/CRC/framing） | supported major candidate，未達 confirmed | 大 group 約 40.5%/45.8%，接近但未超過過半門檻；無 perf stack 可交叉確認 codec/CRC |
| plan/copy | 未確認為唯一根因 | share 約 12–13%，未過半；偶發高 percentile 需另行分解，不能把 outlier 直接視為 steady-state |
| data write/device | 未確認為唯一根因 | strace 的 write 時間高，但 iostat 未顯示裝置飽和 |
| rotation | tail factor | rotation 次數相對 group 數很少；p99.9/max 有明顯長尾，適合另做 rotation latency 實驗 |
| Engine integration | supported major system-level cost | direct WAL 412,817 vs Engine 162,780，約 2.54 倍且五輪 range 不重疊，Engine actual group 約 4,092.6 |

因此本階段的正式結論是：**目前是 WAL userspace prepare、sync/storage、publish 與 Engine
writer integration 共同形成的混合瓶頸；沒有足夠證據把 fsync、device 或任何單一 phase 定為
唯一根因。**

## 11. 後續建議

建議依證據分兩條另案處理，並先補足 latency SLO：

1. **先量 Engine integration。** 以不改 production semantics 的診斷設計分開量 ingress/admission
   wait、writer queue、StateMachine apply、completion enqueue/callback 及 durable callback
   等待，確認 direct WAL 與 Engine 的 2.54 倍差距落在哪一段。
2. **再針對 WAL prepare 做小範圍實驗。** 先細分 encode/CRC、buffer planning/copy、index
   publication 的 userspace成本；不在本報告中直接改 codec、WAL format 或 durability policy。
3. **以 latency 約束選 group。** 目前 8,192 雖只有約 +12.9% throughput uplift，卻將 p99.9
   推到約 507 ms；在 p99 <=20 ms、p99.9 <=50 ms 的暫定參考下，不應先提高 production
   default。若業務接受較高 group delay，應另寫 group-commit tuning design 並重跑五輪矩陣。
4. **在實際部署硬體重跑。** 本報告僅代表本機 ext4 `/dev/sdb2`；不能外推到部署用 SSD、
   RAID、雲端 block device 或不同 kernel/mount policy。

## 12. 原始產物與限制

原始 logs、pilot、外部工具輸出位於：

```text
/tmp/order-books-wal-rcca-20260918
```

約 12 GiB；正式 WAL 共有 14 個 case、每 case 五輪，共 70 個 log，另有 Engine 五輪及外部
診斷檔。`strace`、`/usr/bin/time`、`iostat` 的數字只作交叉證據；sanitizer 只作 correctness
驗證，不產生效能結論。perf 權限受限，故沒有 CPU call-stack 證據。持續 backlog 測試的 tail
也不能直接代表低負載單次 command latency。

本報告檔案目前只寫入 working tree，沒有執行 `git add` 或其他會改變 staging 的操作；原有
staged changes 保持不變。
