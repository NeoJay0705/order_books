# StateMachine／WAL Component Ceiling staged review 必要修正

## 1. 目的與結論

本文件只描述 `docs/state-machine-wal-component-ceiling-design-review.md` 與目前 staged changes 之間，會影響
正確執行、量測有效性或設計接受標準的必要差距。修正範圍限於 benchmark、component runner、測試與本文件；
不得修改 production matching、WAL durability、WAL format、public API、Publisher 或 Completion worker。

目前 staged changes 的架構方向可保留，但在下列問題修正前，component campaign 不得產生
`valid-component-ceiling`：

1. component scope 呼叫不存在的 preflight function，正式流程無法啟動；
2. WAL calibration 以 group 數而非固定 commands 控制工作量，會在大 batch 產生不合理的巨量資料；
3. 固定工作量使用最慢 rate 推導，不能保證最快 case 至少執行 30 秒；
4. 缺少設計要求的 profile off/on bias gate，卻仍宣告結果有效；
5. runner contract tests 只驗證 dry-run，沒有覆蓋 component 正式控制流；
6. StateMachine scenario 與 WAL rate 欄位的驗證語意仍不夠精確。

其餘 staged 修改不需要重寫，也不需要新增 executable、production API 或另一套 runner。

## 2. 修正一：讓 component campaign 可以實際啟動

### 2.1 問題

`benchmarks/run_engine_writer_diagnostics.sh` 的 `run_component_campaign()` 呼叫
`run_component_preflight campaign-state 0`，但實際定義的 function 名稱是 `component_preflight()`。這不是單純
文件問題；非 dry-run 的 `--scope=component` 會以 command-not-found 結束。

### 2.2 必要修改

將呼叫改成：

```bash
component_preflight campaign-state 0 || {
  # 保留既有 preflight-busy 分類與安全停止邏輯
}
```

不要再新增 alias 或第二個包裝 function；直接統一名稱即可，避免同一個 preflight 出現兩個入口。

### 2.3 必要測試

runner contract test 必須至少有一個 `--scope=component` 的非 dry-run happy path。測試使用既有 fake commands 與
test mode，不執行真實 benchmark、不修改 host policy，也不碰 Git index。測試應證明：

- component preflight 確實被呼叫；
- StateMachine、append、fsync 三類 phase 都有被執行；
- component scope 沒有執行 `perf`、`fio`、Engine frontier、Publisher 或 Completion phase；
- 最終分類為 `valid-component-ceiling`；
- campaign 前後 cached diff hash 相同。

另加一個 component preflight failure case，確認 benchmark 尚未啟動即以 `preflight-busy` 停止。

## 3. 修正二：把 calibration 與 warmup 改成有界工作量

### 3.1 問題

目前 append 與 fsync calibration 對每個 batch 都傳入：

```text
--iterations=250000
```

但 WAL workload 的 `iterations` 是 group 數，不是 command 數。`batch=8192` 因而產生：

```text
250000 * 8192 = 2,048,000,000 measured commands
10000  * 8192 =    81,920,000 warmup commands
```

這不再是短 calibration，可能超時、耗盡磁碟或污染後續 preflight。正式 WAL case 也沿用固定 10,000 groups 的
warmup，造成 batch 越大，warmup 資料量越大。

### 3.2 calibration commands

沿用既有 `CALIBRATION_ITERATIONS`，但 component scope 將它轉成固定 command budget，不新增 production 選項：

```text
component_calibration_commands = round_up(max(8192, CALIBRATION_ITERATIONS * 2), 8192)
```

StateMachine calibration 維持 `batch_size=2`：

```text
state_iterations = component_calibration_commands / 2
```

WAL append calibration 的每個 batch 使用：

```text
append_groups(batch) = component_calibration_commands / batch
```

由於 command budget 已向上對齊 8192，matrix 中 `1, 256, 1024, 4096, 8192` 都能整除，不需要 partial group 或
另一條 append 路徑。

fsync calibration 不需要 1,000 samples；1,000 samples 是正式 case 的接受標準。新增 runner-local 常數：

```text
component_fsync_calibration_groups = 100
```

每個 fsync batch 先跑 100 groups 取得估算 rate，再由該 rate 推導正式 groups。這個常數只控制 benchmark runner，
不應加入 public library API。

### 3.3 有界 warmup

component WAL 的 warmup 應按固定 command budget換算，不再固定 group 數：

```text
component_wal_warmup_commands = round_up(max(8192, WARMUP * 2), 8192)
wal_warmup_groups(batch)      = component_wal_warmup_commands / batch
```

`component_run_binary()` 應接受 runner-internal 的 warmup groups 參數，最後只傳一個 `--warmup=N` 給 benchmark。
StateMachine 仍可使用 `WARMUP` iterations；WAL calibration、formal、profile case 都必須使用按 batch 換算後的
warmup groups。

不要修改 benchmark CLI 中 `iterations=groups` 的既有語意，也不要讓同一 command line 出現兩個
`--warmup` 選項。

### 3.4 正式工作量應以最快 rate 推導

StateMachine 四個 scenario 與 append 五個 batch 共用固定 command 數。若要求最快 case 至少執行目標 40 秒，
必須使用 calibration 中的最大 commands/s，而不是最小值：

```text
fixed_commands = ceil(max_rate * target_duration_seconds)
```

然後依序：

1. StateMachine commands 向上對齊 2；
2. append commands 向上對齊 8192；
3. 以每個 case 的 calibration rate 計算 estimated duration；
4. 任一最慢 case 的 estimated duration 若大於 `case_timeout - 30 seconds`，以
   `invalid-run/calibration-exceeds-timeout` 停止，不可默默縮短最快 case；
5. 將每個 calibration rate、固定 commands 與 estimated duration 寫入 frozen plan。

fsync 仍是逐 batch 獨立校準：

```text
formal_groups = max(1000, ceil(calibrated_groups_per_second * target_duration_seconds))
```

正式執行前同樣檢查估計時間不超過 timeout。

### 3.5 disk budget gate

每個 WAL calibration 完成後，從輸出的 `average_wal_bytes_per_command` 估算該 batch 的 formal WAL size：

```text
estimated_case_bytes =
    (formal_commands + warmup_commands) * average_wal_bytes_per_command
```

runner 應使用 `df -B1 --output=avail` 讀取 run root filesystem 的可用空間，並在 frozen plan 記錄：

- available bytes；
- estimated case bytes；
- 保留空間；
- 判定結果。

最小必要安全政策為：執行單一 case 後仍須保留至少 `max(20 GiB, available_bytes / 5)`。不符合時以
`environment-blocked/component-disk-budget-exceeded` 停止。因成功 case 的 WAL data directory 會立即安全刪除，
這裡只需驗證同時存在的最大單一 case，不必把整個 campaign 的 WAL size 全部相加。

正式執行前仍需再讀一次 available bytes；不得只相信 calibration 時的舊值。

## 4. 修正三：補齊 profile round 與 3% bias gate

### 4.1 問題

目前 component campaign 的正式 WAL rounds 全部使用 `--wal-phase-profile=off`，這對 ceiling 是正確的；但 runner
沒有執行任何 profile-on case，也沒有比較 off/on bias，最後仍直接輸出 `valid-component-ceiling`。這不符合設計
的 phase evidence 與接受標準。

### 4.2 必要修改

保留現有五輪 profile-off ceiling，不得把 profile-on rate 加入 CV 或 median。正式 ceiling 完成後：

1. 每個 append batch 執行至多一輪 `phase_profile=on`；
2. 每個 fsync batch 執行至多一輪 `phase_profile=on`；
3. profile case 使用同一 frozen plan、同一 warmup 換算、同一 preflight 與 correctness/replay gate；
4. profile stdout 必須包含對應的 `profiled_groups`、`profiled_commands`、write calls 與 subphase fields；
5. profile case 使用獨立 label，不能符合 `component_validate_cv()` 搜尋正式 rounds 的檔名模式。

選擇 `append batch=4096` 作代表 bias case，額外執行同工作量的 profile-off control，並與該 batch 的 profile-on
rate 比較：

```text
bias_percent = abs(profile_on_rate - profile_off_rate) / profile_off_rate * 100
```

結果寫入 `derived/component-profile-bias.tsv`。若 bias 大於 3%，分類為 `observer-biased`，不得輸出
`valid-component-ceiling`；profile 資料只能在報告中作定性參考。若不超過 3%，profile 仍只用於 subphase
歸因，不進入 ceiling median。

這裡不加入 `perf` 或 `fio`，也不擴充到函式 call graph 或 storage hardware ceiling。

### 4.3 必要測試

fake benchmark 應能依 `--wal-phase-profile=off|on` 輸出不同 rate，至少驗證：

- 2% bias 通過；
- 4% bias 產生 `observer-biased`；
- profile-on 檔案不會被 CV 計算讀入；
- 缺少必要 profile field 時產生 `invalid-run`；
- append 與 fsync 的 formal rounds 仍全部是 profile-off。

## 5. 修正四：收緊 StateMachine correctness

### 5.1 精確 event 計數

目前只驗證 `events != 0`，無法發現 scenario 產生多餘或缺少 event。依現有 StateMachine 語意，measured phase 的
必要結果為：

| Scenario | Expected trades | Expected events | Final active delta |
| --- | ---: | ---: | ---: |
| `new_crossing_pair` | `commands / 2` | `commands * 2` | 0 |
| `new_resting_cancel` | 0 | `commands` | 0 |
| `amend_quantity` | 0 | `commands` | +1 setup target |
| `replace_order` | 0 | `commands` | +1 setup target |

在 measured loop 結束後計算 `expected_events` 並作等值比較。保留現有的 committed/error、engine sequence、target
quantity/price、active count 與 `validate_state()` 檢查。

`consume_output()` 不應把 output 自己攜帶的 sequence 再傳回作 expected value；sequence 驗證已由
`apply_next()` 對照呼叫前的 `fixture.next_engine_seq` 完成。移除多餘 expected parameter，避免形成看似驗證、實際
恆真的比較。

### 5.2 測試

擴充既有四個 scenario CTest regex，分別確認 measured commands、trades、events、active orders 與
`correctness_verified=true`。另加一個奇數 `--pipeline-batch-size` 的 negative test，確認以
`batch_size_must_be_even` 失敗。

不需要為 scenario 建立第二套 StateMachine fixture，也不需要將 benchmark-only scenario 暴露成 production
API。

## 6. 修正五：統一 WAL rate 欄位語意

### 6.1 問題

目前輸出中的：

- `commands_per_second` 是 workload wall rate；
- `target_attainment_percent` 卻由 service rate 計算；
- `wal_mib_per_second` 改成 service rate，但歷史欄位原本代表 wall rate。

同一組 headline fields 因此無法互相推導，也可能讓既有報告誤讀。

### 6.2 必要修改

為維持相容性並讓新欄位清楚：

```text
commands_per_second                    = workload wall commands/s
target_attainment_percent              = workload wall commands/s / target
wal_mib_per_second                     = workload wall MiB/s
workload_wall_commands_per_second      = workload wall commands/s
workload_wall_target_attainment_percent= workload wall commands/s / target
workload_wall_wal_mib_per_second       = workload wall MiB/s
service_commands_per_second            = sum(group service time) commands/s
service_target_attainment_percent      = service commands/s / target
service_wal_mib_per_second             = sum(group service time) MiB/s
```

component runner 明確抽取 `service_commands_per_second` 作 component ceiling；不得再以 regex 的 `tail -n 1`
依賴欄位輸出順序。應使用帶欄位邊界的 extractor，找不到或出現多個同名欄位時視為 `invalid-run`。

同步更新 WAL smoke test，驗證 wall target 與 service target 欄位均存在，且 completion boundary 仍為
`append_batch_return` 或 `group_fsync`。

## 7. Runner contract test 的最小實作方式

為避免正式 component test 因 60 秒 cooldown 或 30 秒 preflight 變慢，test mode 可覆寫 runner-local 值：

```text
COMPONENT_COOLDOWN_SECONDS=0
COMPONENT_TARGET_DURATION_MS=ENGINE_WRITER_DIAGNOSTICS_TEST_TARGET_DURATION_MS
COMPONENT_MIN_DURATION_MS=ENGINE_WRITER_DIAGNOSTICS_TEST_MIN_DURATION_MS
component_fsync_calibration_groups=2
```

只有 `ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1` 時可以使用這些值；production campaign 的預設仍是 cooldown 60 秒、
target 40 秒、minimum 30 秒與 fsync calibration 100 groups。

fake benchmark 至少解析：

- workload、scenario、batch、sync mode、profile mode、iterations、warmup；
- StateMachine 的 measured commands/trades/events/active orders；
- WAL commands、service rate、elapsed、sync samples、replay flag與profile fields。

contract test 不需要模擬真實撮合或 WAL，只驗證 runner orchestration、counter gate、分類與不越界執行。

## 8. 修正後驗證順序

實作完成後依序執行：

1. `bash -n benchmarks/run_engine_writer_diagnostics.sh`；
2. `bash -n benchmarks/test_engine_writer_diagnostics_runner.sh`；
3. build `order_books_benchmark` 與 `order_books_tests`；
4. 執行四個 StateMachine scenario tests 與奇數 batch negative test；
5. 執行 WAL profile off/on、sync none/per-group smoke tests；
6. 執行 runner contract test，其中必須包含非 dry-run component happy path與失敗分類；
7. 執行完整 CTest；
8. 執行 `git diff --check`；
9. 確認 cached diff hash 與修正前相同，證明沒有改變既有 staging。

本階段只做小工作量測試，不執行正式 30 至 60 秒 component campaign。正式壓測仍須等使用者 review 修正內容後
另行執行。

## 9. 完成條件

只有同時符合下列條件，才可視為 staged review 的必要修正已完成：

- 非 dry-run component scope 能完整執行，不存在未定義 function；
- calibration 與 warmup 對所有 batch 都以有界 commands/groups 執行；
- frozen plan 能保證最快 case 至少 30 秒，並拒絕 timeout 或 disk budget 不安全的 plan；
- formal ceiling 只使用 profile-off rounds；
- profile-on rounds 與代表 bias gate 存在，且 bias 大於 3% 不會宣告有效 ceiling；
- StateMachine trades、events、active state及sequence均按 scenario 精確驗證；
- WAL wall/service rate 與 target attainment 欄位語意一致；
- component runner 的正式控制流由 contract test 覆蓋；
- production code、Git staging 與 host policy均未被修改。

## 10. 預估修改量

只做上述必要修正，預估：

| 區域 | 預估修改行數 |
| --- | ---: |
| component 啟動、calibration、warmup、timeout與disk budget | 55–85 |
| profile rounds與bias gate | 55–90 |
| StateMachine correctness與WAL欄位 | 15–30 |
| runner／benchmark tests | 45–70 |
| **合計** | **170–275** |

估算包含新增與修改的程式／測試行，不包含本文件。超出上述範圍的 production optimization、worker 調整、
新增 benchmark framework、`perf`／`fio` 整合或自動生成正式報告，均不是本次必要修正。
