# Order Book unstaged changes 必要修正方案

狀態：U01～U05 已套用；完整 Definition of Done 與全樹格式化仍未完成
審查基線：`docs/Order-book-spec.md`、`docs/order-book-design.md`
審查範圍：本文件建立時相對 index 的 unstaged 與 untracked changes
目的：只修正會造成契約不一致、錯誤 operational behavior、無效驗證或明確冗餘的內容

## 1. 結論與範圍

目前變更的整體架構方向符合需求，沒有加入 network、broker、HA、risk、account、額外 order type 或新第三方依賴。WAL、OrderBook atomicity、configuration manifest、publisher、metrics 與 benchmark 都能追溯至既有需求。

但目前仍不能判定為「剛好滿足設計」：有四項必要 correctness／驗證修正，以及數項應移除的明確冗餘。另外，需求第 20 節的完整驗收矩陣尚未完成；這部分不得用修改設計文字取代實際測試。

本文件分成兩個交付層級：

1. **本輪最小必要修正**：第 2～7 節，約 300～450 行變動，完成後才適合再次 review 目前 unstaged implementation。
2. **完整 Definition of Done**：第 8 節，預估另需 1,200～2,000 行 tests／test support；若本輪不做，文件必須維持「核心修正可驗證、完整驗收未完成」的狀態，不得宣稱整體需求完成。

不得順便加入通用 dependency-injection framework、另一份 event log、監控 exporter、broker adapter、lock-free queue 或新的 benchmark dependency。

## 2. FIX-U01：維持同一 Producer stream 的 completion ordering

### 問題

`ShardRuntime::process_command_batch()` 遇到同 batch 的第二筆相同 Producer stream command 時，會立即把 `PRODUCER_SEQUENCE_GAP` 放入 completion queue；第一筆已接受 command 則要等 WAL fsync、apply 與 invariant validation 後才 dispatch。因此第二筆 callback 可能先於第一筆，違反：

- 同 shard committed result 必須依 EngineSeq dispatch；
- admission error 不得越過同一 Producer stream 尚未完成的前一筆 request；
- 每個 queued request 恰好完成一次。

### 必要修正

涉及檔案：

- `src/runtime/shard_runtime.cpp`
- `tests/integration/engine_test.cpp`

具體作法：

1. 在 batch 內以原始 request index 保存 completion outcome，不要在 admission validation loop 中立即 dispatch。
2. 每個 slot 保存以下其中之一：
   - 已決定的 admission result；
   - 已接受 command 對應的 output index；
   - 尚未決定。
3. WAL durable、全部 accepted commands apply、batch invariant validation 完成後，依原始 batch request 順序 dispatch：
   - admission result 可直接完成；
   - accepted result 使用對應 `ExecutionOutput::result`；
   - 同一 Producer stream 的後一筆 admission error 因而不會越過前一筆。
4. Fatal path 仍以相同 slot 集合完成所有尚未 dispatch 的 callback；已確定的 admission result 保持原結果，accepted 或尚未決定的 slot 回傳 `ENGINE_UNAVAILABLE`。
5. Dispatch 後立刻清空該 slot 的 handler；fatal guard 只能處理仍有 handler 的 slot，確保 exactly once。
6. 不改變 single-in-flight contract；同 batch 第二筆相同 Producer stream 仍可拒絕，本修正只處理 completion ordering。

### 必要測試

- 同一 Producer stream 的 seq 1、seq 2 在同一 group-commit window 入列；驗證 seq 1 committed callback 一定早於 seq 2 admission error。
- 不同 Producer 的 admission error 可正常完成，不要求跨 Producer 的額外 ordering。
- WAL／apply fatal path 中，每個 queued request callback count 恰為一次；此項待 failure-injection seam 完成後驗證。

預估：production 35～60 行，tests 45～75 行。

## 3. FIX-U02：以 cursor 後的實際 WAL bytes 計算 publisher lag

### 問題

目前 `EventPublisher::lag_bytes()` 在存在任何 lag 時直接回傳整個 retained WAL size。若 WAL 因 Snapshot／retention policy 保留 40 GiB，而 publisher 只落後一筆小 record，也會被視為落後 40 GiB，可能錯誤拒絕 mutating commands。

`max_publish_lag_bytes` 的語意必須是 publisher cursor 之後尚未確認的 WAL bytes，而不是 shard 的總 WAL footprint。

### 必要修正

涉及檔案：

- `src/persistence/wal.hpp`
- `src/persistence/wal.cpp`
- `src/runtime/event_publisher.cpp`
- `tests/integration/persistence_test.cpp`
- 視整合測試需要調整 `tests/integration/engine_test.cpp`

具體作法：

1. WAL 的 decoded sequential index 同時保存每筆 record 的 `WalPosition` 或 encoded frame byte size；不要只保存 `CommittedCommand`。
2. 提供窄 internal API，例如：

   ```cpp
   Result<std::uint64_t> bytes_after(EngineSeq cursor,
                                     EngineSeq upper_bound) const;
   ```

3. 計算範圍固定為 `(cursor, upper_bound]`，使用已建立的 sequential index；不得重新掃描或 decode WAL。
4. 跨 segment 時只累加實際 record frame bytes。Segment header 與 cursor 前保留資料不算 publisher lag。
5. 使用飽和加法，避免 `std::uint64_t` wrap。
6. `EventPublisher::lag_bytes()` 以目前 confirmed cursor 與 WAL durable／publishable head 呼叫此 API。
7. 如果 index 不可用或 sequence 不連續，回傳 typed error；publisher 必須進入既有 failure path，不得把錯誤當作 lag 0。
8. `max_publish_lag_age` 必須明確規定為正值。最小方案是在 `Engine::open()` 拒絕 `<= 0`；不要讓 0 隱含成「立即 storage pressure」。`max_publish_lag_bytes == 0` 可維持既有的 disabled 語意，但要在設計或 public config 註解寫明。

### 必要測試

- 建立多筆、跨 segment WAL，cursor 位於中間，驗證只計算 cursor 後 records。
- retention 前後對同一 cursor 的 lag bytes 語意一致。
- cursor 已到 head 時 lag 為 0。
- sequence 不存在或 index 未初始化時回傳 error，不靜默回傳 0。
- `max_publish_lag_age == 0` 在 open 時被拒絕。
- retained WAL 很大但只落後一筆時，不會因總 WAL size 誤觸 storage pressure。

預估：production 45～75 行，tests 45～80 行。

## 4. FIX-U03：configuration reconciliation 只拒絕真正不相容的變更

### 問題

目前只要 shard 存在任一 active order，且整份 desired instrument manifest 與 current manifest 不同，就拒絕啟動。這會錯誤阻止：

- 加入一個新 Instrument；
- 修改沒有 active order 的 Instrument tick／lot；
- 其他不影響現有 active order 的 manifest 更新。

另外，current behavior version 目前取「所有 persisted history 的最大 version」。若啟動設定明確只 supplied 較低版本，runtime 仍可能靜默啟用較高的歷史版本，使 supplied config 與實際 current config 不一致。

### 必要修正

涉及檔案：

- `src/runtime/shard_runtime.cpp`
- `tests/integration/engine_test.cpp`
- 若 public selection 無法清楚表達，再最小調整 `include/order_books/engine.hpp`

具體作法：

1. 先從 `state.order_locations` 或各 book 判斷每個 Instrument 是否有 active order。
2. 對 desired manifest 逐 Instrument 比較：
   - assigned shard 改變：一律拒絕，因第一版沒有 migration protocol；
   - tick／lot 改變且該 Instrument 有 active order：拒絕；
   - tick／lot 改變且該 Instrument 沒有 active order：允許建立新 immutable version；
   - 新增 Instrument：允許；
   - 移除歷史 Instrument：視為 mapping removal，第一版拒絕，除非未來另有 migration/removal protocol。
3. 相同 instrument configuration version 的 canonical content 不同，仍一律拒絕。
4. current behavior version 必須由本次 supplied configuration 決定，persisted manifests 只提供歷史 replay data，不得自動成為 current。
5. 最小 public API 不新增欄位時，以 `config.shard_behaviors` 中最大 supplied version 作 current；若希望允許非最大版本，才新增單一 `behavior_configuration_version` 欄位。兩種做法只能選一種並同步設計，避免同時存在隱含與明確 selection。
6. Recovery 完成後再次確認 `current_*_configuration_version` 對應的內容等於本次 supplied selection。

### 必要測試

- 有 Instrument A active order時，新增 Instrument B 可啟動。
- 有 Instrument A active order時，只修改沒有 active order 的 Instrument B tick／lot可啟動。
- 修改 Instrument A tick／lot時拒絕。
- 任一 Instrument assigned shard 改變時拒絕。
- persisted behavior versions `{1, 2}`，本次 supplied version 1 時，current 不得靜默成為 2。
- 新 current behavior version 寫入新 WAL；舊 WAL 仍使用歷史 version replay。

預估：production 20～40 行，tests 60～100 行。若新增明確 behavior version 欄位，再增加約 10～20 行。

## 5. FIX-U04：讓 durable benchmark 真正量測 durability 並在錯誤時失敗

### 問題

目前 `durable_group_commit` 每 256 筆才 `sync()`，但 CI 使用 5 筆 warmup、20 筆 measured iterations，量測期間不會執行 fsync；最後一次 sync 又在 timer 外。輸出的數字因此只是 append latency。

此外，WAL／Snapshot open、append、sync、replay 或 apply failure 多數被忽略或轉成零 workload，benchmark 仍可 exit 0，使 CI smoke 失去驗證作用。

### 必要修正

涉及檔案：

- `benchmarks/order_book_benchmark.cpp`
- 必要時調整 `.github/workflows/ci.yml` 的 smoke 參數，但不增加 performance threshold

具體作法：

1. 把 durable workload 的一次 measured iteration 定義成完整 group：
   - append 固定數量 commands；
   - 在同一個 timed region 內執行一次 `Wal::sync()`；
   - `WorkloadDelta.commands` 回報該 group 的實際 command 數。
2. CI 可使用較小 group，例如 8 或 16，讓 smoke 快速但每次 sample 都包含 fsync；正式預設仍可使用 256。
3. warmup group 也必須完整 sync，不能把未同步 warmup records 混入 measured sample。
4. 所有 setup 與 workload result 都要檢查：
   - setup failure：印出 diagnostic 並 return non-zero；
   - measured iteration failure：停止 workload、印出 workload 名稱與錯誤並 return non-zero；
   - 不得以 `commands=0` 假裝成功。
5. temporary directory 使用每次執行唯一名稱，避免兩個 benchmark process 互相 `remove_all()`。可使用 PID 加 monotonic timestamp；不新增 dependency。
6. 報告補齊實際 group size、fsync mode、measured elapsed、OS、CPU 識別資訊。若無法可攜取得 CPU model，至少清楚標記 unavailable，不得只把 hardware concurrency 當作 CPU 資訊。
7. recovery benchmark 的 Snapshot/WAL open、write、append、sync、load、replay、apply、invariant 任一步失敗都應使 process non-zero。

### 必要測試／驗證

- `--iterations=1 --warmup=0` 仍執行一次 group fsync。
- 人為指定不可寫路徑時 benchmark 必須 non-zero。
- CI smoke 輸出包含非零 commands、實際 group size 與 fsync metadata。
- recovery workload failure 不得輸出正常 throughput 後 exit 0。

預估：benchmark／CI 55～90 行。

## 6. FIX-U05：移除本次變更新增或留下的明確冗餘

### 必要修正

1. `.gitignore`
   - 移除 `tmp*`。
   - 原因：pattern 過度寬泛、可能隱藏合法 repository files，且產品與測試都使用 system temporary directory，不需要此規則。

2. `OrderBook`
   - 移除已無 caller 的 `crosses()`。
   - 移除只被 `crosses()` 使用的 const／non-const `best_level()`。
   - 不建立替代 abstraction；`plan_match()` 已直接以 ordered levels traversal 完成責任。

3. `ScopeGuard`
   - 移除未使用的 `dismiss()`。
   - 若 FIX-U01 重整 completion slots 後整個 guard 不再需要，才移除 class；不要為保留 helper 而保留 dead API。

4. `Wal::append(..., bool durable)`
   - 目前所有 caller 都傳 `false`，而 `true` path 會對同一 append 執行兩次 fsync。
   - 最小方案：移除 `durable` 參數與兩段 `if (durable)`，維持明確的 `append()` + batch `sync()` contract。
   - 若確定需要單筆 durable API，應改成不同名稱的 `append_and_sync()` 並只 fsync 一次；本版沒有 caller，不應預建。

### 必要驗證

- `rg` 確認被移除 method／參數沒有 caller。
- Debug、Release、Sanitizer build/test 通過。
- `git diff --check` 通過。

預估：production／repository config 約 35～55 行，以刪除為主。

## 7. 本輪文件與 CI 一致性

完成 FIX-U01～U05 後，只做下列必要同步：

1. `docs/order-book-design.md` 的 implementation adjustment record 補充：
   - publisher lag bytes 使用 cursor 後 record bytes；
   - behavior current-version selection 規則；
   - durable benchmark sample 的 group 定義。
2. README benchmark 指令若新增 `--group-size` 等必要參數才同步；不要加入未實作選項。
3. CI format job 至少檢查所有 tracked C/C++ files，使用明確副檔名清單與 `git ls-files`；不可只檢查 `tests/toolchain_smoke_test.cpp`。
4. 若目前全樹尚未符合 clang-format，格式修正應獨立成 mechanical change；在完成前保留「format Definition of Done 未完成」說明，不得假裝 smoke file 等於全樹檢查。

預估：docs／CI 15～30 行，不含可能需要的全樹 mechanical formatting。

## 8. 完整 Definition of Done 尚缺內容

以下不是可刪除的「額外功能」，而是 `docs/Order-book-spec.md` 第 20 節已明列的驗收證據。若目標是完整宣稱符合需求，必須另行完成：

- New／Amend／Replace／Cancel、ExpectedVersion、producer、capacity、query、tombstone 的完整 functional matrix；
- append、partial append、fsync、apply、response、sink ACK、cursor、Snapshot write/fsync/rename failure injection；
- subprocess crash + restart matrix；
- live execution、state recovery、publisher replay 的逐欄位 determinism comparison；
- fixed-seed reference／property tests；
- WAL、Snapshot、cursor、config 的 golden/version compatibility fixtures；
- metrics counter saturation、snapshot consistency、lag threshold 與 injected clock tests；
- 全樹 clang-format CI 證據。

最小 test seam 只放在 storage `FileOps` 與窄 clock boundary；不得把 failpoint 加進 public product API，也不新增通用 DI container。

剩餘預估：tests／test support 1,000～1,700 行，production test seams 100～200 行。這一階段應獨立 review，避免與本輪約 300～450 行的 correctness patch 混在一起。

## 9. 建議執行順序

1. FIX-U01 completion ordering。
2. FIX-U02 exact publisher lag。
3. FIX-U03 configuration reconciliation。
4. FIX-U04 benchmark correctness。
5. FIX-U05 dead code／unrelated ignore cleanup。
6. 第 7 節文件與 CI 同步。
7. 執行 Debug、Release、ASan/UBSan、clang-tidy、benchmark smoke 與 `git diff --check`。
8. 重新 review unstaged changes，再決定是否進入第 8 節完整驗收工作。

## 10. 本輪完成條件

- [x] 正常路徑同 Producer stream callback 不失序。
- [ ] WAL／apply fatal path 中每個 queued request callback count 恰為一次；待 failure-injection seam 完成。
- [x] Publisher lag bytes 只計算 cursor 後的實際 WAL record bytes。
- [x] Instrument config 只拒絕真正影響 active order 或 mapping 的變更。
- [x] Current behavior version 由本次 supplied config 明確決定。
- [x] Durable benchmark 每個 measured sample 都包含 fsync，任何錯誤使 process non-zero。
- [x] `tmp*`、dead OrderBook helpers、unused guard method 與 dead WAL durable branch 已移除。
- [ ] CI／README／design 只宣稱實際具有證據的能力；全樹 clang-format job 仍待獨立 mechanical formatting change。
- [x] 沒有新增任何本文件第 1 節排除的元件或 dependency。
- [x] Staged index 未被修改。
