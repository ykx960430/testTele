# 流媒体播放器 UAF 闪退：取证位置与根因

相关：`.ai-memory/downloads-bigpage-crash.md`（取证方法论沿用）。
2026-11-16 三次闪退（14:48 / 15:01 / 15:03）签名完全一致，确定性崩溃。

## 1. 崩溃事实（3/3 份转储一致）

- `C:\Users\<用户>\AppData\Local\CrashDumps\AyuGram.exe.{18696,13240,22412}.dmp`
- 异常码 `0xC0000005`（写），Rip RVA 恒为 `0x5F80DF`（ASLR 每次启动基址不同，RVA 不变）
- Rcx = `0xDDDDDDDDDDDDDDDD` —— MSVC 调试堆释放填充（0xDD = dead）
- 崩溃点函数 RVA `0x5F80D0` = `std::_Ref_count_base::_Inc_ref`：
  `add rax,8; lock inc [rax]`，rax = rcx+8 → 对已释放控制块的引用计数 +1

## 2. 完整调用链（.pdata + capstone 还原，均为主线程）

> 本节行号是**修复前**的 `media_streaming_player.cpp`；修复后的行号见第 7 节。

```
_Inc_ref (0x5F80DF)
← shared_ptr 拷贝辅助链 0x9F410D0 ← 0x9F36EA0 ← 0x9F3F8D0 ← 0x9F3E820
← rpl::event_stream::fire_forward (0x9F3A6D0)
← fire 包装 (0x9F420A0)
← Player::checkResumeFromWaitingForData (0x9F2BD00)
← Player::videoReceivedTill (0x9F2B690，Expects 字符串锚点 "_video != nullptr")
← on_main lambda (0x9F2CF50，捕获 {Player*, till})
← crl::guarded_wrap::operator() (0x9F358F0，先 check 后调用)
← crl 主队列任务执行 (0x9F30110) ← Qt 事件循环派发
```

反汇编比对：0x9F2BD00 读 `[this+0x135]`（`_pausedByWaitingForData`）、
`bothReceivedEnough(0xBB8=3000ms=kBufferFor)`、写 `[this+0x1D0]`（`_updates`）——
与 `media_streaming_player.cpp:706-712` 逐行吻合。

## 3. 根因：守卫没错，是「信号处理器同步销毁了正在执行回调的对象」

守卫本身没失效：`guarded_wrap::operator()` 的 check（`base::weak_ptr::get()`，
`lib_base/base/weak_ptr.h:325` + seq_cst 原子读 `tracker->value`）在任务开始时
**确实通过**——那一刻 Player 还活着。

真正的时序（全部同步发生在同一次 lambda 执行内）：

1. File 线程投递 `crl::on_main(&_sessionGuard, [=]{ videoReceivedTill(till); })`
   （`media_streaming_player.cpp:399-400 / :421-429`）。
2. 主线程执行，check 通过 → `videoReceivedTill`（:227）→
   `trackReceivedTill`（:153）→ receivedTill 前进 →
   `trackSendReceivedTill`（:196）→ 仅当 `_remoteLoader && 缓存未满` 时
   `_updates.fire({PreloadedVideo{till}})`（:209）。
3. → `Document::handleUpdate`（`media_streaming_document.cpp:151`）
   PreloadedVideo 分支 → `checkSwitchToHigherQuality()`（:156）→
   `_switchQualityRequests.fire_copy(height)`（:220）。
4. → 订阅者 OverlayWidget（`media_view_overlay_widget.cpp:4928`）→
   `applyVideoQuality`（:5674）→ `clearStreaming()` → `_streamed = nullptr`
   → **同步 ~Instance → ~Document → ~Player → 堆释放（0xDD 填充）**。
5. 栈帧逐层返回（栈没坏，只是 `this` 已死）→ `videoReceivedTill` 继续调
   `checkResumeFromWaitingForData()`（:231）→ 读 0xDD 判真 →
   `_updates.fire({WaitingForData{false}})`（:710）→
   `fire_forward` 里 `if (!_data) return;` 读到 0xDDD（真值）→
   `const auto copy = _data;`（`lib_rpl/rpl/event_stream.h:118`）→
   shared_ptr 拷贝 → `_Inc_ref(0xDDD+8)` → AV。

关键源码位置：
- `media_streaming_player.cpp:227-232`：`videoReceivedTill` 先 `trackReceivedTill`
  后 `checkResumeFromWaitingForData`，中间无任何存活复查。
- `media_streaming_player.cpp:176`：`trackPlayedTill` 有现成的正确写法
  （`base::make_weak(&_sessionGuard)` → fire 后 `if (guard)` 才继续），
  但 `trackSendReceivedTill` 路径和 `checkResumeFromWaitingForData` 没套用。
- `media_view_overlay_widget.cpp:5674-5698`：`applyVideoQuality` 同步
  `clearStreaming()`（:5698 `_streamed = nullptr`）。

## 4. 触发条件（解释"偶发"）

PreloadedUpdate → 画质切换链只在以下条件同时成立才走：
`_remoteLoader`（远程加载）+ 缓存未满（`_fullInCacheSinceStart` 为假）+
文档有多档画质 + 未手动锁定画质。当时日志：批量下载 + VPN 慢 +
`Streaming Error / Packet corrupt` + `Audio Info: recreating audio device`——
正是"慢网络看视频 + 缓冲不足 + 画质切换"。窗口是
「check 通过 → checkResume 执行前」这段同步代码，链路一触发必崩，
链路不触发就只是正常播放 → 一小时崩 2~3 次的量级合理。

## 5. 修复方向（上游代码 bug，非下载重构引入）

`audioReceivedTill` / `videoReceivedTill`（player.cpp:214-232）按
`trackPlayedTill`（:176）的既有模式加存活复查：

```cpp
void Player::videoReceivedTill(crl::time position) {
	Expects(_video != nullptr);

	const auto guard = base::make_weak(&_sessionGuard);
	trackReceivedTill(*_video, _information.video.state, position);
	if (guard) {
		checkResumeFromWaitingForData();
	}
}
```

`audioReceivedTill` 同理。这是最小改动；更彻底的做法是让
`applyVideoQuality` 的重启不与 fire 同帧（但那要动上游 UI 逻辑，不值得）。

## 6. 备忘

- 三份 Report.wer（WER ReportArchive）只有异常码+偏移，无堆栈；全量转储在
  CrashDumps。`analyze_dump.py`（仓库根，py 3.12 + capstone）可直接复用：
  解析异常/寄存器/线程/内存表 + .pdata 回溯 + 字符串锚点归因。
- 坑：栈上未验证的 exe 返回值可能是帧内残留脏数据（上次会话把
  0x9F25F00/0xD61840 误当成调用者，实为 checkResume 帧内残留）。
  可靠做法：用函数序言（push rdi + sub rsp）从崩溃点逐帧向上推 entry_rsp，
  每一跳用「前一地址-5 字节是 E8 call」验证。
- `Document` 按 `std::shared_ptr` 共享（`media_streaming_instance.h:95`），
  `Player _player` 是其值成员（`media_streaming_document.h:82`），
  析构全部在主线程 —— 不要往"跨线程析构"方向查。

## 7. 已落地的修复（commit 见 `git log -- media/streaming/media_streaming_player.cpp`）

`media_streaming_player.cpp` 四处加了存活复查，全部沿用 `updatePausedState()`
（:658）与 `trackPlayedTill()`（:182）的既有写法
`const auto guard = base::make_weak(&_sessionGuard); ... if (!guard) return;`：

| 位置 | 改法 | 挡住什么 |
|------|------|----------|
| `trackReceivedTill`（:153） | `trackSendReceivedTill()` 之后 `if (!guard) return;` | `PreloadedUpdate` → 画质切换 → `clearStreaming()` 同步销毁后，继续读 `_pauseReading` / `bothReceivedEnough()` |
| `trackPlayedTill`（:178） | `trackReceivedTill()` 之后加 `if (!guard) return;`，`PlaybackUpdate` 之后再查一次 | 同一路径 + `_file->wake()` |
| `audioReceivedTill` / `videoReceivedTill`（:223/:239） | fire 前取 guard，之后 `if (guard)` 才调 `checkResumeFromWaitingForData()` | **本次崩溃点**：`_updates.fire({WaitingForData{false}})` 在已释放对象上 |
| `checkResumeFromWaitingForData`（:721） | `updatePausedState()` 之后 `if (!guard) return;` | `updatePausedState()` → `start()` 也可能触发销毁，之后还有一次 `_updates.fire` |

为什么 `event_stream::fire_forward` 里 `const auto copy = _data;`（`event_stream.h:119`）
只在第二次 fire 才崩：第一次 fire 时 `copy` 拿到强引用，`_data` 对应的共享块在
`fire_forward` 期间存活，所以从被销毁的 `Player` 里读到的 `_data` 还是有效的
shared_ptr；等这次 fire 返回、`copy` 析构后共享块也死了，下一次 `_updates.fire`
再读 `_data` 就是 0xDD —— 正好是转储里的 `Rcx = 0xDDDDDDDDDDDDDDDD`。

`guard` 本身在对象销毁后析构是安全的：`base::has_weak_ptr` 内部是堆上的
`details::alive_tracker`（`weak_ptr.h:23-45`），`weak_ptr` 只碰这个 tracker，
不碰已释放的 owner。
