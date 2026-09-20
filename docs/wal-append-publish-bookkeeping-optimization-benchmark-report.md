# WAL append publish bookkeeping 最小化壓測報告

## 1. 結論

- result: `inconclusive`
- retain candidate: `no`
- primary reason: correctness 與 identity gate 通過，但 g8192 writer 的五個
  profile-on formal run 只有 41–45 個 `profiled_groups`，低於程序要求的至少 50；因此整份
  formal matrix 不能作為可保留結論。另有 g4096 writer 的 p99.9 與 involuntary context
  switch guardrail 惡化，需在重測前處理／確認。
- observed g4096 writer `wal_publish_ns_per_command` median reduction: 80.49%
  (424.081 → 82.727 ns/command)。這是有效樣本的觀測值，但因上述 validity gate 未通過，
  不宣稱為可保留的效能結論。
- direct WAL regression: none observed；g4096 candidate +29.89%、g8192 candidate
  +23.74% median RPS。
- authoritative Engine regression: none observed；candidate +7.31% median RPS，group
  fill 約 4,092 commands/group，非 supply-limited。

## 2. 測試範圍與方法

- design: `docs/wal-append-publish-bookkeeping-optimization-design.md`
- procedure: `docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md`
- RUN_ROOT: `/home/neojhou/wal-publish-bookkeeping-wQgSFDz7`
- A/B artifacts 在同一 Linux host、同一 ReleaseBenchmark 設定建立；baseline 使用 HEAD 的
  per-record `src/persistence/wal.cpp`，candidate 使用 worktree 的 per-chunk 版本；兩棵 source
  tree 僅該檔案不同。
- 固定條件：W=2、parallel prepare threshold=4096、producer lanes=8192、group delay=1000 us、
  per-group fsync、`taskset -c 2-7`、每 case 五輪、每輪獨立 data directory。
- 正式矩陣：direct WAL g4096/g8192、writer g4096 off/on、writer g8192 off/on、authoritative
  Engine g4096；兩 artifacts × 七 cases × 五輪 = 70 runs。round 1/3/5 為 baseline→candidate，
  round 2/4 為 candidate→baseline。
- pilot 僅用來決定 iterations，不納入正式結果；profile calibration 依序嘗試 N=8、16，N=16
  通過後使用 `SAMPLE_EVERY=16`。
- 不在範圍：W=1/4、group=16384、Publisher／Completion 獨立 microbenchmark、其他 pipeline
  stage、WAL format 或 runtime default 的修改。

## 3. Source、binary 與環境身份

- 執行日期：2026-09-20（formal logs 約 17:51:35–18:44:33 +08:00）。
- HEAD：`7ae86bb4b030695dd713a8c6c3936b6f1cda7342`
- identity before/after：`logs/repository-identity-before.txt` 與
  `logs/repository-identity-after.txt` 比對相同；正式測試期間 staged diff 未改變。
- status（formal 開始及結束均相同，report 建立前取得）：
  `A docs/wal-append-publish-bookkeeping-optimization-design.md`、
  `M src/persistence/wal.cpp`、`M tests/integration/persistence_test.cpp`、
  `?? docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md`。
- cached diff SHA-256：
  `c443c7ed3a44b822476047c722b3d791c3e8ee3c2330e4eda72b02c4e2b77966`
- worktree diff SHA-256（report 建立前）：
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- untracked manifest SHA-256（report 建立前）：
  `3e94abe5e247bbd44612b204b28454e9c553574d8d3c068c1c90e1a310f0301c`
- source tree diff：`logs/source-tree-diff.txt`，只有 `src/persistence/wal.cpp`。
- baseline binary SHA-256：
  `1d07826a99dd9d79950abd9bac8b47b2b917a2b830e449b545eeee93167fd2cb`
- candidate binary SHA-256：
  `d10005dbc4335d60c438deba393c7a67d3ad5f079c253e8dbe99992dea059b29`
  （`binary-sha256-before.txt` 與 `binary-sha256-after.txt` 相同。）
- compiler：GCC 13.3.0；build type：Release；C++20；benchmark binary 為外部 snapshot 建置。
- CPU：AMD Ryzen 7 3700X 8-Core Processor，16 logical CPUs，SMT enabled，boost enabled；affinity
  `2-7`；governor `powersave`。
- kernel：Linux 6.17.0-35-generic x86_64。
- filesystem：`/dev/sdb2`、ext4、`rw,relatime`；root block topology 為 `sdb`→`sdb2`；開始前
  可用空間約 265 GiB。run root 最終約 93 GiB，結束時可用空間約 173 GiB。
- CPU/context/RSS 由 `/usr/bin/time -v` 取得；sync tail 與 phase attribution 由 benchmark
  telemetry／summary 取得。

## 4. Correctness 與有效性

- repository Debug CTest：98/98 passed。
- repository ASan/UBSan CTest：98/98 passed。
- baseline Release CTest：132/132 passed；candidate Release CTest：132/132 passed。
- smoke：baseline/candidate direct WAL g4096 replay verified；writer g4096 profile off/on
  correctness verified；authoritative Engine g4096 durable/replay/telemetry verified。
- formal status：70/70 exit status 0；writer `correctness_verified=true`、direct WAL
  `replay_verified=true`；Engine telemetry dropped samples=0。
- 沒有因低 throughput、慢 fsync 或高 latency 排除任何 formal run；所有 raw stdout/stderr/status/time/
  telemetry 保留在 RUN_ROOT。
- profile calibration：

  | SAMPLE_EVERY | artifact | group | off median RPS | on median RPS | bias |
  | ---: | --- | ---: | ---: | ---: | ---: |
  | 8 | baseline | 4096 | 169,726 | 166,546 | 1.87% |
  | 8 | candidate | 4096 | 122,420 | 178,792 | 46.05% |
  | 8 | baseline | 8192 | 170,740 | 165,117 | 3.29% |
  | 8 | candidate | 8192 | 182,553 | 129,328 | 29.16% |
  | 16 | baseline | 4096 | 169,277 | 166,273 | 1.77% |
  | 16 | candidate | 4096 | 181,440 | 180,926 | 0.28% |
  | 16 | baseline | 8192 | 171,861 | 165,065 | 3.95% |
  | 16 | candidate | 8192 | 181,290 | 176,718 | 2.52% |

  因此 `SAMPLE_EVERY=16` 通過四組 bias ≤5% 的 calibration gate。
- formal profile-on 的 `profiled_groups`：g4096 baseline/candidate 均為 73；g8192 baseline
  為 41/43/43/43/44、candidate 為 41/43/44/44/45。g8192 低於程序要求的 50，為本報告
  `inconclusive` 的主要有效性原因。
- supply-limited：authoritative Engine g4096 的 actual commands/group baseline
  4091.96–4095.61、candidate 4091.96–4095.61；沒有 group-fill 不足的 supply-limited run。

## 5. 正式結果

### 5.1 Primary writer g4096

中心值為五輪 median，range 為五輪 min–max；latency 單位為 µs，phase metrics 單位為
ns/command。

| metric | baseline median [min–max] | candidate median [min–max] | delta | gate |
| --- | ---: | ---: | ---: | --- |
| commands/s | 168,749 [118,763–171,107] | 181,537 [177,804–182,628] | +7.58% | observe |
| p50 | 45,134.3 [44,475.3–45,660.1] | 41,589.4 [40,899.4–41,968.6] | -7.85% | observe |
| p99 | 96,927.6 [87,806.9–481,141] | 95,689.3 [89,164.2–102,066] | -1.28% | tail guardrail |
| p99.9 | 414,024 [355,933–1,540,220] | 418,753 [359,702–535,918] | +1.14% | tail guardrail |
| max | 418,554 [360,087–2,058,520] | 420,894 [363,084–536,817] | +0.56% | tail guardrail |
| `wal_publish_ns_per_command` | 424.081 [411.192–428.895] | 82.727 [80.5317–83.6874] | 80.49% reduction | ≥15% |
| `wal_append_ns_per_command` | 1,705.24 [1,191.34–1,766.10] | 1,006.59 [848.656–1,450.24] | 40.97% reduction | candidate lower |
| actual commands/group | 4093.98 [4090.44–4093.98] | 4093.98 [4090.44–4093.98] | unchanged | correctness |

The publish and append phase signal is strong, but the p99.9 median is slightly higher and the
profiled g4096 involuntary context-switch median is higher (see gates below), so this table alone
does not authorize retention.

### 5.2 Direct WAL controls

`p99`/`p99.9` are group append latency in µs. Regression is zero when candidate is faster.

| group | artifact | RPS median [min–max] | WAL MiB/s median [min–max] | p99 / p99.9 median [min–max] | regression |
| ---: | --- | ---: | ---: | ---: | ---: |
| 4096 | baseline | 442,016 [207,740–495,256] | 51.4278 [24.1702–57.6222] | 6,956.044 / 370,910.445 | 0% |
| 4096 | candidate | 574,131 [502,707–579,618] | 66.7992 [58.4891–67.4375] | 5,359.801 / 368,455.175 | 0% |
| 8192 | baseline | 520,708 [238,082–538,311] | 60.5835 [27.7004–62.6316] | 13,327.166 / 734,328.244 | 0% |
| 8192 | candidate | 644,342 [276,752–650,346] | 74.9681 [32.1996–75.6666] | 10,432.193 / 746,905.166 | 0% |

Candidate median RPS deltas are +29.89% at g4096 and +23.74% at g8192. The g8192 p99.9
median is 1.71% higher despite the RPS gain; this is retained as a tail observation, not
discarded.

### 5.3 Authoritative Engine g4096

| artifact | RPS median [min–max] | group fill | p50 / p99 / p99.9 / max (µs) | regression |
| --- | ---: | ---: | ---: | ---: |
| baseline | 170,202 [75,663.1–173,013] | 4091.96 [4091.96–4095.61] | 44,411.8 / 101,906 / 460,473 / 478,488 | 0% |
| candidate | 182,646 [76,239.3–183,704] | 4091.96 [4091.96–4095.61] | 41,439.1 / 102,447 / 432,527 / 435,186 | 0% |

Candidate median RPS delta is +7.31%; group fill shows the comparison is not supply-limited.
Candidate p99 is +0.53%, while p99.9 and max improve by 6.07% and 9.06% respectively.

### 5.4 Writer g8192 方向性資料

| metric | baseline | candidate | delta | supply-limited note |
| --- | ---: | ---: | ---: | --- |
| profile off RPS | 115,967 [97,130.8–168,352] | 180,099 [87,321.5–183,655] | +55.30% | direction only |
| profile on RPS | 165,657 [113,529–167,425] | 180,699 [147,073–181,842] | +9.08% | direction only |
| profile-on actual commands/group | 6,730.01 [6,633.18–7,191.98] | 6,652.32 [6,493.04–7,191.98] | not comparable to g4096 | configured group=8192，fill is not full |
| `wal_publish_ns_per_command` | 431.438 [412.264–434.908] | 84.2667 [80.789–84.9509] | 80.47% reduction | not primary gate |
| `wal_append_ns_per_command` | 1,335.60 [1,261.28–2,656.88] | 940.598 [903.834–2,462.88] | 29.54% reduction | not primary gate |

The g8192 profile-on sample count is only 41–45 groups per run, so these rows are directional
only and cannot repair the inconclusive validity gate.

### 5.5 CPU、context switches 與 RSS

Values are five-run median [min–max]. CPU and context values are normalized per million commands;
RSS is maximum resident set size in KiB from `/usr/bin/time -v`.

| case | artifact | CPU s/M commands | voluntary/M | involuntary/M | max RSS KiB |
| --- | --- | ---: | ---: | ---: | ---: |
| direct WAL g4096 | baseline | 3.01769 [2.97898–3.02067] | 1,663.27 [1,639.81–1,667.36] | 156.059 [145.561–186.959] | 4,331,070 [4,330,910–4,331,230] |
| direct WAL g4096 | candidate | 2.71689 [2.68786–2.72359] | 1,651.50 [1,650.39–1,665.58] | 157.995 [124.713–180.928] | 4,331,080 [4,330,980–4,331,180] |
| writer g4096 on | baseline | 9.65625 [9.61606–9.70489] | 88,774 [85,051.3–90,392.3] | 2,705.48 [1,998.29–3,007.27] | 2,550,870 [2,547,240–2,551,220] |
| writer g4096 on | candidate | 9.24809 [9.19945–9.29038] | 84,597.5 [79,539–88,234.1] | 3,483.52 [2,856.48–4,728.3] | 2,550,580 [2,546,850–2,566,780] |
| writer g8192 on | baseline | 9.93479 [9.57037–10.0042] | 89,468 [87,434.2–92,996] | 2,621.22 [1,794.12–3,045.95] | 2,546,080 [2,544,680–2,556,970] |
| writer g8192 on | candidate | 9.50530 [9.46192–9.56604] | 88,474.1 [84,813–94,321.1] | 2,456.58 [1,802.15–2,682.61] | 2,556,740 [2,545,580–2,573,990] |
| Engine g4096 | baseline | 9.18335 [9.02014–9.30522] | 82,491.5 [73,027.9–88,294.9] | 2,793.52 [2,412.48–4,594.07] | 2,536,070 [2,535,910–2,554,430] |
| Engine g4096 | candidate | 8.89392 [8.72854–8.93962] | 82,125.9 [79,875.8–91,399.8] | 2,829.65 [1,617.53–3,654.84] | 2,536,150 [2,536,040–2,554,370] |

The primary writer CPU cost falls, but writer g4096 involuntary context switches rise 28.75%
(2,705.48 → 3,483.52 per million commands) and worsen in all five paired rounds. This is a
guardrail failure, not an excluded sample.

### 5.6 Storage sync tail 與 downstream 護欄

All latency values are µs; `>25/>100/>250 ms` are counts per Engine run. Direct WAL does not
emit threshold counts, so those cells are `not measured` rather than inferred from a percentile.

| artifact/case | sync p50 / p99 / p99.9 / max | >25 / >100 / >250 ms | publisher lag (events / bytes / age) | completion queue p50 / p99 / p99.9 / max |
| --- | --- | --- | --- | --- |
| baseline direct WAL g4096 | 2,605.896 / 13,368.819 / 163,965.900 / 212,350.552 | not measured | not measured | not measured |
| candidate direct WAL g4096 | 2,589.095 / 9,792.043 / 28,403.403 / 52,339.889 | not measured | not measured | not measured |
| baseline direct WAL g8192 | 3,616.732 / 12,702.931 / 51,404.747 / 428,571.442 | not measured | not measured | not measured |
| candidate direct WAL g8192 | 3,541.622 / 11,808.049 / 51,185.046 / 65,908.405 | not measured | not measured | not measured |
| baseline Engine g4096 | 2,764 / 14,220 / 57,311 / 60,358 | 4 / 0 / 0 (median; max 117/114/73) | not measured | not measured |
| candidate Engine g4096 | 2,748 / 12,895 / 52,862 / 61,562 | 3 / 0 / 0 (median; max 135/131/65) | not measured | not measured |
| baseline writer g4096 on | not measured | not measured | 3,343,650 / 407,925,300 / 20.055 s | 2,612.16 / 7,436.26 / 9,205.29 / 9,405.61 |
| candidate writer g4096 on | not measured | not measured | 3,351,546 / 408,888,612 / 18.659 s | 2,591.02 / 7,541.32 / 8,935.60 / 9,056.60 |

The candidate improves most direct/Engine sync medians, but storage tails remain highly variable;
publisher lag remains about 3.3 million events because Publisher is intentionally outside this
optimization and was not changed.

## 6. Primary phase attribution

All rows are g4096 writer profile-on medians, ns/command; ranges are omitted here because the raw
five values are in Appendix A and the profile lines are retained in RUN_ROOT.

| phase ns/command | baseline median | candidate median | reduction | interpretation |
| --- | ---: | ---: | ---: | --- |
| WAL prepare | 651.993 | 680.530 | -4.38% | slightly higher; not the target phase |
| WAL plan/copy | 578.143 | 146.084 | 74.73% | observed attribution; not a separate code change in this patch |
| WAL publish | 424.081 | 82.727 | 80.49% | primary target; per-chunk bookkeeping removes repeated per-record work |
| WAL append | 1,705.240 | 1,006.590 | 40.97% | publish reduction propagates into append path |
| WAL write | 41.5811 | 40.6648 | 2.20% | direct write cost essentially unchanged |
| WAL sync | 788.412 | 730.804 | 7.31% | sampled sync contribution; direct sync tail still storage-sensitive |
| StateMachine apply | 1,435.990 | 1,405.380 | 2.13% | downstream apply is not materially changed |
| completion enqueue | 393.548 | 381.587 | 3.04% | outside target; small observed variation |

The phase profile shows the intended publish reduction and a lower append service cost, but complete
writer throughput rises only 7.58% because apply, sync, queueing and storage tails remain in the
same synchronous chain. `plan/copy` is reported for attribution only; this benchmark does not
authorize changing it in the same patch.

## 7. Gate 逐項判定

| gate | required | observed | pass/fail |
| --- | --- | --- | --- |
| primary publish | g4096 writer publish reduction ≥15% | 80.49% observed | pass (not sufficient for retain) |
| append direction | candidate append median lower | 1,006.59 < 1,705.24 ns/command | pass |
| calibration bias | all four artifact/group biases ≤5% | N=16: 0.28%–3.95% | pass |
| profile sample count | every formal profile-on run ≥50 groups | g4096=73; g8192=41–45 | **fail** |
| direct WAL g4096 | regression ≤3% | candidate +29.89% RPS | pass |
| direct WAL g8192 | regression ≤3% | candidate +23.74% RPS | pass |
| authoritative Engine | regression ≤3%, non-supply-limited | candidate +7.31%; fill ≈4092/group | pass |
| correctness/replay/durability | all gates pass | 70/70 status 0; replay/correctness true | pass |
| tail/resource guardrail | no median worsening with ≥3/5 paired worsening | writer g4096 p99.9 +1.14% (4/5); involuntary context +28.75% (5/5) | **fail** |
| repository/binary identity | before/after identical | identity and binary diffs empty | pass |

Because the profile sample-count gate fails, the formal performance result is inconclusive even
though the primary phase and direct WAL guardrails look favorable. The tail/context result is also
not sufficient to mark the candidate retainable.

## 8. 結論與下一步

- Do not retain the candidate based on this run and do not expand this patch.
- Re-run the complete affected formal comparison after increasing the g8192 writer command budget
  enough to produce at least 50 profile groups per profile-on run (the current 2.305M-iteration
  budget produced only 41–45; recalculate the budget from a fresh pilot rather than multiplying
  only one artifact). Keep the same A/B alternation and calibration procedure.
- During that rerun, verify whether the g4096 p99.9 and involuntary-context-switch degradation
  persists under the same storage state. Do not discard slow fsync tails or rerun only the faster
  artifact.
- If the rerun passes validity but still fails the guardrail, return to a separate benchmark-only
  attribution for plan/copy, cache insertion and terminal position update. Publisher and Completion
  remain out of scope for this patch.
- No source, staging, group-size, fsync policy, prepare-worker or runtime-default change was made
  as part of this benchmark.

## Appendix A. 每輪原始摘要

Columns: `RPS` is commands/s; latency is `p50/p99/p99.9` (and direct WAL uses append-group
latency with group-total max); CPU is process user+system seconds; context columns are raw counts;
all rows are valid process exits, but g8192 profile-on rows additionally fail the ≥50 profiled-group
validity rule.

| artifact | case | round | RPS | p50 µs | p99 µs | p99.9 µs | max/group fill | CPU s | voluntary | involuntary | valid |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | direct_wal_g4096 | 1 | 442016 | 4370.305 | 6956.044 | 363986.111 | 1314838.862 | 40.01 | 22394 | 2081 | yes |
| baseline | direct_wal_g4096 | 2 | 495256 | 4320.692 | 6376.416 | 370910.445 | 1222966.766 | 40.49 | 22342 | 1955 | yes |
| baseline | direct_wal_g4096 | 3 | 391234 | 4434.866 | 6967.646 | 377639.839 | 1228324.095 | 40.56 | 22024 | 2096 | yes |
| baseline | direct_wal_g4096 | 4 | 488779 | 4429.496 | 6811.433 | 375674.274 | 1210757.910 | 40.53 | 22094 | 2511 | yes |
| baseline | direct_wal_g4096 | 5 | 207740 | 4484.928 | 7216.440 | 363316.803 | 1340314.705 | 40.57 | 22339 | 2386 | yes |
| candidate | direct_wal_g4096 | 1 | 574131 | 3188.098 | 5105.855 | 365791.539 | 1187879.815 | 36.10 | 22181 | 2160 | yes |
| candidate | direct_wal_g4096 | 2 | 574716 | 3233.463 | 5359.801 | 379873.426 | 1199949.918 | 36.51 | 22195 | 1898 | yes |
| candidate | direct_wal_g4096 | 3 | 572317 | 3198.177 | 5428.740 | 374023.699 | 1220543.648 | 36.58 | 22181 | 2430 | yes |
| candidate | direct_wal_g4096 | 4 | 502707 | 3022.077 | 5008.372 | 348150.510 | 1202491.978 | 36.49 | 22370 | 1675 | yes |
| candidate | direct_wal_g4096 | 5 | 579618 | 3223.523 | 5521.822 | 368455.175 | 1243147.976 | 36.44 | 22166 | 2122 | yes |
| baseline | direct_wal_g8192 | 1 | 538311 | 8844.907 | 12990.580 | 734328.244 | 1628549.006 | 53.13 | 14705 | 2432 | yes |
| baseline | direct_wal_g8192 | 2 | 520708 | 8922.931 | 14299.364 | 734131.476 | 1522394.341 | 53.83 | 14040 | 3147 | yes |
| baseline | direct_wal_g8192 | 3 | 238082 | 8578.046 | 13498.533 | 724938.467 | 1627058.888 | 52.22 | 14801 | 2463 | yes |
| baseline | direct_wal_g8192 | 4 | 325320 | 8651.143 | 12075.655 | 756640.525 | 1579991.361 | 52.66 | 14600 | 2396 | yes |
| baseline | direct_wal_g8192 | 5 | 531886 | 8802.002 | 13327.166 | 737661.548 | 1663888.035 | 52.76 | 14450 | 3273 | yes |
| candidate | direct_wal_g8192 | 1 | 401619 | 6444.054 | 10426.472 | 744007.189 | 1788834.289 | 48.49 | 14542 | 2442 | yes |
| candidate | direct_wal_g8192 | 2 | 276752 | 6427.473 | 10926.369 | 746905.166 | 2530414.610 | 48.02 | 14600 | 2255 | yes |
| candidate | direct_wal_g8192 | 3 | 644342 | 6369.775 | 9839.531 | 748696.563 | 1660985.662 | 48.13 | 14520 | 2581 | yes |
| candidate | direct_wal_g8192 | 4 | 648033 | 6393.820 | 10432.193 | 743608.476 | 1623880.101 | 48.23 | 14586 | 2751 | yes |
| candidate | direct_wal_g8192 | 5 | 650346 | 6478.426 | 11100.602 | 751629.390 | 1558141.679 | 47.59 | 14539 | 2473 | yes |
| baseline | writer_g4096_off | 1 | 130508 | 44514.4 | 330752 | 419633 | 4093.98 | 44.05 | 385773 | 12800 | yes |
| baseline | writer_g4096_off | 2 | 115881 | 45837.3 | 409475 | 519278 | 4093.98 | 45.30 | 425341 | 12581 | yes |
| baseline | writer_g4096_off | 3 | 133237 | 44992.6 | 388408 | 462281 | 4093.98 | 45.24 | 409984 | 14215 | yes |
| baseline | writer_g4096_off | 4 | 170487 | 44326.3 | 96095.8 | 420451 | 4090.44 | 44.80 | 378596 | 22870 | yes |
| baseline | writer_g4096_off | 5 | 63982.9 | 46264.3 | 1034860 | 1204620 | 4093.98 | 44.59 | 394109 | 14267 | yes |
| candidate | writer_g4096_off | 1 | 182089 | 41225.9 | 89639.6 | 404682 | 4090.44 | 43.10 | 381646 | 15789 | yes |
| candidate | writer_g4096_off | 2 | 143595 | 42050.1 | 359827 | 542306 | 4093.98 | 43.16 | 385492 | 21123 | yes |
| candidate | writer_g4096_off | 3 | 120010 | 41695.7 | 336664 | 490032 | 4093.98 | 42.74 | 377408 | 14620 | yes |
| candidate | writer_g4096_off | 4 | 177493 | 42046.8 | 109128 | 509720 | 4090.44 | 43.77 | 396625 | 13957 | yes |
| candidate | writer_g4096_off | 5 | 182597 | 41045.6 | 102284 | 396828 | 4093.98 | 43.06 | 369868 | 26600 | yes |
| baseline | writer_g4096_on | 1 | 123369 | 45571.7 | 481141 | 1540220 | 4093.98 | 45.66 | 413934 | 14220 | yes |
| baseline | writer_g4096_on | 2 | 168749 | 45096.9 | 96927.6 | 414024 | 4093.98 | 45.87 | 427424 | 9449 | yes |
| baseline | writer_g4096_on | 3 | 169126 | 45134.3 | 87806.9 | 409695 | 4093.98 | 45.89 | 419772 | 13031 | yes |
| baseline | writer_g4096_on | 4 | 118763 | 45660.1 | 374521 | 498271 | 4093.98 | 45.56 | 402169 | 12793 | yes |
| baseline | writer_g4096_on | 5 | 171107 | 44475.3 | 90175.2 | 355933 | 4093.98 | 45.47 | 421318 | 11340 | yes |
| candidate | writer_g4096_on | 1 | 181537 | 41968.6 | 89164.2 | 359702 | 4093.98 | 43.70 | 417219 | 16472 | yes |
| candidate | writer_g4096_on | 2 | 182628 | 40899.4 | 95689.3 | 424227 | 4090.44 | 43.50 | 376104 | 15814 | yes |
| candidate | writer_g4096_on | 3 | 177804 | 41906.5 | 102066 | 535918 | 4093.98 | 43.82 | 399248 | 22358 | yes |
| candidate | writer_g4096_on | 4 | 181644 | 41244 | 96233.7 | 418753 | 4093.98 | 43.93 | 403399 | 13507 | yes |
| candidate | writer_g4096_on | 5 | 180615 | 41589.4 | 93272.5 | 398479 | 4093.98 | 43.73 | 400023 | 18568 | yes |
| baseline | writer_g8192_off | 1 | 168352 | 44137.2 | 108762 | 566933 | 7136.32 | 45.25 | 393943 | 8151 | yes |
| baseline | writer_g8192_off | 2 | 115963 | 45659.3 | 364055 | 432153 | 6860.21 | 44.31 | 393320 | 11633 | yes |
| baseline | writer_g8192_off | 3 | 115967 | 45388.7 | 340868 | 439807 | 6870.43 | 43.85 | 366373 | 13735 | yes |
| baseline | writer_g8192_off | 4 | 97130.8 | 46053.9 | 555003 | 1239350 | 6984.94 | 45.22 | 431240 | 7110 | yes |
| baseline | writer_g8192_off | 5 | 117311 | 44906.2 | 378519 | 999342 | 7225.8 | 44.71 | 407680 | 7508 | yes |
| candidate | writer_g8192_off | 1 | 87321.5 | 43268.9 | 651526 | 1311300 | 6779.5 | 41.85 | 390613 | 14452 | yes |
| candidate | writer_g8192_off | 2 | 181306 | 41612.5 | 114111 | 446031 | 6779.5 | 43.74 | 421824 | 10762 | yes |
| candidate | writer_g8192_off | 3 | 180099 | 42519.3 | 99639.7 | 367753 | 6614.15 | 43.64 | 434300 | 10644 | yes |
| candidate | writer_g8192_off | 4 | 183655 | 40715.8 | 94651.8 | 383036 | 7248.52 | 43.50 | 394053 | 6646 | yes |
| candidate | writer_g8192_off | 5 | 136965 | 42456.8 | 287157 | 407160 | 6690.94 | 42.96 | 393131 | 10925 | yes |
| baseline | writer_g8192_on | 1 | 123954 | 45799.5 | 444641 | 762329 | 6633.18 | 45.25 | 408157 | 14042 | yes |
| baseline | writer_g8192_on | 2 | 167425 | 45500.2 | 106656 | 426056 | 6730.01 | 46.12 | 428717 | 8271 | yes |
| baseline | writer_g8192_on | 3 | 165657 | 44419.5 | 115994 | 588814 | 7191.98 | 45.94 | 412453 | 8405 | yes |
| baseline | writer_g8192_on | 4 | 113529 | 46189.2 | 299465 | 479454 | 6710.42 | 44.12 | 403077 | 13842 | yes |
| baseline | writer_g8192_on | 5 | 166416 | 45335.4 | 118470 | 479278 | 6769.54 | 45.80 | 423526 | 12084 | yes |
| candidate | writer_g8192_on | 1 | 180945 | 41752.9 | 112322 | 458457 | 6652.32 | 44.10 | 434826 | 11854 | yes |
| candidate | writer_g8192_on | 2 | 181842 | 41154.6 | 102664 | 374149 | 7191.98 | 43.78 | 396718 | 8308 | yes |
| candidate | writer_g8192_on | 3 | 180699 | 42201.7 | 96004.3 | 380924 | 6493.04 | 43.82 | 428970 | 11325 | yes |
| candidate | writer_g8192_on | 4 | 147073 | 42035.8 | 517195 | 658826 | 6769.54 | 43.82 | 407871 | 10986 | yes |
| candidate | writer_g8192_on | 5 | 175486 | 41762.5 | 125520 | 544962 | 6576.41 | 43.62 | 390993 | 12367 | yes |
| baseline | engine_g4096 | 1 | 170202 | 44737.2 | 101248 | 409451 | 42.76 | 396096 | 17772 | yes |
| baseline | engine_g4096 | 2 | 173013 | 43681.7 | 97870.3 | 420012 | 41.52 | 335583 | 21111 | yes |
| baseline | engine_g4096 | 3 | 75663.1 | 44411.8 | 901405 | 1136840 | 41.45 | 379071 | 11086 | yes |
| baseline | engine_g4096 | 4 | 116474 | 45450 | 436778 | 846871 | 42.31 | 405739 | 12605 | yes |
| baseline | engine_g4096 | 5 | 170786 | 44149.7 | 101906 | 460473 | 42.20 | 369027 | 12837 | yes |
| candidate | engine_g4096 | 1 | 182214 | 41439.1 | 102447 | 403118 | 41.07 | 380384 | 13003 | yes |
| candidate | engine_g4096 | 2 | 76239.3 | 42165.7 | 747701 | 1224920 | 40.11 | 367051 | 16795 | yes |
| candidate | engine_g4096 | 3 | 182646 | 41842.7 | 103299 | 389604 | 41.08 | 420007 | 7433 | yes |
| candidate | engine_g4096 | 4 | 183704 | 41011.8 | 100836 | 432527 | 40.87 | 377391 | 10753 | yes |
| candidate | engine_g4096 | 5 | 182779 | 40931.9 | 100366 | 477088 | 40.67 | 370764 | 14241 | yes |

The direct-WAL `max/group fill` column is group-total max latency; writer/Engine rows use actual
commands/group. The complete raw files, including exact WAL bytes, sync counts and phase lines, are
under `$RUN_ROOT/logs`.

## Appendix B. Artifact 索引

- baseline/candidate binaries：
  `$RUN_ROOT/source/{baseline,candidate}/build/ReleaseBenchmark/benchmarks/order_books_benchmark`
- source tree comparison：`$RUN_ROOT/logs/source-tree-diff.txt`
- identity before/after：`$RUN_ROOT/logs/repository-identity-before.txt`、
  `$RUN_ROOT/logs/repository-identity-after.txt`
- binary hashes：`$RUN_ROOT/logs/binary-sha256-before.txt`、
  `$RUN_ROOT/logs/binary-sha256-after.txt`
- environment：`$RUN_ROOT/logs/environment.txt`
- calibration TSV：`$RUN_ROOT/derived/calibration-cal2-n8.tsv`、
  `$RUN_ROOT/derived/calibration-cal2-n16.tsv`
- selected profile sample：`$RUN_ROOT/derived/selected-sample-every-cal2-n.txt`
- formal raw stdout/stderr/status：`$RUN_ROOT/logs/*formal2*`
- `/usr/bin/time -v` raw resource files：`$RUN_ROOT/time/*formal2*`
- Engine tail telemetry CSV：`$RUN_ROOT/telemetry/*formal2-engine*`
- per-run WAL data：`$RUN_ROOT/data/*formal2*`（未複製進 repository）
