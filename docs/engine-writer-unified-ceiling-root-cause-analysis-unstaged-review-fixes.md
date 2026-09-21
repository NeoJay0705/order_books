# Engine Writer 統一 ceiling／根因分析 unstaged review 必要修正

## 1. 目的與結論

本文件記錄 `docs/engine-writer-unified-ceiling-root-cause-analysis-staged-review-fixes.md`
實作後，針對 worktree 差異再次 review 所確認的必要修正。修正目標是讓 runner 的實際行為、失敗分類、
artifact 與測試契約和設計文件一致。

目前修改方向正確，也沒有跨入 production 演算法；但在下列問題修正前，完整 campaign 尚不能作為
sustainable Engine ceiling 或根因歸因的正式證據：

1. tail candidate 失敗時可能永久關閉後續 case 的 external observers；
2. calibration 沒有驗證精確 command count，且 formal iterations 未使用向上取整；
3. frontier validity gate 尚未驗證設計要求的 artifact 與 WAL counters；
4. CV 與 tail rejection 的失敗種類被混在一起，terminal classification 可能錯誤；
5. tail comparison 與 frontier selection artifact 不完整；
6. perf capability failure 沒有輸出機器可解析的 `environment-blocked`；
7. runner test 只檢查文字存在，沒有驗證必要失敗路徑。

本輪允許修改：

- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- 只有在既有 test wiring 無法執行新 contract test 時，才修改 `benchmarks/CMakeLists.txt`；
- 統一 design／procedure 文件中與 runner 行為直接相關的文字。

本輪不得修改：

- matching、Engine、WAL、Publisher、Completion 或 recovery production code；
- public C++ API、WAL format、durability 或 group-commit 語意；
- production defaults、thread affinity、scheduler、filesystem 或 kernel policy；
- 新的 C++ benchmark、hot-path telemetry 或第三方 dependency。

## 2. 修正順序

依下列順序修改，避免先寫測試時把錯誤行為固化：

1. 修正 `OBSERVER_MODE` 的作用域與 tail failure reason；
2. 修正 calibration count 與向上取整；
3. 補齊 frontier artifact/counter validation 與 rejection reason；
4. 產生完整 selection/tail comparison artifact；
5. 統一 environment/invalid terminal classification；
6. 建立 fake-command contract tests；
7. 同步 design/procedure 並執行短測試。

## 3. Tail attribution 必須恢復 observer 狀態

### 問題

`run_tail_attribution()` 會把全域 `OBSERVER_MODE` 設為 `off`，但 artifact、CSV、drain 或解析失敗時會直接
`return 1`。只有走到函式末端才會恢復原值。最高 RPS candidate 若未通過，下一個 candidate 及其後的
confirmation、Writer profile 便可能在 observer 關閉的情況下執行。

目前函式也把所有非零結果都當成「candidate 不 sustainable」：malformed artifact、無法解析與真正的
backlog/bias rejection 無法區分，可能把 `invalid-run` 靜默降級成嘗試下一個 candidate。

### 具體修改

1. 將目前函式拆成兩層：

   ```text
   run_tail_attribution_impl ENTRY
   run_tail_attribution ENTRY
   ```

2. wrapper 保存 `requested_mode`，呼叫 implementation 後無條件恢復：

   ```bash
   run_tail_attribution() {
     local entry=$1
     local requested_mode=$OBSERVER_MODE
     local status=0
     OBSERVER_MODE=off
     run_tail_attribution_impl "$entry" || status=$?
     OBSERVER_MODE=$requested_mode
     return "$status"
   }
   ```

   `run_tail_attribution_impl()` 不得再直接修改全域 `OBSERVER_MODE`。

3. tail implementation 以明確狀態區分：

   - `0`：CSV、drain、slope 與 bias 全部通過；
   - `10`：backlog/drain/slope 不符合 sustainable 條件，可嘗試下一個 candidate；
   - `11`：tail observer bias 超過 3%，資料只能定性使用，可嘗試下一個 candidate；
   - 其他 artifact、解析、command count 或 runner failure：寫入 `invalid-run` 後終止 campaign，不可
     當成 candidate rejection。

4. 每個 candidate 使用獨立 artifact：

   ```text
   derived/tail-gate-<case>-validation.txt
   derived/tail-gate-<case>.summary
   ```

   不再於每個 candidate 開始時截斷共用的 `tail-gate-validation.txt`。若需要總表，在所有 candidate
   完成後由各 case summary 合併，保留先前失敗候選的證據。

5. tail summary 必須保存 `status` 與 `reason`，例如：

   ```text
   case=g8192-d1000
   status=rejected
   reason=positive-backlog-slope
   ```

### 驗收

- 任一 tail candidate 被拒絕後，下一個 candidate 仍以 tail attribution 規定的 observer-off 執行；
- tail attribution 返回後，confirmation/profile 恢復使用使用者原本指定的 observer mode；
- malformed/missing artifact 產生 `invalid-run`，不會被當成 unsustainable candidate；
- 每個嘗試過的 candidate 都保有獨立 validation 與 summary。

## 4. Calibration 必須驗證精確 commands 並向上取整

### 問題

目前只檢查 `commands > 0`。`engine_durable_single_instrument` 每個 iteration 固定產生兩筆 measured
commands，因此 calibration 若輸出截斷、讀到錯誤欄位或 count 不完整，仍可能產生 iteration plan。

目前 `printf "%.0f"` 是四捨五入，不等同設計要求的 `ceil()`，極端邊界可能少規劃一個 iteration。

### 具體修改

1. 以 overflow-safe 的 `awk` 計算 expected commands，並要求完全相等：

   ```text
   expected_commands = calibration_iterations * 2
   parsed_commands == expected_commands
   ```

   不相等時使用：

   ```text
   result=invalid-run
   reason=calibration-command-count-mismatch-<case>
   ```

2. formal iterations 使用真正的向上取整：

   ```awk
   raw = base * target / elapsed * 1.10
   value = int(raw)
   if (value < raw) ++value
   print value
   ```

3. 驗證 `formal_iterations` 為正整數，且 `formal_iterations * 2` 不溢位 benchmark 使用的
   `std::uint64_t` 範圍；失敗時在正式矩陣開始前分類 `invalid-run`。

4. iteration plan 保存 `calibration_commands`，並由 frozen `formal_iterations * 2` 推導正式 expected
   command count，讓兩個 count gate 都可稽核；不增加新的 benchmark output 或 production counter。

### 驗收

- calibration command count 少一筆或多一筆都會停止 campaign；
- 非整數結果永遠向上取整；
- calibration 仍不列入 ceiling、latency 或 observer median；
- scan、confirmation、tail attribution、profile off/on 仍共用同一份凍結 plan。

## 5. 補齊 frontier validity gate

### 問題

目前 `validate_frontier_case()` 只檢查 status、stdout、RPS、elapsed 與 commands。這不足以證明該輪具有
完整的 durable Engine artifact，也無法排除讀到部分或錯誤 summary 的情況。

### 具體修改

1. 新增只解析 exact key 的 helper，至少取得：

   ```text
   commands
   commands_per_second
   elapsed_ms
   wal_group_commits
   wal_group_commands
   wal_bytes_delta
   ```

   helper 必須只匹配空白邊界後的完整 key，避免 `commands` 誤讀成 `wal_group_commands`。

2. 每輪 scan 驗證下列必要 artifact：

   - `.status` 存在且值為 0；
   - `.stdout`、GNU time 與 normalized `.row` 存在且非空；
   - observer-on 時，perf stat、mpstat、iostat artifact 存在且非空；
   - stderr 只要求存在，不要求非空；成功執行沒有 stderr 是合法情況。

3. 驗證 counters：

   ```text
   commands == frozen_iterations * 2
   wal_group_commands == commands
   commands_per_second > 0
   elapsed_ms >= 30000
   wal_group_commits > 0
   wal_bytes_delta > 0
   ```

   benchmark status 0 繼續代表內建 completion、replay、durability 與 correctness 已通過；runner 不重做
   production correctness 邏輯。

4. `validate_frontier_case()` 不再只回傳通過/失敗。為每個 case 寫入單一且明確的 rejection reason：

   ```text
   missing-artifact
   command-count-mismatch
   counter-invalid
   duration-under-30s
   cv-over-5-percent
   valid
   ```

5. CV 超過 5% 不可直接宣稱 `environment-unstable`。只有既有 external observer artifact 同時有明確、
   預先定義的 host/storage drift 證據時才能使用該分類；本輪不新增事後門檻。沒有第二項證據時分類
   `invalid-run`，reason 為 `cv-over-5-percent`。

6. confirmation 五輪的 CV gate 套用相同分類規則，不再僅因 CV 超標便無條件輸出
   `environment-unstable`。

7. repository identity 與 binary hash 仍由每個 `run_case()` 前後的 `check_frozen_artifacts()` 負責；
   `validate_frontier_case()` 只驗證對應 identity check 已成功完成，不複製 hash 邏輯。

### 驗收

- 缺少 normalized row、GNU time、observer artifact 或必要 WAL counter 時不能進入排序；
- commands 與 frozen plan 不一致時停止，不會只因 RPS 可解析就接受；
- 每個 excluded case 有單一可稽核 reason；
- 沒有 host/storage drift 證據時不會誤報 `environment-unstable`。

## 6. 完成 tail comparison 與 selection artifacts

### 具體修改

1. `frontier-selection.tsv` 使用固定 schema：

   ```text
   case status median_rps cv_percent rank reason
   ```

   所有八個 case 都必須有一列；excluded case 的數值欄位可為 `na`，但 reason 不可省略。

2. 將 valid candidate 依 median RPS 降冪排序後才填入 rank。`frontier-selection.txt` 保存：

   - 完整候選排序；
   - median RPS 與 CV；
   - excluded case 與 reason；
   - 最終 sustainable case；
   - 被 tail gate 拒絕的較高 RPS candidate 與 reason。

3. 選出 sustainable ceiling 後，對下列唯一 case 保存 tail comparison：

   ```text
   production default（目前為 g256-d200）
   g4096-d1000
   selected sustainable ceiling
   ```

   若 case 身份重合，不重跑。若某 case 已在 candidate fallback 過程執行過，直接重用其 artifact。

4. default 與 `g4096/d1000` 的額外 tail comparison 只供比較，不改寫 ceiling median，也不擴大八組正式
   frontier。其 artifact/解析錯誤仍是 `invalid-run`；單純 backlog 或 bias 不通過則在 comparison summary
   記錄 `rejected`，不把 tail-on RPS混入正式結果。

5. 使用 associative map 記錄已執行的 case，避免重複執行同一組 off/on attribution。

### 驗收

- `frontier-selection.txt` 可單獨回答每個 case 為何入選或被排除；
- production default、g4096/d1000、ceiling 均有可用或明確 rejected 的 tail summary；
- tail-on RPS 永遠不成為正式 ceiling median；
- 沒有因比較需求新增新的 group size/delay case。

## 7. Terminal classification 必須可由機器解析

### 問題

perf capability precheck 目前只在錯誤字串中寫「classify ...」，但沒有真正輸出
`result=environment-blocked`。必要工具、CPU affinity與storage identity失敗也有相同問題。

### 具體修改

1. 增加一個可在 `RUN_ROOT` 建立前使用的 helper：

   ```text
   classified_die RESULT REASON EXIT_STATUS
   ```

   至少向 stderr 輸出：

   ```text
   result=environment-blocked
   reason=perf-events-unavailable
   ```

2. `RUN_ROOT` 建立後仍使用 `campaign_fail()`，並確保 `logs/result.txt` 使用同一 schema。

3. 下列環境問題統一分類 `environment-blocked`：

   - 必要工具不存在；
   - benchmark/observer CPU affinity 不可用或重疊；
   - required perf events 或 call graph 不可用；
   - WAL path 無法對應 block-backed filesystem/device。

4. CLI 拼字、負數或空值仍是 usage error，不能冒充 benchmark environment classification。

5. `preflight-busy`、`observer-biased`、`invalid-run` 與 `inconclusive-attribution` 維持既有語意。

### 驗收

- environment gate 失敗時，stdout/stderr 或 `result.txt` 中一定存在唯一 `result=` 與 `reason=`；
- 不再出現只要求人工「classify」但未真正分類的訊息；
- exit status 保持非零。

## 8. 補足 fake-command contract tests

### 原則

現有測試中的 shell syntax、dry-run、Git index mutation 靜態檢查可以保留，但搜尋函式名稱不能視為行為
測試。新增測試不得真的啟動 perf、fio、mpstat、iostat 或長時間 sleep。

### Test mode

1. runner 只在下列條件成立時接受縮短時間與矩陣的內部 override：

   ```text
   ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1
   ```

2. 測試 override 使用明確的 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` 環境變數，只允許調整：

   - preflight/monitor duration；
   - calibration/frontier/confirmation rounds；
   - 測試用 case 列表；
   - target duration／minimum measured duration；
   - fake environment 專用的 observer/tail bias acceptance limit（正式模式固定為 3%，不可由 production
     CLI 或 procedure 覆寫）。

3. 未設定 test mode 時，只要出現任何 test override，runner 必須在建立 run root 前拒絕。正式 CLI 不新增
   這些選項，production procedure 也不使用它們。

### Fake environment

測試以 `mktemp -d` 建立 repository 外 run parent、fake `PATH` 與 scenario state。fake commands 只輸出
runner 需要的最小合法 schema：

- fake benchmark 根據 scenario 回傳合法 summary、non-zero、timeout-like status、短 duration、不同 CV、
  drain 非零或 positive slope；tail-on 時才建立指定的唯一 CSV；
- fake `perf` 驗證 exact event list，並可模擬 unsupported event、record/report failure；
- fake `mpstat`／`iostat` 可正常等待 TERM或提早退出，並留下 PID/status evidence；
- fake `findmnt`／`lsblk`／`jq` 回傳固定且合法的 block/storage/preflight資料；
- fake `fio` 只建立最小 JSON artifact，不寫大型檔案；
- 真正的 Git index 在測試前後以 `git diff --cached --binary | sha256sum` 比對，不執行任何 index mutation。

### 必要 scenarios

至少執行並 assertion：

1. syntax 與 dry-run 成功；
2. repository 內 run parent 在建立 run root 前被拒絕；
3. preflight failure 時 benchmark invocation count 為 0；
4. benchmark non-zero 與 timeout 都是 `invalid-run`；
5. mpstat/iostat 提早退出時 case 失敗，child 已被 `wait`；
6. required perf event 或 call graph 不可用時為 `environment-blocked`；
7. binary hash或repository identity改變後停止後續case；
8. observer gate執行順序為`off,on / on,off / off,on`；
9. frontier/confirmation為tail-off，tail attribution才有唯一tail output path；
10. duration不足、command count不符、CV超標、drain未歸零、positive slope分別被拒絕；
11. 第一個tail candidate被拒絕後，observer mode會恢復，第二個candidate與後續case模式正確；
12. runner不含`git add`、`git reset`、`git restore --staged`或`git commit`。

測試每個 scenario 必須檢查 exit status、`result=`、`reason=`、benchmark invocation log 與必要 artifact，
不能只 grep runner source。

目前 contract test 以 fake scenario 覆蓋 repository-parent、preflight、non-zero／timeout、observer early
exit、perf unsupported、identity change、duration／CV gate、tail positive slope、drain-not-empty、candidate
fallback 與 tail bias；成功 fallback 另驗證 selection artifact、每個 candidate summary 及 Git index hash
前後一致。fake bias limit 只避免 host shell timing 污染行為契約，不代表正式 3% gate 放寬。

### CMake wiring

若現有 `order_books_engine_writer_diagnostics_runner_dry_run` 已呼叫完整 contract test script，維持原 wiring；
只有需要拆分 timeout 時才新增一個 test。不得把 Linux campaign 加入 macOS 或一般 production build。

## 9. 移除唯一已確認的冗餘

刪除只被賦值、從未讀取的全域 `OBSERVER_FAILURE`：

```text
OBSERVER_FAILURE=0
OBSERVER_FAILURE=$failure
```

`stop_observers()` 直接以 return status 傳遞結果即可。其他本輪新增的 calibration、CV、tail、perf
capability 與 classification 邏輯都有對應需求，不應因縮短程式碼而移除。

## 10. 文件同步

實作完成後只同步既有 design/procedure 的事實：

- tail attribution 的 observer mode 一定恢復，rejected 與 invalid candidate 都有 case-specific summary；
- calibration 使用 exact count 與 ceil；
- frontier artifact/counter gate及明確 rejection reason；
- default、g4096/d1000、ceiling 的唯一 tail comparison；
- environment precheck 的 machine-readable classification；
- contract tests 的 fake-command 範圍。

不得在文件中預先寫入 benchmark 結果、primary bottleneck、production 調校或 1M commands/s 已達成。

## 11. 預估修改規模

| 區域 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| runner | observer作用域、calibration、frontier gate、tail artifacts、classification | 90--135 |
| runner contract tests | fake commands、scenario setup與assertions | 190--260 |
| CMake wiring | 通常不需修改；必要時只拆分test | 0--10 |
| design／procedure | 與實際行為同步 | 15--25 |
| 移除冗餘 | 刪除未使用變數 | 刪除2行 |
| production C++ | 不修改 | 0 |

總計約 300--420 行修改／新增，主要增量來自必要的失敗契約測試。若只修 runner 而不補 contract tests，
約需 90--135 行，但不能視為完成本設計的驗收。

## 12. 驗證方式

修正後依序執行：

```bash
bash -n benchmarks/run_engine_writer_diagnostics.sh
bash -n benchmarks/test_engine_writer_diagnostics_runner.sh
bash benchmarks/test_engine_writer_diagnostics_runner.sh
cmake --build build/ReleaseBenchmark --parallel 2
ctest --test-dir build/ReleaseBenchmark --output-on-failure \
  -R order_books_engine_writer_diagnostics_runner
git diff --check
git diff --cached --check
```

這一輪不執行長時間正式 campaign；contract test 只能證明 orchestration 與 failure handling。實際 ceiling
與根因仍須在符合 procedure 的 dedicated Linux host 上執行完整矩陣。

不得執行 `git add`、`git reset`、`git restore --staged`、`git commit` 或任何會改變 staging 的操作。

## 13. 完成定義

- tail candidate 成功、被拒絕或 artifact invalid 都不會洩漏 observer mode；
- calibration command count、ceil 與 timeout budget均可稽核；
- frontier 每輪 artifact/counter完整，所有排除都有明確 reason；
- CV 不會在缺乏第二項證據時被錯誤分類為 environment instability；
- selection 與 tail comparison artifacts足以重建最終決策；
- environment failure具有機器可解析的terminal classification；
- 所有必要 failure paths由數秒內完成的fake-command tests覆蓋；
- production C++、durability語意、public API與Git staging均維持不變。
