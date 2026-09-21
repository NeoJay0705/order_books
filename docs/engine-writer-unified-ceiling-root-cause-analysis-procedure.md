# Engine Writer 統一 ceiling／根因分析操作規格

本文件依據 `docs/engine-writer-unified-ceiling-root-cause-analysis-design-review.md`，使用既有
benchmark 與 `benchmarks/run_engine_writer_diagnostics.sh` 完成一次受控 campaign，取得 Engine ceiling、
CPU profile、WAL／fsync、I/O 與 component evidence。

本流程不可執行 `git add`、`git reset`、`git restore --staged`、commit 或其他 Git index mutation；不可
修改 source、production config、CPU policy、sysctl、I/O scheduler、mount options、WAL format、group
commit 或 worker 設定。所有 run data 必須在 repository 外。

## 1. 完成定義

只有同時滿足下列條件，報告才可使用 `valid-attribution`：

- frozen ReleaseBenchmark binary 的 correctness、replay、durability 與 hash gate 通過；
- initial 及每個 case 前的 30 秒 preflight 通過；
- external observer overhead 不超過 3%；
- component、WAL、Writer 與 Engine case 的完整 raw artifacts 都保留；
- Engine group frontier 完成指定輪次；
- `perf stat`／`perf record`、phase accounting、iostat 與 same-path fio 都可用；
- 主要瓶頸有至少兩種獨立證據，且在多數重複輪次同方向；
- 否則只能輸出 `environment-*`、`invalid-run` 或 `inconclusive-attribution`。

不要用單輪最高 RPS、vendor sequential bandwidth 或單一 phase timer 宣稱 bottleneck。

## 2. 準備與 artifact 凍結

在 repository root 執行下列 read-only 檢查；正式 campaign 應使用專用 Linux host、可隔離的 WAL
filesystem 與 benchmark CPU。binary 必須完成 Debug、ASan／UBSan、Release CTest 與 smoke 後凍結。

```bash
set -euo pipefail
REPO_ROOT=/home/neojhou/repos/order_books
BENCHMARK_BINARY=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCHMARK_BINARY"
sha256sum "$BENCHMARK_BINARY"
git rev-parse HEAD
git diff --cached --binary | sha256sum
git diff --binary | sha256sum
git status --short
```

若 source、cached diff、worktree diff 或 binary 在 campaign 中改變，整次結果無效；不同 run root 或
不同 binary 的 raw rows 不得混合。

## 3. Storage 與 fio block size

`--fio-bs` 不得使用猜測值，必須來自同一 WAL encoding／相同 workload 的實測 bytes per selected group。
若既有有效 WAL report 沒有該欄位，先用既有 `wal_write_ceiling` 短 probe 取得 frame／group bytes：

```bash
"$BENCHMARK_BINARY" --workload=wal_write_ceiling --wal-sync=none \
  --wal-phase-profile=on --wal-group-size=256 --iterations=1000 --warmup=100
```

probe 只取得 bytes，不能當正式 ceiling。default、g4096 與另一個 frontier case 的 bytes 可用逗號傳給
runner，例如 `--fio-bs=16384,262144,524288`。每個值會在 run root 普通檔案中執行 buffered sequential
write + 每次 write 後 fsync；不可指定 raw block device。

執行前記錄 device model、介面、filesystem、mount options、I/O scheduler、vendor／cloud provisioned
bandwidth／IOPS及來源日期。沒有權威 declared limit 時填 `unknown`，由 same-path fio 作 empirical baseline。

## 4. Runner invocation

```bash
cd /home/neojhou/repos/order_books
bash benchmarks/run_engine_writer_diagnostics.sh \
  --binary=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --fio-bs=16384,262144,524288 \
  --run-parent=/home/neojhou --bench-cpus=2-7 --observer-cpus=0-1 \
  --cpu-list=2,3,4,5,6,7 --calibration-iterations=250000 --warmup=10000 \
  --case-timeout-seconds=300 --observer=on
```

只檢查 plan，不啟動 benchmark、perf、fio 或建立 run data：

```bash
bash benchmarks/run_engine_writer_diagnostics.sh --dry-run \
  --binary=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --fio-bs=16384,262144,524288
```

frontier 選出不同 case 後，以新的 run root 補做 call graph：

```bash
bash benchmarks/run_engine_writer_diagnostics.sh --profile-only \
  --binary=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --profile-group-size=4096 --profile-delay-us=1000 --run-parent=/home/neojhou
```

`profile-only` 結果不取代完整 campaign 的 ceiling median。

## 5. Runner 流程與停止規則

Runner 會檢查工具、binary、CPU affinity、`perf` capability 與 run parent，建立 repository 外唯一 run
root，保存 identity／CPU／OS／filesystem／block-device metadata，然後執行 initial 30 秒 preflight。
任一 gate 失敗即輸出 `preflight-busy` 並停止；每個 case 前都會重新 preflight。

建立 run root 前的工具、affinity、perf event/call-graph capability 失敗會輸出機器可解析的
`result=environment-blocked` 與 `reason=...`；run root 建立後才檢查 WAL filesystem／block-device identity，
該類失敗寫入 `logs/result.txt`。CLI 參數錯誤仍是 usage error，不可與環境阻塞混淆。

完整 campaign 會先校準八個 Engine frontier case 的 iterations，然後以相同的 default Engine case 交錯執行
observer `off,on / on,off / off,on`，計算 throughput 與 total CPU seconds/M commands 的 median bias；任一
超過 3% 即輸出 `observer-biased` 並停止，不進入正式 matrix。calibration不納入正式median。

正式 case 在 observer CPU 啟動 `mpstat`／`iostat`，在 benchmark CPU 執行 benchmark；`observer=on` 另以
  `perf stat` 收集 task-clock、cycles、instructions、cache-misses、context-switches、CPU migrations
與 page faults。stdout、stderr、GNU time、perf stat、mpstat、iostat、exit status與data directory
全部保留。正式 Engine ceiling case 預設關閉 `EngineTailTelemetry`，避免 benchmark-side collector 污染
production-like ceiling；在 frontier 穩定性 gate 後，對候選 case 另做 tail off/on attribution，將唯一 CSV
寫到 run root `derived/`，以取得 publisher／completion backlog、drain 與 measured fsync tail。tail-on RPS
不得混入正式 ceiling median，也不得套用到 Writer profile case。

Runner 另執行獨立 `perf record -F 99 -g --call-graph fp`／`perf report --stdio` 及 same-path file-backed
fio。它不會自動等待外部負載、調整 governor／sysctl、刪除慢輪或宣稱 root cause。`perf` 不可用時為
`environment-blocked`，不能增加 hot-path logging 代替。

Calibration 會將每個 Engine case 的 `calibration_commands` 與以十進位字串精確計算的
`calibration_iterations * 2` 做 exact count gate，並以真正的 ceil 計算 formal iterations；formal
iterations 的乘二也必須先通過 uint64 overflow check。frontier scan 固定使用該 formal plan，不可用
calibration 輸出取代正式 commands。每個 frontier row 必須同時具備 status、stdout、GNU time、normalized
row，以及 observer-on 時的 perf/mpstat/iostat artifact；`commands`、`wal_group_commands`、`wal_group_commits`、
`wal_bytes_delta` 與 duration 不符時，以 `command-count-mismatch`、`counter-invalid`、
`duration-under-30s` 或 `missing-artifact` 排除，CV 超過 5% 則記為 `cv-over-5-percent`。duration 不足
會成為該 case 的 exclusion，不會在 selection 前中止整個 frontier。

## 6. 預定 matrix

### 6.1 Component

每個 stage 五輪：`state_machine`、`invariant_validation`、`metrics`、`runtime_handoff`。分開報告
commands/s、latency、CPU、context switches與correctness；`metrics` 的single、separate-registry與
contended-registry結果不可混合。

### 6.2 WAL

group size `256, 1024, 4096, 8192`各執行`sync=none`與`sync=per_group`五輪，固定
`wal-prepare-workers=2`、`wal-parallel-prepare-min-commands=4096`。每輪記錄commands/s、CPU seconds/M、
frame／payload bytes、bytes/s、write calls、writes/group、fsync/group、fsync/s、append／prepare／
plan-copy／write／publish／sync counters，以及p50、p99、p99.9、max與replay verification。

`sync=none`仍可能包含rotation必要sync，不得寫成zero-fsync ceiling。

### 6.3 Full Engine frontier

固定單 instrument、單 shard、crossing pair、producer lanes 8192、W=2、per-group fsync，掃描：

```text
group size:  256, 1024, 4096, 8192
group delay: 200 us, 1000 us
```

八個 case 先各三輪；runner 先排除 CV 超過 5%、artifact 不完整或 duration 不足的 case，再依三輪
completed RPS median 排序。候選依 median 由高到低執行 tail off/on sustainability gate；第一個通過者才
可稱 observed sustainable ceiling。production default、既有 g4096/d1000 與該 case再各五輪。若兩個身份重合只執行一次。每輪 measured
duration至少30秒；calibration產生的 iteration plan 不足時，runner 必須在正式 matrix 前停止。必須同時保留writer service RPS、
completed RPS、command p50／p99／p99.9／max、publisher lag、completion queue depth、drain結果、WAL
bytes/s、group commits、commands/group與fsync/s。async backlog線性增加時，只能稱瞬時ceiling。

每個 candidate 的 tail artifact 使用 `tail-gate-<case>-validation.txt` 與
`tail-gate-<case>.summary`；`status=rejected` 僅表示 backlog/drain/slope 或 bias gate 不通過，
malformed/missing artifact 會以 `status=invalid` 並終止 campaign。wrapper 會在 rejected 或 invalid
return 後恢復原 observer mode，且已執行的 candidate 結果會被 comparison 階段重用，不重跑或覆寫其
validation／summary。`frontier-selection.tsv` 僅保存固定六欄的每 case validity row；
`frontier-selection.txt` 追加 tail rejection、selected sustainable 與 default／g4096／selected 的
comparison 結果，不覆寫先前 candidate 證據。

### 6.4 Writer phase與CPU profile

對production default、g4096/d1000與選出的最高 case執行profile off/on配對，on使用
`--writer-profile-sample-every=16`。逐group tail CSV與
Writer procfs snapshot已移除，不得自行加回；保留既有non-overlapping phase與WAL subphase。sampled
profile相對off的throughput或CPU seconds/M bias超過3%時，phase只能作定性輔助。perf record只作 hotspot
證據，不把該輪RPS併入正式median。

## 7. Gate與 bottleneck 判讀

CPU／storage准入沿用：CPU 2-7每CPU 30秒平均idle最低值>=90%、最大iowait<=5%、WAL block device平均
util<=5%、最大aqu-sz<=0.25。任一case失敗停止整次campaign；不能只保留成功輪次。

observer off/on至少各三輪，使用相同完整Engine case、affinity與data layout。median throughput或CPU
seconds/M差異大於3%時為`observer-biased`。

CPU 根因需要 CPU／task-clock 接近可用核心容量、perf hotspot與phase accounting同區域、且storage未
接近effective ceiling。metrics 根因需要 perf 落在MetricsRegistry／map／mutex且metrics stage與Writer
post-apply share同方向。storage write／fsync根因需要phase主導、WAL bytes/s／fsync/s／sync tail一致、
same-path fio相近，並由device util／await／queue支持。單一phase timer、單一perf sample、device util或
datasheet數字只能支持`inconclusive-attribution`。

## 8. Report format

建立 `docs/engine-writer-unified-ceiling-root-cause-analysis-report.md`，固定包含：

```text
result: valid-attribution | environment-blocked | preflight-busy |
        observer-biased | invalid-run | environment-unstable |
        inconclusive-attribution
run_root:
commit:
binary_sha256:
cached_diff_sha256:
worktree_diff_sha256:
cpu_affinity:
observer_affinity:
filesystem_and_mount:
wal_device_and_model:
declared_storage_limit_source:
perf_capability:
collection_status:
engine_iteration_plan:
```

正文依序放 preflight／observer gate、component ceiling、WAL ceiling、Engine frontier與Pareto表、Writer
phase accounting與unaccounted share、perf stat與top call stacks、WAL bytes/s／fsync/s／commands/fsync、
fio同path ratio、async backlog／drain、tail off/on bias、iteration plan、CV與排除理由、一項classification
及兩種獨立證據、限制與下一個最小production change（或`no production change`）。不可把component RPS、
profile RPS、fio bandwidth、tail-on RPS或單輪max混成Engine RPS。

## 9. Implementation turn 後的檢查

只需執行短 dry-run／shell syntax／contract test，不在 shared CI執行長時間矩陣：

```bash
bash benchmarks/test_engine_writer_diagnostics_runner.sh
```

Contract test 只在 fake environment 執行，使用 `ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1` 的內部
duration／round／case override；未啟用 test mode 時 runner 會拒絕任何 test override。測試可在 fake
host timing 下使用只限 test mode 的 bias limit，但正式 campaign 永遠使用 observer/tail 3% gate。測試
也必須驗證 exact perf event list，以及各 failure scenario 的 `result`、`reason` 與 benchmark invocation
count；malformed tail CSV 必須分類為 `invalid-run`，不可當作 sustainability rejection。

真正 build、CTest與本文件的benchmark須等使用者檢閱 implementation 後另行執行。最後只做 read-only
validation：

```bash
git diff --cached --check
git diff --check
git status --short
```

不得以檢查為由執行任何 staging mutation。
