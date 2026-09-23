# WAL prepare／rotation 最小實驗：單輪壓測操作

## 1. 目的與限制

本流程只做一次低干擾的 spot check：在排除 fsync 與 segment rotation 後，觀察
`W=2` 的 WAL prepare／append-return 吞吐、tail latency 與資源使用情況。benchmark 會寫入
measured-window marker，讓外部 CPU／I/O observer 不把 setup 與 replay 當成 append window。

固定條件：

```text
workload                       wal_write_ceiling
batch                          8192 commands
prepare workers                2
parallel prepare threshold     4096 commands
sync                           none
segment size                   4 GiB
no-rotation epoch              16,777,216 commands
measured groups                2,048
measured commands              16,777,216
```

這是一輪確認性壓測，不足以宣稱正式 ceiling、比較 W=1／2／4，或判斷 256 MiB rotation
的影響。主要參考門檻為 append-return `1.53M commands/s`；未達門檻只能視為需要進一步
診斷，不能直接歸因於 CPU、page cache 或 storage。

## 2. 前置設定

在 repository root 執行：

```bash
set -o pipefail

REPO=/home/neojhou/repos/order_books
BENCH_BIN="$REPO/build/ReleaseBenchmark/benchmarks/order_books_benchmark"
RUN_ROOT=$(mktemp -d /home/neojhou/wal-prepare-spot-XXXXXXXX)
BENCH_CPUS=2-7
MEASUREMENT_MARKER="$RUN_ROOT/logs/measured-window.marker"

test -x "$BENCH_BIN"
command -v mpstat iostat taskset setsid /usr/bin/time >/dev/null
taskset -c "$BENCH_CPUS" true
mkdir -p "$RUN_ROOT/logs" "$RUN_ROOT/data"

git -C "$REPO" status --short > "$RUN_ROOT/logs/git-status.txt"
sha256sum "$BENCH_BIN" > "$RUN_ROOT/logs/binary-sha256.txt"
uname -a > "$RUN_ROOT/logs/environment.txt"
lscpu >> "$RUN_ROOT/logs/environment.txt"
findmnt -T "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"
df -h "$RUN_ROOT" >> "$RUN_ROOT/logs/environment.txt"

printf 'RUN_ROOT=%s\n' "$RUN_ROOT"
```

本流程不修改 Git index、sysctl、CPU governor、IRQ affinity、cgroup 或 filesystem 設定。

## 3. Idle preflight

先確認沒有殘留 workload：

```bash
ps -eo pid=,comm=,args= \
  | awk '$2 == "order_books_benchmark" || $2 == "fio" || $2 == "strace"' \
  > "$RUN_ROOT/logs/residual-processes.txt" || true
cat "$RUN_ROOT/logs/residual-processes.txt"
```

若輸出包含其他仍在執行的 benchmark、`fio` 或 `strace`，停止本輪，不自動終止程序。

再收集十秒 CPU 與 storage 狀態：

```bash
mpstat -P "$BENCH_CPUS" 1 10 > "$RUN_ROOT/logs/preflight-mpstat.txt"
iostat -y -x 1 10 > "$RUN_ROOT/logs/preflight-iostat.txt"
df -B1 "$RUN_ROOT" > "$RUN_ROOT/logs/preflight-df.txt"

cat "$RUN_ROOT/logs/preflight-mpstat.txt"
cat "$RUN_ROOT/logs/preflight-iostat.txt"
cat "$RUN_ROOT/logs/preflight-df.txt"
```

只有下列條件都成立才執行壓測：

- affinity CPU 的平均 idle 均至少 `90%`；
- CPU iowait 不超過 `5%`；
- WAL 所在裝置平均 `%util` 不超過 `5%`，平均 `aqu-sz` 不超過 `0.25`；單一瞬間尖峰只記錄；
- `RUN_ROOT` 所在 filesystem 至少有 `5 GiB` 可用空間。

任一條件不合格，結果記為 `preflight-busy` 並停止；不要在同一個 run root 稍後補跑。

## 4. 執行一次壓測

本輪 marker 模式要求所有 measured commands 位於單一 epoch；本例
`8192 × 2048 = 16,777,216` commands，因此 epoch 上限必須設為 `16777216`。先啟動
benchmark；收到 `measured-window-start` marker 後才啟動讀取型 CPU／I/O observers，收到
`measured-window-end` marker 後立即停止 observers。這樣 setup 與 replay 不會被當成 measured
append window：

```bash
BENCH_PID=
MPSTAT_PID=
IOSTAT_PID=

stop_background_process() {
  local pid=${1:-}
  if [[ -z "$pid" ]]; then
    return
  fi
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

stop_benchmark_process_group() {
  local leader=${1:-}
  if [[ -z "$leader" ]]; then
    return
  fi

  kill -TERM -- "-$leader" 2>/dev/null || true
  wait "$leader" 2>/dev/null || true
}

cleanup_background_processes() {
  stop_background_process "${MPSTAT_PID:-}"
  stop_background_process "${IOSTAT_PID:-}"
  stop_benchmark_process_group "${BENCH_PID:-}"
}

trap cleanup_background_processes EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

setsid /usr/bin/time -v -o "$RUN_ROOT/logs/benchmark-time.txt" \
  taskset -c "$BENCH_CPUS" "$BENCH_BIN" \
    --workload=wal_write_ceiling \
    --wal-group-size=8192 \
    --wal-sync=none \
    --wal-phase-profile=off \
    --wal-prepare-workers=2 \
    --wal-parallel-prepare-min-commands=4096 \
    --wal-segment-size-bytes=4294967296 \
    --wal-measurement-marker="$MEASUREMENT_MARKER" \
    --wal-no-rotation-epoch-commands=16777216 \
    --iterations=2048 \
    --warmup=4 \
    --data-dir="$RUN_ROOT/data/w2-b8192" \
    > "$RUN_ROOT/logs/benchmark.txt" 2>&1 &
BENCH_PID=$!

while kill -0 "$BENCH_PID" 2>/dev/null && \
      ! grep -q '^measured-window-start$' "$MEASUREMENT_MARKER" 2>/dev/null; do
  sleep 0.05
done

if grep -q '^measured-window-start$' "$MEASUREMENT_MARKER" 2>/dev/null; then
  mpstat -P "$BENCH_CPUS" 1 > "$RUN_ROOT/logs/workload-mpstat.txt" &
  MPSTAT_PID=$!
  iostat -y -x 1 > "$RUN_ROOT/logs/workload-iostat.txt" &
  IOSTAT_PID=$!

  while kill -0 "$BENCH_PID" 2>/dev/null && \
        ! grep -q '^measured-window-end$' "$MEASUREMENT_MARKER" 2>/dev/null; do
    sleep 0.05
  done

  stop_background_process "$MPSTAT_PID"
  MPSTAT_PID=
  stop_background_process "$IOSTAT_PID"
  IOSTAT_PID=
else
  printf '%s\n' marker-not-observed > "$RUN_ROOT/logs/observer.status"
fi

wait "$BENCH_PID"
BENCH_STATUS=$?
BENCH_PID=

trap - EXIT INT TERM

printf '%s\n' "$BENCH_STATUS" > "$RUN_ROOT/logs/benchmark.status"

cat "$RUN_ROOT/logs/benchmark.txt"
cat "$RUN_ROOT/logs/benchmark-time.txt"
```

若 benchmark 被中斷，也要先停止兩個 observer，再保存 run root。不要把失敗或中斷結果當成
有效 throughput。

## 5. 有效性與判讀

有效結果必須同時符合：

- exit status 為 `0`；
- `replay_verified=true`；
- `wal_byte_plan_verified=true`；
- `measured_segment_rotations=0`；
- `measured_wal_sync_calls=0`；
- `epochs=1`；
- `segment_size_bytes=4294967296`；
- `actual_parallel_prepare_groups` 與 `actual_prepare_tasks` 均大於 `0`；
- marker 檔案各有一次 `measured-window-start` 與 `measured-window-end`；
- observer 在 start marker 後啟動，且在 end marker 後停止；
- workload 期間沒有明顯的外部 CPU 或 I/O 干擾。

核心判讀：

- `commands_per_second >= 1.53M`：W=2 單輪結果支持 worker-only hypothesis，可再安排正式多輪比較；
- `< 1.53M` 且 CPU 接近飽和：prepare／serialize CPU 成本仍可能是主因；
- `< 1.53M` 且 iowait、`aqu-sz`、`%util` 或 writeback 同時升高：標記為 storage/writeback interference；
- 證據互相矛盾或環境不再 idle：標記 `inconclusive`。

單輪數字只能稱為 `standalone-observation`，不能宣稱 production SLA 或正式 ceiling。

## 6. 報告格式

結果寫入：

```text
docs/wal-prepare-rotation-minimal-experiment-benchmark-report.md
```

使用以下簡短格式：

```markdown
# WAL prepare／rotation 最小實驗報告

## 結論

- Result：PASS／BELOW-TARGET／PREFLIGHT-BUSY／INCONCLUSIVE
- 一句話說明是否支持 W=2 worker-only hypothesis。

## 環境與有效性

| 項目 | 結果 |
| --- | --- |
| Run root | ... |
| Binary SHA-256 | ... |
| CPU affinity | 2-7 |
| Preflight CPU idle／iowait | ... |
| Preflight disk util／aqu-sz | ... |
| Exit status | ... |
| replay／byte plan／zero rotation | ... |

## 壓測結果

| 指標 | 結果 |
| --- | ---: |
| Commands | ... |
| Wall commands/s | ... |
| Service commands/s | ... |
| WAL MiB/s | ... |
| Append p50／p99／p99.9／max | ... |
| User／system CPU seconds | ... |
| Context switches | ... |
| Max RSS | ... |
| Parallel groups／prepare tasks | ... |

## 資源觀測與判讀

- workload CPU idle／iowait：...
- workload disk util／aqu-sz／await：...
- measured-window marker：...
- 與 1.53M commands/s 門檻的差距：...
- 結論限制：單輪 standalone observation，不是正式 ceiling。
```

報告完成後可刪除 `RUN_ROOT/data/w2-b8192` 以回收約數 GiB 空間，但應保留 `logs/` 與報告引用的
run root。刪除前必須再次確認目標是本輪 `RUN_ROOT` 下的明確 data path。
