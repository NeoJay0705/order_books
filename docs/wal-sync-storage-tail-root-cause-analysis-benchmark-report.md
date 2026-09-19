# WAL sync／storage tail 根因分析 benchmark report

## 1. 摘要與結論

本報告整理修正前的 WAL sync／storage tail calibration。它是 blocked report，不是新的正式效能矩陣；
所有數據均保留原始輪次與 artifact，不以重跑取代慢輪。

| 項目 | 結果 | 判定 |
| --- | --- | --- |
| ReleaseBenchmark | 115/115 | pass |
| Debug | 90/90 | pass |
| ASan/UBSan | 90/90 | pass |
| W1 telemetry bias | off median 162,771 RPS；on median 160,107 RPS；約 1.64% | pass，<=5% |
| W2 telemetry bias | off median 172,762 RPS；on median 103,232 RPS；約 40.25% | fail，>5% |
| 正式矩陣 | 0/10 | blocked，依 gate 停止 |
| Artifact consistency | 已執行輪次可讀；drain boundary 多數缺失 | fail，不能進正式矩陣 |
| Sync tail | supported for application-level elapsed increase | 需補同期外部證據 |
| Device/storage | inconclusive | 沒有每個慢輪的完整同期證據 |
| Filesystem/syscall | inconclusive | 本輪未執行 strace correlation |
| Queue/publisher pressure | inconclusive | 缺 deterministic drain start/end |
| Parallel-prepare interaction | inconclusive | W2 慢輪與 telemetry bias 混在一起 |

W2 的兩個慢輪中，新增的 measured sync wait 約可解釋額外 elapsed time 的 94.65% 與 98.92%。這支持
「application-level sync tail 是慢輪的直接解釋」，但不能單獨證明根因來自 device、filesystem 或
`fsync` syscall。下一步必須先完成 collector／state sampler 分離、deterministic drain boundary 與同期
外部監測，不能直接修改 durability 或 group policy。

## 2. 測試身份與環境

- 量測日期：2026-09-19（Asia/Taipei）。
- Host/kernel：Linux 6.17.0-35-generic，x86_64。
- CPU：AMD Ryzen 7 3700X，8 cores／16 threads，boost enabled。
- CPU affinity：`taskset -c 2-7`。
- Filesystem：root filesystem `/dev/sdb2`、ext4、mount options `rw,relatime`；約 47% used、
  468 GiB available。
- Governor：`powersave`。
- Compiler/build：GNU C++ 13.3.0、Release、`-m64 -O3 -DNDEBUG`。
- Workload：單一 instrument、單一 shard、`engine_durable_single_instrument`。
- Formal iterations：1,750,962；warmup：10,000。
- Group：4096 commands、1000 us delay；producer lanes：8192；parallel-prepare threshold：4096。
- W1：`wal_prepare_workers=1`；W2：`wal_prepare_workers=2`。
- 量測時 Source HEAD：`d68d8522b34e3a4aa2c9014c7979a7b9aee56e1c`。
- 量測時 index diff SHA-256：`bbdbabb193b8757b43ddce2d147c21a6aefe3d21cac55868a71fd2bd677328eb`。
- 量測時 worktree diff SHA-256：`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
- Untracked procedure SHA-256：
  `8be830f8cf8869409a4313bf6bb64f92b9cb13e0898412a4cac052ac2e9e9191`。
- Benchmark binary SHA-256：`29b0863d9aa12faa63c12d0159a262c81425b371984c2ec32e92673ef9cf2bba`。
- 原始結果根目錄：`/home/neojhou/wal-sync-tail-analysis-dFGqFZVA`。
- 代表性 artifact SHA-256：
  `telemetry/cal-w2-on-r2.csv` = `bcf8f2c82befe67ca3eb64e61bb59fb8f9300be03d8097fb360f57bd0d952c98`。
- `/usr/bin/time`、`taskset`、`iostat`、`pidstat`、`strace` 可用；本輪未以 strace 執行
  correlation case。

原始結果只有 `source-identity-before.txt`，沒有 `source-identity-after.txt`。因此可以重建量測前的 source、
dirty tree 與 binary identity，但不能宣稱執行前後 identity 已通過一致性比對；此項記為未驗證。

本報告記錄的是修正前執行身份。後續修改後的 correctness test 與 component calibration 必須另行記錄，
不可覆寫這組歷史結果。

## 3. Correctness、smoke 與 calibration

### 3.1 Correctness

ReleaseBenchmark、Debug、ASan/UBSan 的既有測試均通過；以上數字是本次 calibration 執行時的 baseline，
不是正式效能矩陣的成功輪數。

兩個 telemetry smoke 均以 status 0 結束；CSV rows 包含 header：

| Case | Iterations | Elapsed ms | RPS | Sync count | Group count | CSV rows | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| smoke W1-on | 10,000 | 118.137 | 169,295 | 6 | 6 | 25 | pass |
| smoke W2-on | 10,000 | 157.424 | 127,045 | 6 | 6 | 29 | pass |

### 3.2 Pilot 與正式 iterations

| Case | Pilot iterations | Elapsed ms | RPS | Candidate iterations | Status |
| --- | ---: | ---: | ---: | ---: | --- |
| W1-off | 400,000 | 5,375.28 | 148,829 | 1,488,295 | pass |
| W1-on | 400,000 | 5,070.30 | 157,782 | 1,577,816 | pass |
| W2-off | 400,000 | 4,650.29 | 172,032 | 1,720,323 | pass |
| W2-on | 400,000 | 4,568.92 | 175,096 | about 1,750,962 | pass |

`candidate_iterations = ceil(400,000 * 20,000 / pilot_elapsed_ms)`。最後採用原始執行紀錄中的共同值
`FORMAL_ITERATIONS=1,750,962`；表中的 elapsed 只保留到 0.01 ms，因此由顯示值反算可能相差一個
iteration。

### 3.3 Calibration

W1 與 W2 各有 off/on 三輪。RPS median 計算如下：

```text
W1 off = median(163542, 162771, 160308) = 162771
W1 on  = median(160321, 159125, 160107) = 160107
W1 bias = abs(160107 - 162771) / 162771 = 1.64%

W2 off = median(174829, 172762, 171239) = 172762
W2 on  = median(168502, 95081.5, 103232) = 103232
W2 bias = abs(103232 - 172762) / 172762 = 40.25%
```

| Case | RPS | Elapsed ms | Sync count | Sync total ms | CSV rows | Status |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| W1-off-r1 | 163,542 | 21,413.0 | N/A | N/A | N/A | pass |
| W1-off-r2 | 162,771 | 21,514.4 | N/A | N/A | N/A | pass |
| W1-off-r3 | 160,308 | 21,845.0 | N/A | N/A | N/A | pass |
| W1-on-r1 | 160,321 | 21,843.2 | 855 | 2,682.225 | 3,896 | pass |
| W1-on-r2 | 159,125 | 22,007.4 | 856 | 2,672.452 | 3,914 | pass |
| W1-on-r3 | 160,107 | 21,872.5 | 855 | 2,558.316 | 3,898 | pass |
| W2-off-r1 | 174,829 | 20,030.6 | N/A | N/A | N/A | pass |
| W2-off-r2 | 172,762 | 20,270.2 | N/A | N/A | N/A | pass |
| W2-off-r3 | 171,239 | 20,450.5 | N/A | N/A | N/A | pass |
| W2-on-r1 | 168,502 | 20,782.7 | 855 | 2,777.159 | 3,790 | pass |
| W2-on-r2 | 95,081.5 | 36,830.8 | 855 | 18,451.864 | 5,394 | pass |
| W2-on-r3 | 103,232 | 33,923.0 | 855 | 16,282.847 | 5,103 | pass |

off 模式沒有 telemetry collector 或 CSV，因此其 sync count、sync total 與 CSV rows 記為 `N/A`，不是
零。十二個 calibration process 的 status file 均為0；這只表示執行成功，不表示W2 calibration gate
通過。

W1 通過 5% calibration gate；W2 未通過，因此依預先定義的停止規則沒有執行正式 10 輪矩陣。這不是漏跑，
也不能把 W1/W2 calibration 當成正式五輪 aggregate。

## 4. 慢輪與 sync tail 證據

W2 `cal-w2-on-r1` 為 168,502 RPS、elapsed 20,782.7 ms，measured sync total 2,777,159 us。
`cal-w2-on-r2` 降至 95,081.5 RPS、elapsed 36,830.8 ms，measured sync total 18,451,864 us，並有
118 筆 sync >25 ms、103 筆 >100 ms、5 筆 >250 ms。

W2 `cal-w2-on-r3` 為 103,232 RPS、elapsed 33,923 ms，measured sync total 16,282,847 us，並有
102 筆 sync >25 ms、75 筆 >100 ms、2 筆 >250 ms。

以 W2 off median elapsed 20,270.2 ms 作 wall-time baseline，並以正常的 W2 on-r1 sync total
2,777.159 ms 作 sync baseline：

```text
W2 on-r2 = (18,451.864 - 2,777.159) / (36,830.8 - 20,270.2) = 94.65%
W2 on-r3 = (16,282.847 - 2,777.159) / (33,923.0 - 20,270.2) = 98.92%
```

兩個慢輪的 elapsed 增加與 sync total 同時出現。以上比例足以支持 application-level
`wal_sync_latency_us` tail 與吞吐下降相關，但它是以兩個不同baseline組成的近似歸因，不能回答等待在
device、filesystem 或 syscall 哪一層產生，也不是CPU利用率。

## 5. Backlog 與 artifact 限制

原始 telemetry summary 多數輪次的 `drain_publisher_lag_*_last` 為 `na`，因為 10 ms sampler 在
`Engine::stop()` 前沒有必然取得 drain row。這使得當時無法可靠比較 drain 開始與結束的 Publisher lag，
也無法用「lag 是否收斂」判定 queue/publisher pressure。

正常輪的 `iostat`／process I/O 觀察沒有顯示 bandwidth saturation；這只代表那些觀察輪次沒有看到明顯
bandwidth ceiling，不能反證 W2 慢輪不存在 storage latency tail。CSV 沒有 absolute epoch anchor，所以
1 秒 iostat 只能做同輪粗粒度 correlation，不能把單筆 sync sample 精確對齊到某一秒 device sample。

## 6. 未執行與不可宣稱事項

- 沒有執行獨立 `strace` 診斷輪，因此不能宣稱慢 sample 對應 `fsync` 或 `write` syscall。
- 沒有完成 collector-only 與 full sampler 的 component calibration，不能把 40.25% 歸因給 telemetry
  本身，也不能排除 storage tail 恰好集中在 on 輪。
- 沒有正式 W1/W2 五輪矩陣，因此沒有 production aggregate、正式 p50/p99/p99.9 結論或 rollout gate。
- 正式 aggregate、正式逐輪明細與10輪artifact consistency均為`N/A`，不是0效能或測試失敗；唯一的
  `0/10`代表依W2 calibration gate停止後沒有啟動正式case。
- 沒有`source-identity-after.txt`，不能驗證量測期間source或binary是否保持不變。
- Workload是單機closed-loop、單一instrument／shard；10 ms application state與1秒external sampling
  只能提供不同解析度的correlation，結果不可直接外推至production fixed-rate arrival或其他storage。
- 沒有修改 WAL format、durability boundary、group delay、prepare workers、Publisher 或 Completion。

## 7. 下一步

先完成 `docs/wal-sync-storage-tail-root-cause-analysis-post-benchmark-staged-review-fixes.md` 定義的
必要修正，並重新通過 correctness tests：

1. 保證 drain-start／drain-end 與 CSV first／last row 語意一致；
2. 修正 CMake drain count 數值檢查並比對 summary／CSV；
3. 使用同一 binary、參數、CPU affinity 與 filesystem，依固定順序執行 off、collector-only、full 三模式
   component calibration；
4. 每模式三輪、每輪 measured 至少 60 秒，保留所有 outlier；
5. calibration 全部通過後，才重新執行原 W1/W2 full telemetry calibration 與正式矩陣；慢輪另以
   `iostat`、process I/O 與獨立 strace case 補充證據。

在新證據成立前，本報告不支持修改 production durability、降低 fsync、提高 group delay 或變更 W=2
default。
