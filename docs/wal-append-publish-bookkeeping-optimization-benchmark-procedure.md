# WAL append publish bookkeeping 最小化壓測操作與報告規格

## 1. 目的與最終輸出

本文件驗證 `docs/wal-append-publish-bookkeeping-optimization-design.md` 的 production optimization。
測試只回答以下問題：

1. 將 WAL append publish bookkeeping 由 per-record 提升為 per-chunk，是否讓 group=4,096、W=2 的
   writer `wal_publish_ns_per_command` 五輪 median 至少下降 15%；
2. `wal_append_ns_per_command` 是否同方向下降；
3. direct WAL 與 authoritative Engine 的五輪 median 是否都沒有退化超過 3%；
4. p99／p99.9 latency、CPU、context switches 與 storage sync tail 是否沒有跨多數輪穩定惡化；
5. WAL bytes、ordering、durability、replay 與 Engine correctness 是否保持不變。

結果寫入：

```text
docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md
```

本流程不修改 WAL format、group size、group delay、prepare worker 數、fsync policy、runtime defaults
或 benchmark CLI。禁止執行 `git add`、`git reset`、`git restore --staged`、commit，或任何改變
Git index 的操作。

## 2. Baseline／candidate 定義

本案必須比較同一次執行環境下建立的兩個 Release binary，不得把歷史報告數字當作正式 baseline：

| artifact | production `src/persistence/wal.cpp` | 其他 source／設定 |
| --- | --- | --- |
| baseline | `HEAD` 版本，即 per-record bookkeeping | 與 candidate snapshot 相同 |
| candidate | 目前 worktree 版本，即 per-chunk bookkeeping | 與 baseline snapshot 相同 |

兩份 source snapshot 都建立在 repository 外；baseline 只以 `HEAD` 的 `src/persistence/wal.cpp` 覆蓋
candidate snapshot。建立後必須證明兩棵 source tree 只有該檔案不同。這樣不需要切換 branch、stash、
reset 或修改 staging。

若正式矩陣開始後 source、index、worktree、binary、compiler、CPU affinity、filesystem 或固定參數改變，
已完成輪次不得與新輪次合併。

## 3. 固定條件與正式矩陣

### 3.1 共同條件

```text
build                         ReleaseBenchmark
instrument / shard            1 / 1
WAL prepare workers (W)        2
parallel prepare threshold     4,096 commands
Engine producer lanes          8,192
Engine group delay             1,000 us
durability                     per-group fsync
CPU affinity                   taskset -c 2-7
formal rounds                  每個 artifact/case 5 輪
measured duration              每輪至少 20 秒；pilot 以 25 秒為目標
data directory                 每輪全新且事前不存在
```

正式測試期間不得更改 CPU governor、boost、I/O scheduler、mount options、compiler、build flags、
page cache 或系統時間設定；不得同時執行其他高 CPU／高 I/O 工作。有效慢輪與慢 `fsync` 必須保留。

### 3.2 正式 case

| case | group | profile | 用途 | artifact × rounds |
| --- | ---: | --- | --- | ---: |
| direct WAL | 4,096 | off | direct WAL regression guardrail | 2 × 5 |
| direct WAL | 8,192 | off | direct WAL direction／group control | 2 × 5 |
| writer hot path | 4,096 | off | production-like writer throughput／latency | 2 × 5 |
| writer hot path | 4,096 | on | primary publish／append attribution | 2 × 5 |
| writer hot path | 8,192 | off | group direction與 profile bias | 2 × 5 |
| writer hot path | 8,192 | on | phase direction；非 authoritative frontier | 2 × 5 |
| authoritative Engine | 4,096 | off | end-to-end regression guardrail | 2 × 5 |

合計 70 個 formal runs。W=1／4、group=16,384、Publisher／Completion microbenchmark 與其他
pipeline stage 不在本需求內。

## 4. Correctness gate

在 repository root 執行。優先使用現有 wrapper：

```bash
set -euo pipefail
TOOLS=/tmp/order_books-tools/bin
CONAN="$TOOLS/conan"
CMAKE="$TOOLS/cmake"
CTEST="$TOOLS/ctest"
export PATH="$TOOLS:$PATH"
test -x "$CONAN" && test -x "$CMAKE" && test -x "$CTEST"

"$CONAN" install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE" --preset debug
"$CMAKE" --build --preset debug
"$CTEST" --preset debug --output-on-failure

"$CMAKE" --preset sanitizers
"$CMAKE" --build --preset sanitizers
"$CTEST" --preset sanitizers --output-on-failure

git diff --check
git diff --cached --check
```

Debug、ASan 或 UBSan 任一失敗即停止。Sanitizer binary 只作 correctness，不可用於效能測試。

## 5. 建立唯讀身份紀錄與 A/B source snapshots

`RUN_PARENT` 必須位於實體 filesystem，不可使用 tmpfs 或 overlay。70 個正式 runs 會保留獨立 WAL
data，開始前建議至少有 80 GiB 可用空間。

```bash
REPO_ROOT=/home/neojhou/repos/order_books
RUN_PARENT=/home/neojhou
BENCH_CPU_SET=2-7
CASE_TIMEOUT_SECONDS=300

cd "$REPO_ROOT"
for tool in /usr/bin/time timeout taskset findmnt lsblk sha256sum tar diff rg; do
  command -v "$tool" >/dev/null || exit 1
done
taskset -c "$BENCH_CPU_SET" true

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-publish-bookkeeping-XXXXXXXX")
BASELINE_SRC="$RUN_ROOT/source/baseline"
CANDIDATE_SRC="$RUN_ROOT/source/candidate"
mkdir -p "$BASELINE_SRC" "$CANDIDATE_SRC" \
  "$RUN_ROOT"/{data,logs,time,telemetry,derived}

{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  printf 'untracked_manifest_sha256='
  git ls-files --others --exclude-standard -z | sort -z | \
    xargs -0 -r sha256sum | sha256sum
  sha256sum \
    docs/wal-append-publish-bookkeeping-optimization-design.md \
    docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md \
    src/persistence/wal.cpp
} > "$RUN_ROOT/logs/repository-identity-before.txt"

git ls-files -co --exclude-standard -z | \
  tar --null -T - -cf - | tar -xf - -C "$CANDIDATE_SRC"
cp -a "$CANDIDATE_SRC/." "$BASELINE_SRC/"
git archive HEAD src/persistence/wal.cpp | tar -xf - -C "$BASELINE_SRC"

set +e
diff -qr "$BASELINE_SRC" "$CANDIDATE_SRC" \
  > "$RUN_ROOT/logs/source-tree-diff.txt"
tree_diff_status=$?
set -e
test "$tree_diff_status" -eq 1
test "$(wc -l < "$RUN_ROOT/logs/source-tree-diff.txt")" -eq 1
rg -q 'src/persistence/wal.cpp' "$RUN_ROOT/logs/source-tree-diff.txt"
```

若 source tree 不只一個檔案不同，停止測試並修正 snapshot，不得接受「大致相同」。

## 6. 建立與驗證 A/B Release binaries

兩份 snapshot 使用同一 wrapper、Conan profile、compiler 與 flags：

```bash
build_release_tree() {
  source_root=$1
  (
    cd "$source_root"
    "$CONAN" install . --build=missing -s build_type=Release \
      -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
    "$CMAKE" --preset release-benchmark
    "$CMAKE" --build --preset release-benchmark
    "$CTEST" --test-dir build/ReleaseBenchmark --output-on-failure
  )
}

build_release_tree "$BASELINE_SRC"
build_release_tree "$CANDIDATE_SRC"

BASELINE_BIN="$BASELINE_SRC/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
CANDIDATE_BIN="$CANDIDATE_SRC/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
test -x "$BASELINE_BIN" && test -x "$CANDIDATE_BIN"

sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" \
  > "$RUN_ROOT/logs/binary-sha256-before.txt"
{
  "$CMAKE" --version
  "$CONAN" --version
  c++ --version
} > "$RUN_ROOT/logs/toolchain.txt" 2>&1
```

baseline／candidate Release CTest 任一失敗即停止；不可只測 candidate。

## 7. 固定環境紀錄

```bash
{
  date --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>&1 || true
} > "$RUN_ROOT/logs/environment.txt" 2>&1

findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS -T "$RUN_ROOT" \
  > "$RUN_ROOT/logs/filesystem.txt"
```

報告必須記錄 `RUN_ROOT`、filesystem、block topology、CPU 型號／affinity、governor、compiler、
兩個 binary SHA-256、HEAD、cached diff hash 與 worktree diff hash。

## 8. 通用執行函式

以下函式保留 stdout、stderr、exit status、`time -v` 與實際命令。正式命令不得接到會掩蓋 exit
status 的 `tee` pipeline。

```bash
run_case() {
  artifact=$1
  phase=$2
  case_name=$3
  iterations=$4
  warmup=$5
  shift 5

  if test "$artifact" = baseline; then
    bench_bin=$BASELINE_BIN
  elif test "$artifact" = candidate; then
    bench_bin=$CANDIDATE_BIN
  else
    return 2
  fi

  prefix="$artifact-$phase-$case_name"
  case_data="$RUN_ROOT/data/$prefix"
  case_stdout="$RUN_ROOT/logs/$prefix.stdout"
  case_stderr="$RUN_ROOT/logs/$prefix.stderr"
  case_time="$RUN_ROOT/time/$prefix.time"
  case_status="$RUN_ROOT/logs/$prefix.status"
  case_meta="$RUN_ROOT/logs/$prefix.meta"

  test ! -e "$case_data" && test ! -e "$case_stdout" || return 2
  printf 'artifact=%s iterations=%s warmup=%s args=' \
    "$artifact" "$iterations" "$warmup" > "$case_meta"
  printf '%q ' "$@" >> "$case_meta"
  printf '\n' >> "$case_meta"

  if /usr/bin/time -v -o "$case_time" \
      timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
      taskset -c "$BENCH_CPU_SET" "$bench_bin" \
        --iterations="$iterations" --warmup="$warmup" \
        --data-dir="$case_data" "$@" \
        > "$case_stdout" 2> "$case_stderr"; then
    status=0
  else
    status=$?
  fi
  printf '%s\n' "$status" > "$case_status"
  return "$status"
}

run_writer() {
  artifact=$1; phase=$2; group_size=$3; profile=$4
  round=$5; iterations=$6; sample_every=${7:-0}
  args=(--workload=engine_writer_hot_path_profile
        --writer-phase-profile="$profile"
        --engine-group-size="$group_size"
        --engine-group-delay-us=1000
        --engine-producer-lanes=8192
        --wal-prepare-workers=2
        --wal-parallel-prepare-min-commands=4096)
  if test "$profile" = on; then
    args+=(--writer-profile-sample-every="$sample_every"
           --writer-apply-subprofile=off)
  fi
  run_case "$artifact" "$phase" \
    "writer-g${group_size}-${profile}-${round}" "$iterations" 10000 "${args[@]}"
}

run_wal() {
  artifact=$1; phase=$2; group_size=$3; round=$4; iterations=$5
  run_case "$artifact" "$phase" "wal-g${group_size}-${round}" \
    "$iterations" 100 \
    --workload=wal_write_ceiling --wal-phase-profile=off \
    --wal-sync=per_group --wal-group-size="$group_size" \
    --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096
}

run_engine() {
  artifact=$1; phase=$2; round=$3; iterations=$4
  telemetry="$RUN_ROOT/telemetry/$artifact-$phase-engine-g4096-$round.csv"
  run_case "$artifact" "$phase" "engine-g4096-$round" \
    "$iterations" 10000 \
    --workload=engine_durable_single_instrument \
    --engine-group-size=4096 --engine-group-delay-us=1000 \
    --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 --wal-parallel-prepare-min-commands=4096 \
    --engine-tail-telemetry-output="$telemetry" \
    --engine-tail-state-sampling=off
}
```

## 9. Smoke

兩個 artifact 都執行三個 workload，smoke 不納入 aggregate：

```bash
for artifact in baseline candidate; do
  run_wal "$artifact" smoke 4096 r0 2 || exit 1
  run_writer "$artifact" smoke 4096 off r0 10000 || exit 1
  run_writer "$artifact" smoke 4096 on r0 10000 1 || exit 1
  run_engine "$artifact" smoke r0 10000 || exit 1
done
```

每輪 status 必須為 0。Direct WAL 必須有 `replay_verified=true`；writer summary 必須有
`correctness_verified=true`；profile-on 必須輸出 `wal_publish_ns_per_command` 與
`wal_append_ns_per_command`；Engine 必須通過 durable callback、replay 與 telemetry validation。

## 10. Pilot 與 formal iterations

Pilot 只決定 command budget，不納入正式結果。建議初值：

```text
writer g4096/g8192   iterations=400000, warmup=10000
Engine g4096         iterations=400000, warmup=10000
direct WAL           iterations=512 groups, warmup=100 groups
```

對 baseline／candidate 各跑一次 profile-off pilot：

```bash
for artifact in baseline candidate; do
  run_wal "$artifact" pilot 4096 r0 512 || exit 1
  run_wal "$artifact" pilot 8192 r0 512 || exit 1
  run_writer "$artifact" pilot 4096 off r0 400000 || exit 1
  run_writer "$artifact" pilot 8192 off r0 400000 || exit 1
  run_engine "$artifact" pilot r0 400000 || exit 1
done
```

每個 case 取 baseline／candidate 中較快者，依下式計算共同正式 iterations：

```text
required_iterations = ceil(pilot_iterations × 25,000 / pilot_elapsed_ms)
formal_iterations = max(required_iterations_baseline,
                        required_iterations_candidate)
```

分別設定並驗證：

```bash
ITER_WAL_G4096=<依 pilot 計算的正整數>
ITER_WAL_G8192=<依 pilot 計算的正整數>
ITER_WRITER_G4096=<依 pilot 計算的正整數>
ITER_WRITER_G8192=<依 pilot 計算的正整數>
ITER_ENGINE_G4096=<依 pilot 計算的正整數>

for value in "$ITER_WAL_G4096" "$ITER_WAL_G8192" \
  "$ITER_WRITER_G4096" "$ITER_WRITER_G8192" "$ITER_ENGINE_G4096"; do
  test "$value" -gt 0 || exit 1
done
```

正式第一個 A/B pair 若任一 measured duration 少於 20 秒，該 case 已完成的 formal runs 全部作廢，
提高共同 iterations 後以新的 phase prefix 從 r1 重跑；不得只延長 baseline 或 candidate。

## 11. Writer profile sampling calibration

baseline 與 candidate 必須使用相同 `SAMPLE_EVERY`。依序嘗試 N=8、16、32；每個 N、artifact、group
都以三輪 off/on 交錯測試：

```text
r1: off, on
r2: on, off
r3: off, on
```

g4096 使用 `ITER_WRITER_G4096`，g8192 使用 `ITER_WRITER_G8192`。每個 artifact/group 分別計算：

```text
profile_bias_percent =
  abs(profile_on_median_rps - profile_off_median_rps)
  / profile_off_median_rps × 100%
```

只有 baseline 與 candidate 的 g4096 bias 全部 `<= 5%`，才能選用該 N。g8192 bias 仍須記錄；若超過
5%，只將 g8192 phase rows 標為方向性不可解讀，不使 g4096 primary、direct WAL 或 Engine gate 失效。
選定後：

```bash
SAMPLE_EVERY=<8、16 或 32>
test "$SAMPLE_EVERY" -eq 8 -o "$SAMPLE_EVERY" -eq 16 -o "$SAMPLE_EVERY" -eq 32
```

若 N=32 仍不合格，停止 profile-on 正式矩陣並將結果判定為 `inconclusive`；不得用有偏差的 g4096
phase 數據宣稱達到 15% publish reduction。g4096 每個正式 profile-on run 必須至少有 50 個
`profiled_groups`；g8192 profile-on 只有在達到同一門檻時才可列為方向性 phase data，否則保留 raw
summary 並標示 `directional / insufficient samples`。

以下函式完整執行一個 N；一次只執行一個 N，計算完兩組 g4096 bias 後才決定是否執行下一個；
g8192 bias 另行記錄為方向性資料：

```bash
run_calibration_n() {
  sample_every=$1
  for group_size in 4096 8192; do
    if test "$group_size" -eq 4096; then
      iterations=$ITER_WRITER_G4096
    else
      iterations=$ITER_WRITER_G8192
    fi
    for artifact in baseline candidate; do
      run_writer "$artifact" "cal-n$sample_every" "$group_size" off r1 \
        "$iterations" || return 1
      run_writer "$artifact" "cal-n$sample_every" "$group_size" on r1 \
        "$iterations" "$sample_every" || return 1
      run_writer "$artifact" "cal-n$sample_every" "$group_size" on r2 \
        "$iterations" "$sample_every" || return 1
      run_writer "$artifact" "cal-n$sample_every" "$group_size" off r2 \
        "$iterations" || return 1
      run_writer "$artifact" "cal-n$sample_every" "$group_size" off r3 \
        "$iterations" || return 1
      run_writer "$artifact" "cal-n$sample_every" "$group_size" on r3 \
        "$iterations" "$sample_every" || return 1
    done
  done
}

run_calibration_n 8
# 只有任一 g4096 artifact bias > 5% 時才依序執行：
# run_calibration_n 16
# run_calibration_n 32
```

## 12. 正式 A/B 矩陣

每個 case 的 artifact 順序逐輪反轉，降低時間與 storage temperature 偏差：

```bash
for round in 1 2 3 4 5; do
  if test $((round % 2)) -eq 1; then
    artifacts=(baseline candidate)
  else
    artifacts=(candidate baseline)
  fi

  for artifact in "${artifacts[@]}"; do
    run_wal "$artifact" formal 4096 "r$round" "$ITER_WAL_G4096" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_wal "$artifact" formal 8192 "r$round" "$ITER_WAL_G8192" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_writer "$artifact" formal 4096 off "r$round" \
      "$ITER_WRITER_G4096" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_writer "$artifact" formal 4096 on "r$round" \
      "$ITER_WRITER_G4096" "$SAMPLE_EVERY" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_writer "$artifact" formal 8192 off "r$round" \
      "$ITER_WRITER_G8192" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_writer "$artifact" formal 8192 on "r$round" \
      "$ITER_WRITER_G8192" "$SAMPLE_EVERY" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_engine "$artifact" formal "r$round" "$ITER_ENGINE_G4096" || exit 1
  done
done
```

不得因某輪較慢而刪除或補跑第六輪。只有第13節所列的 invalid run 才能排除；排除任一正式輪後，
同一 artifact/case 的五輪集合已不完整，必須修正非效能原因並重跑該 artifact/case 的完整五輪。

## 13. 每輪有效性與監測點

### 13.1 Invalid run

符合任一條件即為 invalid：

- exit status 非 0、timeout、資源耗盡或 data directory 被重用；
- correctness、WAL replay、EngineSeq continuity、durable callback 或 completion exactly-once 失敗；
- measured duration 少於 20 秒；
- source／index／worktree／binary 或固定環境在矩陣中改變；
- writer g4096 profile parent/child accounting 失敗、`profiled_groups < 50` 或 sample denominator 不一致；
- g8192 writer 的 profile sample 不足只使該方向性 rows 不可解讀，不是整體 formal matrix 的 invalid 原因；
- publisher failure、storage pressure、Completion capacity wait 導致 benchmark 自行報錯。

低 throughput、高 latency、慢 `fsync`、較高 CPU 或較多 context switches 本身不是 invalid reason。

### 13.2 Supply-limited

Writer／Engine 每輪計算：

```text
group_fill_percent = actual_commands_per_group / configured_group_size × 100%
```

小於 90% 標為 `supply-limited`。保留 raw data，但不得用該輪宣稱 group frontier；尤其 g8192 只作
phase方向性資料。g4096 authoritative Engine 若不足 90%，主要 Engine gate 判為 `inconclusive`，不可
改用 g8192 或最快輪替代。

### 13.3 WAL byte-identity control

正式矩陣前後各執行一次 deterministic correctness control；此 control 不取代 formal throughput
結果，也不與任何正式輪混合。baseline 與 candidate 使用同一組固定 commands、shard、segment size、
prepare options 與 sync policy，至少涵蓋：

1. 不 rotation 的 batch；
2. 每個 segment 至少一筆、且 batch 橫跨多個 segment 的 rotation batch。

每個 artifact 完成 append、`sync()`、close/reopen 後，依 segment filename 排序產生 manifest：

```text
relative_segment_path  file_size  sha256
```

兩份 manifest 必須以 `diff -u` 完全相同；檔名、size 或 hash 任一不同即為 correctness gate failure。
同時保留兩份 replay 結果與 command ordering assertion。manifest、diff 與 stdout/status 必須保留在
`$RUN_ROOT/logs/byte-identity/`，report 記錄 artifact 路徑與結果。不能以 replay 成功且 WAL size 相同
推定 byte-for-byte 相同。

### 13.4 每輪必收欄位

共同欄位：

```text
artifact, workload, group_size, round, profile, sample_every
iterations, commands, elapsed_ms, commands_per_second
p50, p99, p99.9, max latency
actual_commands_per_group, group_fill_percent
wal_bytes_delta / wal_size_bytes, rotations
correctness/replay result, exit status
user seconds, system seconds, CPU percent, maximum RSS
voluntary/involuntary context switches
```

Writer profile-on 另外收集：

```text
profiled_groups, profiled_commands
writer_service_ns_per_command
wal_append_ns_per_command
wal_prepare_ns_per_command
wal_plan_copy_ns_per_command
wal_chunk_copy_ns_per_command
wal_publish_ns_per_command
wal_write_ns_per_command
wal_sync_ns_per_command
publisher_lag_events/bytes/age
completion service RPS, queue p50/p99/p99.9/max, max queue depth
```

Direct WAL 另外收集 append、sync、group-total 的 p50/p99/p99.9/max 與 WAL MiB/s。Engine 另外收集
telemetry 的 measured sync count、p50/p99/p99.9/max/total、>25/>100/>250 ms counts、publisher lag max
與 queue depth max。

CPU 與 context switches 同時回報 raw 值及正規化值：

```text
cpu_seconds_per_million_commands =
  (user_seconds + system_seconds) / commands × 1,000,000
context_switches_per_million_commands =
  context_switches / commands × 1,000,000
```

## 14. Aggregate、比較公式與通過條件

所有中心值使用五輪 median，range 保留 min--max；不可只報最快輪。

```text
higher_is_better_delta_percent =
  (candidate_median - baseline_median) / baseline_median × 100%

lower_is_better_reduction_percent =
  (baseline_median - candidate_median) / baseline_median × 100%

regression_percent =
  max(0, (baseline_median - candidate_median) / baseline_median × 100%)
```

Candidate 只有同時符合下列條件才可標為 `pass / retain`：

1. g4096/W2 writer `wal_publish_ns_per_command` median reduction >= 15%；
2. g4096/W2 writer `wal_append_ns_per_command` candidate median低於 baseline；
3. writer profile off/on RPS bias <= 5%；
4. g4096 direct WAL median RPS regression <= 3%；
5. g4096 authoritative Engine median RPS regression <= 3%，且非 supply-limited；
6. g8192 direct WAL median RPS regression <= 3%；
7. 每個 g4096 writer profile-on run 至少有 50 個 `profiled_groups`；g8192 writer 僅作方向性資料，
   不作整體 validity gate；
8. correctness與 identity gate全部通過；
9. p99／p99.9、CPU seconds/command、context switches/command 或 storage tail沒有同時出現
   candidate median惡化且五個 paired rounds中至少三輪惡化。

若 correctness／identity失敗，結論是 `invalid`；若 sample bias、有效輪數或 authoritative Engine group
fill不足，結論是 `inconclusive`；若資料有效但任一必要 performance gate失敗，結論是
`no material gain / do not retain`。不得因程式看起來合理而覆寫門檻。

## 15. 結束身份檢查

正式矩陣完成後、建立 repository 內 report 之前，重新取得 identity：

```bash
cd "$REPO_ROOT"
{
  printf 'head='; git rev-parse HEAD
  git status --short
  printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
  printf 'worktree_diff_sha256='; git diff --binary | sha256sum
  printf 'untracked_manifest_sha256='
  git ls-files --others --exclude-standard -z | sort -z | \
    xargs -0 -r sha256sum | sha256sum
  sha256sum \
    docs/wal-append-publish-bookkeeping-optimization-design.md \
    docs/wal-append-publish-bookkeeping-optimization-benchmark-procedure.md \
    src/persistence/wal.cpp
} > "$RUN_ROOT/logs/repository-identity-after.txt"

diff -u "$RUN_ROOT/logs/repository-identity-before.txt" \
  "$RUN_ROOT/logs/repository-identity-after.txt"
sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" \
  > "$RUN_ROOT/logs/binary-sha256-after.txt"
diff -u "$RUN_ROOT/logs/binary-sha256-before.txt" \
  "$RUN_ROOT/logs/binary-sha256-after.txt"
```

Repository identity 必須完全相同；兩個 binary hash也必須與第6節一致。若不同，停止聚合，不得把前後
不同 artifact 的輪次寫成同一份正式結果。identity通過後才建立 report，避免 report 本身改變
worktree/untracked manifest。

## 16. 報告格式與必要內容

Report 必須使用以下結構；沒有資料的欄位填 `not measured` 並說明原因，不可省略。

```markdown
# WAL append publish bookkeeping 最小化壓測報告

## 1. 結論
- result: pass / performance guardrail failed / no material gain / inconclusive / invalid
- retain candidate: yes / no / provisional / undecided
- primary reason:
- publish median reduction:
- direct WAL regression:
- authoritative Engine regression:

## 2. 測試範圍與方法
- design / procedure path
- RUN_ROOT
- fixed matrix、rounds、A/B交錯順序
- 不在範圍內的項目

## 3. Source、binary與環境身份
- date、HEAD、Git status
- cached/worktree/untracked hashes
- baseline/candidate source定義與tree diff
- binary SHA-256、compiler、flags
- CPU、affinity、governor
- filesystem、mount options、block topology、free space

## 4. Correctness與有效性
- Debug／Release／ASan／UBSan結果
- smoke結果
- replay／ordering／durability／completion結果
- deterministic WAL byte-identity manifest、diff與artifact路徑
- invalid/excluded runs及原因
- supply-limited runs
- selected SAMPLE_EVERY與各artifact/group profile bias

## 5. 正式結果
### 5.1 Primary writer g4096
| metric | baseline median [min--max] | candidate median [min--max] | delta | gate |

### 5.2 Direct WAL controls
| group | artifact | RPS median [min--max] | WAL MiB/s | p99/p99.9 | regression |

### 5.3 Authoritative Engine g4096
| artifact | RPS median [min--max] | group fill | p50/p99/p99.9/max | regression |

### 5.4 Writer g8192方向性資料
| metric | baseline | candidate | delta | supply-limited note |

### 5.5 CPU、context switches與RSS
| case | artifact | CPU s/M commands | voluntary/M | involuntary/M | max RSS |

### 5.6 Storage sync tail與downstream護欄
| artifact | sync p50/p99/p99.9/max | >25/>100/>250ms | publisher lag | completion queue |

## 6. Primary phase attribution
| phase ns/command | baseline median | candidate median | reduction | interpretation |
- 必須至少包含 WAL append、prepare、plan/copy、publish、write、sync。
- 說明 publish下降是否傳遞到append與完整writer。

## 7. Gate逐項判定
| gate | required | observed | pass/fail |

## 8. 結論與下一步
- 若pass：保留candidate，指出剩餘第一個同步ceiling。
- 若no material gain：不擴大本patch，回到plan/copy/cache insertion attribution。
- 若inconclusive/invalid：列出必須重測的原因，不下效能結論。
- 若 correctness byte-identity control 未執行：明確標示 `not measured`，不得宣稱 correctness gate 全部通過。

## Appendix A. 每輪原始摘要
| artifact | case | round | RPS | latency | phase | CPU/context | valid |

## Appendix B. Artifact索引
- raw stdout/stderr/status/time/telemetry路徑
- identity與environment檔案路徑
```

報告中的每個 median 必須可追溯至 Appendix A 的五個正式值；所有原始 logs 保留在 `RUN_ROOT`，不把
數十 GiB WAL data複製進 repository。

## 17. 停止點

完成本文件後只產生 benchmark report與候選保留判定，不在同一變更中繼續調整 `records_` container、
buffer representation、`writev`、group size、prepare workers、Publisher或Completion。若結果未達門檻，
下一步先另行設計 `plan/copy`、cache insertion 與 terminal position update 的 benchmark-only nested
attribution；不得在本輪臨時加 instrumentation 後繼續沿用既有正式數據。
