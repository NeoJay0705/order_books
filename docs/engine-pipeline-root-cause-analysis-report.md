# Engine Pipeline 根因分析報告

## 1. 結論摘要

本報告依據 `docs/engine-pipeline-root-cause-analysis-design.md` 的正式矩陣，
在 Linux Release build、固定 CPU affinity 與同一 ext4 filesystem 上完成 invariant、metrics、
runtime handoff 與 Publisher drain 的五輪量測。

結論分類如下：

| 項目 | 分類 | 結論 |
| --- | --- | --- |
| invariant validation state-size scaling | `confirmed ceiling` | active orders 增加時 validation throughput 可重現地下降，100,000 orders 的 median 約 45.2 validations/s。 |
| shared MetricsRegistry | `supported cause` | shared registry median 約 435K command-equivalent/s，separate registry 約 1.703M，差異約 74.5%，五輪 range 不重疊。 |
| runtime handoff lane sweep | `unconfirmed hypothesis` | 測試範圍內 throughput 從 3.61K 上升至 944K callbacks/s；256→1024 lanes 仍提升約 23.5%，944K 只是目前量測上界，尚未形成平台。 |
| runtime queue／Completion 個別根因 | `unconfirmed hypothesis` | lane sweep 與 futex 觀測顯示協調成本存在，但沒有足夠證據把成本分配給特定 queue 或 Completion worker。 |
| Publisher drain | `confirmed ceiling` | 5,120-command backlog 的 sustainable median 為約 363 commands/s，遠低於 1M target。 |
| Publisher cursor／sync syscall | `supported cause` | traced run 中 `fsync` 占 syscall time 60.91%、`rename` 占 16.69%；仍需 before／after cursor batching 實驗才能確認因果 uplift。 |

本報告不選擇新的 production default，也不直接實作 invariant、metrics、queue 或 cursor
batching 優化。

## 2. 執行環境與有效性

### 2.1 Source、build 與主機

```text
source HEAD: f811ab8de14282e1124ca10b0c8822b53035e292
source state: HEAD plus staged root-cause benchmark implementation and unstaged review fixes
compiler: GCC 13.3.0
language/build flags: C++20, -O3, -DNDEBUG, -Wall, -Wextra, -Wpedantic, -Werror
build: Release, warnings-as-errors
binary: /tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark
CPU: AMD Ryzen 7 3700X, 8 cores / 16 logical CPUs
CPU affinity: CPU 0 for invariant; CPUs 0-1 for metrics; CPUs 0-3 for worker stages
CPU governor: powersave
kernel: Linux 6.17.0-35-generic #35~24.04.1-Ubuntu SMP PREEMPT_DYNAMIC
filesystem: ext4, /dev/sdb2, rw,relatime, mount point /
perf_event_paranoid: 4
```

`perf stat` 在此主機被 kernel policy 拒絕；沒有修改 sysctl 或其他安全設定。`strace`、
`iostat` 與 `/usr/bin/time -v` 僅作外部歸因觀測，不納入 baseline throughput。

### 2.2 共通 benchmark 參數

- `--pipeline-batch-size=256` 用於 invariant 與 metrics。
- runtime 固定 `--pipeline-batch-size=64 --engine-group-size=256 --engine-group-delay-us=200`。
- Publisher 固定 `--pipeline-batch-size=256 --iterations=20 --warmup=1`，每輪建立新的暫存
  data directory，measured backlog 為 5,120 commands。
- 所有正式 run 都回報 `correctness_verified=true`；count、callback、cursor 或 sink mismatch
  的 run 不納入結果。

### 2.3 排除的校準 run

校準時 invariant 使用 `iterations=80,000,000`（active orders=0）與 `30,000`
（active orders=1,000），measured duration 分別約 3.67 秒與 2.81 秒，低於設計要求的
5 秒，因此未納入正式五輪。後續正式 run 分別改用 120,000,000 與 60,000 iterations。

runtime lane=256 的初始第五輪為 `elapsed_ms=9969.1`，低於 10 秒，也未納入；該點以
130,000 iterations 重新完成五輪。

## 3. Invariant validation

正式 command template：

```bash
taskset -c 0 /tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=invariant_validation \
  --iterations=<calibrated> --warmup=1000 \
  --pipeline-batch-size=256 --pipeline-active-orders=<state-size>
```

### 3.1 五輪 raw key fields

下表每個欄位依序列出 run 1～5 的 stdout 值；所有 run 都是成功且
`correctness_verified=true`。

| active orders | iterations | elapsed_ms | validations/s | group p50_us | group p99_us |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 120,000,000 | 5527.88, 5610.90, 5515.28, 5494.74, 5459.56 | 21,708,134, 21,386,947, 21,757,723, 21,839,058, 21,979,793 | 0.040, 0.040, 0.040, 0.040, 0.040 | 0.080, 0.071, 0.071, 0.071, 0.071 |
| 1,000 | 60,000 | 5627.10, 5774.29, 5551.26, 5552.25, 5543.77 | 10,662.686, 10,390.883, 10,808.367, 10,806.426, 10,822.954 | 89.768, 92.243, 89.117, 89.497, 89.478 | 127.448, 116.879, 126.407, 120.476, 115.166 |
| 10,000 | 6,000 | 6191.57, 6102.23, 6166.74, 6049.10, 6075.45 | 969.059, 983.247, 972.961, 991.883, 987.581 | 995.405, 986.018, 994.264, 987.991, 981.910 | 1265.70, 1311.54, 1284.68, 1203.54, 1319.16 |
| 100,000 | 300 | 6602.29, 6596.14, 6655.86, 6692.56, 6516.62 | 45.439, 45.481, 45.073, 44.826, 46.036 | 21322.4, 21736.9, 21764.3, 22044.2, 21461.7 | 29273.3, 26366.0, 26739.5, 27453.0, 25428.6 |

### 3.2 Median、range 與攤提成本

`amortized_ns_per_command` 由同一 validation measured elapsed 推導：
`elapsed_ns / (validations × 256)`。這是 validation cost 的攤提值，不是完整 Engine
command latency。

| active orders | median validations/s | run range | median amortized ns/command | worst p99.9_us |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 21,757,723 | 21,386,947–21,979,793 | 0.18 | 0.10 |
| 1,000 | 10,806.426 | 10,390.883–10,822.954 | 361 | 153.257 |
| 10,000 | 983.247 | 969.059–991.883 | 3,973 | 1,723.14 |
| 100,000 | 45.439 | 44.826–46.036 | 85,967 | 31,501.8 |

### 3.3 判讀

validation cost 隨 state size 呈現可重現的非線性放大：從 1,000 到 100,000 active orders，
median validations/s 約下降 239 倍。這足以把完整 invariant scan 標示為
`confirmed ceiling` 與 production optimization candidate；但本報告沒有 completion latency
SLO，不能僅由此選擇新的 validation 或 batch default。

## 4. Metrics registry 對照

正式 command：

```bash
taskset -c 0-1 /tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling --pipeline-stage=metrics \
  --iterations=40000 --warmup=1000 --pipeline-batch-size=256
```

每次 invocation 依序輸出 `writer_only`、`writer_publisher_separate_registries` 與
`writer_publisher_contended`；五次 invocation 共 15 個成功 case。

| case | command-equivalent/s median | 五輪 range | elapsed median_ms | writer commands/s median (range) | publisher commands/s median (range) | group p99 range_us | group p99.9 range_us |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `writer_only` | 1,768,080 | 1,741,870–1,811,140 | 5,791.58 | 1,768,080 (1,741,870–1,811,140) | N/A | 214.953–249.889 | 248.686–269.806 |
| `writer_publisher_separate_registries` | 1,702,830 | 1,680,810–1,728,370 | 6,013.53 | 1,702,837 (1,680,821–1,728,380) | 3,159,279 (3,011,135–3,328,842) | 198.603–216.446 | 212.518–260.849 |
| `writer_publisher_contended` | 434,826 | 373,511–447,199 | 23,549.60 | 434,827 (373,540–447,239) | 507,483 (420,762–521,183) | 1,221.17–1,489.00 | 1,479.39–1,765.91 |

所有 metrics case 都輸出：

```text
correctness_verified=true
contention_mode=<single_worker|separate_registries_two_workers|shared_registry_two_workers>
writer_metric_calls_per_command=11
publisher_metric_calls_per_command=5
```

shared 相對 separate 的 median throughput 下降：

```text
1 - 434,826 / 1,702,830 = 74.5%
```

兩組五輪 throughput range 不重疊，超過設計要求的 20% 判定門檻。因此 shared
`MetricsRegistry` lock／registry ownership contention 是 `supported cause`。這個結果只代表
benchmark metric mix，不等同完整 production command throughput，也不證明 downstream exporter
成本。

## 5. Runtime handoff／Completion

正式 command 固定：

```bash
taskset -c 0-3 /tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling --pipeline-stage=runtime_handoff \
  --iterations=<calibrated> --warmup=100 --pipeline-batch-size=64 \
  --pipeline-producer-lanes=<lanes> --engine-group-size=256 \
  --engine-group-delay-us=200
```

### 5.1 五輪摘要

| producer lanes | iterations | callbacks/s median | 五輪 range | median p50_us | median p99_us | worst p99.9_us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 600 | 3,613.56 | 3,609.71–3,620.96 | 268.664 | 288.151 | 1,119.13 |
| 8 | 5,000 | 28,727.5 | 28,678.4–28,902.0 | 267.401 | 294.261 | 1,385.37 |
| 64 | 40,000 | 224,636 | 223,791–226,147 | 239.90 | 308.048 | 1,226.99 |
| 256 | 130,000 | 764,614 | 759,856–768,592 | 221.966 | 375.404 | 705.102 |
| 1,024 | 150,000 | 944,038 | 933,998–959,687 | 230.402 | 461.746 | 1,816.72 |

每輪均驗證 `queued == callbacks`，且每個 callback 都是預期的 `invalid_command` admission
error；沒有 duplicate、identity mismatch 或 ordering failure。

### 5.2 外部 worker 觀測

以下是每個 lane 各一筆代表性 `/usr/bin/time -v` 觀測；不混入五輪 throughput baseline。

| producer lanes | benchmark elapsed_ms | process wall_s | user_s | system_s | CPU | voluntary context switches | involuntary context switches |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10,617.5 | 12.40 | 0.44 | 0.65 | 8% | 179,704 | 677 |
| 8 | 11,118.6 | 11.36 | 0.74 | 0.62 | 12% | 168,675 | 841 |
| 64 | 11,482.7 | 11.64 | 3.98 | 3.37 | 63% | 371,199 | 693 |
| 256 | 10,950.2 | 11.34 | 12.35 | 9.80 | 195% | 755,977 | 1,467 |
| 1,024 | 10,096.7 | 10.53 | 13.21 | 9.04 | 211% | 587,380 | 977 |

代表 `strace -f -c -e trace=futex` run 使用 lane=256、iterations=1,000（只作 syscall shape，
不納入 baseline）：

```text
futex calls=26,853
errors=8,385
total syscall time=0.537108 s
```

strace 會改變 timing，且 futex syscall 無法單獨區分 ingress、Engine group worker 與
Completion worker，因此只能支持「協調成本存在」的觀察，不能把百分比分配給特定元件。

### 5.3 判讀

throughput 隨 lanes 從 1 增加到 1,024 約由 3.61K 上升到 944K callbacks/s；256 到 1,024
仍增加約 23.5%，因此本輪只確認 producer concurrency 是重要控制變因，尚未確認合併
worker／queue capacity ceiling。queue／Completion 的個別 causal root cause 仍標示為
`unconfirmed hypothesis`。

## 6. Publisher drain

正式 command：

```bash
taskset -c 0-3 /tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling --pipeline-stage=publisher_drain \
  --iterations=20 --warmup=1 --pipeline-batch-size=256
```

每輪使用 benchmark 自動建立的新暫存 root，measured backlog 為 5,120 commands、WAL size
624,662 bytes，`sink_calls=5120`、`sink_events=10240`、`durable_cursor_verified=true`。

| run | elapsed_ms | commands/s | group p99_us | group p99.9_us |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 15,770.7 | 324.653 | 15,770,699 | 15,770,699 |
| 2 | 15,337.4 | 333.825 | 15,337,369 | 15,337,369 |
| 3 | 12,594.8 | 406.518 | 12,594,783 | 12,594,783 |
| 4 | 14,012.0 | 365.402 | 14,011,974 | 14,011,974 |
| 5 | 14,090.7 | 363.360 | 14,090,719 | 14,090,719 |

median drain rate 為 363.360 commands/s，range 為 324.653–406.518；所有 run 都完成 durable
cursor reopen validation。

### 6.1 Syscall shape

代表 `strace -f -c -e trace=openat,write,close,fsync,fdatasync,rename` run 同樣使用 5,120
commands。摘要如下：

| syscall | calls | syscall seconds | traced time | calls/command |
| --- | ---: | ---: | ---: | ---: |
| `fsync` | 10,758 | 0.879142 | 60.91% | 2.101 |
| `rename` | 5,376 | 0.240961 | 16.69% | 1.050 |
| `openat` | 10,817 | 0.196919 | 13.64% | 2.113 |
| `write` | 5,400 | 0.064691 | 4.48% | 1.055 |
| `close` | 10,805 | 0.061681 | 4.27% | 2.110 |
| total | 43,156 | 1.443394 | 100.00% | 8.429 |

`/usr/bin/time -v` 的同類觀測 summary measured drain 約 26.586 秒、process wall time 約
61.78 秒、system time 0.89 秒、voluntary context switches 95,363；這與 strace／filesystem
負載會改變 timing 一致，不能拿來替換 baseline。

### 6.2 Device observation

`iostat -xz 1` 代表觀測期間 `/dev/sdb` 約出現 54%～83% `%util`，system iowait 約
3.2%～15.0%。該 device 同時承載其他工作，這是 device-level mixed signal；因此只能記錄
環境與支持 I/O 活躍，不能把所有 device wait 歸因於 Publisher。

### 6.3 判讀

Publisher drain 是本 workload 的明確 sustainable ceiling，且遠低於 1M target。`fsync` 與
`rename` 在 traced syscall time 中占主要比例，支持逐筆 cursor／durability 操作為後續
優化候選；但 fixture 建置、WAL replay、cursor persistence 與 measured drain 都在同一 process
lifetime，必須等 cursor group-persistence 的 before／after workload 才能確認因果 uplift。

## 7. Correctness 與工具結果

- Release warnings-as-errors benchmark build：通過。
- Clang Release warnings-as-errors build：unavailable（本機沒有可呼叫的 `clang++`）。
- pipeline metrics smoke：三個 case 均 `correctness_verified=true`。
- pipeline runtime／Publisher 正式 runs：所有採用 run 均 `correctness_verified=true`。
- GoogleTest Clang Debug binary：42/42 tests passed。
- ASan／UBSan CTest binary：32/32 tests passed。
- CLI lane `0`：exit code 2，`pipeline_producer_lanes_must_be_positive`。
- CLI lane `abc`：exit code 2，`invalid_arguments`。
- CLI lane `65537`：exit code 2，`pipeline_producer_lanes_exceed_capacity`。
- `git diff --check` 與 `git diff --cached --check`：通過。

ASan／UBSan preset 未開啟 opt-in benchmark target，因此 sanitizer 結果涵蓋 production library
與既有 tests，不代表 benchmark executable 在 sanitizer 下的效能或完整 stage 執行結果。

## 8. 後續建議與限制

1. 以 invariant 的 state-size scaling 為輸入，另案設計保持完整 correctness 的 incremental
   validation；先定義 completion latency SLO，再選擇 batch／validation policy。
2. 以 metrics controlled result 為輸入，另案評估 typed metric IDs、writer ownership 或
   sharded aggregation；不得直接把 benchmark percentage 當成 production uplift。
3. Runtime handoff 先補足 queue、Completion 與 ingress 的可分離觀測，再決定是否評估 batch
   drain；本報告不足以支持直接改成 lock-free queue。
4. Publisher 另案定義 cursor batch size、max interval、confirmed／durable cursor semantics、
   crash duplicate window 與 final flush，再用同一 backlog 做 before／after。
5. `perf` unavailable、CPU governor 為 powersave、`iostat` device 有外部負載；跨主機或跨
   filesystem 不得直接比較本報告數字。
