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

## 5. 下载中列表的落盘（崩溃恢复，`_inflightDownloadsSerialized`）

优雅退出链路（`pauseAll → flushPaused → writeLocationsForce`）覆盖不到闪退，
所以「下载中」必须有一份独立快照，且写入要**勤**（进度变化就写），不能只靠退出。

- 字段一律**追加**在 locations 文件末尾：downloads → paused → inflight，
  读取逐级 `!stream.atEnd()` 守卫。中间插字段会让旧版本读串行。
- 写入调用点只需覆盖 `downloading` 的所有变更口：`addLoading`、`addLoadingExternal`、
  `updateLoadingExternal`、`remove()`、`check()` 的进度分支。
  `pauseAll`/`pauseOne`/`loadingStop`/`clearLoading` 都走 `remove()`，不用再单独加。
- 恢复时**并入 `_paused`**（`reason = Auto`），不要自动续传：崩溃前任务来不及暂停，
  恢复成暂停最安全，也与优雅退出收敛到同一形态。必须在 `validatePausedDownloads()`
  之前并入，让 `.temp` 夹紧逻辑生效。
- `_sessions.size() == 1` 才恢复：快照是全局一份，多账号逐个 trackSession 会重复追加。
- `DownloadingId.path` 是**最终路径**（`FileLoader::_filename`），`.temp` 是
  `path + ".temp"`，所以快照字段与暂停列表完全同构，能直接复用序列化函数。

## 6. 冗余但**不要**动的两处

- `Memento(not_null<UserData*> self)` 的 `self` 未使用：这是所有 Info
  section 的统一签名（`Info::Xxx::Make(self)`），改了会破坏一致性。
- `Row` 存整个 `PausedDownloadDescriptor`：`resumeOne()` 只读其中 5 个字段，
  但裁剪需要改 `resumeOne()` 签名去凑一个 descriptor，收益为零、风险为正。

## 7. 「无法继续该下载」= 消息不在内存（不是 .temp 的问题）

症状：下载 → 退出 APP → 重开 → 点任何一项的「开始」都弹 `无法继续该下载。`。

`Session::data().message(FullMsgId)` **只查内存 `_messages`**（data_session.cpp，无数据库回退），
重启后没打开过的会话一条消息都没有。`resumeOne()` 找不到 `DocumentData` 就 `return false`，
`save()` 根本没被调用 → MTP 日志里连一条 `upload_getFile` 都没有（这是最快的判据：
`mtp_*.txt` 里重启时间点之后没有对应文档的请求，说明失败在 `save()` 之前）。
`Session::document(DocumentId)` 也不可用：它返回的 stub 没有 `_dc` / `_access` / `size`。

修法：`resumeOne()` 找不到文档时调 `requestPausedResume()`，用
`messages.GetMessages` / `channels.GetMessages` + `inputMessageID` 把消息拉回来，
`processExistingMessages()` 入内存后走同一条 `resumePaused()`。
**不要**改成「直接构造 DocumentData」——dc/accessHash/size/fileReference 只能来自消息。
补拉还顺带解决了 fileReference 过期：用新消息的引用，而不是本地 DB 里的旧引用。

排查这类问题先分清「失败在 `save()` 之前还是之后」：之后会有 `upload_getFile`
（offset = `ready`），之前一条请求都没有。
