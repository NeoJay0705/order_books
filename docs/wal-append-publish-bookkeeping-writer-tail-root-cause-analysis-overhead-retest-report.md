# WAL append publish bookkeeping Writer tail：量測有效性重測報告

## 1. 結論

- `result: preflight-busy`。
- 本次依操作檔在第一個 idle preflight 停止，沒有執行任何 B/C benchmark；完成度為 `0/20`。
- 因此沒有 throughput、latency、CPU s/M 或 Writer sync-tail 統計，不能把本次結果解讀為 baseline／candidate
  效能比較。
- 30 秒 preflight 已觀測到 allocated CPU 的高 iowait、WAL device 高 util 與 queue，足以解釋為何不應在
  此環境繼續採樣；這是環境准入失敗，不是 production 根因判定。
- candidate 狀態仍為 `provisional, not accepted`；本報告不支持 WAL、scheduler、publisher、completion
  或 diagnostics production 修改。
- 清除外部 CPU／I/O 負載後，必須建立新的 run root，從第一個 preflight 重新執行；不能沿用本次部分結果。

## 2. Scope、artifact 與執行資訊

- design：`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-design.md`
- procedure：`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-overhead-retest-procedure.md`
- 前次結果：`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-report.md`
- 執行時間：`2026-09-21T00:04:46+08:00`（報告建立前最後 identity capture；preflight 約 30 秒）
- `RUN_ROOT`：`/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI`
- measured B/C runs：`0/20`
- data directory、WAL、benchmark stdout／stderr、GNU time、telemetry：沒有建立 measured run

使用的是前次已通過 correctness gate 的凍結 Release binary：

| artifact | binary | SHA-256 | 驗證 |
| --- | --- | --- | --- |
| baseline | `/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark` | `888471e00e4bdc1b75109609111b3000f77b259e4ef74c25bc3cc58db2ee74ed` | pass |
| candidate | `/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark` | `a84d4a76082bca1a7b05bd5248d59410cf2fd578991a449ef1fe315ab2e24e35` | pass |

固定條件已準備但尚未進入 measured phase：one instrument／one shard、W=2、group size 4096、group delay
1000 us、producer lanes 8192、per-group fsync、affinity CPU 2-7、observer CPU 0-1、warmup 10000、
iterations 2,256,950、預期 measured commands 4,513,900。

## 3. Correctness 與完整性

本次沒有重新編譯或執行 correctness suite，因為操作檔要求重用前次已凍結 binary，且兩個 binary 的 SHA-256
均與前次報告一致。前次 correctness evidence 為：

- Debug CTest：104/104 passed；
- ASan／UBSan CTest：104/104 passed；
- baseline Release CTest：143/143 passed；
- candidate Release CTest：143/143 passed；
- smoke：6/6 passed。

本次：

| gate | 結果 |
| --- | --- |
| frozen binary hash | pass |
| preflight | **fail** |
| B/C measured runs | not run |
| formal A/B/C matrix | not permitted |
| invalid benchmark run | none；沒有 benchmark run |

## 4. Idle preflight 結果

WAL mount source 為 `/dev/sdb2`，對應 block device 為 `sdb`。原始資料保留於：

```text
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-mpstat.json
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-iostat.json
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-summary.txt
```

| metric | observed | required | gate |
| --- | ---: | ---: | --- |
| allocated CPU（2-7）最低 30 秒平均 idle | 66.5397% | >= 90% | **fail** |
| allocated CPU 所有 sample 最大 iowait | 41.84% | <= 5% | **fail** |
| WAL device `sdb` 30 秒平均 util | 44.3813% | <= 5% | **fail** |
| WAL device `sdb` 最大 aqu-sz | 1.66 | <= 0.25 | **fail** |

`sdb` 30 個 sample 的補充範圍：util 最低 0.40%、最高 63.60%；write await 最大 1.07 ms。這些數字只證明
測試開始前 host／storage 已忙碌，不證明是哪個外部 process 或 storage layer 造成負載。

測量時 `/proc/loadavg` 約為 `2.04 2.36 2.31`；preflight 結束後讀值為 `2.47 2.41 2.33`。loadavg 只作
背景資訊，不作唯一停止依據。操作期間未調整 governor、EPP、boost、I/O scheduler、mount options 或
system time。

## 5. Artifact 與 repository identity

preflight 完成、建立本報告前已保存：

```text
RUN_ROOT/logs/repository-identity-before-report.txt
RUN_ROOT/logs/binary-sha256-before-report.txt
RUN_ROOT/logs/environment-after-preflight.txt
```

HEAD 為 `09b4079c0ef4110809cf5a2543af916891982ec9`。建立報告前的 cached diff SHA-256 為
`4875e86409389cd2e71a1edd8fdbed6396a8baba309915253bade897e1077e44`，worktree diff SHA-256 為
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。報告建立後將再次驗證兩者以及
binary SHA-256；untracked manifest 預期只因新增本報告而改變。

CPU 2-7 policy：

```text
scaling_governor=powersave
energy_performance_preference=balance_performance
boost=1
```

filesystem 為 `/dev/sdb2`、ext4、`rw,relatime`。本次沒有修改 Git index 或 staging。

## 6. B/C overhead 結果

沒有 B/C measured run，以下表格不可填入 preflight 數字：

| artifact | B median RPS | C median RPS | throughput bias | B CPU s/M | C CPU s/M | CPU bias | gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | not run | not run | not available | not run | not run | not available | stopped before run |
| candidate | not run | not run | not available | not run | not run | not available | stopped before run |

本次沒有 C telemetry，因此沒有 sync tail、per-thread resource、profile phase 或 completion／publisher
lag 結果；不能以 preflight 的 device util 代替 Writer sync latency。

## 7. 根因判定與限制

依操作檔的優先序，本次唯一結果是 `preflight-busy`，不是下列任何 production 分類：

- `diagnostics-biased`：未執行 B/C，不能比較 diagnostics overhead；
- `storage/fsync tail`：未執行 C，不能把 preflight device activity 稱為 Writer fsync tail；
- `writer scheduler interaction`：沒有 per-thread measured window；
- `async worker contention`：沒有 publisher／completion measured window；
- `prepare scheduling interaction`：沒有 prepare helper measured window；
- `inconclusive attribution`：本次尚未進入歸因矩陣，應保留為 preflight stop。

本次可以確定的只有：開始測量時 CPU 與 WAL block device 不符合准入條件。不能由此推論 candidate 有
regression、diagnostics 有固定 overhead，或某個 worker 是瓶頸。

## 8. 下一步

1. 找出並停止／隔離使用 CPU 2-7 或 `/dev/sdb2` 的外部工作；不改 benchmark 或 production 設定。
2. 保留本 `RUN_ROOT` 作為失敗證據；建立全新的 run root，不與本次任何輪次混合。
3. 重新執行第 5 節的 30 秒 preflight；四個門檻都通過後才開始 baseline／candidate 五組 B/C pair。
4. 若再次 preflight 失敗，繼續停止並記錄環境，不執行 benchmark。
5. 若 preflight 通過但五組 pair 出現不穩定，依操作檔判為 `environment-unstable`；只有得到
   `valid-overhead` 才可執行原操作檔的正式 A/B/C 矩陣。

## Appendix A：原始 artifact manifest

| artifact | path | SHA-256 |
| --- | --- | --- |
| baseline | `/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark` | `888471e00e4bdc1b75109609111b3000f77b259e4ef74c25bc3cc58db2ee74ed` |
| candidate | `/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark` | `a84d4a76082bca1a7b05bd5248d59410cf2fd578991a449ef1fe315ab2e24e35` |

## Appendix B：本次執行資料

```text
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-mpstat.json
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-iostat.json
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/preflight/initial-summary.txt
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/logs/repository-identity-before-report.txt
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/logs/binary-sha256-before-report.txt
/home/neojhou/wal-writer-tail-overhead-retest-g5XPYCpI/logs/environment-after-preflight.txt
```

沒有 benchmark stdout、stderr、GNU time、WAL data、C CSV 或 formal raw row，因為流程在 preflight gate
停止。
