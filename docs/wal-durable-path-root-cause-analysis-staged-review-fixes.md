# WAL Durable Path 根因分析 staged review 必要修正

## 1. 目的與範圍

本文件記錄 `docs/wal-durable-path-root-cause-analysis-design.md` 與目前 staged changes
對照後，仍需完成的最小修正。目標是讓 phase profiling 的量測語意、成功摘要與測試契約
和設計一致。

只修正下列四項：

1. 補齊每個 phase 的 total 輸出；
2. 排除 `wal_append_call` 計時範圍中的 caller-side profile object 初始化；
3. 將 WAL bytes／segment header correctness guard 改為精確驗證；
4. 補上設計已要求、目前尚未被測試鎖定的 CLI、輸出與 persistence 等價性案例。

不修改 WAL format、durability policy、group commit default、production data flow、Publisher、
Completion、public installed API 或效能演算法，也不增加新的 profiling phase。

## 2. 補齊 phase total

### 問題

`benchmarks/order_book_benchmark.cpp` 的 profile summary 目前只有 p50、p99、p99.9 與 max，
但設計要求每個 timing phase 至少輸出 total、p50、p99、p99.9 與 max。Share 不能取代 total，
因為報告仍需用原始累計時間核對 denominator 與外部觀測。

### 修改方法

1. 在 profile summary 一次計算並保存下列 checked totals：
   - `fixture_build_ns`、`wal_append_call_ns`；
   - `lock_wait_ns`、`prepare_ns`、`plan_copy_ns`；
   - `rotation_ns`、`write_ns`、`publish_ns`；
   - `sync_ns`、`total_ns`。
2. 全部使用既有 `sum_samples()`。任何 overflow 都回報
   `workload=wal_write_ceiling phase=report error_code=profile_phase_sum_overflow`，清理資料目錄並
   non-zero 結束。
3. denominator、phase sum、share 與輸出共用這組已驗證的 totals，不要在輸出階段反覆使用
   `sum_samples(...).value()`。
4. Profile 開啟時增加以下 machine-readable 欄位，單位固定為 microseconds：

   ```text
   fixture_build_group_total_us
   wal_append_call_group_total_us
   wal_lock_wait_group_total_us
   wal_prepare_group_total_us
   wal_plan_copy_group_total_us
   wal_rotation_group_total_us
   wal_write_group_total_us
   wal_publish_group_total_us
   sync_total_us
   group_total_us
   ```

5. 沒有 rotation sample 時，rotation total 輸出 `0`，percentile／max 維持 `na`。
6. `sync=none` 時 `sync_total_us=0`，sync percentile與`sync_share_percent`維持`na`。
7. Profile 關閉時不輸出上述 profile-only totals。

這只補齊輸出契約，不改 throughput、percentile或phase邊界。

## 3. 校正 `wal_append_call` 計時邊界

### 問題

目前先記錄 `wal_call_start`，之後才建立 `WalAppendProfile profile`。因此
`wal_append_call_group_*` 和 share denominator 可能包含 caller-side object 初始化，不完全等於
設計定義的真正 WAL call。

### 修改方法

在 `run_wal_groups()` 中將 profile object 移到 WAL call timer 前：

```cpp
storage::WalAppendProfile profile;
const auto wal_call_start = phase_profile ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
const auto appended = phase_profile
                          ? wal.append_batch_profiled(commands, profile)
                          : wal.append_batch(commands);
```

`append_group_*` 仍從 fixture 建立前量到 append 返回，保留原有結果語意；fixture time 仍不納入
WAL phase share denominator。

## 4. 精確驗證 segment header bytes

### 問題

目前 correctness guard 只驗證沒有 rotation 時 header delta 為零、有 rotation 時 header delta
非零。若 `profiled_frame_bytes` 在 rotation case 少算或多算，這個條件仍可能通過。

### 修改方法

1. `Wal::open()` 成功後、warmup 前記錄 freshly-created WAL 的初始大小：

   ```cpp
   const auto segment_header_bytes = wal->size_bytes();
   ```

   `prepare_data_directory()` 已保證本 workload 從空目錄開始，所以此時大小就是第一個 segment
   header。若為零，回報 setup error並結束。
2. 用 checked multiplication 計算：

   ```text
   expected_profiled_header_bytes = measured_rotations * segment_header_bytes
   ```

   overflow 時回報 `profile_counter_overflow` 並 non-zero 結束。
3. 將目前 zero／non-zero 判斷改成精確相等：

   ```text
   wal_bytes_delta - profiled_frame_bytes == expected_profiled_header_bytes
   ```

4. `profiled_header_bytes` 繼續輸出實際 delta，但只有通過精確驗證後才能輸出成功 summary。
5. 不在 benchmark 寫死 `22`，也不為此增加 installed public API。

## 5. 補齊契約測試

### 5.1 Benchmark CLI 與輸出

在 `benchmarks/CMakeLists.txt` 使用既有 executable 與 CTest properties 補上最小案例，不新增
runner或效能threshold：

1. 增加空值案例：

   ```text
   --workload=wal_write_ceiling --wal-phase-profile=
   ```

   必須 exit code 2 並包含 `error_code=wal_phase_profile_invalid`。
2. 強化 profile-on／sync-none smoke，以穩定的 regex 驗證：
   - `replay_verified=true`、`phase_profile=on`；
   - `profiled_groups=2`、`profiled_commands=4`；
   - 新增的 total 欄位；
   - `sync_samples=0`、sync percentile為`na`、`sync_share_percent=na`。
3. 增加 profile-off smoke，驗證 `phase_profile=off` 與 `replay_verified=true`，並以
   `FAIL_REGULAR_EXPRESSION` 拒絕 `profiled_groups=`、`wal_prepare_group_total_us=` 等profile-only欄位。
4. 增加小型 `sync=per_group` profile-on smoke，使用兩個 measured groups，驗證
   `sync_samples=2` 且 `sync_total_us` 存在。
5. 保留既有 unknown value 與 unsupported workload tests；目前
   `check_invalid_cli.cmake` 已支援第二個 CLI argument，不需再重構測試基礎設施。

CTest 只比對穩定的 key/value 契約，不比對實際時間或RPS，避免形成 flaky performance test。

### 5.2 Persistence 等價性

調整 `tests/integration/persistence_test.cpp`：

1. Same-segment test 建立兩個獨立 temporary WAL directories，一個呼叫 normal
   `append_batch()`，另一個呼叫 `append_batch_profiled()`；兩者使用相同 shard、segment size與
   commands。
2. 比較回傳 position 的 `engine_seq`、`end_offset` 與 segment filename，並比較
   `size_bytes()`；不直接比較包含不同 temporary directory prefix 的完整 path。
3. 兩個 WAL 都 `sync()`、釋放、重新 `Wal::open()` 並 `replay()`，驗證 record數、順序與command
   content完全相同。
4. 保留 profiled path 的 `frame_bytes`、`data_write_calls`、`rotations` 與 durable boundary斷言。
5. Rotation test 除 record count 外，再逐筆驗證 replay ordering／content。
6. Empty-batch test 在呼叫前把 `WalAppendProfile` 九個欄位全部設為非零，呼叫後逐欄驗證為零，
   並同時驗證 WAL size 與 durable position 均未改變。

測試不斷言duration必須大於零，也不加入sleep、mock filesystem或production timing hook。

## 6. 驗證順序

修正完成後依序執行：

1. `git diff --check` 與 `git diff --cached --check`；
2. Release warnings-as-errors build；
3. 全部 CTest；
4. GCC ASan／UBSan correctness run；
5. 可用時補做 Clang warnings-as-errors build；
6. 手動各跑一次 profile off、profile on + sync none、profile on + per-group，檢查 summary key與
   reopen／replay結果。

上述 correctness 驗證完成後，才執行主設計定義的五輪 instrumentation bias check、WAL
group／sync matrix、Engine comparison與外部I/O觀測。正式壓測結果另寫benchmark report，
不混入本文件或主設計文件。

## 7. 行數估算與非目標

- benchmark timing、totals與header guard：約30–45行；
- CMake／CLI output tests：約25–40行；
- persistence equivalence與profile reset tests：約20–30行；
- 合計約75–115行。

這些修改都直接對應既有設計契約。除此之外，不需要重構 WAL、抽出通用 tracing framework、
新增第三方benchmark library、調整production defaults或順帶進行效能優化。
