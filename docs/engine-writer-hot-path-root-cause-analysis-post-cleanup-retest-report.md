# Engine Writer Hot Path collector cleanup 後重測報告

## 1. 摘要

本報告依據
`docs/engine-writer-hot-path-root-cause-analysis-post-cleanup-retest-procedure.md`
執行，驗證移除 benchmark-only collector progress counters 後，
`engine_writer_hot_path_profile` 的 bias、phase attribution、Completion worker
與 authoritative controls 是否仍支持
`docs/engine-writer-hot-path-root-cause-analysis-design.md` 的結論。

本輪使用同一個 source state 與同一個 Release benchmark artifact。35 輪正式
matrix 全部 exit 0 且納入統計；沒有因吞吐較低、sync tail 或 context switch
較多而排除有效輪次。N=8 calibration 的 off/on median bias 為 0.593278%，
正式 writer group=4,096 的 bias 為 0.662046%，均低於 5% attribution 門檻。
group=256 的 control bias 為 4.052053%，仍低於 5%；direct WAL 的 bias 為
8.608981%，因此只作方向性證據，不能取代 writer phase attribution。

| Workload | Group | Profile | Sample every | RPS median | Range | Bias | Evidence |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| Writer | 4,096 | off | — | 161,167 | 159,582–163,814 | — | diagnostic control |
| Writer | 4,096 | on | 8 | 160,100 | 98,940.2–162,900 | 0.662046% | attribution passed |
| Writer | 256 | off | — | 76,928.9 | 74,999.4–77,542.4 | — | control |
| Writer | 256 | on | 8 | 73,811.7 | 57,890.6–75,606.9 | 4.052053% | control passed |
| Engine durable | 4,096 | authoritative | — | 164,112 | 98,398–167,211 | — | end-to-end control |
| Direct WAL | 4,096 | off | — | 446,615 | 237,735–447,191 | — | WAL control |
| Direct WAL | 4,096 | on | — | 408,166 | 303,710–417,338 | 8.608981% | directional only |

結論：collector cleanup 沒有改變 writer 的主要根因判斷。穩定的主要成本仍
是 WAL append 與 `StateMachine::apply`；WAL prepare 約 1.061 us/command，
其中 payload encode 與 CRC 合計約 65.5%。Completion service rate 約 2.709M/s，
遠高於約 160K/s 的 writer arrival，沒有 Completion throughput bottleneck 證據。
唯一選定的下一個 production optimization 仍是 **bounded parallel WAL prepare**，
先驗證最多四個有界 worker；sync/group-commit sweep 是後續獨立診斷，不在本輪
同時實作。Direct WAL 與部分 writer 輪次的 fsync 長尾必須在該 prototype 前後
繼續以同一套同步分布量測，不能由本報告將其無條件外推為整個 Engine writer
的唯一根因。

## 2. 測試身份與環境

- 測試日期：2026-09-19（Asia/Taipei）。
- Host/kernel：Linux `master`, `6.17.0-35-generic`。
- CPU：AMD Ryzen 7 3700X；16 logical CPUs、8 physical cores、2 threads/core；
  boost enabled。
- CPU affinity：`taskset -c 2-7`。
- Governor：`powersave`，未修改；測試期間未切換 boost、I/O scheduler 或 mount options。
- Filesystem：`/dev/sdb2`、ext4、`rw,relatime`；沒有使用 tmpfs 或 overlay。
- 測試資料所在 filesystem 可用空間約 551 GiB；formal run 完成後 raw artifact
  約 20 GiB，資料目錄保留。
- Compiler：GCC 13.3.0。
- Release flags：`-std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread`。
- Debug flags：`-std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Werror -pthread`。
- Sanitizer flags：`-std=c++20 -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer`。
- WAL：`per_group` fsync，group size 4,096；每輪使用新的空 data directory；
  group delay 1,000 us，producer lanes 8,192。
- HEAD：`b63c27e189835baa67b612385f16a2b019141b53`。
- index/staged diff SHA-256：
  `d06af1740242732a7a94ea792e2f4efb60efb0a4230e7a337630179c572afda8`。
- worktree diff SHA-256（formal matrix identity freeze 時）：
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`。
- Release benchmark：`/tmp/order-books-writer-retest-build-AR2PRgDo/order_books_benchmark`。
- Benchmark SHA-256：
  `b275441175c9538d8336e876d9d6bf457af45dba29e8cf819e8023b037df4973`。
- Run root：`/home/neojhou/engine-writer-post-cleanup-retest-Q7tQ3vBX`。
- identity 原始紀錄：`$RUN_ROOT/logs/source-identity.txt`；environment 原始紀錄：
  `$RUN_ROOT/logs/environment.txt`。

`cmake`、`ninja` 與 Conan 在本機不可用，因此依操作檔的等價 GCC 路徑完成
build 與 tests；沒有因此跳過測試。formal matrix 前後 source、index、worktree
與 binary identity 一致；本報告檔案是在 identity freeze 與正式量測完成後建立，
不屬於受測 binary 的 source identity。

## 3. Build、correctness 與 calibration

### 3.1 Build 與 correctness

- Release GoogleTest：75/75 通過，`-Werror` 無警告。
- Debug GoogleTest：75/75 通過。
- ASan/UBSan GoogleTest：75/75 通過，無 sanitizer 或 leak error。
- Profile-on smoke：exit 0、`correctness_verified=true`、
  `profiled_commands=completion_profiled_commands=4096`、`profiled_groups=1`。
- CLI invalid checks：sample interval 為 0、非數字、profile off 時指定 sampling、
  以及非 writer workload 指定 sampling，均以 non-zero exit 回報正確 error code。
- 所有 formal writer 輪次均 `correctness_verified=true`；所有 direct WAL 輪次均
  `replay_verified=true`。

### 3.2 N=8 calibration

固定 group=4,096、delay=1,000 us、lanes=8,192、warmup=10,000、
measured commands=2,000,000，依 `off-r1, on-r1, on-r2, off-r2, off-r3, on-r3`
順序執行。每輪 elapsed 約 12.1–12.4 s，profile-on 每輪 62 sampled groups。

| Run | Profile | RPS | Elapsed (ms) | Sampled groups | Valid |
| --- | --- | ---: | ---: | ---: | --- |
| cal-n8-off-r1 | off | 164,931 | 12,126.3 | — | yes |
| cal-n8-on-r1 | on | 161,705 | 12,368.2 | 62 | yes |
| cal-n8-on-r2 | on | 164,175 | 12,182.1 | 62 | yes |
| cal-n8-off-r2 | off | 161,625 | 12,374.3 | — | yes |
| cal-n8-off-r3 | off | 164,004 | 12,194.9 | — | yes |
| cal-n8-on-r3 | on | 163,031 | 12,267.6 | 62 | yes |

| Sample every | Off median RPS | On median RPS | Bias | Sampled groups/run | 決定 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 164,004 | 163,031 | 0.593278% | 62 | 選用 |
| 16 | — | — | — | — | not run；N=8 已通過 |
| 32 | — | — | — | — | not run；N=8 已通過 |

依程序規則選用 `SAMPLE_EVERY=8`；沒有因 phase 比例事後挑選 sampling interval。

## 4. Formal throughput 與 latency

每個 formal case 均執行五輪，所有 measured phase 至少 15 秒。latency 的 p50、
p99 是五輪 per-run median；p99.9 與 max 報五輪最差值。Direct WAL 的 latency
單位是 **group**，不是 command。

| Workload | Group | Profile | RPS median | RPS range | Elapsed median (ms) | p50 median (us) | p99 median (us) | Worst p99.9 (us) | Worst max (us) |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Writer | 4,096 | off | 161,167 | 159,582–163,814 | 20,847.9 | 47,066.5 | 110,721 | 394,197 | 397,473 |
| Writer | 4,096 | on | 160,100 | 98,940.2–162,900 | 20,986.8 | 47,638.1 | 104,540 | 705,682 | 707,594 |
| Writer | 256 | off | 76,928.9 | 74,999.4–77,542.4 | 20,798.4 | 105,499 | 194,475 | 236,024 | 236,810 |
| Writer | 256 | on | 73,811.7 | 57,890.6–75,606.9 | 21,676.8 | 108,036 | 203,858 | 5,584,480 | 5,767,470 |
| Engine durable | 4,096 | authoritative | 164,112 | 98,398–167,211 | 20,473.8 | 46,546.1 | 94,930.9 | 737,084 | 740,696 |
| Direct WAL | 4,096 | off | 446,615 | 237,735–447,191 | 19,287.1 | 7,845.9 group | 16,085.4 group | 427,638.3 group | 850,423.2 group |
| Direct WAL | 4,096 | on | 408,166 | 303,710–417,338 | 21,103.9 | 8,435.5 group | 18,253.1 group | 532,609.9 group | 880,598.8 group |

Authoritative Engine median 為 1M commands/s 目標的 16.411%；writer profile-off
為 16.117%；direct WAL profile-off 為 44.661%。Writer profile-off 與 authoritative
Engine 的 median 差異為 1.795%（以 authoritative Engine 為分母）。

慢輪次全部保留：writer g=4,096 on-r2 為 98,940.2 RPS，g=256 on-r2 為
57,890.6 RPS，Engine control r1 為 98,398 RPS；它們均 correctness 通過，
不得以 outlier 名義移除。

## 5. Writer 與 WAL phase（group=4,096，profile-on，N=8）

以下所有 `ns/command` 先以該輪 sampled commands 為 denominator，再取五輪
median；share 亦先逐輪以相同 parent denominator 計算，再取 median。Writer
service 不包含 Completion worker wall time。

| Phase | ns/command median | min–max | Writer-service share median | 跨輪穩定性 | 證據等級 |
| --- | ---: | ---: | ---: | --- | --- |
| admission | 433.512 | 425.008–447.243 | 7.402% | absolute 穩定；share 受慢輪影響 | 主要以外 |
| WAL append（parent） | 2,020.69 | 1,685.84–2,563.99 | 32.552% | parent 有慢輪變異 | 主要成本 |
| WAL sync | 955.587 | 667.615–5,115.53 | 15.818% | r2 sync tail 明顯 | 方向性長尾 |
| `StateMachine::apply` | 1,319.8 | 1,309.65–1,353.68 | 22.506% | absolute 穩定 | 主要成本 |
| publisher notify | 0.576935 | 0.546489–0.648007 | 0.009725% | 穩定 | 非瓶頸 |
| post-apply | 544.821 | 535.101–551.494 | 9.397% | absolute 穩定 | 接近門檻但非主因 |
| completion enqueue | 402.664 | 397.250–413.208 | 6.844% | absolute 穩定 | 非瓶頸 |
| writer unattributed | 151.089 | 146.803–157.640 | 2.507% | 穩定 | 剩餘成本 |

`writer_service_ns/command` median 為 5,856.88 ns（5,256.03–10,525.4），
`writer_cycle_ns/command` median 為 6,536.63 ns（5,981.85–11,160.1）。
`wal_sync_ns/command` 的逐輪原始對照如下；profile-off 按設計不取得 phase clocks，
所以不可用 profile-off writer 直接配對 sync phase。

| Run | RPS | `wal_sync_ns/command` | `wal_append_ns/command` | `StateMachine::apply ns/command` |
| --- | ---: | ---: | ---: | ---: |
| g4096-on-r1 | 155,803 | 1,320.79 | 1,685.84 | 1,318.14 |
| g4096-on-r2 | 98,940.2 | 5,115.53 | 2,563.99 | 1,309.65 |
| g4096-on-r3 | 160,100 | 705.488 | 2,020.69 | 1,353.68 |
| g4096-on-r4 | 162,900 | 955.587 | 2,235.81 | 1,319.80 |
| g4096-on-r5 | 162,095 | 667.615 | 1,710.92 | 1,342.12 |

### 5.1 WAL nested phases

| Nested phase | ns/command median | min–max | Parent share median | 判斷 |
| --- | ---: | ---: | ---: | --- |
| payload encode | 446.085 | 438.088–457.132 | prepare 的 42.059% | 穩定主子成本 |
| CRC | 248.980 | 246.110–251.724 | prepare 的 23.475% | 穩定主子成本 |
| frame assembly | 193.309 | 191.695–205.062 | prepare 的 18.161% | 穩定 |
| prepare remainder | 176.972 | 172.384–178.397 | prepare 的 16.417% | 穩定 |
| **WAL prepare 合計** | **1,060.62** | **1,052.76–1,091.76** | writer service 的約 **18.109%** | 可評估 bounded parallel |
| chunk copy | 26.7833 | 24.8533–29.1939 | plan/copy 的 6.257% | 非主要成本 |
| plan/copy remainder | 399.557 | 52.5314–957.129 | plan/copy 的 93.343% | run-to-run 變異 |
| **WAL plan/copy 合計** | **428.054** | **81.7253–983.498** | append 的一部分 | 不先平行化 |
| publish | 417.497 | 408.147–425.446 | append 的 20.661% | 穩定 |
| write | 43.6826 | 40.2655–75.0450 | append 的 2.162% | 非主要成本 |

payload encode + CRC 為 prepare 的 65.534%。以 `P=1,060.62/5,856.88`
估算，理想化 Amdahl 上限約為：2 workers +9.956%、3 workers +13.730%、
4 workers +15.716%。2-worker 單獨不足 10%，4-worker 才通過設計中的 10% 方向性
gate；這不是已實現 speedup，仍需 prototype 實測 ordering、error selection、
WAL bytes、fsync boundary 與 latency。

獨立 direct WAL profile-on 的 group-level nested share 亦支持相同方向，但因其
profile bias 超過 5% 只能作 cross-check：prepare median 41.129%
（29.9203–41.6636%）、plan/copy 12.4701%（9.09610–12.5751%）、publish
14.6873%（10.8283–14.9218%）、write 1.59737%（1.21649–1.62829%）、sync
29.2055%（28.5813–48.3901%）。這些 group share 不與 writer 的 ns/command
直接相加。

## 6. Completion worker

Completion 指標只描述 sampled completion items 的 worker service 與 queue residence；
不加入 writer service，也不把 `publisher_notify` 解讀為 Publisher worker service time。

| Group | Service RPS median | Queue p50 median (us) | Queue p99 median (us) | Worst p99.9 (us) | Max depth median | 是否持續累積 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,096 | 2.70894M/s（2.466–2.799M/s） | 2,701.29 | 5,969.36 | 8,529.93 | 4,084 | 未證實 |
| 256 | 3.08202M/s（3.04476–3.19104M/s） | 132.158 | 717.595 | 2,965.77 | 256 | 未證實 |

Writer g=4,096 arrival 約 160K/s，Completion median service 約 16.9 倍；g=256
約 74K/s，service 約 41.8 倍。`max_depth` 接近 group size 只表示 burst/backlog
曾出現，沒有 queue depth 時間序列證明持續增加；因此不選 Completion batching。
Publisher replay、sink ACK 與 cursor persistence 不屬於本輪範圍。

## 7. Sync/storage 長尾與外部觀測

Direct WAL 的慢輪次與 sync tail 同輪出現：

- profile-off r2：237,735 RPS，`sync_p99_us=159,069.741`、
  `sync_max_us=275,900.796`；其餘 off 輪次 sync p99 約 9.4–10.6 ms。
- profile-on r1：370,510 RPS，`sync_p99_us=61,223.330`；
  profile-on r5：303,710 RPS，`sync_p99_us=184,087.323`、
  `sync_max_us=526,921.756`。
- writer profile-on g=4,096 r2 的 sync phase 為 5,115.53 ns/command，
  同輪 RPS 98,940.2；但 profile-off writer 沒有 clocks，不能由此宣稱
  所有 Engine writer 慢輪次都由 sync 造成。

因此 direct WAL sync tail 是 WAL control outlier 的已證實直接關聯；其 p50 sync
仍約 2.6 ms，變異主要在 tail。這不等同於已證明要立即改 durability policy，
也不改變本輪唯一 production optimization 的選擇。

`/usr/bin/time -v` 的一個 profile-off observation（不納入 formal 統計）為：
RPS 161,136、user 24.62 s、system 6.81 s、wall 25.95 s、CPU 121%、
最大 RSS 1,552,048 kB、voluntary context switches 282,938、involuntary
context switches 8,999、exit 0。這顯示 affinity 範圍內有 CPU work 與可用平行度
的方向性證據，但不能當作 CPU profiler 或 cache/IPC 證據；`perf` 未執行，不能
推測 hardware counters。

## 8. 35 輪逐輪有效性

所有列出的輪次 exit status 均為 0、measured phase 均至少 15 秒，均納入統計。
`correctness` 表示 writer；`replay` 表示 direct WAL；Engine control 以
commands/trades/active state expected 驗證。低吞吐與長尾輪次保留。

| Run | Exit | Elapsed (ms) | RPS | Correctness/replay | Profile check | 納入 |
| --- | ---: | ---: | ---: | --- | --- | --- |
| g4096-off-r1 | 0 | 20,847.9 | 161,167 | correctness=true | — | yes |
| g4096-off-r2 | 0 | 20,966.4 | 160,256 | correctness=true | — | yes |
| g4096-off-r3 | 0 | 21,055.0 | 159,582 | correctness=true | — | yes |
| g4096-off-r4 | 0 | 20,511.1 | 163,814 | correctness=true | — | yes |
| g4096-off-r5 | 0 | 20,661.9 | 162,618 | correctness=true | — | yes |
| g4096-on-r1 | 0 | 21,565.8 | 155,803 | correctness=true | 419,305 = 419,305; groups=103 | yes |
| g4096-on-r2 | 0 | 33,959.9 | 98,940.2 | correctness=true | 420,582 = 420,582; groups=103 | yes |
| g4096-on-r3 | 0 | 20,986.8 | 160,100 | correctness=true | 421,888 = 421,888; groups=103 | yes |
| g4096-on-r4 | 0 | 20,626.2 | 162,900 | correctness=true | 421,888 = 421,888; groups=103 | yes |
| g4096-on-r5 | 0 | 20,728.6 | 162,095 | correctness=true | 421,888 = 421,888; groups=103 | yes |
| g256-off-r1 | 0 | 21,333.5 | 74,999.4 | correctness=true | — | yes |
| g256-off-r2 | 0 | 21,323.8 | 75,033.5 | correctness=true | — | yes |
| g256-off-r3 | 0 | 20,789.6 | 76,961.4 | correctness=true | — | yes |
| g256-off-r4 | 0 | 20,798.4 | 76,928.9 | correctness=true | — | yes |
| g256-off-r5 | 0 | 20,633.9 | 77,542.4 | correctness=true | — | yes |
| g256-on-r1 | 0 | 21,748.7 | 73,567.5 | correctness=true | 200,192 = 200,192; groups=782 | yes |
| g256-on-r2 | 0 | 27,638.3 | 57,890.6 | correctness=true | 200,192 = 200,192; groups=782 | yes |
| g256-on-r3 | 0 | 21,609.3 | 74,042.3 | correctness=true | 200,192 = 200,192; groups=782 | yes |
| g256-on-r4 | 0 | 21,676.8 | 73,811.7 | correctness=true | 200,192 = 200,192; groups=782 | yes |
| g256-on-r5 | 0 | 21,162.1 | 75,606.9 | correctness=true | 200,192 = 200,192; groups=782 | yes |
| engine-control-r1 | 0 | 34,147.0 | 98,398 | commands/trades/active expected | — | yes |
| engine-control-r2 | 0 | 20,541.2 | 163,574 | commands/trades/active expected | — | yes |
| engine-control-r3 | 0 | 20,473.8 | 164,112 | commands/trades/active expected | — | yes |
| engine-control-r4 | 0 | 20,361.2 | 165,020 | commands/trades/active expected | — | yes |
| engine-control-r5 | 0 | 20,094.4 | 167,211 | commands/trades/active expected | — | yes |
| wal-off-r1 | 0 | 19,606.9 | 439,330 | replay=true | — | yes |
| wal-off-r2 | 0 | 36,233.1 | 237,735 | replay=true | — | yes |
| wal-off-r3 | 0 | 19,287.1 | 446,615 | replay=true | — | yes |
| wal-off-r4 | 0 | 19,262.2 | 447,191 | replay=true | — | yes |
| wal-off-r5 | 0 | 19,285.7 | 446,647 | replay=true | — | yes |
| wal-on-r1 | 0 | 23,248.8 | 370,510 | replay=true | groups=2,103 | yes |
| wal-on-r2 | 0 | 21,103.9 | 408,166 | replay=true | groups=2,103 | yes |
| wal-on-r3 | 0 | 20,801.3 | 414,104 | replay=true | groups=2,103 | yes |
| wal-on-r4 | 0 | 20,640.1 | 417,338 | replay=true | groups=2,103 | yes |
| wal-on-r5 | 0 | 28,362.2 | 303,710 | replay=true | groups=2,103 | yes |

Profile-on writer 的 profiled commands 與 Completion profiled commands 每輪完全
相等；沒有 publisher failure、storage pressure、phase timeout、callback error
或 sanitizer error。

## 9. 舊 artifact 對照

以下只比較舊報告與本輪新 artifact 各自的 median／range，沒有合併 samples。

| Metric | 舊 artifact（retest report） | 本輪 post-cleanup | 解讀 |
| --- | ---: | ---: | --- |
| g=4,096 writer off RPS | 161,853（96,388–164,475） | 161,167（159,582–163,814） | median -0.424%；新輪 range 未出現同級 off 慢輪 |
| g=4,096 writer on RPS | 160,904（158,193–163,567） | 160,100（98,940.2–162,900） | median -0.500%；新 r2 是有效 sync tail outlier |
| g=4,096 writer bias | 0.586% | 0.662046% | 都低於 5%，collector cleanup 未改變 attribution 可用性 |
| writer service ns/command | 5,870.0（5,371.5–6,054.6） | 5,856.88（5,256.03–10,525.4） | median -0.224%；新範圍受 r2 tail 影響 |
| WAL prepare ns/command | 1,076.5（1,069.8–1,096.3） | 1,060.62（1,052.76–1,091.76） | median -1.47%；仍是穩定可解釋子成本 |
| `StateMachine::apply` ns/command | 1,353.8（1,328.7–1,388.7） | 1,319.8（1,309.65–1,353.68） | median -2.51%；仍約占 writer service 22.5% |
| Completion service RPS | 2.619M/s（2.568–2.914M/s） | 2.709M/s（2.466–2.799M/s） | 同一量級且遠高於 writer arrival |
| authoritative Engine RPS | 167,140（164,744–167,504） | 164,112（98,398–167,211） | median -1.81%；新 r1 為有效慢輪 |
| direct WAL off RPS | 436,994（236,187–446,443） | 446,615（237,735–447,191） | median +2.20%；同樣保留 storage tail |
| direct WAL on RPS | 414,709（409,756–423,773） | 408,166（303,710–417,338） | median -1.58%；bias 由5.10%升至8.61%，仍只作方向性 |

Writer off/on 的中位數與主要 phase 均在同一量級；改變的是有效 run-to-run
tail，而不是 collector cleanup 導致的系統性 writer 根因改變。Direct WAL bias
超過 5% 的結果只降低 nested phase 的證據等級，不能推翻 g=4,096 writer bias
已通過門檻的結論。

## 10. 結論、限制與唯一下一案

### 已證實

1. N=8 calibration 與正式 g=4,096 writer bias 均低於 5%；移除 collector progress
   counters 沒有重新引入主要 instrumentation bias。
2. Writer 的穩定主要成本仍是 WAL append parent 與 `StateMachine::apply`；
   publisher notify 與 completion enqueue 不是同步吞吐瓶頸。
3. WAL prepare 內 payload encode 與 CRC 合計約 65.5%，prepare 約占 writer
   service 18.1%；這是足以進行下一案 bounded parallel 評估的穩定 CPU 成本。
4. Completion service rate 顯著高於 writer arrival，沒有證據支持 Completion
   worker batching 或 queue redesign。
5. Direct WAL 慢輪次與 sync tail 同輪出現；這個因果只對 direct WAL control
   直接成立，Engine writer off 因沒有 phase clocks 仍只能作方向性比較。

### 方向性證據

- 4-worker idealized Amdahl uplift 約 15.716%，但 2-worker 約 9.956%；這是上限，
  不是已達成的吞吐提升。
- `/usr/bin/time` 的 121% CPU、prepare 絕對時間穩定以及 6-CPU affinity 是有
  CPU work／可分配核心的方向性證據；沒有 `perf` counters，不能推測 IPC、cache
  miss 或 allocator contention。
- Direct WAL nested phase share 與 writer nested phase 方向一致，但 direct WAL
  bias 8.608981% 使其不適合當主要 attribution。

### 尚未證實

- bounded parallel prepare 在真實 WAL ordering、partial write、rotation、fsync
  boundary、replay、Completion callback 與 p50/p99/p99.9 latency 下的實際 speedup；
- sync tail 的根因是 block-device queue、kernel writeback、fsync scheduling 或
  其他外部 I/O，以及更大 group／group-commit interval 在指定 latency SLO 下的最佳點；
- `StateMachine::apply` 內 book lookup、matching、event 與 incremental validation
  的細分比例是否在 WAL 優化後成為下一個 ceiling。

### 唯一選定的 production optimization

選 **bounded parallel WAL prepare**，只限於 prepare 內可驗證為 CPU-bound 的
payload encode、CRC 與 frame assembly；不平行化 shared WAL publish、write、fsync
或 `StateMachine` ownership。先以固定上限的 W=2/W=4 prototype gate 驗證：

- W=2 單獨的理想上限不足 10%，W=4 的理想上限約 15.716%；不得把理論值當成
  完成結果；
- 保持單一 deterministic command ordering、EngineSeq、WAL bytes、CRC、segment
  boundary、partial-write 與 fail-stop semantics；
- 比較 off/on 的 p50、p99、p99.9、max、CPU utilization、sync tail 與 Completion
  residence；若 latency SLO、CPU contention 或 correctness 不通過則回退；
- 小 group 保留 single-thread fallback，不引入通用 executor、Publisher 優化、
  queue redesign 或 public API 變更。

這是本報告唯一選出的 production optimization。Sync/group-commit sweep 是必要時
的獨立 diagnostic，不與 bounded prepare 在同一變更中同時展開，避免無法歸因。

### 限制與 artifact 保留

- 結果只代表本次 AMD Ryzen 7 3700X、`powersave`、ext4 `/dev/sdb2`、kernel 與
  GCC artifact，不能外推到其他 SSD、RAID、cloud block device 或 performance governor。
- CMake/Ninja/Conan 不可用，使用操作檔定義的直接 GCC 等價路徑；具備標準工具鏈的
  環境仍應另外跑 CMake/CTest。
- `perf_event_paranoid` 限制硬體 counters；本輪沒有降低 kernel security setting。
- Profile-off writer 沒有 phase clocks，慢輪次不能與 sync phase 做同輪完整因果配對。
- Completion 只提供本次 run length 的 residence/depth 摘要，沒有長時間 queue
  time-series，因此不宣稱 backlog 持續成長。
- Snapshot 在 ceiling workload 停用；rotation 只反映本輪偶發 tail，不能替代 soak test。

完整 raw logs 與 WAL data 保留於：

`/home/neojhou/engine-writer-post-cleanup-retest-Q7tQ3vBX`

保留 policy：不自動刪除任何 raw log 或 data directory；新舊 artifact 不混合統計。
