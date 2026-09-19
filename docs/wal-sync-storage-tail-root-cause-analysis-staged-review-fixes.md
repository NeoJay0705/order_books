# WAL sync／storage tail staged review 必要修正

## 1. 目的與結論

本文件針對 `docs/wal-sync-storage-tail-root-cause-analysis-design.md` 與目前 staged changes 的 review
結果，定義實作前必須完成的最小修正。現有架構方向正確：telemetry 僅存在 benchmark target，沒有修改
production WAL、Engine、Publisher、Completion、durability boundary 或預設設定。

必要修正共五項：

1. 避免 state sampler 在錯過 deadline 後密集補採樣；
2. 在 measured window 前完整預留 bounded telemetry 容量；
3. 修正 benchmark early-return 的 sampler／Engine 停止順序；
4. 以原子 exclusive-create 實作 CSV 不覆寫保證；
5. 補齊 CLI 與 artifact／summary 一致性的 smoke test。

除上述項目外，不重構既有 benchmark、不新增通用 tracing framework、不修改 production code，也不在本案
執行根因矩陣或效能調參。

## 2. 必要修正

### 2.1 避免 sampler 錯過 deadline 後密集補採樣

#### 問題

`EngineTailTelemetry::sampler_loop()` 每輪只執行：

```cpp
next_sample += kStateSampleInterval;
```

若 sampler 因排程、`Engine::metrics()` 或系統負載晚了多個週期，`next_sample` 會停留在過去。後續
`wait_until()` 立即返回，造成連續多次 `Engine::metrics()`。這些呼叫無法取得錯過時點的歷史狀態，只會
形成 sampling burst、放大量測 contention，可能反過來製造或放大待分析的 latency tail。

#### 最小修正

在每次採樣後仍先依原 deadline 增加 10 ms；如果更新後的 deadline 已經不晚於目前時間，直接把下一個
deadline 設為 `now + kStateSampleInterval`，放棄補採過去的 tick：

```cpp
next_sample += kStateSampleInterval;
const auto now = std::chrono::steady_clock::now();
if (next_sample <= now) {
  next_sample = now + kStateSampleInterval;
}
```

這仍是 deadline-based wait；正常情況維持固定週期，逾期時則跳過無法重建的樣本，不進行 burst
catch-up。不要改成無條件 `sleep_for(10ms)`，避免把每次 `metrics()` 執行時間累加為永久漂移。

為了可確定性測試，可抽出 benchmark-internal 純函式，例如：

```cpp
steady_clock::time_point next_sample_deadline(
    steady_clock::time_point previous_deadline,
    steady_clock::time_point observed_at) noexcept;
```

測試至少涵蓋：準時時沿用原 cadence、晚一個以上週期時回到未來、結果永遠大於
`observed_at`。此 helper 只放在 `benchmarks/engine_tail_telemetry.*`，不得成為 public API。

#### 驗收條件

- sampler 不會為錯過的週期連續呼叫 `Engine::metrics()`；
- sampling interval 仍固定為 10 ms，沒有新增 CLI 或 production config；
- deadline helper 的 unit tests 通過。

預估修改：15--25 行。

### 2.2 完整預留 bounded telemetry 容量

#### 問題

collector 允許每類 metric 1,000,000 筆、state 100,000 筆，但目前 constructor 只為 state 額外預留
10,000 筆。較長的合法 benchmark 可能在 measured window 內觸發 `records_` reallocation；reallocation
位於 collector mutex 內，會阻塞 writer 的 metric callback，違反設計要求的 measured 前預留容量。

#### 最小修正

constructor 以實際 bounded 上限計算容量：

```text
metric capacity = min(expected_groups, kMaxMetricSamples) * 2
state capacity  = kMaxStateSamples
total capacity  = metric capacity + state capacity
```

先檢查乘法與加法是否超過 `std::numeric_limits<std::size_t>::max()`；若超過或 `reserve()` 失敗，設定
`setup_failed_`，讓 benchmark 在開啟 Engine 前回報 `tail_telemetry_reserve_failed`。不要在 measured
期間嘗試增加容量，也不要把上限改成無界配置。

#### 驗收條件

- 成功通過 `reserve()` 後，直到三類 sample 各自到達設計上限前都不會 reallocate；
- 容量計算失敗仍在 Engine open 前回報；
- sample 上限與 dropped counter 語意不變。

預估修改：5--10 行。

### 2.3 統一 early-return shutdown 順序

#### 問題

目前多個錯誤路徑先執行 `engine->stop()`，再由 `cleanup()` 停止 sampler。這會讓 sampler 在 Engine
失敗或 teardown 期間繼續呼叫 `Engine::metrics()`，與設計要求的 early-return 順序相反。

#### 最小修正

在 Engine open 成功後建立兩個局部 helper：

```cpp
const auto stop_telemetry = [&] {
  if (telemetry.has_value()) {
    telemetry->stop_collection();
    telemetry->stop_sampler();
  }
};

const auto stop_engine_after_telemetry = [&] {
  stop_telemetry();
  return engine->stop();
};
```

所有 sampler 啟動後、進入正常 drain 以前的 early return，都改呼叫
`stop_engine_after_telemetry()`，之後只清理 owned data directory。正常成功路徑維持既有順序：

```text
begin_drain -> Engine::stop -> stop_collection -> stop_sampler -> write artifact
```

因為正常 drain 必須在 Engine 停止期間持續觀察 publisher lag，不能把成功路徑也改成先停 sampler。
`stop_telemetry()` 必須可重複呼叫，避免 cleanup 或 destructor 形成第二次停止問題。

#### 驗收條件

- 所有 early return 都先 join sampler，再停止 Engine；
- 正常 drain 仍涵蓋 `Engine::stop()`；
- 任一路徑離開函式時沒有 joinable sampler thread；
- 原 Engine/WAL error code 仍優先回報。

預估修改：15--25 行。

### 2.4 原子保證 CSV 不覆寫既有檔案

#### 問題

`exists(path)` 與 `std::ofstream(path)` 是兩個分離操作。兩個 benchmark 同時使用相同輸出路徑時，都可能
先觀察到檔案不存在，之後由其中一方截斷另一方已建立的 artifact，違反「不得覆寫」契約。

#### 最小修正

專案部署與開發平台均為 POSIX（Linux／macOS），因此 `write_csv()` 使用：

```cpp
::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)
```

只有 exclusive create 成功後才能寫入。以 `fdopen()` 取得 buffered `FILE*`，依既有排序結果逐列輸出；
所有 `fprintf`／`fwrite`、`fflush` 與 `fclose` 結果都必須納入成功判定。若 `fdopen()` 失敗，必須關閉原
file descriptor。不要採用「exclusive open 後 close，再用 `ofstream` 重開」的方式，因為中間仍有
unlink／replace race。

輸出內容維持既有固定 schema、base-10 整數與空欄位；不引入 JSON library、CSV dependency、暫存檔
rename protocol或 production storage abstraction。寫入失敗可保留 partial artifact供診斷，但該輪必須
回報 `telemetry_output_failed` 並視為無效。

新增 unit test：預先建立目標檔案並寫入 sentinel，呼叫 `write_csv()` 必須失敗，且 sentinel 內容不可
改變。既有「第一次成功、第二次失敗」測試可以保留並合併 sentinel 驗證，避免重複案例。

#### 驗收條件

- 已存在的一般檔案、symlink 或同時競爭建立的 path 不會被截斷；
- write／flush／close 任一失敗都回傳 false；
- CSV schema與排序語意不變；
- 不在 Engine 停止前執行任何 artifact I/O。

預估修改：25--40 行，包含測試。

### 2.5 補齊 CLI 與 artifact 一致性 smoke test

#### 問題

目前 smoke test 只以 regex 確認部分 summary 欄位及兩種 row 存在，尚未驗證錯誤 workload、完整 CSV
row schema、row count及 aggregate是否和 summary一致。測試即使遇到重複、缺少或 malformed row也可能
通過，無法保證 artifact可用於後續根因報告。

#### 最小修正

1. 讓 `check_invalid_cli.cmake` 接受可選的 `WORKLOAD`，未提供時仍預設
   `engine_durable_single_instrument`，避免影響既有測試。
2. 新增一個 CTest，使用其他 workload搭配 `--engine-tail-telemetry-output=...`，驗證 exit code為 2且
   error code為 `engine_tail_telemetry_requires_engine_durable_workload`。
3. 強化 `check_engine_tail_telemetry.cmake`：
   - 從 summary解析 sync count、group sample count及group command總和；
   - 用 `file(STRINGS)` 逐列解析 artifact；
   - header必須完全相等；
   - `sync`／`group_commands` row只能是 `measured` phase且有8欄；
   - `state` row只能是 `measured` 或 `drain`，數值欄位必須符合schema；
   - 未知 record type、缺欄、多欄或非十進位整數立即失敗；
   - 重算 sync row count、group row count與group command sum，並和 summary比較。
4. smoke workload需穩定跨過至少一個10 ms sampling interval。使用小型 command count、較大的group
   size與固定1 ms group delay即可；不要以忙等或固定RPS assertion達成。
5. 測試結束仍只刪除 fixture自己建立的 artifact。

不要在 smoke test斷言固定RPS、sync duration必須大於0、一定存在tail或publisher lag必須大於0。
replay、durable head與completion correctness仍由 benchmark成功結束及既有validation保證。

#### 驗收條件

- 空 path與錯誤 workload都由CTest覆蓋；
- CSV所有rows均可依固定schema解析；
- artifact重算值與summary完全一致；
- smoke在不同CPU／storage速度下不依賴效能門檻。

預估修改：30--50 行。

## 3. 建議實作順序

1. 修正容量計算與sampler deadline；
2. 修正early-return lifecycle；
3. 改為exclusive CSV writer；
4. 補unit tests與CTest smoke validation；
5. 執行格式、build及correctness測試。

此順序先固定執行期語意，再更新輸出與驗證，可減少測試同時追蹤多種失敗的情況。

## 4. 驗證方式

完成修正後執行：

```text
ReleaseBenchmark build
ReleaseBenchmark full CTest
Debug build and full CTest
ASan/UBSan build and full CTest
git diff --check
```

另外單獨確認：

- telemetry未開啟時仍使用 `NullMetricsSink`且輸出相容；
- telemetry smoke產生可解析artifact並自行清理；
- 相同output path第二次執行失敗且原檔內容不變；
- 錯誤workload與空path回傳CLI exit code 2；
- staging內容不因驗證操作而改變。

本修正階段不需要執行15秒五輪W=1/W=2根因矩陣；那屬於修正完成並通過review後的正式量測。若改動
sampler cadence或collector容量，正式根因分析前仍必須依設計執行telemetry off/on calibration。

## 5. 修改範圍與行數

預期只修改：

```text
benchmarks/engine_tail_telemetry.hpp
benchmarks/engine_tail_telemetry.cpp
benchmarks/order_book_benchmark.cpp
benchmarks/check_invalid_cli.cmake
benchmarks/check_engine_tail_telemetry.cmake
benchmarks/CMakeLists.txt
tests/unit/engine_tail_telemetry_test.cpp
```

預估總修改約90--140行，其中核心實作約40--65行，其餘為必要測試。不得修改：

```text
include/order_books/*
src/runtime/*
src/storage/*
production defaults
WAL format or durability semantics
```

若實作需要超過上述範圍或明顯超過140行，應先重新檢查是否引入了通用exporter、額外configuration、
production abstraction或與本次根因量測無關的重構。
