# WAL append publish bookkeeping guardrail 確認測試報告

## 1. 結論

- result: `controlled guardrail failed / do not retain`
- candidate status: `provisional, not accepted`
- CPU policy: `powersave`、EPP `balance_performance`、boost enabled；測試期間未變更。
- primary reason: g4096 writer 的 p99、p99.9 與 involuntary context-switch guardrail 在新的 10
  paired rounds 仍惡化；Engine g4096 的 throughput、p99.9 與 context-switch 結果沒有相同退化。
- writer p99.9: `437,891.5 -> 481,196.5 us`，+9.89%，6/10 paired worsening；fail。
- writer involuntary context switches: `3,223.7 -> 3,709.6 / M commands`，+15.07%，7/10
  paired worsening；fail。
- 15-pair 合併 p99.9: 9/15 worsening，paired-delta median +11.95%；fail。
- 15-pair 合併 involuntary context switches: 12/15 worsening，paired-delta median +15.84%；fail。
- writer `wal_publish_ns_per_command`: `445.1345 -> 81.48245 ns/command`，降低 81.70%。
- writer `wal_append_ns_per_command`: `1,657.20 -> 951.176 ns/command`，降低 42.60%。
- writer throughput: `167,322 -> 177,442.5 commands/s`，提升 6.05%，但不能抵銷 guardrail failure。
- authoritative Engine throughput: `168,970 -> 181,487 commands/s`，提升 7.41%；非 supply-limited。

這次確認了 publish／append 的局部優化仍有效，但沒有解除 writer synchronous path 的 tail 與排程
風險。因此 candidate 仍不可作為已接受的 production change。

## 2. 與原報告的關係

- 原報告：`docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md`。
- 本測試只重測 g4096 writer profile-on 與 authoritative Engine g4096：4 個 smoke runs 加 40 個
  formal runs；沒有重跑 direct WAL、g8192、Publisher 或 Completion。
- 原正式測試與本次都使用 `powersave` governor、W=2、group=4096、group delay=1000 us、producer
  lanes=8192、per-group fsync、CPU affinity 2-7 及相同 immutable binaries。
- 原五輪與本次十輪的絕對 latency／RPS 分開報告；只有原本失敗的 p99.9 與 involuntary context
  switches 以 15 個 paired deltas 做合併 gate，避免用新的 median 覆蓋原始失敗證據。
- 這次只建立較新的 raw evidence；原報告的 direct-WAL byte identity、CTest 與 70-run 矩陣結果仍
  保留，不因本次範圍外而重算。

## 3. Artifact 與環境身份

- 執行時間：2026-09-20 20:37:12 +08:00 至約 21:11:21 +08:00。
- `RUN_ROOT`：`/home/neojhou/wal-publish-bookkeeping-confirm-dJ4gmU2X`。
- repository HEAD（身份檢查時）：`ea72a31e25f87d2528b795704a6c2d5b07423e72`。
- formal 開始及結束時 staged identity 相同；report 建立前的 status 為：
  `M docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md`、
  `M docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md`、
  `M docs/wal-append-publish-bookkeeping-optimization-design.md`、
  `A docs/wal-append-publish-bookkeeping-optimization-post-benchmark-design-review.md`、
  `?? docs/wal-append-publish-bookkeeping-optimization-confirmatory-benchmark-procedure.md`。
- cached diff SHA-256 before/after：
  `9a4d5e356656f27f3356113362a844fa333492115f2c7ede5f5cc2a33ecb37d2`。
- worktree diff SHA-256 before/after：
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
- untracked manifest SHA-256 before/after report 建立前：
  `7277071982d742adf6b2be8e69764a95be4ec4261c4c9fe849439e7e2f789b45`。
- baseline binary：`$SOURCE_RUN_ROOT/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark`；
  SHA-256 `1d07826a99dd9d79950abd9bac8b47b2b917a2b830e449b545eeee93167fd2cb`。
- candidate binary：`$SOURCE_RUN_ROOT/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark`；
  SHA-256 `d10005dbc4335d60c438deba393c7a67d3ad5f079c253e8dbe99992dea059b29`。
- baseline／candidate 使用 GCC 13.3.0、Release、C++20；兩個 binary hash 在測試前後相同。
- CPU：AMD Ryzen 7 3700X，16 logical CPUs、SMT enabled、boost enabled；affinity `2-7`。
- CPU driver：`amd-pstate-epp`；governor `powersave`；EPP `balance_performance`。
- kernel：Linux 6.17.0-35-generic x86_64。
- filesystem：`/dev/sdb2`、ext4、`rw,relatime`；測試前可用空間約 172 GiB。
- `perf_event_paranoid=4`；普通使用者 `perf stat` preflight 失敗，排程 counters 本次填 `not measured`。

## 4. Correctness 與有效性

- smoke：baseline/candidate writer 各 1 輪、Engine 各 1 輪，4/4 status 0。
- formal：writer 20/20、Engine 20/20，合計 40/40 status 0。
- writer 20/20 `correctness_verified=true`；每輪 `profiled_groups=73`，均達至少 50。
- Engine 20/20 status 0，`telemetry_dropped_samples=0`；durable callback、completion 與 benchmark
  recovery validation 均未回報錯誤。
- writer measured duration：32.80–65.46 秒；Engine：31.15–65.95 秒，均達至少 20 秒。
- writer actual commands/group：4,090.44–4,093.98；Engine：4,088.32–4,095.61，均高於 90% fill
  gate。
- 沒有因低 throughput、慢 fsync、p99.9 outlier、高 CPU 或 context switches 排除任何 formal run。
- invalid／excluded formal runs：none。
- 本次沒有重跑 Debug／Release／ASan／UBSan CTest；既有報告記錄的 CTest 與 byte-identity 結果仍
  是先前證據，本次只使用未變更的 immutable binaries 做確認。
- WAL byte identity control：本次未重跑，填 `not measured in this confirmatory scope`；原報告的
  no-rotation 與 rotation manifest/hash diff 仍為空。

## 5. Writer g4096 profile-on

Latency 單位為 µs；phase 單位為 ns/command；CPU 與 context switch 以每百萬 commands 正規化。
中心值為 10 輪 median，range 為 min–max。

| metric | baseline median [min–max] | candidate median [min–max] | delta | paired worsening | gate |
| --- | ---: | ---: | ---: | ---: | --- |
| commands/s | 167,322 [113,315–168,396] | 177,442.5 [86,634–183,898] | +6.05% |  | observe |
| p50 | 45,320.3 [44,874.9–45,712.9] | 42,108.1 [40,858.8–43,469.6] | -7.09% |  | observe |
| p99 | 101,390.1 [91,031–518,061] | 106,128 [90,013.2–803,148] | +4.67% | 6/10 | **fail** |
| p99.9 | 437,891.5 [368,738–714,371] | 481,196.5 [369,529–1,179,930] | +9.89% | 6/10 | **fail** |
| max | 440,802.5 [377,034–811,111] | 484,124.5 [376,033–1,232,290] | +9.83% | 7/10 | observation |
| `wal_publish_ns_per_command` | 445.1345 [409.091–472.744] | 81.48245 [80.283–84.7268] | 81.70% reduction |  | pass |
| `wal_append_ns_per_command` | 1,657.20 [1,263.92–2,738.26] | 951.176 [851.697–1,489.74] | 42.60% reduction |  | pass |
| `wal_prepare_ns_per_command` | 674.4395 [659.521–726.184] | 666.209 [636.591–705.344] | -1.22% |  | observe |
| `wal_plan_copy_ns_per_command` | 477.0895 [57.6498–1,528.05] | 89.32015 [45.93–607.923] | -81.28% |  | attribution |
| `wal_write_ns_per_command` | 40.98545 [39.6514–135.736] | 42.77545 [39.8745–167.033] | +4.37% |  | observe |
| `wal_sync_ns_per_command` | 730.673 [668.375–3,430.22] | 820.8075 [670.212–6,675.05] | +12.34% |  | observe |
| StateMachine apply | 1,428.84 [1,372.18–1,452.76] | 1,409.485 [1,366.43–1,432.96] | -1.35% |  | observe |
| completion enqueue | 391.3985 [371.741–403.231] | 376.8235 [370.991–393.902] | -3.72% |  | observe |
| CPU s/M commands | 9.690 [9.426–9.789] | 9.252 [9.130–9.663] | -4.52% |  | pass |
| voluntary/M | 90,048 [86,030–93,733] | 89,271 [80,074–100,202] | -0.86% | 5/10 | pass |
| involuntary/M | 3,223.7 [1,920.7–6,008.8] | 3,709.6 [2,236.4–5,943.1] | +15.07% | 7/10 | **fail** |
| max RSS KiB | 2,547,006 [2,490,040–2,602,308] | 2,547,400 [2,546,564–2,566,724] | +0.015% | 7/10 | observation |

新的 10-round paired deltas 中，p99.9 median delta 為 +24.98%，involuntary context-switch median
delta 為 +11.15%。這不是單一慢輪造成的 gate 判定；兩項均同時滿足 median 惡化與至少 6/10
paired worsening。

## 6. Authoritative Engine g4096

| metric | baseline median [min–max] | candidate median [min–max] | delta | paired worsening | gate |
| --- | ---: | ---: | ---: | ---: | --- |
| commands/s | 168,970 [79,196.6–172,392] | 181,487 [85,276.8–183,234] | +7.41% |  | pass |
| actual commands/group | 4,091.96 [4,088.32–4,095.61] | 4,091.96 [4,088.32–4,095.61] | unchanged |  | pass |
| p50 | 44,828.65 [43,423.8–46,352.8] | 41,366.6 [41,157.7–42,318.4] | -7.72% |  | observe |
| p99 | 110,644.5 [94,889–815,282] | 100,771.4 [86,652.5–750,904] | -8.92% | 5/10 | pass |
| p99.9 | 415,990 [392,788–1,121,830] | 412,925.5 [373,747–1,168,440] | -0.74% | 2/10 | pass |
| max | 421,967 [393,161–1,198,270] | 415,682.5 [376,897–1,244,560] | -1.49% | 3/10 | observation |
| CPU s/M commands | 9.269 [9.192–9.447] | 8.854 [8.685–8.974] | -4.48% | 0/10 | pass |
| voluntary/M | 82,194 [78,665–94,518] | 79,807 [74,676–87,756] | -2.90% | 3/10 | pass |
| involuntary/M | 2,318.7 [1,283.3–3,536.7] | 2,181.7 [1,388.4–3,486.6] | -5.91% | 4/10 | pass |
| max RSS KiB | 2,534,176 [2,532,052–2,601,936] | 2,535,790 [2,483,924–2,605,788] | +0.064% | 4/10 | observation |

Engine storage sync tail：sync p50 `2,743.5 -> 2,760 us` (+0.60%)，p99 `12,693.5 -> 13,758 us`
(+8.39%)，p99.9 `50,928 -> 46,078.5 us` (-9.52%)，max `61,367.5 -> 56,200 us` (-8.42%)。
`>25 ms` median 為 5→5，`>100 ms` 為 0→0，`>250 ms` 為 0→0；storage tail 仍有高變異，
但沒有形成本次 Engine gate failure。

## 7. 排程診斷

| metric | baseline | candidate | delta | interpretation |
| --- | ---: | ---: | ---: | --- |
| writer involuntary/M | 3,223.7 | 3,709.6 | +15.07% | formal `/usr/bin/time -v` observation；gate fail |
| Engine involuntary/M | 2,318.7 | 2,181.7 | -5.91% | Engine 未重現 writer regression |
| `perf stat` | not measured | not measured |  | `perf_event_paranoid=4` 阻擋普通使用者 |

沒有可用的 `perf` counters，因此不能把 writer context-switch regression 歸因給 CPU migrations、
scheduler preemption、wakeup/blocking、WAL 或 fsync。這次資料只支持「writer workload 下可重現的
資源護欄惡化」，不支持更細的 OS 根因。

## 8. Gate 逐項判定

| gate | required | observed | pass/fail |
| --- | --- | --- | --- |
| correctness / durability | 40/40 formal runs status 0 | 40/40 | pass |
| writer profile sample | 每輪至少 50 groups | 20/20 為 73 | pass |
| writer publish | reduction ≥15% | 81.70% reduction | pass |
| writer append | candidate median lower | 951.176 < 1,657.20 ns/command | pass |
| writer p99 | 不得 median 惡化且 ≥6/10 paired worsening | +4.67%，6/10 | **fail** |
| writer p99.9 | 不得 median 惡化且 ≥6/10 paired worsening | +9.89%，6/10 | **fail** |
| writer involuntary context | 不得 median 惡化且 ≥6/10 paired worsening | +15.07%，7/10 | **fail** |
| Engine throughput | regression ≤3% | +7.41% | pass |
| Engine supply | group fill ≥90% | 4,088.32–4,095.61 / 4,096 | pass |
| Engine p99 / p99.9 | 同一 latency guardrail | -8.92% / -0.74% | pass |
| Engine context | 同一 context guardrail | -5.91%，4/10 worsening | pass |
| 15-pair p99.9 | median delta ≤0 或 worsening <9/15 | +11.95%，9/15 | **fail** |
| 15-pair involuntary context | median delta ≤0 或 worsening <9/15 | +15.84%，12/15 | **fail** |
| byte identity | 本次 scope 外 | not measured；原報告 pass | not measured |
| repository/binary identity | before/after unchanged | diff/hash empty | pass |

## 9. 與原結果的合併結論

- original powersave result：writer p99.9 3/5 worsening、involuntary context 5/5 worsening，原結論
  `performance guardrail failed / do not retain`。
- confirmatory powersave result：writer p99、p99.9、involuntary context gates 仍 fail；Engine gates
  通過。
- 15-pair p99.9 gate：原 5 輪加本次 10 輪後為 9/15 worsening，仍 fail。
- 15-pair involuntary-context gate：原 5 輪加本次 10 輪後為 12/15 worsening，仍 fail。
- 判定：candidate 不保留、不擴大 patch；publish／append 局部改善可以作為後續優化依據，但目前不能
  宣稱整體 production acceptance。
- 根因定位：問題穩定出現在 writer g4096 profile-on path，Engine 與局部 WAL correctness 沒有重現；
  由於 perf 不可用，尚未能區分 writer scheduler、queue interaction、storage tail 或其他 host
  contention。
- 下一步只做一件事：另行設計 writer g4096 的排程／storage-tail root-cause attribution，補足 thread-level
  scheduler counters；在此之前不再調整 publish、Publisher 或 Completion。

## Appendix A. 每輪原始摘要

CPU 欄位為每百萬 commands；writer 的 `profiled_groups` 應為 73，Engine 為 `n/a`。

| case | round | artifact | RPS | p50 us | p99 us | p99.9 us | max us | actual/group | CPU s/M | involuntary/M | profiled | valid |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| writer | 1 | baseline | 167692.0 | 45056.6 | 104009.0 | 442228.0 | 445969.0 | 4093.98 | 9.616 | 3385.6 | 73 | yes |
| writer | 1 | candidate | 180037.0 | 42071.8 | 100077.0 | 369529.0 | 376033.0 | 4090.44 | 9.663 | 3878.8 | 73 | yes |
| writer | 2 | baseline | 166883.0 | 45269.7 | 98771.2 | 433555.0 | 435636.0 | 4090.44 | 9.789 | 2996.3 | 73 | yes |
| writer | 2 | candidate | 177205.0 | 42661.2 | 110294.0 | 433279.0 | 439365.0 | 4093.98 | 9.373 | 4415.1 | 73 | yes |
| writer | 3 | baseline | 113315.0 | 44874.9 | 384437.0 | 486998.0 | 599504.0 | 4093.98 | 9.426 | 6008.8 | 73 | yes |
| writer | 3 | candidate | 86634.0 | 43226.8 | 706833.0 | 976364.0 | 1064830.0 | 4093.98 | 9.204 | 2901.3 | 73 | yes |
| writer | 4 | baseline | 168124.0 | 45368.7 | 97132.9 | 403098.0 | 406028.0 | 4093.98 | 9.654 | 2851.8 | 73 | yes |
| writer | 4 | candidate | 179194.0 | 41741.9 | 96960.4 | 477684.0 | 481844.0 | 4093.98 | 9.267 | 4107.0 | 73 | yes |
| writer | 5 | baseline | 167793.0 | 45396.4 | 104094.0 | 449520.0 | 451564.0 | 4093.98 | 9.745 | 3216.6 | 73 | yes |
| writer | 5 | candidate | 183898.0 | 40858.8 | 90013.2 | 421054.0 | 424588.0 | 4090.44 | 9.176 | 3465.3 | 73 | yes |
| writer | 6 | baseline | 114452.0 | 45602.1 | 518061.0 | 714371.0 | 811111.0 | 4093.98 | 9.570 | 4056.0 | 73 | yes |
| writer | 6 | candidate | 179874.0 | 42051.4 | 103939.0 | 395720.0 | 397741.0 | 4093.98 | 9.193 | 4340.4 | 73 | yes |
| writer | 7 | baseline | 168396.0 | 45549.4 | 97486.3 | 368738.0 | 377034.0 | 4093.98 | 9.699 | 2540.3 | 73 | yes |
| writer | 7 | candidate | 176548.0 | 42205.6 | 104581.0 | 484709.0 | 486405.0 | 4093.98 | 9.238 | 5943.1 | 73 | yes |
| writer | 8 | baseline | 166906.0 | 45086.5 | 113291.0 | 457187.0 | 462177.0 | 4093.98 | 9.682 | 3230.8 | 73 | yes |
| writer | 8 | candidate | 120157.0 | 42144.4 | 492931.0 | 661833.0 | 663975.0 | 4093.98 | 9.130 | 2236.4 | 73 | yes |
| writer | 9 | baseline | 167247.0 | 45712.9 | 91031.0 | 385724.0 | 388368.0 | 4093.98 | 9.722 | 4246.1 | 73 | yes |
| writer | 9 | candidate | 87993.4 | 43469.6 | 803148.0 | 1179930.0 | 1232290.0 | 4093.98 | 9.276 | 3540.4 | 73 | yes |
| writer | 10 | baseline | 167397.0 | 45271.9 | 94291.1 | 410586.0 | 412733.0 | 4093.98 | 9.745 | 1920.7 | 73 | yes |
| writer | 10 | candidate | 177680.0 | 41593.7 | 107675.0 | 544731.0 | 549336.0 | 4093.98 | 9.276 | 2748.4 | 73 | yes |
| engine | 1 | baseline | 169494.0 | 43423.8 | 130462.0 | 583623.0 | 585363.0 | 4095.61 | 9.203 | 1995.7 | n/a | yes |
| engine | 1 | candidate | 117531.0 | 41430.6 | 413192.0 | 568074.0 | 575881.0 | 4091.96 | 8.696 | 1963.8 | n/a | yes |
| engine | 2 | baseline | 168446.0 | 45237.5 | 113016.0 | 393153.0 | 395657.0 | 4091.96 | 9.366 | 2274.5 | n/a | yes |
| engine | 2 | candidate | 183158.0 | 41162.2 | 90748.6 | 388086.0 | 393381.0 | 4088.32 | 8.868 | 2292.8 | n/a | yes |
| engine | 3 | baseline | 170538.0 | 44794.4 | 98595.8 | 392788.0 | 393161.0 | 4091.96 | 9.196 | 3428.7 | n/a | yes |
| engine | 3 | candidate | 182547.0 | 41544.1 | 99317.8 | 379524.0 | 383413.0 | 4088.32 | 8.953 | 2122.2 | n/a | yes |
| engine | 4 | baseline | 115412.0 | 46352.8 | 461025.0 | 747050.0 | 763508.0 | 4088.32 | 9.201 | 2747.0 | n/a | yes |
| engine | 4 | candidate | 116498.0 | 42318.4 | 357945.0 | 706471.0 | 837753.0 | 4091.96 | 8.685 | 3486.6 | n/a | yes |
| engine | 5 | baseline | 170630.0 | 44835.1 | 108273.0 | 409703.0 | 411798.0 | 4091.96 | 9.301 | 2362.9 | n/a | yes |
| engine | 5 | candidate | 85276.8 | 41280.5 | 750904.0 | 1168440.0 | 1244560.0 | 4091.96 | 8.702 | 1840.2 | n/a | yes |
| engine | 6 | baseline | 167366.0 | 45566.4 | 94889.4 | 414812.0 | 422018.0 | 4095.61 | 9.447 | 2208.6 | n/a | yes |
| engine | 6 | candidate | 182391.0 | 41406.9 | 102225.0 | 373747.0 | 376897.0 | 4091.96 | 8.974 | 1720.5 | n/a | yes |
| engine | 7 | baseline | 79196.6 | 45421.4 | 815282.0 | 1121830.0 | 1198270.0 | 4091.96 | 9.266 | 1283.3 | n/a | yes |
| engine | 7 | candidate | 183234.0 | 41157.7 | 86652.5 | 406762.0 | 410242.0 | 4095.61 | 8.840 | 2241.2 | n/a | yes |
| engine | 8 | baseline | 170020.0 | 44498.2 | 115708.0 | 408291.0 | 410539.0 | 4095.61 | 9.192 | 3536.7 | n/a | yes |
| engine | 8 | candidate | 182051.0 | 41326.3 | 89533.6 | 375913.0 | 380332.0 | 4091.96 | 8.964 | 1388.4 | n/a | yes |
| engine | 9 | baseline | 172392.0 | 43897.1 | 99090.7 | 417168.0 | 421916.0 | 4091.96 | 9.273 | 1579.2 | n/a | yes |
| engine | 9 | candidate | 128261.0 | 41767.8 | 441968.0 | 834983.0 | 1024660.0 | 4091.96 | 8.796 | 2671.2 | n/a | yes |
| engine | 10 | baseline | 168437.0 | 44822.2 | 103551.0 | 493265.0 | 496425.0 | 4088.32 | 9.416 | 2384.2 | n/a | yes |
| engine | 10 | candidate | 180923.0 | 41237.4 | 98195.2 | 419089.0 | 421123.0 | 4091.96 | 8.953 | 2266.9 | n/a | yes |

## Appendix B. Artifact 索引

- raw stdout/stderr/status：`$RUN_ROOT/logs/`。
- `/usr/bin/time -v`：`$RUN_ROOT/time/`。
- Engine telemetry：`$RUN_ROOT/telemetry/`。
- WAL data：`$RUN_ROOT/data/`。
- environment 與 CPU policy：`$RUN_ROOT/logs/environment-before.txt`、`$RUN_ROOT/logs/environment-after.txt`、
  `$RUN_ROOT/logs/cpu-policy.txt`。
- identity：`$RUN_ROOT/logs/*-before.sha256`、`$RUN_ROOT/logs/*-after.sha256`。
- perf evidence：`$RUN_ROOT/perf/status.txt`、`$RUN_ROOT/perf/preflight.stderr`，本次為 unavailable。
