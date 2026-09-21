# Engine Writer 統一 ceiling／根因分析 staged review 必要修正

## 1. 目的與範圍

本文件只記錄 staged review 後確認會影響量測有效性、設計一致性或失敗判定的必要修正。修正完成前，
目前 implementation 可用於編譯與 smoke test，但 runner 產生的最高 RPS 不可稱為 sustainable Engine
ceiling，也不可據此修改 production。

允許修改：

- `benchmarks/run_engine_writer_diagnostics.sh`；
- `benchmarks/test_engine_writer_diagnostics_runner.sh`；
- 必要的 benchmark CMake test wiring；
- 統一 ceiling 設計與操作文件。

不修改 matching、Engine/WAL production API、WAL format、durability、group commit、Publisher、Completion、
recovery、production metrics或production defaults；不新增C++ benchmark或hot-path telemetry。

## 2. 正式 ceiling 與 Engine tail telemetry 分離

### 問題

`run_case()`目前只要看到`engine_durable_single_instrument`，便自動啟用`EngineTailTelemetry`。collector會在
Writer處理WAL metrics時讀取時鐘、取得mutex並保存record，但external observer off/on gate兩邊都開啟
它，因此正式ceiling沒有production-like、telemetry-off baseline。

### 必要修改

1. 移除`run_case()`依workload自動附加tail CLI的行為。
2. 新增runner內部wrapper：

   ```text
   run_engine_case LABEL ITERATIONS TAIL_MODE GROUP_SIZE DELAY_US
   TAIL_MODE = off | on
   ```

   `off`不傳tail CLI；`on`才建立唯一CSV並開啟state sampling。這是runner內部介面，不增加benchmark
   public CLI。
3. external observer gate、八組frontier scan及confirmation一律使用`TAIL_MODE=off`。
4. 穩定性檢查後，把有效候選依median completed RPS由高到低排序。對最高候選執行獨立tail attribution
   gate；若不符合sustainable條件，再驗證下一候選，不得直接沿用原最高值。
5. tail attribution對同一case交錯執行三組：

   ```text
   round 1: off, on
   round 2: on, off
   round 3: off, on
   ```

6. 分別計算tail-on相對tail-off的median RPS與total CPU seconds/M commands bias：

   ```text
   absolute_bias_percent = abs(on - off) / off * 100
   ```

   任一bias超過3%時，tail資料只能定性使用，tail-on RPS不得進入ceiling median；沒有其他低偏差backlog
   證據時，結果只能是`inconclusive-attribution`。
7. production default及g4096/d1000若不是ceiling候選，各保留一組tail attribution供比較，不擴大完整
   frontier matrix。

### 驗收

- 正式frontier stdout不含`tail_telemetry=on`。
- tail-on case有唯一CSV；tail-off case沒有CSV。
- 報告中的Engine ceiling RPS只來自tail-off正式輪次。
- tail bias與external observer bias分開呈現。

## 3. 正式矩陣前校準至少30秒的measured window

### 問題

目前預設四百萬iterations。durable Engine每iteration產生兩筆command，所以只有八百萬commands；吞吐
超過約266,667 commands/s時，measured window必然短於30秒。接近1M/s時只執行約8秒，之後才被runner
判為invalid，迫使整個campaign以新run root重跑。

### 必要修改

1. 保留`--iterations`給component、WAL及Writer workload；Engine正式case使用每個
   `(group_size, delay_us)`獨立校準的iterations。
2. frontier前，對八個唯一Engine case各做一次不列入結果的短calibration：tail off、external observers
   off，其他binary、affinity、warmup、W=2、producer lanes及filesystem policy與正式case相同。
3. calibration只解析實際completed commands及`elapsed_ms`；失敗、count不符或elapsed為零即
   `invalid-run`。
4. 以40秒為正式目標，向上取整並加10%安全係數：

   ```text
   formal_iterations =
       ceil(calibration_iterations * 40000 / calibration_elapsed_ms * 1.10)
   ```

5. 同一case的scan、confirmation、tail attribution及profile off/on使用同一份已凍結iterations，不得依
   中途結果再次調整。
6. 寫入`derived/engine-iteration-plan.tsv`：

   ```text
   case calibration_iterations calibration_elapsed_ms formal_iterations
   estimated_duration_ms binary_sha256
   ```

7. 正式執行前驗證預估時間小於`case_timeout_seconds - 30`；衝突時立即`invalid-run`，不可開始長矩陣。
8. 正式case仍保留`elapsed_ms >= 30000`硬gate；calibration不能取代正式驗證。

### 驗收

- 1M commands/s會規劃至少約四千萬commands，而不是沿用八百萬commands。
- 每個正式Engine case可追溯到唯一iteration plan。
- calibration不進入frontier median、latency或observer overhead結果。
- iterations不足會在正式矩陣前被發現。

## 4. Frontier selection前完成有效性與sustainability gate

### 問題

`select_engine_frontier_case()`目前只取三輪RPS median並挑最大值，沒有排除跨輪不穩定、artifact不完整、
backlog持續成長或drain未歸零的case。

### 必要修改

1. 新增`validate_frontier_case CASE`；通過後才能進入排序。
2. 三輪正式scan每輪都必須符合：

   - exit status為0，且benchmark內建command count、completion、replay及durability驗證成功；
   - `elapsed_ms >= 30000`；
   - normalized row與必要raw artifacts存在且非空；
   - commands、completed RPS、group commits及WAL bytes可解析且為正值；
   - binary及repository identity沒有變更。

3. 對三輪completed RPS計算population CV：

   ```text
   cv_percent = standard_deviation(rps) / mean(rps) * 100
   ```

   CV大於5%的case不得參與selection；若host/storage observers同時顯示漂移，分類為
   `environment-unstable`，否則為`invalid-run`。
4. tail attribution CSV另驗證：

   - dropped samples為0，sampler error與aggregate overflow為false；
   - measured state samples至少10筆；
   - drain最後的publisher lag events、bytes及age均為0；
   - measured samples按時間分成前後四分位，最後四分位lag-events median不可高於第一四分位median加
     一個configured group size；
   - lag events對elapsed time的least-squares slope換算成30秒後，增量不可超過一個configured group
     size。

   後兩項共同排除持續線性成長；單一瞬間max不能單獨判定無界成長。
5. 第一個同時通過正式scan、CV及tail gate的候選才是observed sustainable ceiling。全部失敗時輸出
   `inconclusive-attribution`，不得退回單純最高median。
6. 分離資料收集狀態與歸因結果：

   - runner正常完成寫`collection_status=complete`；
   - `result=`只能使用設計列出的terminal classification；
   - 證據不足時寫`result=inconclusive-attribution`，不再使用`result=campaign-complete`。

### 驗收

- CV超過5%的case不會成為best case。
- backlog未歸零或線性增加的case只能稱為瞬時Writer ceiling。
- `frontier-selection.txt`包含候選排序、median、CV及排除原因。
- `logs/result.txt`只含設計允許的classification。

## 5. Observer gate採交錯順序與正規化CPU成本

### 必要修改

1. 將`off,off,off,on,on,on`改成：

   ```text
   round 1: off, on
   round 2: on, off
   round 3: off, on
   ```

   不增加輪數，也不使用隨機順序。
2. 同時解析GNU time的user及system seconds：

   ```text
   cpu_seconds_per_million =
       (user_seconds + system_seconds) / completed_commands * 1000000
   ```

3. RPS與CPU成本分別計算off/on median及absolute bias；任一超過3%即停止並分類
   `observer-biased`。
4. 任一輪無法解析completed commands、user time或system time時直接`invalid-run`，不得代入零值。

### 驗收

- case label可證明執行順序為AB／BA／AB。
- CPU gate包含system CPU並按實際completed commands正規化。
- summary保存各輪原值、median與bias。

## 6. Observer/perf失敗必須終止並補足runner tests

### Runtime failure handling

1. perf capability precheck使用正式event集合：

   ```text
   task-clock,cycles,instructions,cache-misses,
   context-switches,cpu-migrations,page-faults
   ```

   輸出含`<not supported>`或`<not counted>`時分類`environment-blocked`。
2. 另做一次短`perf record -g --call-graph fp`能力檢查；只驗證權限與artifact可讀性，不納入數據。
3. `start_observers()`保存PID、工具名稱及啟動時間。benchmark結束時：

   - observer仍存活才TERM並wait，預期的TERM status可接受；
   - observer已提前退出時保存真實status並將case標為`invalid-run`；
   - output不存在或為空同樣`invalid-run`；
   - 不得使用`wait ... || true`吞掉非預期退出。

4. fio、perf report及必要post-processing失敗時，先寫classification與failed step，再以非零status離開；
   不可只由`set -e`無分類退出。
5. trap只清理runner記錄的PID，不得按process name清理其他process。

### Fake-command tests

擴充`test_engine_writer_diagnostics_runner.sh`，以`mktemp -d`建立fake `PATH`與repository外run parent。
只有`ENGINE_WRITER_DIAGNOSTICS_TEST_MODE=1`可縮短preflight及matrix；正式模式必須拒絕這些override。fake
benchmark只輸出最小合法schema，不連結production library。

至少覆蓋：

1. shell syntax與dry-run成功。
2. run parent位於repository內時，在建立run root前失敗。
3. preflight失敗時，fake benchmark執行次數為0。
4. benchmark timeout與non-zero exit都分類`invalid-run`。
5. `mpstat`或`iostat`提前退出會使case失敗，且child已被wait。
6. exact perf events或call graph不可用時分類`environment-blocked`。
7. binary hash或repository identity在case間改變時停止後續case。
8. observer順序為AB／BA／AB。
9. tail-off正式case沒有telemetry CLI；tail-on case有唯一output path。
10. 低於30秒、CV超過5%、drain未歸零及backlog正斜率分別被拒絕。
11. runner不含`git add`、`git reset`、`git restore --staged`或`git commit`。

測試不得真的啟動perf、fio、mpstat、iostat或長時間sleep，不得建立repository內artifact或修改Git index。

### 驗收

- 每個失敗路徑都有classification、failed step及非零exit status。
- observer提前退出不會產生成功normalized row。
- CTest runner test在數秒內完成，且不需要perf權限或block device。

## 7. 文件同步

只同步以下必要內容：

1. 設計文件改為正式ceiling使用tail-off，tail telemetry為受bias gate控制的attribution run。
2. 操作文件移除固定四百萬Engine iterations範例，改為calibration與iteration-plan artifact。
3. 文件明列AB／BA／AB、total CPU seconds/M commands公式及CV gate。
4. 報告分開呈現tail-off ceiling、tail bias、backlog/drain、external observer bias、collection status及
   terminal classification。

不得藉此加入production調校或預先宣稱瓶頸。

## 8. 預估修改規模

| 區域 | 必要變動 | 預估行數 |
| --- | --- | ---: |
| runner | tail模式、calibration、validity gate、observer失敗處理 | 450--520 |
| runner tests | contract與failure cases | 35--50 |
| CMake wiring | 僅在test-mode環境確有需要時 | 0--10 |
| 設計／操作文件 | 與實作同步 | 55--70 |
| production C++ | 不修改 | 0 |

總計約540--650行修改／新增；實際變更主要集中在runner的受控矩陣與結果驗證。若需要修改production Engine、WAL、queue、worker或public API，即超出本次
修正範圍，應停止並重新review。

## 9. 完成定義

- 正式ceiling不含未經gate的benchmark-side telemetry。
- 每個正式Engine case執行前都有凍結iteration plan，實際measured window至少30秒。
- case通過correctness、duration、artifact、穩定性與sustainability gate後才能參與selection。
- observer overhead使用交錯順序及total CPU seconds/M commands。
- perf、observer與post-processing失敗不會被吞掉或產生成功結果。
- runner tests覆蓋所有必要失敗路徑。
- terminal classification與設計一致，production語意與Git staging維持不變。
