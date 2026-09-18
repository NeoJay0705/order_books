# WAL Durable Path 根因分析設計審查與必要修改

## 1. 文件目的

本文件承接 `engine-writer-throughput-optimization-benchmark-report.md`。該報告已確認：

- `StateMachine` 在 0 至 100K active orders 下約為 0.999M–1.128M commands/s；
- metrics ownership split 後 writer 約為 1.697M commands/s；
- 完整 durable writer 在 sustained backlog 下約於 166K commands/s 平台化；
- group size 由 4,096 增至 8,192 只提升約 0.10%；
- saturated workload 將 group delay 由 200 us 增至 5 ms，沒有提高 throughput。

因此下一階段不再猜測 batch 參數，而是以可重現量測把 durable path 分解，確認限制主要來自
WAL userspace preparation、data write、`fsync`、segment rotation，或 WAL 之外的 writer
integration。

本文件是下一階段的實作與量測契約。交付物是根因證據及後續優化選擇，不是直接改寫 WAL
演算法，也不承諾本階段達成 1,000,000 durable commands/s。

## 2. Review 結論

目前資料足以把瓶頸範圍縮小到 durable writer，但不足以宣稱 `fsync` 是唯一根因：

- `engine_durable_single_instrument` 只提供整體 durable completion throughput／latency；
- `wal_write_ceiling` 已分開量測 append call 與 `Wal::sync()`，但 append sample 仍包含 benchmark
  fixture 建立，而且看不到 `Wal::append_batch()` 內部各階段；
- 19.4 MiB/s 即出現平台是重要線索，但不能單憑低 bandwidth 判定是 sync latency、CPU、copy、
  filesystem 或 device；
- `strace` 可分辨 syscall shape，卻無法分解 encode／CRC／buffer copy；目前環境亦不能把 `perf`
  權限視為前提。

為得到足以決定下一項 production 優化的證據，必要修改只有：

1. 讓 `Wal::append_batch()` 提供 opt-in、internal-only 的分階段 profile；一般 production call
   不取 clock、不配置 profile samples；
2. 擴充既有 `wal_write_ceiling`，把 fixture construction 與真正 WAL call 分開，並輸出 profile
   counters／latency；
3. 以既有 WAL ceiling、Engine durable workload及外部 syscall／device 觀測完成同環境對照。

不建立假的 memory-only WAL、第二套 encoder 或 mock filesystem，因為這些路徑可能與 production
實作漂移。Encode／CRC 與 memory planning 的 ceiling 由同一次正式 append 中的 exclusive phase
time 推算。

此範圍直接回答目前尚未確認的瓶頸，沒有包含 async WAL、`io_uring`、direct I/O、WAL format、
durability policy 或 production default 變更，符合「只做必要修改」的要求。

## 3. 需求理解與合理假設

### 3.1 必須回答的問題

1. 使用相同 group size 時，直接 WAL durable ceiling 與完整 Engine durable writer 相差多少？
2. `Wal::append_batch()` 的時間分別花在：
   - encode 與 CRC framing；
   - metadata preflight、segment planning 與 contiguous chunk copy；
   - segment rotation；
   - data `write`；
   - in-memory record index／position publication；
3. `Wal::sync()` 在不同 group size 下占 durable group wall time多少？
4. throughput 平台是否同時伴隨 block-device bandwidth、await、queue depth 或 utilization 飽和？
5. 下一階段應優化 userspace preparation、write path、sync policy／storage，還是 Engine integration？

### 3.2 不變條件

- 目標情境仍是單一 shard、單一 instrument、單一 WAL writer。
- 一筆 command 只有在所屬 group append 成功且 `fsync` 成功後，才算 durable completion。
- RPO=0、EngineSeq ordering、WAL bytes、checksum、recovery、Publisher visibility 與 callback
  boundary 均不變。
- profile 使用 `std::chrono::steady_clock`，只在明確啟用的 benchmark run 執行。
- 正式比較使用相同 CPU affinity、compiler／flags、filesystem、block device 與 source state。
- throughput 使用五輪 median 與 range；phase share 使用五輪 median，不以單次最佳值下結論。
- sanitizer 只驗證 correctness，不產生效能結論。

### 3.3 參考目標與量測語意

1,000,000 commands/s 是長期方向，不是本階段 CI gate。報告必須同時呈現：

```text
absolute ceiling
target attainment percent
相對於目前約 166K/s Engine durable ceiling 的差異
在暫定 p99 <= 20 ms、p99.9 <= 50 ms 下可維持的 profile
```

Sustained-backlog 測試用來找 ceiling，不代表低負載端到端 latency。若 queue 已累積大量 backlog，
completion latency 超過暫定 SLO 不能直接歸因於單次 `fsync`。

## 4. 範圍與非目標

### 4.1 In scope

- 正式 `Wal::append_batch()` 的 opt-in exclusive phase timing 與必要 counters；
- `wal_write_ceiling` 的 phase-profile CLI、sample aggregation 與輸出；
- `sync=none`／`per_group`、group size 與 Engine durable 的 controlled comparison；
- `strace`、`iostat`、`/usr/bin/time` 及可用時的 `perf` 輔助歸因；
- profile correctness、CLI、persistence、reopen／replay 與 benchmark smoke tests；
- 根因分析報告。

### 4.2 Out of scope

- async／background WAL writer、multiple in-flight sync；
- `io_uring`、AIO、direct I/O、mmap、`writev`、`pwrite`；
- `fsync` 改成 `fdatasync`，或降低 sync 頻率／durability；
- WAL record／segment format、codec schema、checksum coverage；
- CRC implementation、allocator、buffer pool或record index的production優化；
- group-commit production defaults；
- ingress、Completion、Publisher 或 queue redesign；
- 多 shard／多 instrument scaling；
- 修改 kernel、mount options、I/O scheduler 或清除 page cache；
- shared CI 的固定 RPS threshold；
- 通用 tracing framework、metrics exporter 或第三方 profiling dependency。

以上項目必須由本階段證據支持後另案設計，不能在量測變更中順帶實作。

## 5. 架構與資料流

正式 production 路徑保持不變：

```text
ShardRuntime writer
  -> Wal::append_batch(commands)
       -> encode + CRC framing
       -> metadata preflight + segment plan + contiguous chunk copy
       -> optional rotation
       -> FileOps::write_all
       -> publish in-memory WAL index / positions
  -> Wal::sync()
       -> FileOps::sync_file (fsync)
  -> StateMachine::apply
  -> Completion queue
```

診斷路徑只由 benchmark 選用：

```text
wal_write_ceiling --wal-phase-profile=on
  -> build command fixture                    [fixture_build]
  -> Wal::append_batch_profiled(commands)
       -> same production implementation      [prepare / plan_copy / rotation / write / publish]
  -> optional Wal::sync()                     [sync]
  -> aggregate samples and counters
  -> reopen + replay correctness validation
  -> print result
```

`append_batch()` 與 `append_batch_profiled()` 必須共用同一個 implementation。Profile 介面不得
複製 framing、segment planning、write 或 metadata publication 邏輯。

外部工具只包覆相同 executable：

```text
baseline run       -> authoritative throughput and latency
strace run         -> syscall count / time shape; not authoritative throughput
iostat + time run  -> device and process resource evidence
optional perf run  -> CPU call-stack evidence; not required when permission unavailable
```

## 6. WAL Phase Profile 介面

### 6.1 Internal data model

在 internal `src/persistence/wal.hpp` 增加不安裝為 public API 的資料結構，名稱可依既有風格微調：

```cpp
struct WalAppendProfile {
  std::uint64_t lock_wait_ns{};
  std::uint64_t prepare_ns{};
  std::uint64_t plan_copy_ns{};
  std::uint64_t rotation_ns{};
  std::uint64_t write_ns{};
  std::uint64_t publish_ns{};
  std::uint64_t frame_bytes{};
  std::uint64_t data_write_calls{};
  std::uint64_t rotations{};
};
```

新增 internal diagnostic overload：

```cpp
Result<WalPosition> append_batch_profiled(
    std::span<const domain::CommittedCommand> commands,
    WalAppendProfile& profile);
```

契約：

- 進入方法時先將 `profile` 清零，避免 caller 誤用上一組資料；
- 只有整個 append 成功時，benchmark 才可聚合該 profile；
- normal `append()`／`append_batch()` 不呼叫 clock，也不建立 profile object；
- profiled 與 normal path 共用 framing、planning、I/O及publication implementation；
- profile 不寫入 WAL、Snapshot、metrics registry或public `CommandResult`；
- profile durations 為 exclusive wall time；不得讓同一段工作同時計入兩個 phase；
- 不保證 duration 非零，測試不得依賴 clock resolution；
- counters 使用 overflow-safe 累加，任何無法表示的結果視為 benchmark/profile failure。

這個 overload 位於既有 internal storage header，而不是 `include/order_books/`，避免形成 public
library contract。

### 6.2 Phase 邊界

phase 定義固定如下，避免設計與報告對同一欄位有不同解讀：

| Phase | 包含 | 不包含 |
| --- | --- | --- |
| `lock_wait` | 等待 `Wal` mutex | 持鎖後工作 |
| `prepare` | `encode_committed_command`、record version、CRC、frame 建立 | segment plan、data write |
| `plan_copy` | overflow／capacity preflight、cached metadata準備、segment chunk規劃、contiguous buffer copy | filesystem I/O |
| `rotation` | rotation-triggered old-segment sync、close、新 header write／sync、directory sync | 正常 batch-data write、group sync |
| `write` | `FileOps::write_all()` 的 batch-data write | header write、group `fsync` |
| `publish` | 更新 active bytes、positions、dirty state及`records_` | state machine apply |
| `sync` | benchmark 包覆 `Wal::sync()` 的既有 sample | rotation sync、append work |

`rotation` 可能包含 sync，是現有 segment durability 規則的一部分，必須獨立回報，不能算入一般
group `sync`。若 measured phase 沒有 rotation，欄位為零且 `rotations=0`，不得輸出虛構 percentile。

### 6.3 實作約束

- 使用一個 shared append implementation，可用 nullable collector 或兩個薄 wrapper；不得保留兩套
  algorithm。
- normal path 的 clock 呼叫數必須維持為零；只有 profiled overload 可呼叫 `steady_clock::now()`。
- 每個 phase 以 group／chunk 為單位計時，不得在每筆 command 周圍取 clock。
- 現有 error、partial-write、rotation、fail-stop 與 recovery semantics 不變。
- Profile 不能改變 buffer reserve、chunk boundaries、write count 或 sync count。
- `data_write_calls` 必須等於實際 batch-data `FileOps::write_all()` 次數；header write 不計入。
- `frame_bytes` 是本次 batch 所有完整 record frames 的總和，不含 segment header。

## 7. Benchmark 修改

### 7.1 CLI

擴充既有 workload：

```text
--workload=wal_write_ceiling
--wal-phase-profile=off|on
```

規則：

- 預設 `off`，維持既有 benchmark 成本與輸出用途；
- parser 必須另記錄 option 是否由使用者明確指定；只有 `wal_write_ceiling` 接受明確指定的
  profile option，其他 workload 指定時以 exit code 2 拒絕；
- 空值與未知值必須在建立 data directory 前拒絕；
- 不新增第二個 root-cause workload，避免重複 WAL runner。

### 7.2 保留與新增的 timing

現有 `append_group_*` 為了報告相容性保留原語意：從 fixture 建立開始，到
`Wal::append_batch*()` 返回為止。Profile 開啟時另加：

```text
fixture_build_group_*
wal_append_call_group_*
wal_lock_wait_group_*
wal_prepare_group_*
wal_plan_copy_group_*
wal_rotation_group_*
wal_write_group_*
wal_publish_group_*
sync_*
group_total_*
```

其中 `fixture_build` 不是 production WAL 成本，必須獨立顯示；authoritative
`commands_per_second` 仍使用整個 measured phase wall time，保持與既有結果可比較。

每個 phase 至少輸出 total、p50、p99、p99.9、max；沒有 sample 的 percentile 輸出 `na`。另輸出：

```text
phase_profile=on|off
profiled_groups
profiled_commands
profiled_frame_bytes
profiled_header_bytes
profiled_data_write_calls
profiled_rotations
lock_wait_share_percent
prepare_share_percent
plan_copy_share_percent
rotation_share_percent
write_share_percent
publish_share_percent
sync_share_percent
unattributed_share_percent
```

share denominator 是 profiled groups 的 `wal_append_call + sync` total；fixture time 不納入 WAL
share。`sync=none` 時 `sync_share_percent=na`。因 clock 與 wrapper 會有少量成本，
`unattributed` 定義為 denominator 減去 exclusive phases 後的非負剩餘，不把微小差額偷偷分配給
任一 phase。

Profile 關閉時只需輸出 `phase_profile=off`，不輸出 phase 欄位；既有欄位與語意不得改變。

### 7.3 Correctness guard

成功 summary 前必須同時驗證：

- profiled group／command 數與 measured request 相同；
- phase counters 加總未 overflow；
- `frame_bytes` 與 measured WAL byte delta 的差異只能來自 segment header；
- `data_write_calls >= measured groups`；未 rotation 且每組不跨 segment時應等於 groups；
- profile rotations 與 measured segment-count delta 一致；
- normal 的 completion count、final sync、reopen、replay、EngineSeq 與 record content 驗證通過。

任一 mismatch 必須輸出 workload、phase、machine-readable error code並以 non-zero 結束；不得輸出
看似成功的 RPS。

## 8. 正式根因量測方法

### 8.1 共通環境

報告記錄：source state、compiler／flags、CPU model與affinity、governor、kernel、filesystem與
mount options、block device、WAL path、segment size、benchmark command及原始 logs。

每個 authoritative case：

- Release build；
- 相同 CPU set 與 target WAL device；
- fresh empty data directory；
- warmup 後開始 measured phase；
- 五輪，measured phase至少 15 秒；
- 回報 median、range、p50／p99／p99.9／max及correctness；
- 不清 page cache、不修改 host configuration。

### 8.2 Instrumentation bias check

先以 `group=4096, sync=none` 對 `phase-profile=off／on` 各跑五輪。若 profile-on median 比
profile-off 低超過 5%，或五輪 range 顯示一致性退化：

- ceiling 只採 profile-off 結果；
- phase share 只能標示為診斷估計；
- 報告必須揭露差異；
- 不可用 profile-on RPS 宣稱 production ceiling。

5% 是本階段判斷 instrumentation 是否干擾量測的門檻，不是產品效能 SLO。

### 8.3 WAL matrix

使用既有 `wal_write_ceiling`：

```text
group size:     256, 1024, 4096, 8192
sync mode:      none, per_group
phase profile:  off for ceiling, on for phase attribution
```

若相鄰 group 的 throughput uplift 已低於 5%，後續更大 group 可以停止，並在報告記錄停止理由。
每個 sync=per_group case 的 group 必須完整填滿；這與 Engine 的 actual commands/group 分開回報。

### 8.4 Engine comparison

使用既有 `engine_durable_single_instrument` 重跑與 WAL plateau 相同的 group，至少包含：

```text
group=4096
delay=1 ms
producer_lanes=8192
```

不在 Engine hot path 加 phase clock。以相同裝置、record shape 與 group 設定比較：

- direct WAL per-group throughput；
- Engine accepted durable throughput；
- Engine actual commands/group；
- WAL MiB/s；
- durable completion p99／p99.9。

若 Engine group 未填滿，不得把它與 full WAL group 當作完全相同工作量；報告必須按實際
commands/group解釋差異。

### 8.5 外部觀測

對代表性的 `group=4096` 各執行獨立診斷 run：

```bash
strace -f -c -e trace=write,fsync,fdatasync,openat,close <benchmark command>
/usr/bin/time -v <benchmark command>
iostat -xz 1
```

若權限允許，再執行 `perf stat`／`perf record -g`。規則：

- `strace`／`perf record` 會擾動 wall time，只用於 syscall／call-stack attribution；
- `iostat` 必須確認觀測的是 WAL 實際所在 block device；
- 報告至少列出 write／fsync calls per group、user／system CPU、context switches、device
  throughput、await、queue depth及utilization；
- 工具不存在或權限不足時記錄限制，不修改系統安全設定，也不偽造缺少的數據。

## 9. 根因判定規則

結論必須由五輪受控對照與至少一項交叉證據支持，不用 source inspection 猜測百分比。

| 證據 | 可支持的結論 | 後續另案方向 |
| --- | --- | --- |
| direct WAL 與 Engine median 差距小於 20%，且 range 大致重疊 | WAL durable path 是 writer 主限制 | 依 phase share 選 storage 子階段 |
| direct WAL 明顯高於 Engine 至少 20%，且 range 不重疊 | WAL 之外仍有 writer integration 成本 | 另做 queue／admission／apply 分段 |
| sync 占 append+sync 過半，且 fsync／device await 證據一致 | sync／storage latency 是主要候選 | storage與durability方案；不得自行降低RPO |
| prepare 持續占過半，CPU stack亦集中於codec／CRC | encode／CRC 是主要候選 | codec allocation或CRC實作優化 |
| plan_copy 持續占過半 | buffer allocation／copy或index preflight是主要候選 | buffer reuse、copy或index設計 |
| write 持續占過半，且device指標同步飽和 | data write／device bandwidth是主要候選 | write path或storage device |
| rotation 只造成少數tail spikes | rotation是tail因素，不是steady-state ceiling | 另評估rotation latency，不改主路徑 |
| 沒有單一phase過半 | 混合瓶頸 | 依累積占比選最小的下一個實驗 |

20% 與「過半」是本次根因分類門檻，不是 CI gate。若 run range 重疊或 external evidence
矛盾，結論必須標示為未確認，不得直接實作高風險架構變更。

## 10. 錯誤處理

- Profiled append 沿用現有 `Result<T>`／`ErrorCode`；profile 不是新的錯誤域。
- append／rotation／write／sync 任一步失敗時，WAL 與 Engine 沿用既有 fail-stop及restart
  recovery，不因 profiling 改變。
- Failed group 的 partial durations可以出現在錯誤診斷，但不得納入成功 percentile或share。
- Clock、counter與sample容量的arithmetic必須檢查overflow；失敗時benchmark non-zero結束。
- CLI error在任何filesystem mutation前回報。
- 外部工具失敗只使該外部觀測無效；baseline correctness／throughput仍可獨立成立。

## 11. 測試策略

### 11.1 Persistence tests

在既有 `tests/integration/persistence_test.cpp` 增加最小案例：

1. Profiled single-segment batch：position、durable boundary、replay bytes／commands與normal path
   一致；commands、frame bytes及data write count正確。
2. Profiled rotation batch：rotation count、segment count及replay ordering正確；rotation sync不被
   算成group sync。
3. Empty profiled batch：回傳既有錯誤、profile清零、WAL不變。

測試只驗證 phase counters／邊界，不斷言 duration 大於零或固定比例。

### 11.2 Benchmark tests

- `--wal-phase-profile=on` 的小型 smoke完成並回報`replay_verified=true`；
- `off` 保留既有輸出，且不出現 profile-only欄位；
- 未知值、空值及在不支援workload使用時回傳exit code 2；
- profile groups／commands／bytes／writes／rotations mismatch會拒絕成功summary；
- `sync=none` 的sync percentile與share為`na`；`per_group` 的sync sample數等於measured groups。

### 11.3 全專案驗證

- GCC與Clang warnings-as-errors build；
- 現有GoogleTest／CTest全部通過；
- ASan／UBSan通過；
- replay、partial-tail、corruption、rotation與Engine durable integration tests不退化；
- benchmark正式效能只在Release、無sanitizer build執行。

不新增production timing test hook、mock filesystem或sleep-based test。

## 12. 必要檔案修改

### `src/persistence/wal.hpp`

- 增加internal `WalAppendProfile`與profiled append overload；
- 保持public installed headers不變。

### `src/persistence/wal.cpp`

- 讓normal／profiled wrapper共用同一append implementation；
- 只在profiled path記錄exclusive phase與counters；
- 不改WAL bytes、segment plan、write、sync、error或recovery語意。

### `benchmarks/order_book_benchmark.cpp`

- 增加`--wal-phase-profile=off|on`嚴格解析；
- profile-on時分開fixture與Wal call timing，聚合phase samples／shares；
- 保留既有throughput與append欄位語意；
- 成功summary前完成profile counters及reopen／replay驗證。

### `benchmarks/check_invalid_cli.cmake`／`benchmarks/CMakeLists.txt`

- 增加profile CLI invalid cases與一個小型correctness smoke；
- 不加入performance threshold。

### `tests/integration/persistence_test.cpp`

- 增加profiled same-segment、rotation與empty-batch契約測試；
- 不重複既有codec、corruption或Engine business tests。

不需要修改`include/order_books/*`、domain model、runtime config、Publisher、Completion、WAL format、
Snapshot、README或第三方依賴。正式量測完成後另建benchmark report，不把機器特定數字寫入主設計。

預估production/internal與benchmark程式修改約250–400行，測試約70–120行；實際以共用既有
percentile／CLI helper及避免重複runner為原則。文件與報告行數不計入程式修改估算。

## 13. 驗收條件

實作完成且尚未進行正式壓測時，至少必須：

- normal path沒有新增clock取樣，production語意與defaults不變；
- profile-on／off皆可編譯並通過correctness smoke；
- profiled append與normal append共用實作且replay結果一致；
- CLI與counter mismatch能fail closed；
- staged changes未被任何命令改動。

正式量測完成後，報告才可宣稱本階段完成。報告至少必須：

- 通過instrumentation bias check；
- 提供WAL group／sync矩陣與Engine對照；
- 提供phase share、syscall shape及可取得的device evidence；
- 按第9節規則標示confirmed、supported或unconfirmed root cause；
- 只提出由證據支持的下一項production修改，不在本階段直接實作該修改。

## 14. 已知限制與後續擴充

- `steady_clock` phase timing能分解wall time，但不能單獨說明CPU stall、cache miss或kernel內部
  fsync工作；必須與外部證據交叉判讀。
- `prepare`合併encode與CRC；只有它被確認為主要成本後，才值得增加更細的codec／CRC控制組。
- `plan_copy`合併allocation、index preflight與memcpy；只有該phase被確認後才再細分，避免現在
  為所有可能性加入永久instrumentation。
- Profile只量direct WAL。Engine差距若顯著，需另案設計writer integration profiling。
- 本機ext4數據不能代表部署裝置；正式硬體必須重跑同一矩陣。
- 單writer RPO=0架構本身可能無法達到1M/s；是否改用async pipeline、multiple outstanding I/O、
  replicated durability或不同storage，必須在本階段數據完成後決策。
