# WAL record cache 最小調整：單輪壓測操作

## 1. 目的與範圍

本流程只驗證 `records_` 改為分段成長容器後，既有 WAL append-return case 能否在 idle
環境下正常完成，並記錄吞吐、append latency 與 CPU／I/O 狀態。

固定情境：

```text
workload                    wal_write_ceiling
batch                       8192 commands
prepare workers             2
parallel prepare threshold  4096 commands
sync                        none
segment size                4 GiB
measured commands           16,777,216
measured groups             2,048
rotation                    0
```

本次只跑一輪，不執行 Engine、Publisher、Completion、`fsync`、`fio` 或 prepare-buffer
第二階段實驗。單輪結果是 spot check，不是 production ceiling，也不能取代同環境的正式
baseline／candidate 配對比較。

## 2. 建立 run root

在 repository root 執行：

```bash
set -o pipefail

REPO=/home/neojhou/repos/order_books
BENCH_BIN="$REPO/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
RUN_ROOT=$(mktemp -d /home/neojhou/wal-record-cache-spot-XXXXXXXX)
BENCH_CPUS=2-7

test -x "$BENCH_BIN"
command -v mpstat iostat taskset setsid /usr/bin/time sha256sum >/dev/null
taskset -c "$BENCH_CPUS" true
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data"

git -C "$REPO" status --short > "$RUN_ROOT/logs/git-status.txt"
git -C "$REPO" rev-parse HEAD > "$RUN_ROOT/logs/git-head.txt"
sha256sum "$BENCH_BIN" > "$RUN_ROOT/logs/binary-sha256.txt"
uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"

printf 'RUN_ROOT=%s\n' "$RUN_ROOT"
```

流程不修改 Git index、sysctl、CPU governor、IRQ affinity、cgroup 或 filesystem 設定。

## 3. Idle preflight

先確認沒有其他壓測程序：

```bash
ps -eo pid=,comm=,args= \
  | awk '$2 == "order_books_benchmark" || $2 == "fio" || $2 == "strace"' \
  > "$RUN_ROOT/logs/residual-processes.txt" || true
cat "$RUN_ROOT/logs/residual-processes.txt"
```

若存在其他仍在執行的 benchmark、`fio` 或 `strace`，本輪停止，不自動終止程序。

收集十秒 idle 狀態：

```bash
mpstat -P "$BENCH_CPUS" 1 10 > "$RUN_ROOT/logs/preflight-mpstat.txt"
iostat -y -x 1 10 > "$RUN_ROOT/logs/preflight-iostat.txt"
df -B1 "$RUN_ROOT" > "$RUN_ROOT/logs/preflight-df.txt"

cat "$RUN_ROOT/logs/preflight-mpstat.txt"
cat "$RUN_ROOT/logs/preflight-iostat.txt"
cat "$RUN_ROOT/logs/preflight-df.txt"
```

符合以下條件才開始：

- affinity CPUs 平均 idle 至少 `90%`；
- 平均 iowait 不超過 `5%`；
- WAL 所在裝置平均 `%util` 不超過 `5%`、平均 `aqu-sz` 不超過 `0.25`；單一瞬間尖峰只記錄，
  連續三個 sample 超標才視為 busy；
- run root 所在 filesystem 至少有 `5 GiB` 可用空間。

若不合格，結果記為 `PREFLIGHT-BUSY`，保留 run root 並停止。不要在同一個 run root 補跑。

## 4. 執行一次壓測

啟動讀取型 observer，再執行一次 benchmark：

```bash
MPSTAT_PID=
IOSTAT_PID=

cleanup_observers() {
  for pid in "${MPSTAT_PID:-}" "${IOSTAT_PID:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}

trap cleanup_observers EXIT INT TERM

mpstat -P "$BENCH_CPUS" 1 > "$RUN_ROOT/logs/workload-mpstat.txt" &
MPSTAT_PID=$!
iostat -y -x 1 > "$RUN_ROOT/logs/workload-iostat.txt" &
IOSTAT_PID=$!

setsid /usr/bin/time -v -o "$RUN_ROOT/logs/benchmark-time.txt" \
  taskset -c "$BENCH_CPUS" "$BENCH_BIN" \
    --workload=wal_write_ceiling \
    --wal-group-size=8192 \
    --wal-sync=none \
    --wal-phase-profile=off \
    --wal-prepare-workers=2 \
    --wal-parallel-prepare-min-commands=4096 \
    --wal-segment-size-bytes=4294967296 \
    --wal-no-rotation-epoch-commands=16777216 \
    --iterations=2048 \
    --warmup=4 \
    --data-dir="$RUN_ROOT/data/w2-b8192" \
    > "$RUN_ROOT/logs/benchmark.txt" 2>&1
BENCH_STATUS=$?

cleanup_observers
MPSTAT_PID=
IOSTAT_PID=
trap - EXIT INT TERM

printf '%s\n' "$BENCH_STATUS" > "$RUN_ROOT/logs/benchmark.status"
cat "$RUN_ROOT/logs/benchmark.txt"
cat "$RUN_ROOT/logs/benchmark-time.txt"
```

observer 涵蓋 benchmark setup、measured window 與 replay，因此 CPU／I/O 數據只能作為環境與
資源佐證，不應當成精確的 phase attribution。

## 5. 有效性與判讀

有效結果必須同時符合：

- exit status 為 `0`；
- measured commands 為 `16,777,216`；
- `replay_verified=true`；
- `wal_byte_plan_verified=true`；
- measured segment rotations 為 `0`；
- measured WAL sync calls 為 `0`；
- parallel prepare groups 與 prepare tasks 都大於 `0`；
- workload 期間沒有持續的外部 CPU 或 I/O 壓力。

結果分類：

- `VALID`：有效性條件全部成立；
- `BELOW-TARGET`：有效，但 service throughput 低於 `1.53M commands/s`；
- `PREFLIGHT-BUSY`：測試前環境不合格，未執行 benchmark；
- `INVALID`：benchmark 失敗、correctness gate 失敗或 measured window 被持續外部負載干擾。

可以將 p99.9／max 與先前 `676,690.794 us`／`1,483,040 us` 作歷史方向性比較，但因不是同一
環境的配對輪，不能據此宣稱 record-cache growth 已被證明或排除。正式保留決策仍需另做同環境
baseline／candidate 配對。

## 6. 報告格式

結果寫入：

```text
docs/wal-record-cache-and-prepare-buffer-minimal-adjustment-benchmark-report.md
```

使用以下格式：

```markdown
# WAL record cache 最小調整單輪壓測報告

## 結論

- Result：VALID／BELOW-TARGET／PREFLIGHT-BUSY／INVALID
- 一句話說明本輪 throughput、tail 與環境是否有效。
- 明確註明：單輪不能證明因果或 production ceiling。

## 環境與有效性

| 項目 | 結果 |
| --- | --- |
| Run root | ... |
| Git HEAD／worktree | ... |
| Binary SHA-256 | ... |
| CPU affinity | 2-7 |
| Preflight CPU idle／iowait | ... |
| Preflight disk util／aqu-sz | ... |
| Exit status | ... |
| Replay／byte plan | ... |
| Rotations／sync calls | ... |

## 壓測結果

| 指標 | 結果 |
| --- | ---: |
| Measured commands／groups | ... |
| Wall commands/s | ... |
| Service commands/s | ... |
| WAL MiB/s | ... |
| Append p50／p99／p99.9／max | ... |
| Parallel groups／prepare tasks | ... |

## 資源觀測

| 指標 | 結果 |
| --- | ---: |
| User／system CPU seconds | ... |
| Voluntary／involuntary context switches | ... |
| Max RSS | ... |
| Workload CPU idle／iowait | ... |
| Workload disk util／aqu-sz | ... |

## 判讀與限制

- 是否達到 1.53M commands/s。
- p99.9／max 相較歷史值的方向，但不得寫成配對改善百分比。
- 是否觀察到 CPU 飽和或 storage/writeback 干擾。
- 下一步只能是正式配對驗證，或依無效原因重跑；本報告不啟動 prepare-buffer 第二階段。
```

所有原始 stdout、GNU time、preflight 與 workload observer 檔案保留在 run root；確認報告完成前
不要刪除。
