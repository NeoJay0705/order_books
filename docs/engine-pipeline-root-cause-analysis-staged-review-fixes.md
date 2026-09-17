# Engine Pipeline 根因分析 staged review 必要修正

## 1. 文件目的

本文件只記錄 staged changes 對照
`docs/engine-pipeline-root-cause-analysis-design.md` 後確認的必要修正，作為下一次修改與驗證的
直接依據。修正不得改動 production source、public API、持久化格式、durability／ordering
語意或 production default，也不得在本輪加入 metrics、Completion、queue 或 Publisher 的
production 優化。

目前兩個 benchmark 控制本身方向正確：

- metrics 的 shared／separate registry case 使用相同 worker、metric mix、command 數與
  wall-time completion boundary；
- `--pipeline-producer-lanes` 只控制 runtime handoff 的 producer concurrency，且每個 lane
  仍維持 single-in-flight；
- production `src/` 與 public headers 沒有變更。

因此不重寫 benchmark 架構，只修正以下三個已確認缺口。

## 2. 必要修正一：補齊 `writer_only` 輸出契約

### 2.1 問題

`benchmarks/pipeline_ceiling_benchmark.cpp` 的 `writer_only` 目前輸出：

```text
metric_calls_per_command=11
metric_names=...
```

但設計要求三個 metrics case 都能以 `contention_mode` 明確區分，並使用固定、可機器解析的
writer／Publisher metric-mix 欄位。parallel case 已使用 canonical 欄位，`writer_only` 尚未
一致；後續報告若同時解析三個 case，會需要例外分支，且無法直接確認其控制組身分。

### 2.2 具體修改

只修改 `run_metrics_single()` 傳給 `print_timing()` 的 details，不改量測流程、計數、metric
mix 或 throughput 算法：

```cpp
"contention_mode=single_worker"
    " writer_metric_calls_per_command=" +
    std::to_string(kWriterMetricCallsPerCommand) +
    " publisher_metric_calls_per_command=" +
    std::to_string(kPublisherMetricCallsPerCommand) +
    " writer_metric_names=" + std::string(kWriterMetricNames) +
    // 保留 observations 與 observations_per_second
```

移除原本名稱不一致的 `metric_calls_per_command` 與 `metric_names`。其中
`publisher_metric_calls_per_command=5` 是本次兩 worker 對照使用的固定參考 mix；
`writer_only` 實際 operation／observation 數仍只計算 writer mix，不得將 Publisher calls
加入其 throughput 分子。

不新增新的 output formatter 或資料結構，因為只有一個既有 details 字串需要對齊。

### 2.3 驗證

執行短 smoke：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=metrics \
  --iterations=2 \
  --warmup=1 \
  --pipeline-batch-size=4
```

驗證三行 summary 分別包含：

```text
case=writer_only contention_mode=single_worker
case=writer_publisher_separate_registries contention_mode=separate_registries_two_workers
case=writer_publisher_contended contention_mode=shared_registry_two_workers
```

三者都必須有 `correctness_verified=true`；兩個 parallel case 的 writer／Publisher count 與
worker rate 必須維持正確。

### 2.4 預估修改量

`benchmarks/pipeline_ceiling_benchmark.cpp` 約修改 5～8 行。

## 3. 必要修正二：補上獨立的 README root-cause 範例

### 3.1 問題

README 新增了 producer-lane 與 metrics 控制說明，但插在 durable group-commit 的介紹與其
命令範例之間，而且沒有設計要求的 root-cause command example。讀者會把後面的
`engine_durable_single_instrument` 命令誤認為前述 runtime root-cause 範例。

### 3.2 具體修改

先讓原有文字：

```text
For a durable group-commit matrix, the existing Engine workload accepts benchmark-only overrides:
```

直接接回既有 `engine_durable_single_instrument` code block。再於該 code block 之後放置
root-cause 說明及一個獨立命令，例如：

```bash
./build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=runtime_handoff \
  --iterations=1000 \
  --warmup=100 \
  --pipeline-batch-size=256 \
  --pipeline-producer-lanes=64 \
  --engine-group-size=256 \
  --engine-group-delay-us=200
```

命令只示範如何操作新控制，不宣稱 `64` 是最佳 lane 數，也不把這個短範例描述為正式
五輪結果。metrics 的 separate／shared case 仍由同一個 `--pipeline-stage=metrics` invocation
依序輸出，不需再增加第二個命令範例。

### 3.3 驗證

- durable group-commit 說明與其 code block 必須相鄰；
- root-cause 說明與 `runtime_handoff` code block 必須相鄰；
- 文件明確保留「diagnostic control，不是 production setting／最佳預設」的限制；
- README 不加入尚未取得的效能數字。

### 3.4 預估修改量

`README.md` 約新增或調整 10～16 行。

## 4. 必要修正三：完成正式五輪量測與根因報告

### 4.1 問題

目前 correctness smoke、單輪 metrics 對照與少量 producer-lane 測試只能證明 benchmark
可運作，不能滿足設計的根因分析 Definition of Done。特別是：

- metrics 單輪且 measured duration 未達 5 秒，不能用來確認 20%／range 判定；
- runtime 只測 lanes `1` 與 `8`，未形成完整 lane curve；
- invariant 未完成四個 state size 的五輪長測；
- Publisher 未完成長 backlog、五輪 baseline 與 syscall／device 證據；
- 尚無文件將結論分成 `confirmed ceiling`、`supported cause` 與
  `unconfirmed hypothesis`。

### 4.2 執行前固定條件

使用同一個 Release binary、source revision、CPU set 與 filesystem／device完成同組比較。
先記錄：

```text
git revision
compiler and build flags
CPU model, fixed CPU set and governor
kernel
filesystem, mount options and block device
kernel.perf_event_paranoid
```

不得修改 kernel security policy。若 `perf` 因權限不可用，在報告標記 `unavailable`，使用
受控對照、`/usr/bin/time -v`、`strace` 與 `iostat` 取得仍可用的證據。

先以 calibration run 找到符合時間下限的 `iterations`，再固定該值執行同一組五輪；不得讓
各 repetition 使用不同 iterations，也不得把 profiler／strace run 混入 baseline median。

### 4.3 必要測試矩陣

#### Invariant validation

- 固定 `--pipeline-batch-size=256`；
- `--pipeline-active-orders=0,1000,10000,100000`；
- 每點五輪，每輪 measured phase 至少 5 秒；
- 記錄 validations/s、median、worst p99／p99.9 及 amortized ns/command。

#### Metrics

- 固定 `--pipeline-stage=metrics --pipeline-batch-size=256`；
- 每次 invocation 依序產生 writer-only、separate registries 與 shared registry 三組；
- 五輪，每個 case measured phase 至少 5 秒；
- 比較 throughput median、range 與各 worker rate；
- 只有 shared median 至少低 20%，且五輪 range 不重疊，才能標示 shared registry
  contention 為 `supported cause`。

#### Runtime handoff／Completion

- 固定 `--engine-group-size=256 --engine-group-delay-us=200`；
- `--pipeline-producer-lanes=1,8,64,256,1024`；
- 每點五輪，每輪 measured phase 至少 10 秒；
- 記錄 callbacks/s、p50／p99／p99.9／max、user/system CPU、context switches；
- 另以代表組合執行 `strace -f -c -e trace=futex`；`perf` 可用時才補 profile；
- 只有 throughput 隨 lanes 上升後形成可重現平台，才能確認合併 ceiling。沒有 profiler 或
  受控對照時，不得進一步宣稱 queue、writer 或 Completion worker 的個別占比。

#### Publisher drain

- 每輪使用新的空 data directory；
- backlog 必須讓 measured drain 至少持續 10 秒；
- baseline 五輪；
- 代表組合另外執行 `/usr/bin/time -v`、syscall summary 與 `iostat -xz 1`；
- syscall run 只作形狀歸因，不納入 baseline throughput；
- fixture 建置也在 process lifetime 內，報告不得把所有 open／write 都歸因於 drain。

### 4.4 新增報告

新增：

```text
docs/engine-pipeline-root-cause-analysis-report.md
```

報告最少包含：

1. source revision、build、CPU affinity、filesystem／device 與工具限制；
2. 每個 stage 的完整 command、五輪結果、median、range、worst p99／p99.9；
3. 無效或失敗 run 及原因，不得只保留成功結果；
4. metrics shared／separate 的受控差值；
5. runtime lane curve 與是否已形成平台；
6. Publisher sustainable drain rate 與正規化 syscall counts；
7. 每個候選根因的證據與下列分類：

```text
confirmed ceiling
supported cause
unconfirmed hypothesis
```

若環境或工具使某項無法完成，必須記錄實際阻礙、已完成的替代觀測及仍不能回答的問題；
不得以 source inspection 或先前短測補成確定結論。

### 4.5 預估修改量

報告約 150～250 行；量測本身不需要修改 production 或 benchmark 程式。若正式測試暴露
correctness failure，應先停止效能判讀並另行審查，不在報告撰寫時順手調整 production
hot path。

## 5. 完成後驗證

必要驗證如下：

1. GCC／Clang Release warnings-as-errors build 成功；
2. 既有 GoogleTest 全部通過；
3. ASan／UBSan correctness suite 通過；
4. pipeline `all` smoke 的所有 case 都輸出 `correctness_verified=true`；
5. producer lanes `0`、非數字及 `65537` 均以 exit code 2 拒絕；
6. `git diff --check` 與 `git diff --cached --check` 通過；
7. 確認 `include/order_books/`、`src/domain/`、`src/persistence/`、`src/runtime/`、WAL format、
   production config 與 dependency manifest 均未改動。

正式壓測不加入 CI performance gate；CI 只保留短 correctness smoke，避免受 shared runner
雜訊影響。

## 6. 修改量總結

```text
benchmarks/pipeline_ceiling_benchmark.cpp          5～8 行
README.md                                         10～16 行
docs/engine-pipeline-root-cause-analysis-report.md 150～250 行
production source                                  0 行
```

排除實測報告後，程式與 README 的必要修正約 15～24 行。這些修改只補齊已定義的輸出、使用
方式與交付證據；不新增 framework、production hook、performance threshold 或未經證據支持的
優化，因此沒有超出需求。
