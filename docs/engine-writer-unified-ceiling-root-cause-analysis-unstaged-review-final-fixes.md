# Engine Writer 統一 ceiling／根因分析 unstaged review 最終必要修正

## 1. 目的與結論

本文件記錄對目前 unstaged changes 再次 review 後，仍需完成的最小修正。既有 runner 的核心行為已符合
`docs/engine-writer-unified-ceiling-root-cause-analysis-design-review.md`：decimal-safe command count、
frontier duration exclusion、固定六欄 TSV、tail attribution cache 與 malformed CSV invalidation 均不需再改。

剩餘問題只涉及 test-only matrix override 與 contract test 覆蓋不足；不需要修改 production C++、benchmark
workload、CMake、公開 API、WAL format、durability、thread model 或正式 campaign defaults。

本輪只允許修改：

- `benchmarks/run_engine_writer_diagnostics.sh` 的 test-only override 清單與 test-mode assignment；
- `benchmarks/test_engine_writer_diagnostics_runner.sh` 的必要 assertions。

既有 design／procedure 已描述正確的目標契約，本輪不需再次修改其內容。

## 2. 恢復 pipeline／WAL 的 test-only round override

### 問題

目前 unstaged changes 移除了：

```text
ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS
ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS
```

這不屬於本輪六項功能修正，並造成兩個回歸：

1. fake contract test 在 test mode 也會執行正式的五輪 pipeline 與 WAL matrix，增加不必要的測試時間；
2. 未啟用 test mode 時，上述既有 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` 變數不再被拒絕，而是被靜默忽略，
   與「test override 必須受 test mode 保護」的契約不一致。

### 必要修改

在 `TEST_OVERRIDE_NAMES` 恢復兩個名稱：

```bash
ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS
ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS
```

在 `TEST_MODE == 1` 區塊恢復：

```bash
PIPELINE_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_PIPELINE_ROUNDS:-1}
WAL_ROUNDS=${ENGINE_WRITER_DIAGNOSTICS_TEST_WAL_ROUNDS:-1}
```

正式模式的 `PIPELINE_ROUNDS=5` 與 `WAL_ROUNDS=5` 不變；不增加 production CLI option，也不讓正式
procedure 使用這些環境變數。

### 驗收

- test mode 未指定 override 時，pipeline 與 WAL 各執行一輪；
- 非 test mode 只要設定其中任一變數，runner 在建立 run root 前以 usage error 拒絕；
- 正式模式仍固定各五輪。

## 3. 獨立驗證 scenario override guard

### 問題

目前同一次測試同時設定：

```text
ENGINE_WRITER_DIAGNOSTICS_TEST_MIN_DURATION_MS
ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO
```

即使未來 `SCENARIO` 從 `TEST_OVERRIDE_NAMES` 遺失，測試仍會因 `MIN_DURATION_MS` 而通過，形成 false
positive，無法證明本輪新增的 scenario guard 有效。

### 必要修改

將既有 non-test-mode override 測試改成只設定：

```bash
ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO=normal \
  "$RUNNER" --dry-run --binary=/bin/true --fio-bs=4096 ...
```

保留既有 exit status `2` 與訊息
`test overrides require ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1` assertions。不要再為同一條 guard 建立重複
scenario。

### 驗收

- 單獨設定 `ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO` 且未啟用 test mode 時必須失敗；
- 測試不得建立 campaign run root，也不得啟動 fake benchmark。

## 4. 補齊 failure contract assertions

### 原則

只補 assertions，不增加新的正式 campaign case，也不修改 runner 的 production 行為。每個既有 fake
scenario 至少證明 process status、terminal result、明確 reason，以及 benchmark invocation 為零或非零；
需要 case-specific artifact 的路徑，同時驗證該 artifact。

### 4.1 Preflight、identity 與 observer failure

在既有 assertions 上補充：

- `preflight_failure`：`logs/result.txt` 包含 `reason=initial-preflight-busy`，且 benchmark count 為零；
- `identity_change`：包含 `reason=artifact-identity-changed`，benchmark count 恰為一，證明未啟動下一個 case；
- `observer_early_exit`：包含 `reason=observer-failed`、benchmark invocation 非零，並保留既有 PID/status/wait
  evidence assertion；
- `command_mismatch`：保留 calibration mismatch reason，並驗證 benchmark invocation 非零。

`benchmark_nonzero` 與 `benchmark_timeout` 既有不同 reason及一次／零次 invocation assertions維持不變。

### 4.2 Duration 與 CV exclusion

`duration_short` 除既有六欄 exclusion row 外，補充：

```text
frontier-candidates.txt 為空
engine-scan-r*-g256-d200 的 invocation count 等於 FRONTIER_ROUNDS
```

如此才能證明短 duration 是完成 scan 後由 selection 排除，而不是在第一輪提前終止。

`cv_over` 補充：

- terminal reason 為 `frontier-selection-failed`；
- `engine-scan-r*-g256-d200` invocation count 為三；
- `frontier-selection.tsv` 保留 `cv-over-5-percent`，沿用既有 assertion。

不要新增新的 duration 或 CV scenario。

### 4.3 Tail cache 與 selection artifact

在既有 `tail_fallback` scenario 補充：

- `tail-gate-g256-d200-validation.txt` 非空，證明 rejected evidence仍保留；
- 已被第一輪拒絕的 `g256-d200` invocation count 維持二，不因 comparison 重跑；
- 完成三輪 off/on 的 selected `g4096-d1000` invocation count恰為六，不因 comparison 再增加；
- `frontier-selection.txt` 仍包含 `best_case=` 與 `best_median_rps=`；
- TSV 繼續只驗證固定六欄，且不得包含上述兩個摘要 key。

這些 assertions 只驗證既有 cache 與 artifact 行為，不新增另一份 cache、TSV 或 summary。

### 4.4 Drain、bias 與 malformed CSV

對三個既有 scenario 補充最小 terminal／invocation contract：

- `drain_nonzero`：terminal `reason=no-sustainable-frontier-case`、case summary
  `reason=drain-not-empty`、benchmark invocation 非零；
- `tail_bias`：terminal `reason=no-sustainable-frontier-case`、case summary
  `reason=tail-observer-bias`、benchmark invocation 非零；
- `malformed_tail_csv`：保留 terminal `reason=tail-attribution-g256-d200-20` 與 case summary
  `reason=malformed-tail-csv`，並驗證 benchmark invocation 非零。

positive-slope 已由 `tail_fallback` 的 summary、comparison row與 invocation count涵蓋，不新增獨立 scenario。

## 5. 不應進行的修改

本輪不得：

- 修改 matching、Engine、WAL、Publisher、Completion 或 recovery production code；
- 修改 benchmark輸出schema、workload matrix或正式round數；
- 新增 telemetry、metrics、threshold、效能最佳化或根因結論；
- 新增 CMake target或第三方 dependency；
- 重寫已正確的 decimal helper、tail cache、CSV validation 或 TSV selection 邏輯；
- 執行 `git add`、`git reset`、`git restore --staged`、`git commit` 或其他 staging mutation。

## 6. 預估修改規模

| 檔案 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| `benchmarks/run_engine_writer_diagnostics.sh` | 恢復兩個 override名稱與兩個 test-mode assignment | 4 |
| `benchmarks/test_engine_writer_diagnostics_runner.sh` | scenario guard及缺少的 contract assertions | 15--25 |
| production C++／CMake／design／procedure | 不修改 | 0 |

總計約 19--29 行修改／新增。若需要修改 production code、CMake 或增加新 scenario，代表已超出本文件範圍，
應先重新 review。

## 7. 驗證方式

修改完成後依序執行：

```bash
bash -n benchmarks/run_engine_writer_diagnostics.sh
bash -n benchmarks/test_engine_writer_diagnostics_runner.sh
bash benchmarks/test_engine_writer_diagnostics_runner.sh
git diff --check
git diff --cached --check
```

測試前後另以 read-only 方式比對：

```bash
git diff --cached --binary | sha256sum
```

前後 hash 必須完全一致。contract test 只能使用 repository 外的 `mktemp` fake environment，不得啟動真實
benchmark campaign、perf、fio、mpstat 或 iostat。

## 8. 完成定義

- test mode 可將 pipeline／WAL matrix各縮成一輪，正式模式仍固定五輪；
- 所有既有 test override在非 test mode 下都被拒絕；
- scenario guard由獨立測試證明，不依賴另一個 override；
- duration exclusion證明所有預定 scan完成且沒有候選；
- rejected與selected tail case均證明未在comparison階段重跑；
- selection文字摘要與固定六欄TSV各自維持單一責任；
- 既有 failure scenario 均驗證 terminal reason、invocation與必要 artifact；
- production行為、正式defaults、設計範圍與Git staging維持不變。
