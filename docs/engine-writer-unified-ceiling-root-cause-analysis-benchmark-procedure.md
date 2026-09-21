# Engine Writer 統一 Ceiling／根因分析壓測操作文件

## 1. 目的與適用範圍

本文件將
`docs/engine-writer-unified-ceiling-root-cause-analysis-design-review.md` 與
`docs/engine-writer-unified-ceiling-root-cause-analysis-procedure.md` 轉成可執行的正式壓測流程。目標是在同一份
frozen binary、同一組 repository identity 與同一台 Linux host 上，一次取得：

- component、direct WAL 與完整 Engine 的 ceiling；
- group size／delay 的 throughput-latency frontier；
- Writer phase、CPU profile、I/O、fsync 與 async backlog 證據；
- 一項有至少兩種獨立證據支持的瓶頸分類，或明確的 `inconclusive-attribution`。

上述完整證據只由 full-attribution mode 產生；affinity-only 是權限受限時的先行 ceiling campaign，不包含
CPU profile、fio storage control 或有效根因歸因。

本次不修改 production code、production defaults、CPU／kernel／filesystem policy，也不把 Publisher 或
Completion worker 納入優化範圍。所有壓測資料必須寫在 repository 外；執行期間不得修改 source、binary、
worktree 或 Git index。

## 2. 執行模式與必要條件

本文件保留兩種互斥的 evidence mode；同一次 run 不得在兩者之間切換，也不得混用 artifact：

| Mode | 用途 | 必要工具 | 可下的結論 |
| --- | --- | --- | --- |
| `affinity-only` | 目前開發機的先行 ceiling 測試 | `mpstat`、`iostat`、`jq`、GNU `time`、`taskset` | sustainable Engine ceiling、latency、async sustainability；根因固定為未知 |
| `full-attribution` | 隔離 benchmark host 的正式歸因 | 上述工具加上可用的 `perf stat`、`perf record -g` 與 `fio` | ceiling，以及通過雙重證據門檻的根因分類 |

目前開發機的 `kernel.perf_event_paranoid=4`，執行帳號沒有生效中的 `CAP_PERFMON`；`perf stat` 已回報
access denied。因此本次選擇 `affinity-only`，不修改 sysctl、capability 或其他全域 kernel policy，直接執行
4.2。這不是完整歸因的替代品；結果必須標記 `inconclusive-attribution`，且不得據此修改 production code。

若之後改用隔離 benchmark host，必須以新的 run root 從 build／CTest、identity 與 preflight gate 重新開始，
不得把本次 affinity-only 數據補入 full-attribution campaign。

### 2.1 Affinity-only 必要條件

Affinity-only campaign 必須同時符合：

- benchmark CPU 與 observer CPU 不重疊；
- run parent 位於實際要評估的 WAL filesystem，且不在 repository 內；
- `mpstat`、`iostat`、`jq`、GNU `time`、`taskset` 與 `sha256sum` 可用；
- Debug、ASan／UBSan、Release CTest 與 ReleaseBenchmark build 已通過；
- 執行期間不設定任何 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` 環境變數；
- campaign 開始後 binary、HEAD、cached diff、worktree diff 與 untracked files 不再改變。

Affinity-only 不要求 `perf` 或 `fio`，並跳過 3.3、4.1 與 4.3；缺少這兩類 artifact 是模式限制，不是
artifact failure。

### 2.2 Full-attribution 必要條件

正式 campaign 必須在 Linux 專用或可隔離 host 執行，並同時符合：

- benchmark CPU 與 observer CPU 不重疊；
- run parent 位於實際要評估的 WAL filesystem，且不在 repository 內；
- `perf stat` 與 `perf record -g` 可用；
- `mpstat`、`iostat`、`jq`、`fio`、GNU `time`、`taskset` 與 `sha256sum` 可用；
- Debug、ASan／UBSan、Release CTest 與 ReleaseBenchmark build 已通過；
- 正式執行不設定任何 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` 環境變數；
- campaign 開始後 binary、HEAD、cached diff、worktree diff 與 untracked files 不再改變。

若已選擇 full-attribution，但 host 無法提供 `perf` 權限、CPU affinity 或 block-device identity，該次結果應為
`environment-blocked`。可另外建立新的 run root，明確改走 4.2 affinity-only；不得在原 run 降低 gate、加入
hot-path telemetry，或把 affinity-only 結果標成完整歸因。

## 3. 測試前準備

### 3.0 安裝並驗證必要工具（Ubuntu 24.04）

先確認命令是否已存在；`ctest` 由 Ubuntu 的 `cmake` 套件一併提供，不是另一個套件。`fio` 只要求於
full-attribution：

```bash
command -v cmake
command -v ctest
command -v conan
command -v fio  # 僅 full-attribution
```

任一所選 mode 的必要命令缺少時，不得只沿用既有 benchmark binary 跳過 build／CTest gate。在本文件使用的
Ubuntu 24.04 host 上，以系統套件安裝 CMake、CTest、Ninja；full-attribution 另安裝 fio。Python virtual
environment 則用來隔離 Conan 2：

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends \
  cmake ninja-build python3-venv python3-pip
# 僅 full-attribution：
sudo apt-get install --no-install-recommends fio

CONAN_VENV=/home/neojhou/.venvs/order-books-conan2
python3 -m venv "$CONAN_VENV"
"$CONAN_VENV/bin/python" -m pip install --upgrade pip
"$CONAN_VENV/bin/python" -m pip install 'conan>=2,<3'
export PATH="$CONAN_VENV/bin:$PATH"
hash -r
```

若目前帳號沒有 sudo 權限，可在同一個 venv 內補裝 CMake／Ninja 以完成 configure、build 與 CTest gate；
這是 build-only fallback，不能取代需要系統 I/O 工具與目標 storage path 的 fio gate：

```bash
CONAN_VENV=/home/neojhou/.venvs/order-books-conan2
"$CONAN_VENV/bin/python" -m pip install 'cmake>=3.25' ninja
export PATH="$CONAN_VENV/bin:$PATH"
```

沒有 root 權限時，full-attribution 仍必須取得可執行的 `/usr/bin/fio`（請由系統管理者安裝 Ubuntu `fio`
套件）；未取得前，該 mode 必須維持 `environment-blocked`。Affinity-only 不執行 fio，不受此條件阻擋。

每個新的 shell 都必須先重新設定 Conan 環境：

```bash
CONAN_VENV=/home/neojhou/.venvs/order-books-conan2
test -x "$CONAN_VENV/bin/conan"
export PATH="$CONAN_VENV/bin:$PATH"
```

安裝後執行版本 gate：

```bash
cmake --version
ctest --version
ninja --version
conan --version
fio --version  # 僅 full-attribution
```

必須確認 CMake／CTest 至少為 3.25、Conan 顯示 2.x；full-attribution 另須確認 fio 顯示 `fio-3.x`。若
`fio` 顯示 Python Fiona 相關說明，代表 PATH 命中了同名但錯誤的程式，必須改用 Ubuntu `fio` 套件的
`/usr/bin/fio`。任何所選 mode 的必要安裝或版本 gate 失敗都分類為 `environment-blocked`，不得開始 campaign。

### 3.1 建置與 correctness gate

在 repository root 執行：

```bash
set -euo pipefail
REPO_ROOT=/home/neojhou/repos/order_books
CONAN_VENV=/home/neojhou/.venvs/order-books-conan2
export PATH="$CONAN_VENV/bin:$PATH"
cd "$REPO_ROOT"

conan profile detect --force
conan install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja
conan install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers

cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset release-benchmark
cmake --build --preset release-benchmark

bash -n benchmarks/run_engine_writer_diagnostics.sh
bash -n benchmarks/test_engine_writer_diagnostics_runner.sh
bash benchmarks/test_engine_writer_diagnostics_runner.sh
```

上述兩次 `conan install` 分別產生 Sanitizers／Debug 與 Release／ReleaseBenchmark presets 所需的 toolchain
及 GoogleTest dependency metadata。任何 dependency install、建置或測試失敗都必須先修正；不得以舊 binary
或失敗 binary 執行正式 campaign。若重新建置使 benchmark binary SHA-256 改變，full-attribution 必須重跑
3.3 的四組 WAL size probe，不得沿用先前的 `--fio-bs`；affinity-only 不需要該 probe。

### 3.2 選定 CPU 與 storage path

先記錄 CPU topology、NUMA 與目標 filesystem：

```bash
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
findmnt -T /home/neojhou
lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS,MODEL
```

以下範例使用 benchmark CPU `2-7`、observer CPU `0-1`；實際值必須依 host topology 選擇，兩者不得重疊。
`--cpu-list` 必須完全落在 benchmark CPU set 內。`RUN_PARENT` 必須位於目標 WAL filesystem，且有足夠空間
容納所有 benchmark data；只有 full-attribution 另需容納每個 fio block size 的 1 GiB control file。

### 3.3 取得 fio block size（僅 full-attribution）

Affinity-only 完全跳過本節，不建立 WAL size probe。Full-attribution 的 `--fio-bs` 必須來自目前 binary 的
WAL 實測資料，不可使用猜測值。因為正式 frontier 可能選到任一 group size，先在 repository 外的不同空目錄，
對 `256`、`1024`、`4096`、`8192` 各執行一次短 probe：

```bash
BENCHMARK_BINARY=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
PROBE_PARENT=$(mktemp -d /home/neojhou/order-books-wal-size-probes-XXXXXXXX)

for group in 256 1024 4096 8192; do
  probe_dir="$PROBE_PARENT/g${group}"
  mkdir -p "$probe_dir"
  "$BENCHMARK_BINARY" \
    --workload=wal_write_ceiling --wal-sync=none --wal-phase-profile=on \
    --wal-group-size="$group" --iterations=1000 --warmup=100 \
    --data-dir="$probe_dir" | tee "$PROBE_PARENT/g${group}.stdout"
done
```

`wal_write_ceiling` 的摘要目前不輸出 `wal_group_commits`；此 workload 的每個 measured group
對應一次 WAL append group，且摘要會輸出精確的 frame bytes 與 group counter。每個 probe 從最後一列取得
`profiled_frame_bytes` 與 `profiled_groups`，確認 `profiled_groups == iterations` 且兩者皆為正數，計算：

```text
fio_bs = ceil(profiled_frame_bytes / profiled_groups)
```

`wal_bytes_delta` 可能包含 segment header（發生 rotation 時不等於 payload frame bytes），不可用來直接推算
fio block size。將四個正整數以逗號串接；本次 binary 得到 `31232,124928,499712,999424`。probe 只決定 fio write
size，不列入任何 ceiling、latency 或瓶頸結論。若輸出缺少任一 frame/group counter、group 數不符或計算結果
不是正整數，停止，不得沿用其他 binary 的舊數值。

### 3.4 凍結 artifact identity

所有建置與所選 mode 必要的 probe 完成後才凍結 identity：

```bash
BENCHMARK_BINARY=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCHMARK_BINARY"
sha256sum "$BENCHMARK_BINARY"
git rev-parse HEAD
git status --short
git diff --cached --binary | sha256sum
git diff --binary | sha256sum
git ls-files --others --exclude-standard -z | LC_ALL=C sort -z | xargs -0 -r sha256sum | sha256sum
```

把輸出保存在 campaign notes。從這一步到 campaign 結束，不得編譯、編輯檔案、切換 branch、改變 staging
或替換 benchmark binary。

## 4. 執行方法

### 4.1 Full-attribution dry run

本節僅適用 full-attribution。Affinity-only 直接從 4.2.1 開始。先用實際路徑與 CPU set 驗證完整 runner
參數；dry run 不建立 run root，也不啟動 benchmark、perf 或 fio：

```bash
bash benchmarks/run_engine_writer_diagnostics.sh --dry-run \
  --binary=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --fio-bs=31232,124928,499712,999424 \
  --run-parent=/home/neojhou \
  --bench-cpus=2-7 --observer-cpus=0-1 --cpu-list=2,3,4,5,6,7 \
  --calibration-iterations=250000 --warmup=10000 \
  --case-timeout-seconds=300 --observer=on
```

確認 plan 包含
`preflight,calibration,observer_gate,component,wal,engine_frontier,tail_attribution,writer_profile,perf_record,fio`。

### 4.2 Affinity-only constrained campaign（不改 kernel policy）

若測試主機不允許調整 `perf_event_paranoid` 或授予 `CAP_PERFMON`，但仍要先取得接近 production scheduling
條件的 Engine ceiling，可執行本節。此模式只對本次啟動的 process 設定 CPU affinity，不修改 sysctl、CPU
governor、boost、IRQ、cpuset、cgroup、filesystem 或 production defaults，也不執行 `perf record` 或 fio。

這不是 4.3 的完整歸因 campaign。目前 `run_engine_writer_diagnostics.sh` 會在建立 run root 前無條件檢查
perf capability，且完整流程最後必定執行 fio；因此不可使用不存在的 `--perf=off`／`--fio=off` 參數，也不可
設定 `ENGINE_WRITER_DIAGNOSTICS_TEST_*` 繞過 gate。本節直接呼叫現有 benchmark binary，結果固定為
`inconclusive-attribution`，只能回答 sustainable Engine ceiling、latency 與 1M commands/s 差距；
`primary_bottleneck` 必須填 `unknown`。

本次開發機執行入口固定為本節：完成 3.0、3.1、3.2 與 3.4 後，依序執行 4.2.1 至 4.2.6；不執行
3.3 的 WAL size probe、4.1 的完整 runner dry run 或 4.3 的正式 unified runner。若未來 binary identity 改變，
affinity-only 只需重做 build／CTest 與本節 campaign，不需要為 fio 重新建立 probe。

CPU affinity 只限制 benchmark／observer 可在哪些 CPU 執行，不會獨占 CPU，也不會隔離 LLC、memory
bandwidth、thermal effect 或 storage queue。Engine workload 本身會持續寫 WAL 並 fsync；不得在承載 production
traffic 或與 production 共用目標 block device 的主機執行。建議使用相同規格的 staging／benchmark host；若
只能使用共享的非 production host，必須先安排測試時段並設定 300 秒 process timeout。

#### 4.2.1 建立外部 run root 並凍結 identity

build／CTest gate 與 3.4 完成後執行；從 identity before 到 after 之間不得編譯、編輯、切換 branch 或改變
staging：

```bash
set -euo pipefail
REPO_ROOT=/home/neojhou/repos/order_books
BENCHMARK_BINARY="$REPO_ROOT/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
RUN_PARENT=/home/neojhou
BENCH_CPUS=2-7
OBSERVER_CPUS=0-1
CPU_LIST=2,3,4,5,6,7
WARMUP=10000
POST_CASE_COOLDOWN_SECONDS=60

cd "$REPO_ROOT"
test -x "$BENCHMARK_BINARY"
RUN_ROOT=$(mktemp -d "$RUN_PARENT/engine-writer-affinity-only-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,derived,logs,monitors,preflight,time}

MOUNT_SOURCE=$(findmnt -no SOURCE -T "$RUN_ROOT")
BLOCK_DEVICE=$(lsblk -no PKNAME "$MOUNT_SOURCE" 2>/dev/null | awk 'NF {print; exit}')
test -n "$BLOCK_DEVICE"
test "$(df --output=avail -B1 "$RUN_ROOT" | tail -n 1)" -ge 21474836480

sha256sum "$BENCHMARK_BINARY" >"$RUN_ROOT/logs/binary-sha256-before.txt"
git rev-parse HEAD >"$RUN_ROOT/logs/head-before.txt"
git status --short >"$RUN_ROOT/logs/status-before.txt"
git diff --cached --binary | sha256sum >"$RUN_ROOT/logs/cached-diff-before.txt"
git diff --binary | sha256sum >"$RUN_ROOT/logs/worktree-diff-before.txt"
git ls-files --others --exclude-standard -z | LC_ALL=C sort -z \
  | xargs -0 -r sha256sum | sha256sum >"$RUN_ROOT/logs/untracked-before.txt"

printf 'run_root=%s\nmount_source=%s\nblock_device=%s\n' \
  "$RUN_ROOT" "$MOUNT_SOURCE" "$BLOCK_DEVICE" | tee "$RUN_ROOT/logs/run-context.txt"
```

20 GiB 是本流程的保守 free-space gate，不是 WAL capacity 結論。空間不足時應更換 `RUN_PARENT`，不可降低
門檻後繼續。所有 data、monitor 與 time artifacts 都留在 `RUN_ROOT`，不得寫入 repository。

#### 4.2.2 唯讀 preflight 與停止條件

initial preflight 及每個正式 case 前各執行 30 秒；observer 只讀取系統 counter，固定在 CPU 0–1：

```bash
run_preflight() {
  local label=$1
  local cpu_json="$RUN_ROOT/preflight/${label}-mpstat.json"
  local disk_json="$RUN_ROOT/preflight/${label}-iostat.json"
  local summary="$RUN_ROOT/preflight/${label}-summary.txt"

  taskset -c "$OBSERVER_CPUS" mpstat -P "$CPU_LIST" 1 30 -o JSON >"$cpu_json" &
  local cpu_pid=$!
  taskset -c "$OBSERVER_CPUS" iostat -y -dx "$BLOCK_DEVICE" 1 30 -o JSON >"$disk_json" &
  local disk_pid=$!
  wait "$cpu_pid"
  wait "$disk_pid"

  local idle_min iowait_p95 iowait_max util_avg aqu_p95 aqu_max
  idle_min=$(jq -r '
    [.sysstat.hosts[0].statistics[] | .["cpu-load"][]]
    | sort_by(.cpu) | group_by(.cpu)
    | map(map(.idle) | add / length) | min' "$cpu_json")
  iowait_p95=$(jq -r '
    [.sysstat.hosts[0].statistics[]
      | ([.["cpu-load"][] | .iowait] | add / length)]
    | sort
    | length as $count
    | .[(($count * 0.95 | ceil) - 1)]' "$cpu_json")
  iowait_max=$(jq -r '
    [.sysstat.hosts[0].statistics[]["cpu-load"][] | .iowait] | max' "$cpu_json")
  util_avg=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .util]
    | if length == 0 then null else add / length end' "$disk_json")
  aqu_p95=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]]
    | if length == 0 then null
      else sort
      | length as $count
      | .[(($count * 0.95 | ceil) - 1)]
      end' "$disk_json")
  aqu_max=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]]
    | if length == 0 then null else max end' "$disk_json")

  printf 'cpu_idle_min=%s\ncpu_iowait_p95=%s\ncpu_iowait_max=%s\ndisk_util_avg=%s\ndisk_aqu_p95=%s\ndisk_aqu_max=%s\n' \
    "$idle_min" "$iowait_p95" "$iowait_max" "$util_avg" "$aqu_p95" "$aqu_max" | tee "$summary"
  [[ "$idle_min" != null && "$iowait_p95" != null && "$iowait_max" != null &&
     "$util_avg" != null && "$aqu_p95" != null && "$aqu_max" != null ]]
  if awk -v value="$aqu_max" 'BEGIN { exit !(value > 0.25 && value <= 0.50) }'; then
    printf 'warning=disk_aqu_max_above_preferred_limit\n' | tee -a "$summary"
  fi
  awk -v value="$idle_min" 'BEGIN { exit !(value >= 90.0) }'
  awk -v value="$iowait_p95" 'BEGIN { exit !(value <= 5.0) }'
  awk -v value="$iowait_max" 'BEGIN { exit !(value <= 10.0) }'
  awk -v value="$util_avg" 'BEGIN { exit !(value <= 5.0) }'
  awk -v value="$aqu_p95" 'BEGIN { exit !(value <= 0.25) }'
  awk -v value="$aqu_max" 'BEGIN { exit !(value <= 0.50) }'
}

run_preflight initial
```

前一個 benchmark case 完成後，下一個 case 不得立即啟動 preflight。WAL 檔案雖已由 benchmark 關閉，Linux
仍可能在背景 writeback；因此 wrapper 在每個非 initial case 前先等待 `POST_CASE_COOLDOWN_SECONDS=60`，
再以 `pidof` 的精確 process name 檢查確認沒有殘留 `order_books_benchmark`、`mpstat`、`iostat`、`fio` 或 `perf`。這是
等待本次 campaign 自己的 I/O 尾端，不是降低 gate 或反覆抽樣；冷卻後的 preflight 仍只執行一次，失敗即停止。
若要延長冷卻時間，必須在 campaign notes 記錄固定值，不得針對單一 case 臨時調整。

`cpu_idle_min` 是各 benchmark CPU 在 30 秒內平均 idle 的最小值；`cpu_iowait_p95` 先對每個一秒 interval
計算所有 benchmark CPU 的平均 iowait，再取 30 個 interval 的 nearest-rank p95；`cpu_iowait_max` 則保留
所有單一 CPU／一秒樣本的絕對上限。Affinity-only preflight 的固定門檻為：

- `cpu_idle_min >= 90%`；
- `cpu_iowait_p95 <= 5%`；
- `cpu_iowait_max <= 10%`；
- `disk_util_avg <= 5%`；
- `disk_aqu_p95 <= 0.25`；
- `disk_aqu_max <= 0.50`。

`disk_aqu_p95` 是 30 個一秒 `aqu-sz` 樣本的 nearest-rank p95，作為持續 storage queue 的主要判定；
`disk_aqu_max` 只保留為瞬時尖峰的絕對上限。當 `disk_aqu_max` 落在 `(0.25, 0.50]` 且其他 gate
均通過時，summary 記錄 `disk_aqu_max_above_preferred_limit` warning，但不停止 run。超過 `0.50`，或
`disk_aqu_p95` 超過 `0.25`，才判為 `preflight-busy`。

這組規則只適用於 4.2 affinity-only，不改變 4.3 full-attribution runner 的 gate。門檻修改後必須建立新的
run root；先前因舊 preflight 規則停止的 run 不得回溯改判。

任一 preflight 不通過時，結果為 `preflight-busy`，停止整次 run；不可反覆執行直到偶然通過。執行中另有
下列任一情況時立即停止並保留 run root：benchmark timeout／非零退出、系統服務告警、可用空間低於 10 GiB、
observer 失敗、command/group counter 不符、replay/correctness 失敗或 identity 改變。CPU affinity 不能隔離
storage，因此 production 或共享 storage 出現 latency 告警時，不得以「benchmark CPU 已綁定」為由繼續。

#### 4.2.3 固定 Engine case wrapper

每個 case 使用獨立 data directory、300 秒 timeout、GNU time 及唯讀 mpstat/iostat。只有 tail validation case
開啟既有 EngineTailTelemetry；frontier ceiling case保持關閉：

```bash
run_engine_case() {
  local label=$1 iterations=$2 group=$3 delay_us=$4 tail_mode=${5:-off}
  local data_dir="$RUN_ROOT/data/$label"
  local -a tail_args=()
  mkdir -p "$data_dir"
  if [[ -e "$RUN_ROOT/logs/previous-case-complete" ]]; then
    sleep "$POST_CASE_COOLDOWN_SECONDS"
    ! pidof order_books_benchmark >/dev/null 2>&1
    ! pidof mpstat >/dev/null 2>&1
    ! pidof iostat >/dev/null 2>&1
    ! pidof fio >/dev/null 2>&1
    ! pidof perf >/dev/null 2>&1
  fi
  run_preflight "${label}-preflight" || return 20

  if [[ "$tail_mode" == on ]]; then
    tail_args=(
      "--engine-tail-telemetry-output=$RUN_ROOT/derived/${label}-engine-tail.csv"
      --engine-tail-state-sampling=on
    )
  fi

  taskset -c "$OBSERVER_CPUS" mpstat -P "$CPU_LIST" 1 \
    >"$RUN_ROOT/monitors/${label}-mpstat.txt" &
  local mpstat_pid=$!
  taskset -c "$OBSERVER_CPUS" iostat -y -dx "$BLOCK_DEVICE" 1 \
    >"$RUN_ROOT/monitors/${label}-iostat.txt" &
  local iostat_pid=$!

  local status=0
  /usr/bin/time -v -o "$RUN_ROOT/time/${label}.time" \
    taskset -c "$BENCH_CPUS" timeout 300 "$BENCHMARK_BINARY" \
      --workload=engine_durable_single_instrument \
      --iterations="$iterations" --warmup="$WARMUP" --data-dir="$data_dir" \
      --engine-group-size="$group" --engine-group-delay-us="$delay_us" \
      --engine-producer-lanes=8192 --wal-prepare-workers=2 \
      --wal-parallel-prepare-min-commands=4096 "${tail_args[@]}" \
      >"$RUN_ROOT/logs/${label}.stdout" 2>"$RUN_ROOT/logs/${label}.stderr" || status=$?

  kill "$mpstat_pid" "$iostat_pid" 2>/dev/null || true
  wait "$mpstat_pid" 2>/dev/null || true
  wait "$iostat_pid" 2>/dev/null || true
  printf '%s\n' "$status" >"$RUN_ROOT/logs/${label}.status"
  [[ "$status" == 0 ]] || return "$status"

  local row commands group_commands group_commits wal_bytes expected
  row=$(grep '^engine_durable_single_instrument ' "$RUN_ROOT/logs/${label}.stdout" | tail -n 1)
  commands=$(grep -o ' commands=[0-9]*' <<<"$row" | cut -d= -f2)
  group_commands=$(grep -o ' wal_group_commands=[0-9]*' <<<"$row" | cut -d= -f2)
  group_commits=$(grep -o ' wal_group_commits=[0-9]*' <<<"$row" | cut -d= -f2)
  wal_bytes=$(grep -o ' wal_bytes_delta=[0-9]*' <<<"$row" | cut -d= -f2)
  expected=$(awk -v value="$iterations" 'BEGIN { printf "%.0f\n", value * 2 }')
  [[ -n "$row" && "$commands" == "$expected" && "$group_commands" == "$expected" ]]
  awk -v commits="$group_commits" -v bytes="$wal_bytes" \
    'BEGIN { exit !(commits > 0 && bytes > 0) }'
  printf '%s\n' "$row" >"$RUN_ROOT/derived/${label}.row"
  : >"$RUN_ROOT/logs/previous-case-complete"
}
```

#### 4.2.4 三段 ramp 與正式 frontier

先只對 production default `g256-d200` 執行三段 ramp；每段都必須通過 wrapper 的 correctness／replay gate，
下一段才能開始：

```bash
run_engine_case ramp-g256-d200-i10000 10000 256 200 off
run_engine_case ramp-g256-d200-i50000 50000 256 200 off
run_engine_case ramp-g256-d200-i250000 250000 256 200 off
```

三段 ramp 僅為安全檢查，不列入 ceiling。通過後，對以下八個 case 各先執行 250,000 iterations calibration：

```text
g256-d200    g256-d1000
g1024-d200   g1024-d1000
g4096-d200   g4096-d1000
g8192-d200   g8192-d1000
```

每個 case 從摘要取得 `elapsed_ms`，依完整 runner 的同一公式建立 frozen plan：

```text
formal_iterations = ceil(250000 * 40000 / calibration_elapsed_ms * 1.10)
```

確認 calibration 的 `commands == 500000`，並將 case、calibration elapsed、formal iterations、預估 duration
與 binary SHA-256 寫入 `$RUN_ROOT/derived/engine-iteration-plan.tsv`。若預估 duration 接近 270 秒，視為
`invalid-run`，不可提高 timeout。

依 frozen plan 對八個 case 各執行三輪；奇數輪按上述順序，偶數輪反向。每輪都使用 4.2.3 wrapper，且
measured `elapsed_ms >= 30000`。每個 logical case 計算三輪 completed commands/s median 與 CV；只有
CV ≤ 5%、三輪 counter 正確且 identity 未改變的 case可列入 frontier。選擇 median completed RPS最高的有效
case作為 candidate；calibration、ramp、單輪最大值及 Writer service RPS不得替代 completed RPS ceiling。

#### 4.2.5 Async sustainability gate

對 candidate 執行 tail off/on 各三輪，順序交錯，iterations 必須沿用 frozen plan：

```bash
run_engine_case tail-r1-candidate-off FORMAL_ITERATIONS GROUP DELAY_US off
run_engine_case tail-r1-candidate-on  FORMAL_ITERATIONS GROUP DELAY_US on
```

第二輪先 on 後 off，第三輪再 off 後 on。tail-on 摘要與 CSV 必須同時滿足：

- `telemetry_dropped_samples == 0`；
- measured group/sample commands 與 Engine counters 一致；
- `drain_publisher_lag_events_last == 0`、bytes/age last 也為 0；
- measured state samples 至少 10 筆；
- lag events 最後四分位 median 不得高於第一四分位 median 加一個 group；
- 線性斜率外推 30 秒的正向 lag 增量不得超過一個 group；
- tail off/on 的 median RPS 與 CPU seconds/M commands bias 都不超過 3%。

任一 backlog／drain條件不通過時，candidate不是 sustainable ceiling；依 frontier排名測下一個candidate。
artifact缺失、counter錯誤或telemetry dropped則整次結果為`invalid-run`，不可把它當作candidate rejection。

#### 4.2.6 Identity after 與 affinity-only 報告

完成後重新取得與 4.2.1 相同的五項 identity，before／after 必須完全一致。報告另存為：

```text
docs/engine-writer-unified-ceiling-root-cause-analysis-affinity-only-report.md
```

報告開頭固定使用：

```yaml
evidence_mode: affinity-only
result: inconclusive-attribution | preflight-busy | invalid-run | environment-unstable
primary_bottleneck: unknown
perf_collected: false
perf_reason: kernel-policy-preserved
fio_collected: false
fio_reason: risk-constrained
kernel_policy_modified: false
cpu_affinity:
observer_affinity:
run_root:
binary_sha256:
repository_identity_match:
selected_case:
sustainable_engine_ceiling_rps:
gap_to_1m_percent:
```

正文只保留必要的 ramp gate、八組 frontier三輪數據、latency、command/WAL counters、CV、mpstat/iostat、
tail off/on bias與drain結果。即使phase、CPU usage或iostat呈現明顯相關性，仍只能列為`candidate_evidence`；
沒有perf call graph與same-path fio control時，不得填入primary bottleneck，也不得據此修改production code。

### 4.3 Full-attribution unified campaign

使用與 dry run 完全相同的參數，移除 `--dry-run`：

```bash
bash benchmarks/run_engine_writer_diagnostics.sh \
  --binary=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark \
  --fio-bs=31232,124928,499712,999424 \
  --run-parent=/home/neojhou \
  --bench-cpus=2-7 --observer-cpus=0-1 --cpu-list=2,3,4,5,6,7 \
  --calibration-iterations=250000 --warmup=10000 \
  --case-timeout-seconds=300 --observer=on
```

將 stdout 中的 `run_root=...` 記下。不得因中途慢輪或不理想結果重啟單一 case；任何 gate 失敗都保留整個
run root，依 terminal classification 結束該次 campaign。排除外部干擾後若要重跑，必須建立全新的 run
root，舊、新 raw rows 不得混合。

Runner 固定執行：

1. identity、工具、CPU affinity、perf capability 與 initial 30 秒 preflight；
2. 八個 Engine case calibration，產生 frozen iteration plan；
3. observer off/on 各三輪 overhead gate；
4. 四個 component stage 各五輪；
5. 四種 WAL group size × `sync=none/per_group` 各五輪；
6. 八個 Engine group size／delay case 各三輪 frontier scan；
7. 依 median RPS 由高到低執行 tail sustainability gate；
8. production default、`g4096-d1000` 與 selected case 各五輪確認，重複身份只執行一次；
9. Writer profile off/on、獨立 perf record/report；
10. 相同 filesystem、buffered write、queue depth 1、每次 write 後 fsync 的 fio control。

### 4.4 Terminal classification 與停止規則

| 結果 | 處理方式 |
| --- | --- |
| `environment-blocked` | 修正所選 mode 必要的工具、權限、CPU 或 storage identity 後，以新 run root 重跑全部；full-attribution 也可另開新的 affinity-only run |
| `preflight-busy` | 移除外部負載後，以新 run root 重跑全部；不可降低 idle gate |
| `observer-biased` | 降低外部 observer 開銷或更換 host；不可使用該次 ceiling |
| `invalid-run` | 依 `reason` 修正 correctness、timeout、counter 或 artifact 問題後重跑全部 |
| `environment-unstable` | 更換或隔離 host/storage 後重跑全部 |
| `inconclusive-attribution` | 可保留有效 ceiling，但不得宣稱 primary bottleneck |
| `valid-attribution` | ceiling 與瓶頸證據皆完整，可提出下一個最小 production change |

Runner 完成資料收集時可能先輸出 `inconclusive-attribution`；只有完成人工交叉分析並取得兩種獨立證據後，
最終報告才可將結果提升為 `valid-attribution`。

## 5. Artifact 完整性檢查

以下完整 artifact 清單適用於 4.3 正式 unified campaign。4.2 affinity-only 模式只要求：identity
before／after、run context、frozen iteration plan、每個 ramp／calibration／frontier／tail case 的 status、stdout、
stderr、GNU time、mpstat、iostat、normalized row，以及 tail-on CSV／validation。該模式不產生 component、direct
WAL、perf 或 fio artifact；報告必須明確標記為 policy/risk constrained，不能把它們寫成遺失後再以人工值補齊。
任一 affinity-only 必要 artifact 缺少時仍為 `invalid-run`。

正式分析前必須確認下列資料存在且非空：

- `logs/result.txt`；
- `logs/repository-identity-before.txt`、`repository-identity-after.txt`；
- `logs/binary-sha256-before.txt`、`binary-sha256-after.txt`；
- `logs/environment-before.txt` 與 CPU policy before／after；
- `derived/engine-iteration-plan.tsv`；
- `derived/frontier-selection.tsv` 與 `frontier-selection.txt`；
- 每個正式 case 的 status、stdout、stderr、GNU time 與 normalized row；
- observer-on case 的 perf stat、mpstat 與 iostat；
- 每個執行過的 candidate 的 tail validation 與 summary；
- selected case 的 perf data 與 `perf report --stdio`；
- 每個 fio block size 的 JSON。

另外確認：

- before／after binary SHA-256 與 repository identity 完全一致；
- `frontier-selection.tsv` 每列固定六欄；
- formal Engine case 有 `commands == formal_iterations * 2`、
  `wal_group_commands == commands`、`wal_group_commits > 0`、`wal_bytes_delta > 0`；
- 每個有效 Engine round 的 measured duration 至少 30 秒；
- selected case 的 Publisher／Completion backlog 在 drain 後歸零，measured window 無持續正斜率；
- observer 與 tail off/on 的 throughput 或 CPU seconds/M bias 均不超過 3%；
- frontier case CV 不超過 5%。

任一必要 artifact 缺少、格式錯誤或 identity 改變，整次結果為 `invalid-run`，不得以人工補值。

## 6. 數據整理與根因判定

4.2 affinity-only 模式只整理完整 Engine completed RPS、latency、GNU time CPU seconds、WAL/group counters、
mpstat／iostat 與 async sustainability；不得製造缺少的 perf/fio ratio，也不得用 component 或舊 campaign 數據
補足本次歸因。其根因結論固定為 `inconclusive-attribution`／`primary_bottleneck: unknown`。

### 6.1 不可混用的數據

以下結果必須分表，不能互相替代：

- component microbenchmark RPS；
- direct WAL RPS；
- Writer profile RPS；
- tail-on attribution RPS；
- fio bandwidth／IOPS；
- full Engine completed commands/s。

Engine ceiling只取通過 correctness、duration、CV、artifact、tail sustainability 與 drain gate的case，以三輪
frontier completed RPS median排序；單輪最大值、calibration與profile run不列入ceiling。

### 6.2 必要計算

至少計算並列出：

```text
cpu_seconds_per_million = (user_seconds + system_seconds) / commands * 1,000,000
commands_per_fsync      = wal_group_commands / wal_group_commits
wal_bytes_per_command   = wal_bytes_delta / wal_group_commands
wal_bytes_per_second    = wal_bytes_delta / measured_seconds
fsync_per_second        = wal_group_commits / measured_seconds
engine_to_fio_bw_ratio  = engine_wal_bytes_per_second / fio_write_bytes_per_second
engine_to_fio_sync_ratio = engine_fsync_per_second / fio_fsync_per_second
```

若 Writer phase有完整的per-command CPU時間，可另算：

```text
cpu_writer_ceiling_rps = 1,000,000,000 / writer_cpu_ns_per_command
storage_bandwidth_ceiling_rps = fio_write_bytes_per_second / wal_bytes_per_command
storage_flush_ceiling_rps = fio_fsync_per_second * commands_per_fsync
```

phase accounting 的總和若未覆蓋至少 90% Writer service wall time，差額必須列為 `unaccounted`，不可分攤到
任意 phase。

### 6.3 瓶頸證據門檻

Primary bottleneck 必須由至少兩種獨立證據支持，且在多數重複輪次同方向：

| 分類 | 必要交叉證據 |
| --- | --- |
| Writer CPU | Writer接近單core容量；perf hotspot與phase一致；storage仍有餘裕 |
| prepare CPU | helper CPU／runqueue接近飽和；prepare phase與perf hotspot一致 |
| metrics | perf落在MetricsRegistry／map／mutex；metrics component與post-apply share一致 |
| storage bandwidth | Engine bytes/s接近same-path fio；append/write phase與iostat一致 |
| durable flush | sync phase／tail主導；commands/fsync × fio fsync/s可解釋Engine ceiling |
| scheduler／host | runqueue、migration或host activity與慢輪共變；isolated ceiling沒有同樣限制 |

只有單一 phase timer、單次 perf sample、device util或vendor datasheet時，結論必須維持
`inconclusive-attribution`。

## 7. 報告檔案與固定格式

本節格式適用於 4.3 完整 campaign；4.2 affinity-only 使用 4.2.6 指定的獨立檔名與縮減格式，兩種結果不得
合併成同一份有效歸因報告。

將結果寫入：

```text
docs/engine-writer-unified-ceiling-root-cause-analysis-report.md
```

報告開頭固定使用：

```yaml
result: valid-attribution | environment-blocked | preflight-busy | observer-biased | invalid-run | environment-unstable | inconclusive-attribution
run_root:
commit:
binary_sha256:
cached_diff_sha256:
worktree_diff_sha256:
untracked_manifest_sha256:
host:
kernel:
compiler_and_flags:
cpu_affinity:
observer_affinity:
numa_policy:
filesystem_and_mount:
wal_device_and_model:
io_scheduler:
declared_storage_limit_source:
perf_capability:
collection_status:
engine_iteration_plan:
fio_block_sizes:
```

正文固定依下列順序撰寫。

### 7.1 執行摘要

- 測試目標與固定 workload；
- terminal classification；
- observed sustainable Engine ceiling、相對 1M commands/s 的差距；
- primary bottleneck，或無法判定的明確原因；
- 下一個最小production change，或 `no production change`。

### 7.2 Validity gates

| Gate | 結果 | 數據／artifact | 判定 |
| --- | --- | --- | --- |
| Artifact identity |  |  | pass/fail |
| Initial／per-case preflight |  |  | pass/fail |
| Observer overhead | RPS bias；CPU bias |  | pass/fail |
| Correctness／replay／durability |  |  | pass/fail |
| Duration／CV／counter |  |  | pass/fail |
| Async backlog／drain |  |  | pass/fail |

### 7.3 Component ceiling

| Stage | Rounds | Median commands/s | CPU s/M commands | p99 | CV | 備註 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| state_machine |  |  |  |  |  |  |
| invariant_validation |  |  |  |  |  |  |
| metrics |  |  |  |  |  |  |
| runtime_handoff |  |  |  |  |  |  |

### 7.4 WAL ceiling

| Sync | Group | Median commands/s | CPU s/M | MiB/s | commands/fsync | fsync/s | p99.9 sync | CV |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

另列prepare、plan/copy、write、publish、sync phase及replay verification；`sync=none`的rotation sync不得被
描述成zero-fsync。

### 7.5 Engine frontier與Pareto結果

| Group | Delay us | Median completed RPS | p50 us | p99 us | p99.9 us | max us | CV | Sustainable | Exclusion reason |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |

指出selected case、production default與`g4096-d1000`五輪確認結果。另列writer service RPS與completed
RPS；兩者不同時不可只呈現較高者。

### 7.6 Writer CPU與phase accounting

| Case | Profile mode | Writer CPU | Prepare helper CPU | Apply | Validation | WAL prepare | Append/write | Sync | Metrics/post-apply | Notify/enqueue | Unaccounted |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

列出profile off/on bias，超過3%時將phase結果標示為定性資料。

### 7.7 Perf、I/O與storage ceiling

- `perf stat`：task-clock、cycles、instructions、IPC、cache misses、context switches、migrations；
- `perf report`：top call stacks及其thread role；
- `iostat`：bytes/s、IOPS、await、util、aqu-sz；
- Engine：WAL bytes/s、fsync/s、commands/fsync與sync tail；
- fio：每個block size的write bytes/s、fsync/s與latency；
- Engine/fio bandwidth及sync ratio；
- vendor／cloud上限來源與日期，未知時填`unknown`。

### 7.8 Async sustainability

列出Publisher lag events／bytes／age、Completion queue depth、measured slope、drain終值、tail off/on bias及
dropped samples。若backlog不bounded，只能報Writer瞬時ceiling，不能報sustainable Engine ceiling。

### 7.9 根因結論

固定使用以下格式：

```text
classification:
primary_bottleneck:
evidence_1:
evidence_2:
counter_evidence:
confidence:
ceiling_rps:
gap_to_1m_percent:
next_minimum_change:
```

每項證據需指向具體artifact與數值。若沒有兩種獨立證據，`primary_bottleneck`填`unknown`、結果填
`inconclusive-attribution`，下一步只允許補足缺少的證據，不直接修改production。

### 7.10 限制

記錄host共享程度、storage virtualisation、thermal／frequency狀態、perf sampling限制、fio與實際WAL語意
差異，以及本次只涵蓋單instrument／單shard crossing-pair workload。不得把結果外推成多instrument或
production SLA。

## 8. 完成後的唯讀檢查

報告完成後只執行：

```bash
git diff --check
git diff --cached --check
git status --short
```

不得為了執行、整理或驗證壓測而執行 `git add`、`git reset`、`git restore --staged`、`git commit` 或其他
staging mutation。
