# AyuGram 下载系统重构 - 开发文档

版本: v1.0  
状态: 讨论定稿 → 待开发  
工程根目录: `D:\Work\AyuGramDesktop`

---

## 一、概述

### 1.1 目标

对 AyuGram Desktop（Telegram Desktop Fork）的下载系统做一次完全重构，解决以下 7 个核心问题：

| # | 问题 | 类别 |
|---|------|------|
| P0 | 退出 APP 时下载进度直接消失，无法恢复 | **阻塞性** |
| P1 | 入口太深（面包屑→设置→高级设置→下载） | 体验 |
| P2 | 正在下载和已下载没有分组展示 | 信息架构 |
| P3 | 列表项只显示「视频/图片」不显示具体文件名 | 信息架构 |
| P4 | 没有快捷「在文件夹中打开」 | 效率 |
| P5 | 顶部菜单只有「删除所有文件」一个高危按钮 | 交互安全 |
| P6 | 没有暂停/继续机制，中断后只能重下 | 功能缺失 |

### 1.2 总体方案

大方向：**新建 BigPage + 侧边栏入口 + 持久化暂停** 三位一体。

- **侧边栏**新增下载图标，角标实时显示未完成任务数
- **点击下载图标**进入全新「下载大页面」，上下结构：标题栏 → 统计卡片 → 类型Tab → 正在下载列表 → 已完成列表
- **退出流程**不再取消下载，而是静默暂停并保存进度（含磁盘部分文件断点续传）
- **顶部菜单**不再只有「删除全部」，改为「全部暂停/继续」等低风险操作
- **列表项**显示文件名、来源会话、进度、磁盘操作（在文件夹打开）

---

## 二、代码架构现状（摸底结论）

### 2.1 下载管理器 `Data::DownloadManager`

**头文件**: `Telegram/SourceFiles/data/data_download_manager.h`  
**实现**: `Telegram/SourceFiles/data/data_download_manager.cpp`

```
DownloadManager
├── _sessions: flat_map<Session*, SessionData>
│   └── SessionData
│       ├── downloaded: vector<DownloadedId>     (已完成的)
│       ├── downloading: vector<DownloadingId>    (正在下载的)
│       └── resolveNeeded...
├── _loading: flat_set<HistoryItem*>              (正在下载项)
├── _loadingDocuments: flat_set<DocumentData*>    (正在下载文档)
├── _loadingDone: flat_set<HistoryItem*>          (下载完成标记)
└── _loaded: flat_set<HistoryItem*>               (已完成项)
```

#### DownloadObject（下载绑定结构）
```cpp
struct DownloadObject {
    not_null<HistoryItem*> item;
    DocumentData *document = nullptr;
    PhotoData *photo = nullptr;
};
```

#### DownloadId（序列化标识）
```cpp
struct DownloadId {
    uint64 objectId = 0;  // DocumentId or PhotoId
    DownloadType type = DownloadType::Document;
};
```

#### DownloadedId（已完成序列化）
```cpp
struct DownloadedId {
    DownloadId download;
    DownloadDate started = 0;
    QString path;
    int64 size = 0;
    FullMsgId itemId;
    uint64 peerAccessHash = 0;
    std::unique_ptr<DownloadObject> object;
};
```
⚠️ `DownloadedId` 中 `object` 指针为 nullptr 时（反序列化后）处于"已下载但状态未还原"状态，等待 `resolve()` 阶段重新拉取消息。

#### DownloadingId（正在下载）
```cpp
struct DownloadingId {
    DownloadObject object;
    DownloadDate started = 0;
    QString path;
    Fn<void()> externalCancel;
    int64 ready = 0;
    int64 total = 0;
    bool hiddenByView = false;
    bool done = false;
};
```

#### 关键方法速查

| 方法 | 作用 |
|------|------|
| `addLoading(DownloadObject)` | 新建下载任务 |
| `addLoadingExternal(...)` | 外部文件下载 |
| `cancel(SessionData&, iter)` | 取消并从列表移除 |
| `remove(SessionData&, iter)` | 从列表移除（不取消文件） |
| `clearLoading()` | 清理所有 downloading（不触发取消） |
| `loadingStop(Session*)` | 遍历 cancel 所有下载（含取消+移除） |
| `loadingStopWithConfirmation(callback)` | 弹确认窗后调 loadingStop + 执行 callback |
| `loadingInProgress()` | 是否还有下载在跑 |
| `loadingList()` | 视图：所有 downloading |
| `loadedList()` | 视图：所有 downloaded（跳过 object==nullptr 的） |
| `deleteFiles(ids)` | 按 ID 删除文件 |
| `deleteAll()` | 删除全部 |
| `serializator(session)` | 序列化 downloaded → QByteArray |
| `deserialize(session)` | 反序列化 downloaded |
| `writePostponed(session)` | 触发写入存储 |

#### 信号速查

| 信号 | 触发时机 |
|------|----------|
| `_loadingListChanges` | 新增/移除任何 downloading 项 |
| `_loadingProgress` | 下载中 read/total 变更（来自 DocumentData::progress updates） |
| `_loadedAdded` | 有新的 downloaded 完成 |
| `_loadedRemoved` | downloaded 被删除 |
| `_loadedResolveDone` | 反序列化 resolve 完成 |

### 2.2 下载 UI Provider `Info::Downloads::Provider`

**头文件**: `Telegram/SourceFiles/info/downloads/info_downloads_provider.h`  
**实现**: `Telegram/SourceFiles/info/downloads/info_downloads_provider.cpp`

类继承自 `Media::ListSectionDelegate`，是下载列表的数据源适配器。

#### 内部 Element 结构

```cpp
struct Element {
    not_null<const HistoryItem*> item;
    int64 started = 0;
    QString path;
    int64 size = 0;
    bool found = false;     // 搜索命中标志
    QStringList words;      // 搜索索引
    QList<QChar> letters;   // 首字母索引
};
```

`_elements` 存储所有 downloading + downloaded 项（混在一起），统一按 `started` 时间逆序排列。

#### 核心渲染：`createLayout()`

```cpp
std::unique_ptr<BaseLayout> Provider::createLayout(Element element, ...) {
    // 强制使用 forceFileLayout = true
    // 返回 Overview::Layout::Document 实例
    fields.externalLoading = [item]() -> std::optional<DocumentExternalLoading> {
        auto &manager = Core::App().downloadManager();
        const auto state = manager.loadingExternalState(item);
        if (!state || state->done) return std::nullopt;
        return DocumentExternalLoading{ .ready = state->ready, .total = state->total };
    };
    fields.externalCancel = [item] {
        Core::App().downloadManager().cancelLoadingExternal(item);
    };
    return std::make_unique<Document>(delegate, element.item, fields, songSt);
}
```

问题：
- `forceFileLayout = true` 导致只显示文件类型图标，不显示文件名
- `externalLoading` 只在「外部下载」时有数据，MTProto 下载不走这条路径
- 布局使用 `Overview::Layout::Document` 无法自定义内容

#### fillSections 流程

```
Provider::fillSections(delegate):
  1. 对每个 _elements（逆序）:
     - 搜索模式且 !found: 跳过
     - getLayout(element, delegate): 查缓存或新建
     - 加入 result 的 ListSection
  2. section.finishSection()
  return result
```

#### Provider 现有「取消」回调

```cpp
void Provider::contextMenuItem(... action_id) {
    case ContextMenuItem::CancelDownload:
        // 外部下载: cancelLoadingExternal
        // 普通下载: document->cancel()
    break;
}
```

### 2.3 现有下载页面 Widget

**Widget**: `Info::Downloads::Widget` (`info_downloads_widget.h/.cpp`)

```
Widget
├── _searchField     (搜索按钮)
└── _list           (Media::ListWidget)
```

`Widget::fillTopBarMenu()` 只添加了един «删除全部文件» 按钮。

委托 `Provider::layoutRemoved()` / `computeSelectionData()` / `applyDragSelection()` 给底层 Media 列表处理。

### 2.4 退出流程 `Application::downloadPreventsQuit()`

**文件**: `Telegram/SourceFiles/core/application.cpp:1170`

```
preventsQuit():
  if (downloadPreventsQuit()) return true
  ...

downloadPreventsQuit():
  if (loadingInProgress()):
    loadingStopWithConfirmation([=]{ Quit(); })
    return true   ← 阻塞退出
  return false
```

确认框通过 `Ui::MakeConfirmBox()` 弹出：
- 标题：`lng_download_sure_stop`（「停止下载？」）
- 文案：解释「停止后需要重新下载」
- 确认按钮：「停止下载」→ `loadingStop()` + `Quit()`
- 取消按钮：留在当前页面

### 2.5 文件下载器 `FileLoader` 体系

**Base**: `storage/file_download.h/cpp`  
**MTProto 特化**: `storage/file_download_mtproto.h/cpp`  
**下载任务**: `storage/download_manager_mtproto.h/cpp`

#### mtpFileLoader 关键状态

```cpp
class mtpFileLoader : public FileLoader, private DownloadMtprotoTask {
    int64 _nextRequestOffset = 0;  // 下次请求的偏移量
    bool _lastComplete = false;
    int64 _pausedBytes = 0;        // ← 新增：暂停时已下载字节数
}
```

#### 文件写入逻辑

`FileLoader::checkForOpen()` (file_download.cpp:263):
```cpp
bool FileLoader::checkForOpen() {
    if (_filename.isEmpty() || (_toCache != LoadToFileOnly) || _fileIsOpen)
        return true;
    _fileIsOpen = _file.open(QIODevice::WriteOnly);  // ← 每次创建会截断文件
    if (_fileIsOpen) return true;
    cancel(FailureReason::FileWriteFailure);
    return false;
}
```

`FileLoader::cancel()` (file_download.cpp:347):
```cpp
void FileLoader::cancel(FailureReason fail) {
    const auto started = (currentOffset() > 0);
    cancelHook();  // 子类实现：mtpFileLoader::cancelHook() → cancelAllRequests()
    _cancelled = true;
    _finished = true;
    if (_fileIsOpen) {
        _file.close();
        _fileIsOpen = false;
        _file.remove();   // ← 关键：这里删除了部分文件！
    }
    _data = QByteArray();
    ...
}
```

⚠️ **这里是「退出无法续传」的第二层原因**：不仅 DownloadManager 移除了列表，连磁盘上的部分文件都被删除了。

#### DocumentData 侧的 cancel()

```cpp
void DocumentData::cancel() {
    if (!loading()) return;
    _flags |= Flag::DownloadCancelled;
    destroyLoader();                   // ← 销毁 loader（如果 cancelled 标志存在）
    _owner->documentLoadDone(this);    // 通知加载完成（实际是取消）
}

void DocumentData::destroyLoader() {
    if (!_loader) return;
    const auto loader = base::take(_loader);
    if (cancelled()) {
        loader->cancel();  // 触发 FileLoader::cancel()，删除文件
    }
}
```

所以 `DocumentData::cancel()` → `destroyLoader()` → `loader->cancel()` → `_file.remove()`。

### 2.6 侧边栏 `Window::FiltersMenu`

**头文件**: `window_filters_menu.h`  
**实现**: `window_filters_menu.cpp`

```
FiltersMenu
├── _outer       (RpWidget)
│   ├── _menu    (SideBarButton) — 顶部主菜单
│   └── _scroll  (ScrollArea)
│       └── _container  (VerticalLayout)
│           ├── _list    (TabListLayout) — 筛选文件夹
│           └── _setup   (SideBarButton) — 设置按钮
└── ...
```

#### SideBarButton Badge API

```cpp
void SideBarButton::setBadge(const QString &badge, bool isMuted);
```
见 `filters_menu.cpp:649` 和 `:743` 对文件夹未读/提及数量的角标设置。

#### 按钮创建模式

```cpp
// 简体代码
auto button = prepareButton(_container, id, name);
// id < 0 表示非筛选项
button->setBadge(QString(), false);  // 清空角标
button->setClickedCallback([=]{ /* 跳转 */ });
```

### 2.7 Info Section 体系

**枚举**: `Info::Section::Type` (在 `info/info_controller.h`)
```
enum class Type : uchar {
    Profile, Media, GlobalMedia, CommonGroups, ...,
    Downloads,                                 ← 现有下载 Section
    Stories, SavedMusic, ...,
};
```

**注册方式**: Section 仅通过 enum 值和 `SectionMemento` 子类绑定；  
SectionWidget → WrapWidget → ContentWidget → ContentMemento/Media::ListWidget → Provider

Downloads 异常的 section routing 要点：
- `SectionMemento` 子类: 暂无独立子类，直接使用 `ContentMemento(Downloads::Tag)`
- `Section::Type::Downloads` 在 `wrap_widget.cpp:532` 有专门的 top bar 处理逻辑
- `WrapWidget::setupTopBarWidgets()` 收集 `loadingListChanges` / `loadedAdded` / `loadedRemoved` 来决定是否显示菜单按钮

### 2.8 已下载序列化格式

**序列化流（downloaded 列表）**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `count` | qint32 | 数量 N |
| N × 每条记录见下 |||
| `download.objectId` | quint64 | 文档/图片 ID |
| `download.type` | qint32 | Document / Photo |
| `started` | qint64 | 时间戳（毫秒） |
| `size` | quint32 | 文件大小 |
| `itemId.peer` | quint64 | 来源消息 peer ID |
| `itemId.msg` | qint64 | 来源消息 ID |
| `peerAccessHash` | quint64 | 来源 peer 访问 hash |
| `path` | QString | 本地路径 |

---

## 三、新需求详细方案

### 3.1 侧边栏直达入口

#### 3.1.1 目标

- 侧边栏最下方新增「下载」图标按钮
- 角标实时显示「未完成任务数」（正在下载 + 已暂停）
- 角标规则：0 不显示，1-99 显示数字，≥99 显示「99+」
- 点击切换到大下载页面

#### 3.1.2 需要新增/修改的文件

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `window_filters_menu.h` | 修改 | 新增 `_downloadButton` 类成员 |
| `window_filters_menu.cpp` | 修改 | 创建按钮、订阅信号、回调跳转 |
| `window_filters.style` | 修改 | 新增下载按钮的视觉风格字段 |
| Resources SVG | 新增 | 下载箭头图标 |

#### 3.1.3 代码实现要点

**window_filters_menu.h 改动**：

```cpp
// private members 新增
Ui::SideBarButton *_downloadButton = nullptr;
```

**window_filters_menu.cpp 改动**：

```cpp
// 在 setupFilters() 后追加
_downloadButton = _container->add(
    object_ptr<Ui::SideBarButton>(
        this,
        st::windowFiltersButton,    // 复用现有 folder 按钮样式
        rpl::single(QString()),       // 无文字
        u":/gui/icons/downloads.svg"_q));  // 新图标

_downloadButton->setBadge(QString(), false);
_downloadButton->setClickedCallback([=] {
    if (const auto controller = _controller) {
        controller->show(Info::Downloads::MakeBig(controller->session().user()));
    }
});

// 订阅下载数量变化
auto &manager = Core::App().downloadManager();
rpl::merge(
    rpl::single(false),
    manager.loadingListChanges(),
    manager.pausedListChanges()  // ← 新增信号
) | rpl::on_next([=, &manager] {
    const int total = manager.loadingCount() + manager.pausedCount();
    if (total == 0) {
        _downloadButton->setBadge(QString(), false);
    } else {
        _downloadButton->setBadge(
            total >= 100 ? u"99+"_q : QString::number(total),
            false);
    }
}, lifetime);
```

#### 3.1.4 角标图标规范

- 48x48px 工具栏风格，单色（跟随 palette）
- 视觉参考：Material Design `download` 或 Fluent UI `Arrow Download`
- iconColor 与 `windowFiltersButton.iconFg` 一致

---

### 3.2 新 BigPage 总体架构

#### 3.2.1 页面结构（从上到下）

```
BigPageWidget (Info::ContentWidget 子类)
│
├─ _titleBar          (← WrapWidget 提供，含标题+关闭+搜索框)
│
├─ _searchToggle      (搜索按钮, 可选)
│
├─ _statsRow          (HorizontalLayout, 统计卡片行)
│   ├─ _statVideo      VideoStatWidget   [视频] 数量 大小 总占比%
│   ├─ _statPhoto      PhotoStatWidget   [图片] 数量 大小 总占比%
│   ├─ _statAudio      AudioStatWidget   [音频] 数量 大小 总占比%
│   └─ _statOther      OtherStatWidget   [其他] 数量 大小 总占比%
│
├─ _filterTabs        (SlidingTabStrip, 类型过滤)
│   ├─ 全部 (0)
│   ├─ 视频 (1)
│   ├─ 图片 (2)
│   └─ 其他 (3)
│
├─ _downloadingSectionHeader  (RpWidget)
│   └─ "正在下载 N个文件，共 X MB    [全部暂停|全部继续]"
│
├─ _downloadingScroll  (ScrollArea)
│   └─ _downloadingList (VerticalLayout, item 列表)
│       ├─ DownloadingItemWidget ─────────┐
│       ├─ DownloadingItemWidget          │ 每个 item 高度 ~80-100px
│       └─ ...                            │
│
├─ _downloadedSectionHeader   (RpWidget)
│   └─ "已完成 N个文件"
│
└─ _downloadedScroll    (ScrollArea)
    └─ _downloadedList   (VerticalLayout, item 列表)
        ├─ DownloadedItemWidget
        ├─ DownloadedItemWidget
        └─ ...
```

带滚动的容器：所有区域都在一个大的 `VerticalLayout` 中，外层是一个 `ScrollArea`。统计卡片和 Tab 固定在顶部（sticky），两个列表区域共享滚动。

#### 3.2.2 数据流

```
BigPageWidget
│
├─ _downloadingProvider     (DownloadingProvider)
│   └── 监听：loadingListChanges
│       生成 DownloadingElement { item, ready, total, path, externalState }
│
├─ _downloadedProvider      (DownloadedProvider)  
│   └── 监听：loadedAdded, loadedRemoved
│       生成 DownloadedElement { item, path, size, itemId }
│
└─ _statsAggregator         (StatsAggregator)
    └── 查询：loadedList() / pausedList()
        按 type 分类计数和 size 汇总
```

#### 3.2.3 新增 Section 注册

**Step 1**: `info_controller.h` enum 追加

```cpp
enum class Type : uchar {
    ...,
    Downloads,       // 现有
    DownloadsBig,    // ← 新增
    ...,
};
```

**Step 2**: 新建 Memento + Widget

- `Info::Downloads::BigPageMemento` : public Info::SectionMemento
- `Info::Downloads::BigPageWidget` : public Info::ContentWidget

**Step 3**: `SectionMemento` 子类注册（在 `section_memento.h`）

```cpp
class SectionMemento {
public:
    virtual object_ptr<SectionWidget> createWidget(...) = 0;
    ...
};
```

继承实现 `createWidget()` → `Info::Downloads::BigPageWidget`。

**Step 4**: WrapWidget routing（`info_wrap_widget.cpp`）

在 `setupTopBarWidgets()` 中判 `section.type() == Section::Type::DownloadsBig` 时使用带齿轮的自定义 top bar。

在 `isDownloads()` 旁追加 `isDownloadsBig()` 判断。

在 `downloadsCountChanged` 等段落中对 Big 版做相同处理。

**Step 5**: 齿轮按钮 + 跳转到高级设置

```cpp
} else if (section.type() == Section::Type::DownloadsBig) {
    // 齿轮 button
    const auto gear = _topBar->addButton(
        base::make_unique_q<Ui::IconButton>(_topBar, st::infoTopBarMenu));
    gear->addClickHandler([=] {
        _controller->showSettings(::Settings::DownloadsId());
    });
}
```

#### 3.2.4 DownloadingProvider

继承 `FlatItemDelegate`（或简化版），提供正在下载列表的数据。

```cpp
class DownloadingProvider : public FlatItemDelegate {
public:
    void refresh() override;
    
    struct DownloadingElement {
        not_null<const HistoryItem*> item;
        int64 ready = 0;
        int64 total = 0;
        bool external = false;
        bool done = false;
        QString path;
    };
    
private:
    std::vector<DownloadingElement> _elements;
    base::flat_map<const DocumentData*, ExternalLoadingState> _externalStates;
};
```

刷新时机：`loadingListChanges` 信号。

渲染项构造：
- 从 `_elements` 取数据
- 传给 `DownloadingItemWidget` 构造
- Widget 订阅 `document->progress()` 或 `loadingExternalState()` 以获取实时进度

#### 3.2.5 DownloadedProvider

```cpp
class DownloadedProvider : public FlatItemDelegate {
public:
    struct DownloadedElement {
        not_null<const HistoryItem*> item;
        QString path;
        int64 size = 0;
        DownloadDate started;
        DownloadId download;
    };
};
```

刷新时机：`loadedAdded` / `loadedRemoved` 信号。

#### 3.2.6 统计卡片 StatsAggregator

```cpp
class StatsAggregator {
public:
    struct Stat {
        int count = 0;
        int64 bytes = 0;
        float64 ratio = 0.;  // 占总磁盘比例
    };
    
    Stat video() const;
    Stat photo() const;
    Stat audio() const;
    Stat other() const;
    Stat total() const;
    
    void refresh();
    
private:
    base::flat_map<DownloadType, Stat> _stats;
};
```

数据来源：`loadedList()` 遍历，按 `download.type` 和文件后缀分类（音频需要按 MIME/后缀区分）。

#### 3.2.7 类型 Filter Tabs

定义四种类型：全部、视频、图片、其他（含音频等其他文件）

Tab 切换时：
- 按类型过滤两个 Provider 的显示
- 两种实现：
  - A) Provider 内部 filter：`setFilterType(Type)` + `refresh()`
  - B) UI 层 filter：Tab 切换时遍历 list item，按 data attrs 显示/隐藏

推荐方案 A，因为数据已在 Provider 内部，过滤效率更高。

#### 3.2.8 DownloadingItemWidget（下载中的列表项）

```cpp
class DownloadingItemWidget : public Ui::RpWidget {
public:
    DownloadingItemWidget(
        QWidget *parent,
        const DownloadingProvider::DownloadingElement &element);
    
    void progressUpdated(int64 ready, int64 total);
    void paused();
    void resumed();
    void failed(const QString &reason);
    
    rpl::producer<> openInFolderRequests() const;
    rpl::producer<> pauseResumeRequests() const;
    rpl::producer<> cancelRequests() const;
    rpl::producer<> retryRequests() const;
};
```

**视觉模板**：

```
┌──────────────────────────────────────────────────────────────────────┐
│ ┌────────┐                                                          │
│ │ 缩略图  │ 文件名.mp4                           [▶继续│⏸暂停][✕]  │
│ │ 180px  │ 来源: 张三的群 · 今天 14:23                     73%     │
│ │ 宽高比 │ ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ │
│ │ 保持   │ █████████████████████████░░░░░░░░░░░░░░░░░ 168/234 MB    │
│ └────────┘                                                          │
└──────────────────────────────────────────────────────────────────────┘
```

字段要求：
- 文件名：加粗、单行超出省略
- 来源：灰色副文本，peer+时间
- 进度条：蓝色/品牌色，rounded
- 右侧操作按钮：暂停/继续图标 + 取消图标 + 在文件夹打开（hover 显示）
- 中心靠右：百分比 + 已下载/总大小

**状态机**（每个 item）：

```
下载中 ─── 点击暂停按钮 ──→ 已暂停（可恢复）
                    ├── 网络异常 → 暂停 + 错误标记（显示"网络错误，点击重试"）
                    └── 用户点取消 → 移除（删文件+从列表移除）
                    
已暂停 ─── 点击继续按钮 ──→ 下载中（offset 续传）
```

#### 3.2.9 DownloadedItemWidget（已完成的列表项）

```
┌──────────────────────────────────────────────────────────────────────┐
│ ┌────────┐                                                          │
│ │ 缩略图  │ 文件名.mp4                               [📂在文件夹]   │
│ │ 180px  │ 来源: 张三的群 · 昨天 10:23                          │
│ └────────┘ 大小: 234 MB · MP4 H264                                │
└──────────────────────────────────────────────────────────────────────┘
```

交互：
- 点击文件名/缩略图：打开文件
- 长按/右键：显示操作菜单
  - 在文件夹中打开
  - 另存为...
  - 删除（同时删除文件和记录）
  - 再次下载（重新下载，原文件保留）

#### 3.2.10 长按/右键菜单

在 DownloadingItemWidget 和 DownloadedItemWidget 中：

- 长按触发 `showContextMenu(pos)` → 弹出 `Ui::PopupMenu`
- 菜单项根据状态动态生成

对于 DownloadingItemWidget:
```
┌─────────────────────┐
│ ⏸ 暂停              │  (下载中 → 显示「暂停」; 已暂停 → 显示「继续」)
│ ─────────────────── │
│ 📂 在文件夹中打开    │  (enabled 仅当文件 ready > 0)
│ 💾 另存为...         │
│ ─────────────────── │
│ ❌ 取消下载并删除    │
└─────────────────────┘
```

对于 DownloadedItemWidget:
```
┌─────────────────────┐
│ 📂 在文件夹中打开    │
│ 📄 打开文件          │
│ ─────────────────── │
│ 🗑 删除              │
│ 🔁 再次下载          │
└─────────────────────┘
```

---

### 3.3 退出不丢失 + 持久化 + 断点续传

#### 3.3.1 核心变更总结

| 现状 | 新设计 |
|------|--------|
| 退出弹框阻止，确认后 cancel+remove | 不弹框，自动 pause，保留所有状态 |
| cancel() 删除文件 | pause() 仅停止网络，保留文件 |
| downloading 列表不持久化 | paused 列表持久化到本地存储 |
| 崩溃/闪退即丢下载中任务 | downloading 快照单独落盘，重启后并入暂停列表 |
| resume 时重新下载 | resume 时从上次 offset 续传 |

#### 3.3.2 暂停列表数据结构 `PausedDownload`

```cpp
// 在 data/data_download_manager.h 中新增
enum class PauseReason : uchar {
    User,     // 用户手动暂停
    Auto,     // 退出 APP 自动暂停
    Network,  // 网络异常自动暂停
};

struct PausedDownload {
    // 用于恢复下载的 key fields
    DocumentId documentId = 0;
    PhotoId photoId = 0;
    int64 itemIdPeer = 0;
    int64 itemIdMsg = 0;
    uint64 peerAccessHash = 0;
    DownloadType downloadType = DownloadType::Document;
    
    // 下载进度
    int64 ready = 0;
    int64 total = 0;
    QString path;
    
    // UI 快照（来源消息可能被删除）
    QString displayName;       // 文件名快照
    QString sourceTitle;       // 来源 peer 名称快照
    int64 started = 0;
    PauseReason reason = PauseReason::Auto;
};
```

#### 3.3.3 序列化

新增独立的序列化流 `_pausedSerialized`，使用独立的 `StorageKey`，不与现有的 `downloadsSerialized` 混用（避免破坏向后兼容性）。

**PausedDownload 序列化字段**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `version` | qint32 | 当前写 1（用于日后扩展） |
| `count` | qint32 | 数量 N |
| N × 每条记录 |||
| `documentId` | quint64 ||
| `photoId` | quint64 ||
| `itemIdPeer` | quint64 ||
| `itemIdMsg` | qint64 ||
| `peerAccessHash` | quint64 ||
| `downloadType` | qint32 ||
| `ready` | qint64 | 已下载字节（可超过 32bit） |
| `total` | qint64 | 总字节（可超过 32bit） |
| `started` | qint64 ||
| `reason` | qint32 ||
| `path` | QString ||
| `displayName` | QString ||
| `sourceTitle` | QString ||

#### 3.3.4 StorageKey 注册

在 `storage/storage_account.h` 中新增：

```cpp
// 在现有 *_Key 定义处追加
StorageKey _downloadsPausedKey = 0;
```

反序列化在 `DownloadManager` 构造时进行（类似现有 `deserialize` 机制）。

#### 3.3.5 FileLoader 改造

##### FileLoader::pause()

新增方法（`file_download.h` 声明 + `file_download.cpp` 实现）：

```cpp
void FileLoader::pause() {
    if (_cancelled || _finished) return;
    
    _finished = true;   // 阻止继续处理响应
    _paused = true;     // 标记暂停
    _pausedBytes = currentOffset();  // 保存当前偏移量
    
    cancelHook();  // 子类实现：仅停止网络请求（mtpFileLoader 调用 cancelAllRequests）
    
    if (_fileIsOpen) {
        _file.flush();
        _file.close();   // 关闭句柄但保留文件！
        _fileIsOpen = false;
    }
    // 注意：不调用 _file.remove()
}
```

##### FileLoader 构造: startOffset 参数

构造时允许传入 startOffset（默认 0）：

```cpp
FileLoader(
    ..., 
    int64 startOffset = 0);   // 新增默认参数
```

在 `checkForOpen()` 中处理：
```cpp
bool FileLoader::checkForOpen() {
    if (_filename.isEmpty() || (_toCache != LoadToFileOnly) || _fileIsOpen)
        return true;
    
    if (_startOffset > 0) {
        // 续传模式：打开现有文件，检查大小
        _fileIsOpen = _file.open(QIODevice::WriteOnly | QIODevice::Append);
        if (_fileIsOpen && _file.size() < _startOffset) {
            // 文件被截断，回退到任意续传点
            _startOffset = _file.size();
            _file.seek(_startOffset);
        }
    } else {
        _fileIsOpen = _file.open(QIODevice::WriteOnly);  // 普通下载截断
    }
    
    if (!_fileIsOpen) {
        cancel(FailureReason::FileWriteFailure);
        return false;
    }
    return true;
}
```

##### mtpFileLoader 适配

```cpp
class mtpFileLoader : ... {
    int64 _nextRequestOffset = 0;  // 已有，初始 = startOffset
    int64 _pausedBytes = 0;        // 新增：暂停偏移量
    int64 _startOffset = 0;        // 新增：起始偏移量
};

void mtpFileLoader::cancelHook() {
    cancelAllRequests();  // 只停止网络，不动文件
}

void mtpFileLoader::setStartOffset(int64 offset) {
    _startOffset = offset;
    _nextRequestOffset = offset;  // 从偏移量开始请求
}
```

构造时从父类传递 startOffset：
```cpp
mtpFileLoader::mtpFileLoader(..., int64 startOffset)
: FileLoader(..., startOffset)
, _nextRequestOffset(startOffset)
, _startOffset(startOffset) {
}
```

#### 3.3.6 DocumentData 改造

新增：
- `pauseLoad(int64 *outReadyBytes)` — 调用 `_loader->pause()`
- `resumeLoad(int64 offset)` — 重置 cancelled 状态、创建新的 mtpFileLoader 以 offset 为起点

```cpp
void DocumentData::pauseLoad(int64 *outReadyBytes) {
    if (!loading()) return;
    if (_loader) {
        _loader->pause();
        if (outReadyBytes) *outReadyBytes = _loader->fileName().isEmpty() 
            ? _loader->currentOffset() : QFileInfo(_loader->fileName()).size();
    }
    _flags |= Flag::DownloadPaused;
    // 注意：不清空 _loader，不调用 destroyLoader
    _owner->documentLoadDone(this);  // 通知加载完成（暂停也是完成）
}
```

```cpp
void DocumentData::resumeLoad(int64 offset) {
    _flags &= ~(Flag::DownloadCancelled | Flag::DownloadPaused);
    
    if (_loader) {
        // 已有 loader: 尝试 wake-up
        _loader->resume();   // 新增方法：_setFinished(false), _cancelled(false), restart network
    } else {
        // loader 已销毁，重建
        status = FileReady;
        auto newLoader = std::make_unique<mtpFileLoader>(
            &session(),
            StorageFileLocation(...),
            origin,
            locationType(),
            toFile,
            size,
            size,
            ..., 
            offset);   // ← 传递 startOffset
        _loader = std::move(newLoader);
        handleLoaderUpdates();
    }
    if (loading()) _loader->start();
}
```

##### DocumentData::resume() (唤醒暂停的 Loader)

```cpp
// FileLoader 基类新方法：
void FileLoader::resume() {
    if (!_paused) return;
    _paused = false;
    _cancelled = false;   // 解除 cancelled 状态
    _finished = false;
    // 调用 startLoading() 子类实现，从 _pausedBytes 偏移继续
}
```

#### 3.3.7 DownloadManager 新增接口

```cpp
class DownloadManager {
public:
    // === 新增接口 ===
    
    // 暂停所有下载（不恢复，仅 user trigger）
    void pauseAll(Main::Session *onlyInSession = nullptr);
    
    // 恢复所有已暂停的下载
    void resumeAll(Main::Session *onlyInSession = nullptr);
    
    // 暂停单个
    void pauseOne(not_null<const HistoryItem*> item);
    
    // 恢复单个（返回 true 表示已找到并恢复）
    bool resumeOne(PausedDownload descriptor);
    
    // 取消单个已暂停任务（删除记录，保留文件让用户管理）
    void cancelPaused(not_null<const PausedDownload*> descriptor);
    
    // 静默停止（供退出流程使用：= pauseAll + 不发信号阻塞）
    void loadingStopSilent(Main::Session *onlyInSession = nullptr);
    
    // 查询
    int loadingCount(Main::Session *onlyInSession = nullptr) const;
    int pausedCount() const;   // 跨 Session 的聚合
    
    ranges::any_view<const PausedDownload*> pausedList() const;
    
    // 信号
    rpl::producer<> pausedListChanges() const;
    
private:
    std::vector<PausedDownload> _paused;
    rpl::event_stream<> _pausedListChanges;
};
```

#### 3.3.8 退出流程改造

**File**: `core/application.cpp`

```cpp
bool Application::downloadPreventsQuit() {
    // 原实现：if (loadingInProgress()) { loadingStopWithConfirmation(...); return true; }
    
    // 新实现：静默暂停所有下载，不阻塞退出
    if (_data->loadingInProgress()) {
        _data->loadingStopSilent();
        // 可选：show 一个短 toast：tr::lng_downloads_paused_on_quit();
    }
    return false;  // 允许退出
}
```

`loadingStopSilent()` 当前实现：等价于 `pauseAll()` + 发出 pausedListChanges 信号。

#### 3.3.9 恢复下载的完整性检查

启动时反序列化后，对每个 PausedDownload 做恢复性检查：

```
对每个 descriptor:
  1. path 对应的文件是否存在？
     - 否 → 标记为 "file_missing"，置灰显示
     - 是 → 继续
  2. QFileInfo(path).size() == descriptor.ready？
     - 否（文件损坏/截断）→ 设置 ready = qMin(fileSize, ready)，尝试续传
     - 是 → 就绪
  3. 尝试通过 itemId + accessHash 还原 HistoryItem？
     - 成功 → sourceTitle 用历史数据更新
     - 失败 → sourceTitle = descriptor.sourceTitle快照（可能显示「来源已过期」）
```

结果分为三类：

| 状态 | UI 表现 | 用户操作 |
|------|--------|---------|
| Ready（文件完整，来源可用） | 正常颜色，显示「继续」 | 点击继续续传 |
| SourceExpired（文件完整，来源不可用） | 正常颜色，「来源不可用」副文本 | 仍可继续续传（文件仍有效） |
| Corrupted（文件损坏） | 灰显，红色文字「文件不完整，需重新下载」 | 仅能重新下载 |
| FileMissing（文件被删） | 灰显，红色文字「文件不存在」 | 重新下载 |

#### 3.3.10 部分文件命名

下载过程中应先写入临时文件（带 `.download` 后缀），完成后再 rename 为最终文件名。这样：

1. 恢复时可以直接找到 `.download` 文件，对比 size
2. 已完成文件被用户正常管理（不会混有未完成文件）
3. 异常中断时容易区分「需恢复」vs「已完成」文件

**FileLoader 现有行为**: 查看 `_filename` 传递方式——`DocumentData::save(origin, toFile)` 已传最终路径。需要改造为：
- 传入 `toFile` 作为最终文件名
- FileLoader 内部使用 `toFile + u".download"_q` 作为临时写入文件
- finalizeResult() 时 rename `.download` → 原文件名

这会导致首次恢复时也是从 `.download` 续传，最终完成时 rename。

---

## 四、多 Account 处理

### 设计原则

用户明确要求：**下载不与 Account 关联，任何有待下载任务就显示。**

### 实现要点

1. **侧边栏角标**: 跨 Session 聚合 `Σ(loadingCount + pausedCount)`（调用 DownloadManager 的聚合统计）
2. **BigPage 页面**: 显示所有 Session 的任务混合在一起
3. **来源显示**: 如果来自其他 Account，前缀显示 Account 名称或头像（可简化为显示 peer 的 displayName）
4. **`loadingStopSilent`**: 默认只针对当前 Session；退出时需要遍历所有 Session（内部通过 `std::unordered_map<Session*, SessionData>` 已实现）

当用户从 Account A 切到 Account B 时：
- 大页面内容需要刷新（新 Session 的任务列表）
- Provider 需要重新绑定到新 Session 的 DownloadManager

做法：BigPageWidget 内持有当前 `Main::Session*`，Session 切换时重新初始化 Provider。

---

## 五、UI 风格规范

### 样式文件

新建 `info/downloads/info_downloads_big_page.style`（或通过包含 `info/info.style` 追加）：

```style
using "ui/basic.style";
using "ui/widgets/widgets.style";

downloadStatsCard {
    width: pixels;
    height: pixels;
    topMargin: margins;
    padding: margins;
}
download_statsCardVideo : downloadStatsCard {}
download_statsCardPhoto : downloadStatsCard {}
download_statContent : ... {}
...

download_item {
    height: margins;
    padding: margins;
}

download_titleText : font(15px);  // 文件名
download_infoText : font(12px);   // 副文本
download_progress : ... {}
```

配色：与现有的 `box`、`menu`、`popupMenu` 体系保持一致，背景跟随 `windowBg`，卡片使用 `boxBg`/`windowBgOver`。

### 图标资源（需设计及切图）

| 资源名 | 用途 |
|--------|------|
| `downloads.svg` | 侧栏入口（48px） |
| `folder-open.svg` | 在文件夹打开（24px） |
| `pause.svg` | 暂停（24px） |
| `resume.svg` | 恢复（24px） |
| `cancel.svg` | 取消（24px） |
| `info.svg` | 错误提示（24px） |

---

## 六、开发任务拆分

### Phase 1：基础设施（核心）✅ 已完成

**Git 锚点**: `v0.1.0-phase1-pause-resume`  
**提交**: `b7f1567` feat: download system Phase 1 - pause/resume infrastructure  
**改动**: 11 个文件

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| T1.1 | `PauseReason` 枚举 + `PausedDownloadDescriptor` 结构体 | `data_download_manager.h` | ✅ |
| T1.2 | `FileLoader::pause()` / `resume()` / `setStartOffset()` + checkForOpen Appender 模式 | `file_download.h/.cpp` | ✅ |
| T1.3 | `mtpFileLoader` 全部 4 个构造函数加 `startOffset` 参数 | `file_download_mtproto.h/.cpp` | ✅ |
| T1.4 | `.temp` 部分文件命名（下载时 `.temp`，完成后 rename） | `file_download.cpp` | ✅ |
| T1.5 | `DocumentData::pauseLoad()` / `resumeLoad()` + `DownloadPaused` 标志 | `data_document.h/.cpp` | ✅ |
| T1.6 | `DownloadManager::pauseAll()` / `pauseOne()` | `data_download_manager.cpp` | ✅ |
| T1.7 | `DownloadManager::resumeOne()` / `cancelPaused()` | `data_download_manager.cpp` | ✅ |
| T1.8 | `serializePaused()` / `deserializePaused()` — QDataStream round-trip | `data_download_manager.cpp` | ✅ |
| T1.9 | `updatePausedDownloads()` / `pausedDownloadsSerialized()` + write/read 流 | `storage_account.h/.cpp` | ✅ |
| T1.10 | `writePostponed()` 中增加 paused 序列化调用 | `data_download_manager.cpp` | ✅ |
| T1.11 | `Application::downloadPreventsExit()` — 不再阻塞退出 | `core/application.cpp` | ✅ |
| T1.12 | `loadingStopSilent()` — 调 `pauseAll()` | `data_download_manager.cpp` | ✅ |
| T1.13 | `loadingCount()` / `pausedCount()` / `pausedList()` 只读访问器 | `data_download_manager.h/.cpp` | ✅ |
| T1.14 | `pausedListChanges()` rpl 信号 | `data_download_manager.h/.cpp` | ✅ |
| T1.15 | `validatePausedDownloads()` — 反序列化后校验文件 size | `data_download_manager.cpp` | ✅ |

**未编译验证**：工作区 `D:\Work\AyuGramDesktop` 没有预配的 CMake `out/` 目录，需要在有 VS2022 + Qt 环境的 x64 Native Tools Command Prompt 中执行 `cmake --build out --config Debug --target Telegram` 验证。

### Phase 2-A：BigPage 骨架 ✅ 已完成

**Git 锚点**: `v0.1.1-before-bigpage`（开发前）  
**提交**: `11050c5` feat: BigPage skeleton — section registration and empty widget  
**改动**: 8 文件，+172 行

| # | 任务 | 文件 | 状态 |
|---|------|------|------|
| T2A.1 | `Section::Type::DownloadsBig` 枚举 + `DownloadsBig::Tag` | `info_controller.h` | ✅ |
| T2A.2 | `Info::Key(DownloadsBig::Tag)` + `isDownloadsBig()` | `info_controller.h/.cpp` | ✅ |
| T2A.3 | `ContentMemento(DownloadsBig::Tag)` + `key()` 分支 | `info_content_widget.h/.cpp` | ✅ |
| T2A.4 | `BigPageMemento` + `BigPageWidget` stub 类 | `info/downloads/info_downloads_big_widget.h/.cpp` | ✅ |
| T2A.5 | `DownloadsBig::Make(self)` 工厂函数 | `info/downloads/info_downloads_big_widget.cpp` | ✅ |
| T2A.6 | WrapWidget 路由守卫 (setupTopBarWidgets / activeChat / requireTopBarSearch) | `info_wrap_widget.cpp` | ✅ |
| T2A.7 | CMakeLists.txt 添加新文件 | `Telegram/CMakeLists.txt` | ✅ |

### Phase 2-B：BigPage 视觉层 ✅ 已完成（工作区，未提交）

**实现方式调整**：为降低第一版的复杂度与风险，采用单一 `InnerWidget`（自绘行）而不是多个独立 ItemWidget +
独立 Provider 的组件树。Stat 卡片与 FilterTabs 暂时不做，先用「总计行 + 三个分组标题」代替，
后续可以在同一 `InnerWidget` 上增量加视觉层，不需要重构数据流。

| # | 任务 | 实际实现 | 状态 |
|---|------|---------|------|
| T2B.1 | 统计信息 | `buildRows()` 里统计 `loadedList()` 总大小，产出 `lng_downloads_big_total` 的 Stats 行 | ✅ |
| T2B.2 | FilterTabs | 暂缓（改用分组标题：Downloading / Paused / Downloaded） | ⏸ 后续 |
| T2B.3 | 下载中列表项 | `paintItem()`：圆角图标框 + 文件名（中间省略）+ 「已下载 x / 总大小」+ 圆角进度条 | ✅ |
| T2B.4 | 已完成列表项 | 同上布局，状态行显示文件大小；点击 → `File::ShowInFolder(path)` | ✅ |
| T2B.5 | 容器重构 | `InnerWidget : Ui::RpWidget` + `resizeGetHeight()` + `rebuild()/relayout()`，`Widget::setInnerWidget` 挂载 | ✅ |
| T2B.6 | 视觉样式 | 新增 `info/downloads/info_downloads_big.style`，已注册进 `td_ui.cmake` 的 `style_files` | ✅ |

**数据流**：`rpl::merge(loadingListChanges, pausedListChanges, loadedAdded, loadedRemoved, loadedResolveDone)`
→ `rebuild()`；进度条单独订阅 `loadingProgressValue()` 只触发 `update()`，不重建列表（避免每帧重建行模型）。
`rebuild()` 内有 `_rebuilding` 重入保护，因为 `resize()` 会同步回调 `resizeGetHeight()`。

**当前可交互行为**（有意收敛范围）：

- 暂停行 → 点击 resume（`DownloadManager::resumeOne(descriptor)`）
- 已完成行 → 点击在文件夹中显示
- 下载中的行**故意不可点**，避免误触暂停；暂停/取消等操作留给 T2C.4/T2C.5 的顶栏菜单与右键菜单

### Phase 2-C：侧边栏入口 + 交互闭环 🚧 部分完成

| # | 任务 | 实际实现 | 状态 |
|---|------|---------|------|
| T2C.1 | 下载图标 | 复用已有 `Resources/icons/menu/download.png`（含 @2x/@3x），未新增 SVG | ✅ |
| T2C.2 | 侧边栏按钮 + 角标 | `FiltersMenu::setupDownloadsButton()` / `updateDownloadsBadge()`；按钮挂在 `_outer` 底部（`updateDownloadsButtonGeometry()`），**不随 `_scroll` 滚动、不参与文件夹列表折叠**；角标订阅 `loadingListChanges + pausedListChanges`，显示 `loadingCount + pausedCount`；随 `chatFiltersTabsMode` 切换 icon/text 显隐 | ✅ |
| T2C.3 | 跳转 BigPage | `_downloads->setClickedCallback` → `_session->showSection(Info::DownloadsBig::Make(user))` | ✅ |
| T2C.4 | 全部暂停/继续 | 已从顶栏菜单移到 BigPage 顶部**固定操作条**（`ActionBar`，自绘、不随列表滚动，`setScrollTopSkip(st::downloadsBigBarHeight)` 让滚动区从条下面开始）；顶栏菜单（`info_wrap_widget.cpp` 给 `Section::Type::DownloadsBig` 加 `addTopBarMenuButton()`）只保留导出/导入 | ✅ |
| T2C.5 | 右键菜单（暂停/恢复/取消/在文件夹打开） | — | ⏳ |
| T2C.6 | 文本本地化 | BigPage 的 6 个 `lng_downloads_big_*` + 顶栏菜单 9 个 `lng_downloads_*` key 全部已加；新增 `Tr(en, zh)` 兜底（见下） | ✅ |

> 注意：`T2C.2` 的按钮是 `_outer` 的直接子控件（不是 `_container` 的子控件），所以文件夹列表再长也不会把它挤出可视区；
> `_scroll` 的高度由 `updateDownloadsButtonGeometry()` 里 `height - _menu.height() - bottom` 得到，按钮本身 `raise()` 在最上层。

### 第一轮构建测试反馈修复（第一轮真机反馈）🚧 工作区，未提交

对应反馈：5 个严重 bug + 6 项 UI 不符 + 2 项优化。

#### 严重 bug

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| B1 | 有下载中退出 → `can't dereference invalidated vector iterator` | `pauseAll()`/`pauseOne()` 持有 `data.downloading` 的迭代器，`document->pauseLoad()` 会同步走 `documentLoadDone → _documentLoadProgress.fire_copy → DownloadManager::check()` → `remove(data, i)` 把该元素删掉 | 不再跨 `pauseLoad()` 持有迭代器：先拷出 `object/path/total/started`，暂停后用 `ranges::find(data.downloading, item, ByItem)` 重新定位再 `remove`（`data_download_manager.cpp`） |
| B2 | 重开 APP 后「下载中」任务丢失 | B1 的连带后果：崩溃发生在 `pauseAll()` 里、`flushPaused()` 之前，暂停列表没落盘 | 同 B1；退出链路 `preventsQuit → downloadPreventsQuit → loadingStopSilent → pauseAll → flushPaused → writeLocationsForce()` |
| B3 | 点「全部暂停」→ 任务全丢 + 警报 + 闪退 | 同 B1，`pauseAll()` 在第一个元素就被迭代器失效打断 | 同 B1 |
| B4 | 另存为下载的视频，页面显示原始文件名 | 行标题原来取 `document->filename()`（消息里的名字），没用 `id->path`（用户另存为的路径） | 新增 `TitleFor(path, fallback)`：优先 `QFileInfo(path).fileName()`；`File::ShowInFolder(row.path)` 本来就用的保存路径 |
| B5 | 下载中时导出提示 `There is nothing to export` | `exportPausedJson()` 只序列化 `_paused`，下载中的任务不在里面 | 追加遍历各会话的 `data.downloading`（带当前 `ready/total`）；`items` 为空才返回空；Toast 计数改为 `pausedCount() + loadingCount()` |

#### UI 不符项

| # | 反馈 | 处理 |
|---|------|------|
| U1 | 侧边栏下载按钮要在最下方、不随列表折叠 | 已修：按钮改为 `_outer` 子控件，`move(0, height - buttonHeight)` |
| U2a | 封面、来源会话、进度、磁盘操作 | 四项都已做：封面缩略图（`ThumbnailFor()` 走 `createMediaView()->thumbnail()/thumbnailInline()`，无图时画占位图标）、来源会话（下载中取 `peer->name()`，暂停取 `descriptor.sourceTitle`）、进度文字、磁盘操作 = 整行点击 `ShowInFolder` |
| U2b | 文案全是英文 | 新增本地 `Tr(en, zh)`：`ChineseUi()` 判断 `Lang::Id()`（`zh` 前缀或 `QLocale` 判定 Chinese），BigPage 全部文案 + Toast 走 `Tr()` |
| U2c | 页面太窄 | 已做：BigPage 是以 `Info::LayerWidget`（居中浮层）呈现的，宽度被 `infoDesiredWidth`(392px) 卡住；现改为按 `Info::Key::isDownloadsBig()` 取 `st::downloadsBigLayerWidth`(780px)，约为原来两倍（`info_layer_widget.cpp::parentResized`） |
| U2d | 「下载中」要可折叠 | 已做：分组标题右侧画展开/收起箭头（`st::downloadsBigToggle`，收起时旋转 180°）；折叠状态只影响行，标题与计数保留；折叠时 `updateProgress()` 直接跳过，不会每帧重建 |
| U2e | 全部暂停/全部继续固定显示在下载中模块顶部 | 新增 `ActionBar`（固定高度 46px，两个自绘按钮，hover/禁用态）；列表滚动区通过 `setScrollTopSkip()` 从条下方开始，所以条不跟着滚 |
| U2f | 已完成列表要按完成时间倒序 | `buildRows()` 里 `std::sort(completed, a.started > b.started)` |
| U3a | 已完成项：缩略图 + 文件名（可多行、必须完整显示）+ 文件大小 · 来源会话 | 文件名用 `DrawTitle()`：`Ui::Text::SimpleGeometry(w, 0, 0, false)` 后手动置 `geometry.breakEverywhere = true`（TD 文本引擎只在 `breakEverywhere` 时才拆超长词），行高用 `countHeight(w, true)` 动态计算；状态行 = `大小 · 来自 X` |
| U3b | 已完成项点击跳转到来源消息 | `jumpToRow()` → `_controller->showPeerHistory(peer, Way::Forward, msg)` |
| U3c | 正在下载项右侧多一个下载/暂停图标 | `relayout()` 给下载中/暂停行预留 `downloadsBigActionSize` 宽度，`paintItem()` 画 `history_file_pause` / `history_file_play`；点击该矩形直接暂停/继续 |
| U3d | 暂停任务必须留在「下载中」列表 | `buildRows()` 把 `loading` 与 `paused` 行合并进同一个「下载中」分组（`activeCount = loading + paused`），不再单设「已暂停」组；暂停行状态前缀「已暂停」 |

#### `Tr(en, zh)` 的设计取舍

上游只随包发英文 `lang.strings`，其余语言来自 Telegram 语言包；自定义 key（`lng_downloads_big_*`）
不在官方语言包里，所以选中文时也只能回退到英文 —— 这就是 U2b 的根因。
现阶段的折中是：文案仍然先取 `tr::lng_*(tr::now)`（保留未来接语言包的可能），
再由 `Tr(en, zh)` 在中文界面上覆盖成中文。代价是新增文案需要在两个地方各写一次。

#### 第二轮真机 UI 反馈（①~⑤，2026-11-16）

| # | 反馈 | 处理 |
|---|------|------|
| ① | 导出下载列表时把「已下载」一起导出 | `exportPausedJson()` 追加只读 `completed` 数组（documentId/photoId/peer/msg/size/started/path/displayName/sourceTitle）。**只能记录、不能导入**：恢复一条已完成记录需要源消息在内存里，JSON 给不了；`importPausedJson()` 仍只认 `version == 1` 与 `items`，未知 key 直接忽略 |
| ② | 顶栏两个按钮重叠且溢出到弹窗右侧（只看得到一个「全」字） | 根因：`ActionBar::updateRects()` 把 `right`（右边缘 X）当成**左边缘**用，且从不减去按钮宽度，两个按钮一路画到控件右边界之外。改为从 `downloadsBigBarPadding.left()` 开始从左往右排 |
| ③ | 两行文件名与「来自 X · 大小」重叠 | **未锁定根因**。测量与绘制是同一条路径（`countHeight(w, true)` ↔ `DrawTitle()` 的 `SimpleGeometry(w, 0, 0, false)` + `breakEverywhere = true`；Renderer 两者都取 `_geometry.breakEverywhere`），静态比对找不到差异。本轮只做加固：状态行改成独立矩形 `row.statusRect`（原来是在 `row.text` 的高度上做减法），间距 2px → 4px。下一轮若仍重叠，需要截图 |
| ④ | 顶部「共 xxx MB」与单项大小要能显示 GB | `Ui::FormatSizeText` 最大只到 MB（`ui/text/format_values.cpp`），所以新增本地 `SizeText()`：≥1 GiB 时输出一位小数的 GB，否则回落到 `FormatSizeText`。进度、已完成状态行、统计卡片三处统一走它（没有改上游公共函数） |
| ⑤a | 下载中/暂停的小图标与封面互斥 | 改为**叠加**：`relayout()` 算出封面上的 `row.badge` 矩形（22px 圆角底色 + 居中图标），`paintItem()` 在封面之后绘制 |
| ⑤b | 点「下载中」行应该跳转会话，而不是触发继续 | `Row::clickable()` 现在对 Downloading/Paused/Completed 一律为真；`activateRow()` 先判右侧动作矩形（命中才暂停/继续/显示文件夹），否则整行 `jumpToRow()`。暂停行因此需要真实 msg，`buildRows()` 补上 `row.itemId` |
| ⑤c | 「已下载」行右侧加文件夹图标，点击在资源管理器中显示 | 复用已完成行已有的 `row.path`（保存路径）：`relayout()` 给它也预留动作位（`row.folder`），点击走 `File::ShowInFolder(row.path)`，图标 `st::menuIconShowInFolder` |

⑥（会话里已下载/另存为过的视频，界面下载图标要显示成已下载）**用户已确认现有行为符合需求，不做**。

#### 仍待处理

| # | 项 | 为什么没做 |
|---|----|-----------|
| U4a | 已完成项的「来源会话」需要真实可点的链接样式 | 目前是整行点击跳转，没有单独画出「来自 X」的链接外观；已满足「点击跳转到对应消息」的功能需求 |
| U4b | 两行文件名与状态行重叠（③） | 静态比对没能锁定根因，已做加固；若复现需要截图或真机上打印测量值 |
| T2C.5 | 右键菜单（暂停/恢复/取消/在文件夹打开） | 未开始 |

> 暂停任务的数据模型说明：`pauseOne()` 会把条目从内存中的 `data.downloading` 容器**移到** `_paused`
> 列表（这是容器之间的搬移，不是业务删除，`DownloadingId` 里的 `externalCancel`/进度也一并转成
> `PausedDownloadDescriptor` 保存）。BigPage 现在把两者都画在「下载中」分组里，所以暂停后任务不会从
> 页面上消失。之前「先拷出来再删」的描述仅指 `pauseAll()` 为了避开迭代器失效而先拷字段再重新定位，
> 与 UI 分组无关。

### 第三轮真机反馈（2026-11-17）· 方案已定，待实现

本轮 CI `24bbf42`（tag `v0.1.9-build-crash-fix`）修掉两件事：`DEBUG_LOG` 单括号编译错误、
流媒体播放器 UAF 闪退（`media/streaming/media_streaming_player.cpp`，见
`.ai-memory/streaming-player-uaf.md` 第 7 节）。出包后用户提了 5 项，逐条定案如下。
**规划/实现分开，本节就是给实现会话的交接。**

#### 先确认的既有结论（上一轮遗留，别重做）

- ② 顶栏按钮重叠、④ MB→GB、⑤a/b/c、① 导出含已下载、⑨ `[DL]` 埋点：已提交（`8d722e4` / `24bbf42`）。
- ③ 两行文件名与状态行重叠：**只做了加固，根因未锁定**（U4b）。若还复现，先要截图再查。
- 续传 `[DL]` 日志（`validate` / `resume: start/clamp/did not start`）本轮才第一次有数据，
  下一轮验证时先看 `DebugLogs/log_*.txt` 里 `[DL]` 开头的行。

#### R4-1 计数口径：显示「下载中 + 暂停中」，完成立刻 -1

- 左侧栏角标 = `loadingCount() + pausedCount()`
  （`window/window_filters_menu.cpp::updateDownloadsBadge()`，约 :511）；
  联系人列表下载条数字 = `MakeDownloadBarContent()` 的 `content.count`
  （`data/data_download_manager.cpp` 约 :2144，显示在 `ui/controls/download_bar.cpp` 约 :86）。
- 根因：完成时不移除条目，而是先挪进 `_loadingDone`（`data_download_manager.cpp` 约 :649），
  等 `kClearLoadingTimeout = 5s` 的 `_clearLoadingTimer` 到点才由 `clearLoading()`（约 :1220）删除；
  而这个定时器只在 `_loading.empty()`（全部下完）时才启动（约 :658）。
  `loadingCount()`（约 :1179）数的是 `downloading.size()`，把「已完成还没清」的算了进去，
  所以「一个接一个下 / 完成」时数字只涨不跌。
- 决定：**计数只算未完成（`!done`）的条目**，完成那一瞬间就减 1
  （`check()` 里的 `_loadingListChanges.fire({})` 已经会通知两处 UI）。
  - `loadingCount()` 改为数 `!done`；
  - `MakeDownloadBarContent()` 的 `count` 同理（`done` 继续单独数）；
  - 左侧栏**保留** `+ pausedCount()`（用户选的是「下载中 + 暂停中」）；
  - **连带必改**：`download_bar.cpp` 约 :76 的 `_finished = (_content.done == _content.count)`
    依赖 `count` 含已完成项，改口径后要换成「还有未完成条目」的判断，否则「全部完成」动画失效。

#### R4-2 除了「高级设置」，其它入口全部换成 BigPage

`Info::Downloads::Make(...)` 全仓库共 5 处：

| 位置 | 是什么 | 处理 |
|------|--------|------|
| `dialogs/dialogs_widget.cpp:1660` / `:1669` | 联系人列表顶部下载条（点条目 / 「查看」） | **换** |
| `dialogs/ui/dialogs_suggestions.cpp:2195` | 「全部下载」标签页 | **换** |
| `settings/sections/settings_chat.cpp:1954` | 聊天设置里的「下载」入口 | **换** |
| `settings/sections/settings_advanced.cpp:187` | 高级设置里的「下载」入口 | **不动**（用户明确排除） |

换成 `Info::DownloadsBig::Make(controller->session().user())`（签名 `Make(not_null<UserData*>)`，
`info/downloads/info_downloads_big_widget.h:66`），并加对应 include。

#### R4-3 「全部暂停 / 全部继续」并入「下载中」分组

- 用户明确要求：这两个按钮**和下载中列表一同显示、放在一起**，跟着分组折叠/展开；
  没有下载中任务时不显示。所以**不做**「保留固定条、只控制显隐」的折中方案。
- 现状：`ActionBar` 是 `Widget` 的固定子控件（`info/downloads/info_downloads_big_widget.cpp`
  约 :1100），靠 `setScrollTopSkip(barHeight)` 给滚动区让位（约 :1161），所以常驻、不跟随折叠。
- 实现要点：
  1. `Row` 增加一种 action bar 行（`relayout()` 给它一个矩形，高度沿用 `st::downloadsBigBarHeight`）；
  2. `buildRows()` 在「下载中」header 之后插入该行，仅当 `activeCount > 0`，且受 `_collapsed[0]` 控制；
  3. 去掉 `Widget::_actionBar` 子控件与 `setScrollTopSkip`；
  4. `paint` / `rowAt` / `mouseMoveEvent` / `activateRow` / `updateProgress` 都要认识这种行。
- **推荐做法**（比把 `ActionBar` 改成纯自绘省事）：保留 `ActionBar` 现有的绘制与命中代码，
  新增一个 `paintIn(QPainter&, const QRect&)`，由 `InnerWidget::paint()` 按行矩形调用；
  命中判断复用 `row` 矩形 —— 这样 `ActionBar` 的 hover / 按钮状态逻辑基本不用动。

#### R4-4 自动备份：每加一个任务写一次盘，最多留 10 份

- 命名：`AutoBackup` + `YYYYMMDDHHMMSS`（本地时间，
  `QDateTime::currentDateTime().toString(u"yyyyMMddhhmmss")`），扩展名沿用导出用的 `.json`。
- 位置：程序缓存目录，用 `session->local().cachePath()`
  （`Account::cachePath()`，`storage/storage_account.h:125`）—— 它本身按账号分目录，多账号天然隔离。
- 内容：直接复用 `exportPausedJson()`（已含 `items` + 只读 `completed`）。
- 时机：**至少**在 `addLoading()` / `addLoadingExternal()` 里写（用户明确要求「每加一个就写盘」）。
  **待确认**：暂停 / 恢复 / 完成时写不写（目的是崩溃后能找回，建议写，但要防高频写盘）。
- 清理：写新文件后（或写之前）扫目录，按文件名排序，超过 10 份删最旧。
- 写盘放主线程同步即可（几 KB）；若担心抖动再换 `crl::async`。

#### R4-5 进度细线：两端圆角（胶囊）

- 两处：联系人列表下载条 `ui/controls/download_bar.cpp::paint()`（约 :180）；
  BigPage 行 `info/downloads/info_downloads_big_widget.cpp::paintItem()`（约 :973）。
- 数据现成：下载条用 `computeProgress()`（约 :289，`ready/total`）；BigPage 行用 `row.ready / row.total`。
- 形状：**两端半圆** → `p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2)`；
  底轨一条浅灰 + 进度一条主色。尺寸 / 颜色一律进 `.style` 文件，
  **禁止在代码里写死像素**（BigPage 加在 `info_downloads_big.style`）。

#### 建议的批次（CI 一轮 1.5-4h，尽量合并）

| 批次 | 内容 | 风险 |
|------|------|------|
| A | R4-1 + R4-2 + R4-5 | 低：读现成数据 + 画 UI。R4-1 与 R4-5 都要改 `download_bar`，同一批一起改 |
| B | R4-3 | 中：`Row` 结构改动，单独一轮好回退 |
| C | R4-4 | 中：写盘策略，先确认上面两处「待确认」 |

#### 交接给实现会话的开场（可直接复制）

> 读 `docs/DownloadsPageDev.md` 的「第三轮真机反馈」一节，以及 `.ai-memory/streaming-player-uaf.md`、
> `.ai-memory/ci-windows-build.md`，然后按 R4-1 → R4-2 → R4-5 → R4-3 → R4-4 的顺序做，每批一次 CI。

#### 播放器两项优化（O1 / O2）

| # | 反馈 | 处理 |
|---|------|------|
| O1 | 视频/图片播放界面下载要默认走「另存为」，让用户改文件名 | `OverlayWidget::downloadMedia()` 原来先判断 `askDownloadPath()` 设置、否则直接落到默认下载目录；现在只保留 `_photo/_document` 判空 + 一行 `saveAs()`（`saveAs()` 内部已处理复制限制、文档/图片/图片视频、以及 `addLoaded` 记录） |
| O2 | 播放界面点「消息信息」复制到剪贴板并提示「已复制」 | 该区域就是 caption（`Over::Caption` 枚举存在但一直没接）。新增 `OverlayWidget::ensureCaptionCopyLink()`（`LambdaClickHandler` + `crl::guard`），复制 `_caption.toTextForMimeData()` 并复用 `showSaveMsgToastWith(..., tr::lng_text_copied, 1000)`；`updateOver()` 的 caption 分支改为 `_stories ? ensureCaptionExpandLink() : ensureCaptionCopyLink()`（stories 保留原来的展开行为） |

### 第四轮真机反馈（2026-11-18）· 方案已定，待实现

本轮 3 项：① 同一条视频在别的消息里打开仍显示下载按钮；② 复制文本挂脚本转换（规则要跨项目通用 + 可更新）；③ 播放器「消息信息」常驻 + 底部媒体列表通栏。**规划/实现分开，本节是给实现会话的交接。**

#### F4-1 「已下载」状态：两套账本，键不同

**现象**：BigPage「已下载」里已有这条视频，但同一条视频在另一条消息里打开，查看器仍显示下载按钮；在原消息里打开不显示。

**两套判据**：

| 谁 | 判据 | 键 |
|----|------|----|
| 查看器 | `_documentMedia->loaded(true)`（`media/view/media_view_overlay_widget.cpp:1743`）→ `!_bytes.isEmpty() \|\| !_owner->filepath(true).isEmpty()`（`data/data_document_media.cpp:380`） | `DocumentData` 的 `_location` / 该 view 的 `_bytes` |
| BigPage | `DownloadedId`（`data/data_download_manager.cpp:637`）+ `_loaded`（`:652`） | `DownloadId{ document->id, Document }` / `HistoryItem*` |

`filepath(true)` 在 `_location.name().isEmpty()` 时直接返回空（`data/data_document.cpp:1496`），所以 `_location` 是硬门槛。

**谁会写 `_location`**：只有 `DocumentData::save()`（`data/data_document.cpp:1245`）和 `finishLoad()`（`:1062`）。
**谁不会**：`DownloadManager::addLoaded()`（`data/data_download_manager.cpp:623-682`）和查看器的 `saveAs()`（`media/view/media_view_overlay_widget.cpp:3344-3350`；AyuGram 已把查看器下载强制走 `saveAs()`，见 `:3465-3472`）。

**结论（用户已真机验证）**：`DocumentData` 按 `id + accessHash + dc` 去重 —— **转发**的同一条视频是同一个对象，`_location` 全消息共享，已下载状态天然一致（用户实测如此）。用户遇到的是**重发**：`id` 不同、内容相同，两套账本都没有内容级关联，所以查看器不认。

**决定（两步都做）**：

1. **补账**：`DownloadManager::addLoaded()` 里对 `object.document` 调 `document->setLocation(Core::FileLocation(path))` + `session().local().writeFileLocation(mediaKey(), ...)`（照抄 `data/data_document.cpp:1533-1535` 的写法）；`saveAs()` 同样补一次。这样「查看器里另存过」的文档在其它消息里也认。
2. **跨文档认内容（内容键）**：给 `DownloadedId` 加一个跟 `document id` 无关的指纹。**用户定的口径：大小（精确字节）+ 时长 ‖ 原始文件名** —— 大小和时长都相同基本就是同一个文件。
   - `DownloadedId` 已有 `size`（`data/data_download_manager.h:63`），再追加两个字段（加在结构末尾，`:57`）：
     - `int64 duration = -1`（毫秒，`-1` = 未知；取 `document->hasDuration() ? document->duration() : -1`，`data/data_document.h:211` / `:1937`）
     - `QString sourceFilename`（取 `document->filename()`，`data/data_document.h:297`）
   - **匹配规则**：`size` 必须相同，且满足任一条 ——
     - 双方 `duration >= 0` 且相等；或
     - 双方 `sourceFilename` 非空且相等。
     - 「未知」的那一项（`duration < 0` / 文件名为空）**不参与**，避免空值互相误撞。
   - 新增 `DownloadManager::isDownloaded(const DocumentData*)`：先按 `document->size` 过滤，再比上面两条。列表量级小，先线性扫，真机嫌慢再建索引。
   - **三个挂载点，缺一个都会漏**：
     - `OverlayWidget::updateControls()` 的卡片下载判定（`media/view/media_view_overlay_widget.cpp:1743`）→ `loaded(true) || isDownloaded(_document)`；
     - `OverlayWidget::computeSaveButtonVisible()` 的文档分支（`:1669`，现在是 `_document->filepath(true).isEmpty()`）→ 追加 `&& !isDownloaded(_document)`；
     - BigPage「已下载」列表（可选）也按 `isDownloaded` 认重发副本。
   - **图片不涉及**：查看器里图片没有这个下载按钮 —— 卡片分支要求 `_document`（`:1716`），`computeSaveButtonVisible()` 的照片分支是 `_photo->hasVideo()` 语义（`:1667`）。
   - 不哈希：未下载的文档取不到内容，哈希只能覆盖「已下载过的那一侧」，且要读整个文件。
   - 序列化：`DownloadedId` 走 `QDataStream`（`deserialize()` 在 `data/data_download_manager.cpp:1744`，字段顺序 `:1787-1794`），两个新字段**追加在 `path` 之后** + `!stream.atEnd()` 守卫，否则旧版本存的数据会错位（见 AGENTS.md）。
   - 填写点：`addLoaded()`（`:623`）从 `object.document` 取；`resolveRequestsFinished()`（`:1413`，导入列表里消息还在的条目）从真实 `document` 取；`generateEntry()`（`:1441`，消息已删 / 磁盘恢复的假条目）`duration = -1`、`sourceFilename = QFileInfo(path).fileName()`（就是保存时的文件名，可能已被用户改过，只强不弱）。

#### F4-2 复制文本转换：规则外置成 JSON

**两个挂载点**（用户明确只要这两个）：

| 场景 | 位置 | 现状 |
|------|------|------|
| 播放器点「消息信息」 | `OverlayWidget::ensureCaptionCopyLink()`（`media/view/media_view_overlay_widget.cpp:8347-8363`） | 复制 `_caption.toTextForMimeData().expanded`（已处理 `\r` / `\n`） |
| 消息列表复制 | `HistoryInner::copySelectedText()`（`history/history_inner_widget.cpp:4073-4085`）、`copyContextText()`（`:4179-4192`） | 走 `Iv::SetRichBlocksClipboard(text, richBlocks, session)` |

- AyuGram 存档列表那两处（`ayu/ui/message_history/history_inner.cpp:1401-1403`、`:1430-1434`）是 `TextUtilities::SetClipboardText` 直拷，同一条路径，可顺带。
- ⚠️ **格式会丢**：`copySelectedText` 的 rich 块偏移是按原文算的，改了文本就错位。**用户已确认接受**：这个复制功能就是拿来给文件重命名的，不需要保留富文本。决定：**转换走纯文本**（`TextUtilities::SetClipboardText(TextForMimeData::Simple(text))`）。

**规则外置（跨项目通用 + 可更新）**：

- ❌ C++ 里跑 JS：QJSEngine 依赖 qtdeclarative，CI 不构建（`cmake/external/qt/package.cmake:78-79` 只在 `OPTIONAL_COMPONENTS`）。
- ❌ 构建期把 `GTRule.js` 生成成头文件：运行时不能更新。
- ✅ **决定**：规则存 JSON，新增 `Ayu::ClipboardRules`（`ayu/utils/clipboard_rules.h/.cpp`）。
  - 路径：portable / 可执行目录下的 `ayu_clipboard_rules.json`；缺失时用内置默认（等价当前 `GTRule.js`）。
  - 更新：`QFileSystemWatcher` 监听该文件，变化即重载 —— **不是轮询**。
  - 跨项目：坚果云里 `GTRule.js` 仍是源，另存一份同内容 JSON（其它项目读同一份 JSON）；实现会话先写一次性转换脚本，之后手工同步。
  - 加载失败 / JSON 非法：保留旧规则，不弹错误框。

**转换目标 = 「文本 → 文件名」**（用户已确认：复制就是为了重命名文件）。所以复制钩子**不套 `handleName()`** —— `handleName()` 假设输入已经是 `[标签]名字.后缀`（`GT重命名.js:72-102`），而消息正文没有这个结构，套上去只会把正文切碎。决定：

- 新增 `Ayu::ClipboardRules::toFilename(const QString &text)`：繁简转换 → 替换规则 → 去表情 → 去链接 → 去 `@人名` → 压掉换行/多余空白 → 删 Windows 非法字符 `\ / : * ? " < > |` → 限长（默认 80 字符；`#标签` 默认保留，可关）。输出**不含扩展名**（扩展名跟着下载的文件走）。
- `handleName()` 仍按原计划移植成 `toCleanName()`，只服务「复制文件名」场景（`AddCopyFilename`，`history/view/history_view_context_menu.cpp:2699`；注意 `:2707-2712` 现在显式排除了视频，放宽需单独确认）。
- 两者共用原子步骤：`removeEmoji` / `replaceWord` / `takeType` / `simplized`。
- 规则（启用哪些步骤、限长、是否保留 `#标签`）全部来自那份 JSON。

#### F4-3 播放器两项优化

**(1) 「消息信息」在所有媒体上都显示**

- 「消息信息」= **caption**（本文件 `:1400` 的 O2 已定案）。
- 根因：`refreshCaption()`（`media/view/media_view_overlay_widget.cpp:4106`）在 `_message` 带 instant-view 媒体 / webpage / poll 时返回**按 item 的文本**（`:4112-4143`），且 `caption.text.isEmpty()` 时**直接 return**（`:4145-4151`），不回退到 `_message->translatedText()`。所以「只有第一个 item 带 caption」时就表现为“只在第一个文件显示”。
- 决定：把 `:4145` 的提前 return 改为**先回退** —— per-item 文本为空时用 `_message->translatedText()`，仍为空才走原来的 `_streamed` 时间戳清理 + return。
- **截图已确认就是 caption**：截图里底部居中的深色卡片（「异域风情巨根美男 / Xanzo精选 / 第一辑 / #帅哥 …」）就是 `_caption`，右侧带引号图标。按上面的回退改即可。

**(2) 底部媒体列表改通栏居中**

- 根因：`_groupThumbsAvailableWidth` 不是按窗口宽度算，而是 `width() - 2 * bottomSkip`（`:1908-1912`），`bottomSkip` = max(`_dateNav` 右缘, `_headerNav` 右缘) + `mediaviewCaptionMargin`（`:1903-1907`）—— **底栏那行字（发送者名 + 日期）/ 顶栏文件名有多宽，缩略图区就被压多窄**。
- `ComputeThumbsLimit()`（`media/view/media_view_group_thumbs.cpp:220-230`）：`limit = (availableWidth - currentWidth - 2 * singleWidth) / (2 * singleWidth)`。`availableWidth ≈ 300` → 1 张，`≈ 450` → 2 张，与「只能显示 1~2 张」吻合。
- 决定：**把两个宽度拆开**。
  - 新增 `_captionAvailableWidth`，沿用现在 `bottomSkip` 的算法；`refreshCaptionGeometry()` 的 `captionWidth`（`:1981-1984`）改用它，caption 宽度不变。
  - `_groupThumbsAvailableWidth` 改为 `width() - 2 * <新边距>`，边距加在 `media/view/media_view.style`（**禁止硬编码像素**）；`_groupThumbsLeft` 的居中公式（`:1913`）不变。
  - 效果：以 1000px 窗口为例，缩略图从 1~2 张变成约 5 张，仍整体居中。
- 连带检查（都跟着 `_groupThumbsLeft` 走，**不用改**）：`_groupThumbsRect`（`:4331-4335`）、`paintGroupThumbs`（`:6397-6404`）、`getState`（`:8250`）、`refreshGroupThumbs`（`:4256-4295`）。

#### 用户已确认（2026-11-18）

1. F4-1：现象是**重发**（转发天然共享，已实测）；内容键用「大小 + 文件名」，不哈希。
2. F4-2：**接受丢失富文本格式** —— 复制就是为了重命名文件，转换目标是「文本 → 文件名」。
3. F4-3(1)：截图确认是 **caption**。

### 第五轮真机反馈（2026-11-19，run 84 包）· 已定位并修复

run 84 = `679ac2b`（CI success）。用户真机测出 4 点，逐条拿代码定位如下。
第 1 点由 run 85（`90f60f2`）修复，其余三点在本轮修复。

#### F5-1 caption 只在第一个媒体显示

- 现象：相册里只有第一个媒体显示「消息信息」，其余没有。
- 根因：`OverlayWidget::refreshCaption()` 只取 `_message` 自己的
  `InstantViewMediaCaption` 和 `_message->translatedText()`，两者都空就提前 return。
  相册正文只挂在其中一个 part 上，看第二个 part 时两者都空。
- 修法（run 85 已提交，`media/view/media_view_overlay_widget.cpp:4155-4176`）：
  把提前 return 改成三级回退 —— `_message->translatedText()` → 同组里第一个
  `!emptyText()` 的 part 的 `translatedText()` → `_message` 自身
  `HistoryMessageMediaForInstantView::captions` 里第一个非空 caption。

#### F5-2 复制没有按原始 js 提取标签

- 现象：复制出来的文件名没有 `[标签]` 前缀。
- 根因：`Ayu::ClipboardRules::ToFilename()` 只跑清洗步骤，**从不调用 `TakeType()`**
  （只有 `ToCleanName()` 调）。而 `takeType` 是 js 里唯一补标签的地方
  （`GT重命名.js:30-47`），所以标签必然丢失。
- 修法（`ayu/utils/clipboard_rules.cpp:348-370`）：把原步骤循环抽成匿名命名空间
  函数 `ApplySteps()`；`ToFilename()` 改为 解析 `[...]` 前缀标签 → `ApplySteps` 清洗
  → `TakeType(tags, result)` → 有标签就拼回 `[标签]名字`，与 `ToCleanName()` 同构。
- 连带：清洗步骤会把「只有链接 / 只有表情 / 只有 @人名」的正文清成空串，空剪贴板
  看起来就是「什么都没复制」，所以加一条兜底 —— 结果为空时退回
  `CollapseWhitespace(RemoveIllegalChars(text))`。

#### F5-3 消息列表「复制文本」没复制出文本

- 现象：右键「复制文本」后剪贴板是空的。
- 根因：`HistoryInner::copyContextText()` 对**相册**优先用 `HistoryGroupText(group)`，
  而该函数（`history/history_item_text.cpp:471-514`）在「组里没有任何一条带 caption」
  时返回**空** `TextForMimeData`（`hasCaption` 全 false → 末尾 `first == end` 直接
  `return TextForMimeData()`），`ToFilename("")` → 空剪贴板。
- 另一条路径：`ListWidget::copySelectedText()` 走
  `Iv::SetRichBlocksClipboard(text, getSelectedRichBlocks(), session)`，其中
  `getSelectedRichBlocks()` 取的是 instant-view 的 rich page 块
  （`history/history_item_text.cpp:464-469`），普通消息为空 → 走纯文本兜底，
  复制的是**未转换的原文**，等于没挂上 F4-2。
- 修法：
  - `history/history_inner_widget.cpp:4180-4195`：group text 为空时回退
    `HistoryItemText(item).expanded`；
  - `history/view/history_view_context_menu.cpp:1918-1938`：同样回退，并统一走
    `ToFilename` + `TextForMimeData::Simple`，不再走 rich blocks；
  - `history/view/history_view_list_widget.cpp:3582-3590`：删掉
    `getSelectedRichBlocks()`（定义 + `history_view_list_widget.h` 声明，已无调用者），
    `copySelectedText()` 改为转换后纯文本复制。
- ⚠️ 用户口径：复制就是为了给文件重命名，**接受丢失富文本格式**（见 F4-2）。

#### F5-4 已下载媒体没有背景色标识

- 现象：聊天里已下载的视频，气泡没有换背景色。
- 根因：`Message::draw()` 的 `downloadedVideo` 判据只有
  `Core::App().downloadManager().isDownloaded(doc)`，而 `isDownloaded()`
  （`data/data_download_manager.cpp:1247-1269`）只遍历 `_sessions[...].downloaded`，
  也就是**本版本下载系统记录过**的文件。用户此前已经存到磁盘的视频没有这条记录，
  判据不成立。
- 修法（`history/view/history_view_message.cpp:1943-1949`）：追加本地文件判据 ——
  `!doc->filepath().isEmpty() || isDownloaded(doc)`。
  `DocumentData::filepath()`（`data/data_document.cpp:1496-1500`）返回 `_location`，
  而 `_location` 在 `setRemoteLocation()`（`:1983-2004`）构造时就会从本地存储
  `readFileLocation(mediaKey())` 读回来，所以「以前保存过的文件」也能认。
- 保持用户已确认的口径：只标识**视频**，**整个气泡**换 `st::msgFile2Bg` 底色
  （`:1958`），图片不标识。

### 第六轮真机反馈（2026-11-19，run 86 包）· 已定位并修复

run 86 = `2c1b23f`（CI success）。用户确认 F5-1、F5-2 通过，F5-3、F5-4 无效果，
另提两条新需求。逐条拿代码定位如下。

#### F6-1 单击消息正文没有触发复制（F5-3 的残留）

- 现象：聊天窗口单击消息正文的文字没有复制；查看器 caption 上单击可以。
- 根因：**功能缺失，不是触发区域失效**。查看器 caption 有专门的点击处理器
  `OverlayWidget::ensureCaptionCopyLink()`（`media/view/media_view_overlay_widget.cpp:8384-8399`），
  由 `updateOver()` 在 `_captionRect.contains(pos)` 且 `textState.link` 为空时挂上；
  而聊天消息正文**没有任何点击处理器** —— `Message::getStateText()`
  （`history/view/history_view_message.cpp:4782`）的普通文本分支只返回
  `TextState(item, text().getState(...))`，`link` 为空就是空操作（Telegram 里
  单击正文本就是这个行为）。
- 修法：
  - 新增 `MessageTextCopyClickHandler`（`history_view_message.cpp:364-382`），
    左键点击调用 `Message::copyTextToClipboard()`；
  - `Message::getStateText()` 普通文本分支在 `link` 为空时挂上它，并把
    `cursor` 置 `None`（`:4981-4986`），真实链接不受影响；
  - `Message::allowTextSelectionByHandler()` 放行该 handler（`:5906-5908`），
    否则 `HistoryInner::mouseActionStart()` 会因 `pressed` 直接进 `PrepareDrag`，
    拖选正文就没了。
- 文本获取统一走 `Ayu::ClipboardRules::CopyItemTextToClipboard()`
  （`ayu/utils/clipboard_rules.cpp:390-406`）；`HistoryInner::copyContextText()`
  和右键菜单「复制文本」改为调用它，不再各写一份。

#### F6-2 已下载视频背景色仍不生效（F5-4 的残留）

- 现象：新下载和历史落盘两种都测过，视频气泡背景和别的消息一致；BigPage 显示「已完成」。
- 排查：渲染链路逐段验证是通的 —— `Message::draw()` → `Ui::SimpleBubble::solidBg`
  （`history_view_message.cpp:2003`）→ `PaintBubble`（`ui/chat/message_bubble.cpp:363-371`）
  → `PaintSolidBubble`（`:286-312`）→ `fillRect(rect, st::msgFile2Bg)`，
  `msgFile2Bg = #5fbe67`（`lib_ui/ui/colors.palette:398`）。所以断点在**判据**。
- 判据的两个薄弱点：
  1. `DownloadManager::isDownloaded()`（`data/data_download_manager.cpp:1247-1269`）
     按 `size` +（`duration` 或 `sourceFilename`）做内容匹配；视频没有 filename，
     只要 `size` 或 `duration` 对不上就漏。
  2. 视图层 `media->getDocument()` 依赖具体媒体视图；相册是
     `GroupedMedia::getDocument()`（`history/view/media/history_view_media_grouped.cpp:918-920`），
     返回的是**主 part** 的文档 —— 下载的是组内非主 part 就判不到。
- 修法（`history/view/history_view_message.cpp:1967-1995`）：改用**数据层**判据：
  - `entry->media()->document()` 直接取文档，绕开视图层；
  - 新增 `DownloadManager::isDownloadedItem(FullMsgId)`
    （`data/data_download_manager.cpp:1271-1284`），按持久化的
    `DownloadedId::itemId` 精确匹配（`deserialize()` 会读回
    `itemIdPeer` / `itemIdMsg`，历史记录同样有效）；
  - 判据扩为 `!filepath().isEmpty() || isDownloaded(doc) || isDownloadedItem(...)`；
  - **相册整组**：组内任一 part 命中即整条变色（`Data::Groups::find()` 遍历
    `group->items`）。
- 口径确认：历史下载、后续新下载都认；一条消息内任一媒体已下载 → 整条变色。

#### F6-3 新需求：复制前去掉 `#话题` 并折叠空格

- 口径：**先提取标签，再去 `#xx`**。`typeRules` 的关键词本身可能带 `#`
  （如 `#fist`），顺序反了就提取不到。
- 修法（`ayu/utils/clipboard_rules.cpp`）：
  - `ApplySteps()` 移除 `removeHashtags` 分支和末尾兜底，hashtag 不再被提前清掉；
  - `ToFilename()` / `ToCleanName()` 改为在 `TakeType()` **之后**再
    `CollapseWhitespace(RemoveHashtags(result))`（`:378-384`、`:432-434`）；
  - `keepHashtags` 默认值改为 `false`（`clipboard_rules_data.h` 与 `Parse()`）。

#### F6-4 新需求：规则数据外置

- 口径：`typeRules` / `replaceRule` / 繁简字表（js 的 `charPYStr` / `ftPYStr`）
  不应打进 exe，用户改完不用重新出包。
- 方案（用户选 A）：exe 内保留一份默认数据作「种子」，首次运行把
  `ayu_clipboard_rules.json` 写到 exe 目录（`EnsureFile()`，
  `ayu/utils/clipboard_rules.cpp:66-77`），之后读该文件并由
  `QFileSystemWatcher` 热重载（`StartWatcher()` 已有）。用户改 exe 旁边的
  json 即时生效，无需重新打包。

### Phase 3：集成测试 ⏳ 待开发

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T3.1 | CMake 配置 + Debug build | — | 1-3h（含环境配置） |
| T3.2 | 运行期测试：下载 → 退出 → 启动 → 续传 | — | 2h |
| T3.3 | 运行期测试：暂停/继续、断网恢复 | — | 1h |
| T3.4 | 运行期测试：来源消息删除后 UI 是否健壮 | — | 0.5h |
| T3.5 | 运行期测试：多 Account 场景 | — | 0.5h |

---

## 七、风险点 & 已知限制

1. **文件后缀 `.download`**：需要确认其他 code path（如自动加载已有文件）不会把 `.download` 文件误认为损坏文件。

2. **WebFile 下载**：现有的 `webFileLoader` 同样通过 FileLoader base，需要保持兼容。暂停/续传逻辑需要适配 Web 下载。

3. **AudioAlbumThumb** 等特殊位置：如果不在 StorageFileLocation 分支，暂停续传可能不生效。需要测试。

4. **Web 环境 cache resume**：已有 `startLoadingWithPartial()` 用于缓存恢复，新方案不应破坏该路径。

5. **竞态条件**：pause 期间如果 DocumentData 正在触发 `documentLoadDone`，需要保证不 double-fire。考虑在 pause() 时先 `base::take(_loader)` 再 pause。

6. **内存占用**：大文件（比如 4GB 视频）的部分文件可能非常大，退出时的 save 不应触发 reload。仅序列化小的元数据结构即可。

7. **清理问题**：`.download` 临时文件可能在多种场景下残留。需要：完成后立即 rename；过期未完成文件的清理可由用户主动管理。

8. **D-Bus /「在文件夹中打开」Platform 适配**：已有的 `Core::FileLocation` 和 `Platform::File::ShowInFolder(path)` 应能直接使用。

---

## 八、关键文件变更清单

### Phase 1 + Phase 2-A 已修改文件（当前 HEAD）

| 文件 | 变更类型 |
|------|---------|
| `Telegram/SourceFiles/data/data_download_manager.h` | 新增 PausedDownloadDescriptor / PauseReason；新增 pauseAll/pauseOne/resumeOne/cancelPaused/loadingStopSilent/loadingCount/pausedCount/pausedList/pausedListChanges 接口；private: resumePaused / requestPausedResume |
| `Telegram/SourceFiles/data/data_download_manager.cpp` | 完整实现上述接口 + serializePaused/deserializePaused + validatePausedDownloads；消息不在内存时按 id 补拉（`messages.GetMessages` / `channels.GetMessages`） |
| `Telegram/SourceFiles/data/data_document.h` | 新增 pauseLoad/resumeLoad/paused；DownloadPaused 标志 |
| `Telegram/SourceFiles/data/data_document.cpp` | 完整实现上述接口 |
| `Telegram/SourceFiles/storage/file_download.h` | 新增 pause/resume/setStartOffset；构造函数加 startOffset 参数 |
| `Telegram/SourceFiles/storage/file_download.cpp` | pause/resume/checkForOpen Appender/temp 命名/finalizeResult rename |
| `Telegram/SourceFiles/storage/file_download_mtproto.h` | 构造函数加 startOffset；_startOffset 成员 |
| `Telegram/SourceFiles/storage/file_download_mtproto.cpp` | 4 个构造函数初始化 _nextRequestOffset(startOffset) |
| `Telegram/SourceFiles/storage/storage_account.h` | 新增 updatePausedDownloads/pausedDownloadsSerialized |
| `Telegram/SourceFiles/storage/storage_account.cpp` | 序列化写/读追加 _pausedDownloadsSerialized |
| `Telegram/SourceFiles/core/application.cpp` | downloadPreventsExit 不再阻塞退出 |
| `Telegram/SourceFiles/info/info_controller.h` | Section::Type::DownloadsBig 枚举；Key 构造函数 + isDownloadsBig |
| `Telegram/SourceFiles/info/info_controller.cpp` | Key 枚举构造函数实现 |
| `Telegram/SourceFiles/info/info_content_widget.h` | ContentMemento 加 DownloadsBig::Tag 构造 + isDownloadsBig |
| `Telegram/SourceFiles/info/info_content_widget.cpp` | key() 返回 DownloadsBig::Tag |
| `Telegram/SourceFiles/info/info_wrap_widget.cpp` | 3 处守卫 |
| `Telegram/CMakeLists.txt` | 添加 BigPage 文件 |

### Phase 1 + Phase 2-A 新增文件

| 文件 | 说明 |
|------|------|
| `Telegram/SourceFiles/info/downloads/info_downloads_big_widget.h` | BigPage Memento + Widget + Make 工厂 |
| `Telegram/SourceFiles/info/downloads/info_downloads_big_widget.cpp` | BigPage stub 实现 |

### Phase 2-B + Phase 2-C.1–3 修改文件（工作区未提交）

| 文件 | 变更类型 |
|------|---------|
| `Telegram/SourceFiles/info/downloads/info_downloads_big_widget.h` | 去掉错误的前置声明（`UserData` 是全局作用域）、未使用的 `InnerWidget` 成员；补 `Ui::RpWidget` 前置声明 |
| `Telegram/SourceFiles/info/downloads/info_downloads_big_widget.cpp` | 全量重写：`Row` 行模型 + `InnerWidget` 自绘列表 + 三组分组 + 进度条 + 悬停/点击 |
| `Telegram/cmake/td_ui.cmake` | `style_files` 注册 `info/downloads/info_downloads_big.style` |
| `Telegram/Resources/langs/lang.strings` | 新增 6 个 `lng_downloads_big_*` key（空态/总计/三组标题/进度文案）+ 9 个顶栏菜单/导出导入/续传失败 key |
| `Telegram/SourceFiles/window/window.style` | 新增 `windowFiltersDownloads` 与 `windowFiltersDownloadsTextOnly`（两套 tabs 模式下的尺寸/角标位置） |
| `Telegram/SourceFiles/window/window_filters_menu.h` | 新增 `setupDownloadsButton()` / `updateDownloadsBadge()` / `_downloads` 成员 |
| `Telegram/SourceFiles/window/window_filters_menu.cpp` | 创建侧栏下载按钮、角标订阅、跳转 BigPage |
| `Telegram/SourceFiles/info/info_wrap_widget.cpp` | 删除 `setupTopBarWidgets` 里已失效的 `DownloadsBig` 死分支；给 `Section::Type::DownloadsBig` 加 `addTopBarMenuButton()`（顶栏 ⋮ 菜单） |
| `Telegram/SourceFiles/info/info_layer_widget.cpp` | `parentResized()` 里按 `Info::Key::isDownloadsBig()` 选用 `st::downloadsBigLayerWidth`（默认仍是 `infoDesiredWidth`） |
| `Telegram/SourceFiles/data/data_download_manager.h/.cpp` | `exportPausedJson()` / `importPausedJson()` + `flushPaused()`；`pauseAll()` 结尾改 `flushPaused()`；`descriptor.displayName/sourceTitle` 填充 |
| `Telegram/SourceFiles/storage/storage_account.h/.cpp` | `writeLocationsForce()`：取消 1s 延迟定时器并立即落盘 |
| `Telegram/SourceFiles/storage/file_download.cpp` | `checkForOpen()` 续传分支改 `ReadWrite`；`currentOffset()` 在 `_paused` 时返回 `_pausedBytes` |

### Phase 2-B + Phase 2-C 新增文件

| 文件 | 说明 |
|------|------|
| `Telegram/SourceFiles/info/downloads/info_downloads_big.style` | BigPage 全部尺寸/字体/颜色（取代「复用 style_info」的预估方案） |

> 注意：style 生成器用 `NAME_WLE` 命名头文件（路径被剃掉），所以 `info/downloads/info_downloads_big.style`
> 生成的头文件是 `styles/style_info_downloads_big.h`，不是 `style_downloads_big.h`。

### 顺带修掉的四个续传正确性 bug

这四个都是实测前 review 出来的，属于 Phase 1 遗留，不在原任务清单里：

| 问题 | 后果 | 修法 |
|------|------|------|
| `FileLoader::currentOffset()` 在 `pause()` 之后返回 0 | `descriptor.ready` 被记成 0，续传从头开始 | `_paused` 时返回 `_pausedBytes`；关键是 `pause()` 必须在 `_paused = true` **之前**取偏移，否则读到的还是旧值 0 |
| `DownloadManager::resumeOne()` 只删了暂停记录 | 「继续」是个空操作，界面点了没反应 | 定位源消息/文档 → `document->resumeLoad(ready)` + `document->save(origin, path)` + `addLoading()` |
| `validatePausedDownloads()` 把 `d.path` 覆盖成 `path + ".temp"` | 续传会把数据写到 `file.ext.temp`，最终 rename 目标错 | 保留最终路径，只按 `.temp` 的实际 size 修正 `ready` |
| `FileLoader::checkForOpen()` 续传分支用了 `WriteOnly` | `QFile::open(WriteOnly)` 截断已有的 `.temp`，偏移永远归零、续传退化为重新下载 | 改用 `ReadWrite` + 显式 `seek(_startOffset)` |

另外 `FileLoader::checkForOpen()` 在 `_startOffset > 0` 时原本用 `WriteOnly | Append`，但 `writeResultPart()`
是按绝对偏移 `seek(offset)` 后写入的，Append 会忽略 seek 造成数据错位；已改成 `ReadWrite` + 显式
`seek(_startOffset)`，并夹紧到文件实际长度。这里必须用 `ReadWrite` 而不是 `WriteOnly`：
`QFile::open(WriteOnly)` 会把已有的 `.temp` 截断成 0 字节，`ready` 就永远归零、续传退化成重新下载。
另外 `checkForOpen()` 末尾的兜底分支（打不开 `.temp` 时回退到最终文件名 `WriteOnly`）在 `_startOffset > 0`
时同样会截断最终文件、留下零填充空洞，已加 `_startOffset == 0` 条件，续传场景直接 `cancel(FileWriteFailure)`。
`DocumentData::save()` 把续传偏移通过 `_pendingResumeOffset`
→ `base::take()` → `mtpFileLoader(..., startOffset)` 一路透传，`save()` 结束时清零。

### 退出不丢任务的持久化保证 + 下载列表导出/导入

链路：`Application::preventsQuit()` → `downloadPreventsQuit()` → `loadingStopSilent()` → `pauseAll()`。

| 环节 | 原始行为 | 问题 | 现在 |
|------|---------|------|------|
| `pauseAll()` 结尾 | `writePostponed(nullptr)` | 只把暂停列表塞进 `Account` 的回调，真正落盘走 `writeLocationsDelayed()` 的 1s 定时器 | 改成 `flushPaused()`：`writePostponed` + `Account::writeLocationsForce()` 立即同步写盘 |
| `~Account()` | 只 flush `writePrefs()` / `writeMap()` | `_locationsChanged` 无人处理，1s 内退出就丢暂停列表 | 不再依赖析构；`writeLocationsForce()` 取消定时器并直接 `writeLocations()` |

`writeLocations()` 内部本来就会调用 `_pausedDownloadsSerialize()`，所以「强制写 locations」
天然覆盖暂停列表，不需要额外的存储 key。

**下载中列表的独立快照（崩溃恢复）**：locations 文件尾部依次追加三个 blob
`downloadsSerialized` → `pausedDownloadsSerialized` → `inflightDownloadsSerialized`，
新字段一律追加在末尾，读取用 `!stream.atEnd()` 逐级守卫（见 AGENTS.md 的序列化顺序约束）。

- 写入：`DownloadManager::writeInflightPostponed()` 在 `addLoading` / `addLoadingExternal` /
  `updateLoadingExternal` / `remove()` / `check()`（有进度变化时）为每个 session 注册
  `_inflightDownloadsSerialize` 回调，复用 `writeLocationsDelayed()` 的 1s 去抖。内容是当时
  所有未 `done`、非 admin-log 的 `DownloadingId` 转成 `PausedDownloadDescriptor`
  （`PauseReason::Auto`）。
- 恢复：`trackSession()` 在 `validatePausedDownloads()` 之前调用 `deserializeInflight()`，
  把条目**并入 `_paused`**（不是自动续传）——崩溃前任务来不及暂停，恢复成暂停状态最安全，
  也与优雅退出链路（同样落在 `_paused`）收敛到同一形态。多账号时只在
  `_sessions.size() == 1` 时恢复一次，避免全局快照被重复追加。
- 快照里的 `ready` 走 `validatePausedDownloads()` 的既有夹紧：`.temp` 偏小则下调，
  `.temp` 不存在则归零。

**导出 / 导入**（顶栏 ⋮ 菜单，`Widget::exportPausedList()` / `importPausedList()`）：

- 导出：`DownloadManager::exportPausedJson()` → `FileDialog::GetWritePath`，默认名 `downloads.json`，
  成功后 `Ui::Toast` 提示条数；列表为空时提示「没有可导出的内容」。
- 导入：`FileDialog::GetOpenPath` → 读文件 → `DownloadManager::importPausedJson()`，
  校验 `format == "downloads"` 与 `version == 1`，按 `documentId + photoId + itemIdPeer + itemIdMsg`
  去重后合并，调用 `validatePausedDownloads()` 修正 `ready`（按 `.temp` 实际大小夹紧），
  再 `_pausedListChanges.fire({})` + `flushPaused()`。
- 导入**只接受 Document 条目**：photo 无法续传，且目前没有移除暂停行的入口，
  导入一个 photo 行会留下永远删不掉的条目，所以直接跳过（导出本身不会产生 photo 条目）。
- 全部条目重复时返回 0，UI 提示「没有新增下载」（`lng_downloads_import_empty`），
  不再复用「Imported 0 downloads」。
- 格式（`format: "downloads"` 作为校验标记，64 位 id 与 `ready/total/started` 用字符串避免 JSON double 精度丢失；
  导入时 `documentId` 等字段与 `downloadType/reason` 都同时兼容字符串和数字两种形式）：

```json
{
  "app": "AyuGram",
  "format": "downloads",
  "version": 1,
  "exportedAt": 1762953600,
  "items": [
    {
      "documentId": "5412375...",
      "photoId": "0",
      "downloadType": 0,
      "itemIdPeer": "-1001234567890",
      "itemIdMsg": "4242",
      "peerAccessHash": "-6123456789...",
      "ready": "8388608",
      "total": "104857600",
      "started": "1762950000",
      "reason": 1,
      "path": "C:/Users/x/Downloads/movie.mp4",
      "displayName": "movie.mp4",
      "sourceTitle": "Some Channel"
    }
  ]
}
```

**源消息不在内存时按 id 补拉**：`resumeOne()` 先按 `FullMsgId` 在 `_sessions` 里找消息；找不到时
（典型场景：重启后那个会话还没加载，`Session::message()` 只查内存 `_messages`，没有数据库回退）
不再直接失败，而是 `requestPausedResume()` 用 `messages.GetMessages` / `channels.GetMessages`
（`inputMessageID`）把这条消息拉回来，`processExistingMessages()` 入内存后再走同一条
`resumePaused()`。dc / accessHash / size / fileReference 都来自这条新消息，所以顺带避开了
旧 fileReference 过期的问题。拉取失败、或消息里没有匹配的 document 时保留暂停记录（可再点一次重试），
并写 `DEBUG_LOG`（前缀 `[DL]`）。如果文档已经下载完成或正在下载（暂停记录已过期），
`resumePaused()` 会直接把该记录从列表里移除，不再尝试续传。列表本身（行、角标、导出内容）不依赖消息，始终可用。

**续传失败时的 `ready` 同步**：若 `.temp` 不存在或比记录的 `ready` 短，`resumePaused()` 会拒绝续传并把
`ready` 夹到 `.temp` 的实际大小（同时 `_pausedListChanges.fire({})` + `writePostponed(nullptr)`），
界面上的进度条会立刻反映磁盘真实情况，不会一直显示过期进度。

**旧版本数据的路径兼容**：在 `validatePausedDownloads` 改为保留最终路径之前，磁盘上的暂停记录里
`path` 存的是 `<name>.temp`。`deserializePaused()` 在读取后做了一次修正：如果 `path` 以 `.temp` 结尾、
`ready > 0`、`path` 本身存在而 `path + ".temp"` 不存在，则剥掉 `.temp` 后缀还原成最终路径。
不做这层修正的话，续传会把文件写到 `<name>.temp`，真正的 `<name>` 永远不会出现。

---

## 九、重要文件路径速查

```
D:\Work\AyuGramDesktop\Telegram\SourceFiles\data\data_download_manager.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\data\data_download_manager.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\data\data_document.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\data\data_document.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\file_download.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\file_download.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\file_download_mtproto.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\file_download_mtproto.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\storage_account.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\storage\storage_account.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\core\application.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\window\window_filters_menu.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\window\window_filters_menu.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\info_controller.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\info_wrap_widget.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\downloads\info_downloads_widget.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\downloads\info_downloads_widget.cpp
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\downloads\info_downloads_provider.h
D:\Work\AyuGramDesktop\Telegram\SourceFiles\info\downloads\info_downloads_provider.cpp
```

---

## 十、开发阶段划分

### 阶段总体视图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Phase 1: 核心下载续传（可独立测试）                                          │
│                                                                              │
│  目标：下载任务持久化 + 断点续传 + 退出不丢失                                  │
│  产出：可独立验证的核心能力，无 UI 变化                                       │
│  测试：下载中退出 → 重启后能续传                                              │
├──────────────────────────────────────────────────────────────────────────────┤
│ Phase 2: UI + 交互优化                                                       │
│                                                                              │
│  目标：BigPage + 侧边栏入口 + 统计/过滤/操作菜单                              │
│  产出：完整用户体验闭环                                                      │
│  测试：所有 UI 交互走通                                                      │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

### Phase 1：核心下载续传

#### 1.1 阶段目标

**可独立验证的能力闭环：**

```
下载中 ──→ 退出 APP ──→ 重启 APP ──→ 任务恢复（断点续传）──→ 完成
```

不涉及任何 UI 改动，纯底层重构 + 单元测试。

#### 1.2 阶段交付标准

| 验证项 | 通过条件 |
|--------|---------|
| 下载中断后可续传 | 下载进度 ≥ 30% 时关闭 APP，重启后任务仍在列表（灰显/待续传状态），点击继续能从断点恢复 |
| 手动暂停/继续有效 | 正在下载的项目可暂停（释放网络，保留文件），可继续（从断点续传） |
| 序列化/反序列化正确 | 暂停任务重启后数据完整（ready 值、path、itemId 一致） |
| 不破坏现有功能 | 普通下载流程（不下退出）不受影响 |
| 无内存/资源泄露 | pause 后 Loader 正确销毁，文件句柄释放 |
| 单测 / Debug 构建通过 | CMake 构建成功、运行无崩溃 |

#### 1.3 Phase 1 任务清单

| # | 任务 | 涉及文件 | 关键产出 |
|---|------|---------|---------|
| T1.1 | `PausedDownloadDescriptor` 结构体定义 | `data_download_manager.h` | 核心数据结构 |
| T1.2 | `PauseReason` 枚举 | `data_download_manager.h` | 暂停原因分类 |
| T1.3 | `FileLoader::pause()` + `setStartOffset()`，不再 `remove()` 文件 | `file_download.h/.cpp` | 文件保留 + offset 追踪 |
| T1.4 | `mtpFileLoader::setStartOffset()` 适配（`_nextRequestOffset` 初始化） | `file_download_mtproto.h/.cpp` | MTProto 侧续传起点 |
| T1.5 | `FileLoader` 部分文件后缀：`.temp` 标识（完成前 `.temp`，完成后 rename） | `file_download.cpp` | 临时文件命名规范 |
| T1.6 | `DocumentData::pauseLoad()` —— 调用 `_loader->pause()`，不清空 loader，标记 Paused 状态 | `data_document.h/.cpp` | 文档层暂停入口 |
| T1.7 | `DocumentData::resumeLoad()` —— 从 offset 恢复，或重建 loader | `data_document.h/.cpp` | 文档层恢复入口 |
| T1.8 | `DownloadManager::pauseAll(session)` 实现 —— 遍历 downloading，构建 Descriptor | `data_download_manager.cpp` | 批量暂停入口 |
| T1.9 | `DownloadManager::resumeOne(descriptor)` 实现 —— 校验文件 → 重建 DocumentData → 以 offset 调用 resumeLoad | `data_download_manager.cpp` | 单点恢复入口 |
| T1.10 | `DownloadManager::loadingStopSilent()` 实现 —— `pauseAll` + 阻止弹出确认框 + fire `loadingListChanges` | `data_download_manager.cpp` | 静默停止入口 |
| T1.11 | `_pausedSerialized` StorageKey 注册 | `storage/storage_account.h` | 持久化 key |
| T1.12 | PausedDownload 序列化/反序列化（QDataStream 格式）：writePaused + deserializePaused | `data_download_manager.cpp` + `storage_account.cpp` | 持久化实现 |
| T1.13 | `Application::downloadPreventsQuit()` 改造 —— 不再阻塞退出，静默 pauseAll | `core/application.cpp` | 退出流程变化 |
| T1.14 | 反序列化后自动执行「恢复前检查」：验证文件 size vs descriptor.ready，分类 Ready/Corrupted/FileMissing | `data_download_manager.cpp` 构造函数 | 状态自检 |
| T1.15 | `pausedListChanges` 信号 + `pausedCount()` 计数 | `data_download_manager.h/.cpp` | UI 通知接口 |

#### 1.4 不在 Phase 1 范围内（留至 Phase 2）

- ❌ 新 BigPage UI
- ❌ 侧边栏入口按钮（可使用已有的 Settings → Advanced → Download 入口）
- ❌ DownloadingItemWidget / DownloadedItemWidget
- ❌ 统计卡片 / 类型 Tab
- ❌ 暂停列表 UI 展示（Phase 1 仅保证文件不断裂，数据可恢复）
- ❌ 角标

#### 1.5 测试方案

**手动测试（Debug Build）：**

1. 启动 APP，下载一个 ≥ 50MB 文件（进度 ≥ 30%）
2. 关闭 APP（正常退出流程，触发 `loadingStopSilent`）
3. 重启 APP，打开现有下载页面（Settings → Advanced → Download），验证：
   - 存在"已暂停」任务项（或等待 UI 实现后再验证）
4. 删除 `/out/Debug/TelegramForcePortable` 中的对应 `.temp` 文件
5. 重启 APP，验证：
   - 任务标记为"文件损坏/缺失"（未来 UI 设计的基础）

---

### Phase 2：交互优化（UI + UX）

#### 2.1 阶段目标

在 Phase 1 基础上叠加完整 UI/UX：

- 侧边栏直达入口 + 角标
- 全新 BigPage 页面
- 正在下载 / 已完成 分组
- 文件名显示
- 在文件夹打开
- 顶部菜单：全部暂停/继续（无高危按钮）

#### 2.2 Phase 2 任务清单

| # | 任务 | 涉及文件 | 关键产出 |
|---|------|---------|---------|
| T2.1 | 下载按钮 + 角标（SideBarButton 风格） | `window_filters_menu.h/.cpp` | 侧边栏入口 |
| T2.2 | 统计聚合器 StatsAggregator | `info/downloads/stats_aggregator.h/.cpp` | 分类统计 |
| T2.3 | 统计卡片 UI | BigPageWidget 内嵌 | 信息聚合展示 |
| T2.4 | 类型过滤 Tab | BigPageWidget | 筛选交互 |
| T2.5 | DownloadingProvider | `info/downloads/downloading_provider.h/.cpp` | 正在下载数据层 |
| T2.6 | DownloadedProvider | `info/downloads/downloaded_provider.h/.cpp` | 已完成数据层 |
| T2.7 | DownloadingItemWidget | `ui/widgets/downloading_item_widget.h/.cpp` | 下载中项 UI |
| T2.8 | DownloadedItemWidget | `ui/widgets/downloaded_item_widget.h/.cpp` | 已完成项 UI |
| T2.9 | 长按菜单 | 两个 Item Widget 内 | 操作菜单 |
| T2.10 | Section::DownloadsBig 注册 + Widget/InnerWidget 框架 | `info_controller.h` + 新文件 | Info 体系导航 |
| T2.11 | WrapWidget 齿轮按钮路由 | `info_wrap_widget.cpp` | 跳转高级设置 |
| T2.12 | 顶部菜单替换（删除"删除所有文件"，改为"全部暂停/继续"） | `info_downloads_widget.cpp` | 菜单安全化 |
| T2.13 | 图标资源（downloads.svg 等） | `Resources/gui/icons/` | 视觉资源 |
| T2.14 | 风格文件 | `info/downloads/info_downloads_big_page.style` | UI 风格定义 |
| T2.15 | 本地化字符串 | `Resources/langs/lang.strings` | 多语言文案 |

#### 2.3 不在 Phase 2 范围内（未来可能考虑）

- ❌ 第三方下载（非 MTProto 来源）
- ❌ 下载限速
- ❌ 自动重试网络错误
- ❌ 托盘通知（下载完成等）

---

## 十一、待办 & 下一步

- [x] 用户审阅并确认全文
- [x] 如有分歧：修改设计
- [x] Phase 1 开发（tag `v0.1.0-phase1-pause-resume`）
- [x] Phase 2-A BigPage 骨架（tag `v0.1.1-before-bigpage`）
- [x] Phase 2-B BigPage 内容层（工作区，未编译验证）
- [x] Phase 2-C.1–3 侧边栏入口 + 图标 + 角标 + 跳转（工作区，未编译验证）
- [x] Phase 2-C.4 顶栏 ⋮ 菜单（全部暂停 / 全部继续 / 导出下载列表 / 导入下载列表）
- [x] Phase 2-C.6 菜单文案本地化
- [x] 退出持久化保证（`writeLocationsForce` + `flushPaused`）
- [x] 续传偏移两个 bug（`currentOffset()` / `checkForOpen()` 截断）
- [x] Code review 一轮：`pause()` 取偏移顺序、BigPage done 条目重复、`_pendingResumeOffset` 泄漏、暂停记录强制落盘
- [x] 下载中列表落盘（`_inflightDownloadsSerialized`，崩溃后恢复为暂停行）
- [x] 第一轮真机反馈的 5 个 bug（迭代器失效崩溃 ×2、重启丢任务、另存为文件名、下载中导出为空）
- [x] 第一轮真机反馈的 UI 项：侧边栏按钮置底、来源会话、固定操作条（全部暂停/继续）、已完成倒序、中文文案
- [x] 第二轮反馈：封面缩略图（U2a）、层宽加倍到 780px（U2c）、下载中可折叠（U2d）、已完成项多行完整文件名 + 大小·来源 + 点击跳转（U3a/U3b）、行右侧暂停/继续图标（U3c）、暂停任务保留在「下载中」组（U3d）
- [x] 第二轮反馈：播放器下载默认另存为（O1）+ 点消息信息复制并提示（O2）
- [x] 第四轮反馈：F4-1 重发副本内容键（`DownloadManager::isDownloaded`）、F4-2 复制转换 + JSON 规则 + 热重载、F4-3 播放器 caption 常驻 + 底部列表通栏（CI run 84 出包）
- [x] 第五轮反馈根因定位与修复（#2 复制标签 / #3 复制空剪贴板 / #4 已下载底色；#1 caption 由 run 85 修复，见「第五轮真机反馈」）
- [x] 推 CI 出包（run 86 = `2c1b23f`，success，产物 `Telegram-Windows-Debug`）
- [x] 第五轮真机复测：#1、#2 通过；#3、#4 无效果（见「第六轮真机反馈」）
- [x] 第六轮反馈修复：F6-1 单击消息正文复制、F6-2 数据层判据 + 相册整组变色、
      F6-3 复制前去 `#话题`、F6-4 规则数据外置（种子 + 热重载）
- [ ] **真机复测第六轮 4 项 + 2 条新需求**
- [ ] Phase 2-C.5 右键菜单（暂停/恢复/取消/在文件夹打开）
- [ ] Phase 3 集成测试

> ⚠️ 因为新增了 `.style` 文件、新样式符号（`st::windowFiltersDownloads`、`st::downloadsBig*`）
> 和新 lang key（`lt_ready` 以及 15 个 `lng_downloads_*`），必须重新 configure + 构建才会生成
> `styles/style_info_downloads_big.h` 和 lang 头文件；只增量编译已有的 .cpp 是看不到入口的。
> 另外本项目所有产物都由 GitHub Actions 从 git 提交构建（`git push github dev`），工作区未提交的改动
> 不会出现在产物里——这一点是上一轮「看不到侧边栏入口」的直接原因。

---

## 十二、下一会话开场白（预写）

```
你好，我是 CatPaw（妙手），继续 AyuGramDesktop 下载系统重构项目。先读 docs/DownloadsPageDev.md
的「第六轮真机反馈」和本节，再动手。本机不能编译，一律走 github/dev 的 CI。

已完成：
- Phase 1（暂停/续传核心 + 序列化 + 退出行为），tag v0.1.0-phase1-pause-resume
- Phase 2-A（BigPage Section 注册骨架），tag v0.1.1-before-bigpage
- Phase 2-B（BigPage 真实内容：分组 + 行模型 + 进度条 + 点击恢复/打开文件夹）
- Phase 2-C.1-4 + 2-C.6（侧边栏下载入口 + 角标 + 跳转 BigPage；顶栏 ⋮ 菜单：全部暂停/继续 + 导出/导入下载列表；文案全量本地化）
- 退出不丢任务：`Account::writeLocationsForce()` + `DownloadManager::flushPaused()`
- 第一 / 二 / 三 / 四 / 五轮真机反馈修复（详见文档对应小节）
- 第四轮 F4-1（CI run 79 出包）：`DownloadManager::isDownloaded(const DocumentData*)`
  按「大小精确相等 +（双方都有时长且相等 || 双方原始文件名非空且相等）」认领重发副本；
  `addLoaded()` 补写 `document->setLocation()` + `writeFileLocation(mediaKey(), ...)`；
  `DownloadedId` 加 `duration` / `sourceFilename`；overlay 三处判定改为
  `loaded(true) || isDownloaded(_document)`。
- 第四轮 F4-3（CI run 84 出包）：播放器 caption 常驻 + 底部媒体列表通栏居中。
- 第四轮 F4-2（CI run 84 出包）：`Ayu::ClipboardRules`（ayu/utils/clipboard_rules.*）
  —— `ToFilename` / `ToCleanName` / `Current` / `StartWatcher`，规则 JSON 缺失时用
  内置默认，`QFileSystemWatcher` 热重载，不轮询。
- 第五轮修复（CI run 86 = `2c1b23f`，success）：
  - #1 caption 只在第一个媒体显示 → run 85（`90f60f2`）已修（overlay 三级回退）；
  - #2 复制没按原始 js 提取标签 → `ToFilename()` 现在解析 `[...]` 前缀并调 `TakeType()`；
  - #3 消息列表复制没复制出文本 → 相册 `HistoryGroupText()` 为空时回退单条正文；
    `ListWidget::copySelectedText()` 删掉 `getSelectedRichBlocks()`，统一走 `ToFilename`；
  - #4 已下载视频没底色 → `Message::draw()` 判据追加 `!doc->filepath().isEmpty()`。

- 第五轮真机复测：#1 caption、#2 复制标签 通过；#3、#4 无效（原因见第六轮）。
- 第六轮修复（待出包）：
  - F6-1 单击消息正文复制 → 新增 `MessageTextCopyClickHandler`，
    `Message::getStateText()` 在正文 `link` 为空时挂上，并在
    `allowTextSelectionByHandler()` 放行以保住拖选；
  - F6-2 已下载视频底色 → 判据改数据层（`entry->media()->document()`）+
    新增 `DownloadManager::isDownloadedItem(FullMsgId)` 按持久化 itemId 匹配 +
    相册组内任一 part 命中即整条变色；
  - F6-3 复制前去 `#话题` → `TakeType()` 之后再 `RemoveHashtags` +
    `CollapseWhitespace`，`keepHashtags` 默认 false；
  - F6-4 规则外置 → `EnsureFile()` 首次运行把 `ayu_clipboard_rules.json`
    写到 exe 目录，之后热重载。

未做：Phase 2-C.5 右键菜单；第六轮 4 项 + 2 条新需求的真机验证；Phase 3 集成测试。

本轮任务：**先推 CI 出包（`git push github dev`），真机复测第六轮 4 项 + 2 条新需求**：
1. 聊天窗口单击消息正文的文字 → 复制（拖选仍然可用）；
2. 历史下载 / 后续新下载的视频，整个气泡换 `st::msgFile2Bg` 底色；
   相册里下载了任意一个 → 整组都变色；
3. 复制出来的文件名不含 `#话题`，但仍带 `[标签]` 前缀；
4. exe 旁边出现 `ayu_clipboard_rules.json`，改它不用重新出包（改完不用重启）。

注意：
- 每批推 CI 一轮，推之前做静态自查（括号/引号/原始字符串平衡、逐 hunk 复核删除行、
  文本常数字节数 < 16380），见 .ai-memory/ci-windows-build.md 末节。
- `AddCopyFilename`（复制文件名）仍显式排除视频，见 F4-2；放宽需单独确认。
```

---

*文档由 CatPaw AI （妙手），基于代码系统调研后自动生成。*
