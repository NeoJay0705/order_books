# StateMachine／WAL Component Ceiling unstaged review 必要修正

## 1. Review 結論與範圍

本文件只描述目前 unstaged changes 中仍會影響 component campaign 正確執行、frozen plan 語意或結果有效性的必要修正。修正範圍限於：

- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- 必要時調整既有 component runner contract test。

不得修改 production matching、WAL durability、WAL format、public API、Publisher 或 Completion worker，也不需要新增 benchmark executable、另一套 runner、`perf` 或 `fio` 流程。

目前 benchmark、runner 與測試的整體架構可保留。尚需修正三個結果有效性問題，並移除一個已確認未使用的 runner-local 變數：

1. rate parser 不接受 benchmark 真實輸出的科學記號；
2. StateMachine／append fixed commands 使用四捨五入，而非設計要求的向上取整；
3. profile 與 fsync counter 只檢查非零或下限，沒有與 frozen plan 作精確比對；
4. `COMPONENT_FSYNC_RATES_BY_BATCH` 只有寫入、沒有讀取，可直接移除。

除上述項目外，不需要重寫現有 component campaign，也不應加入新的量測維度或 production optimization。

## 2. 修正一：接受有限科學記號的數值輸出

### 2.1 問題

`order_books_benchmark` 使用 C++ stream 的預設浮點格式。高 throughput 會輸出科學記號，例如實際 StateMachine 執行可得到：

```text
commands_per_second=1.05936e+06
```

目前 `component_extract_rate()` 只接受：

```text
^[0-9]+([.][0-9]+)?$
```

因此真實 component campaign 會在 StateMachine 或 WAL calibration 被錯誤分類為 `invalid-run/*-calibration-rate-*`。fake benchmark 固定輸出整數，所以既有 contract test 無法發現此問題。

### 2.2 必要修改

在 `benchmarks/run_engine_writer_diagnostics.sh` 增加一個 runner-local numeric validator，例如：

```bash
component_is_non_negative_number() {
  [[ "$1" =~ ^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$ ]]
}
```

使用此 validator 取代下列欄位目前各自的純十進位 regex：

- `component_extract_rate()` 取得的 `commands_per_second`／`service_commands_per_second`；
- append calibration 的 `average_wal_bytes_per_command`；
- fsync calibration 的 `average_wal_bytes_per_command`。

仍須保留以下限制：

- 同一欄位必須恰好出現一次；
- 不接受負數、`nan`、`inf`、空字串或帶尾隨字元的值；
- rate 在進入計算時仍必須大於零；
- 不修改 benchmark 的輸出格式，也不在 runner 中重寫數值。

### 2.3 必要測試

讓 fake benchmark 的 component happy path 至少輸出一次科學記號 rate，例如：

```text
commands_per_second=1.05e+06
service_commands_per_second=1.05e+06
```

contract test 必須證明該 campaign 不會因合法科學記號失敗。另以 helper-level assertion 或 malformed scenario 確認 `nan`、`inf` 與多個同名 rate 欄位仍被拒絕。

## 3. 修正二：fixed command plan 使用真正的 ceiling

### 3.1 問題

StateMachine 與 append plan 目前先執行：

```awk
printf "%.0f\n", value
```

這是四捨五入，不是：

```text
ceil(max_rate * target_duration_seconds)
```

例如 raw StateMachine commands 為 `100.4` 時，現況會先得到 `100`，而正確結果應先向上取得 `101`，再對齊 cycle length 成為 `102`。append 若 raw commands 剛超過 8192 的整數倍，也可能被四捨五入回較小的倍數，使最快 case 的預估時間低於目標。

### 3.2 必要修改

在兩個 plan 計算中先明確向上取整，再沿用既有 alignment：

```awk
BEGIN {
  value = rate * duration / 1000
  minimum = 2 # append case 使用 8192
  if (value < minimum) value = minimum
  rounded = int(value)
  if (rounded < value) ++rounded
  printf "%.0f\n", rounded
}
```

之後保留既有規則：

- StateMachine 向上對齊 2 commands；
- append 向上對齊 8192 commands；
- fsync 已有逐 batch 的 ceiling 計算，不要重寫；
- timeout 與 disk budget gate 保持不變。

不要以縮短目標時間或改用最慢 rate 迴避 timeout；任一慢 case 超出 timeout，仍應依既有設計分類為 `invalid-run/calibration-exceeds-timeout-*`。

### 3.3 必要測試

為 rounding helper 或 frozen plan 加入邊界案例，至少涵蓋：

- raw StateMachine commands 位於偶數邊界稍上方時，結果必須前進到下一個偶數；
- raw append commands 位於 8192 倍數稍上方時，結果必須前進到下一個 8192 倍數；
- 已整除時不得多增加一個 quantum。

測試只需驗證 plan arithmetic，不需要執行真實 30 至 60 秒 benchmark。

## 4. 修正三：精確驗證 WAL、profile 與 sync counters

### 4.1 問題

目前 profile output gate 只要求：

```text
profiled_groups > 0
profiled_commands > 0
profiled_data_write_calls > 0
```

fsync output gate則只要求：

```text
sync_samples >= 1000
```

這無法證明輸出 counter 與 frozen plan 相同。例如 plan 要求 1,500 groups，輸出 `sync_samples=1000` 仍會通過，最後可能錯誤宣告 `valid-component-ceiling`。

### 4.2 必要修改

擴充 `component_validate_output()`，讓 WAL case 同時取得 expected commands 與 expected groups。建議介面：

```bash
component_validate_output output expected_commands kind profile_mode expected_groups
```

驗證規則如下：

- 所有 WAL case：`commands == expected_commands`、`groups == expected_groups`；
- `sync=none`：`sync_samples == 0`；
- `sync=per_group`：`sync_samples == expected_groups`；
- profile-on：`profiled_groups == expected_groups`；
- profile-on：`profiled_commands == expected_commands`；
- profile-on：`profiled_data_write_calls >= expected_groups`，因 segment rotation 可能增加 write call；
- profile-off：不得要求 profile-only fields；
- StateMachine validation 介面與行為保持不變。

可以使用既有帶欄位邊界的 `component_extract_field()` 取得 counter，但每個必要欄位都必須恰好出現一次且是非負整數。不要依賴欄位輸出順序，也不要以 `tail -n 1` 隱藏重複欄位。

各 call site 的 expected groups 為：

- append：`COMPONENT_APPEND_COMMANDS / batch`；
- fsync：`COMPONENT_FSYNC_GROUPS_BY_BATCH[batch]`；
- profile append control：`COMPONENT_APPEND_COMMANDS / 4096`；
- profile append／fsync：沿用對應 frozen plan groups。

精確比對完成後，移除 profile／formal fsync call site 外層只檢查 `sync_samples >= 1000` 的重複 regex。至少 1,000 samples 的條件應在 fsync plan 建立時由 `formal_groups = max(1000, ...)` 保證，output gate只需確認實際值等於 plan。

### 4.3 必要測試

擴充 fake benchmark scenario，至少驗證：

- `profiled_commands` 少一筆時分類為 `invalid-run`；
- `profiled_groups` 與 planned groups 不同時分類為 `invalid-run`；
- `sync_samples` 少於或多於 planned groups 時分類為 `invalid-run`；
- 正常 profile-on、profile-off、append 與 fsync happy path仍通過；
- malformed counter 不得產生 `valid-component-ceiling`。

不需要修改 production benchmark 的 counter 產生邏輯；現有 benchmark 自我驗證可保留，runner 的精確 gate用來確保 artifact 與 frozen plan 一致。

## 5. 最小冗餘清理

`COMPONENT_FSYNC_RATES_BY_BATCH` 目前只有宣告與賦值，後續 plan、formal round、profile round或報告資料都沒有讀取它。fsync calibration rate已直接寫入 frozen plan，因此：

- 刪除 associative array宣告；
- 刪除 calibration 中的賦值；
- 不以另一個容器或 abstraction 取代。

這只移除兩行無效狀態，不改變任何行為。

## 6. 不需修改的內容

下列 unstaged changes 符合設計，應保持不變：

- `component_preflight()` 名稱修正與 component happy/failure control-flow test；
- fixed command calibration budget與按 batch 換算的 bounded warmup；
- fastest-rate plan方向、timeout gate及disk reserve政策；
- profile-off formal rounds、profile-on attribution rounds及3% bias gate；
- StateMachine exact trades／events／active state驗證；
- WAL wall／service rate欄位語意；
- component scope不執行Engine、Publisher、Completion、`perf`或`fio`；
- safe per-case data cleanup與Git／binary identity gate。

## 7. 驗證順序

修正後依序執行：

1. `bash -n benchmarks/run_engine_writer_diagnostics.sh`；
2. `bash -n benchmarks/test_engine_writer_diagnostics_runner.sh`；
3. 執行 component runner contract test；
4. build `order_books_benchmark` 與 `order_books_tests`；
5. 執行四個 StateMachine scenario tests、odd-batch negative test及WAL smoke tests；
6. 執行完整 CTest；
7. 執行 `git diff --check`；
8. 比對修正前後 cached diff hash，確認 staging完全未改變。

本階段仍不執行正式 30 至 60 秒 component campaign；實際壓測應在程式與測試 review完成後另行執行。

## 8. 完成條件

只有同時符合下列條件，才可視為本次 unstaged review修正完成：

- 真實 benchmark 的一般十進位與科學記號 rate都能被正確解析；
- `nan`、`inf`、負數、空值與重複欄位仍會失敗；
- StateMachine／append fixed commands不小於數學 ceiling，且符合各自 alignment；
- WAL groups、commands、profile counters及sync samples都與 frozen plan精確一致；
- malformed fake output不會被分類為有效結果；
- 沒有未使用的 fsync rate array；
- production code、Git staging與host policy均未修改。

## 9. 預估修改量

| 區域 | 預估修改行數 |
| --- | ---: |
| 科學記號數值驗證與測試 | 8–15 |
| fixed-command ceiling與邊界測試 | 8–15 |
| WAL／profile精確counter gate與測試 | 20–35 |
| 移除未使用變數 | 2 |
| **合計** | **38–67** |

估算包含程式與測試的新增／修改行，不包含本文件。超出此範圍的 production optimization、worker調整、新增量測matrix或正式報告產生器都不是本次必要修正。
