# WAL sync／storage tail post-benchmark staged review 必要修正

## 1. 目的與結論

本文件根據 `docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-review.md` 與目前 staged
changes 的 review 結果，定義合併前必須完成的最小修正。現有實作的架構方向正確：collector／state
sampler 已能分開啟用，drain 起訖快照只存在 benchmark target，production WAL、Engine API、durability
語意與預設設定皆未修改。

仍需完成三項必要調整：

1. 消除 periodic sampler 與 drain-start snapshot 的競態，並移除會掩蓋 boundary 缺失的 fallback；
2. 修正 smoke test 對 drain sample count 的錯誤判斷，並驗證 summary 與 CSV 的 boundary 數值一致；
3. 補交原設計明列的 blocked benchmark report。

除此之外不修改 production code、不調整 `fsync`／group policy／prepare workers，也不在本修正中執行新的
component calibration 或正式根因矩陣。

## 2. 必要修正一：保證 drain boundary 與 CSV 順序一致

### 2.1 問題

目前 controller 的正常路徑是：

```text
begin_drain()
  -> Engine::metrics()
  -> record_drain_snapshot(..., drain_start)
```

`begin_drain()` 將 phase 切換為 `drain` 後，periodic sampler 仍在背景執行。若 sampler 恰好在
`Engine::metrics()` 與 `record_drain_snapshot()` 之間醒來，它可能先寫入一筆沒有 boundary marker 的
drain state row。CSV 不輸出內部 boundary marker，因此 CSV 的第一筆 drain row 可能不是明確的
drain-start snapshot；summary 則會選擇帶有 `drain_start` marker 的資料，兩者語意可能不一致。

另外，`EngineTailTelemetry::summary()` 目前會在缺少明確 boundary 時，用一般 periodic drain row
補出 `first`／`last`。這會讓 boundary 遺失被誤認為成功，與「full telemetry 必須包含明確
drain-start 與 drain-end」的驗證契約衝突。

### 2.2 最小修法

在 `EngineTailTelemetry` 內新增兩個由既有 `mutex_` 保護的狀態：

```cpp
bool drain_start_recorded_{};
bool drain_end_recorded_{};
```

不要增加新的 mutex、condition variable 或 public Engine API。修改 `record_state()` 在取得 `mutex_` 且
確認 phase 後，套用以下規則：

1. `phase == drain`、`boundary == none` 且尚未記錄 drain-start：略過該 periodic sample，不計為 drop，
   並回傳成功；它只是落在尚未建立明確 drain 起點的競態窗口。
2. `boundary == drain_start`：只有尚未記錄 start／end 時才允許寫入；成功 `push_back()` 後才設定
   `drain_start_recorded_ = true`。
3. `boundary == drain_end`：只有 start 已存在且 end 尚未存在時才允許寫入；成功 `push_back()` 後才設定
   `drain_end_recorded_ = true`。
4. end 已存在後不得再接受 drain state row。正常流程已先 join sampler，再寫入 drain-end，因此此規則
   只負責防止未來呼叫順序退化。
5. 容量不足、overflow、phase 錯誤或重複 boundary 仍回傳 false；controller 依既有流程將該輪判為無效。

上述狀態必須在 record 實際寫入後才更新，避免 `push_back()` 失敗卻留下假的 boundary 完成狀態。

### 2.3 移除 summary fallback

從 `EngineTailTelemetry::summary()` 移除：

- 六個 `fallback_drain_*` local optional；
- `boundary == none` 時更新 fallback 的分支；
- loop 結束後以 fallback 補入 first／last 的兩組邏輯。

summary 僅能由明確 marker 產生：

```text
drain_start -> drain_publisher_lag_*_first
drain_end   -> drain_publisher_lag_*_last
```

如此一來，現有 `full_state_sampling_valid` 對 first／last optional 的檢查，就同時代表兩個明確 boundary
確實存在。`drain_state_sample_count` 仍計算所有 drain state rows，包含 start、periodic samples 與 end。

### 2.4 測試

保留既有 `RecordsDrainBoundarySnapshots`，並補充下列斷言：

- drain-end 不能在 drain-start 之前記錄；
- drain-start／drain-end 各自只能成功一次；
- boundary 缺失時 summary 不得使用一般 periodic sample 補值；
- full smoke 產生的 CSV 第一筆 drain row等於 summary 的 `*_first`，最後一筆 drain row等於
  summary 的 `*_last`。

若不希望為 unit test 暴露 private `record_state()`，periodic-before-start 的情境由 integration smoke
驗證即可；不要為測試把 helper 升格成 public production API。

### 2.5 驗收條件

- full telemetry 成功時，CSV 第一筆／最後一筆 drain row 分別是明確 start／end snapshot；
- summary first／last 與上述 CSV row 的 events、bytes、age 完全相同；
- 缺少任一 boundary 時 benchmark 非零結束；
- collector-only 仍沒有任何 state row；
- measured throughput window、CSV schema與 production 行為不變。

預估修改：benchmark helper與unit test約25--45行異動，其中會刪除約20--30行fallback程式。

## 3. 必要修正二：以數值方式驗證 drain sample count 與 artifact

### 3.1 修正錯誤的正規表示式

目前檢查：

```cmake
if(NOT STDOUT MATCHES "drain_state_sample_count=[2-9][0-9]*")
```

只能接受首位為2到9的數值，會錯誤拒絕`10--19`、`100--199`等合法結果。不要再用regex表示
「大於等於2」。先擷取十進位數字，再做數值比較：

```cmake
string(REGEX MATCH "drain_state_sample_count=([0-9]+)"
       DRAIN_COUNT_MATCH "${STDOUT}")
set(SUMMARY_DRAIN_COUNT "${CMAKE_MATCH_1}")
if(NOT DRAIN_COUNT_MATCH OR SUMMARY_DRAIN_COUNT LESS 2)
  message(FATAL_ERROR "drain boundary samples missing\nstdout:\n${STDOUT}")
endif()
```

collector-only 模式則要求 summary count與CSV drain row count都等於0。

### 3.2 比對 summary 與 CSV boundary

強化 `check_engine_tail_telemetry.cmake` 的CSV loop，將 measured與drain state row分開解析。對drain row
維護：

```text
CSV_DRAIN_COUNT
CSV_DRAIN_EVENTS_FIRST / BYTES_FIRST / AGE_FIRST
CSV_DRAIN_EVENTS_LAST  / BYTES_LAST  / AGE_LAST
```

CSV已依`elapsed_us`與內部order排序，所以第一筆drain row只設定一次first，之後每筆都更新last。完成
解析後：

1. `CSV_DRAIN_COUNT == SUMMARY_DRAIN_COUNT`；
2. full模式下count至少為2；
3. summary的六個first／last欄位都必須是十進位整數；
4. summary first三值等於CSV第一筆drain row；
5. summary last三值等於CSV最後一筆drain row；
6. collector-only模式下`STATE_COUNT == 0`、`CSV_DRAIN_COUNT == 0`，first／last維持`na`。

不要加入固定lag值、lag一定下降、固定RPS或固定drain row數量的斷言；這些數值受當次排程與Publisher
進度影響，不屬於functional contract。

### 3.3 驗收條件

- 任意合法的drain count（包含10、100）都能通過數值檢查；
- 少於兩筆、缺欄、非整數或summary／CSV不一致會使CTest失敗；
- full與collector-only仍共用同一個checker，僅依`STATE_SAMPLING`切換必要條件；
- 不延長benchmark workload，也不新增sleep或忙等。

預估修改：CMake smoke checker約25--40行。

## 4. 必要修正三：補交 blocked benchmark report

### 4.1 檔案

建立：

```text
docs/wal-sync-storage-tail-root-cause-analysis-benchmark-report.md
```

此報告描述修正前已執行、但因W=2 calibration gate失敗而停止的那次量測。它不是新的正式壓測結果，
也不得把calibration輪次包裝成正式矩陣。

### 4.2 必須記錄的內容

依既有artifact `/home/neojhou/wal-sync-tail-analysis-dFGqFZVA` 核對後寫入：

- 測試結果：ReleaseBenchmark 115/115、Debug 90/90、ASan/UBSan 90/90；
- W1 telemetry bias約1.64%，通過5% gate；
- W2 telemetry bias約40.25%，未通過5% gate；
- 正式矩陣0/10，原因是依預定gate停止；
- 兩個W=2慢輪新增sync wait約可解釋94.65%與98.92%的額外elapsed time；
- sync tail判定、device/storage、filesystem/syscall、queue/publisher與parallel-prepare目前各自的證據強度；
- observed正常輪沒有bandwidth saturation，但不能用它排除慢輪的storage tail；
- 原始artifact位置、工具可用性，以及沒有執行strace的限制；
- 此次結果只支持「下一步需要拆分collector／sampler成本並補同期外部證據」，不支持production rollout或
  durability／group policy變更。

報告至少包含摘要、環境與identity、correctness、calibration、慢輪證據、未執行項目、限制與下一步。
若原始artifact中的值與上述約值不同，以artifact為準並保留計算式；不得憑設計文件填造缺失欄位。

### 4.3 與後續壓測的界線

本blocked report完成後，仍不代表component calibration已通過。新的三模式60秒calibration應在本文件
第2、3節程式與測試修正完成、review通過後另行執行；結果應追加或另建後續報告，不覆寫本次歷史結論。

預估新增：80--130行文件。

## 5. 實作順序

1. 在telemetry helper加入boundary狀態與periodic drain gate；
2. 移除summary fallback並補unit test；
3. 修正CMake數值判斷，加入summary／CSV first-last一致性檢查；
4. 執行targeted telemetry tests及完整correctness測試；
5. 依既有artifact建立blocked benchmark report；
6. 確認diff範圍後再安排三模式component calibration。

## 6. 驗證方式

修正完成後先執行correctness驗證：

```text
ReleaseBenchmark build
telemetry targeted CTest
ReleaseBenchmark full CTest
Debug build and full CTest
ASan/UBSan build and full CTest
git diff --check
```

另以只讀檢查確認：

- staged production檔案仍為0；
- telemetry off沒有collector或sampler；
- collector-only只有sync／group rows；
- full artifact的drain first／last與summary一致；
- blocked report沒有把0/10正式矩陣寫成成功結果；
- 沒有執行任何會改變staging的操作。

## 7. 修改範圍與行數

預期只修改：

```text
benchmarks/engine_tail_telemetry.hpp
benchmarks/engine_tail_telemetry.cpp
benchmarks/check_engine_tail_telemetry.cmake
tests/unit/engine_tail_telemetry_test.cpp
docs/wal-sync-storage-tail-root-cause-analysis-benchmark-report.md
```

若採用上述boundary gate，通常不需要再修改`order_book_benchmark.cpp`或public header。預估程式與測試
約50--85行異動，blocked report約80--130行，合計約130--215行異動。若實作需要修改production
WAL／Engine、CSV schema、RuntimeConfig或新增通用sampling framework，代表已超出本次必要範圍，應停止並
重新review。
