# BigPage 打开卡死 / 闪退：取证位置与根因

相关：`.ai-memory/downloads-pause-resume.md`（下载数据层）、`docs/DownloadsPageDev.md`（设计）。

## 1. TD 自己的日志里没有崩溃信息

- 主日志 `cWorkingDir() + "log.txt"`；Debug 日志 `DebugLogs\log.txt`（轮转成 `log_<MM_DD>.txt`）；
  MTP `DebugLogs\mtp_*.txt`。本机 `cWorkingDir()` = `D:\APP\T Telegram`（便携目录）。
- 实测：卡死闪退时两份日志都**停在崩溃前几秒**，之后只剩 MTP 心跳；
  **没有任何崩溃行、没有断言失败**。别指望从 TD 日志找崩溃原因。
- TD 的 breakpad 在本机没写出 `.dmp`，`tdata\dumps` 是空的。

## 2. 唯一可靠的崩溃来源：WER 的 minidump（不是 Report.wer）

- `Report.wer`（`C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_AyuGram.exe_*\`）
  只有异常码和故障偏移，**没有堆栈**。
- **完整转储在** `C:\Users\<用户>\AppData\Local\CrashDumps\AyuGram.exe.<pid>.dmp`，
  每个约 105 MB，含全量用户内存 + 全部线程上下文 —— 足以还原调用链。
  实测 2026-11-15 有 10 份（09:18/09:27 的旧构建 3 份，15:01–15:09 的新构建 6 份）。
- 清理：仓库根 `Clear-DebugLogs.ps1`（默认保留 WER，`-WithWer` 才清）。

## 3. minidump 解析要点（踩过的坑）

- 头部 `MDMP` + 流目录（每项 12 字节 {type,size,rva}）。流：3=线程表、4=模块表、
  5=内存表、6=异常、7=系统信息。
- 异常流：ThreadId@0、ExceptionRecord@8（code@8、address@24、NumberParameters@32、
  ExceptionInformation@40，15×u64）、ThreadContext 描述符@160。
- **x64 CONTEXT 寄存器块在上下文的 +0x78**（Rax..Rip 共 17 个 u64）。
  判据：拿每个线程的 Rsp 去比对它自己的栈区间，80/80 命中 0x78。
- **内存表的 Rva 是绝对文件偏移**，不是相对流起点 —— 用流起点会读到一片零。
- `.pdata` 解出 (Begin,End,UnwindInfo)。UNWIND_INFO：byte0=Version(低3位)|Flags(高5位)、
  byte1=SizeOfPrologue、byte2=CountOfCodes、byte3=FrameRegister(低4位)|FrameOffset(高4位)。
  **UNWIND_CODE 的 byte1：低 4 位是 UnwindOp，高 4 位是 OpInfo**（搞反会得到 0x10 这种假帧大小）。
  需要额外数据的 op（ALLOC_LARGE / SAVE_NONVOL_FAR / SAVE_XMM128_FAR）的额外槽
  **算在 CountOfCodes 里**，要边遍历边消费。
- 走栈：`ret = read64(rsp + framesize)`，`rsp += framesize + 8`。
  用 `.pdata` 函数区间 + 调用点回查可校验每一跳。
- 归因锚点：函数里 RIP 相对 `lea` 指向的字符串（Qt 的 `qimage.cpp` / `painter.cpp` 等）。

## 4. 已证实的根因：`ThumbnailFor` 里的 `Image*` 悬垂（use-after-free）

崩溃事实（6/6 份新构建转储完全一致，确定性）：

- 异常码 `0xC0000005`，RVA `0x108df31b` = `mov r9d,[rax+0x30]`，
  所在函数 `0x108df2a0` 是 QtGui 的 `QImage` 矩形拷贝（`QImage::copy` 形状）。
- `rax` = `src->d` = `0xDDDDDDDDDDDDDDDD` —— **MSVC 调试堆释放填充（0xDD = dead）**。
- `src`（源 `QImage*`）= `0x1049a2da100`，**不在转储任何内存段里** → 已释放/未提交。
- `dst` 是栈上局部量，`rect` 是 `QRect(0,0,-1,-1)`（空矩形 → 整图拷贝），
  即 `QImage tmp = *某个悬垂指针;`。

源码级根因（已定位到行）：

- `info_downloads_big_widget.cpp` 的 `ThumbnailFor()` 原来写：
  ```cpp
  if (object.document) {
      const auto view = object.document->createMediaView();   // 局部 shared_ptr
      image = view->thumbnail() ? ... ;                       // 裸 Image*
  }
  ...
  image->original();   // ← view 已出作用域
  ```
- `DocumentData::_media` 是 **`std::weak_ptr`**（`data/data_document.h:412`），
  `createMediaView()`（`data/data_document.cpp:991`）返回的 `shared_ptr`
  在没有其它持有者的情况下就是**唯一所有者**。
- 于是 `view` 在 `if` 块结束时销毁 → `DocumentMedia` 析构 →
  它的 `std::unique_ptr<Image> _thumbnail / _inlineThumbnail / _goodThumbnail`
  （`data/data_document_media.h:108-111`）全部释放 → `image` 悬垂 →
  `image->original()` 读出已释放的 `QImage`，`d` 是 0xDDDD… → 拷贝时读 `d->format` 崩。
- **修复**：把 `shared_ptr<DocumentMedia>` / `shared_ptr<PhotoMedia>` 提到函数作用域，
  让它在 `image->original()` 调用期间一直活着。
- 推论：`Images::Prepare` 拿到垃圾 `QImage`（尺寸/字节数都是填充字节）会走极长的缩放循环，
  这解释了「先卡死再闪退」而不仅仅是「直接闪退」。
- **通用品坑**：`createMediaView()` 返回的 `shared_ptr` 不保证背后对象常驻
  （owner 是 weak_ptr）。任何取出来自 view 的裸指针，都必须让这个 shared_ptr 活到用法之后。

## 5. 旧构建那次闪退是另一回事

- 09:18 / 09:27 的 3 份转储：RVA `0x109a2f88`，读地址 `0x0`（空指针）。
- 那对应 `413957f` 修掉的「命名空间作用域拷贝 `st::*` 样式 → 空字体」：
  `style::StartManager`（`lib_ui/ui/style/style_core.cpp:63`）只在 `Application::run()`
  （`core/application.cpp:347`）里跑，CRT 静态初始化阶段拷到的句柄是 null。
- **通用品坑**：不要在命名空间作用域拷贝 `st::*` 样式对象，要么直接用 `st::`，
  要么在函数/构造里现取（`Ui::Text::String` 只保存 `const style::TextStyle *_st`，
  `lib_ui/ui/text/text.h:545`）。

## 6. 还没解决的

- 已下载（completed）列表在 BigPage 里不显示：`loadedList()`（`data_download_manager.cpp:1038`）
  是惰性视图，过滤 `id.object != nullptr`，而 `object` 要等 `resolveRequestsFinished()`
  才填（`data_download_manager.cpp:1157`）；`resolve()` 还会把 `path` 为空的条目
  **直接从 `downloaded` 里 erase**（`:1142`）。旧设置页能显示说明条目本身还在。
  本轮没动。
- 「下载中列表上方有一条占位」「音乐行只显示半个 `全`」＝ 行高/文本高度问题，
  要等能不闪退地打开页面后才能验证。
- `DownloadManager::loadedList()` 在 UI 线程对每个已下载项做 `QFileInfo` 并发的 MTProto
  请求（`:1061` `resolve()`），历史多时打开页面仍会卡。本轮没动。

## 7. 相关提交

- `413957f` fix: resolve downloads page text style after style start（修掉空字体那次闪退）
- 回退锚点 `v0.1.3-before-bigpage-fix` → `3b4dcc1`
