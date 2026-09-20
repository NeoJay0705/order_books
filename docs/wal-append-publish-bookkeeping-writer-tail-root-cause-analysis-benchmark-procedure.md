# WAL append publish bookkeeping Writer tail 根因分析：壓測操作與報告規格

## 1. 目的與停止點

本文件只執行
`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-design.md` 定義的 Writer
根因歸因，回答下列問題：

1. Writer workload 的排程成本實際由 load generator、writer、WAL prepare helper、publisher 或
   completion 哪一個 thread role 貢獻；
2. tail regression 是否只在 Writer phase profile 開啟時出現；
3. writer runqueue wait、involuntary context switches 或 migration 是否與 p99.9 同方向；
4. WAL sync p99／p99.9／max 或慢 sync 次數是否與慢輪同時上升；
5. 證據只能分類為哪一項根因，或是否仍為 `inconclusive attribution`。

本流程不修改 production code、WAL format、fsync policy、group size、worker 數量、queue、affinity
策略或 runtime default，也不重跑 direct WAL、g8192、Publisher、Completion 或完整 Engine 矩陣。
不得執行 `git add`、`git reset`、`git restore --staged`、commit 或其他會改變 Git index 的操作。

完成後只新增：

```text
docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-report.md
```

## 2. Baseline／candidate 與證據邊界

正式比較必須使用同一份診斷實作建立的兩個 Release binary：

| artifact | `src/persistence/wal.cpp` | 其他 source、diagnostics、build flags |
| --- | --- | --- |
| baseline | 目前 snapshot 反向套用 commit `ea72a31` 的純 bookkeeping diff | 與 candidate 完全相同 |
| candidate | 目前 worktree 的 per-chunk bookkeeping | 與 baseline 完全相同 |

舊 confirmatory binaries 沒有本次 thread-resource 與 Writer sync-tail diagnostics，不得拿來執行 case
C。目前 `HEAD` 已包含 per-chunk optimization，不能把整份 `HEAD` WAL 當成 baseline；否則也會移除
本次新增的 prepare-thread naming。兩份 source snapshot 必須建立在 repository 外，baseline 只能反向
套用已驗證 commit `ea72a31e25f87d2528b795704a6c2d5b07423e72` 的單一 production hunk，且
`diff -qr` 必須證明只有該檔案不同。如此兩邊仍使用完全相同的 thread naming 與 diagnostics。

本測試輸出是根因分類，不重新接受或拒絕既有 optimization。原 candidate 狀態仍為
`provisional, not accepted`，直到後續 production 修正另有設計與 acceptance evidence。

## 3. 固定條件與最小矩陣

```text
OS                               Linux
instrument / shard               1 / 1
workload                          engine_writer_hot_path_profile
WAL prepare workers              2
parallel prepare threshold       4,096 commands
producer lanes                   8,192
group size                       4,096
group delay                      1,000 us
durability                       per-group fsync
writer profile sample every      16
writer apply subprofile          off
CPU affinity                     taskset -c 2-7
warmup                           10,000 iterations
formal rounds                    每個 artifact/case 5 輪
measured duration                每輪至少 20 秒；calibration 以 25 秒為目標
data directory                   每輪全新且事前不存在
```

正式矩陣只有 30 輪：

| case | phase profile | thread diagnostics | artifact × rounds | 用途 |
| --- | --- | --- | ---: | --- |
| A | off | off | 2 × 5 | production-like Writer control |
| B | on | off | 2 × 5 | 判斷 phase-profile interaction |
| C | on | on | 2 × 5 | per-thread scheduler 與逐 group sync 歸因 |

case C 必須提供唯一的 `--writer-tail-telemetry-output`。case A/B 不得輸出任何
`thread_diagnostics`、`thread_resource` 或 `writer_tail_*` 欄位。

## 4. Correctness gate

在 repository root 執行。以下路徑沿用本專案現有工具；若工具位置不同，只能替換工具路徑，不得
更改編譯模式或跳過測試。

```bash
set -euo pipefail

REPO_ROOT=/home/neojhou/repos/order_books
TOOLS_DIR=/tmp/order_books-tools/bin
CONAN_BIN="$TOOLS_DIR/conan"
CMAKE_BIN="$TOOLS_DIR/cmake"
CTEST_BIN="$TOOLS_DIR/ctest"
export PATH="$TOOLS_DIR:$PATH"

cd "$REPO_ROOT"
test -x "$CONAN_BIN"
test -x "$CMAKE_BIN"
test -x "$CTEST_BIN"

"$CONAN_BIN" install . --build=missing -s build_type=Debug \
  -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
"$CMAKE_BIN" --preset debug
"$CMAKE_BIN" --build --preset debug --parallel 4
"$CTEST_BIN" --preset debug --output-on-failure

"$CMAKE_BIN" --preset sanitizers
"$CMAKE_BIN" --build --preset sanitizers --parallel 4
"$CTEST_BIN" --preset sanitizers --output-on-failure

git diff --check
git diff --cached --check
```

Debug、ASan／UBSan、diff check 任一失敗即停止。Sanitizer binary 只作 correctness，不得用於效能
數據。正式 snapshot 建立後，baseline／candidate 的 Release CTest 還要各自通過一次。

## 5. 建立 run root、身份紀錄與 A/B snapshots

`RUN_PARENT` 必須位於正式 WAL 使用的實體 filesystem，不可使用 tmpfs 或 overlay。開始前確認空間足以
保留 30 輪 WAL data、logs 與 telemetry；不得在正式矩陣中途清除有效輪次。

```bash
RUN_PARENT=/home/neojhou
BENCH_CPU_SET=2-7
CASE_TIMEOUT_SECONDS=300
PILOT_ITERATIONS=400000
WARMUP_ITERATIONS=10000
SAMPLE_EVERY=16

BOOKKEEPING_COMMIT=ea72a31e25f87d2528b795704a6c2d5b07423e72
EXPECTED_BOOKKEEPING_PATCH_SHA=349027d397401fe350c01a3d6422a774f922ffe78bc3728e9c0f569a039088ed

for tool in /usr/bin/time timeout taskset sha256sum findmnt lsblk diff rg awk tar patch; do
  command -v "$tool" >/dev/null || exit 1
done
taskset -c "$BENCH_CPU_SET" true

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-writer-tail-root-cause-XXXXXXXX")
BASELINE_SRC="$RUN_ROOT/source/baseline"
CANDIDATE_SRC="$RUN_ROOT/source/candidate"
mkdir -p "$BASELINE_SRC" "$CANDIDATE_SRC" \
  "$RUN_ROOT"/{data,logs,time,telemetry,derived}
printf '%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/logs/run-root.txt"

record_repository_identity() {
  output=$1
  {
    printf 'head='; git rev-parse HEAD
    git status --short
    printf 'cached_diff_sha256='; git diff --cached --binary | sha256sum
    printf 'worktree_diff_sha256='; git diff --binary | sha256sum
    printf 'untracked_manifest_sha256='
    git ls-files --others --exclude-standard -z | LC_ALL=C sort -z | \
      xargs -0 -r sha256sum | sha256sum
  } > "$output"
}

record_repository_identity "$RUN_ROOT/logs/repository-identity-before.txt"

git ls-files -co --exclude-standard -z | \
  tar --null -T - -cf - | tar -xf - -C "$CANDIDATE_SRC"
cp -a "$CANDIDATE_SRC/." "$BASELINE_SRC/"

test "$(git rev-parse "$BOOKKEEPING_COMMIT")" = "$BOOKKEEPING_COMMIT"
BOOKKEEPING_PATCH="$RUN_ROOT/logs/per-chunk-bookkeeping.patch"
git --no-pager diff --no-ext-diff --no-color \
  "$BOOKKEEPING_COMMIT^" "$BOOKKEEPING_COMMIT" \
  -- src/persistence/wal.cpp > "$BOOKKEEPING_PATCH"
test "$(sha256sum "$BOOKKEEPING_PATCH" | awk '{print $1}')" \
  = "$EXPECTED_BOOKKEEPING_PATCH_SHA"

patch --batch --no-backup-if-mismatch --dry-run --reverse --strip=1 \
  --directory="$BASELINE_SRC" \
  < "$BOOKKEEPING_PATCH"
patch --batch --no-backup-if-mismatch --reverse --strip=1 \
  --directory="$BASELINE_SRC" \
  < "$BOOKKEEPING_PATCH"

set +e
diff -qr "$BASELINE_SRC" "$CANDIDATE_SRC" \
  > "$RUN_ROOT/logs/source-tree-diff.txt"
source_diff_status=$?
set -e
test "$source_diff_status" -eq 1
test "$(wc -l < "$RUN_ROOT/logs/source-tree-diff.txt")" -eq 1
rg -q 'src/persistence/wal.cpp' "$RUN_ROOT/logs/source-tree-diff.txt"
```

若 source tree 不只一個檔案不同，停止；不得用舊 baseline binary 或手動忽略差異替代。

## 6. 建置與凍結 Release artifacts

```bash
build_release_tree() {
  source_root=$1
  (
    cd "$source_root"
    "$CONAN_BIN" install . --build=missing -s build_type=Release \
      -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja
    "$CMAKE_BIN" --preset release-benchmark
    "$CMAKE_BIN" --build --preset release-benchmark --parallel 4
    "$CTEST_BIN" --test-dir build/ReleaseBenchmark --output-on-failure
  )
}

build_release_tree "$BASELINE_SRC"
build_release_tree "$CANDIDATE_SRC"

BASELINE_BIN="$BASELINE_SRC/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
CANDIDATE_BIN="$CANDIDATE_SRC/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
test -x "$BASELINE_BIN"
test -x "$CANDIDATE_BIN"
sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" \
  > "$RUN_ROOT/logs/binary-sha256-before.txt"

{
  "$CMAKE_BIN" --version
  "$CONAN_BIN" --version
  c++ --version
} > "$RUN_ROOT/logs/toolchain.txt" 2>&1
```

正式矩陣開始後不得重新建置或替換 binary。

## 7. 環境紀錄與穩定性

```bash
capture_cpu_policy() {
  for cpu in 2 3 4 5 6 7; do
    for field in scaling_governor energy_performance_preference; do
      path="/sys/devices/system/cpu/cpu${cpu}/cpufreq/$field"
      if test -r "$path"; then
        printf 'cpu=%s %s=' "$cpu" "$field"
        cat "$path"
      else
        printf 'cpu=%s %s=not_available\n' "$cpu" "$field"
      fi
    done
  done
  if test -r /sys/devices/system/cpu/cpufreq/boost; then
    printf 'boost='; cat /sys/devices/system/cpu/cpufreq/boost
  else
    printf 'boost=not_available\n'
  fi
}

{
  date --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS
  cat /proc/loadavg
  cat /proc/sys/kernel/perf_event_paranoid
  capture_cpu_policy
} > "$RUN_ROOT/logs/environment-before.txt" 2>&1

capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-reference.txt"
verify_cpu_policy() {
  capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-current.txt"
  diff -u "$RUN_ROOT/logs/cpu-policy-reference.txt" \
    "$RUN_ROOT/logs/cpu-policy-current.txt" >/dev/null
}
```

測試期間不得修改 governor、EPP、boost、I/O scheduler、mount options、CPU affinity 或系統時間，亦不得
同時執行其他大量 CPU／I/O 工作。開始前觀察至少 30 秒 idle 狀態；若持續有外部負載，停止而非事後
排除慢輪。無法取得的 CPU policy 欄位記為 `not_available`，不得填 0。

## 8. 通用執行函式

```bash
run_writer() {
  artifact=$1
  phase=$2
  case_id=$3
  round=$4
  iterations=$5

  if test "$artifact" = baseline; then
    bench_bin=$BASELINE_BIN
  elif test "$artifact" = candidate; then
    bench_bin=$CANDIDATE_BIN
  else
    return 2
  fi

  prefix="$artifact-$phase-case${case_id}-$round"
  case_data="$RUN_ROOT/data/$prefix"
  case_stdout="$RUN_ROOT/logs/$prefix.stdout"
  case_stderr="$RUN_ROOT/logs/$prefix.stderr"
  case_time="$RUN_ROOT/time/$prefix.time"
  case_status="$RUN_ROOT/logs/$prefix.status"
  case_meta="$RUN_ROOT/logs/$prefix.meta"
  args=(--workload=engine_writer_hot_path_profile
        --engine-group-size=4096
        --engine-group-delay-us=1000
        --engine-producer-lanes=8192
        --wal-prepare-workers=2
        --wal-parallel-prepare-min-commands=4096)

  case "$case_id" in
    A)
      args+=(--writer-phase-profile=off)
      ;;
    B)
      args+=(--writer-phase-profile=on
             --writer-profile-sample-every="$SAMPLE_EVERY"
             --writer-apply-subprofile=off)
      ;;
    C)
      telemetry="$RUN_ROOT/telemetry/$prefix.csv"
      test ! -e "$telemetry" || return 2
      args+=(--writer-phase-profile=on
             --writer-profile-sample-every="$SAMPLE_EVERY"
             --writer-apply-subprofile=off
             --writer-thread-diagnostics=on
             --writer-tail-telemetry-output="$telemetry")
      ;;
    *)
      return 2
      ;;
  esac

  test ! -e "$case_data"
  test ! -e "$case_stdout"
  printf 'artifact=%s phase=%s case=%s round=%s iterations=%s args=' \
    "$artifact" "$phase" "$case_id" "$round" "$iterations" > "$case_meta"
  printf '%q ' "${args[@]}" >> "$case_meta"
  printf '\n' >> "$case_meta"

  verify_cpu_policy
  if LC_ALL=C /usr/bin/time -v -o "$case_time" \
      timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
      taskset -c "$BENCH_CPU_SET" "$bench_bin" \
        --iterations="$iterations" --warmup="$WARMUP_ITERATIONS" \
        --data-dir="$case_data" "${args[@]}" \
        > "$case_stdout" 2> "$case_stderr"; then
    status=0
  else
    status=$?
  fi
  printf '%s\n' "$status" > "$case_status"
  verify_cpu_policy
  return "$status"
}
```

每輪保留 stdout、stderr、GNU time、exit status、完整參數、WAL data 與 case C CSV。命令不得接到會
掩蓋 exit status 的 `tee` pipeline。

## 9. Smoke 與 iteration calibration

### 9.1 Smoke

```bash
for artifact in baseline candidate; do
  run_writer "$artifact" smoke A r0 10000 || exit 1
  run_writer "$artifact" smoke B r0 10000 || exit 1
  run_writer "$artifact" smoke C r0 10000 || exit 1
done
```

六輪都必須 exit 0 且含 `correctness_verified=true`。A/B 不得有診斷欄位；C 必須有五個必要 role、
`thread_diagnostics=on`、`writer_tail_telemetry=on`，且 CSV 同時含 `sync,measured,` 與
`group_commands,measured,`。smoke 不納入效能統計。

### 9.2 決定共同 formal iterations

以 baseline／candidate 的 B/C 各跑一次短 pilot：

```bash
for artifact in baseline candidate; do
  run_writer "$artifact" iteration-pilot B r0 "$PILOT_ITERATIONS" || exit 1
  run_writer "$artifact" iteration-pilot C r0 "$PILOT_ITERATIONS" || exit 1
done
```

從四份 summary 取 `elapsed_ms`，各自計算：

```text
candidate_iterations = ceil(PILOT_ITERATIONS × 25,000 / elapsed_ms)
FORMAL_ITERATIONS = 四個 candidate_iterations 的最大值
```

設定並記錄：

```bash
FORMAL_ITERATIONS=<依 pilot 計算的正整數>
test "$FORMAL_ITERATIONS" -gt 0
printf '%s\n' "$FORMAL_ITERATIONS" \
  > "$RUN_ROOT/derived/formal-iterations.txt"
```

正式第一組 B/C pilot 若 measured duration 仍少於 20 秒，所有 overhead pilot 作廢，增加共同
`FORMAL_ITERATIONS` 後重跑；不得只延長其中一個 artifact 或 case。

## 10. Diagnostics overhead pilot

每個 artifact 都用相同 `FORMAL_ITERATIONS` 執行三組 B/C paired pilot：

```bash
for artifact in baseline candidate; do
  run_writer "$artifact" overhead B r1 "$FORMAL_ITERATIONS" || exit 1
  run_writer "$artifact" overhead C r1 "$FORMAL_ITERATIONS" || exit 1
  run_writer "$artifact" overhead C r2 "$FORMAL_ITERATIONS" || exit 1
  run_writer "$artifact" overhead B r2 "$FORMAL_ITERATIONS" || exit 1
  run_writer "$artifact" overhead B r3 "$FORMAL_ITERATIONS" || exit 1
  run_writer "$artifact" overhead C r3 "$FORMAL_ITERATIONS" || exit 1
done
```

分別對 baseline／candidate 計算 B 與 C 三輪 median：

```text
throughput_bias_percent =
  abs(median_rps_C - median_rps_B) / median_rps_B × 100%

cpu_seconds_per_million =
  (GNU-time user_seconds + system_seconds) / commands × 1,000,000

cpu_bias_percent =
  abs(median_cpu_C - median_cpu_B) / median_cpu_B × 100%
```

兩個 artifact 的 throughput bias 與 CPU bias 都必須 `<= 5%`。任一超過 5% 即停止，不執行正式
矩陣；報告結論為 `instrumentation-biased`，不能用 C case 做根因歸因，也不能挑選較有利輪次。

## 11. 正式 A／B／C 矩陣

奇數輪使用 A→B→C、baseline→candidate；偶數輪同時反轉 case 與 artifact 順序。每個 case 的
baseline／candidate 相鄰執行：

```bash
for round in 1 2 3 4 5; do
  if test $((round % 2)) -eq 1; then
    cases=(A B C)
    artifacts=(baseline candidate)
  else
    cases=(C B A)
    artifacts=(candidate baseline)
  fi

  for case_id in "${cases[@]}"; do
    for artifact in "${artifacts[@]}"; do
      run_writer "$artifact" formal "$case_id" "r$round" \
        "$FORMAL_ITERATIONS" || exit 1
    done
  done
done
```

不得因 throughput 較低、p99.9 較高、context switches 較多或出現慢 fsync 而刪除輪次，也不得補跑
第六輪。只有第 12 節的 invalid reason 才能排除；若任一 artifact/case 少於完整五輪，修正非效能
原因後重跑該 artifact/case 的五輪，不得混入舊輪次。

## 12. 每輪有效性與監測點

### 12.1 Invalid run

符合任一條件即為 invalid：

- exit status 非 0、timeout、data directory／telemetry path 重用；
- 缺少 `correctness_verified=true`，或 benchmark 自行回報 recovery、replay、completion、storage、
  telemetry、counter、normalization 錯誤；
- measured duration 少於 20 秒；
- source、index、worktree、binary、CPU policy、affinity、filesystem 或固定參數在矩陣中改變；
- B/C 的 profile sample denominator 不一致或 `profiled_groups < 50`；
- C 缺少任何必要 role，或 migration absence 沒有明確輸出 `not_measured`；
- C 的 CSV 缺少 measured sync／group-command rows，或 summary 中：
  `writer_sync_count != wal_group_commits`、
  `writer_group_sample_count != wal_group_commits`、
  `writer_group_sample_commands != wal_group_commands`。

低 throughput、高 tail latency、慢 sync、高 runqueue wait 或高 context switches 都是必須保留的觀測，
不是 invalid reason。

### 12.2 每輪必要欄位

A／B／C 共同收集：

```text
artifact, case, round, iterations, commands, elapsed_ms, commands_per_second
p50_us, p99_us, p99_9_us, max_us
wal_group_commits, wal_group_commands, actual_commands_per_group
publisher_lag_events, publisher_lag_bytes, publisher_lag_age_ns
wal_size_bytes, correctness_verified, exit_status
GNU-time user_seconds, system_seconds, voluntary/involuntary context switches, max_RSS
```

B／C 另外收集 Writer profile phase 與 `profiled_groups`。C 每個 thread role 收集：

```text
cpu_runtime_ns, cpu_seconds_per_million
runqueue_wait_ns, runqueue_wait_ns_per_million
sched_timeslices, sched_timeslices_per_million
voluntary_context_switches, voluntary_per_million
involuntary_context_switches, involuntary_per_million
cpu_migrations, migrations_per_million
```

C 的 sync tail 收集：

```text
writer_sync_count, writer_sync_p50_us, writer_sync_p99_us
writer_sync_p99_9_us, writer_sync_max_us, writer_sync_total_us
writer_sync_over_25ms, writer_sync_over_100ms, writer_sync_over_250ms
writer_group_sample_count, writer_group_sample_commands
```

需要比較 thread blocking 範圍時，只能依相同 measured window 推導：

```text
unclassified_blocked_or_sleep_ns =
  measured_elapsed_ns - cpu_runtime_ns - runqueue_wait_ns
```

只有減法不 underflow 時才可使用，並以每百萬 commands 正規化。這個值同時包含 condition wait、
sleep、I/O wait 與其他 blocking，只能稱為 `unclassified_blocked_or_sleep`，不得命名為 I/O wait。

`/usr/bin/time -v` 是 process-wide 交叉檢查，不得稱為 writer-thread counter。CPU migration 為
`not_measured` 時不得改填 0。

## 13. 矩陣結束與身份驗證

建立報告前先凍結測試結果並確認身份未變：

```bash
record_repository_identity "$RUN_ROOT/logs/repository-identity-after.txt"
diff -u "$RUN_ROOT/logs/repository-identity-before.txt" \
  "$RUN_ROOT/logs/repository-identity-after.txt"

sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" \
  > "$RUN_ROOT/logs/binary-sha256-after.txt"
diff -u "$RUN_ROOT/logs/binary-sha256-before.txt" \
  "$RUN_ROOT/logs/binary-sha256-after.txt"

capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-after.txt"
diff -u "$RUN_ROOT/logs/cpu-policy-reference.txt" \
  "$RUN_ROOT/logs/cpu-policy-after.txt"

{
  date --iso-8601=ns
  cat /proc/loadavg
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
} > "$RUN_ROOT/logs/environment-after.txt" 2>&1
```

任一 diff 非空使正式矩陣失效。報告建立本身會改變 worktree／untracked identity，因此只能在以上驗證
完成後新增報告。

## 14. 統計與判讀規則

- 每個 artifact/case 使用五輪 median 與 min–max；不得以平均值或最快輪替代。
- baseline／candidate 以同 round 計算 paired delta，並列出五輪中同方向的輪數。
- A 的 baseline/candidate 結果回答 production-like regression 是否存在。
- 同 artifact 的 A/B 差異回答 phase-profile interaction；B/C 差異只用於 diagnostics overhead。
- C 的 per-role counters 與 sync tail 必須以多數 paired rounds及 median 同方向支持結論。
- correlation 不是 causation；若多個 role 同時惡化，只能寫 contention 範圍。

最終只能選下列一項分類：

| 觀測 | 分類／下一步 |
| --- | --- |
| A 無退化、B 退化，且 overhead gate 通過 | `profile instrumentation interaction`；修 benchmark，不改 production scheduler |
| A/B 都退化，writer runqueue／involuntary／migration 多數輪同方向增加 | `writer scheduler interaction`；另立最小 scheduler／wake-up 設計 |
| 慢輪 sync p99.9／max及 writer blocked proxy 同升，runqueue 未增加 | `storage/fsync tail`；另立 storage-path 設計或環境 SLO |
| publisher／completion role 與 lag 同方向惡化 | `async worker contention`；留待 async worker 專案 |
| prepare helper CPU／runqueue 與 `wal_prepare` 同方向增加 | `prepare scheduling interaction`；另立 W=2 prepare 設計 |
| 訊號無法跨多數輪重現 | `inconclusive attribution`；不得推測性修改 production |

## 15. 報告格式與必要內容

報告檔案：

```text
docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-report.md
```

必須依下列順序撰寫。

### 15.1 結論

- `result:` 第 14 節六種分類之一，或 `instrumentation-biased`／`invalid`；
- candidate 狀態仍為 `provisional, not accepted`；
- 用 3--6 點列出支持分類的主要 paired evidence；
- 明確寫出下一步，以及本資料不能支持的 production 修改。

### 15.2 Scope、artifact 與環境

- design／procedure 路徑、執行時間、`RUN_ROOT`；
- HEAD、staged／unstaged／untracked identity before/after；
- baseline／candidate source 定義、binary SHA-256、compiler、CMake、Conan；
- CPU、SMT、affinity、governor、EPP、boost、kernel；
- filesystem、mount options、block device、可用空間；
- 未量到或不可用的 kernel counters。

### 15.3 Correctness 與有效性

- Debug、Release、ASan／UBSan 結果；
- smoke 6/6、overhead pilot 12/12 狀態；overhead gate通過時列出formal 30/30，否則明確寫
  `not run: instrumentation-biased stop`；
- measured duration range、profiled-groups range、telemetry completeness；
- invalid／excluded runs；若無則明確寫 `none`；
- 不得把低效能或慢 fsync 列為排除理由。

### 15.4 Diagnostics overhead pilot

| artifact | B median RPS | C median RPS | throughput bias | B CPU s/M | C CPU s/M | CPU bias | gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline | | | | | | | |
| candidate | | | | | | | |

### 15.5 A／B／C aggregate

每個 case 各一張表：

| metric | baseline median [min–max] | candidate median [min–max] | delta | paired direction | interpretation |
| --- | ---: | ---: | ---: | ---: | --- |
| commands/s | | | | | |
| p50 us | | | | | |
| p99 us | | | | | |
| p99.9 us | | | | | |
| max us | | | | | |
| actual commands/group | | | | | |
| CPU s/M commands | | | | | |
| voluntary/M | | | | | |
| involuntary/M | | | | | |

B/C 另列必要 Writer profile phases；不得在 A 表偽造未量測 phase。

### 15.6 Case C per-thread resource

每個 role 至少一列；數字以五輪 median `[min--max]` 表示：

| role | artifact | CPU s/M | runqueue ns/M | unclassified blocked/sleep ns/M | voluntary/M | involuntary/M | timeslices/M | migrations/M |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ob-bench` | baseline/candidate | | | | | | | |
| `ob-wr-1` | baseline/candidate | | | | | | | |
| `ob-wp-1-1` | baseline/candidate | | | | | | | |
| `ob-pub-1` | baseline/candidate | | | | | | | |
| `ob-cmp-1` | baseline/candidate | | | | | | | |

若 migrations 不可用，整欄寫 `not_measured`，不可寫 0。

### 15.7 Case C Writer sync tail

| metric | baseline median [min–max] | candidate median [min–max] | delta | paired direction |
| --- | ---: | ---: | ---: | ---: |
| sync p50 us | | | | |
| sync p99 us | | | | |
| sync p99.9 us | | | | |
| sync max us | | | | |
| sync total us | | | | |
| sync >25 ms | | | | |
| sync >100 ms | | | | |
| sync >250 ms | | | | |

同節列出 telemetry count 對帳，並說明 sync 共變只能支持 storage-tail 範圍，不能直接證明 filesystem、
device firmware 或 host 層因果。

### 15.8 根因決策與限制

- 逐項套用第 14 節 decision table；
- 只能選一項最小充分分類，證據不足則選 `inconclusive attribution`；
- process-wide `/usr/bin/time` 與 per-thread procfs 數據分開陳述；
- 列出未測 direct WAL、g8192、Engine、Publisher／Completion ceiling，避免誤稱已驗證；
- 不在本報告直接提出未經設計的 production patch。

### Appendix A：30 輪 raw summary

| case | round | artifact | RPS | p50 | p99 | p99.9 | max | actual/group | CPU s/M | involuntary/M | valid |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

### Appendix B：Case C raw resource 與 telemetry manifest

逐輪列出五個 role 的 raw／normalized counter、CSV path、CSV SHA-256、sync/group sample counts與
WAL authoritative counters。raw stdout、stderr、time、CSV 與 WAL data 只引用 `$RUN_ROOT` 路徑，
不把大型 artifacts 複製進 repository。

## 16. 完成定義

- repository correctness gate及兩個 Release artifact tests通過；
- source tree只有 `src/persistence/wal.cpp` 不同，binary與repository identity前後不變；
- diagnostics overhead pilot對兩個artifact都通過5% gate；若未通過，產出
  `instrumentation-biased`停止報告，但本根因歸因需求尚未完成；
- overhead gate通過時，30個formal runs全部有效，且每輪至少20秒；
- A/B不含diagnostics輸出，C的五個roles及telemetry count完整；
- 報告包含全部必要表格、raw artifact位置、限制與唯一根因分類；
- 未修改Git staging，未新增本需求範圍外的benchmark或production行為。
