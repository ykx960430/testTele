# BigPage 打开卡死 / 闪退：取证位置与根因

相关：`.ai-memory/downloads-pause-resume.md`（下载数据层）、`docs/DownloadsPageDev.md`（设计）。

## 1. TD 自己的日志里没有崩溃信息

- 主日志 `cWorkingDir() + "log.txt"`；Debug 日志 `DebugLogs\log.txt`（轮转成 `log_<MM_DD>.txt`）；
  MTP `DebugLogs\mtp_*.txt`；崩溃转储 `tdata\working`、`tdata\dumps`。
  本机 `cWorkingDir()` = `D:\APP\T Telegram`（便携目录）。
- 实测：卡死闪退时两份日志都**停在崩溃前几秒**，之后只剩 MTP 心跳；
  **没有任何崩溃行、没有断言失败**。别指望从 TD 日志找崩溃原因。
- TD 的 breakpad 在本机没写出 `.dmp`，`tdata\dumps` 是空的。

## 2. 唯一可靠的崩溃来源：Windows WER

- 报告：`C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_AyuGram.exe_*\Report.wer`
  （UTF-16LE，`Get-Content -Encoding Unicode` 读）。
- `Sig[6]` = 异常码（`0xc0000005` = 访问违例），`Sig[7]` = **故障偏移**（按 RVA 与 PE 节表对齐验证过）。
  **没有堆栈、没有故障地址**，只能拿偏移回查 exe。
- 清理：仓库根 `Clear-DebugLogs.ps1`（默认保留 WER，`-WithWer` 才清）。

## 3. 由偏移回查代码的方法（本机没有 Qt 头文件、拿不到 Qt 源码）

- `D:\Work\Libraries` 是空的，`web_fetch` 取不到 Qt 源码 → 只能逆向 `AyuGram.exe`。
- 手算 PE 节表：`.text` 的 VA→文件偏移 = `VA - 0xC00`；`.data` 在 `rawend_va` 之后是零填充，
  落在那里的全局**文件里没有初值字节**，只能说明「没存初值」，不能说明运行期是 0 ——
  要再查有没有初始化函数写它。
- `.pdata` 解出函数区间；扫 `E8 rel32` 找调用点；capstone 必须**从 `.pdata` 的函数起点**
  开始反汇编（从中间开始会失步）。
- 字符串是给 Qt 函数归因最快的锚点（`painting\qpainter.cpp`、`QPainter::setPen: ...`）。
- 断言没触发的判据：`base::assertion::fail` 会写 null，地址是 `0xc9365c8` / `0xc936ac7`；
  崩溃偏移不等于这两个地址，就说明不是断言。

## 4. 已定位的根因：样式全局被提前拷贝（空字体）

`info_downloads_big_widget.cpp` 曾经在匿名命名空间写：

```cpp
const auto kNameStyle = style::TextStyle{ .font = st::downloadsBigNameFont };
```

- `style::StartManager`（`lib_ui/ui/style/style_core.cpp:63`）只在 `Application::run()`
  （`core/application.cpp:347`）里调用，**在 CRT 静态初始化之后**。
- 命名空间作用域的对象在 CRT 阶段初始化，拷到的 `_data` 是 `nullptr`，**整个进程都是 null**。
  exe 里能看到这个初始化函数 `0x486a60`：
  `mov rax,[st::downloadsBigNameFont 的引用指针]; mov rax,[rax]; mov [&kNameStyle],rax`。
- `Ui::Text::String` 只保存 `const style::TextStyle *_st`（`lib_ui/ui/text/text.h:545`），
  排版时读 `_st->font->height`（`text.cpp:1706`）→ 每一行文本都是空字体，列表画不出来。
- **通用品坑：不要在命名空间作用域拷贝 `st::*` 样式对象**（`font` / `color` 都是
  `StartManager` 才解析的句柄）。要么直接用 `st::` 引用，要么在函数或构造里现取。

## 5. 同轮加的保护

- `relayout()` 加布局缓存（同宽度 + 同批行直接返回上次高度）。
  `Info::ContentWidget::updateControlsGeometry()` 每次 resize 都会
  `_innerWrap->resizeToWidth(width())`，`rebuild()` 里还会再排一次。
- `ThumbnailFor()` 按 document / photo id 缓存（上限 200）：原来每次 `buildRows()`
  都在 UI 线程对所有行做 `Images::Prepare` + `Images::Round`。
- `updateProgress()` 的 `_rows[index++]` 和 `_rows[_downloadingBegin + _downloadingCount - 1]`
  原来没有边界检查，会越界写 → 堆损坏。

## 6. 还没解决的

- WER 偏移 `0x109a2f88` 落在 **QtGui** 的 `QPainter::drawText` 链
  （`0x10983590`，调用点在 `paintSmall` / `paintItem` / `ActionBar::paintEvent`），
  读的是 `QPainterState::font`。而空字体按代码路径应该先在 `text.cpp` 里炸 ——
  两条链**没统一**，所以「空字体就是那次闪退」是**推断，不是证实**。
- `DownloadManager::loadedList()`（`data_download_manager.cpp:1038`）在 UI 线程对每个已下载项
  做 `QFileInfo` 磁盘检查并发 MTProto 请求 → 历史多时打开页面仍会卡。本轮没动。
- 验证方式：重新构建后先测「打开下载页」。若还崩，WER 报告留着，拿新偏移继续对。

## 7. 相关提交

- `413957f` fix: resolve downloads page text style after style start
- 回退锚点 `v0.1.3-before-bigpage-fix` → `3b4dcc1`
