# 项目技术档案（.ai-memory/）

这里只记「实测 / 实编译踩出来的事实」，也就是 `docs/`、代码注释和 git log 里查不到的那部分。
`AGENTS.md` 每次会话自动注入，并在开头指向本目录 —— **新会话开始先读这里**。

## 文件

- `ci-windows-build.md` — Windows Debug CI：缓存键、trim 策略、并行度、产物路径，
  以及下载系统前两轮编译失败的根因。
- `downloads-pause-resume.md` — 暂停 / 续传 / 落盘的坑（`FileLoader` 偏移顺序、
  暂停列表序列化、`.temp` 尺寸、`loadingList()` 含 `done`）。
- `downloads-bigpage-crash.md` — BigPage 打开卡死 / 闪退的取证位置、
  空字体根因，以及还没统一的 WER 偏移。

## 写法约定

- 只写**结论 + 证据定位**（`文件:行号` 或 exe 内地址），不写推导过程。
- 一条一个坑，写清「反例会怎样」，让下一个人一眼能认出来。
- 和 `docs/DownloadsPageDev.md` 分工：那份是设计与任务表，这里是踩坑事实。
