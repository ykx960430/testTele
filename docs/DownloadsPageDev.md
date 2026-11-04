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

### Phase 1：基础设施（核心）

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T1.1 | PausedDownload 结构体定义 | `data_download_manager.h` | 1h |
| T1.2 | FileLoader::pause() 实现 | `file_download.cpp` + `.h` | 2h |
| T1.3 | mtpFileLoader 适配（startOffset + resume） | `file_download_mtproto.cpp` + `.h` | 3h |
| T1.4 | DocumentData::pauseLoad() / resumeLoad() | `data_document.cpp` | 3h |
| T1.5 | Partial File Naming（.download 后缀） | `file_download.cpp` | 2h |
| T1.6 | DownloadManager::pauseAll / resumeAll / pauseOne / resumeOne 实现 | `data_download_manager.cpp` | 4h |
| T1.7 | 序列化 PausedDownload 列表 | `data_download_manager.cpp` (serializator + deserialize) | 3h |
| T1.8 | StorageKey 注册 | `storage/storage_account.h/.cpp` | 1h |

### Phase 2：退出行为改造

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T2.1 | `loadingStopSilent()` 实现 | `data_download_manager.cpp` | 1h |
| T2.2 | `Application::downloadPreventsQuit()` 改造 | `core/application.cpp` | 1h |
| T2.3 | 启动时 PausedDownload 恢复检测 + 信号连接 | `data_download_manager.cpp` 构造 | 2h |

### Phase 3：侧边栏入口

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T3.1 | 下载 SVG 图标 | `Resources/gui/icons/` | 0.5h |
| T3.2 | 按钮创建 + Badge 信号绑定 | `window_filters_menu.h/.cpp` | 1.5h |
| T3.3 | 跳转到 BigPage 的导航入口 | `window_filters_menu.cpp` + `MakeBig` 函数 | 0.5h |

### Phase 4：BigPage 框架

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T4.1 | Section 枚举追加 + Memento/Widget 框架新建 | `info_controller.h` + 新建 memento 文件 | 2h |
| T4.2 | BigPageWidget 基础布局 (VerticalLayout + ScrollArea) | `info/downloads/info_downloads_big_page.h/.cpp` | 3h |
| T4.3 | TopBar 齿轮按钮（跳转高级设置） | `wrap_widget.cpp` 路由扩展 | 1h |
| T4.4 | 统计卡片 StatsAggregator 逻辑 | `info/downloads/stats_aggregator.h/.cpp` | 2h |
| T4.5 | 统计卡片 UI 实现 | 同上 | 3h |
| T4.6 | 类型 FilterTabs + Provider filter | `info/downloads/info_downloads_big_page.h/.cpp` | 2h |

### Phase 5：下载项 UI

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T5.1 | DownloadingItemWidget 实现 | `ui/widgets/downloading_item_widget.h/.cpp` | 4h |
| T5.2 | DownloadedItemWidget 实现 | `ui/widgets/downloaded_item_widget.h/.cpp` | 3h |
| T5.3 | DownloadingProvider 实现 | `info/downloads/info_downloads_provider.h/.cpp` | 3h |
| T5.4 | DownloadedProvider 实现 | `info/downloads/downloaded_provider.h/.cpp` | 2h |
| T5.5 | 右键菜单（长按菜单实现） | 各自 Item Widget 中 | 2h |
| T5.6 | 错误状态展示 | Widget | 1h |

### Phase 6：集成测试

| # | 任务 | 文件 | 估计 |
|---|------|------|------|
| T6.1 | 编译验证 | — | 1h |
| T6.2 | Debug build 运行 | — | 1h |
| T6.3 | 手动测试：下载 → 退出 → 启动 → 续传 | — | 2h |
| T6.4 | 手动测试：暂停/继续、断网恢复 | — | 1h |
| T6.5 | 手动测试：来源消息删除后恢复 | — | 0.5h |
| T6.6 | 手动测试：多 Account 场景 | — | 0.5h |

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

### 修改文件

| 文件 | 变更类型 |
|------|---------|
| `Telegram/SourceFiles/data/data_download_manager.h` | 新增 struct、新增接口声明 |
| `Telegram/SourceFiles/data/data_download_manager.cpp` | 新增 pause/resume 实现、新增序列化 |
| `Telegram/SourceFiles/data/data_document.h` | 新增 pauseLoad/resumeLoad |
| `Telegram/SourceFiles/data/data_document.cpp` | 新增 pauseLoad/resumeLoad |
| `Telegram/SourceFiles/storage/file_download.h` | 新增 pause/resume/setStartOffset 声明 |
| `Telegram/SourceFiles/storage/file_download.cpp` | 新增 pause/resume 实现 |
| `Telegram/SourceFiles/storage/file_download_mtproto.h` | 新增 startOffset 成员、resume 声明 |
| `Telegram/SourceFiles/storage/file_download_mtproto.cpp` | 适配 startOffset/resume |
| `Telegram/SourceFiles/storage/storage_account.h` | 新增 StorageKey |
| `Telegram/SourceFiles/storage/storage_account.cpp` | 新增序列化读/写 |
| `Telegram/SourceFiles/core/application.cpp` | 改造退出流程 |
| `Telegram/SourceFiles/window/window_filters_menu.h` | 新增下载按钮成员 |
| `Telegram/SourceFiles/window/window_filters_menu.cpp` | 新增按钮创建、信号绑定 |
| `Telegram/SourceFiles/info/info_controller.h` | DownloadsBig 枚举 |
| `Telegram/SourceFiles/info/info_wrap_widget.cpp` | BigPage 路由 |
| `Telegram/SourceFiles/info/downloads/info_downloads_provider.h` | pause/sourceTitle 字段扩展 |
| `Telegram/SourceFiles/info/downloads/info_downloads_provider.cpp` | 适配 pause/sourceTitle |

### 新增文件

| 文件 | 说明 |
|------|------|
| `Telegram/SourceFiles/info/downloads/info_downloads_big_page.h` | BigPage Memento + Widget + Inner |
| `Telegram/SourceFiles/info/downloads/info_downloads_big_page.cpp` | BigPage 实现 |
| `Telegram/SourceFiles/info/downloads/stats_aggregator.h` | 统计聚合 |
| `Telegram/SourceFiles/info/downloads/stats_aggregator.cpp` | 统计聚合 |
| `Telegram/SourceFiles/info/downloads/downloading_provider.h` | 正在下载 Provider |
| `Telegram/SourceFiles/info/downloads/downloading_provider.cpp` | 正在下载 Provider |
| `Telegram/SourceFiles/info/downloads/downloaded_provider.h` | 已完成 Provider |
| `Telegram/SourceFiles/info/downloads/downloaded_provider.cpp` | 已完成 Provider |
| `Telegram/SourceFiles/ui/widgets/downloading_item_widget.h` | 自定义下载项 Widget |
| `Telegram/SourceFiles/ui/widgets/downloading_item_widget.cpp` | 同上 |
| `Telegram/SourceFiles/ui/widgets/downloaded_item_widget.h` | 自定义已完成项 Widget |
| `Telegram/SourceFiles/ui/widgets/downloaded_item_widget.cpp` | 同上 |
| `Telegram/Resources/gui/icons/downloads.svg` | 侧栏下载图标 |
| `Telegram/Resources/gui/icons/folder_open.svg` | 在文件夹打开图标 |
| `Telegram/Resources/gui/icons/pause.svg` | 暂停图标 |
| `Telegram/Resources/gui/icons/resume.svg` | 恢复图标 |
| `Telegram/Resources/gui/icons/cancel.svg` | 取消图标 |
| `Telegram/Resources/langs/lang.strings` | 新增本地化字符串 |
| `Telegram/Resources/langs/lang.strings` | 新增统计卡片标题等 |

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
- [ ] **下一会话启动 Phase 1 开发**
- [ ] 生成可独立编译的开发任务单
- [ ] Phase 1 完成后立即进入用户测试环节
- [x] 在新会话启动编码（已就位）

---

## 十二、下一会话开场白（预写）

```
你好，我是 CatPaw（妙手），继续 AyuGramDesktop 下载系统重构项目。

前一会话已完成：
- 项目代码调研（DownloadManager / FileLoader / DocumentData / Application / FiltersMenu / InfoSection）
- 开发文档编写（docs/DownloadsPageDev.md）
- 文档已分 Phase 1（核心下载续传）和 Phase 2（交互优化）两个阶段
- Git 初始提交 + 锚点 tag v0.0.1-before-download-refactor 已落盘

目标会话：按照 Phase 1 任务清单逐步编码。
首任务：T1.1 PausedDownloadDescriptor 结构体 + PauseReason 枚举 → data_download_manager.h
完成后编译验证 → T1.2 → ... → T1.15，完成一轮后交付 Debug build 测试。
```

---

*文档由 CatPaw AI （妙手），基于代码系统调研后自动生成。*
