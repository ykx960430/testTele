# 下载系统（暂停 / 续传 / BigPage）踩坑记录

相关文档：`docs/DownloadsPageDev.md`（完整设计与任务表）。
本文件只记「实测前 review 出来、容易再踩」的坑。

## 1. `FileLoader::pause()` 取偏移的顺序（最隐蔽，曾让整套续传失效）

`currentOffset()` 在 `_paused == true` 时直接返回 `_pausedBytes`。
所以 `pause()` 里必须**先取偏移、后置 `_paused`**：

```cpp
_finished = true;              // 已能防重入（pause 开头检查 _finished）
cancelHook();
if (_fileIsOpen) { _file.flush(); }
_pausedBytes = currentOffset(); // 此时 _paused 仍为 false → 拿到真实偏移
_paused = true;
if (_fileIsOpen) { _file.close(); _fileIsOpen = false; }
```

反例（本次 review 前的工作区状态）：`_paused = true;` 写在
`_pausedBytes = currentOffset();` 之前 → 赋值恒为 0 →
`descriptor.ready` 恒为 0 → BigPage 暂停行显示「0 B / X」→
`resumeOne()` 里 `paused.ready > 0` 的 clamp 分支永不进入 →
`resumeLoad(0)` 从头重下，`.temp` 前缀完全没用上 →
`validatePausedDownloads()` 的 `.temp` size 修正也全部空转。

> 注意：`_finished = true` 放在最前就足够防重入，不需要靠提前置 `_paused`。
> `cancelHook()`（`cancelAllRequests()`）不会回调 `currentOffset()`。

## 2. `DocumentData::save()` 的 `_pendingResumeOffset` 不能「用完再清」

`save()` 有一条提前 return 的分支（`activeMediaView()->loaded(true)`），
清理语句必须放在函数**入口** `base::take` 到局部变量，否则提前返回时残留，
会被下一次无关的 `save()` 用成续传偏移。

## 3. BigPage 的 `loadingList()` 含 `done` 条目

`DownloadManager::loadingList()` 不过滤 `done`，完成后条目还会滞留
`kClearLoadingTimeout`（5 s）。旧 provider（`info_downloads_provider.cpp`）
有 `if (!id->done)`；BigPage 的 `buildRows()` 和 `updateProgress()`
两处都要跳过，否则：
- 完成后约 5 s 内同一条目同时出现在「下载中」和「已完成」两组；
- `updateProgress()` 的 `_downloadingCount` 与行数不一致，可能触发多余 rebuild。

## 4. 暂停记录的落盘

`pauseAll()` / `importPausedJson()` 用 `flushPaused()`（强制
`writeLocationsForce()`）；`pauseOne()` / `resumeOne()` / `cancelPaused()`
也要用，否则 `~Account()` 不补写 locations，1 s 内退出会丢/重记录。
`flushPaused()` 是 private，`writePostponed()` 只更新内存里的序列化缓存。

## 5. 冗余但**不要**动的两处

- `Memento(not_null<UserData*> self)` 的 `self` 未使用：这是所有 Info
  section 的统一签名（`Info::Xxx::Make(self)`），改了会破坏一致性。
- `Row` 存整个 `PausedDownloadDescriptor`：`resumeOne()` 只读其中 5 个字段，
  但裁剪需要改 `resumeOne()` 签名去凑一个 descriptor，收益为零、风险为正。
