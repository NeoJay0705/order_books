# Publisher Lag Hot-Path Optimization 壓測報告

## 1. 報告範圍

- 測試日期：2026-09-17
- 測試平台：Linux x86-64，`g++ 13.3.0`
- C++ 標準：C++20
- 測試 source：working tree 中的 prefix-index 與 batch-local pressure sample 實作
- 測試目標：確認 `Wal::bytes_after()` 線性掃描修正後的 correctness、sanitizer 安全性與單一交易對 durable workload 行為
- 本報告未執行或改變 Git staging；測試產物放在 `/tmp`，benchmark 自動清理其暫存資料目錄

CMake/Ninja 不在本次執行環境的 PATH，因此測試 binary 使用目前 source 以 g++ 直接重新編譯，並連結 Conan 提供的 GoogleTest；沒有使用 repository 內可能過期的既有 executable。

主要執行命令如下；所有 executable 均先由目前 source 重新編譯至 `/tmp`：

```text
/tmp/order_books_tests_current --gtest_color=no
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/order_books_tests_asan_ubsan --gtest_color=no
/tmp/order_books_benchmark_current --workload=engine_durable_single_instrument \
  --iterations=N --warmup=W
```

## 2. 負載情境

測試使用 `engine_durable_single_instrument` workload：

| 項目 | 設定 |
| --- | --- |
| Shard 數量 | 1 |
| Instrument 數量 | 1，`instrument_id=1` |
| Order book | 單一交易對，交叉的 sell/buy new-order pairs |
| 每次 iteration | 2 commands，預期產生 1 trade |
| Producer lanes | 1,024；每個 lane 同時最多 1 筆 in-flight command |
| Ingress queue | 65,536 |
| Group commit 上限 | 256 commands |
| Group commit delay | 200 microseconds |
| WAL durability | 每 group `fsync` |
| Snapshot | workload 期間以極大 command/time interval 停用觸發 |
| Event sink | 立即成功的 benchmark sink；publisher 仍正常 replay 與更新 cursor |
| Completion boundary | durable callback；不等待 downstream EventSink durable ACK |

每個 phase 的 command 數為 `iteration × 2`。Warmup 完成後才清除 latency samples；measured phase 的 elapsed time 從開始提交到所有 completion callback 完成計算。

## 3. 監測點與驗證方式

### 3.1 Benchmark 直接輸出的監測點

- `commands`：measured phase durable command counter delta。
- `trades`：measured phase trade counter delta。
- `commands_per_second`、`trades_per_second`。
- completion latency：submit 前記錄時間，completion callback 取樣；包含 queue、WAL append/sync、matching、invariant validation 與 completion dispatch，不包含 publisher cursor 完成。
- `p50_us`、`p99_us`、`p99.9_us`、`max_us`。
- `elapsed_ms`。
- `active_orders`、`active_levels`。
- `wal_bytes`：最後 metrics snapshot 的 WAL size。
- group size、group delay、fsync mode、completion boundary、instrument/shard/producer lane metadata。

### 3.2 Correctness 監測點

Benchmark 對每一筆 command 驗證：

- submit 成功且 completion 恰好一次。
- completion identity 與 producer lane 預期相同。
- command status 為 committed、error code 為 none。
- measured command/trade counter 與預期相同。
- measured latency sample 數量與 command 數相同。
- phase 結束時 `active_orders == 0`、`active_levels == 0`。

### 3.3 本次未由 benchmark stdout 輸出的監測點

`MetricsSnapshot` 目前有 `event_publish_lag_events`、`event_publish_lag_bytes`、`event_publish_lag_age_ns`，但 benchmark 最終輸出尚未列出其數值；本報告不對這些欄位做推估。後續若要把 publisher backlog 作為壓測報表正式欄位，應另做 observability 變更，不在本次效能修正中偷偷擴張。

## 4. 測試結果

### 4.1 功能與 sanitizer

| 測試 | 結果 |
| --- | --- |
| Debug-style GoogleTest | 35/35 passed，8 suites |
| Release-style GoogleTest | 35/35 passed，8 suites |
| ASan + UBSan GoogleTest | 35/35 passed |
| `git diff --check` | passed |
| Durable smoke | passed，40 commands／20 trades |

測試包含新增的 `WalBytesAfterRebasesPrefixIndexAfterRetention` 與
`PublisherPressureRejectsNewMutationButPreservesDuplicate`，分別驗證 retention
後 prefix index 重建、retention 後 append 的 lag bytes 計算，以及 publisher
pressure 下的新 mutation／duplicate admission precedence。

### 4.2 修正後 durable benchmark

所有數據均為未綁定 CPU 的 Release-style `-O2 -DNDEBUG` build；每列 workload 使用唯一暫存 data directory。

| iterations | warmup | measured commands | trades | commands/s | elapsed ms | p50 us | p99 us | p99.9 us | max us | WAL bytes | active orders/levels |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 20 | 5 | 40 | 20 | 8,404.17 | 4.75954 | 4,716.46 | 4,740.20 | 4,740.20 | 4,740.20 | 6,122 | 0 / 0 |
| 10,000 | 1,000 | 20,000 | 10,000 | 38,978.8 | 513.10 | 24,504.3 | 49,972.3 | 50,083.4 | 50,100.5 | 2,684,022 | 0 / 0 |
| 20,000 | 2,000 | 40,000 | 20,000 | 29,546.1 | 1,353.82 | 34,173.3 | 65,687.8 | 66,696.0 | 66,817.6 | 5,368,022 | 0 / 0 |
| 40,000 | 4,000 | 80,000 | 40,000 | 22,347.4 | 3,579.84 | 46,055.6 | 80,355.0 | 89,895.5 | 89,942.9 | 10,736,022 | 0 / 0 |
| 80,000 | 8,000 | 160,000 | 80,000 | 10,165.8 | 15,739.1 | 94,947.1 | 219,340.0 | 234,980.0 | 235,080.0 | 21,472,022 | 0 / 0 |

每次執行均通過 command/trade/completion/book-empty validation；沒有 silent rejection、publisher failure 或資料遺失訊號。

## 5. Profile 監測結果

以目前 source 建立 `-O2 -DNDEBUG -pg` binary，執行 20,000 iterations、2,000 warmup（40,000 measured commands）後使用 gprof 檢查 CPU hot path：

| Profile point | 結果 |
| --- | --- |
| `Wal::bytes_after()` | 474 calls，self CPU 0.00%（不再按 backlog 長度消耗 CPU） |
| `EventPublisher::lag_bytes()` | 471 calls |
| `EventPublisher::oldest_unconfirmed_received_at()` | 480 calls |
| Tombstone `OrderId` hash lookup | 31.25% self CPU |
| CRC32C | 8.33% self CPU |
| `MetricsRegistry::observe()` | 4.17% self CPU |
| `validate_state()` | 4.17% self CPU |
| `process_command_batch()` | 4.17% self CPU |

gprof 的 `frame_dummy` 取樣占比屬 profiling/instrumentation artifact，不作為 domain hot path 結論。

## 6. 歷史基線與可比性

以下是修正前曾取得的結果，當時使用 CPU 2 pinning、不同 source 狀態與 profiling/執行條件，因此只能作退化趨勢參考，不能當作嚴格 benchmark uplift：

| iterations | commands | commands/s | elapsed | p50 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 20,000 | 18,905 | 1.058 s | 50.1 ms | 82.5 ms |
| 20,000 | 40,000 | 11,857 | 3.373 s | 86.96 ms | 140.8 ms |
| 40,000 | 80,000 | 5,378 | 14.875 s | 153 ms | 486.5 ms |
| 100,000 | 200,000 | 971 | 206.0 s | 1,049 ms | 2,235 ms |

修正後最重要的證據不是單一 RPS 數字，而是 gprof 顯示 `bytes_after()` 不再是主導 CPU 的函式；長測仍下降代表已進入下一個成本結構，不能再把它歸因於原本的 O(n²) lag scan。

## 7. 結論與限制

本次修正通過功能、sanitizer、smoke 與中型 durable workload 驗證，並達成設計文件的主要完成條件：publisher lag bytes 查詢保留原有語意，但不再在線性掃描 backlog。

目前 80,000 iterations 的 throughput 仍低於 10,000 iterations，profile 顯示後續應觀察 tombstone hash lookup、CRC32C、metrics/invariant validation，以及尚未以新 syscall trace 量化的 WAL file I/O。這些屬於下一階段診斷，不應在本報告中直接宣稱新的單一根因或提前實作 publisher/cursor batching。
