# StateMachine／WAL Component Ceiling 壓測操作與報告規格

## 1. 目的與量測邊界

本文件將 `docs/state-machine-wal-component-ceiling-design-review.md` 轉成可重複執行的正式壓測流程，取得：

- 四種 `StateMachine::apply()` scenario 的固定 command ceiling；
- WAL `append_batch()` 在 batch `1／256／1024／4096／8192` 下、每個 measured epoch 不跨 segment 的
  append-return ceiling；
- 以 production segment size 執行的 rotation-control／rotation-trigger attribution case；
- 相同 batch matrix 下，`append_batch() + Wal::sync()` 的 durable throughput／latency frontier；
- 每個 case 的 process CPU、host CPU 與 storage 使用量；
- WAL profile-on round 的 prepare、plan/copy、write、publish與sync分段證據。

本次只執行 `--scope=component`。結果不包含 Engine queue／admission、Publisher、Completion worker、network
downstream或端到端completion latency，也不能替代完整Engine sustainable ceiling。

正式append ceiling使用profile-off的`wal_append_no_rotation`資料；rotation diagnostic與profile-on資料只作
成本歸因。durable frontier仍使用`append_batch() + Wal::sync()`，允許production rotation。測試不執行`perf`或
`fio`，因此不能宣稱函式級CPU hotspot或storage理論上限。

### 1.1 本輪是否需要重跑

需要。這次實作修正了calibration與formal case的duration gate邊界、失敗case資料保留、resource欄位及rotation
subphase契約；舊run不能證明新runner能完成整個matrix，也不能和新binary的輪次混用。必須以新run root重新執行
完整component campaign，才能產生有效ceiling。

已知的calibration假阻塞已由runner contract測試覆蓋：fake calibration只回報1 ms，而正式case最小duration仍為
30秒；calibration只驗證command、rate、bytes、correctness／replay等規劃所需語意，不套用正式duration gate。
因此不應再因「短校準未達30秒」停止。這不代表campaign保證成功；真實的host busy、disk budget、benchmark
timeout、correctness／replay、counter、CV、observer bias或identity失敗仍必須安全停止，不能略過。

## 2. 安全與可重複性要求

- 所有run artifacts寫在repository外、與目標WAL相同的block-backed filesystem。
- benchmark CPU與observer CPU不得重疊。
- 不修改sysctl、CPU governor、IRQ、cgroup、filesystem設定或Git index。
- 不設定任何`ENGINE_WRITER_DIAGNOSTICS_TEST_*`環境變數。
- identity凍結後直到campaign結束，不得編譯、修改檔案、切換branch或改變staging。
- 每個正式case由runner執行preflight；不通過時保留該run root並停止，不在同一run root反覆嘗試。
- 不以舊run補足新run的缺失輪次，不混合不同binary、worktree或host的資料。
- runner只刪除其建立且已成功驗證的case data directory；logs、monitor、time與derived artifacts保留。

## 3. 工具與主機前置檢查

在repository root執行：

```bash
set -euo pipefail

REPO_ROOT=/home/neojhou/repos/order_books
CONAN_VENV=/home/neojhou/.venvs/order-books-conan2
export PATH="$CONAN_VENV/bin:$PATH"
cd "$REPO_ROOT"

for tool in conan cmake ctest ninja git rg sha256sum findmnt lsblk lscpu \
  mpstat iostat jq taskset timeout awk realpath ps cmp sed sort tee uname \
  xargs mktemp date df pgrep basename head tail tr; do
  command -v "$tool"
done
test -x /usr/bin/time

cmake --version
ctest --version
conan --version
ninja --version
mpstat -V
iostat -V
```

必要條件：CMake／CTest至少3.25、Conan為2.x，且`sysstat`提供`mpstat`與`iostat`。component scope不要求
`perf`或`fio`。

確認沒有測試用環境變數：

```bash
if env | rg '^ENGINE_WRITER_DIAGNOSTICS_TEST_'; then
  printf '請使用未設定 ENGINE_WRITER_DIAGNOSTICS_TEST_* 的乾淨 shell\n' >&2
  exit 1
fi
```

## 4. Build與correctness gate

正式campaign只使用ReleaseBenchmark binary。若目前source或dependency已改變，重新建立Release toolchain、build
與CTest；任何一步失敗都不得開始壓測：

```bash
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake --preset release-benchmark
cmake --build --preset release-benchmark --parallel 4
ctest --test-dir build/ReleaseBenchmark --output-on-failure

bash -n benchmarks/run_engine_writer_diagnostics.sh
bash -n benchmarks/test_engine_writer_diagnostics_runner.sh
bash benchmarks/test_engine_writer_diagnostics_runner.sh
git diff --check
git diff --cached --check
```

Sanitizer只用於correctness，不用於正式效能數據。若本次implementation review後尚未執行ASan／UBSan，應在
identity凍結前補做既有sanitizer preset；不要在campaign進行中建置。

## 5. 選擇CPU與run parent

先唯讀確認topology、目標filesystem與可用空間：

```bash
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
findmnt -T /home/neojhou
lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS,MODEL
df -h /home/neojhou
```

以下正式範例沿用benchmark CPU `2-7`、observer CPU `0-1`。執行前必須確認這些CPU online，且兩組不重疊：

```bash
BENCH_CPU_SET=2-7
OBSERVER_CPU_SET=0-1
CPU_LIST=2,3,4,5,6,7
RUN_PARENT=/home/neojhou
BENCHMARK_BINARY="$REPO_ROOT/build/ReleaseBenchmark/benchmarks/order_books_benchmark"

test -x "$BENCHMARK_BINARY"
taskset -c "$BENCH_CPU_SET" true
taskset -c "$OBSERVER_CPU_SET" true
test "$(realpath -m "$RUN_PARENT")" != "$(realpath "$REPO_ROOT")"
sha256sum "$BENCHMARK_BINARY"
```

不使用CPU isolation、cgroup或系統政策修改。其他process仍可能使用相同CPU，因此runner的30秒preflight才是
正式有效性gate。

## 6. 凍結identity

Build、CTest與操作文件都完成後，記錄以下資料。從此刻到runner結束不得變更：

```bash
git rev-parse HEAD
git status --short
git diff --cached --binary | sha256sum
git diff --binary | sha256sum
git ls-files --others --exclude-standard -z | LC_ALL=C sort -z | \
  xargs -0 -r sha256sum | sha256sum
sha256sum "$BENCHMARK_BINARY"
```

允許worktree原本不是clean，但before／after identity必須完全相同。正式report只能在campaign結束後建立；執行中
新增或編輯report也會使run失效。

## 7. Dry run

Dry run只驗證參數與scope，不建立run root、不執行benchmark：

```bash
bash benchmarks/run_engine_writer_diagnostics.sh \
  --scope=component --dry-run \
  --binary="$BENCHMARK_BINARY" \
  --run-parent="$RUN_PARENT" \
  --bench-cpus="$BENCH_CPU_SET" \
  --observer-cpus="$OBSERVER_CPU_SET" \
  --cpu-list="$CPU_LIST" \
  --calibration-iterations=250000 \
  --warmup=10000 \
  --case-timeout-seconds=300 \
  --observer=on
```

輸出必須包含`scope=component`以及`state_machine`、`wal_append`、`wal_fsync`，且不得包含`perf_record`或
`fio`。component scope不需要`--fio-bs`。

## 8. 執行前idle screening

這一步只避免在明顯繁忙時啟動長campaign，不取代runner的正式per-case preflight。必須在預計啟動campaign的
同一時段執行一次，通過後立即進入第9節；不得反覆抽樣直到偶然通過。

本機CPU `2-7`的SMT siblings是`10-15`。正式benchmark affinity仍為`2-7`，但啟動前screening同時觀察
`2-7,10-15`，避免漏掉同一physical core上的競爭。若CPU topology改變，先依第5節的`lscpu`結果更新
`SCREEN_CPU_LIST`，不能沿用舊值。

```bash
SCREEN_CPU_LIST=2,3,4,5,6,7,10,11,12,13,14,15
MOUNT_SOURCE=$(findmnt -n -o SOURCE -T "$RUN_PARENT")
BLOCK_DEVICE=$(lsblk -no PKNAME "$MOUNT_SOURCE" 2>/dev/null | awk 'NF {print; exit}')
if test -z "$BLOCK_DEVICE"; then
  BLOCK_DEVICE=$(basename "$MOUNT_SOURCE")
fi
test -n "$BLOCK_DEVICE"

ps -eo pid,comm,psr,pcpu,stat,args --sort=-pcpu | head -n 20
if ps -eo comm= | awk '
  $1 == "order_books_benchmark" || $1 == "mpstat" || $1 == "iostat" ||
  $1 == "fio" || $1 == "perf" { print; found=1 }
  END { exit found ? 0 : 1 }'; then
  printf '發現殘留benchmark或observer process，停止啟動\n' >&2
  exit 1
fi

AVAILABLE_BYTES=$(df --output=avail -B1 "$RUN_PARENT" | tail -n 1 | tr -d ' ')
MIN_FREE_BYTES=$((20 * 1024 * 1024 * 1024))
test "$AVAILABLE_BYTES" -gt "$MIN_FREE_BYTES"

IDLE_PROBE_ROOT=$(mktemp -d "$RUN_PARENT/state-machine-wal-component-idle-XXXXXXXX")
taskset -c "$OBSERVER_CPU_SET" \
  mpstat -P "$SCREEN_CPU_LIST" 1 30 -o JSON >"$IDLE_PROBE_ROOT/mpstat.json" &
CPU_PROBE_PID=$!
taskset -c "$OBSERVER_CPU_SET" \
  iostat -y -dx "$BLOCK_DEVICE" 1 30 -o JSON >"$IDLE_PROBE_ROOT/iostat.json" &
DISK_PROBE_PID=$!
wait "$CPU_PROBE_PID"
wait "$DISK_PROBE_PID"

CPU_IDLE_MIN=$(jq -r '
  [.sysstat.hosts[0].statistics[] | .["cpu-load"][]]
  | sort_by(.cpu) | group_by(.cpu)
  | map(map(.idle) | add / length) | min' "$IDLE_PROBE_ROOT/mpstat.json")
CPU_IOWAIT_P95=$(jq -r '
  [.sysstat.hosts[0].statistics[] | .["cpu-load"][] | .iowait]
  | sort | .[((length - 1) * 0.95 | floor)]' "$IDLE_PROBE_ROOT/mpstat.json")
CPU_IOWAIT_MAX=$(jq -r '
  [.sysstat.hosts[0].statistics[] | .["cpu-load"][] | .iowait] | max' \
  "$IDLE_PROBE_ROOT/mpstat.json")
DISK_UTIL_AVG=$(jq --arg device "$BLOCK_DEVICE" -r '
  [.sysstat.hosts[0].statistics[].disk[]
    | select(.disk_device == $device) | .util]
  | if length == 0 then null else add / length end' "$IDLE_PROBE_ROOT/iostat.json")
DISK_AQU_P95=$(jq --arg device "$BLOCK_DEVICE" -r '
  [.sysstat.hosts[0].statistics[].disk[]
    | select(.disk_device == $device) | .["aqu-sz"]]
  | if length == 0 then null else sort | .[((length - 1) * 0.95 | floor)] end' \
  "$IDLE_PROBE_ROOT/iostat.json")
DISK_AQU_MAX=$(jq --arg device "$BLOCK_DEVICE" -r '
  [.sysstat.hosts[0].statistics[].disk[]
    | select(.disk_device == $device) | .["aqu-sz"]]
  | if length == 0 then null else max end' "$IDLE_PROBE_ROOT/iostat.json")

IDLE_RESULT=pass
if ! awk -v value="$CPU_IDLE_MIN" 'BEGIN { exit !(value != "null" && value >= 90.0) }'; then
  IDLE_RESULT=fail
fi
if ! awk -v value="$CPU_IOWAIT_P95" 'BEGIN { exit !(value != "null" && value <= 5.0) }'; then
  IDLE_RESULT=fail
fi
if ! awk -v value="$CPU_IOWAIT_MAX" 'BEGIN { exit !(value != "null" && value <= 10.0) }'; then
  IDLE_RESULT=fail
fi
if ! awk -v value="$DISK_UTIL_AVG" 'BEGIN { exit !(value != "null" && value <= 5.0) }'; then
  IDLE_RESULT=fail
fi
if ! awk -v value="$DISK_AQU_P95" 'BEGIN { exit !(value != "null" && value <= 0.25) }'; then
  IDLE_RESULT=fail
fi
if ! awk -v value="$DISK_AQU_MAX" 'BEGIN { exit !(value != "null" && value <= 0.50) }'; then
  IDLE_RESULT=fail
fi

printf 'result=%s\ncpu_scope=%s\nblock_device=%s\navailable_bytes=%s\n' \
  "$IDLE_RESULT" "$SCREEN_CPU_LIST" "$BLOCK_DEVICE" "$AVAILABLE_BYTES" \
  >"$IDLE_PROBE_ROOT/summary.txt"
printf 'cpu_idle_min=%s\ncpu_iowait_p95=%s\ncpu_iowait_max=%s\n' \
  "$CPU_IDLE_MIN" "$CPU_IOWAIT_P95" "$CPU_IOWAIT_MAX" \
  >>"$IDLE_PROBE_ROOT/summary.txt"
printf 'disk_util_avg=%s\ndisk_aqu_p95=%s\ndisk_aqu_max=%s\n' \
  "$DISK_UTIL_AVG" "$DISK_AQU_P95" "$DISK_AQU_MAX" \
  >>"$IDLE_PROBE_ROOT/summary.txt"
cat "$IDLE_PROBE_ROOT/summary.txt"
test "$IDLE_RESULT" = pass
```

門檻與WAL per-case preflight一致：CPU idle最低值至少90%、iowait p95不超過5%且max不超過10%、device
util平均不超過5%、aqu-sz p95不超過0.25且max不超過0.50。20 GiB只是啟動前最低保留空間；calibration後
runner仍會以實測bytes/command逐case驗證disk budget。

如果screening失敗，保留`IDLE_PROBE_ROOT`並先查明top process或外部I/O來源；不要由本流程自動kill未知process。
如果通過，也不能關閉runner內建preflight，因為數小時campaign期間環境仍可能改變。

## 9. 正式component campaign

建立repository外的console log，執行唯一正式命令：

```bash
CAMPAIGN_LOG=$(mktemp "$RUN_PARENT/state-machine-wal-component-campaign-XXXXXXXX.log")

set +e
bash benchmarks/run_engine_writer_diagnostics.sh \
  --scope=component \
  --binary="$BENCHMARK_BINARY" \
  --run-parent="$RUN_PARENT" \
  --bench-cpus="$BENCH_CPU_SET" \
  --observer-cpus="$OBSERVER_CPU_SET" \
  --cpu-list="$CPU_LIST" \
  --calibration-iterations=250000 \
  --warmup=10000 \
  --case-timeout-seconds=300 \
  --observer=on 2>&1 | tee "$CAMPAIGN_LOG"
CAMPAIGN_STATUS=${PIPESTATUS[0]}
set -e

RUN_ROOT=$(awk -F= '/^run_root=/{value=$2} END {print value}' "$CAMPAIGN_LOG")
printf 'campaign_status=%s\nrun_root=%s\n' "$CAMPAIGN_STATUS" "$RUN_ROOT"
test -n "$RUN_ROOT"
test -d "$RUN_ROOT"
```

runner自動執行下列流程，不要在外層自行重排或平行化：

1. repository／binary／CPU policy identity；
2. StateMachine calibration與固定command frozen plan；
3. 四個scenario各五輪，輪序正反交替；
4. WAL append calibration，凍結各batch的fresh-WAL epoch command plan，五個batch各五輪；
5. b1 rotation-control與rotation-trigger各五輪的profile attribution；
6. WAL fsync calibration與五個batch各五輪；
7. append／fsync各batch一輪profile-on及一輪代表性profile-off control；
8. counter、duration、CV、observer bias、replay與identity gate。

預設矩陣共有約105次benchmark invocation：14次短calibration、20次StateMachine正式case、25次append、
10次rotation diagnostic、25次fsync及11次profile/control。加上每次preflight、WAL cooldown與約40秒正式measured
window，正常執行時間約3小時，實際仍取決於fsync calibration與host。60秒cooldown或30秒preflight期間沒有新
benchmark stdout是預期行為，不能因此判定卡住或中止。

campaign執行中只做唯讀進度檢查：

```bash
tail -n 30 "$CAMPAIGN_LOG"
pgrep -af 'run_engine_writer_diagnostics|order_books_benchmark|mpstat|iostat'
find "$RUN_ROOT/logs" -maxdepth 1 -name 'component-*.status' -type f | wc -l
if test -s "$RUN_ROOT/logs/result.txt"; then
  cat "$RUN_ROOT/logs/result.txt"
fi
```

只要runner仍存活且尚未寫出terminal result，就讓目前case完成。不要在執行中編譯、修改文件、改staging、建立
report或啟動第二個campaign。若runner退出，必須依第10節分類；不得以手動略過gate或接續同一run root處理。

StateMachine case只以CPU gate決定是否可開始；WAL case先cooldown 60秒，再執行30秒CPU＋storage preflight。
所有正式case measured phase至少30秒，所以完整campaign可能需要數小時；不得因等待時間而縮短duration、rounds或
cooldown。

Append正式輸出應為`wal_append_no_rotation`，且每個epoch必須有
`measured_segment_rotations=0 rotation_scope=no_rotation replay_verified=true`。latency輸出包含
`latency_sample_stride`、`append_latency_sample_count`與精確`append_group_max_us`；percentile只代表固定
stride sample，total與max仍由全量measured groups累積。rotation diagnostic另存
`derived/component-rotation-attribution.tsv`，不得把它併入append ceiling的CV或median。
該TSV保留control／trigger每輪raw row，至少包含rotation前後active segment id、active offset、Dirty／Writeback
快照，以及append與old-segment sync、new-header write/sync、directory sync latency；`rotation_triggered`
與replay/correctness欄位則由對應stdout驗證。
rotation diagnostic由benchmark的`--wal-rotation-diagnostic=control|trigger`提供；每次只量一個
`--wal-group-size=1 --iterations=1` target group，setup-fill在measured boundary外且不sync。control必須精確
0次rotation，trigger必須精確1次rotation；兩者的stdout名稱必須是`wal_rotation_diagnostic`，並有
`wal_byte_plan_verified=true`及`append_latency_sample_count=1`的證據。任何缺欄位、重複欄位、`na` resource counter、
或rotation／replay不符預期都分類為`invalid-run`，不套用30秒duration gate。

## 10. Terminal classification與停止規則

先查看：

```bash
cat "$RUN_ROOT/logs/result.txt"
```

只有下列組合可進入正式彙整：

```text
CAMPAIGN_STATUS=0
collection_status=complete
result=valid-component-ceiling
```

其他分類：

| 結果 | 意義 | 處理方式 |
| --- | --- | --- |
| `preflight-busy` | CPU或storage不夠idle | 保留run root；環境穩定後以新run root重跑完整campaign |
| `observer-biased` | profile off/on bias超過3% | ceiling仍不得由該run宣稱；保留artifact分析observer成本 |
| `collection-complete-results-partial` | 所有case已收集，但一個或多個case CV超過5% | 保留rejected raw rows；不得用它宣稱ceiling或Pareto；只重跑新的完整run root |
| `environment-unstable` | 早期流程或舊版runner以environment名義停止 | 依run root中的具體reason分類；不能把workload rotation I/O泛稱外部環境繁忙 |
| `invalid-run` | counter、duration、replay、artifact或identity失敗 | 先修正原因，再以新run root重跑 |

不得刪除慢輪、以第六輪替換失敗輪，或把不同run root的輪次合併成五輪。

## 11. Artifact驗證

有效run至少驗證：

```bash
test -s "$RUN_ROOT/derived/component-frozen-plan.tsv"
test -s "$RUN_ROOT/derived/component-duration-plan.tsv"
test -s "$RUN_ROOT/derived/component-disk-budget.tsv"
test -s "$RUN_ROOT/derived/component-cv.tsv"
test -s "$RUN_ROOT/derived/component-case-status.tsv"
test -s "$RUN_ROOT/derived/component-rotation-attribution.tsv"
test -s "$RUN_ROOT/derived/component-profile-bias.tsv"
test -s "$RUN_ROOT/logs/environment-before.txt"
test -s "$RUN_ROOT/logs/repository-identity-before.txt"
test -s "$RUN_ROOT/logs/repository-identity-after.txt"
test -s "$RUN_ROOT/logs/binary-sha256-before.txt"
test -s "$RUN_ROOT/logs/binary-sha256-after.txt"

cmp "$RUN_ROOT/logs/repository-identity-before.txt" \
  "$RUN_ROOT/logs/repository-identity-after.txt"
cmp "$RUN_ROOT/logs/binary-sha256-before.txt" \
  "$RUN_ROOT/logs/binary-sha256-after.txt"
cat "$RUN_ROOT/derived/component-frozen-plan.tsv"
cat "$RUN_ROOT/derived/component-cv.tsv"
cat "$RUN_ROOT/derived/component-profile-bias.tsv"
```

每個formal case必須同時具有：

- `logs/component-<label>.stdout`、`.stderr`與`.status`；
- `time/component-<label>.time`；
- `monitors/component-<label>-mpstat.txt`與`-iostat.txt`；
- `preflight/component-<label>-summary.txt`；
- `derived/component-<label>.row`。

runner只有在replay、schema、counter、position與duration語意驗證全部成功後，才刪除該case的WAL data
directory；binary exit 0但任一語意gate失敗時仍須保留data供診斷，不能把失敗case先清掉，也不能把成功後
data directory不存在誤判為artifact缺失。

## 12. 指標來源與計算規則

### 12.1 共通統計

- 每個正式case保留五輪原始值。
- throughput headline使用五輪`commands_per_second`或`service_commands_per_second`的median。
- 穩定性直接引用`component-cv.tsv`；CV必須`<=5%`。
- percentile欄位以五個per-run percentile的median呈現，並另外列五輪min–max。
- `max`欄位使用五輪中最大的per-run max，不取median，以保留最差tail。
- 不合併不同run的samples，不從aggregate percentile反推單筆command percentile。

### 12.2 StateMachine

來源：`logs/component-state-r<1..5>-<scenario>.stdout`。

- throughput：`commands_per_second`；
- latency：`average_ns_per_command`的五輪median與min–max；
- correctness：`measured_commands`、trades、events、active state、EngineSeq與
  `correctness_verified=true`；
- completion boundary：`state_apply_return`。

StateMachine沒有逐command sampling，不得報p50／p99／p99.9。

### 12.3 WAL append-return

來源：`logs/component-append-r<1..5>-b<batch>.stdout`。

- headline throughput：`service_commands_per_second`；
- secondary throughput：`workload_wall_commands_per_second`；
- bandwidth：`service_wal_mib_per_second`及`workload_wall_wal_mib_per_second`；
- latency：`append_group_p50_us／p99_us／p99.9_us／max_us`；
- storage shape：`average_wal_bytes_per_command`、`wal_bytes_delta`、segment count與rotation；
- validity：`rotation_scope=no_rotation`與`measured_segment_rotations=0`；
- byte plan：`frame_bytes_per_command`、`planned_wal_bytes_delta`與
  `wal_byte_plan_verified=true`；每個epoch actual delta必須等於planned delta，segment id/count不得改變；
- sampling：`latency_sample_stride`與`append_latency_sample_count`。percentile只代表固定stride
  sample，`append_group_max_us`是全量measured groups的精確max；
- measured resource：`measured_user_seconds`、`measured_system_seconds`、context switches、
  `/proc/self/io`的`measured_syscw`／`measured_wchar`／`measured_write_bytes`／
  `measured_cancelled_write_bytes`、Dirty／Writeback與`measured_wal_write_calls`／
  `measured_wal_sync_calls`；`measured_wal_write_calls`是measured window的write-family syscall數，
  `profiled_data_write_calls`則是phase profile的data-chunk write數，兩者不可互換；
- validity gate：`measured_rusage_valid`、`measured_io_valid`及`measured_meminfo_valid`各恰好一次且為true；
  所有上述欄位必須是非負數字，`measured_wal_write_calls == measured_syscw`，且sync calls等於planned
  groups。任何缺失、重複、`na`或不一致都立即分類為`invalid-run`；
- completion boundary：`append_batch_return`。

append latency是每個`append_batch()` group的latency。只有`batch=1`可同時視為singleton command call latency；
其他batch不得把group percentile寫成per-command percentile。

### 12.4 WAL durable fsync

來源：`logs/component-fsync-r<1..5>-b<batch>.stdout`。

- headline throughput：`service_commands_per_second`；
- `commands_per_fsync = group_size`；
- `service_fsync_per_second = groups / (service_elapsed_ms / 1000)`；
- `wall_fsync_per_second = groups / (workload_wall_elapsed_ms / 1000)`；
- append latency：`append_group_*`；
- fsync latency：`sync_*`；
- durable group latency：`group_total_*`；
- rotation evidence：`measured_segment_rotations`與`rotation_scope=rotation_inclusive`；
- completion boundary：`group_fsync`。

每輪`sync_samples`必須等於planned groups且至少1,000；runner已執行精確counter gate。

### 12.5 CPU與memory

來源：`time/component-<label>.time`及`monitors/component-<label>-mpstat.txt`。

```text
cpu_seconds = user_seconds + system_seconds
average_process_cpu_percent = cpu_seconds / GNU_time_wall_seconds * 100
cpu_seconds_per_million = cpu_seconds / measured_commands * 1,000,000
```

同時報告maximum resident set size、voluntary／involuntary context switches。process CPU可超過100%，不得截斷。
GNU time與mpstat涵蓋整個process，包括setup、warmup與replay；欄位必須命名為`whole_process_*`，不能宣稱為
純measured hot-path CPU。

Benchmark process內的`measured_*`欄位只涵蓋measured append或durable loop；Dirty／Writeback是measured
boundary後的`/proc/meminfo` gauge，不是累積counter。報告必須把兩者分開，不得以measured process counter
取代iostat的device await、queue與util。

### 12.6 Storage

來源：`monitors/component-<label>-iostat.txt`。

逐輪報告目標device的：

- write MiB/s與write IOPS的interval平均；
- write await與`aqu-sz`的p95及max；
- `%util`的平均及max。

iostat window同樣涵蓋整個process。StateMachine的iostat只作環境證據；WAL結果才可用於storage usage分析。
本流程沒有fio control，不能以這些數據宣稱裝置理論頻寬或fsync硬體極限。

### 12.7 WAL phase profile

來源：`logs/component-profile-append-b*.stdout`、`component-profile-fsync-b*.stdout`與
`derived/component-profile-bias.tsv`。

報告fixture build、WAL append call、lock wait、prepare、prepare task、plan/copy、rotation、write、publish、sync
與unattributed的group percentile、total及share。profile bias必須`<=3%`；即使通過，profile throughput仍不納入
ceiling median。

## 13. 正式報告格式

建立：

```text
docs/state-machine-wal-component-ceiling-benchmark-report.md
```

報告至少使用以下結構。

### 13.1 執行摘要

```markdown
# StateMachine／WAL Component Ceiling 壓測報告

## 結論

- Result：valid-component-ceiling／其他分類
- StateMachine最高scenario ceiling：
- WAL append-return ceiling與batch：
- Durable WAL throughput／latency Pareto candidates：
- 主要resource evidence：
- 與1M commands/s的差距：
- 不可外推範圍：
```

### 13.2 Identity與環境

| 項目 | 值 |
| --- | --- |
| Run root／執行時間 | |
| HEAD／cached diff／worktree diff／untracked manifest | |
| Binary SHA-256 | |
| Host／kernel／compiler | |
| CPU topology／benchmark affinity／observer affinity | |
| Filesystem／mount source／block device | |
| CPU policy | |
| 啟動前idle probe root／30秒screening摘要 | |
| Build／CTest／runner contract | |

### 13.3 Validity gates

| Gate | 結果 | 證據 |
| --- | --- | --- |
| 外部30秒idle screening | | `IDLE_PROBE_ROOT/summary.txt` |
| Runner initial及per-case preflight | | |
| Calibration完成且未套用formal duration gate | | |
| Formal case完成數：StateMachine 20／append 25／rotation 10／fsync 25／profile/control 11 | | |
| Formal duration >= 30 s | | |
| 五輪CV <= 5% | | |
| Profile bias <= 3% | | |
| Counter／final state／replay | | |
| Repository／binary identity | | |

### 13.4 Frozen plan

直接整理`component-frozen-plan.tsv`，列出kind、scenario／batch、fixed commands、warmup、calibration rate、
預估duration、預估bytes與rounds。

### 13.5 StateMachine結果

| Scenario | Commands | RPS median | RPS min–max | CV | Avg ns/cmd median | Avg ns/cmd min–max | CPU s/M | whole-process CPU% | Validity |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

另附五輪raw rows。不得新增p50／p99欄位。

### 13.6 WAL append-return結果

| Batch | Commands | Service RPS median | Wall RPS median | CV | Service MiB/s | Append p50 | p99 | p99.9 | Worst max | CPU s/M | Write MiB/s | Validity |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

### 13.7 WAL durable fsync結果

| Batch | Commands/fsync | Service RPS median | Service fsync/s | CV | Append p99.9 | Sync p50 | Sync p99 | Sync p99.9 | Group p99.9 | Worst group max | CPU s/M | I/O await p95 | aqu-sz p95 | Util avg/max | Validity |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

每個batch附五輪raw rows。Pareto candidate定義為：不存在另一個有效batch同時具有不低於其median throughput且
不高於其median group p99.9，並至少一項嚴格較佳。這是本host的component frontier，不是production default。

### 13.8 Rotation attribution結果

直接整理`derived/component-rotation-attribution.tsv`與兩個rotation stdout raw rows：

| Case | Expected rotations | Measured rotations | Append p50 | Append p99 | Append max | Rotation total | Old segment sync | Header write | Header sync | Directory sync |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

control必須為0次rotation，trigger必須精確為1次；這兩個case只用來拆解rotation／durability成本，不得納入
append ceiling throughput、CV或Pareto。

### 13.9 Resource與profile證據

| Case | whole-process CPU% | User/System s | CPU s/M | Max RSS | Involuntary CS | Write MiB/s | Write IOPS | Await p95 | aqu-sz p95/max | Util avg/max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

| Workload／batch | Profile bias | Prepare share | Plan/copy share | Write share | Publish share | Sync share | Unattributed share | Evidence level |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

### 13.10 結論與限制

報告必須明確回答：

1. 四種StateMachine scenario各自的ceiling，是否有任一低於1M commands/s；
2. WAL append batch增加後的throughput uplift與plateau位置；
3. fsync amortization提高throughput時造成的p99.9／max代價；
4. 當時CPU、I/O queue、await與util是否顯示resource saturation；
5. profile分段是否足以指出component內成本，或只能標成`inconclusive-attribution`；
6. component ceiling與完整Engine結果的差距，但不得把差距直接歸因給Publisher／Completion；
7. 下一步是否已有足夠證據值得另立production optimization design。

## 14. 接受標準與後續

只有以下條件全部成立，報告才可宣稱有效component ceiling：

- runner terminal result為`valid-component-ceiling`；
- 所有formal measured duration至少30秒；
- 每個case五輪CV不超過5%；
- profile bias不超過3%；
- StateMachine與append使用各自一致的固定command數；
- append每個epoch的`measured_segment_rotations`均為0，且rotation attribution control／trigger分別精確滿足0／1；
- fsync每case至少1,000個且精確等於planned groups的sync samples；
- 所有correctness、counter、WAL replay與artifact gate通過；
- campaign前後repository、binary與CPU policy identity一致。

完成後保留run root與console log的絕對路徑。正式結論只基於本次有效run；歷史報告可另表比較，但不得混入
median、CV或percentile。若單一case CV被拒絕但其餘matrix已完整收集，使用
`collection-complete-results-partial`，報告只呈現rejected raw data，不產生ceiling或production修改建議。
