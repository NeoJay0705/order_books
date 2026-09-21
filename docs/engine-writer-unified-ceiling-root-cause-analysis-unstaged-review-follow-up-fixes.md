# Engine Writer 統一 ceiling／根因分析 unstaged review follow-up 必要修正

## 1. 目的與結論

本文件記錄對目前 unstaged changes 的 follow-up review 結果，作為下一輪實作的唯一修正範圍。現有
方向正確，且不需要修改 production C++；但仍有六項會破壞正式 campaign 證據完整性或測試契約的問題。
這些項目都直接對應既有設計與
`docs/engine-writer-unified-ceiling-root-cause-analysis-unstaged-review-fixes.md`，不是新的效能功能。

本輪只允許修改：

- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- 與實際 runner 行為直接相關的 design／procedure 文字。

本輪不得修改：

- matching、Engine、WAL、Publisher、Completion、recovery 等 production C++；
- public API、WAL format、durability、group-commit 或 thread model；
- production defaults、CPU affinity、scheduler、filesystem 或 kernel policy；
- benchmark workload、CMake wiring或第三方 dependency。

## 2. 修正順序

依下列順序修改，避免測試先固化錯誤 artifact：

1. 修正 uint64 command count 計算；
2. 讓 frontier duration failure 進入 selection gate；
3. 維持 `frontier-selection.tsv` 固定 schema；
4. 快取 tail attribution 結果，禁止重跑與覆寫；
5. 強化 tail CSV 結構與數字驗證；
6. 補齊 fake-command contract assertions；
7. 同步 design／procedure 的事實描述。

## 3. 使用十進位字串安全計算 command count

### 問題

目前 calibration 與 formal expected commands 使用 `awk` double 執行乘二及 uint64 上限判斷。double 無法
精確表示 uint64 邊界；例如 `9223372036854775807 * 2` 可能輸出
`18446744073709551616`，且與 `UINT64_MAX` 的比較也可能因同一個浮點表示而無法偵測 overflow。

### 必要修改

1. 在 runner 增加單一 helper，例如 `double_decimal_u64 VALUE`：

   - 只接受十進位正整數；
   - 移除多餘 leading zero；
   - 先以字串長度與逐字典序比較，要求輸入不大於
     `9223372036854775807`，即 `floor(UINT64_MAX / 2)`；
   - 從最低位逐位乘二並處理 carry；
   - 輸出精確十進位字串；
   - 超過界限或格式錯誤時回傳非零。

2. calibration 的 `expected_commands` 必須由此 helper 計算，不再以 `awk` 乘二。

3. formal iteration 計算仍可用既有 `awk` 做 duration 比例與 `ceil`，但產生結果後必須：

   - 通過正整數格式檢查；
   - 再呼叫 `double_decimal_u64 "$formal_iterations"`，證明正式 command count 不溢位；
   - 失敗時以 `result=invalid-run`、`reason=calibration-iterations-overflow-<case>` 結束。

4. `validate_frontier_case()` 與 `run_tail_attribution_impl()` 先從 frozen plan 讀取
   `formal_iterations` 字串，再用同一 helper 推導 expected commands。不可在 `awk` 中寫 `$5 * 2`。

5. 不增加 `bc`、Python 或其他 runtime dependency。

### 測試

- 驗證 `10 -> 20`；
- 驗證 `9223372036854775807 -> 18446744073709551614`；
- 驗證 `9223372036854775808` 被拒絕；
- 保留 calibration 少一筆或多一筆即停止 campaign 的 scenario。

## 4. Frontier duration failure 必須成為 case exclusion

### 問題

`run_case()` 目前在正式 Engine scan 結束後立即檢查 30 秒 duration 並終止整個 campaign。因此
`validate_frontier_case()` 的 `duration-under-30s` 分支無法執行，`frontier-selection.tsv` 也無法保存所有
case 的 valid／excluded 結果。這與「先完成 frontier，再排除無效 case」的設計不一致。

### 必要修改

1. 為 `run_engine_case()` 增加只供 runner 內部使用的 `allow_short` 參數，預設為 `0`。

2. `run_engine_frontier()` 呼叫時將 `allow_short=1` 傳入，使 scan 即使不足 30 秒仍保留 status、stdout、
GNU time、normalized row與observer artifact。

3. 只有 `validate_frontier_case()` 決定該 scan case 是否以 `duration-under-30s` 排除。

4. calibration 已使用 `--runner-allow-short`，維持不變；tail attribution、confirmation、writer profile
與 perf record 不得放寬正式 duration gate。

5. 若所有 case 都被排除，terminal result 維持 `invalid-run`／`frontier-selection-failed`；個別原因保存在
固定 schema 的 selection artifact。

### 測試

`duration_short` scenario 應完整走到 selection，並驗證：

- terminal result 為 `invalid-run`；
- `frontier-selection.tsv` 對該 case 寫入 `excluded` 與 `duration-under-30s`；
- 不把該 case 寫入 `frontier-candidates.txt`；
- benchmark invocation log 證明所有預定 scan round 已完成。

## 5. `frontier-selection.tsv` 必須維持單一固定 schema

### 問題

目前 TSV 在六欄 header與case rows之後，另追加 `best_case=...` 和 `best_median_rps=...` 單欄文字。這使
TSV 不再能由固定欄位 parser 安全讀取。

### 必要修改

1. `frontier-selection.tsv` 只能包含：

   ```text
   case<TAB>status<TAB>median_rps<TAB>cv_percent<TAB>rank<TAB>reason
   ```

2. 每個 production frontier case恰好一列；無有效 case 時也不得追加其他格式。

3. `best_case`、`best_median_rps`、tail rejection、selected sustainable case與tail comparison只寫入
`frontier-selection.txt`。

4. 不新增另一份重複 TSV。

### 測試

- 逐列驗證 TSV 恰好六欄；
- 驗證 row count 等於 test case數加header；
- 驗證 TSV 不含 `best_case=` 或 `best_median_rps=`；
- 驗證 `.txt` 仍保留上述摘要。

## 6. Tail attribution 結果必須快取並重用

### 問題

目前 `TAIL_COMPARISON_SEEN` 只記錄 comparison loop 內的重複 case，沒有記錄 candidate fallback 已執行的
case。若 default或`g4096-d1000`先在fallback被拒絕，comparison階段會重新執行六輪off/on、覆寫原本的
validation／summary，並增加數分鐘不必要負載。

### 必要修改

1. 在 tail wrapper 附近建立全域 associative map：

   ```text
   TAIL_ATTRIBUTION_STATUS_BY_CASE[case]=0|10|11|20
   ```

2. `run_tail_attribution CASE` 的行為改為：

   - 若 map 已有該 case，直接回傳保存的 status，不執行 benchmark；
   - 否則保存目前 `OBSERVER_MODE`、以 observer-off 執行 implementation、恢復原 mode；
   - 將 implementation status存入map後回傳。

3. `20` 等 invalid status 雖通常會立即終止 campaign，仍保存以保持 wrapper 契約一致。

4. comparison loop 可保留避免身份重合的 local `seen` map，但不得用它取代跨階段 status cache。

5. 重用結果時只在 `frontier-selection.txt` 追加 comparison 判斷，不重寫
`tail-gate-<case>-validation.txt` 或 `.summary`。

### 測試

在 `tail_fallback` scenario 中記錄每個 label 的 invocation count，驗證：

- 被第一輪 fallback 拒絕的 default case 在 comparison 階段沒有再次執行；
- rejected summary與validation檔案仍存在；
- comparison row使用先前保存的 rejection reason；
- selected case不重跑。

## 7. Tail CSV 不得把非數字資料轉成零

### 問題

目前 AWK 使用 `$3 + 0` 和 `$6 + 0`。非數字內容會被隱式轉成零，十筆 malformed row 因而可能通過
sample count、quartile與slope gate。

### 必要修改

1. 驗證第一列完全符合既有八欄 header。

2. 對每一筆納入計算的 `state,measured` row 驗證：

   - 欄位數為8；
   - `elapsed_us` 與 `publisher_lag_events` 為非負十進位整數；
   - elapsed time 可形成正的 regression denominator；
   - 至少10筆合法 measured state samples。

3. 任一 measured row 格式錯誤時立即讓 AWK 回傳專用 nonzero status，外層寫入
`status=invalid`、`reason=malformed-tail-csv`，不得略過該row或將其視為零。

4. 不擴大到新的統計門檻；quartile與30秒slope限制維持原設計。

### 測試

增加 `malformed_tail_csv` scenario，至少將一筆 measured row 的 elapsed或lag改成非數字，驗證 campaign
以 `invalid-run` 終止，並保留case-specific invalid summary。

## 8. Contract test 必須驗證宣告的 failure contract

### 問題

目前 fake perf 只記錄event list而沒有驗證；`ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO` 未被納入test-only
override拒絕清單；部分scenario也只檢查`result=`，未確認`reason=`與benchmark invocation log。

### 必要修改

1. 將 `ENGINE_WRITER_DIAGNOSTICS_TEST_SCENARIO` 加入已知 test override；未啟用 test mode時必須在建立
run root 前拒絕。

2. fake `perf stat` 驗證event list恰好為runner設計的七個events；不一致時回傳非零。成功scenario另
assert `perf.log` 中保存相同event list。

3. `run_case()` 對 benchmark nonzero／timeout在`logs/result.txt`增加明確reason：

   - status 124：`benchmark-timeout`；
   - 其他非零：`benchmark-exit-nonzero`；
   - 原始 exit status仍保留。

4. 每個 fake scenario 最少驗證：

   - process exit status；
   - terminal `result=`；
   - 明確 `reason=`；
   - benchmark invocation應為零或非零；
   - 該failure path要求的summary／validation artifact。

5. 增加或補齊下列 assertions，不增加新的正式 campaign case：

   - nonzero與timeout理由不同；
   - preflight／perf capability failure沒有啟動benchmark；
   - identity change後沒有啟動下一個case；
   - malformed CSV為invalid而非sustainability rejection；
   - duration exclusion、CV exclusion、drain、slope及bias各保留對應reason；
   - Git cached diff hash前後一致。

6. 測試仍只能使用repository外的`mktemp`目錄與fake commands，不得啟動真實perf、fio、mpstat、iostat
或修改Git index。

## 9. 文件同步

實作完成後，只同步以下既有事實：

- formal expected command count使用decimal-safe乘二；
- frontier duration不足會成為case exclusion，不會在selection前中止；
- `frontier-selection.tsv` 永遠維持六欄case schema；
- 已執行的tail attribution由comparison重用，不重跑、不覆寫；
- malformed tail numeric fields屬`invalid-run`；
- fake contract test驗證exact perf events及每個scenario的result/reason/invocation。

procedure中若仍寫成block-backed storage在run root建立前驗證，需改成：工具、CPU與perf capability在建立
run root前分類；WAL filesystem/device identity在run root建立後分類並寫入`logs/result.txt`。不得寫入尚未
實際取得的benchmark結果、瓶頸結論或production tuning建議。

## 10. 預估修改規模

| 區域 | 必要修改 | 預估行數 |
| --- | --- | ---: |
| runner | decimal-safe count、frontier duration、TSV schema、tail cache、CSV validation、failure reason | 50--75 |
| contract test | uint64邊界、TSV、cache、malformed CSV、perf events與failure assertions | 30--45 |
| design／procedure | 與實際行為同步及storage gate文字修正 | 5--10 |
| production C++／CMake | 不修改 | 0 |

總計約85--130行修改／新增，另刪除2--4行錯誤TSV輸出。超出此範圍前應重新確認是否把新功能帶入
follow-up修正。

## 11. 驗證方式

依序執行：

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

測試前後另以read-only方式比對：

```bash
git diff --cached --binary | sha256sum
```

不得執行`git add`、`git reset`、`git restore --staged`、`git commit`或任何會改變staging的操作。本輪不
執行長時間正式campaign；contract test通過只代表runner orchestration與failure handling符合契約。

## 12. 完成定義

- uint64 command count不依賴浮點邊界比較；
- duration不足的frontier case會留下固定schema exclusion row；
- `frontier-selection.tsv` 每列恰好六欄；
- 同一case的tail attribution在完整campaign中最多執行一次；
- malformed tail CSV不能被轉成合法零值；
- fake tests對必要failure path驗證status、result、reason、invocation與artifact；
- production C++、公開API、durability語意、正式defaults及Git staging皆維持不變。
