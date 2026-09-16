# order_books 專案設計

狀態：第一階段實作依據  
範圍：現代 C++ 專案初始化，不包含 order book 業務功能

## 1. 文件定位

本文件是第一階段實作的主要依據，定義必須建立的專案骨架、工具鏈、模組邊界與驗收條件。[技術選型評估](technical-decisions.md)保留各方案的比較與背景；若兩份文件不一致，以本文件已確認的決策為準。

實作若發現本設計有矛盾、缺漏或無法落地，應在同一個變更中更新本文件，並記錄：

1. 原始設計；
2. 實際問題；
3. 採取的調整；
4. 調整的影響與取捨。

不為尚未出現的需求預建抽象。新增目錄、target、依賴或工具時，必須能對應到已確認需求或實際使用案例。

## 2. 需求理解

### 2.1 已確認需求

| ID | 需求 |
| --- | --- |
| R1 | 使用 C++20。 |
| R2 | 使用 CMake 與 Ninja 建置。 |
| R3 | 使用 Conan 2 管理第三方依賴。 |
| R4 | 使用 GoogleTest 作為測試框架。 |
| R5 | 開發環境為 macOS，使用 Apple Clang。 |
| R6 | 部署環境為 Linux，主要使用 GCC，另以 Linux Clang 驗證可攜性。 |
| R7 | 開發與 CI 支援 ASan、UBSan。Sanitizer 不進入正式建置。 |
| R8 | 使用 clang-format 與 clang-tidy。 |
| R9 | `order_books/` 是獨立 Git repository，預設分支為 `main`。 |
| R10 | 採用 Apache License 2.0。 |
| R11 | 第一階段只建立專案骨架，不實作 order book 業務功能。 |
| R12 | 專案應具備與目前規模相符的開源專案可讀性、可維護性、可測試性與開發者體驗。 |

### 2.2 合理假設

- 專案名稱、未來 C++ namespace 與 CMake target 前綴統一使用 `order_books`。
- 第一階段不承諾穩定的 C++ API、ABI、套件格式或部署映像。
- macOS 同時可能存在 Apple Silicon 與 Intel；目前不加入架構專屬程式碼或設定。
- Linux distribution、glibc 基線、CPU 架構與 GCC 版本尚未指定，因此 CI 只代表建置驗證，不等同正式支援矩陣。
- GitHub 是預期的公開託管平台，因此使用 GitHub Actions；不在此階段建立發布流程。
- CMake 不自行下載依賴。第三方依賴解析集中由 Conan 負責。

### 2.3 本階段不包含

- 訂單、價格、數量、order book、撮合規則等領域資料模型。
- 核心 library、CLI、service、network adapter 或 persistence adapter。
- 公開 API、安裝規則、CMake package export、ABI 相容性承諾。
- Docker runtime image 或正式部署流程。
- benchmark、coverage、fuzzing、TSan、Doxygen 與自動發布。
- 尚無實際需要的 issue template、CODEOWNERS、CODE_OF_CONDUCT、SECURITY 或 changelog。

這些項目不是永遠排除，而是在出現相應產品需求後另行設計。提前加入會造成空殼程式碼、錯誤的穩定性暗示或不必要的維護成本。

## 3. 設計審查結論

先前提出的 `include/`、`src/`、`apps/` 與核心 library／示範 executable 對目前需求並非必要。業務功能尚未定義時，建立公開 header 或可執行程式會迫使實作者猜測未來介面，因此本階段移除這些項目。

第一階段保留以下必要能力：

- 可重現且清楚分層的依賴、設定、建置與測試流程；
- macOS Apple Clang、Linux GCC 與 Linux Clang 的持續驗證；
- 一個只驗證 C++20 與 GoogleTest 整合的 infrastructure smoke test；
- 可由未接觸專案的開發者依 README 完成首次建置；
- 格式、靜態分析、compiler warnings 與 sanitizer 品質閘門；
- 清楚說明目前沒有產品功能，避免使用者誤認為已有 order book 實作。

此收斂符合全部已確認需求，沒有引入產品層功能或與需求無關的架構。

## 4. 系統架構與邊界

本階段沒有 runtime system，也沒有領域資料流。架構描述的是開發與驗證管線：

```text
Developer / CI
      |
      v
Conan 2 resolves GoogleTest and generates CMake integration
      |
      v
CMake configures target-scoped project policies
      |
      v
Ninja builds the infrastructure smoke test
      |
      v
CTest discovers and executes GoogleTest tests
      |
      v
CI reports compiler, lint, sanitizer and test results
```

### 4.1 元件責任

| 元件 | 責任 | 不負責 |
| --- | --- | --- |
| Conan 2 | 鎖定並提供 GoogleTest，產生 CMake toolchain 與 dependency metadata。 | 不定義專案 target、compiler warnings 或測試案例。 |
| CMake | 定義 C++20、專案選項、target、CTest 整合及 target-scoped build policy。 | 不從網路下載依賴，不承擔套件解析。 |
| Ninja | 執行建置圖。 | 不保存專案政策。 |
| GoogleTest | 執行 infrastructure smoke test；未來承載單元測試。 | 不作為產品程式碼依賴。 |
| clang-format | 提供一致的原始碼格式。 | 不取代 code review。 |
| clang-tidy | 執行保守且可行動的靜態分析規則。 | 不在預設本機 build 中強制執行。 |
| ASan／UBSan | 在測試執行時檢查記憶體錯誤與未定義行為。 | 不連結至 Release／部署產物。 |
| GitHub Actions | 在受支援工具鏈上重現 configure、build、test 與品質檢查。 | 不發布套件或部署服務。 |

### 4.2 CMake target 邊界

第一階段只需要兩類 target：

1. `order_books_project_options`：內部 `INTERFACE` target，集中承載 C++20 compile feature、warnings 與選配 sanitizer flags。提供 `order_books::project_options` alias，供本專案 target 使用，但不安裝或 export。
2. `order_books_tests`：測試 executable，連結 `GTest::gtest_main` 與 `order_books::project_options`，透過 `gtest_discover_tests` 註冊至 CTest。

不建立空的 `order_books` library。未來第一個產品 use case 確認後，再設計核心 target、來源目錄與公開介面。

GoogleMock 雖由 GoogleTest 專案提供，但第一階段沒有需要 mock 的介面，因此不連結 `GTest::gmock`。實際出現 boundary interface 後才能加入。

### 4.3 未來產品架構約束

業務設計尚未開始，但後續應維持一項方向性約束：order book 領域核心不得依賴 CLI、網路、資料庫或特定 framework。外部輸入與輸出應依賴核心，而不是讓核心反向依賴 adapter。

此項只約束依賴方向，不預先規定 class、repository interface、event bus 或分層數量。

## 5. 技術方案

### 5.1 語言與編譯器

- 所有專案 target 使用 `cxx_std_20`。top-level CMake 將 `CMAKE_CXX_EXTENSIONS` 預設設為 `OFF`，且每個本專案 target 明確維持 `CXX_EXTENSIONS OFF`；此屬性不假設能透過 `INTERFACE` target 傳遞。
- macOS 的日常開發工具鏈為 Apple Clang。
- Linux GCC 是主要部署相容性檢查。
- Linux Clang 是第二編譯器檢查，降低依賴單一 compiler extension 或診斷行為的風險。
- compiler flags 必須以 target scope 設定，不修改全域 `CMAKE_CXX_FLAGS`。
- `ORDER_BOOKS_WARNINGS_AS_ERRORS` 預設關閉，CI 開啟，避免本機不同 compiler patch version 讓基本開發流程失效。

### 5.2 CMake

- 採用支援目前 presets 與 Conan 2 整合方式的穩定 CMake 版本；第一階段基線定為 CMake 3.25。
- 使用標準 `BUILD_TESTING` 選項，不另造同義開關。
- 使用 out-of-source build，所有產物位於 `build/`。
- 不使用 `file(GLOB)` 收集 source；每個 target 明確列出檔案。
- 專案選項放在一個小型 `cmake/ProjectOptions.cmake`，不拆成沒有獨立責任的多個 helper module。
- 提供 configure、build 與 test presets；preset 名稱描述用途，不硬編碼本機絕對路徑。

### 5.3 Conan 2

- 使用 `conanfile.py`，因 GoogleTest 是測試依賴，需以 `test_requires` 表達，而非成為未來產品的 runtime dependency。
- 使用 `CMakeToolchain`、`CMakeDeps` 與 `cmake_layout`。
- GoogleTest 使用明確版本 `gtest/1.18.0`，不使用浮動範圍；此版本在 ConanCenter 提供且與 C++20 工具鏈相容。
- 第一階段將 `gtest/*:build_gmock` 設為 `False`；沒有需要 mock 的產品介面時，不建置未使用的 GoogleMock 元件。
- 不使用 CMake `FetchContent`、Git submodule 或 vendored copy 重複管理 GoogleTest。
- 開發者的 compiler、OS 與 architecture 由本機 Conan profile 描述；README 提供 `conan profile detect` 的首次設定步驟。
- 正式 Linux compiler／glibc 基線未定前，不提交聲稱可用於 production 的 Conan deployment profile。
- 不將 Conan 產生的 `CMakeUserPresets.json`、toolchain、dependency metadata 或 binary cache 納入 Git。

### 5.4 測試

第一階段的 `tests/toolchain_smoke_test.cpp` 只驗證：

- 編譯器確實以 C++20 或更新模式編譯；
- GoogleTest 能由 Conan 提供、連結、被 CTest 發現並執行。

smoke test 不應假裝測試 order book 行為，也不加入無意義的 product fixture 或 mock。

後續測試遵循：

- 領域功能以 unit test 驗證正常路徑、邊界值與失敗路徑；
- integration test 僅跨越真實元件邊界時建立；
- regression test 應先重現缺陷，再驗證修正；
- 測試名稱描述可觀察行為，不綁定私有實作細節；
- 測試不得依賴執行順序、網路或未固定的系統時間。

### 5.5 Sanitizer、靜態分析與格式

- `ORDER_BOOKS_ENABLE_SANITIZERS` 控制 ASan + UBSan，預設為 `OFF`。
- sanitizer 僅在 GCC／Clang 相容工具鏈啟用；不支援時於 configure 階段清楚失敗，不靜默忽略。
- sanitizer preset 使用帶 debug information 的非正式建置，並保留 frame pointer 以改善 stack trace。
- `.clang-tidy` 啟用一組保守規則；初始設定不得依賴大量 suppression 才能通過。
- clang-tidy 由明確 preset 或 CI job 啟用，避免每次本機編譯都承擔分析成本。
- `.clang-format` 是格式的唯一準則；CI 使用 check mode，不在 CI 自動修改檔案。

### 5.6 CI

單一 `.github/workflows/ci.yml` 足以涵蓋第一階段：

| Job | 平台／工具鏈 | 驗證內容 |
| --- | --- | --- |
| Linux GCC | GitHub-hosted Linux runner + GCC | configure、build、test、warnings-as-errors |
| Linux Clang | GitHub-hosted Linux runner + Clang | configure、build、test、clang-tidy |
| Linux Sanitizers | GitHub-hosted Linux runner + Clang | ASan + UBSan build 與 test |
| macOS | GitHub-hosted macOS runner + Apple Clang | configure、build、test |
| Format | Linux runner | clang-format check |

CI 應重用 README 中相同的核心命令。第一階段依賴很小，不先加入 cache、coverage upload、release job 或複雜 reusable workflow；有可量化的時間問題後再加入 cache。

第三方 GitHub Actions 應限制到必要數量並固定至明確版本。CI 安裝 Conan 2，使用與 job 工具鏈一致的 profile，且所有 job 由乾淨 checkout 開始。

## 6. 錯誤處理

目前沒有 runtime 業務錯誤，因此不預先選擇 exception、error code 或 `std::expected` 風格。

工具鏈錯誤採 fail-fast：

- 缺少必要 compiler feature、GoogleTest target 或不支援的 sanitizer 組合時，CMake configure 直接失敗並提供可行動訊息。
- build、lint、format、sanitizer 或 test 任一失敗時，CI job 失敗。
- CTest 使用 `--output-on-failure` 顯示失敗內容。
- 不用 warning suppression 隱藏本專案問題；第三方 header 應透過 imported target／system include 避免污染本專案診斷。

領域錯誤模型必須等 order book 操作、效能要求與 API 邊界確定後設計。

## 7. 專案組織與必要修改

第一階段實作後的檔案應為：

```text
order_books/
├── .github/
│   └── workflows/
│       └── ci.yml
├── cmake/
│   └── ProjectOptions.cmake
├── docs/
│   ├── project-design.md
│   └── technical-decisions.md
├── tests/
│   ├── CMakeLists.txt
│   └── toolchain_smoke_test.cpp
├── .clang-format
├── .clang-tidy
├── .gitignore
├── CMakeLists.txt
├── CMakePresets.json
├── CONTRIBUTING.md
├── LICENSE
├── README.md
└── conanfile.py
```

### 7.1 各檔案的必要性

| 檔案 | 必要內容 |
| --- | --- |
| `CMakeLists.txt` | 專案宣告、C++ 語言、CTest、project options 與 tests 子目錄。 |
| `cmake/ProjectOptions.cmake` | target-scoped warnings、C++20 policy、sanitizer 選項。 |
| `CMakePresets.json` | 一致的本機 configure／build／test 入口；不含機器絕對路徑。 |
| `conanfile.py` | GoogleTest test requirement 與 CMake generators/layout。 |
| `tests/*` | GoogleTest 與 C++20 infrastructure smoke test。 |
| `.clang-format` | 跨開發者的一致格式。 |
| `.clang-tidy` | 可重現的靜態分析規則。 |
| `.github/workflows/ci.yml` | 受支援平台、編譯器與品質閘門。 |
| `.gitignore` | 排除 build、Conan、IDE 與 OS 產物；不忽略 source 或 lockfile。 |
| `README.md` | 專案狀態、需求、快速開始、建置／測試／分析命令與限制。 |
| `CONTRIBUTING.md` | 開發環境、提交前檢查與設計偏離處理方式。 |
| `LICENSE` | Apache License 2.0 完整文字。 |
| `docs/*` | 保存權衡背景與具約束力的設計。 |

`CONTRIBUTING.md` 保留為獨立文件，是因需求明確要求未來第三方能參與；其內容應簡短並引用 README 的建置命令，不重複整套說明。

### 7.2 明確不建立的項目

本階段不建立：

```text
include/
src/
apps/
benchmarks/
examples/
Dockerfile
CHANGELOG.md
CODEOWNERS
```

上述項目須由具體產品、發布、效能或治理需求觸發。

## 8. 開發流程

新開發者的標準流程保持為四個概念步驟：

1. 安裝或確認 CMake、Ninja、Python／Conan 2 與相應 compiler。
2. 初始化本機 Conan profile，安裝鎖定依賴。
3. 使用 CMake preset configure 與 build。
4. 使用 CTest preset 執行測試。

README 必須提供可直接複製的 macOS 與 Linux 命令，並說明 sanitizer、clang-tidy 與 format check 的獨立入口。文件命令與 CI 命令不得形成兩套不同流程。

格式化是顯式開發動作，不加入會偷偷修改工作目錄的 build target。檢查可以失敗，但自動修正必須由開發者主動執行。

## 9. 關鍵決策與取捨

| 決策 | 理由 | 代價 |
| --- | --- | --- |
| 第一階段不建立產品 target | 不猜測尚未定義的 API 或執行模式。 | 初始 repository 只有工具鏈驗證，沒有可供使用的功能。 |
| 選 Conan 2 | 已確認需求，且能在未來表達跨平台 binary configuration 與私有依賴。 | 對目前只有 GoogleTest 的專案而言比 FetchContent 複雜。 |
| GoogleTest 僅為 test requirement | 防止測試框架成為未來產品依賴。 | recipe 需使用 `conanfile.py` 表達。 |
| 一個 project options target | 集中共用政策且維持 target scope，符合目前規模。 | 未來若政策分化，才需拆成 warnings／sanitizers targets。 |
| warnings-as-errors 只在 CI 開啟 | 保持 CI 嚴格，同時避免本機 compiler 差異阻塞開發。 | 本機 warning 不一定立即造成 build failure。 |
| 暫不提供 production container/profile | 部署 distro、架構與 ABI 尚未定義，現在提供會製造錯誤承諾。 | 正式部署前仍需補一輪 deployment design。 |
| 不加入 coverage、benchmark、TSan、fuzzing | 目前沒有產品程式可產生有意義結果。 | 業務功能加入後需重新評估品質工具。 |

## 10. 已知限制與擴充方向

### 10.1 已知限制

- repository 初始化後仍不提供 order book 功能。
- CI runner 的 Linux 環境不等於正式 production ABI 基線。
- 尚未定義版本策略、release artifact、安裝方式與 package consumer experience。
- 尚未定義效能、threading、determinism、allocation 或 exception policy。

### 10.2 需求出現後的擴充順序

1. 定義 order book use cases、語意、不變條件與效能目標。
2. 設計核心資料模型與對外操作，再建立 `include/`、`src/` 與核心 library target。
3. 根據真實輸入輸出邊界建立 adapters 或 executable；不預設一定需要 CLI 或 service。
4. 有穩定公開 API 後加入 install/export、版本策略與 Conan package metadata。
5. 有可衡量的 hot path 後加入 benchmark；有多執行緒後加入 TSan；有適合輸入面後加入 fuzzing。
6. 部署 distro、CPU 與 ABI 基線確定後，建立 production Conan profile、container 與 release workflow。

## 11. 第一階段驗收條件

所有條件均滿足才視為實作完成：

- `order_books/` 為獨立 Git repository，預設分支為 `main`。
- 全新 checkout 可依 README 在 macOS Apple Clang 完成 dependency install、configure、build 與 test。
- CI 可在 Linux GCC、Linux Clang 與 macOS Apple Clang 完成 configure、build 與 test。
- Linux sanitizer job 在 ASan + UBSan 下通過。
- clang-format check 與 clang-tidy 通過。
- C++ extensions 關閉，測試確認使用 C++20 或更新語言模式。
- GoogleTest 只透過 Conan 取得，CMake 不自行下載依賴。
- Release configuration 不含 sanitizer flags。
- repository 不包含 build、Conan 生成檔或本機 IDE 產物。
- README 明確標示目前沒有 order book 業務功能。
- 專案採用完整 Apache License 2.0。
- 沒有 `include/`、`src/`、`apps/` 或虛構產品 API。
- 實作與本文件一致；任何必要偏離均已同步記錄。

## 12. 需求追溯

| 需求 | 設計對應 | 驗證方式 |
| --- | --- | --- |
| R1 | target compile feature、extensions off | smoke test + compiler command |
| R2 | CMake presets、Ninja | macOS／Linux CI build |
| R3 | `conanfile.py`、CMake generators | clean dependency install |
| R4 | `order_books_tests`、CTest discovery | CTest 通過 |
| R5 | macOS CI job | Apple Clang build/test |
| R6 | Linux GCC／Clang jobs | 兩組 build/test 通過 |
| R7 | sanitizer option/preset/job | ASan + UBSan test 通過，Release 無 sanitizer |
| R8 | config files 與 CI jobs | format/tidy check 通過 |
| R9 | repository metadata | Git root 與預設分支檢查 |
| R10 | `LICENSE` | Apache-2.0 文字檢查 |
| R11 | 無產品目錄與 target | repository tree review |
| R12 | README、CONTRIBUTING、CI、target-scoped policy | fresh-clone 驗證與 review |

## 13. 實作調整紀錄

### 13.1 CMake extensions policy 的落點

- 原始設計：由共用 project options target 統一表達 C++20 與 extensions off。
- 實際問題：CMake 的 `CXX_EXTENSIONS` 是 target property，不應假設可由 `INTERFACE` target 傳遞給 consumer。
- 採取調整：top-level CMake 設定 `CMAKE_CXX_EXTENSIONS OFF` 作為預設值，並在每個本專案 concrete target 明確設定 `CXX_EXTENSIONS OFF`；project options target 只傳遞 compile feature、warnings 與 sanitizer flags。
- 影響與取捨：維持原本的 C++20／無 compiler extensions 行為，並避免依賴無效或不明確的 property propagation；未增加任何公開 target。

### 13.2 Conan 與 C++20 profile 一致性

- 原始設計：專案與依賴使用 C++20，開發者可使用 Conan 自動偵測 profile。
- 實際問題：自動偵測 profile 可能以 `gnu17` 作為 `compiler.cppstd`，使 Conan toolchain 對依賴顯示不同的語言標準，即使專案 target 最終要求 C++20。
- 採取調整：README 與所有 CI `conan install` 命令明確傳入 `-s compiler.cppstd=20`。
- 影響與取捨：依賴與專案的 Conan／CMake 設定一致；profile 仍保留給開發者描述 OS、compiler 與 architecture，沒有硬編碼本機路徑。

### 13.3 GoogleMock 建置範圍

- 原始設計：第一階段不連結 `GTest::gmock`，因為尚無需要 mock 的產品介面。
- 實際問題：Conan 的 GoogleTest recipe 預設會一併建置 GoogleMock，即使專案沒有使用它。
- 採取調整：在 `conanfile.py` 將 `gtest/*:build_gmock` 設為 `False`，並只連結 `GTest::gtest_main`。
- 影響與取捨：減少初始依賴建置時間與產物範圍；未來出現需要 mock 的介面時，移除此 option 並同步更新設計與測試即可。
