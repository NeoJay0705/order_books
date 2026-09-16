# C++ 專案工具選型評估

> 本文件保存選型比較與背景；第一階段的最終決策及實作規格以[專案設計](project-design.md)為準。

## 專案環境

- 開發平台：macOS
- 部署平台：Linux
- 建議語言標準：C++20
- 建議建置系統：CMake + Ninja

建議在 macOS 使用 Apple Clang 日常開發，並在 Linux CI 中以 GCC 作為主要部署驗證編譯器，同時增加一組 Clang 建置以檢查可攜性。正式執行檔應在與部署環境相容的 Linux container 或 CI runner 中建置，不應直接使用 macOS 的編譯產物。

## ASan 與 UBSan

Sanitizer 是由編譯器插入程式中的執行期檢查，可在測試期間找出一般編譯器不容易發現的錯誤。

### AddressSanitizer（ASan）

ASan 主要檢查記憶體錯誤：

- heap、stack 與 global buffer overflow
- use-after-free、use-after-return、use-after-scope
- double-free 與 invalid free
- 部分記憶體洩漏

範例：

```cpp
std::vector<int> values(3);
values[10] = 42; // ASan 會在執行時指出越界位置
```

ASan 通常會增加約兩倍的執行時間，並使用更多記憶體。因此適合開發與 CI 測試，不適合連結至正式部署執行檔。

參考：[Clang AddressSanitizer 文件](https://clang.llvm.org/docs/AddressSanitizer.html)

### UndefinedBehaviorSanitizer（UBSan）

UBSan 主要檢查 C++ 未定義行為：

- signed integer overflow
- 無效的 bit shift
- null pointer 解參照
- 未對齊的記憶體存取
- 無效型別轉換
- 部分陣列越界

範例：

```cpp
int value = std::numeric_limits<int>::max();
++value; // signed overflow，屬於 undefined behavior
```

UBSan 的額外成本通常比 ASan 小，並支援 Linux 與 macOS。

參考：[Clang UndefinedBehaviorSanitizer 文件](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)

### 使用建議

提供獨立的 CMake preset：

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

若 order book 未來使用多執行緒，可另外加入 ThreadSanitizer（TSan）組態以檢查 data race。TSan 應單獨執行，不能和 ASan 同時啟用。

## GCC 與 Clang

| 面向 | GCC | Clang／Apple Clang |
| --- | --- | --- |
| macOS 開發 | 非系統預設，需額外安裝 | 系統原生工具鏈，Xcode 整合最好 |
| Linux 部署 | Linux 常見預設，`libstdc++` 生態成熟 | Linux 也完整支援，可搭配 `libstdc++` 或 `libc++` |
| 錯誤訊息 | 已有明顯改善 | 通常較清楚，適合日常開發 |
| 靜態分析 | 提供 GCC analyzer | `clang-tidy` 與 Clang Static Analyzer 整合成熟 |
| 最佳化效能 | 依實際 workload 而定 | 依實際 workload 而定 |
| Sanitizer | 支援 ASan、UBSan 等 | Sanitizer 工具鏈整合成熟 |
| 可攜性驗證 | 可發現 Clang 未暴露的問題 | 可發現 GCC 未暴露的問題 |

不應只依「哪個編譯器產生的程式比較快」做選擇；效能差異必須使用實際 order book workload benchmark。

macOS 上的 Apple Clang 通常搭配 `libc++`，Linux 上的 GCC 通常搭配 `libstdc++`。即使兩邊都使用 C++20，標準函式庫、ABI 和系統 API 仍可能不同，因此建議配置如下：

```text
macOS:
  Apple Clang -> 日常開發與測試

Linux CI:
  GCC         -> 主要部署驗證
  Clang       -> 第二編譯器驗證

Production:
  在與部署環境相容的 Linux image 中建置
```

例如部署環境是 Ubuntu 24.04，應在相同或 ABI 相容的 Linux container 中產生正式執行檔，以避免 glibc 版本不相容。

參考：

- [Clang 使用手冊](https://clang.llvm.org/docs/UsersManual.html)
- [GCC Instrumentation Options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)

## vcpkg、Conan 與 CPM.cmake

| 項目 | CPM.cmake | vcpkg | Conan 2 |
| --- | --- | --- | --- |
| 定位 | CMake `FetchContent` 包裝 | C/C++ 套件管理器 | 完整 C/C++ 套件與二進位管理器 |
| 上手難度 | 最低 | 中等 | 最高 |
| 額外工具 | 幾乎不需要 | 需要 vcpkg | 需要 Python／Conan CLI |
| 依賴取得 | 通常抓取 source 一起建置 | ports 與預建／快取套件 | recipe、binary package、遠端倉庫 |
| 版本固定 | Git tag／commit | manifest + baseline | recipe revision + lockfile |
| Binary cache | 有限，主要依賴建置快取 | 支援 | 完整支援 |
| 編譯器／平台設定 | 交給 CMake | triplet | profile、settings、options |
| 私有套件 | 不擅長 | 可使用 registry | 很適合 |
| 跨平台 | 支援 | 支援良好 | 支援良好 |
| 適合情境 | 小型、純 CMake、依賴少 | 一般跨平台應用 | 複雜產品、CI、私有套件、多種 binary configuration |

### CPM.cmake

CPM 是 `FetchContent` 的薄包裝，增加簡潔語法、版本控制與快取；依賴通常會隨專案一起從原始碼建置。

優點：

- 設定少，不需要另外安裝 package manager
- 適合 Catch2、fmt、nlohmann/json 等容易從原始碼建置的依賴
- 可直接從 `CMakeLists.txt` 看出依賴

缺點：

- 大型依賴的建置成本可能很高
- 不是真正完整的 binary package manager
- 複雜系統函式庫、不同 ABI 或 transitive dependencies 較難管理
- 私有套件管理能力有限

參考：[CPM.cmake 官方說明](https://github.com/cpm-cmake/CPM.cmake/blob/master/README.md)

### vcpkg

vcpkg 是跨平台 C/C++ 套件管理器，提供 manifest mode、版本管理、triplet、registry 與 binary caching，並可與 CMake 整合。

優點：

- 使用方式相對直觀，套件種類多
- manifest 容易閱讀
- Windows／Visual Studio 整合特別好，亦支援 macOS 與 Linux
- 支援 binary cache 與 CI

缺點：

- 自訂平台設定主要依賴 triplet，需要理解其模型
- 複雜私有套件與多維 binary configuration 不如 Conan 靈活
- 需要 bootstrap 並管理 vcpkg 本身

參考：[vcpkg 官方文件](https://learn.microsoft.com/en-us/vcpkg/)

### Conan 2

Conan 可針對作業系統、架構、編譯器、編譯器版本、Debug／Release、shared／static 等組合管理不同二進位套件，並提供 build/host profiles 與 lockfiles。

優點：

- macOS 開發、Linux 部署的模型完整
- 適合控制 GCC／Clang、架構、ABI 與 build type
- binary caching 與私有 package registry 能力強
- 適合大型 CI 與多個內部 C++ library
- cross-compilation 支援清楚

缺點：

- 概念與設定較多
- 團隊必須管理 Conan profiles
- CMake configure 前通常需要先執行 `conan install`
- 對小型專案可能過重

參考：

- [Conan 官方介紹](https://docs.conan.io/2/introduction.html)
- [Conan CMake 整合](https://docs.conan.io/2/integrations/cmake.html)

### 套件管理建議

一般而言，只有測試框架與少量 header-only library 時，CPM.cmake 已足夠。

若後續會加入 Boost、網路函式庫、資料庫 client、壓縮函式庫或多個內部 library，並要求建置結果嚴格可重現，應考慮從一開始使用 Conan 2。

vcpkg 位於兩者之間；若團隊已熟悉 vcpkg，直接採用也合理。

本專案已確定使用 Conan 2，第一階段便建立單一、可延續的依賴管理流程，不再同時引入 CPM.cmake 或 vcpkg。

## Catch2 與 GoogleTest

| 面向 | Catch2 | GoogleTest |
| --- | --- | --- |
| 語法 | 精簡、接近自然語言 | 傳統 xUnit 風格 |
| 基本 assertion | `REQUIRE`、`CHECK` | `ASSERT_*`、`EXPECT_*` |
| Fixtures | 支援 | 支援且成熟 |
| Parameterized tests | Generators | Typed／value-parameterized tests |
| Mock framework | 無官方同級內建方案 | 內含 GoogleMock |
| Benchmark | 內建基本 benchmark | 通常另外搭配 Google Benchmark |
| CMake／CTest | 支援 | 支援 |
| 常見使用情境 | 小中型專案、演算法測試 | 大型專案、介面與 mock 較多 |

### Catch2

Catch2 的特色包括 assertion expression、`SECTION`、data generators 和內建 benchmark。測試通常容易閱讀，也能透過 `catch_discover_tests` 註冊至 CTest。

參考：

- [Catch2 文件](https://catch2-temp.readthedocs.io/en/latest/index.html)
- [Catch2 CTest 整合](https://catch2-temp.readthedocs.io/en/latest/usage-tips.html)

### GoogleTest

GoogleTest 的優勢是成熟、普及，且 GoogleMock 是同一套官方工具，適合模擬行情來源、交易所連線、clock、storage 等抽象介面。

參考：

- [GoogleTest 官方文件](https://google.github.io/googletest/)
- [GoogleMock 文件](https://google.github.io/googletest/reference/mocking.html)

### 測試框架建議

若 order book 後續會連接 market data feed、clock、event publisher 或其他外部介面，建議使用 GoogleTest + GoogleMock。

若核心只是純資料結構與撮合演算法、幾乎沒有外部介面，Catch2 會更輕巧，其 generator 也適合測試大量價格與數量組合。

本專案已確定使用 GoogleTest。第一階段沒有產品介面，因此只連結 GoogleTest，不使用 GoogleMock。

## 最終選型決議

```text
C++20
CMake + Ninja
macOS: Apple Clang
Linux CI/deployment: GCC
CI 額外執行 Linux Clang build
ASan + UBSan: Debug/CI
GoogleTest
Conan 2
正式部署時在相容的 Linux 環境中產生 build
```

第一階段只使用 GoogleTest；尚無需要 mock 的產品介面，因此暫不連結 GoogleMock。正式 deployment container 須等 Linux distribution、CPU architecture 與 ABI 基線確定後建立。
