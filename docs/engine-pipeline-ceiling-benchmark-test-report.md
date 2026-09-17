# Engine Pipeline Ceiling Benchmark 測試結果報告

## 1. 測試範圍

- 測試日期：2026-09-17
- 目的：驗證 pipeline ceiling benchmark 的編譯、正確性契約、metrics contention 計數、
  barrier wall-time 量測與既有測試回歸。
- 範圍：單一 shard、單一 instrument；包含 StateMachine、invariant validation、metrics、
  runtime handoff 與 publisher drain。
- 本輪是修正後的 correctness／measurement smoke，不是正式性能基線；尚未執行每個組合五次
  的長時間壓測。
- 測試過程沒有執行 `git add`、`git reset`、`git restore` 或 `git commit`。

## 2. 測試環境

| 項目 | 數值 |
| --- | --- |
| OS | Linux |
| Compiler | GCC 13.3.0 |
| Language | C++20 |
| Build | Release，warnings-as-errors 設定 |
| Benchmark build | `/tmp/order_books-pipeline-build-werror` |
| CPU metadata | `cpu_threads=16`；benchmark 回報 `cpu_model=unavailable` |
| Data directory | workload 使用 temporary directory |

## 3. 測試方法與命令

### 3.1 編譯

```bash
make -C /tmp/order_books-pipeline-build-werror \
  -j2 order_books_benchmark
```

結果：成功重新編譯 `pipeline_ceiling_benchmark.cpp` 並連結 benchmark executable。

### 3.2 StateMachine XOR-zero regression

```bash
/tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=state_machine \
  --iterations=1 --warmup=0 \
  --pipeline-batch-size=4 --pipeline-active-orders=3
```

驗證結果：

```text
commands=4 trades=2 events=8 active_orders=3 last_engine_seq=7
latency_scope=command_group correctness_verified=true
```

### 3.3 Metrics writer／Publisher contention

```bash
/tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=metrics \
  --iterations=1000 --warmup=100 --pipeline-batch-size=256
```

Measured commands 為 `1000 × 256 = 256,000`。結果如下：

| Case | Operations | Observations/s | Command-equivalent/s | p99 (us) | Elapsed (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `writer_only` | 2,816,000 | 18,346,273.55 | 1,667,840 | 222.547 | 153.492 |
| `writer_publisher_contended` | 4,096,000 | 6,075,277.12 | 379,705 | 1,468.56 | 674.208 |

Contended case 的計數驗證：

```text
writer_commands=256000
publisher_commands=256000
writer_metric_calls_per_command=11
publisher_metric_calls_per_command=5
writer_observations=2816000
publisher_observations=1280000
observations=4096000
```

因此 `operations` 為兩側 observation 總和，而 command-equivalent 只計算完成的
writer／Publisher command pair，沒有將兩側 command 重複相加。

### 3.4 完整 pipeline smoke

```bash
/tmp/order_books-pipeline-build-werror/benchmarks/order_books_benchmark \
  --workload=engine_pipeline_ceiling \
  --pipeline-stage=all \
  --iterations=20 --warmup=4 \
  --pipeline-batch-size=8 --pipeline-active-orders=100
```

| Stage / case | Operations/s | Command-equivalent/s | p99 (us) | Elapsed (ms) | Correctness |
| --- | ---: | ---: | ---: | ---: | --- |
| `state_machine/crossing_pair` | 1,208,550 | 1,208,550 | 12.694 | 0.13239 | passed |
| `invariant_validation/fixed_state` | 105,000 | 839,996 | 10.700 | 0.190477 | passed |
| `metrics/writer_only` | 18,989,458.69 | 1,726,310 | 5.981 | 0.092683 | passed |
| `metrics/writer_publisher_contended` | 5,713,775.56 | 357,111 | 114.705 | 0.448040 | passed |
| `runtime_handoff/admission_error_completion` | 395,454 | 395,454 | 331.552 | 0.404598 | passed |
| `publisher_drain/durable_cursor` | 394.148 | 394.148 | 405,939 | 405.939 | passed |

Publisher drain 另外驗證：

```text
wal_commands=160
sink_calls=160
sink_events=320
cursor_head=160
durable_cursor_verified=true
```

每一行 summary 都包含正確的 `latency_scope` 與 `correctness_verified=true`。

### 3.5 GoogleTest

```bash
/tmp/order_books-pipeline-tests-build-clang-debug/tests/order_books_tests
```

結果：42 tests from 10 test suites，全部通過。

### 3.6 Diff／格式檢查

```bash
git diff --check
git diff --cached --check
git status --short
```

結果：兩個 diff check 通過；測試命令沒有改變 Git staging。

## 4. 結論

本輪結果確認：

- barrier completion wall-time 修正可編譯並可正常執行。
- `validation_call`、`command_group`、`metric_group`、`concurrent_worker_group` 與
  `full_backlog_drain` 的輸出語意一致。
- StateMachine 不再因合法 XOR checksum 為零而誤判失敗。
- Metrics writer／Publisher mix、observation count 與 command-equivalent 換算正確。
- Publisher drain 完成 durable cursor 與 sink event 驗證。
- 既有 GoogleTest 沒有回歸。

本報告不能用來宣稱達成穩定的 1,000,000 commands/s。`pipeline-stage=all` 只是 correctness
smoke；正式性能結論仍需固定硬體、filesystem、CPU 設定後，對目標參數重複至少五次，報告
throughput median、p99、p99.9 與 max。

