# WAL append publish bookkeeping Writer tail 根因分析：staged review 必要修正

## 1. 目的與結論

本文件記錄對 `docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-design.md`
及其 staged implementation 的 review 結果，只列出合併前必須完成的修正。

目前實作的架構方向正確，production runtime 的改動只用於 thread naming，沒有修改 WAL format、
append／sync 語意、group commit、queue、worker 數量、affinity、priority、Publisher 或 Completion
處理流程。以下五項修正都是為了滿足既有設計契約或避免診斷結果失真；不加入新的 production
功能，也不擴大正式壓測矩陣。

完成這些修正前，現有 staged changes 不應視為完成，原因是：macOS 測試會固定失敗、telemetry
尚未完全保證 bounded／完整、diagnostics-off 改變既有輸出、不可寫的輸出路徑發現得太晚，以及
migration 缺少設計要求的 normalization。

## 2. 修正範圍與邊界

允許修改：

- `benchmarks/CMakeLists.txt`；
- `benchmarks/check_writer_thread_diagnostics.cmake`；
- `benchmarks/check_writer_diagnostics_output.cmake`；
- `benchmarks/engine_writer_profile_benchmark.cpp`；
- `benchmarks/engine_tail_telemetry.cpp`，僅加入已配置容量的硬邊界；
- `tests/unit/engine_tail_telemetry_test.cpp`；
- 與上述行為直接相關的既有 benchmark／unit tests。

不需要修改：

- `src/persistence/wal.cpp` 的 append、prepare、publish 或 sync 演算法；
- `src/runtime/shard_runtime.cpp` 的 command processing；
- `EngineConfig`、public API、WAL format、queue item 或 durability contract；
- worker 數量、scheduler policy、thread affinity 或正式 benchmark case；
- `ThreadResourceSnapshot` 的資料模型。

## 3. 必要修正

### 3.1 Linux-only diagnostics smoke 不得使 macOS CTest 失敗

#### 問題

`order_books_benchmark_writer_thread_diagnostics_smoke` 目前在所有平台註冊。非 Linux 平台的
`capture_thread_resources()` 會正確回傳 `unsupported_platform`，所以該正向 smoke 在 macOS
必然失敗，與設計要求的「macOS 可編譯並通過一般測試」衝突。

#### 具體修改

在 `benchmarks/CMakeLists.txt`：

1. 只用 `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` 包住
   `order_books_benchmark_writer_thread_diagnostics_smoke` 的 `add_test()`；
2. parser／delta unit tests 保持跨平台執行，因為它們使用固定 fixture，不依賴 live procfs；
3. CLI 的參數合法性測試保持跨平台執行；
4. 不為 macOS 實作替代 scheduler counter，也不把 Linux 錯誤偽裝成成功。

#### 驗收

- Linux 仍執行並通過 diagnostics 正向 smoke；
- macOS 不註冊該正向 smoke，但其餘 benchmark tests 正常註冊；
- 不以 runtime skip 回傳成功，避免把未執行的 Linux diagnostics 誤報為通過。

預估修改：4--8 行。

### 3.2 Telemetry 必須在量測期間維持 bounded，並驗證逐 group 完整性

#### 問題

Writer 目前以 `ceil(measured_commands / configured_group_size)` 預留 telemetry；這只是 group 數量的
理想估計。group delay 可能讓實際 group 小於 configured group size，因此實際 group 數可能更多。
`EngineTailTelemetry::observe()` 在 vector capacity 用完後仍可能擴容，會把 allocation 帶入量測熱路徑。

此外，正式結果目前只拒絕 dropped sample、aggregate overflow 與 sampler error，沒有把 telemetry
sample count 與 Engine 的 authoritative group counters 對帳。若某類 metric 靜默缺失，run 仍可能被
當作有效證據。

#### 具體修改

在 `benchmarks/engine_tail_telemetry.cpp` 的 `EngineTailTelemetry::observe()`：

1. 保留現有 per-type `kMaxMetricSamples` 檢查；
2. 在 `records_.push_back()` 前檢查 `records_.size() >= records_.capacity()`；
3. capacity 已滿時只增加 `dropped_samples_` 並 return，不允許 vector 在 measured／drain phase 擴容；
4. 不改變既有 constructor 的 reserve 策略，也不新增 Writer 專用 collector。

在 `benchmarks/engine_writer_profile_benchmark.cpp`：

1. 保留現有 `telemetry_dropped_samples == 0`、overflow 與 sampler error gate；
2. 在 `measured_group_commits`／`measured_group_commands` 已計算完成後，額外要求：
   - `measured_sync_count == measured_group_commits`；
   - `measured_group_sample_count == measured_group_commits`；
   - `measured_group_sample_commands == measured_group_commands`；
3. 任一不符時輸出 `error_code=writer_tail_telemetry_count_mismatch`，不寫正式 summary，run 回傳失敗；
4. 成功 summary 額外輸出 `writer_group_sample_count` 與
   `writer_group_sample_commands`，讓報告可直接稽核完整性；
5. 不用增加預設 group size、延遲或 worker 數來掩蓋 capacity 問題。

#### 測試

- 擴充 diagnostics smoke，要求 CSV 同時包含 `sync` 與 `group_commands` record；
- smoke 成功即代表 runtime completeness gate 已通過；
- 在 `engine_tail_telemetry_test.cpp` 補一個容量到達硬邊界時增加 dropped count、且不拋例外的測試；
- 不建立效能數字或固定 RPS 的 CTest gate。

#### 驗收

- measured phase 中 `records_` 不發生 capacity growth；
- 每個 measured WAL group 恰有一筆 sync 與一筆 group-command sample；
- sample command 總數與 Engine authoritative counter 相同；
- 不完整資料必須使 run 非 0，不得只在報告中警告。

預估修改：實作 20--30 行，測試 15--25 行。

### 3.3 Diagnostics 關閉時維持既有 stdout schema

#### 問題

summary 現在無條件輸出 `thread_diagnostics=off|on`。設計要求 diagnostics 關閉時不得輸出診斷欄位，
因此預設 workload 的既有 stdout schema 被不必要地改變。

#### 具體修改

在 `benchmarks/engine_writer_profile_benchmark.cpp`：

1. 先輸出原有 summary 到 `fsync_mode=per_group`；
2. 只有 `options.thread_diagnostics == true` 時才追加：
   - `thread_diagnostics=on`；
   - `writer_tail_telemetry=on`；
   - Writer tail summary 與 telemetry path；
3. diagnostics off 時不輸出 `thread_diagnostics=off`，也不輸出任何
   `thread_resource`／`writer_tail_*` 欄位；
4. 不改變 diagnostics on 的欄位名稱，避免讓後續報告腳本出現兩種 schema。

#### 測試

重用既有 `check_writer_profile_output.cmake`，在一個預設 diagnostics-off Writer smoke 的
`FORBIDDEN_REGEXES` 加入：

```text
thread_diagnostics=
phase=thread_resource
writer_tail_telemetry=
writer_sync_
writer_tail_telemetry_file=
```

不需要新增另一套測試 script。

#### 驗收

- diagnostics off 的 summary 與本變更前既有 schema 相同；
- diagnostics on 才出現診斷欄位；
- diagnostics off 不讀 procfs、不配置 telemetry collector、不建立輸出檔。

預估修改：實作 3--6 行，測試 3--8 行。

### 3.4 不可寫的 telemetry parent 必須在 measured phase 前拒絕

#### 問題

目前 setup 只確認 output 不存在且 parent 是目錄。parent 沒有 write／search 權限時，完整 measured
phase 仍會執行，最後 `write_csv()` 才失敗。這會浪費正式輪次，也違反設計中 setup failure 必須早於
正式量測的要求。

#### 具體修改

在 `benchmarks/engine_writer_profile_benchmark.cpp` 的 output setup validation：

1. 先把空 parent path 正規化成 `.`；
2. 保留 output 不得已存在及 parent 必須是 directory 的檢查；
3. 在 Linux／macOS 以 `::access(parent.c_str(), W_OK | X_OK)` 驗證目前 process 可在 parent 建立檔案；
4. 檢查失敗時沿用 `error_code=writer_tail_telemetry_output_unavailable`，並在 runtime start 前返回；
5. 最終輸出仍使用既有 `O_CREAT | O_EXCL`，它是避免覆寫及處理 setup-check 後競爭的 authoritative gate；
6. 不建立永久 probe file，也不改成覆寫既有 output。

這裡只需要符合本專案支援的 Linux／macOS；不新增跨平台 filesystem abstraction。

#### 測試

- output 已存在的情況必須在 benchmark 開始前失敗；
- parent 不存在或不是 directory 必須失敗；
- 權限測試只在能可靠建立非特權測試環境時執行；不可在以 root 執行時建立必然誤判的 flaky test。

#### 驗收

- 明確不可用的 output path 不會進入 warmup／measured phase；
- setup 與 final write 都不會覆寫使用者既有檔案；
- final `O_EXCL` 失敗仍使 run 無效。

預估修改：實作 8--15 行，可靠的 path tests 10--18 行。

### 3.5 輸出 migration 的 per-million normalization 與明確 absence

#### 問題

`ThreadResourceDelta` 已保留 optional migration raw delta，但輸出缺少設計要求的
`migrations_per_million`。migration 不可用時目前輸出 `na`，沒有明確表達這是未量測，而不是零或
計算失敗。

#### 具體修改

在 `print_thread_resource_deltas()`：

1. `cpu_migrations` 有值時，用既有 `per_million()` 計算 normalized value；
2. normalization overflow 時沿用 `thread_resource_normalization_failed`，run 回傳失敗；
3. 輸出 raw `cpu_migrations=<value>` 與 `migrations_per_million=<value>`；
4. migration 不可用時，兩欄都輸出 `not_measured`；
5. 不推導 I/O wait，也不加入新的 scheduler counter。

#### 測試

- 現有 migration-present fixture 驗證 normalized value；
- migration-absent fixture 驗證 absence 保留，且 smoke schema 含
  `migrations_per_million=`；
- 不要求 live migration counter 大於零，避免 flaky test。

#### 驗收

- 報告可以直接比較 baseline／candidate 的 migrations/M commands；
- `not_measured` 與實際數值零可明確區分；
- normalization 失敗不會產生部分有效的 resource row。

預估修改：實作 8--12 行，測試 5--10 行。

## 4. 建議實作順序

1. 先加入 telemetry capacity hard stop 與完整性 gate；
2. 修正 diagnostics-off schema；
3. 補 migration normalization；
4. 補 output parent 的 setup validation；
5. 將正向 diagnostics smoke 限制於 Linux；
6. 最後補齊只針對上述行為的 regression tests。

此順序讓測試先依賴最終輸出契約，避免反覆修改 smoke regex。

## 5. 檔案級修改清單與行數

| 檔案 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| `benchmarks/engine_tail_telemetry.cpp` | capacity hard stop | 5--8 |
| `benchmarks/engine_writer_profile_benchmark.cpp` | completeness、off schema、path preflight、migration normalization | 35--50 |
| `benchmarks/CMakeLists.txt` | Linux smoke gate及既有 off-path forbidden regex | 7--14 |
| `benchmarks/check_writer_thread_diagnostics.cmake` | CSV／resource schema assertions | 10--18 |
| `benchmarks/check_writer_diagnostics_output.cmake` | existing／missing parent path regression | 35--45 |
| `tests/unit/engine_tail_telemetry_test.cpp` 或既有相關 test | capacity／drop regression | 15--25 |
| 必要的 path／normalization tests | 只覆蓋新增分支 | 10--18 |

總修改量預估約 110--145 行。production runtime、WAL 及 public headers 不需增加修改。

## 6. 驗證方式

至少執行：

```bash
cmake -S . -B build/ReleaseBenchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DORDER_BOOKS_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON \
  -DORDER_BOOKS_WARNINGS_AS_ERRORS=ON
cmake --build build/ReleaseBenchmark --parallel 4
ctest --test-dir build/ReleaseBenchmark --output-on-failure --parallel 4
```

並額外確認：

- Linux diagnostics smoke 產生非空 CSV，五個必要 role 全部存在；
- CSV 同時包含 sync 與 group-command records；
- summary 的 telemetry counts 與 WAL group counters 完全一致；
- diagnostics-off smoke 不含任何診斷欄位；
- macOS configure 後沒有註冊 Linux-only diagnostics 正向 smoke；
- `git diff --check` 與 `git diff --cached --check` 通過；
- 修正期間不得執行 `git add`、`git reset`、`git restore --staged` 或其他會改變 index 的操作。

## 7. 完成定義

只有同時符合以下條件，staged review 才能結案：

- Linux 與 macOS 的測試註冊符合平台能力；
- diagnostics measured path 不會因 telemetry vector 自動擴容而引入未標記的 allocation；
- telemetry 每個 measured WAL group 的 sync／command sample 完整且可對帳；
- diagnostics off 的既有輸出與行為保持不變；
- 不可用 output path 在正式 measured phase 前被拒絕；
- migration raw／normalized／not-measured 三種語意清楚；
- 所有既有 correctness、replay、benchmark smoke 及新增 regression tests 通過；
- 沒有新增 production algorithm、public API 或不屬於本次根因分析的調校。
