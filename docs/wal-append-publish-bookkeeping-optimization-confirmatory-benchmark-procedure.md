# WAL append publish bookkeeping guardrail 確認測試操作與報告規格

## 1. 目的與停止點

本文件只確認
`docs/wal-append-publish-bookkeeping-optimization-benchmark-report.md` 中尚未排除的兩項風險：

1. g4096 writer p99.9 是否因 per-chunk bookkeeping 穩定惡化；
2. g4096 writer involuntary context switches 是否因 candidate 穩定增加。

同時以 authoritative Engine g4096 確認 production-like throughput、tail latency 與 correctness
沒有退化。這不是新的 optimization，也不重跑 direct WAL、g8192、W=1/4、Publisher 或 Completion
矩陣。測試完成後只建立：

```text
docs/wal-append-publish-bookkeeping-optimization-confirmatory-benchmark-report.md
```

不得在本流程修改 production source、benchmark、runtime default 或既有報告；不得執行 `git add`、
`git reset`、`git restore --staged`、commit，或任何改變 Git index 的操作。

## 2. 證據與 artifact 定義

沿用原正式測試已建立且驗證過的 immutable binaries，避免重建造成 source 或 toolchain 差異：

```text
SOURCE_RUN_ROOT=/home/neojhou/wal-publish-bookkeeping-wQgSFDz7
baseline=$SOURCE_RUN_ROOT/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark
candidate=$SOURCE_RUN_ROOT/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark
baseline SHA-256=1d07826a99dd9d79950abd9bac8b47b2b917a2b830e449b545eeee93167fd2cb
candidate SHA-256=d10005dbc4335d60c438deba393c7a67d3ad5f079c253e8dbe99992dea059b29
```

baseline 是 per-record bookkeeping；candidate 是 per-chunk bookkeeping。若任一 binary hash 不符，
本次測試直接判為 `invalid`，不可重新編譯後沿用本文件中的 baseline 身份。

原報告是使用 `powersave` governor 的有效既有證據，不得刪除或重新標成 invalid。本次沿用相同
governor，並要求 governor、EPP 與 boost 在整個矩陣中保持不變，以重測原本的 guardrail failure。

## 3. 固定條件與最小矩陣

```text
build/artifacts                  沿用第 2 節兩個 ReleaseBenchmark binaries
instrument / shard               1 / 1
WAL prepare workers              2
parallel prepare threshold       4,096 commands
Engine producer lanes            8,192
Engine group size                4,096
Engine group delay               1,000 us
durability                       per-group fsync
writer phase profile             on
writer profile sample every      16
writer apply subprofile          off
CPU affinity                     taskset -c 2-7
CPU governor                     測試開始值，預期 powersave；CPUs 2-7 相同且全程不變
formal rounds                    10 paired rounds
writer iterations                2,364,273
Engine iterations                2,297,636
warmup                           10,000
data directory                   每個 artifact/case/round 全新且事前不存在
```

正式矩陣只有 40 runs：

| case | artifact | rounds | 用途 |
| --- | --- | ---: | --- |
| writer g4096 profile-on | baseline/candidate | 各 10 | 重測 p99.9、context switches 與 phase |
| authoritative Engine g4096 | baseline/candidate | 各 10 | production-like regression/correctness guardrail |

奇數輪依 baseline → candidate 執行，偶數輪依 candidate → baseline 執行。不得挑選較快輪、排除慢
`fsync` 或補跑第 11 輪。

固定 iterations 沿用原正式 case；每輪 measured duration 必須至少 20 秒。若任一 artifact/case 少於
20 秒，停止正式矩陣，依兩個 artifacts 中較快者將該 case iterations 等比例增加至
至少 25 秒，更新 `ITER_WRITER_G4096` 或 `ITER_ENGINE_G4096`，再從 round 1 重跑整個 10-round
case；不可混用不同 iterations。

## 4. 前置檢查與環境控制

以下操作在 repository root 執行。`RUN_PARENT` 必須位於與正式部署相同類型的實體 filesystem，不可使用
tmpfs 或 overlay。

```bash
set -euo pipefail

REPO_ROOT=/home/neojhou/repos/order_books
SOURCE_RUN_ROOT=/home/neojhou/wal-publish-bookkeeping-wQgSFDz7
RUN_PARENT=/home/neojhou
BENCH_CPU_SET=2-7
CASE_TIMEOUT_SECONDS=300
ITER_WRITER_G4096=2364273
ITER_ENGINE_G4096=2297636
BASELINE_BIN="$SOURCE_RUN_ROOT/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
CANDIDATE_BIN="$SOURCE_RUN_ROOT/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
EXPECTED_BASELINE_SHA=1d07826a99dd9d79950abd9bac8b47b2b917a2b830e449b545eeee93167fd2cb
EXPECTED_CANDIDATE_SHA=d10005dbc4335d60c438deba393c7a67d3ad5f079c253e8dbe99992dea059b29

cd "$REPO_ROOT"
for tool in /usr/bin/time timeout taskset sha256sum findmnt lsblk rg awk; do
  command -v "$tool" >/dev/null || exit 1
done
test -x "$BASELINE_BIN" && test -x "$CANDIDATE_BIN"
test "$(sha256sum "$BASELINE_BIN" | awk '{print $1}')" = "$EXPECTED_BASELINE_SHA"
test "$(sha256sum "$CANDIDATE_BIN" | awk '{print $1}')" = "$EXPECTED_CANDIDATE_SHA"

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-publish-bookkeeping-confirm-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,logs,time,telemetry,derived,perf}
printf '%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/logs/run-root.txt"

git status --short > "$RUN_ROOT/logs/git-status-before.txt"
git diff --binary | sha256sum > "$RUN_ROOT/logs/worktree-diff-before.sha256"
git diff --cached --binary | sha256sum > "$RUN_ROOT/logs/cached-diff-before.sha256"
while IFS= read -r -d '' path; do
  sha256sum "$path"
done < <(git ls-files --others --exclude-standard -z | LC_ALL=C sort -z) \
  > "$RUN_ROOT/logs/untracked-before.sha256"
sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" > "$RUN_ROOT/logs/binary-sha256-before.txt"

{
  date --iso-8601=seconds
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  lsblk -o NAME,TYPE,FSTYPE,SIZE,ROTA,SCHED,MOUNTPOINTS
  df -h "$RUN_ROOT"
  cat /proc/loadavg
  cat /proc/sys/kernel/perf_event_paranoid
  for cpu in 2 3 4 5 6 7; do
    printf 'cpu%s governor=' "$cpu"
    cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
    printf 'cpu%s epp=' "$cpu"
    cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/energy_performance_preference"
  done
  printf 'boost='
  cat /sys/devices/system/cpu/cpufreq/boost
} > "$RUN_ROOT/logs/environment-before.txt" 2>&1
```

記錄測試開始時的 governor、EPP 與 boost。本次為原結果的同環境確認，因此 governor 必須為
`powersave`，CPUs 2-7 的 governor 與 EPP 必須各自一致；本流程不修改任何 host policy，也不需要
`sudo`。

```bash
TEST_GOVERNORS="$RUN_ROOT/logs/test-governors.txt"
TEST_EPPS="$RUN_ROOT/logs/test-epps.txt"
for cpu in 2 3 4 5 6 7; do
  cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
  cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/energy_performance_preference" \
    >> "$TEST_EPPS"
done > "$TEST_GOVERNORS"
test "$(sort -u "$TEST_GOVERNORS" | wc -l)" -eq 1
test "$(sort -u "$TEST_EPPS" | wc -l)" -eq 1
TEST_GOVERNOR=$(head -n 1 "$TEST_GOVERNORS")
TEST_EPP=$(head -n 1 "$TEST_EPPS")
TEST_BOOST=$(cat /sys/devices/system/cpu/cpufreq/boost)
test "$TEST_GOVERNOR" = powersave

verify_cpu_policy() {
  for cpu in 2 3 4 5 6 7; do
    test "$(cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor")" \
      = "$TEST_GOVERNOR"
    test "$(cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/energy_performance_preference")" \
      = "$TEST_EPP"
  done
  test "$(cat /sys/devices/system/cpu/cpufreq/boost)" = "$TEST_BOOST"
}
```

正式矩陣期間不得執行其他高 CPU／I/O 工作，不得改變 boost、I/O scheduler、mount options、CPU
affinity 或 perf kernel policy。開始前記錄至少 30 秒 idle 狀態；若 CPUs 2-7 持續有非 benchmark
高負載，停止而不是事後刪除慢輪。

## 5. 執行函式

```bash
run_case() {
  artifact=$1
  phase=$2
  case_name=$3
  iterations=$4
  shift 4

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
  printf 'artifact=%s iterations=%s args=' "$artifact" "$iterations" > "$case_meta"
  printf '%q ' "$@" >> "$case_meta"
  printf '\n' >> "$case_meta"

  verify_cpu_policy
  if /usr/bin/time -v -o "$case_time" \
      timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
      taskset -c "$BENCH_CPU_SET" "$bench_bin" \
        --iterations="$iterations" --warmup=10000 \
        --data-dir="$case_data" "$@" \
        > "$case_stdout" 2> "$case_stderr"; then
    status=0
  else
    status=$?
  fi
  printf '%s\n' "$status" > "$case_status"
  verify_cpu_policy
  return "$status"
}

run_writer() {
  artifact=$1; phase=$2; round=$3
  run_case "$artifact" "$phase" "writer-g4096-on-$round" "$ITER_WRITER_G4096" \
    --workload=engine_writer_hot_path_profile \
    --writer-phase-profile=on \
    --writer-profile-sample-every=16 \
    --writer-apply-subprofile=off \
    --engine-group-size=4096 \
    --engine-group-delay-us=1000 \
    --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 \
    --wal-parallel-prepare-min-commands=4096
}

run_engine() {
  artifact=$1; phase=$2; round=$3
  telemetry="$RUN_ROOT/telemetry/$artifact-$phase-engine-g4096-$round.csv"
  run_case "$artifact" "$phase" "engine-g4096-$round" "$ITER_ENGINE_G4096" \
    --workload=engine_durable_single_instrument \
    --engine-group-size=4096 \
    --engine-group-delay-us=1000 \
    --engine-producer-lanes=8192 \
    --wal-prepare-workers=2 \
    --wal-parallel-prepare-min-commands=4096 \
    --engine-tail-telemetry-output="$telemetry" \
    --engine-tail-state-sampling=off
}
```

## 6. Smoke 與正式矩陣

Smoke 不納入 aggregate：

```bash
for artifact in baseline candidate; do
  run_writer "$artifact" smoke r0 || exit 1
  run_engine "$artifact" smoke r0 || exit 1
done
```

每個 status 必須為 0。Writer 必須有 `correctness_verified=true`、`profiled_groups >= 50`、
`wal_publish_ns_per_command` 與 `wal_append_ns_per_command`；Engine 必須通過 durable callback、replay、
completion exactly-once 與 telemetry validation。確認 smoke measured duration 均至少 20 秒後才執行：

```bash
for round in 1 2 3 4 5 6 7 8 9 10; do
  if test $((round % 2)) -eq 1; then
    artifacts=(baseline candidate)
  else
    artifacts=(candidate baseline)
  fi

  for artifact in "${artifacts[@]}"; do
    run_writer "$artifact" confirm "r$round" || exit 1
  done
  for artifact in "${artifacts[@]}"; do
    run_engine "$artifact" confirm "r$round" || exit 1
  done
done
```

## 7. 每輪有效性與計算方式

### 7.1 Invalid run

任一條件成立即為 invalid：

- exit status 非 0、timeout、資料目錄重用或 measured duration 少於 20 秒；
- correctness、replay、durable callback、completion exactly-once 或 telemetry validation 失敗；
- writer `profiled_groups < 50`、profile parent/child accounting 失敗或 denominator 不一致；
- 任一 g4096 Engine group fill 小於 90%；
- binary hash、source/index/worktree 身份或第 3 節固定環境在正式矩陣中改變；
- governor、EPP 或 boost 未維持測試開始值，或 CPU affinity、filesystem、mount options 改變。

低 throughput、p99.9 outlier、慢 `fsync`、較高 CPU 或較多 context switches 不是 invalid reason。任一正式
run invalid 時不得只補單輪；修正非效能原因後，重跑該 case 的完整 10 paired rounds。

### 7.2 每輪與 aggregate 計算

每個 artifact/case 分別由 10 個值計算 median、min、max。偶數樣本 median 是排序後第 5 與第 6 個值
的平均。每一輪另外依相同 round 配對 baseline/candidate：

```text
paired_delta_percent = (candidate - baseline) / baseline * 100
CPU_seconds_per_million = user_plus_system_CPU_seconds / commands * 1,000,000
voluntary_per_million = voluntary_context_switches / commands * 1,000,000
involuntary_per_million = involuntary_context_switches / commands * 1,000,000
group_fill_percent = actual_commands_per_group / 4,096 * 100
```

latency、CPU、context switches 與 RSS 是越低越好；RPS 是越高越好。不得以 range、最快輪或單一
outlier 取代 10-round median 與 paired sign count。

## 8. 判定規則與舊證據處理

本次 10-round guardrail 延續原本的 3/5 多數規則：某個越低越好的指標只有在「candidate median 高於
baseline，且 candidate 在至少 6/10 paired rounds 較高」時判定 fail。RPS regression 為：

```text
regression_percent = max(0, (baseline_median - candidate_median) / baseline_median * 100)
```

必要 gates：

| gate | pass 條件 |
| --- | --- |
| correctness / durability | 40/40 formal runs 全部通過 |
| writer p99.9 | 不同時出現 median 惡化與至少 6/10 paired worsening |
| writer involuntary context switches/M | 不同時出現 median 惡化與至少 6/10 paired worsening |
| writer p99 / CPU | latency／CPU 使用同一套 median + 6/10 guardrail |
| writer max / RSS | 只作觀察；不得單獨造成 acceptance failure |
| Engine throughput | candidate median regression ≤3% |
| Engine supply | 每輪 group fill ≥90% |
| Engine p99 / p99.9 / CPU / context | 使用同一套 median + 6/10 guardrail |
| Engine max / RSS | 只作觀察；不得單獨造成 acceptance failure |
| publish optimization | `wal_publish_ns_per_command` reduction 仍 ≥15% |
| append direction | candidate `wal_append_ns_per_command` median 低於 baseline |

結果分類：

- `controlled guardrail failed / do not retain`：本次任一必要 gate fail，或下述 15-pair 合併 gate fail；
- `controlled pass / retain`：本次全部 pass，且 15-pair 合併 gate 也 pass；
- `inconclusive`：正式資料完整，但本次與原結果矛盾且合併資料無法得到穩定方向；
- `invalid`：第 7.1 節任一條件成立。

原五輪與本次十輪使用相同 artifacts、workload、iterations、affinity 與 `powersave` policy，因此針對
原本失敗的 writer p99.9 與 involuntary context switches，另將每輪 paired delta 合併為 15 個值：

- paired-delta median 大於 0%，且 candidate 在至少 9/15 pairs 較差時，合併 gate fail；
- 其他情況合併 gate pass。

不得把兩次測試的絕對 latency、RPS 或 context-switch counts 合併成 15-run median，也不得因新結果
較好而刪除原 guardrail failure。報告必須並列原五輪、本次十輪及 15-pair delta 結論。

## 9. 條件式排程診斷

`perf` 會帶來量測開銷，因此不得包住第 6 節 formal runs。只有 writer involuntary context-switch gate
仍 fail，或 controlled result 與原報告相反時，才在 formal matrix 完成後另跑 diagnostic A/B；其數值
只用於歸因，不用於 acceptance median。

先檢查權限：

```bash
if command -v perf >/dev/null && perf stat -e task-clock -- true \
    >/dev/null 2>"$RUN_ROOT/perf/preflight.stderr"; then
  printf 'available\n' > "$RUN_ROOT/perf/status.txt"
else
  printf 'unavailable perf_event_paranoid=%s\n' \
    "$(cat /proc/sys/kernel/perf_event_paranoid)" > "$RUN_ROOT/perf/status.txt"
fi
```

若不可用，報告填 `not measured`；本流程不以 `sudo` 執行 benchmark，也不修改
`perf_event_paranoid`。若需要 privileged perf，必須另立診斷流程，不能把不同權限或 kernel policy 的
數據混入本次 formal runs。若 perf 可用，使用全新的 data directories，各執行三組交錯 diagnostic
pairs：

```text
perf stat events:
task-clock, context-switches, cpu-migrations, page-faults,
cycles, instructions, branches, branch-misses, cache-references, cache-misses
```

命令必須沿用 `run_writer` 的完整 benchmark arguments，以 `perf stat -x, -o <artifact>` 包住
`taskset ... <binary>`；每輪記錄 elapsed、commands 與每 million commands counters。若仍需 thread-level
歸因，再另用 `perf sched record/timehist`，不得把 sched tracing 結果當 throughput 或 latency 正式值。

具體 diagnostic 指令如下；即使使用相同使用者執行，perf overhead 仍使它只能作排程歸因，不能與
formal latency／RPS 比較：

```bash
run_writer_perf() {
  artifact=$1; round=$2
  if test "$artifact" = baseline; then
    bench_bin=$BASELINE_BIN
  else
    bench_bin=$CANDIDATE_BIN
  fi
  prefix="$artifact-perf-writer-g4096-on-$round"
  data_dir="$RUN_ROOT/data/$prefix"
  test ! -e "$data_dir" || return 2

  perf stat -x, \
    -o "$RUN_ROOT/perf/$prefix.csv" \
    -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,branches,branch-misses,cache-references,cache-misses \
    -- taskset -c "$BENCH_CPU_SET" "$bench_bin" \
      --iterations="$ITER_WRITER_G4096" --warmup=10000 \
      --data-dir="$data_dir" \
      --workload=engine_writer_hot_path_profile \
      --writer-phase-profile=on \
      --writer-profile-sample-every=16 \
      --writer-apply-subprofile=off \
      --engine-group-size=4096 \
      --engine-group-delay-us=1000 \
      --engine-producer-lanes=8192 \
      --wal-prepare-workers=2 \
      --wal-parallel-prepare-min-commands=4096 \
      > "$RUN_ROOT/logs/$prefix.stdout" \
      2> "$RUN_ROOT/logs/$prefix.stderr"
}

for round in 1 2 3; do
  if test $((round % 2)) -eq 1; then
    artifacts=(baseline candidate)
  else
    artifacts=(candidate baseline)
  fi
  for artifact in "${artifacts[@]}"; do
    run_writer_perf "$artifact" "r$round" || exit 1
  done
done
```

排程診斷只回答：增加的是 CPU migrations、scheduler preemption，還是 workload thread 的 wakeup／
blocking；沒有對應證據時，不得把 context-switch regression 歸因給 WAL、fsync 或 OS scheduler。

## 10. 結束身份與 CPU policy 檢查

```bash
git status --short > "$RUN_ROOT/logs/git-status-after.txt"
git diff --binary | sha256sum > "$RUN_ROOT/logs/worktree-diff-after.sha256"
git diff --cached --binary | sha256sum > "$RUN_ROOT/logs/cached-diff-after.sha256"
while IFS= read -r -d '' path; do
  sha256sum "$path"
done < <(git ls-files --others --exclude-standard -z | LC_ALL=C sort -z) \
  > "$RUN_ROOT/logs/untracked-after.sha256"
sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" > "$RUN_ROOT/logs/binary-sha256-after.txt"

diff -u "$RUN_ROOT/logs/worktree-diff-before.sha256" \
  "$RUN_ROOT/logs/worktree-diff-after.sha256"
diff -u "$RUN_ROOT/logs/cached-diff-before.sha256" \
  "$RUN_ROOT/logs/cached-diff-after.sha256"
diff -u "$RUN_ROOT/logs/untracked-before.sha256" \
  "$RUN_ROOT/logs/untracked-after.sha256"
diff -u "$RUN_ROOT/logs/binary-sha256-before.txt" \
  "$RUN_ROOT/logs/binary-sha256-after.txt"

verify_cpu_policy
```

Git status 文字可能因新增本次 report 而不同，所以 identity gate 比較的是執行前後的 cached/worktree
diff hashes、untracked manifest 與兩個 binaries；report 必須在 identity 比對完成後才建立。

## 11. 報告格式與必要內容

```markdown
# WAL append publish bookkeeping guardrail 確認測試報告

## 1. 結論
- result: controlled pass / controlled guardrail failed / inconclusive / invalid
- candidate status: retain / provisional / do not retain
- CPU policy used:
- primary reason:
- writer p99.9 gate:
- writer involuntary-context-switch gate:
- authoritative Engine gate:

## 2. 與原報告的關係
- 原報告路徑、result 與兩個 failed gates
- 為何只重測 g4096 writer 與 Engine
- 原五輪與本次十輪的固定條件一致性
- 絕對值分開報告、paired delta 合併判定的理由

## 3. Artifact 與環境身份
- date、RUN_ROOT、HEAD、Git status
- cached/worktree diff hashes、untracked manifest before/after
- baseline/candidate binary paths 與 SHA-256 before/after
- CPU model、affinity、SMT、boost、governor 與 EPP before/after
- filesystem、mount options、block topology、free space
- perf_event_paranoid 與 perf availability

## 4. Correctness 與有效性
- smoke、40/40 formal status
- replay、durability、completion、telemetry 結果
- measured duration、profiled_groups、group fill
- invalid/excluded runs；若無則填 none
- 明確聲明沒有排除慢 fsync 或 tail outlier

## 5. Writer g4096 profile-on
| metric | baseline median [min--max] | candidate median [min--max] | delta | paired worsening | gate |
- commands/s、p50、p99、p99.9、max
- wal_publish_ns_per_command、wal_append_ns_per_command
- CPU s/M、voluntary/M、involuntary/M、RSS

## 6. Authoritative Engine g4096
| metric | baseline median [min--max] | candidate median [min--max] | delta | paired worsening | gate |
- commands/s、group fill、p50、p99、p99.9、max
- sync p50/p99/p99.9/max 與 >25/>100/>250 ms
- CPU s/M、voluntary/M、involuntary/M、RSS

## 7. 排程診斷
| metric | baseline | candidate | delta | interpretation |
- perf unavailable 時填 not measured 並記錄 perf_event_paranoid
- formal `/usr/bin/time -v` 與 diagnostic perf 不可混為同一數據集
- 只記錄證據支持的歸因

## 8. Gate 逐項判定
| gate | required | observed | pass/fail |

## 9. 與原結果的合併結論
- original powersave result:
- confirmatory powersave result:
- writer p99.9 15-pair delta gate:
- writer involuntary-context-switch 15-pair delta gate:
- candidate 最終保留判定：
- 下一步只列一個必要動作；不得順便擴大 optimization

## Appendix A. 每輪原始摘要
| case | round | order | artifact | RPS | p99 | p99.9 | max | CPU/M | voluntary/M | involuntary/M | valid |

## Appendix B. Artifact 索引
- raw stdout/stderr/status/time/telemetry
- environment、identity、binary hashes
- perf diagnostic 或 unavailable evidence
```

每個 median 必須能追溯至 Appendix A 的十個正式值。原始 logs、time 與 telemetry 保留在
`RUN_ROOT`；WAL data 不複製進 repository。若 controlled run 通過，也只能對本文件固定的 Linux host
policy、W=2、g4096、1 ms group delay 與 per-group fsync 組合下結論，不能外推至 g8192、其他 storage
或百萬 RPS。
