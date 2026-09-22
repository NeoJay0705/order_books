# StateMachine／WAL Component Ceiling benchmark follow-up 必要修正

## 1. 結論與範圍

目前實作方向正確，fresh-WAL epoch、單一 target group rotation diagnostic、measured resource counter、exact
WAL byte plan 與 replay 驗證都應保留。但 review 後仍有四類必要缺口，若不修正，runner 可能接受語意不完整的
rotation artifact，或在失敗時先刪除最需要保留的 WAL 證據；現有測試也不足以防止這些契約回歸。

本輪只修改：

- `benchmarks/order_book_benchmark.cpp`；
- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- `benchmarks/CMakeLists.txt`、既有的 `tests/integration/persistence_test.cpp` 及必要的 benchmark-local
  輕量測試；
- `docs/state-machine-wal-component-ceiling-design-review.md`；
- `docs/state-machine-wal-component-ceiling-benchmark-procedure.md`。

不修改 production WAL format、segment size、rotation／fsync 順序、StateMachine、Engine、Publisher、Completion
worker 或 public API；不新增效能優化、`perf`、`fio`、O_DIRECT、io_uring 或正式長時間 campaign。

## 2. 修正一：完整驗證 rotation diagnostic 契約

### 2.1 Benchmark 端驗證 segment position

`run_wal_write_ceiling()`的 rotation diagnostic 已驗證 rotation count 與 WAL byte delta，但仍需在 target append
後明確驗證 segment id 與offset，不只把它們輸出。

control 必須同時滿足：

```text
after.segment_count == before.segment_count
after.active_engine_seq == before.active_engine_seq
after.active_offset == before.active_offset + frame_bytes
```

trigger 必須同時滿足：

```text
after.segment_count == before.segment_count + 1
after.active_engine_seq == target_sequence
after.active_offset == initial_segment_header_bytes + frame_bytes
```

所有加法先做 checked arithmetic。任一條件失敗時，以明確錯誤碼
`rotation_target_position_mismatch`停止，不輸出成功資料。

這些檢查只增加 benchmark diagnostic correctness gate，不改變 WAL 實作或 durability 行為。

### 2.2 補齊 target group 的 profile subphase

目前 diagnostic 已收集 `prepare`、`plan_copy`、`write`與`publish`，但輸出缺少部分欄位。stdout 至少補齊：

```text
wal_prepare_group_total_us
wal_plan_copy_group_total_us
wal_rotation_group_total_us
wal_write_group_total_us
wal_publish_group_total_us
rotation_sync_total_us
rotation_header_write_total_us
rotation_header_sync_total_us
rotation_directory_sync_total_us
```

上述數值只能來自唯一的 measured target group；setup-fill 不得進入 profile aggregate。

### 2.3 Runner 驗證 diagnostic mode 與 byte plan

不要只依 `rotation_scope`推斷 output type。為`component_validate_output()`增加可選的
`expected_rotation_case`參數，rotation control／trigger呼叫時分別傳入`control`與`trigger`。

rotation diagnostic 必須驗證：

- stdout名稱為`wal_rotation_diagnostic`；
- `case`恰好出現一次，且等於預期mode；
- `groups=1`、`commands=1`、`profiled_groups=1`、`profiled_commands=1`；
- `latency_sample_stride=1`、`append_latency_sample_count=1`；
- `wal_byte_plan_verified=true`恰好出現一次；
- `planned_wal_bytes_delta == wal_bytes_delta`；
- control為精確0次rotation且`rotation_triggered=false`；
- trigger為精確1次rotation且`rotation_triggered=true`；
- segment id／offset符合第2.1節的mode規則；
- 第2.2節所有profile subphase欄位各出現一次，且為非負有限數字；
- `replay_verified=true`。

其中header大小不要在runner硬編碼推算。benchmark應輸出
`segment_header_bytes=<exact integer>`；runner使用該欄位驗證trigger的offset與byte plan。control則直接以
`before_offset + frame_bytes_per_command`驗證after offset。

任何不一致都以`invalid-run`立即停止，不得降級成CV rejected或繼續產生正式ceiling。

### 2.4 Rotation TSV

保留現有22欄raw TSV即可，不必加入所有一般profile欄位。stdout是完整profile契約，TSV只保留報告需要的
rotation attribution摘要。只需確保寫入TSV前已通過第2.3節全部驗證。

## 3. 修正二：語意驗證成功後才刪除case data

目前`component_run_binary()`在binary、observer與基本artifact成功後立即呼叫`component_safe_remove_data()`；但
counter、output schema、rotation position、duration與replay欄位的runner驗證發生在函式回傳之後。這會使
binary exit 0但語意驗證失敗的case先失去WAL資料。

最小調整方式：

1. 從`component_run_binary()`移除成功路徑上的`component_safe_remove_data "$data_dir"`。
2. 新增小型helper：

```bash
component_finalize_case_data() {
  local label=$1
  component_safe_remove_data "$RUN_ROOT/data/component-${label}"
}
```

3. 每個case只有在下列條件全部通過後才呼叫helper：
   - binary及observer成功；
   - `component_validate_output()`成功；
   - 該case要求的duration／counter／rotation gate成功；
   - stdout已確認`correctness_verified=true`或`replay_verified=true`。
4. calibration case也要先完成其command、rate及bytes欄位驗證，再刪除data；calibration是短校準，不套用正式
   case的30秒duration gate。
5. 任一失敗路徑不得刪除該case data；`campaign_fail`保留現有run root供診斷。

不要改成campaign結束後使用glob批次刪除。每次只解析並刪除本次run root下的精確label路徑，保留既有
`component_safe_remove_data()`的路徑安全檢查。

## 4. 修正三：補齊runner failure contracts

### 4.1 Fake benchmark scenarios

擴充`benchmarks/test_engine_writer_diagnostics_runner.sh`的fake benchmark，只加入下列必要scenario：

| Scenario | 注入錯誤 | 預期結果 |
| --- | --- | --- |
| `component_wrong_no_rotation_name` | append no-rotation輸出`wal_write_ceiling` | `invalid-run` |
| `component_resource_missing` | 缺少一個required resource欄位 | `invalid-run` |
| `component_resource_duplicate` | 重複一個required resource欄位 | `invalid-run` |
| `component_resource_na` | required resource輸出`na` | `invalid-run` |
| `component_write_call_mismatch` | `measured_wal_write_calls != measured_syscw` | `invalid-run` |
| `component_byte_plan_mismatch` | planned bytes不等於actual bytes | `invalid-run` |
| `component_rotation_control_nonzero` | control回報1次rotation | `invalid-run` |
| `component_rotation_trigger_zero` | trigger回報0次rotation | `invalid-run` |
| `component_rotation_trigger_multiple` | trigger回報2次rotation | `invalid-run` |
| `component_rotation_multiple_groups` | diagnostic回報groups/profiled_groups大於1 | `invalid-run` |
| `component_rotation_case_mismatch` | control輸出`case=trigger`或反之 | `invalid-run` |
| `component_rotation_position_mismatch` | id或offset與mode不一致 | `invalid-run` |

每個scenario只在最早相關case注入錯誤，以縮短contract test。測試至少驗證：

```text
runner exit status != 0
logs/result.txt contains result=invalid-run
reason points to the failed case
logs/result.txt does not contain valid-component-ceiling
failed case data directory still exists
later formal phases were not executed
```

直接呼叫`component_validate_measured_resources()`的輕量unit-style檢查可保留，但不能取代上述完整runner流程。

### 4.2 Fake output完整性

happy-path fake output補上：

- `case=control|trigger`；
- `segment_header_bytes`；
- `frame_bytes_per_command`；
- rotation diagnostic完整profile subphase；
- 所有required resource與byte-plan欄位各一次；
- 每筆fake output結尾newline。

不要讓fake output使用與正式binary不同的欄位名稱，避免contract test只驗證fake自身格式。

## 5. 修正四：補齊輕量CTest

### 5.1 Bounded latency sampler

現有`latency_sample_stride()`與`observe_latency()`位於anonymous namespace，尚無synthetic boundary test。
最小做法是將兩者及`LatencyAggregate`移到benchmark-private header，例如：

```text
benchmarks/wal_latency_sampler.hpp
```

該header不得安裝、不得加入public library include path，只供benchmark及小型test executable使用。測試固定驗證：

- group count小於及等於1,000,000時stride為1；
- group count為1,000,001時stride為2；
- observe超過上限時sample vector仍有界；
- `observed`與`total_ns`涵蓋全部輸入；
- `max_ns`等於全量最大值，不受sampling影響；
- aggregate overflow回傳失敗。

測試使用synthetic integers，不建立WAL、不寫大量檔案。

### 5.2 No-rotation smoke

擴充現有CTest regex或改用小型CMake驗證script，確認：

- output name正確；
- planned／actual bytes相等；
- `measured_rusage_valid`、`measured_io_valid`、`measured_meminfo_valid`各一次且為true；
- required resource欄位不是`na`；
- rotation為0且replay通過。

單一長regex不容易驗證欄位唯一性；建議沿用runner的field extraction規則或新增benchmark-local CMake script，
不要在CMakeLists複製大量解析邏輯。

### 5.3 Rotation CLI negative tests

保留既有invalid value與wrong workload測試，另以獨立case覆蓋：

- `--wal-sync=per_group`；
- `--wal-phase-profile=off`；
- `--wal-group-size`不是1；
- `--iterations`不是1；
- 同時指定`--wal-no-rotation-epoch-commands`。

每個case都要求exit code 2及
`error_code=wal_rotation_diagnostic_requires_b1_profiled_single_group`。這些是CLI契約測試，不執行WAL workload。

### 5.4 Rotation subphase correctness

沿用既有small-segment WAL測試能力，驗證profile中的rotation、old-segment sync、header write／sync及directory sync
counter存在，並確認replay正確。不要讓CTest每次建立完整256 MiB segment；production-size control／trigger仍只保留為
人工或正式campaign前的一輪integration gate。

## 6. 文件同步

在設計與操作文件只補兩項語意：

1. `measured_wal_write_calls`是`/proc/self/io` measured window的write-family syscall數，正式Linux runner要求它
   等於`measured_syscw`；`profiled_data_write_calls`則是WAL profile中的data-chunk write數，兩者不可互換。
2. case data只有在runner完成語意驗證後才可刪除；binary exit 0但schema／counter／position gate失敗時仍須保留。

不改寫既有benchmark report，也不把本輪correctness修正描述成效能提升。

## 7. 驗證順序

完成修改後依序執行：

```bash
bash -n benchmarks/run_engine_writer_diagnostics.sh
bash -n benchmarks/test_engine_writer_diagnostics_runner.sh

cmake --build --preset release-benchmark --parallel 4
ctest --test-dir build/ReleaseBenchmark --output-on-failure

git diff --check
git diff --cached --check
```

再以新的repository外暫存目錄各執行一次production-size control與trigger integration gate，確認exact 0／1
rotation、完整subphase、byte plan、position與replay。這不是正式壓測，不套用30秒duration gate。

執行前後記錄：

```bash
git diff --cached --binary | sha256sum
```

hash必須完全相同；不得執行`git add`、`git reset`、`git restore --staged`或其他改變staging的操作。

## 8. 完成條件

只有以下條件全部成立才算完成：

- rotation diagnostic mode、group count、profile count、byte plan與segment position均由benchmark及runner驗證；
- diagnostic stdout具有必要的plan/copy與publish subphase；
- runner只在所有語意gate通過後刪除case data，失敗case資料保留；
- 所列runner failure scenarios全部得到`invalid-run`且不能產生valid ceiling；
- bounded sampler、no-rotation resource contract、rotation subphase及所有CLI互斥條件都有輕量測試；
- build、完整CTest、runner contract與一輪control／trigger integration gate通過；
- production程式與public API沒有變更；
- staged diff hash在修改前後一致。

預估必要修改量約180至260行，主要為failure-contract與sampler測試；runtime／runner邏輯約40至70行。不要為降低
重複而進行額外重構，也不要趁本輪加入新的benchmark matrix或效能優化。
