# WAL fsync 攤提上限壓測操作與報告規格

## 1. 目的與停止規則

本文件執行 `docs/wal-fsync-amortization-frontier-design.md` 定義的固定矩陣，確認 bounded parallel
prepare 啟用後，group size 是否能降低每個 command 的 durable fsync 成本。這是 ceiling diagnostic，
不是 production deployment canary，也不直接改變 runtime default。

本流程只測單一 instrument／shard，保持 per-group `fsync`、WAL format、recovery 與 durable callback
語意，重用既有 benchmark 與 collector-only telemetry，並保留所有慢輪與 storage tail。禁止執行
`git add`、`git reset`、`git restore`、`git commit` 或其他改變 staging 的操作；禁止調整 governor、
boost、I/O scheduler、mount option、sysctl、NTP 或 page cache。

正式矩陣開始後，不因結果不理想而增加 group、追加 round 或更換參數。執行前的 setup failure 只能
建立新的 `RUN_ROOT` 修正後重來，不得混合不同 attempt 的正式輪。

結果另寫入：

```text
docs/wal-fsync-amortization-frontier-benchmark-report.md
```

## 2. 固定矩陣與測試語意

### 2.1 共同設定

| 參數 | 固定值 |
| --- | --- |
| workload | `engine_durable_single_instrument`、`wal_write_ceiling` |
| instrument / shard | 1 / 1 |
| fsync mode | `per_group` |
| group sizes | 4096、8192、16384 |
| prepare workers W | 1、2、4 |
| parallel threshold | 4096 |
| Engine producer lanes | 16384（所有 Engine case 相同） |
| Engine group delay | 1000 us |
| telemetry | Engine collector-only；state sampling off |
| CPU affinity | 全部 case 相同，預先記錄 |
| build | 同一 `ReleaseBenchmark` binary |
| formal rounds | 每 case 5 輪 |
| measured phase | 每輪至少 20 秒 |

W=1 是 baseline，W=2 是唯一可進入後續 fixed-rate canary 評估的 candidate，W=4 只作 diagnostic。
`wal_write_ceiling` 的 latency 是 WAL group latency；Engine 的 latency 是 durable completion latency，
兩者不得在報告中混成同一欄。

### 2.2 Run 數量

```text
18 smoke  = 2 workloads × 3 groups × 3 W
18 pilot  = 2 workloads × 3 groups × 3 W
90 formal = 2 workloads × 3 groups × 3 W × 5 rounds
```

smoke 與 pilot 不納入正式 aggregate。每個 case 需有獨立 data directory、stdout、stderr、status、
`time -v` 與 telemetry/artifact 路徑。

### 2.3 必須保留的統計

每輪至少記錄：

```text
workload, group_size, prepare_workers, parallel_threshold, producer_lanes, group_delay_us
commands_per_second, elapsed_ms, wal_mib_per_second
wal_group_commands, wal_group_commits, actual_commands_per_group
syncs_per_second, commands_per_sync
Engine: completion p50/p99/p99.9/max
WAL: group p50/p99/p99.9/max
Engine telemetry: measured fsync p50/p99/p99.9/max/total and >25/>100/>250 ms counts
actual_parallel_prepare_groups, actual_prepare_tasks
segment rotations, WAL byte delta, correctness/replay result
CPU, maximum RSS, voluntary/involuntary context switches
```

`syncs_per_second = wal_group_commits / measured_seconds`；`commands_per_sync = wal_group_commands /
wal_group_commits`。兩個值都使用 measured phase 的 delta，不能使用包含 warmup 或 drain 的 full-run
counter。

## 3. Build、correctness 與唯讀身份檢查

在 repository root 執行。若專案環境使用 wrapper，將 `TOOLS` 指向既有 wrapper 目錄；不要安裝或
改寫 repository 內檔案。

```bash
set -o pipefail
TOOLS=/tmp/order_books-tools/bin
CONAN="$TOOLS/conan"
CMAKE="$TOOLS/cmake"
CTEST="$TOOLS/ctest"
test -x "$CONAN" && test -x "$CMAKE" && test -x "$CTEST"

"$CONAN" install . --build=missing -s build_type=Release \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset release-benchmark
"$CMAKE" --build --preset release-benchmark
"$CTEST" --test-dir build/ReleaseBenchmark --output-on-failure

"$CONAN" install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset debug
"$CMAKE" --build --preset debug
"$CTEST" --preset debug --output-on-failure

"$CMAKE" --preset sanitizers
"$CMAKE" --build --preset sanitizers
"$CTEST" --preset sanitizers --output-on-failure

BENCH_BIN=/home/neojhou/repos/order_books/build/ReleaseBenchmark/benchmarks/order_books_benchmark
test -x "$BENCH_BIN"
```

任一 correctness gate 失敗即停止。ASan/UBSan binary 只作 correctness，不作效能矩陣。TSan 若因
runtime mapping error 無法執行，記錄環境限制，不宣稱 TSan 通過或失敗。`git status`、`git diff` 與
hash 命令都是唯讀；本流程禁止任何會改變 index 的 Git 命令。

## 4. 環境與 artifact root

`RUN_PARENT` 必須位於要測的實體 filesystem。WAL、artifact 與 benchmark binary 的 filesystem 資訊
均須記錄；若 `findmnt` 顯示 dm-crypt、LVM、mdraid 或多個 leaf device，報告列出完整 topology。

```bash
set -o pipefail
BENCH_CPU_SET=2-7
CASE_TIMEOUT_SECONDS=300
INITIAL_ITERATIONS=400000
INITIAL_WAL_COMMANDS=1048576
SMOKE_WAL_COMMANDS=163840
RUN_PARENT=/home/neojhou
test -d "$RUN_PARENT"

for tool in /usr/bin/time timeout taskset findmnt lsblk; do
  command -v "$tool" >/dev/null || exit 1
done
taskset -c "$BENCH_CPU_SET" true

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-fsync-amortization-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,logs,time,telemetry,derived}

{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  sha256sum docs/wal-fsync-amortization-frontier-design.md \
    docs/wal-fsync-amortization-frontier-benchmark-procedure.md "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-before.txt"

{
  date --utc --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  timedatectl show 2>&1 || true
} > "$RUN_ROOT/logs/environment.txt" 2>&1

findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS -T "$RUN_ROOT" > "$RUN_ROOT/logs/filesystem.txt"
WAL_SOURCE=$(findmnt -no SOURCE -T "$RUN_ROOT")
lsblk -s -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS "$WAL_SOURCE" \
  > "$RUN_ROOT/logs/block-topology.txt"
```

建議預留至少 30 GiB。每輪建立新的 data directory；不得清 page cache、刪除慢輪或重用失敗輪的 data
directory。正式矩陣結束後、建立 report 前，再寫一份 `source-identity-after.txt`。若 HEAD、cached
diff、tracked worktree diff、兩份 input document、binary 或固定環境改變，整個矩陣不得合併。

## 5. 通用執行函式

以下函式只使用明確的 case path；不使用 `killall`、`pkill` 或模糊 process matching。函式中的
`workload` 只能是 `engine_durable_single_instrument` 或 `wal_write_ceiling`。

```bash
run_case() {
  phase=$1
  case_name=$2
  workload=$3
  group_size=$4
  workers=$5
  iterations=$6
  warmup=$7
  case_data="$RUN_ROOT/data/$phase-$case_name"
  case_stdout="$RUN_ROOT/logs/$phase-$case_name.stdout"
  case_stderr="$RUN_ROOT/logs/$phase-$case_name.stderr"
  case_time="$RUN_ROOT/time/$phase-$case_name.time"
  case_status="$RUN_ROOT/logs/$phase-$case_name.status"
  test ! -e "$case_data" && test ! -e "$case_stdout" && test ! -e "$case_status" || return 2

  args=(--workload="$workload" --iterations="$iterations" --warmup="$warmup"
        --wal-prepare-workers="$workers"
        --wal-parallel-prepare-min-commands=4096
        --data-dir="$case_data")
  if test "$workload" = engine_durable_single_instrument; then
    telemetry="$RUN_ROOT/telemetry/$phase-$case_name.csv"
    args+=(--engine-group-size="$group_size" --engine-group-delay-us=1000
           --engine-producer-lanes=16384
           --engine-tail-telemetry-output="$telemetry"
           --engine-tail-state-sampling=off)
  else
    args+=(--wal-group-size="$group_size" --wal-sync=per_group --wal-phase-profile=off)
  fi

  printf 'workload=%s group_size=%s workers=%s iterations=%s warmup=%s\n' \
    "$workload" "$group_size" "$workers" "$iterations" "$warmup" \
    > "$RUN_ROOT/logs/$phase-$case_name.meta"
  /usr/bin/time -v -o "$case_time" \
    timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
    taskset -c "$BENCH_CPU_SET" "$BENCH_BIN" "${args[@]}" \
    > "$case_stdout" 2> "$case_stderr"
  status=$?
  printf '%s\n' "$status" > "$case_status"
  return "$status"
}
```

不要把 `run_case` 接在會覆蓋 benchmark exit status 的 `tee` pipeline 後；需要即時觀看時，另開唯讀
`tail -f`。Shell 沒有 process substitution 時也應直接寫 stdout/stderr，不能犧牲 exit status。

## 6. Smoke 與 pilot

### 6.1 Smoke

每個 `(workload, group, W)` 執行一次 smoke。Engine 使用 warmup=1000；direct WAL 使用 warmup=100，
避免把 10,000 個 WAL groups（且每個 group 都包含 per-group fsync）誤當成一般 command warmup。
Engine 使用 10000 iterations；direct WAL 使用 `SMOKE_WAL_COMMANDS / group_size` iterations，讓三個
group 的 smoke command budget 相同。
下列 loop 產生固定的 18 個 smoke：

```bash
for workload in engine_durable_single_instrument wal_write_ceiling; do
  for group_size in 4096 8192 16384; do
    for workers in 1 2 4; do
      if test "$workload" = engine_durable_single_instrument; then
        smoke_iterations=10000
      else
        smoke_iterations=$((SMOKE_WAL_COMMANDS / group_size))
      fi
      if test "$workload" = engine_durable_single_instrument; then
        smoke_warmup=1000
      else
        smoke_warmup=100
      fi
      run_case smoke "${workload}-g${group_size}-w${workers}" \
        "$workload" "$group_size" "$workers" "$smoke_iterations" "$smoke_warmup" || exit 1
    done
  done
done
```

每個 smoke 必須 exit 0。Engine case 必須通過 summary、completion、durable head、WAL replay 與
telemetry validation；WAL case 必須通過 WAL byte／sequence／replay validation。W=1 的 parallel
groups 必須為 0；W=2／W=4 在 group >= threshold 時必須大於 0。任何 smoke 不符合即停止，不進入
pilot 或正式矩陣。

### 6.2 Pilot

每個 Engine `(group, W)` 以 `INITIAL_ITERATIONS=400000`、warmup=10000 執行一次；每個 direct WAL
`(group, W)` 以 `INITIAL_WAL_COMMANDS / group_size` iterations、warmup=100 執行一次。pilot 只用來
決定同一 workload 的共同正式負載，不納入 aggregate：

```text
ENGINE_FORMAL_ITERATIONS = max over 9 Engine pilots of
  ceil(400000 * 20000 / pilot_elapsed_ms)

WAL_FORMAL_COMMANDS = max over 9 WAL pilots of
  ceil(1048576 * 20000 / pilot_elapsed_ms), rounded up to a multiple of 16384
```

Engine 的 9 個 group/W case 使用同一 `ENGINE_FORMAL_ITERATIONS`；WAL 的每個 case 使用
`WAL_FORMAL_COMMANDS / group_size` iterations。兩個值必須為正整數，WAL budget 必須可被 16384
整除。若估算值為 0、overflow、不可整除、或以該值執行的第一輪短於 15 秒，放棄該 RUN_ROOT，增加
共同 budget 並從新的 RUN_ROOT 重新開始。

執行 pilot 的固定命令模式如下；Engine 的 `pilot_iterations=400000`、warmup=10000，direct WAL
的 iterations 由 `INITIAL_WAL_COMMANDS / group_size` 計算、warmup=100：

```bash
for workload in engine_durable_single_instrument wal_write_ceiling; do
  for group_size in 4096 8192 16384; do
    for workers in 1 2 4; do
      if test "$workload" = engine_durable_single_instrument; then
        pilot_iterations=$INITIAL_ITERATIONS
      else
        pilot_iterations=$((INITIAL_WAL_COMMANDS / group_size))
      fi
      if test "$workload" = engine_durable_single_instrument; then
        pilot_warmup=10000
      else
        pilot_warmup=100
      fi
      run_case pilot "${workload}-g${group_size}-w${workers}" \
        "$workload" "$group_size" "$workers" "$pilot_iterations" "$pilot_warmup" || exit 1
    done
  done
done
```

從每個 pilot summary 的 `elapsed_ms` 計算 `ENGINE_FORMAL_ITERATIONS` 與
`WAL_FORMAL_COMMANDS`，並將值寫入 `$RUN_ROOT/logs/formal-iterations.txt`。不要使用不同 group 的
不同正式 workload budget。

## 7. 正式 90 輪

每個 case 固定五輪。輪序必須在執行前固定；以下順序以 group、workload、W 交錯，避免先完成的
case 總是在同一個 storage 狀態：

```text
r1: E4096W1, W4096W1, E8192W2, W8192W2, E16384W4, W16384W4,
    E4096W2, W4096W2, E8192W1, W8192W1, E16384W2, W16384W2,
    E4096W4, W4096W4, E8192W4, W8192W4, E16384W1, W16384W1
r2: E8192W1, W8192W1, E16384W2, W16384W2, E4096W4, W4096W4,
    E8192W2, W8192W2, E16384W1, W16384W1, E4096W2, W4096W2,
    E8192W4, W8192W4, E16384W4, W16384W4, E4096W1, W4096W1
r3: E16384W1, W16384W1, E4096W2, W4096W2, E8192W4, W8192W4,
    E16384W2, W16384W2, E4096W1, W4096W1, E8192W2, W8192W2,
    E16384W4, W16384W4, E4096W4, W4096W4, E8192W1, W8192W1
r4: E4096W1, W4096W1, E8192W4, W8192W4, E16384W2, W16384W2,
    E4096W2, W4096W2, E8192W1, W8192W1, E16384W4, W16384W4,
    E4096W4, W4096W4, E8192W2, W8192W2, E16384W1, W16384W1
r5: E8192W2, W8192W2, E16384W4, W16384W4, E4096W1, W4096W1,
    E8192W1, W8192W1, E16384W2, W16384W2, E4096W4, W4096W4,
    E8192W4, W8192W4, E16384W1, W16384W1, E4096W2, W4096W2
```

`E` 是 Engine、`W` 是 direct WAL；後綴是 prepare workers。每個 token 使用該 workload 的共同
formal iterations，呼叫第 5 節 `run_case formal ...`。每輪完成後立即唯讀驗證 status、summary、
counter、telemetry 與 replay；失敗仍保留 artifact，並依第 8 節處理，不以重跑取代。

先由 pilot 結果決定 `WAL_FORMAL_COMMANDS`，再計算每個 group 的 iterations：

```bash
WAL_ITER_4096=$((WAL_FORMAL_COMMANDS / 4096))
WAL_ITER_8192=$((WAL_FORMAL_COMMANDS / 8192))
WAL_ITER_16384=$((WAL_FORMAL_COMMANDS / 16384))
test "$((WAL_ITER_4096 * 4096))" -eq "$WAL_FORMAL_COMMANDS"
test "$((WAL_ITER_8192 * 8192))" -eq "$WAL_FORMAL_COMMANDS"
test "$((WAL_ITER_16384 * 16384))" -eq "$WAL_FORMAL_COMMANDS"
printf 'ENGINE_FORMAL_ITERATIONS=%s WAL_FORMAL_COMMANDS=%s\n' \
  "$ENGINE_FORMAL_ITERATIONS" "$WAL_FORMAL_COMMANDS" \
  > "$RUN_ROOT/logs/formal-iterations.txt"
```

token 對應的唯一命令模板如下；依上表每一輪從左到右呼叫，不能以結果排序：

```bash
run_token() {
  round=$1
  token=$2
  case_name="${round}-${token}"
  case "$token" in
    E4096W1)  run_case formal "$case_name" engine_durable_single_instrument 4096 1 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E4096W2)  run_case formal "$case_name" engine_durable_single_instrument 4096 2 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E4096W4)  run_case formal "$case_name" engine_durable_single_instrument 4096 4 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E8192W1)  run_case formal "$case_name" engine_durable_single_instrument 8192 1 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E8192W2)  run_case formal "$case_name" engine_durable_single_instrument 8192 2 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E8192W4)  run_case formal "$case_name" engine_durable_single_instrument 8192 4 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E16384W1) run_case formal "$case_name" engine_durable_single_instrument 16384 1 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E16384W2) run_case formal "$case_name" engine_durable_single_instrument 16384 2 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    E16384W4) run_case formal "$case_name" engine_durable_single_instrument 16384 4 "$ENGINE_FORMAL_ITERATIONS" 10000 ;;
    W4096W1)  run_case formal "$case_name" wal_write_ceiling 4096 1 "$WAL_ITER_4096" 100 ;;
    W4096W2)  run_case formal "$case_name" wal_write_ceiling 4096 2 "$WAL_ITER_4096" 100 ;;
    W4096W4)  run_case formal "$case_name" wal_write_ceiling 4096 4 "$WAL_ITER_4096" 100 ;;
    W8192W1)  run_case formal "$case_name" wal_write_ceiling 8192 1 "$WAL_ITER_8192" 100 ;;
    W8192W2)  run_case formal "$case_name" wal_write_ceiling 8192 2 "$WAL_ITER_8192" 100 ;;
    W8192W4)  run_case formal "$case_name" wal_write_ceiling 8192 4 "$WAL_ITER_8192" 100 ;;
    W16384W1) run_case formal "$case_name" wal_write_ceiling 16384 1 "$WAL_ITER_16384" 100 ;;
    W16384W2) run_case formal "$case_name" wal_write_ceiling 16384 2 "$WAL_ITER_16384" 100 ;;
    W16384W4) run_case formal "$case_name" wal_write_ceiling 16384 4 "$WAL_ITER_16384" 100 ;;
    *) return 2 ;;
  esac
}

ROUND1=(E4096W1 W4096W1 E8192W2 W8192W2 E16384W4 W16384W4
         E4096W2 W4096W2 E8192W1 W8192W1 E16384W2 W16384W2
         E4096W4 W4096W4 E8192W4 W8192W4 E16384W1 W16384W1)
ROUND2=(E8192W1 W8192W1 E16384W2 W16384W2 E4096W4 W4096W4
         E8192W2 W8192W2 E16384W1 W16384W1 E4096W2 W4096W2
         E8192W4 W8192W4 E16384W4 W16384W4 E4096W1 W4096W1)
ROUND3=(E16384W1 W16384W1 E4096W2 W4096W2 E8192W4 W8192W4
         E16384W2 W16384W2 E4096W1 W4096W1 E8192W2 W8192W2
         E16384W4 W16384W4 E4096W4 W4096W4 E8192W1 W8192W1)
ROUND4=(E4096W1 W4096W1 E8192W4 W8192W4 E16384W2 W16384W2
         E4096W2 W4096W2 E8192W1 W8192W1 E16384W4 W16384W4
         E4096W4 W4096W4 E8192W2 W8192W2 E16384W1 W16384W1)
ROUND5=(E8192W2 W8192W2 E16384W4 W16384W4 E4096W1 W4096W1
         E8192W1 W8192W1 E16384W2 W16384W2 E4096W4 W4096W4
         E8192W4 W8192W4 E16384W1 W16384W1 E4096W2 W4096W2)

: > "$RUN_ROOT/logs/formal-failures.txt"
for round_tokens in ROUND1 ROUND2 ROUND3 ROUND4 ROUND5; do
  eval "tokens=(\"\${${round_tokens}[@]}\")"
  for token in "${tokens[@]}"; do
    if ! run_token "$round_tokens" "$token"; then
      printf '%s\n' "${round_tokens}-${token}" >> "$RUN_ROOT/logs/formal-failures.txt"
    fi
  done
done
```

`eval` 只讀取上方五個固定陣列；若執行環境禁止 `eval`，直接以五段 `for token in "${ROUNDn[@]}"`
取代，不能從檔名或 benchmark output 動態產生順序。

正式輪的單一失敗不應被 shell 靜默吞掉；`formal-failures.txt` 非空時，報告必須列出每個失敗
case與原因，並將受影響 aggregate 標為 `invalid` 或 `resource-limited`。不得用另一輪取代失敗輪。

## 8. 有效性與錯誤處理

### 8.1 Engine case

只有同時符合下列條件才可納入 aggregate：

- status 0、未 timeout；
- measured commands、group commands、completion count 一致；
- `wal_group_commits > 0`，`measured_sync_count == wal_group_commits`；
- `measured_group_sample_commands == wal_group_commands`；
- W=1 的 `actual_parallel_prepare_groups == 0`；W=2／W=4 的 group >=4096 時大於 0；
- WAL reopen/replay、durable head、expected EngineSeq 與 empty-book 驗證成功；
- telemetry dropped samples、sampler error、aggregate overflow 皆為 0/false；
- measured phase 至少 20 秒（第一輪若短於 15 秒，整個 workload 矩陣重來）。

### 8.2 Direct WAL case

只有同時符合下列條件才可納入 aggregate：

- status 0、未 timeout；
- measured group count、group commands 與 expected commands 一致；
- `wal_sync=per_group`，W=1 parallel groups 為 0、W=2／W=4 為正值；
- WAL byte delta、segment rotation、sequence continuity 與 replay verification 成功；
- measured phase 至少 20 秒。

### 8.3 失敗分類

- correctness、counter、telemetry 或 replay 失敗：`invalid`，不得納入效能統計；
- timeout、memory／disk resource failure：保留 artifact，標示 `resource-limited`；
- measured phase 中的 storage tail：`valid`，納入 worst p99.9/max，不能刪除或挑最快輪；
- 命令尚未啟動前的 setup failure：可修正後在新的 RUN_ROOT 重跑完整矩陣；
- identity 在矩陣期間變更：所有正式輪 `invalid_identity`，不得和新 binary 合併。

## 9. 報告格式與計算

報告 `docs/wal-fsync-amortization-frontier-benchmark-report.md` 必須包含：

1. commit、index/worktree/input-document identity、binary hash、host、kernel、CPU、compiler、filesystem、
   device、affinity、governor 與工具版本；
2. correctness gate、18 smoke、18 pilot 與 90 formal run status；
3. Engine 與 direct WAL 的五輪明細；
4. 每個 workload/W 的 group frontier aggregate；
5. actual commands/group、commands/sync、sync/s 與 WAL MiB/s；
6. completion／group／measured fsync latency 的 p50、p99、p99.9、max 與 threshold counts；
7. CPU、RSS、context switches、rotation 與 WAL bytes；
8. 每個 case 的 validity 與排除理由；
9. fixed-rate canary candidate gate 逐項 pass/fail；
10. 1M commands/s 的絕對與百分比差距；
11. production defaults 是否保持不變，以及後續是否值得另立 canary／storage design。

### 9.1 Frontier 規則

對每個 workload 與 W，找出最大有效 median throughput。距最大值 5% 內的最小 group 是該 W 的
ceiling Pareto point；group 增加但 uplift <5% 即標為 plateau。若 actual commands/group 未達
configured group 的 90%，只能標記為 supply-limited，不能宣稱平台化。

### 9.2 Fixed-rate canary candidate

只有 W=2 且同時符合以下條件，才可提出下一份 fixed-rate canary design：

- 相對同 group W=1 的五輪 median throughput uplift >=10%；
- 相對 `group=4096, W=1` 的 uplift >=10%；
- 五個配對 round 中，completion p99 超過對照 110% 的輪數少於 3；
- correctness、replay、parallel path、resource 與 drain 全部通過。

這不是 production acceptance。固定輸入率下的 p99/p99.9、publisher lag time-series、CPU budget 與
rollback 仍需另一份 canary procedure。若只有 W=4 通過，結果只能作 diagnostic ceiling。

## 10. 結束條件與後續

測試完成後重新建立：

```bash
{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  sha256sum docs/wal-fsync-amortization-frontier-design.md \
    docs/wal-fsync-amortization-frontier-benchmark-procedure.md "$BENCH_BIN"
} > "$RUN_ROOT/logs/source-identity-after.txt"
```

建立報告前必須確認 identity 與 before 相同；報告本身的 hash 不應回寫到 before/after identity。
所有 raw stdout、stderr、status、time、CSV、WAL directory 與 derived CSV 需保留並在報告列出位置。

若 direct WAL 與 Engine 都未達 1M，結論是目前單一 durable writer ceiling 不足，不得只靠提高 group
delay 宣稱可解決。若 group frontier 已平台化，下一步才可另立 storage/fsync policy 或更大架構設計；
本流程不自動進入 `fdatasync`、放寬 durability、修改 filesystem 或調整 production default。
