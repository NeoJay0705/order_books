# WAL append publish bookkeeping Writer tail 根因分析：壓測報告

## 1. 結論

- `result: instrumentation-biased`。
- candidate 狀態：`provisional, not accepted`。
- Debug、ASan／UBSan、baseline Release、candidate Release 的 correctness gate 均通過；六輪 smoke 與十二輪 diagnostics overhead pilot 也都 exit 0。
- baseline 的 B/C throughput median 為 168,911／167,641 commands/s，bias 0.75%；candidate 的 B/C median 為 178,305／123,019 commands/s，bias 31.01%，超過操作檔案規定的 5% gate。
- candidate 的 B/C CPU s/M bias 0.92%，但 throughput bias 已足以停止正式矩陣；C case 不能當作沒有量測開銷的 production path。
- 執行期間 CPU policy、binary hash、repository identity 均未變，但 `/proc/loadavg` 由開始時 `2.31 2.46 2.28` 升至結束時 `4.66 4.45 3.57`；這是需要先排除的環境噪聲，不能歸因給 production code。

下一步是固定或隔離外部 CPU／I/O 負載後重跑同一份 overhead procedure；在 gate 通過前不執行正式 A/B/C 矩陣，也不修改 production scheduler、WAL、publisher 或 completion worker。這批結果不能支持 `storage/fsync tail`、`writer scheduler interaction`、`async worker contention` 或其他 production 根因分類。

## 2. Scope、artifact 與環境

- design：`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-design.md`
- procedure：`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-procedure.md`
- 執行時間：2026-09-20 23:01:01--23:15:24 +0800
- `RUN_ROOT`：`/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n`
- HEAD：`09b4079c0ef4110809cf5a2543af916891982ec9`
- 測試固定參數：Linux、affinity `2-7`、one instrument／one shard、group size `4096`、group delay `1000 us`、producer lanes `8192`、WAL prepare workers `2`、sample every `16`、warmup `10000`、formal iteration candidate `2,256,950`（每輪 `4,513,900` commands）。
- baseline：candidate source snapshot 加上 `src/persistence/wal.cpp` 的 per-chunk bookkeeping commit `ea72a31e...` 反向 patch；candidate：未反向 patch 的同一 snapshot。source tree diff 僅有該檔案。
- baseline binary：`source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark`，SHA-256 `888471e00e4bdc1b75109609111b3000f77b259e4ef74c25bc3cc58db2ee74ed`
- candidate binary：`source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark`，SHA-256 `a84d4a76082bca1a7b05bd5248d59410cf2fd578991a449ef1fe315ab2e24e35`
- compiler：GCC 13.3.0；CMake 4.4.3；Conan 2.32.0
- host：Linux 6.17.0-35-generic，AMD Ryzen 7 3700X，8 cores／16 threads，SMT 2 threads/core；affinity 為 `2-7`。
- CPU policy：CPU 2--7 均 `scaling_governor=powersave`、`energy_performance_preference=balance_performance`、`boost=1`。
- filesystem：`/dev/sdb2`、ext4、`rw,relatime`；開始可用 149G，結束可用 142G。
- kernel counters：本次只收集 procfs thread resource counters；未取得額外 block-layer／device firmware counters，記為 `not_available`，不能據此判斷裝置因果。

身份驗證在建立本報告前完成：

| identity | before | after | 結果 |
| --- | --- | --- | --- |
| cached diff SHA-256 | `c0fafa1ca7736c636601385ec4264edfe3040247bb1b79dece0873b5f629d75a` | 相同 | unchanged |
| worktree diff SHA-256 | `ea6c2e6a9aba58d44db8326ce5310765b8063766267906e0cdfa76eac1438f0d` | 相同 | unchanged |
| untracked manifest SHA-256 | `5f302022393982d7212804e8d7a06125c0800cc31d01b04b08b2c4d5564e7bf6` | 相同 | unchanged |
| binary SHA-256 | 上列兩個 hash | 相同 | unchanged |
| CPU policy | 上列 policy | 相同 | unchanged |

報告檔案是在上述 identity check 後才建立；因此建立報告本身不被誤計入 benchmark identity。

## 3. Correctness 與有效性

- Debug CTest：`104/104` passed。
- ASan／UBSan CTest：`104/104` passed。
- baseline Release CTest：`143/143` passed。
- candidate Release CTest：`143/143` passed。
- smoke：`6/6` passed（baseline／candidate × A/B/C），均含 `correctness_verified=true`；A/B 沒有 diagnostics 欄位，C 含五個 role 與 measured telemetry rows。
- iteration calibration pilot：`4/4` passed，elapsed 4,430.76--4,819.41 ms，僅用於決定共同 iterations，不納入正式統計。
- diagnostics overhead pilot：`12/12` passed，所有 measured duration 24,990.1--61,122.1 ms，`profiled_groups=69`，沒有因低效能、慢 fsync 或高 tail latency 排除輪次。
- formal A/B/C：`not run: instrumentation-biased stop`。
- invalid／excluded runs：正式選定 run root 的 12 個 overhead runs 無 invalid；早期 setup／preflight 失敗的暫存 run roots 未納入本報告，亦未混入任何統計。

執行前修正了一個必要的 benchmark preflight 問題：`symlink_status()` 對正常不存在的 telemetry target 回報 `ENOENT`，程式將此正常狀態清除後才檢查 parent directory；dangling symlink 仍被拒絕。這是 output preflight 修正，不是 production WAL 行為；修正後才取得上述 Release 143/143 與 smoke 6/6。

## 4. Diagnostics overhead pilot

CPU s/M 是 `/usr/bin/time` process-wide user+system time 正規化，不是 writer-thread counter。

| artifact | B median RPS | C median RPS | throughput bias | B CPU s/M | C CPU s/M | CPU bias | gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | 168,911 | 167,641 | 0.75% | 9.690 | 9.726 | 0.37% | pass |
| candidate | 178,305 | 123,019 | 31.01% | 9.183 | 9.099 | 0.92% | **fail** |

candidate C 三輪的 RPS 為 `180,628`、`123,019`、`83,791.4`；candidate B 為 `115,379`、`178,893`、`178,305`。這是 gate 所禁止的非穩定配對，不能挑掉慢輪或拿最快輪替代 median。

### 4.1 B/C Writer profile phases（僅 overhead pilot）

以下為三輪 median 的 `ns/command`；不是 formal A/B/C 結果，不能作 production 歸因。

| artifact/case | writer service | group collect | admission | WAL append | fsync (`wal_sync`) | apply | post apply | completion enqueue | prepare | plan/copy | publish | write |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline B | 5,079.670 | 719.607 | 428.380 | 1,281.810 | 760.935 | 1,412.180 | 561.746 | 378.793 | 682.929 | 106.332 | 426.031 | 40.831 |
| baseline C | 4,931.180 | 638.548 | 443.856 | 1,246.430 | 714.391 | 1,439.060 | 572.980 | 391.649 | 696.391 | 52.905 | 425.559 | 41.165 |
| candidate B | 5,995.800 | 732.057 | 429.897 | 1,018.750 | 705.044 | 1,411.430 | 557.849 | 383.436 | 666.583 | 59.425 | 82.640 | 40.864 |
| candidate C | 7,063.950 | 614.861 | 452.152 | 901.048 | 3,261.790 | 1,386.870 | 558.678 | 376.290 | 645.320 | 54.948 | 81.223 | 41.106 |

candidate C 的 `wal_sync` 與 writer service 在 overhead pilot 中和 RPS 波動同時變化；由於 diagnostics gate 已失敗，這只能描述觀測到的 instrumented path，不能證明 fsync 或任何 production path 因果。

## 5. A／B／C aggregate

正式矩陣在 overhead gate 失敗後依規則停止；以下三張表明確保留各 case，不能以 overhead pilot 代替正式五輪。

### 5.1 Case A（profile off）

| metric | baseline median [min--max] | candidate median [min--max] | delta | paired direction | interpretation |
| --- | ---: | ---: | ---: | --- | --- |
| commands/s | not run | not run | not available | not available | instrumentation-biased stop |
| p50 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99.9 us | not run | not run | not available | not available | instrumentation-biased stop |
| max us | not run | not run | not available | not available | instrumentation-biased stop |
| actual commands/group | not run | not run | not available | not available | instrumentation-biased stop |
| CPU s/M commands | not run | not run | not available | not available | instrumentation-biased stop |
| voluntary/M | not run | not run | not available | not available | instrumentation-biased stop |
| involuntary/M | not run | not run | not available | not available | instrumentation-biased stop |

### 5.2 Case B（phase profile on）

| metric | baseline median [min--max] | candidate median [min--max] | delta | paired direction | interpretation |
| --- | ---: | ---: | ---: | --- | --- |
| commands/s | not run | not run | not available | not available | instrumentation-biased stop |
| p50 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99.9 us | not run | not run | not available | not available | instrumentation-biased stop |
| max us | not run | not run | not available | not available | instrumentation-biased stop |
| actual commands/group | not run | not run | not available | not available | instrumentation-biased stop |
| CPU s/M commands | not run | not run | not available | not available | instrumentation-biased stop |
| voluntary/M | not run | not run | not available | not available | instrumentation-biased stop |
| involuntary/M | not run | not run | not available | not available | instrumentation-biased stop |

### 5.3 Case C（phase profile、thread diagnostics、tail telemetry on）

| metric | baseline median [min--max] | candidate median [min--max] | delta | paired direction | interpretation |
| --- | ---: | ---: | ---: | --- | --- |
| commands/s | not run | not run | not available | not available | instrumentation-biased stop |
| p50 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99 us | not run | not run | not available | not available | instrumentation-biased stop |
| p99.9 us | not run | not run | not available | not available | instrumentation-biased stop |
| max us | not run | not run | not available | not available | instrumentation-biased stop |
| actual commands/group | not run | not run | not available | not available | instrumentation-biased stop |
| CPU s/M commands | not run | not run | not available | not available | instrumentation-biased stop |
| voluntary/M | not run | not run | not available | not available | instrumentation-biased stop |
| involuntary/M | not run | not run | not available | not available | instrumentation-biased stop |

A／B／C 的 formal rows 均未建立；低效能與慢 fsync 不是 invalid reason，但本次是 instrumentation gate failure。

## 6. Case C per-thread resource（3 輪 overhead pilot）

數字是三輪 median `[min--max]`；`unclassified_blocked_or_sleep` 依相同 measured window 推導，包含 condition wait、sleep、I/O wait 與其他 blocking，不能稱為 I/O wait。

| role | artifact | CPU s/M | runqueue ns/M | unclassified blocked/sleep ns/M | voluntary/M | involuntary/M | timeslices/M | migrations/M |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ob-bench` | baseline | 1.085 [1.048--1.138] | 176,700,893 [132,164,782--218,184,162] | 4.703e9 [4.623e9--12.275e9] | 24,378 [20,739--24,928] | 1,986 [1,681--2,194] | 26,572 [22,726--26,609] | 224 [220--247] |
| `ob-bench` | candidate | 1.098 [1.049--1.100] | 106,046,327 [100,152,074--116,341,503] | 6.974e9 [4.336e9--10.720e9] | 23,825 [20,677--24,114] | 1,208 [1,118--1,347] | 25,034 [21,795--25,461] | 219 [205--251] |
| `ob-wr-1` | baseline | 4.946 [4.906--5.002] | 42,090,160 [27,182,269--77,814,373] | 0.921e9 [0.920e9--8.557e9] | 46,042 [39,167--49,943] | 406 [342--593] | 46,449 [39,761--50,286] | 263 [233--269] |
| `ob-wr-1` | candidate | 4.563 [4.557--4.590] | 31,353,352 [23,040,925--37,727,363] | 3.543e9 [0.915e9--7.340e9] | 42,605 [38,793--47,681] | 359 [243--382] | 42,965 [39,036--48,063] | 226 [182--236] |
| `ob-wp-1-1` | baseline | 0.541 [0.540--0.541] | 25,680,595 [12,675,111--37,902,753] | 5.400e9 [5.339e9--12.962e9] | 492 [489--501] | 69 [58--69] | 559 [551--570] | 90 [75--92] |
| `ob-wp-1-1` | candidate | 0.529 [0.509--0.535] | 10,274,237 [3,447,126--12,209,996] | 7.596e9 [4.991e9--11.413e9] | 488 [488--490] | 67 [65--79] | 557 [553--568] | 82 [82--89] |
| `ob-pub-1` | baseline | 0.982 [0.852--1.025] | 108,509,727 [79,076,425--144,429,730] | 4.874e9 [4.789e9--12.544e9] | 4,155 [3,647--4,340] | 106 [100--123] | 4,261 [3,769--4,440] | 439 [416--470] |
| `ob-pub-1` | candidate | 0.869 [0.857--0.985] | 61,808,570 [53,430,993--84,337,278] | 7.206e9 [4.489e9--10.993e9] | 3,739 [3,674--4,163] | 103 [91--115] | 3,854 [3,766--4,266] | 460 [412--496] |
| `ob-cmp-1` | baseline | 0.624 [0.590--0.648] | 165,879,235 [144,900,472--277,315,126] | 5.175e9 [5.099e9--12.673e9] | 12,512 [11,934--12,651] | 1,975 [1,230--2,316] | 14,250 [13,742--14,626] | 117 [105--142] |
| `ob-cmp-1` | candidate | 0.629 [0.603--0.639] | 101,351,214 [89,882,665--118,363,824] | 7.424e9 [4.807e9--11.187e9] | 11,492 [10,313--12,211] | 1,070 [812--1,353] | 12,304 [11,666--13,282] | 117 [101--118] |

這些 role 數據只證明 diagnostics pilot 的 instrumented window 存在明顯 run-to-run blocking／load 波動；因為 C throughput gate 失敗，不支持 scheduler 或 async worker production 分類。

## 7. Case C Writer sync tail（3 輪 overhead pilot）

| metric | baseline median [min--max] | candidate median [min--max] | delta | paired direction |
| --- | ---: | ---: | ---: | --- |
| sync p50 us | 2,726 [2,651--2,819] | 2,806 [2,732--2,832] | +80 | mixed／not attributable |
| sync p99 us | 11,818 [11,351--392,842] | 180,670 [12,701--339,722] | +168,852 | mixed／not attributable |
| sync p99.9 us | 50,905 [47,014--580,664] | 279,443 [45,316--434,368] | +228,538 | mixed／not attributable |
| sync max us | 58,346 [52,555--762,181] | 302,876 [49,961--447,474] | +244,530 | mixed／not attributable |
| sync total us | 3,352,962 [3,256,005--37,933,355] | 15,211,399 [3,397,986--32,358,575] | +11,858,437 | mixed／not attributable |
| sync >25 ms | 4 [4--121] | 83 [3--105] | +79 | mixed／not attributable |
| sync >100 ms | 0 [0--118] | 79 [0--104] | +79 | mixed／not attributable |
| sync >250 ms | 0 [0--85] | 3 [0--71] | +3 | mixed／not attributable |

每個 C run 的 telemetry 對帳均為 `writer_sync_count=1103=wal_group_commits`、
`writer_group_sample_count=1103`、`writer_group_sample_commands=4513900=wal_group_commands`；CSV 均含
`sync,measured,` 與 `group_commands,measured,`，且五個 role 完整。sync 共變只能支持 instrumented
storage-tail 範圍，不能直接證明 filesystem、device firmware 或 host 層因果。

## 8. 根因決策與限制

依第 14 節 decision table：

- `profile instrumentation interaction`：不能選，因為它要求 overhead gate 通過；本次 candidate gate 失敗。
- `writer scheduler interaction`：不能選，未取得有效 formal A/B paired matrix。
- `storage/fsync tail`：不能選，雖然 C pilot 有 sync tail，但 gate 失敗且 process／thread blocking 同時受環境噪聲影響。
- `async worker contention`：不能選，未完成有效 formal matrix，且 publisher／completion 只是在 instrumented pilot 中觀測。
- `prepare scheduling interaction`：不能選，未完成有效 formal matrix。
- `inconclusive attribution`：正式根因分類暫不適用；依 procedure 的優先規則，本次應以 `instrumentation-biased` 停止，而不是用猜測補選一個 production 分類。

本次未量測或未驗證：direct WAL ceiling、producer lanes `8192` 的獨立 ceiling、Engine worker 的完整 formal ceiling、Publisher／Completion worker 的獨立 ceiling。`/usr/bin/time` process-wide 數據與 per-thread procfs 數據已分開，不能互換解讀。

## Appendix A：formal 30 輪 raw summary

`not run: instrumentation-biased stop`。沒有建立任何 formal A/B/C raw row，因此不能填入 30 輪數字，也不能用 overhead pilot rows 冒充 formal rows。

### Appendix A.1：overhead pilot raw summary（追溯資料）

| artifact | case | round | RPS | elapsed ms | p50 us | p99 us | p99.9 us | max us | actual/group | CPU s/M | valid |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | B | 1 | 170,857 | 26,419.1 | 43,818.2 | 105,529 | 397,786 | 401,858 | 4,092.38 | 9.593 | yes |
| baseline | B | 2 | 168,911 | 26,723.5 | 44,718.8 | 101,115 | 405,879 | 408,339 | 4,092.38 | 9.690 | yes |
| baseline | B | 3 | 167,173 | 27,001.5 | 45,196.7 | 92,676.1 | 395,513 | 398,297 | 4,092.38 | 9.881 | yes |
| baseline | C | 1 | 167,641 | 26,925.9 | 45,045.3 | 101,273 | 425,851 | 427,873 | 4,092.38 | 9.726 | yes |
| baseline | C | 2 | 73,850.5 | 61,122.1 | 45,244.5 | 814,148 | 1,239,830 | 1,384,300 | 4,092.38 | 9.389 | yes |
| baseline | C | 3 | 169,706 | 26,598.4 | 44,813.4 | 105,278 | 365,410 | 371,119 | 4,092.38 | 9.781 | yes |
| candidate | B | 1 | 115,379 | 39,122.5 | 41,422.4 | 355,915 | 487,881 | 499,041 | 4,092.38 | 9.112 | yes |
| candidate | B | 2 | 178,893 | 25,232.3 | 42,169.9 | 99,768.3 | 358,763 | 366,087 | 4,092.38 | 9.294 | yes |
| candidate | B | 3 | 178,305 | 25,315.7 | 41,408.7 | 108,901 | 596,598 | 599,377 | 4,092.38 | 9.183 | yes |
| candidate | C | 1 | 180,628 | 24,990.1 | 41,688.0 | 92,210.7 | 389,233 | 394,691 | 4,092.38 | 9.338 | yes |
| candidate | C | 2 | 123,019 | 36,692.8 | 41,656.6 | 436,836 | 543,890 | 569,400 | 4,092.38 | 9.094 | yes |
| candidate | C | 3 | 83,791.4 | 53,870.7 | 42,772.5 | 665,734 | 801,599 | 865,518 | 4,092.38 | 9.099 | yes |

## Appendix B：Case C raw resource 與 telemetry manifest

每個 CSV 有 2,207 行（header 加 1,103 sync rows 與 1,103 group-command rows）。完整 stdout、stderr、GNU time、WAL data 與 telemetry 均保留在 `RUN_ROOT`。

| artifact | round | CSV | SHA-256 | roles | sync/group samples |
| --- | ---: | --- | --- | --- | --- |
| baseline | 1 | `telemetry/baseline-overhead-caseC-r1.csv` | `d2cf0694404170051998f8174382005882f6b4ffb1a21bd1136392858e871bbd` | 5/5 | 1103/1103 |
| baseline | 2 | `telemetry/baseline-overhead-caseC-r2.csv` | `77856d36f6b1a8972268513b077ac30b891db37fe0e838027589e95eb75ffe1a` | 5/5 | 1103/1103 |
| baseline | 3 | `telemetry/baseline-overhead-caseC-r3.csv` | `923a1535bacfd106fad3ddd134bb92adb8dd9ae03f0232dd82c34e8a1242fa2c` | 5/5 | 1103/1103 |
| candidate | 1 | `telemetry/candidate-overhead-caseC-r1.csv` | `a8bba0d5cd9eb2b17679399634766240c7fbd0baa998c90a0852182b50ce49b9` | 5/5 | 1103/1103 |
| candidate | 2 | `telemetry/candidate-overhead-caseC-r2.csv` | `1e31d322dff8f53a24c1d88d02dff6fd46a0fdcd6806468bc2ab635ee64ef322` | 5/5 | 1103/1103 |
| candidate | 3 | `telemetry/candidate-overhead-caseC-r3.csv` | `0332a21d458f0d895a9ba21cb47b4a00a20e36553424e5676ec5af3cade43864` | 5/5 | 1103/1103 |

Raw paths are relative to `/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n`.
