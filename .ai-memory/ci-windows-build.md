# Windows Debug CI 构建踩坑记录

仓库：https://github.com/ykx960430/testTele （本地 remote 名 `github`，走 ghproxy.net；`origin` 是 Gitee 镜像，**推 dev 必须显式 `git push github dev`**）
工作流：`.github/workflows/build-win.yml`，runner `windows-2022`（4 vCPU / 16 GB / D 盘 150 GB）。
结果：run 56 成功，产物 `Telegram-Windows-Debug`（129 MB zip，内含 `D:\out\Debug\AyuGram.exe`，解压后 496 MB）。

## 已定位并修好的根因

1. **`actions/cache@v4` 的 post 保存步骤带 `post-if: "success()"`** → 任务失败时缓存保存被跳过。
   这就是前 53 轮都没把 Libraries 存下来的原因。`restore@v4` / `save@v4` 没有 post 步骤。
   → 改用 `actions/cache/restore@v4` + 显式 `actions/cache/save@v4`，**紧跟 prepare 之后**保存。
2. **缓存保存需要 `actions: write` 权限**。提交 `ad14924` 里没有 `permissions:` 块。
   → 顶部加 `permissions: contents: read / actions: write`。
3. **runner "lost communication"**：`--parallel 4` × MSVC `/MP`（`cmake/options_win.cmake:34`）= 最多 16 个 cl.exe 同时跑。
   → 构建 `--parallel 2`，prepare 设 `NUMBER_OF_PROCESSORS=2` + `CMAKE_BUILD_PARALLEL_LEVEL=2`。
4. **缓存恢复后 prepare.py 会重建 Qt**：`checkCacheKey()` 里目录不存在就返回 `NotFound`；
   Qt 源码目录 `win64/qt_5.15.19` 缓存前被清空（tar 不保留空目录）。
   → 加 "Recreate cached stage directories" 步骤 `mkdir -p` 补回空目录。
5. **不能把失败的中间产物写进缓存**：`save` 用 `if: success()`（不是 `always()`），`continue-on-error: true`。
6. **产物路径写错**：可执行文件叫 `AyuGram.exe`（`Telegram/CMakeLists.txt:2576`），
   多配置生成器输出到 `${CMAKE_BINARY_DIR}`（`Telegram/CMakeLists.txt:2626`）→ `D:\out\Debug\AyuGram.exe`。
   原来上传 `Telegram.exe`，`if-no-files-found: warn` 只报警不失败 → 假成功。
   → 改路径，并加 "Verify executable produced" 步骤 + `if-no-files-found: error`。
7. **源码语法错误**：提交 `b7f1567`（download system Phase 1）把 `storage_account.cpp` 里
   `Account::readLocations()` 的收尾 `}` 删掉了，导致 run 55 编译到 2477/2623 时 C2601/C1075 一片。
   → 补回该 `}`（提交 `619cdd8`）。

## 缓存键与 trim 安全性（实测数据）

- 缓存键 = `prepare-<hash(prepare.py)>-<hash(qt_version.py)>`，与库内容无关。
- `computeCacheKey()` 在 `runStages()` 里对每个 stage 都先算一遍，`depends:` 的 glob 必须命中。
  全部 `depends:` 只涉及 `win64/python/Scripts/activate.bat`、`win64/patches/**`、`win64/ThirdParty/**`，trim 不碰。
- `environmentKey` 来自 prepare.py 内部固定 dict，**不含** NUMBER_OF_PROCESSORS / CMAKE_BUILD_PARALLEL_LEVEL。
- 实测体积：Libraries 原始 **19971 MB** → 删 Qt 源码后 **9947 MB** → 再删 tg_angle/tg_owt 多余产物后 **6833 MB**，
  压缩后 **1.38 GB**，远低于单仓库 10 GB 上限。D 盘 150 GB，磁盘完全不是瓶颈。
- trim 只删：`win64/qt_5.15.19` 内容、所有 `.git`、`tg_angle` 除 `include`/`out` 外的一切、
  `tg_angle/out/Release`、`tg_owt/out/Release`、两个 `out/Debug` 里非 `*.lib/*.pdb/*.a` 的文件。
  CMake 只用 `tg_angle/include` + `out/Debug/tg_angle.lib`、`tg_owt/src` + `out/Debug/tg_owt.lib`。

## 耗时实测（缓存命中后）

prepare 全跳过 → Configure ~1 min → Build ~1h27m（`--parallel 2`，2623 个目标）。
一轮 **约 1.5 小时**。首次 prepare 全程约 2h45m（满并行）/ ~3h10m（并行 2）。

## 产物能否直接运行

可以。Qt 在 prepare.py 里用 `-static -static-runtime` 编译，平台插件 `qwindows.lib` 静态链接
（`cmake/external/qt/CMakeLists.txt:178`），CRT 也是 `/MTd` → **不需要任何额外 DLL**。
496 MB 体积即静态链接 + Debug 符号的证据。
- 便携隔离：exe 旁边放一个空文件夹 `TelegramForcePortable`（`Telegram/SourceFiles/core/launcher.cpp:254`）。
- 否则共用 `%APPDATA%\AyuGramDesktop`（`setApplicationName("AyuGramDesktop")`，launcher.cpp:343），
  且同一数据目录不允许两个实例同时运行。
- Debug 构建 `/Od` + `/RTC1`，比 Release 慢、内存大，属正常。

## 排查方法备忘

- 单 job 日志：`GET /repos/ykx960430/testTele/actions/jobs/<job_id>/logs`，需带 token
  （`git credential fill`，host=ghproxy.net）。run 级 zip 在 runner 暴死时只有 preflight 文件。
- runner 暴死时日志尾部未 flush，最后一行会停在半路，看不到报错是正常的。
- git-bash 里有 `du` / `find` / `df -h`；GitHub 的 bash 带 `-e -o pipefail`，脚本开头要 `set +e`。
- 本地 `js-yaml` 校验 YAML：`npm install js-yaml` 到临时目录后按绝对路径 require。
- 别用本地 `python`（Microsoft Store 占位符，exit 49）；用 `node -e` 处理 JSON。
- 括号平衡脚本对含 `{`/`}` 的注释和字符串会误报，不能单独作为判据（13 个基线文件误报但都能编译）。

## 一个容易忘的前提

本地没有 `out/` 目录，**所有构建都由 CI 从 git 提交触发**。所以：
工作区里没提交的改动，在 CI 产物里完全不存在。
"A 功能看不到"首先要确认 A 的代码是否已经 commit + `git push github dev`。
（2026-11 排查"侧边栏没有下载入口"就是这么来的：HEAD 只有 Phase 2-A 注册的空页面，
2-C 的入口代码还在工作区里躺着。）

新增 `.style` 文件 / 新 lang key 时，必须走完整 configure（CI 是全新 configure，天然满足），
只增量编译老 .cpp 不会重新生成 `styles/style_*.h` 和 lang 头文件。

## 2026-11 下载系统排查中踩到的 4 个坑

1. **`.style` 生成的头文件名是 `style_<stem>.h`，路径被剃掉**
   `info/downloads/info_downloads_big.style` → `styles/style_info_downloads_big.h`。
   `Telegram/lib_ui/cmake/generate_styles.cmake` 用的是 `get_filename_component(... NAME_WLE)`。
   写成 `style_downloads_big.h` 会直接编译失败（找不到头文件）。
2. **`QFile::open(QIODevice::WriteOnly)` 会截断已有文件**
   断点续传打开 `.temp` 必须用 `ReadWrite`：`WriteOnly` 截断 → `size()` 归零 →
   `_startOffset` 被夹到 0 → 续传退化成重新下载；`Append` 又会让 `seek()` 失效。
3. **`Account::writeLocationsDelayed()` 有 1s 定时器，`~Account()` 不 flush locations**
   暂停列表存在 `writeLocations()` 里（它调用 `_pausedDownloadsSerialize()`），
   所以退出前必须 `writeLocationsForce()` 同步落盘，否则 1s 内退出就丢。
4. **`QFileInfo(path + ".temp").size()` 才是真实的已下载字节数**
   反序列化后要把 `ready` 按它夹紧，但 `path` 必须保持最终路径（否则 rename 目标错）。

## 2026-11 下载系统 code review 又修掉的 6 个坑

1. **旧版本把 `.temp` 路径当最终路径存进了磁盘**
   HEAD 的 `validatePausedDownloads()` 会执行 `d.path = d.path + ".temp"` 再落盘。
   新版本按最终路径解释 `d.path` → 去找 `<name>.temp.temp`（不存在）→ `ready=0`
   → `resumeOne` 的 temp 检查被跳过 → 文件写到 `<name>.temp`，真正的 `<name>` 永远不出现。
   → 在 `deserializePaused()` 里做 legacy 修正：`path` 以 `.temp` 结尾且 `ready>0`
   且 `path` 存在而 `path+".temp"` 不存在时，`path.chop(5)`。
2. **`FileLoader::pause()` 在 `_file.flush()` 之前取 `currentOffset()`**
   `QFile::size()` 可能不含写缓冲里未落盘的数据 → `_pausedBytes` 偏小 → 续传多下一段。
   → 改成 flush → 测量 → close 三步，注意不能先 close（close 后 `_fileIsOpen=false`，
   `currentOffset()` 会去读 `_data.size()`，而 file 模式下 `_data` 是空的）。
3. **侧栏下载角标没对齐文件夹按钮的行为**
   文件夹按钮（`window_filters_menu.cpp:684`）和主菜单图标都检查
   `AyuSettings::hideNotificationCounters()`，且 `count > 999` 时显示 `"99+"`。
   新角标两样都没做 → 补上，并把 `hideNotificationCountersValue()` 加进 `rpl::merge`
   （用 `| rpl::map_to(true)` 统一成 `producer<bool>`，否则 merge 的 generator 类型对不上）。
4. **`importPausedJson()` 只校验 `format`，不校验 `version`**
   → 补 `version == 1` 校验；同时把 `downloadType`/`reason` 从 `toInt()` 改成和
   `documentId` 一样的 `readInt()`（字符串/数字都兼容），否则 `"downloadType": "1"`
   会被读成 0（Document）。
5. **导入允许 photo 条目，但 UI 无法处理**
   photo 不能续传（`resumeOne` 直接 return false），也没有移除暂停行的入口
   （`cancelPaused` 全仓无调用者）→ 导入一个 photo 行就永远删不掉。
   → 导入只接受 Document 条目；`pauseOne`/`cancelPaused` 是 T2C.5 右键菜单要用的 API，先留着。
6. **`resumeOne()` 失败时 `ready` 停在过期值**
   → 拒绝续传时把 `i->ready` 夹到 `.temp` 实际大小，并 `_pausedListChanges.fire({})`
   + `writePostponed(nullptr)`，让界面和磁盘立刻同步。

> 另外确认：`checkForOpen()` 在 `_startOffset > 0` 且 temp 过短时 `cancel(FileWriteFailure)`
> 理论上可能走到 `setDownloadPath(QString())` 清空用户下载目录，但 `resumeOne` 已经用
> `partial.size() < paused.ready` 拦了一道，且 `_startOffset>0` 时 temp 必然非空
> （`started = currentOffset() > 0` 为真），所以这条路径实际不可达，没有改 `data_document.cpp`。

## 2026-11 下载系统两次 CI 编译失败（run 57 / run 58）

两轮都是「一次只报一个错、ninja 就停」，所以**每轮只能看到一个 TU 的错误**，
必须靠静态审计把其余 TU 一起过一遍，否则就是无底洞式的一小时一轮。

1. **`data_download_manager.cpp(905)`：`HistoryItem` 没有 `document()` 成员**
   `resumeOne()` 里写的是 `found->document()`，但 `document()` 在 `Data::Media` 上，
   `HistoryItem` 只有 `media()`。
   → 改成 `(found && found->media()) ? found->media()->document() : nullptr`。
   注意 `HistoryItem::media()` 可能返回 nullptr，不能只写 `found->media()->document()`。
2. **`window_filters_menu.cpp(484)`：`shared_ptr<Info::Memento>` 转 `shared_ptr<Window::SectionMemento>` 失败**
   **根因是不完整类型**：`Info::Memento` 只被 `info_wrap_widget.h:45` 前向声明，
   `std::shared_ptr<Derived>` → `std::shared_ptr<Base>` 的转换要求 `Derived` 在**调用点**完整。
   → 在 `// AyuGram includes` 块里补 `#include "info/info_memento.h"`。
   **判据**：其它 `showSection(Info::Xxx::Make(...))` 的调用点
   （`dialogs_stories_content.cpp:23`、`info_profile_actions.cpp:71`）都显式包含了
   `info/info_memento.h`；只前向声明是不够的。
   顺手把 `#include "info/downloads/info_downloads_big_widget.h"` 从上游 include 列表
   挪进了 `// AyuGram includes` 块（AGENTS.md 要求 AyuGram 头文件单独成块）。

> 静态审计这次覆盖了：新增 widget 的全部 `st::` 符号与 palette 颜色、19 个 lang key
> （`#one`/`#other` 复数键也算）、`td_ui.cmake` 的 style 注册、`DownloadManager` /
> `DocumentData` / `FileLoader` / `mtpFileLoader` / `Storage::Account` / `SideBarButton` /
> `VerticalLayout` / `MenuCallback` / `Toast` / `FileDialog` / `rpl::merge` 的类型匹配，
> 没有发现第二处编译错误。

## 2026-11 第三次 CI 编译失败（run 59 / run 60）

`info_downloads_big_widget.cpp(844)` 调了 `Data::DownloadManager::loadedCount()`，
头文件里没有这个成员 —— 82b0693 写调用点时 API 还没加（两轮 CI 报的是同一个错，
因为 dd7dc65 只加了落盘，没碰这一行）。
→ 在 `pausedCount()` 旁边补 `loadedCount()`：累加各 session 的 `downloaded.size()`，
语义是「磁盘上存在的下载记录数」，配合 `_rows.empty()` 表示「还在 resolve」。
这个信号是安全的：`resolveRequestsFinished()` 会给每条记录填 `object`
（找不到消息就 `generateEntry()` 造一个），文件被删的记录会被 `erase`，
所以 resolve 结束后 `_rows` 必然非空，不会出现「永远转圈」。

顺带修掉 cda5293 留下的空转：`activateRow()` 把 `Downloading` 也归到「跳转」，
但 `buildRows()` 从没给下载中行填 `row.itemId`，`jumpToRow()` 第一行就
`if (!row.itemId.peer) return;` 直接返回 → 点下载中行永远没反应。
补 `row.itemId = id->object.item->fullId();`。

**教训**：`downloadManager()` 的调用点散在多个 widget / controller 里，
推 CI 前必须 `grep -o "downloadManager()\.[A-Za-z_]*"` 与头文件 public 成员逐一对账；
只审计「本次改动的文件」会漏（82b0693 改的就是这个文件，但上一轮清单里没有它）。

## 2026-11 第四次 CI 编译失败

`storage/storage_account.cpp(981)`：

```
error C2275: 'QString': expected an expression instead of a type
error C2143: syntax error: missing ')' before 'string'
error C2059: syntax error: ')'
```

`DEBUG_LOG("[DL] ...")` 写成了单括号。`lib_base/base/debug_log.h:33` 的宏是

```cpp
#define DEBUG_LOG(message) { ... LogWriteDebug(QString message, ...); }
```

也就是参数必须是**能接在 `QString` 后面的括号表达式**，所以正确写法是
`DEBUG_LOG(("text %1").arg(x))`（`LOG` / `PROFILE_LOG` 同理）。
单括号展开成 `QString "[DL] ..."` —— 类型后面跟字符串字面量，编译器就报 C2275。

**教训**：所有 `[DL]` 埋点都是新写的代码，推之前用
`grep -rn "DEBUG_LOG([^(]" --include=*.cpp SourceFiles` 扫一遍单括号写法；
多行写法 `DEBUG_LOG(\n\t\t("...")` 是合法的，扫出来要人工看一眼下一行。
