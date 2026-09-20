# WAL append publish bookkeeping Writer tail：量測有效性重測操作與報告規格

## 1. 目的與範圍

前次報告
`docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-benchmark-report.md`
因 candidate 的 B/C throughput bias 為 31.01%，依 gate 以 `instrumentation-biased` 停止；但 B 與 C
各自都出現不規則慢輪，CPU seconds/M commands 沒有同步上升，且測試期間 host load 明顯改變。因此
該結果不能區分：

1. diagnostics 本身穩定干擾 Writer；
2. host／storage 的間歇性 stall；
3. 兩者混合，仍不足以歸因。

本文件只重測 diagnostics overhead validity。最小矩陣為 baseline／candidate 各五組相鄰 B/C pair，
共 20 輪；不執行 A case，也不執行正式 A/B/C 30 輪。只有本重測得到 `valid-overhead`，後續才可另行
執行原操作檔的正式矩陣。

本流程不得修改 production code、benchmark code、WAL、fsync policy、worker、queue、CPU policy 或
Git staging；不得執行 `git add`、`git reset`、`git restore --staged`、commit 或其他改變 index 的操作。

完成後只新增：

```text
docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-overhead-retest-report.md
```

## 2. 固定 artifact 與測試條件

重用前次已通過 Debug 104/104、ASan／UBSan 104/104、baseline／candidate Release 143/143 與 smoke 6/6
的凍結 binary。不得在本重測中重新編譯或以其他 binary 代替：

```text
artifact root   /home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n
baseline SHA    888471e00e4bdc1b75109609111b3000f77b259e4ef74c25bc3cc58db2ee74ed
candidate SHA   a84d4a76082bca1a7b05bd5248d59410cf2fd578991a449ef1fe315ab2e24e35
```

固定 workload：

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
CPU affinity                     2-7
observer affinity                0-1
warmup iterations                10,000
measured iterations              2,256,950
expected measured commands       4,513,900
minimum measured duration        20 seconds
rounds                            5 adjacent B/C pairs per artifact
```

case 定義：

| case | phase profile | thread diagnostics | tail telemetry |
| --- | --- | --- | --- |
| B | on | off | off |
| C | on | on | on，唯一 CSV path |

## 3. 工具、run root 與身份凍結

在 repository root 執行：

```bash
set -euo pipefail

REPO_ROOT=/home/neojhou/repos/order_books
SOURCE_RUN_ROOT=/home/neojhou/wal-writer-tail-root-cause-ZUhTpv8n
RUN_PARENT=/home/neojhou
BENCH_CPU_SET=2-7
OBSERVER_CPU_SET=0-1
CPU_LIST=2,3,4,5,6,7
CASE_TIMEOUT_SECONDS=300
MONITOR_TIMEOUT_SECONDS=340
FORMAL_ITERATIONS=2256950
WARMUP_ITERATIONS=10000
SAMPLE_EVERY=16

BASELINE_BIN="$SOURCE_RUN_ROOT/source/baseline/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
CANDIDATE_BIN="$SOURCE_RUN_ROOT/source/candidate/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
BASELINE_SHA=888471e00e4bdc1b75109609111b3000f77b259e4ef74c25bc3cc58db2ee74ed
CANDIDATE_SHA=a84d4a76082bca1a7b05bd5248d59410cf2fd578991a449ef1fe315ab2e24e35

cd "$REPO_ROOT"
for tool in /usr/bin/time timeout taskset sha256sum findmnt lsblk \
  mpstat iostat jq awk rg stdbuf; do
  command -v "$tool" >/dev/null || exit 1
done
test -x "$BASELINE_BIN"
test -x "$CANDIDATE_BIN"
printf '%s  %s\n' "$BASELINE_SHA" "$BASELINE_BIN" | sha256sum -c -
printf '%s  %s\n' "$CANDIDATE_SHA" "$CANDIDATE_BIN" | sha256sum -c -
taskset -c "$BENCH_CPU_SET" true
taskset -c "$OBSERVER_CPU_SET" true

RUN_ROOT=$(mktemp -d "$RUN_PARENT/wal-writer-tail-overhead-retest-XXXXXXXX")
mkdir -p "$RUN_ROOT"/{data,logs,time,telemetry,monitors,preflight,derived}
printf '%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/logs/run-root.txt"

MOUNT_SOURCE=$(findmnt -no SOURCE -T "$RUN_ROOT")
BLOCK_DEVICE=$(lsblk -no PKNAME "$MOUNT_SOURCE" | awk 'NF {print; exit}')
if test -z "$BLOCK_DEVICE"; then
  BLOCK_DEVICE=$(basename "$MOUNT_SOURCE")
fi
test -n "$BLOCK_DEVICE"
printf 'mount_source=%s\nblock_device=%s\n' "$MOUNT_SOURCE" "$BLOCK_DEVICE" \
  > "$RUN_ROOT/logs/storage-target.txt"
```

建立報告以前，repository identity 必須保持不變：

```bash
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
sha256sum "$BASELINE_BIN" "$CANDIDATE_BIN" \
  > "$RUN_ROOT/logs/binary-sha256-before.txt"
```

## 4. CPU policy 與環境紀錄

不主動改 governor、EPP、boost、I/O scheduler 或 mount options；只驗證測試前後一致：

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

capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-reference.txt"

{
  date --iso-8601=ns
  uname -a
  lscpu
  findmnt -T "$RUN_ROOT"
  df -h "$RUN_ROOT"
  lsblk -o NAME,KNAME,PKNAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS
  cat /proc/loadavg
  capture_cpu_policy
  ps -eo pid,tid,psr,stat,comm,%cpu,%mem --sort=-%cpu | head -n 30
} > "$RUN_ROOT/logs/environment-before.txt" 2>&1
```

## 5. 每組 pair 的 30 秒 idle preflight

每組 B/C pair 前量測 30 秒；observer 固定在 CPU 0-1。門檻只用來決定是否開始 pair，不能等待多次直到
偶然通過，也不能事後刪除已完成的慢輪：

- CPU 2-7 中，每個 CPU 的 30 秒平均 idle 最低值必須 `>= 90%`；
- CPU 2-7 所有 sample 的 iowait 最大值必須 `<= 5%`；
- WAL block device 的 30 秒平均 util 必須 `<= 5%`；
- WAL block device 的 aqu-sz 最大值必須 `<= 0.25`。

```bash
idle_preflight() {
  label=$1
  cpu_json="$RUN_ROOT/preflight/$label-mpstat.json"
  disk_json="$RUN_ROOT/preflight/$label-iostat.json"
  summary="$RUN_ROOT/preflight/$label-summary.txt"

  taskset -c "$OBSERVER_CPU_SET" mpstat -P "$CPU_LIST" 1 30 -o JSON \
    > "$cpu_json" &
  cpu_pid=$!
  taskset -c "$OBSERVER_CPU_SET" iostat -y -dx "$BLOCK_DEVICE" 1 30 -o JSON \
    > "$disk_json" &
  disk_pid=$!
  wait "$cpu_pid"
  wait "$disk_pid"

  cpu_idle_min=$(jq -r '
    [.sysstat.hosts[0].statistics[]["cpu-load"][]]
    | sort_by(.cpu) | group_by(.cpu)
    | map(map(.idle) | add / length) | min' "$cpu_json")
  cpu_iowait_max=$(jq -r '
    [.sysstat.hosts[0].statistics[]["cpu-load"][] | .iowait] | max' \
    "$cpu_json")
  disk_util_avg=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .util] | add / length' \
    "$disk_json")
  disk_aqu_max=$(jq --arg device "$BLOCK_DEVICE" -r '
    [.sysstat.hosts[0].statistics[].disk[]
      | select(.disk_device == $device) | .["aqu-sz"]] | max' \
    "$disk_json")

  printf 'cpu_idle_min=%s\ncpu_iowait_max=%s\ndisk_util_avg=%s\ndisk_aqu_max=%s\n' \
    "$cpu_idle_min" "$cpu_iowait_max" "$disk_util_avg" "$disk_aqu_max" \
    > "$summary"

  awk -v value="$cpu_idle_min" 'BEGIN { exit !(value >= 90.0) }'
  awk -v value="$cpu_iowait_max" 'BEGIN { exit !(value <= 5.0) }'
  awk -v value="$disk_util_avg" 'BEGIN { exit !(value <= 5.0) }'
  awk -v value="$disk_aqu_max" 'BEGIN { exit !(value <= 0.25) }'
}
```

任一 preflight 失敗即停止整次重測，結果記為 `preflight-busy`。先排除外部工作，再建立全新的
`RUN_ROOT` 從 r1 重跑；不得保留已完成輪次與新輪次混合。

## 6. 每輪監測與 benchmark 執行函式

每輪用 1 秒粒度的 `mpstat`／`iostat` 留下 host 與 block-device 時序。observer 與 benchmark CPU
隔離；兩個 monitor 在 B、C 都啟用，因此不會只加在 diagnostics case。

```bash
run_writer() {
  artifact=$1
  case_id=$2
  round=$3

  if test "$artifact" = baseline; then
    bench_bin=$BASELINE_BIN
  elif test "$artifact" = candidate; then
    bench_bin=$CANDIDATE_BIN
  else
    return 2
  fi

  prefix="$artifact-overhead-retest-case${case_id}-$round"
  case_data="$RUN_ROOT/data/$prefix"
  case_stdout="$RUN_ROOT/logs/$prefix.stdout"
  case_stderr="$RUN_ROOT/logs/$prefix.stderr"
  case_time="$RUN_ROOT/time/$prefix.time"
  case_status="$RUN_ROOT/logs/$prefix.status"
  case_meta="$RUN_ROOT/logs/$prefix.meta"
  cpu_monitor="$RUN_ROOT/monitors/$prefix-mpstat.txt"
  disk_monitor="$RUN_ROOT/monitors/$prefix-iostat.txt"

  args=(--workload=engine_writer_hot_path_profile
        --engine-group-size=4096
        --engine-group-delay-us=1000
        --engine-producer-lanes=8192
        --wal-prepare-workers=2
        --wal-parallel-prepare-min-commands=4096
        --writer-phase-profile=on
        --writer-profile-sample-every="$SAMPLE_EVERY"
        --writer-apply-subprofile=off)

  case "$case_id" in
    B)
      ;;
    C)
      telemetry="$RUN_ROOT/telemetry/$prefix.csv"
      test ! -e "$telemetry" || return 2
      args+=(--writer-thread-diagnostics=on
             --writer-tail-telemetry-output="$telemetry")
      ;;
    *)
      return 2
      ;;
  esac

  test ! -e "$case_data"
  test ! -e "$case_stdout"
  printf 'artifact=%s case=%s round=%s iterations=%s args=' \
    "$artifact" "$case_id" "$round" "$FORMAL_ITERATIONS" > "$case_meta"
  printf '%q ' "${args[@]}" >> "$case_meta"
  printf '\nstart=' >> "$case_meta"
  date --iso-8601=ns >> "$case_meta"

  capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-current.txt"
  diff -u "$RUN_ROOT/logs/cpu-policy-reference.txt" \
    "$RUN_ROOT/logs/cpu-policy-current.txt" >/dev/null

  taskset -c "$OBSERVER_CPU_SET" stdbuf -oL \
    timeout "${MONITOR_TIMEOUT_SECONDS}s" mpstat -P "$CPU_LIST" 1 \
    > "$cpu_monitor" 2>&1 &
  cpu_monitor_pid=$!
  taskset -c "$OBSERVER_CPU_SET" stdbuf -oL \
    timeout "${MONITOR_TIMEOUT_SECONDS}s" iostat -y -dx -t "$BLOCK_DEVICE" 1 \
    > "$disk_monitor" 2>&1 &
  disk_monitor_pid=$!

  if LC_ALL=C /usr/bin/time -v -o "$case_time" \
      timeout --signal=TERM --kill-after=30s "${CASE_TIMEOUT_SECONDS}s" \
      taskset -c "$BENCH_CPU_SET" "$bench_bin" \
        --iterations="$FORMAL_ITERATIONS" --warmup="$WARMUP_ITERATIONS" \
        --data-dir="$case_data" "${args[@]}" \
        > "$case_stdout" 2> "$case_stderr"; then
    status=0
  else
    status=$?
  fi

  kill "$cpu_monitor_pid" "$disk_monitor_pid" 2>/dev/null || true
  wait "$cpu_monitor_pid" 2>/dev/null || true
  wait "$disk_monitor_pid" 2>/dev/null || true
  printf '%s\n' "$status" > "$case_status"
  printf 'end=' >> "$case_meta"
  date --iso-8601=ns >> "$case_meta"

  capture_cpu_policy > "$RUN_ROOT/logs/cpu-policy-current.txt"
  diff -u "$RUN_ROOT/logs/cpu-policy-reference.txt" \
    "$RUN_ROOT/logs/cpu-policy-current.txt" >/dev/null
  return "$status"
}

run_pair() {
  artifact=$1
  first_case=$2
  second_case=$3
  round=$4

  idle_preflight "$artifact-$round" || {
    printf 'preflight-busy artifact=%s round=%s\n' "$artifact" "$round" \
      > "$RUN_ROOT/logs/stop-reason.txt"
    return 1
  }
  run_writer "$artifact" "$first_case" "$round" || return 1
  run_writer "$artifact" "$second_case" "$round" || return 1
}
```

`kill` 只終止本函式剛啟動的 observer PID，不碰 benchmark、其他使用者 process 或 repository。

## 7. 五組配對執行順序

奇偶輪同時反轉 artifact 與 B/C 順序，降低固定順序、溫度與時間漂移造成的偏差。相同 artifact 的 B/C
必須相鄰，中間不能執行其他 benchmark：

```bash
run_pair baseline  B C r1
run_pair candidate C B r1

run_pair candidate B C r2
run_pair baseline  C B r2

run_pair baseline  B C r3
run_pair candidate C B r3

run_pair candidate B C r4
run_pair baseline  C B r4

run_pair baseline  B C r5
run_pair candidate C B r5
```

共 20 輪，全部必須 exit 0、`correctness_verified=true` 且 measured duration `>= 20 s`。B 不得輸出
diagnostics 欄位；C 必須有五個 role、完整 CSV，並符合：

```text
writer_sync_count = wal_group_commits
writer_group_sample_count = wal_group_commits
writer_group_sample_commands = wal_group_commands
```

慢 throughput、高 latency、慢 fsync 或執行期間的 host/storage spike 都是有效觀測，不得刪除、補跑或
以第六輪取代。只有 exit failure、timeout、correctness failure、檔案重用、固定參數改變或 telemetry
不完整屬於 invalid；任一 invalid 時整個 artifact 的五組 pair 必須用新路徑完整重跑。

## 8. 統計與判定門檻

每輪從 stdout 與 GNU time 擷取：

```text
commands_per_second, elapsed_ms, p50_us, p99_us, p99_9_us, max_us
wal_group_commits, wal_group_commands, actual_commands_per_group
profiled_groups, correctness_verified
user_seconds, system_seconds, voluntary_context_switches
involuntary_context_switches, max_RSS
```

計算：

```text
CPU s/M = (user_seconds + system_seconds) / commands * 1,000,000
paired throughput bias = abs(C_RPS - B_RPS) / B_RPS * 100%
paired CPU bias = abs(C_CPU_s_per_M - B_CPU_s_per_M) / B_CPU_s_per_M * 100%
case RPS spread = (max_RPS - min_RPS) / median_RPS * 100%
case CPU spread = (max_CPU_s_per_M - min_CPU_s_per_M) / median_CPU_s_per_M * 100%
```

不得挑除 outlier，使用五輪 median、min--max 與五個 paired bias。依下列優先序只選一個結果：

| 優先序 | 條件 | 結果與下一步 |
| ---: | --- | --- |
| 1 | preflight 未通過 | `preflight-busy`；不產生效能結論，清除外部負載後全新重跑 |
| 2 | 任一必要 run invalid | `invalid`；修正非效能問題後重跑完整五組 pair |
| 3 | 任一 artifact/case 的 RPS spread >10% 或 CPU spread >5%，或 monitor 顯示慢輪與 host/device spike 共變 | `environment-unstable`；不跑正式矩陣、不改 production |
| 4 | B 穩定、C 穩定，且任一 artifact 至少 4/5 pair 的 throughput 或 CPU bias >5%，方向一致為 C 較差 | `diagnostics-biased`；先降低 diagnostics 侵入性 |
| 5 | 兩個 artifact 的 median paired throughput/CPU bias 均 <=5%，且各自至少 4/5 pair 的兩項 bias 均 <=5% | `valid-overhead`；允許另行執行正式 A/B/C 矩陣 |
| 6 | 通過穩定性門檻但不符合以上一致方向 | `retest-inconclusive`；不跑正式矩陣、不推測 production 根因 |

`environment-unstable` 的判斷不能只看 `/proc/loadavg`；必須引用 pair preflight、每輪 mpstat/iostat 或
C 的 sync tail。C 的 sync tail 能指出 instrumented run 發生 storage stall，但不能單獨證明是 filesystem、
device firmware 或其他 host workload。

## 9. 結束驗證

建立報告前執行；任一 identity、binary 或 CPU policy diff 非空，整次重測為 `invalid`：

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
  ps -eo pid,tid,psr,stat,comm,%cpu,%mem --sort=-%cpu | head -n 30
} > "$RUN_ROOT/logs/environment-after.txt" 2>&1
```

報告建立會改變 untracked identity，因此必須在上述驗證完成後才新增報告。

## 10. 報告格式與必要內容

報告檔案：

```text
docs/wal-append-publish-bookkeeping-writer-tail-root-cause-analysis-overhead-retest-report.md
```

### 10.1 結論

- `result:` 只能是第 8 節六種結果之一；
- candidate 仍為 `provisional, not accepted`；
- 以 3--6 點列出判定所需證據；
- 明確寫出是否允許執行正式 A/B/C 矩陣；
- 明確寫出本重測不能直接支持任何 production 修改。

### 10.2 Artifact、環境與完整性

- procedure、前次報告、執行時間與 `RUN_ROOT`；
- baseline／candidate binary path 與 SHA-256；
- HEAD、cached/worktree/untracked identity before/after；
- CPU、SMT、affinity、governor、EPP、boost；
- filesystem、mount、block device；
- 20/20 run、correctness、duration、telemetry 與 identity gate 結果。

### 10.3 Preflight 表

| artifact | round | CPU idle min | CPU iowait max | disk util avg | disk aqu max | gate |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| baseline/candidate | r1--r5 | | | | | pass/fail |

### 10.4 五組 paired raw result

| artifact | round | order | B RPS | C RPS | throughput bias | B CPU s/M | C CPU s/M | CPU bias | valid |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| baseline/candidate | r1--r5 | B-C/C-B | | | | | | | |

### 10.5 Aggregate 與 gate

| artifact | case | RPS median [min--max] | RPS spread | CPU s/M median [min--max] | CPU spread | stable |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| baseline/candidate | B/C | | | | | |

| artifact | median paired throughput bias | pairs <=5% | median paired CPU bias | pairs <=5% | overhead gate |
| --- | ---: | ---: | ---: | ---: | --- |
| baseline | | /5 | | /5 | |
| candidate | | /5 | | /5 | |

### 10.6 Host/storage 共變

逐輪列出 benchmark p99.9/max、C sync p99.9/max/`>100 ms` 次數，以及相同時間窗的 CPU idle/iowait、
device util/aqu-sz/await peak。必須區分：

- CPU 使用量增加；
- runqueue wait；
- unclassified blocked/sleep；
- storage sync tail；
- 無法分類的外部噪聲。

不得把 `elapsed - CPU - runqueue` 稱為 I/O wait，也不得只因 fsync tail 共變就宣稱 device firmware
是根因。

### 10.7 決策與下一步

逐項套用第 8 節優先序。只有 `valid-overhead` 可以建議執行原設計的正式 A/B/C 矩陣；其他結果都要
停在對應的量測或環境問題，不得順便提出 WAL、scheduler、publisher 或 completion production patch。

### Appendix A：20 輪 raw summary

| artifact | case | round | RPS | elapsed | p50 | p99 | p99.9 | max | actual/group | CPU s/M | involuntary/M | valid |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |

### Appendix B：Monitor 與 telemetry manifest

逐輪列出 stdout、stderr、GNU time、mpstat、iostat、C CSV 路徑與 SHA-256；C 另列五個 thread role、
sync/group sample counts及 WAL authoritative counters。大型 artifacts 只保留在 `$RUN_ROOT`，不複製進
repository。

## 11. 完成定義

- 固定 binary hash、workload、iterations、affinity 與 CPU policy 全程未變；
- 每組 pair 開始前通過一次且僅一次 30 秒 idle preflight；
- baseline／candidate 各完成五組相鄰且順序交錯的 B/C pair；
- 20 輪原始資料全部保留，沒有刪除慢輪或補跑較有利輪次；
- 依穩定性與 paired overhead gate 得到唯一結果；
- 報告包含必要表格、artifact manifest、限制及下一步；
- 未執行正式 A/B/C 矩陣、未修改 production code或Git staging。
