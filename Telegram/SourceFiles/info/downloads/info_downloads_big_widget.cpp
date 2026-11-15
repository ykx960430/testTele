/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_big_widget.h"

#include "info/info_controller.h"
#include "info/info_memento.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_download_manager.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "base/algorithm.h"
#include "base/flat_map.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/text/text.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "styles/style_basic.h"
#include "styles/style_info_downloads_big.h"
#include "styles/style_menu_icons.h"

#include <algorithm>
#include <memory>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>

namespace Info::DownloadsBig {
namespace {

[[nodiscard]] bool ChineseUi() {
	const auto id = Lang::LanguageIdOrDefault(Lang::Id());
	return id.startsWith(u"zh"_q)
		|| (QLocale(id).language() == QLocale::Chinese);
}

[[nodiscard]] QString Tr(QString en, QString zh) {
	return ChineseUi() ? std::move(zh) : std::move(en);
}

[[nodiscard]] QString JoinStatus(
		const QString &first,
		const QString &second) {
	if (first.isEmpty()) {
		return second;
	} else if (second.isEmpty()) {
		return first;
	}
	return first + u" · "_q + second;
}

struct Row {
	enum class Kind {
		Stats,
		Header,
		Downloading,
		Paused,
		Completed,
	};
	Kind kind = Kind::Header;
	QString title;
	Ui::Text::String titleText;
	QString status;
	int64 ready = 0;
	int64 total = 0;
	int titleHeight = 0;
	Data::DownloadDate started = 0;
	Data::PausedDownloadDescriptor paused;
	QString path;
	QImage thumb;
	FullMsgId itemId;
	HistoryItem *item = nullptr;
	QRect geometry;
	QRect text;
	QRect action;
	bool collapsible = false;
	bool collapsed = false;

	[[nodiscard]] bool clickable() const {
		// Photos are only tracked, their download is neither paused nor
		// resumable, so a paused photo row must not look clickable.
		return (kind == Kind::Completed)
			|| (kind == Kind::Paused
				&& paused.downloadType == Data::DownloadType::Document);
	}
	[[nodiscard]] bool hoverable() const {
		return clickable() || collapsible || !action.isEmpty();
	}
	[[nodiscard]] bool small() const {
		return (kind == Kind::Header) || (kind == Kind::Stats);
	}
};

[[nodiscard]] QString DocumentName(const Data::DownloadObject *object) {
	return (object && object->document)
		? object->document->filename()
		: QString();
}

[[nodiscard]] QString TitleFor(
		const QString &path,
		const QString &fallback) {
	// WHY: the path holds the name the user typed in "Save as", while the
	// document keeps the name it had in the message.
	const auto name = QFileInfo(path).fileName();
	return name.isEmpty() ? fallback : name;
}

[[nodiscard]] QString SourceText(const QString &title) {
	return title.isEmpty()
		? QString()
		: Tr(u"from "_q, u"来自 "_q) + title;
}

[[nodiscard]] QString ProgressText(int64 ready, int64 total) {
	const auto readyText = Ui::FormatSizeText(ready);
	if (total <= 0) {
		return readyText;
	}
	const auto totalText = Ui::FormatSizeText(total);
	return Tr(
		tr::lng_downloads_big_progress(
			tr::now,
			lt_ready,
			readyText,
			lt_total,
			totalText),
		u"%1 / %2"_q.arg(readyText, totalText));
}

[[nodiscard]] QString PeerTitleOf(const Data::DownloadedId &id) {
	const auto item = id.object ? id.object->item.get() : nullptr;
	if (!item) {
		return QString();
	}
	const auto history = item->history();
	if (const auto peer = history->session().data().peerLoaded(
			id.itemId.peer)) {
		return peer->name();
	}
	return history->peer->name();
}

[[nodiscard]] QImage ThumbnailFor(
		const Data::DownloadObject &object,
		base::flat_map<uint64, QImage> &cache) {
	const auto size = st::downloadsBigThumbSize;
	const Image *image = nullptr;
	auto id = uint64(0);
	// WHY: createMediaView() hands over the only shared_ptr, so the view
	// and the Image pointers inside it die at the end of the block
	// unless we keep it alive until the image has been read out.
	std::shared_ptr<Data::DocumentMedia> documentMedia;
	std::shared_ptr<Data::PhotoMedia> photoMedia;
	if (object.document) {
		id = object.document->id;
		documentMedia = object.document->createMediaView();
		image = documentMedia->thumbnail()
			? documentMedia->thumbnail()
			: documentMedia->thumbnailInline();
	} else if (object.photo) {
		id = object.photo->id;
		photoMedia = object.photo->createMediaView();
		image = photoMedia->thumbnailInline();
	}
	if (!id) {
		return QImage();
	}
	if (const auto i = cache.find(id); i != cache.end()) {
		return i->second;
	}
	if (!image || image->original().isNull()) {
		return QImage();
	}
	auto prepared = Images::Prepare(
		image->original(),
		QSize(size, size),
		{ .outer = QSize(size, size) });
	const auto result = Images::Round(
		std::move(prepared),
		Images::Option::RoundSmall,
		QRect(QPoint(0, 0), QSize(size, size)));
	if (cache.size() > 200) {
		cache.clear();
	}
	cache.emplace(id, result);
	return result;
}

// WHY: the style globals are only resolved by style::StartManager, which
// runs once the app starts and never before the static initializers, so a
// namespace-scope copy would keep a null font for the whole session.
[[nodiscard]] style::TextStyle MakeNameStyle() {
	auto result = style::TextStyle(st::defaultTextStyle);
	result.font = st::downloadsBigNameFont;
	return result;
}

// WHY: the name has to be shown in full, so a long unbroken name is split
// across lines instead of being clipped at the row edge.
void DrawTitle(Painter &p, const Ui::Text::String &text, const QRect &rect) {
	auto geometry = Ui::Text::SimpleGeometry(rect.width(), 0, 0, false);
	geometry.breakEverywhere = true;
	text.draw(p, Ui::Text::PaintContext{
		.position = rect.topLeft(),
		.availableWidth = rect.width(),
		.geometry = std::move(geometry),
		.align = style::al_left,
		.palette = &p.textPalette(),
	});
}

} // namespace

namespace {

class ActionBar final : public Ui::RpWidget {
public:
	ActionBar(
		QWidget *parent,
		Fn<void()> pauseAll,
		Fn<void()> resumeAll);

private:
	enum class Action {
		None,
		Pause,
		Resume,
	};

	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *) override;

	void refresh();
	void updateRects();
	[[nodiscard]] Action actionAt(QPoint position) const;
	[[nodiscard]] QRect rectFor(Action action) const;
	[[nodiscard]] QString textFor(Action action) const;
	[[nodiscard]] bool enabled(Action action) const;

	Fn<void()> _pauseAll;
	Fn<void()> _resumeAll;
	QString _pauseText;
	QString _resumeText;
	QRect _pauseRect;
	QRect _resumeRect;
	Action _hovered = Action::None;
	Action _pressed = Action::None;
	int _loadingCount = 0;
	int _pausedCount = 0;

};

ActionBar::ActionBar(
	QWidget *parent,
	Fn<void()> pauseAll,
	Fn<void()> resumeAll)
: RpWidget(parent)
, _pauseAll(std::move(pauseAll))
, _resumeAll(std::move(resumeAll))
, _pauseText(Tr(
	tr::lng_downloads_pause_all(tr::now),
	u"全部暂停"_q))
, _resumeText(Tr(
	tr::lng_downloads_resume_all(tr::now),
	u"全部继续"_q)) {
	setMouseTracking(true);

	auto &manager = Core::App().downloadManager();
	_loadingCount = manager.loadingCount();
	_pausedCount = manager.pausedCount();
	rpl::merge(
		manager.loadingListChanges() | rpl::map_to(true),
		manager.pausedListChanges() | rpl::map_to(true)
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());
}

void ActionBar::resizeEvent(QResizeEvent *) {
	updateRects();
}

void ActionBar::refresh() {
	auto &manager = Core::App().downloadManager();
	const auto loading = manager.loadingCount();
	const auto paused = manager.pausedCount();
	if (loading == _loadingCount && paused == _pausedCount) {
		return;
	}
	_loadingCount = loading;
	_pausedCount = paused;
	if (_hovered != Action::None && !enabled(_hovered)) {
		_hovered = Action::None;
		setCursor(style::cur_default);
	}
	update();
}

void ActionBar::updateRects() {
	const auto &font = st::downloadsBigBarButtonFont;
	const auto &padding = st::downloadsBigBarButtonPadding;
	const auto buttonHeight = font->height + padding.top() + padding.bottom();
	const auto top = (height() - buttonHeight) / 2;
	auto right = width() - st::downloadsBigBarPadding.right();
	const auto place = [&](const QString &text) {
		const auto buttonWidth = font->width(text)
			+ padding.left()
			+ padding.right();
		auto result = QRect(right, top, buttonWidth, buttonHeight);
		right = result.left() - st::downloadsBigBarButtonSkip;
		return result;
	};
	_resumeRect = place(_resumeText);
	_pauseRect = place(_pauseText);
}

ActionBar::Action ActionBar::actionAt(QPoint position) const {
	if (_pauseRect.contains(position)) {
		return Action::Pause;
	} else if (_resumeRect.contains(position)) {
		return Action::Resume;
	}
	return Action::None;
}

QRect ActionBar::rectFor(Action action) const {
	return (action == Action::Pause) ? _pauseRect : _resumeRect;
}

QString ActionBar::textFor(Action action) const {
	return (action == Action::Pause) ? _pauseText : _resumeText;
}

bool ActionBar::enabled(Action action) const {
	return (action == Action::Pause)
		? (_loadingCount > 0)
		: (_pausedCount > 0);
}

void ActionBar::paintEvent(QPaintEvent *) {
	// WHY: the rects are only recomputed on resize, and a bar that never
	// gets one (deferred while hidden) would keep null rects, so both
	// labels get centered on x = 0 and the buttons hit-test nothing.
	updateRects();
	auto p = Painter(this);
	p.fillRect(rect(), st::downloadsBigBarBg);
	p.fillRect(
		QRect(0, height() - st::downloadsBigBarBorder, width(),
			st::downloadsBigBarBorder),
		st::downloadsBigBarBorderFg);

	p.setFont(st::downloadsBigBarButtonFont);
	for (auto action : { Action::Pause, Action::Resume }) {
		const auto button = rectFor(action);
		const auto isEnabled = enabled(action);
		const auto hovered = (_hovered == action) && isEnabled;
		if (hovered) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::downloadsBigBarButtonBgOver);
			p.drawRoundedRect(
				button,
				st::downloadsBigBarButtonRadius,
				st::downloadsBigBarButtonRadius);
		}
		p.setPen(isEnabled
			? (hovered
				? st::downloadsBigBarButtonFgOver
				: st::downloadsBigBarButtonFg)
			: st::downloadsBigBarButtonFgDisabled);
		p.drawText(button, Qt::AlignCenter, textFor(action));
	}
}

void ActionBar::mouseMoveEvent(QMouseEvent *e) {
	const auto action = actionAt(e->pos());
	const auto hovered = enabled(action) ? action : Action::None;
	if (hovered == _hovered) {
		return;
	}
	_hovered = hovered;
	setCursor((_hovered != Action::None)
		? style::cur_pointer
		: style::cur_default);
	update();
}

void ActionBar::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton)
		? actionAt(e->pos())
		: Action::None;
}

void ActionBar::mouseReleaseEvent(QMouseEvent *e) {
	const auto action = actionAt(e->pos());
	const auto pressed = _pressed;
	_pressed = Action::None;
	if (e->button() != Qt::LeftButton
		|| action != pressed
		|| !enabled(action)) {
		return;
	}
	if (action == Action::Pause) {
		_pauseAll();
	} else if (action == Action::Resume) {
		_resumeAll();
	}
}

void ActionBar::leaveEventHook(QEvent *) {
	if (_hovered == Action::None) {
		return;
	}
	_hovered = Action::None;
	setCursor(style::cur_default);
	update();
}

} // namespace

namespace {

class InnerWidget final : public Ui::RpWidget {
public:
	InnerWidget(QWidget *parent, not_null<Info::Controller*> controller);

private:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *) override;

	void rebuild();
	void buildRows();
	void updateProgress();
	int relayout(int newWidth);
	[[nodiscard]] int rowAt(QPoint position) const;
	void updateRow(int index);
	void activateRow(int index);
	void resumeRow(const Row &row);
	void jumpToRow(const Row &row);
	[[nodiscard]] QRect toggleRect(const Row &row) const;
	void paintSmall(Painter &p, const Row &row) const;
	void paintItem(Painter &p, const Row &row, bool hovered) const;

	not_null<Info::Controller*> _controller;
	style::TextStyle _nameStyle;
	std::vector<Row> _rows;
	base::flat_map<uint64, QImage> _thumbs;
	int _downloadingBegin = 0;
	int _downloadingCount = 0;
	int _pausedCount = 0;
	int _hovered = -1;
	QPoint _lastClick;
	bool _downloadingCollapsed = false;
	bool _rebuilding = false;
	int _rowsVersion = 0;
	int _layoutVersion = -1;
	int _layoutWidth = -1;
	int _layoutHeight = 0;

};

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Info::Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _nameStyle(MakeNameStyle()) {
	setMouseTracking(true);

	auto &manager = Core::App().downloadManager();
	// WHY: loadingProgressValue() replays the current value on subscription,
	// which would rebuild the rows a second time right after this one.
	rebuild();
	rpl::merge(
		manager.loadingListChanges() | rpl::map_to(true),
		manager.pausedListChanges() | rpl::map_to(true),
		manager.loadedAdded() | rpl::map_to(true),
		manager.loadedRemoved() | rpl::map_to(true),
		manager.loadedResolveDone() | rpl::map_to(true)
	) | rpl::on_next([=] {
		rebuild();
	}, lifetime());

	manager.loadingProgressValue(
	) | rpl::on_next([=] {
		updateProgress();
	}, lifetime());
}

int InnerWidget::resizeGetHeight(int newWidth) {
	return relayout(newWidth);
}

void InnerWidget::rebuild() {
	if (_rebuilding) {
		return;
	}
	_rebuilding = true;
	_hovered = -1;
	buildRows();
	if (width() > 0) {
		resize(width(), relayout(width()));
	}
	_rebuilding = false;
	update();
}

void InnerWidget::buildRows() {
	auto &manager = Core::App().downloadManager();
	auto result = std::vector<Row>();

	const auto addHeader = [&](QString title, int count, bool collapsible) {
		if (!count) {
			return;
		}
		auto row = Row();
		row.kind = Row::Kind::Header;
		row.title = std::move(title);
		row.collapsible = collapsible;
		row.collapsed = collapsible && _downloadingCollapsed;
		result.push_back(std::move(row));
	};

	auto loading = std::vector<Row>();
	for (const auto id : manager.loadingList()) {
		if (id->done) {
			continue;
		}
		auto row = Row();
		row.kind = Row::Kind::Downloading;
		row.ready = id->ready;
		row.total = id->total;
		row.started = id->started;
		row.path = id->path;
		row.item = id->object.item.get();
		row.title = TitleFor(id->path, DocumentName(&id->object));
		row.titleText.setText(_nameStyle, row.title);
		row.thumb = ThumbnailFor(id->object, _thumbs);
		row.status = JoinStatus(
			SourceText(id->object.item->history()->peer->name()),
			ProgressText(id->ready, id->total));
		loading.push_back(std::move(row));
	}

	auto paused = std::vector<Row>();
	for (const auto descriptor : manager.pausedList()) {
		auto row = Row();
		row.kind = Row::Kind::Paused;
		row.paused = *descriptor;
		row.path = descriptor->path;
		row.ready = descriptor->ready;
		row.total = descriptor->total;
		row.started = descriptor->started;
		row.title = TitleFor(descriptor->path, descriptor->displayName);
		if (row.title.isEmpty()) {
			row.title = Tr(tr::lng_downloads_big_paused(tr::now), u"已暂停"_q);
		}
		row.titleText.setText(_nameStyle, row.title);
		row.status = JoinStatus(
			Tr(tr::lng_downloads_big_paused(tr::now), u"已暂停"_q),
			JoinStatus(
				SourceText(descriptor->sourceTitle),
				ProgressText(row.ready, row.total)));
		paused.push_back(std::move(row));
	}

	auto completed = std::vector<Row>();
	auto totalSize = int64(0);
	for (const auto id : manager.loadedList()) {
		if (id->path.isEmpty()) {
			continue;
		}
		auto row = Row();
		row.kind = Row::Kind::Completed;
		row.path = id->path;
		row.started = id->started;
		row.itemId = id->itemId;
		row.title = TitleFor(id->path, DocumentName(id->object.get()));
		row.titleText.setText(_nameStyle, row.title);
		if (id->object) {
			row.thumb = ThumbnailFor(*id->object, _thumbs);
		}
		row.status = JoinStatus(
			Ui::FormatSizeText(id->size),
			SourceText(PeerTitleOf(*id)));
		totalSize += id->size;
		completed.push_back(std::move(row));
	}
	std::sort(completed.begin(), completed.end(), [](const Row &a, const Row &b) {
		return a.started > b.started;
	});

	if (!completed.empty()) {
		const auto sizeText = Ui::FormatSizeText(totalSize);
		auto stats = Row();
		stats.kind = Row::Kind::Stats;
		stats.title = Tr(
			tr::lng_downloads_big_total(tr::now, lt_size, sizeText),
			u"共 %1"_q.arg(sizeText));
		result.push_back(std::move(stats));
	}
	// WHY: a paused task is still a download task, so it stays in the
	// downloading group instead of moving to a list of its own.
	const auto activeCount = int(loading.size()) + int(paused.size());
	_pausedCount = int(paused.size());
	addHeader(
		Tr(tr::lng_downloads_big_downloading(tr::now), u"下载中"_q),
		activeCount,
		true);
	_downloadingBegin = int(result.size());
	_downloadingCount = _downloadingCollapsed ? 0 : activeCount;
	if (!_downloadingCollapsed) {
		for (auto &row : loading) {
			result.push_back(std::move(row));
		}
		for (auto &row : paused) {
			result.push_back(std::move(row));
		}
	}
	addHeader(
		Tr(tr::lng_downloads_big_completed(tr::now), u"已下载"_q),
		int(completed.size()),
		false);
	for (auto &row : completed) {
		result.push_back(std::move(row));
	}

	++_rowsVersion;
	_rows = std::move(result);
}

void InnerWidget::updateProgress() {
	if (_downloadingCollapsed) {
		return;
	}
	auto &manager = Core::App().downloadManager();
	auto index = _downloadingBegin;
	auto count = 0;
	const auto loadingCount = std::max(_downloadingCount - _pausedCount, 0);
	if (_downloadingBegin < 0
		|| _downloadingBegin + loadingCount > int(_rows.size())) {
		rebuild();
		return;
	}
	for (const auto id : manager.loadingList()) {
		if (id->done) {
			continue;
		}
		if (count++ >= loadingCount) {
			rebuild();
			return;
		}
		auto &row = _rows[index++];
		row.ready = id->ready;
		row.total = id->total;
		row.status = JoinStatus(
			SourceText(id->object.item->history()->peer->name()),
			ProgressText(row.ready, row.total));
	}
	if (count != loadingCount) {
		rebuild();
		return;
	}
	if (!_downloadingCount
		|| _downloadingBegin + _downloadingCount > int(_rows.size())) {
		return;
	}
	const auto first = _rows[_downloadingBegin].geometry.top();
	const auto last = _rows[
		_downloadingBegin + _downloadingCount - 1].geometry.bottom();
	update(0, first, width(), last - first);
}

int InnerWidget::relayout(int newWidth) {
	if (_rows.empty()) {
		return st::downloadsBigEmptyTop
			+ st::downloadsBigEmptyFont->height
			+ st::downloadsBigPadding.bottom();
	}
	if (_layoutWidth == newWidth && _layoutVersion == _rowsVersion) {
		return _layoutHeight;
	}
	const auto &padding = st::downloadsBigRowPadding;
	const auto thumbSize = st::downloadsBigThumbSize;
	auto top = 0;
	for (auto &row : _rows) {
		if (row.small()) {
			top += st::downloadsBigHeaderTop;
			row.geometry = QRect(
				0,
				top,
				newWidth,
				st::downloadsBigHeaderHeight);
			top += row.geometry.height() + st::downloadsBigHeaderBottom;
			continue;
		}
		const auto hasAction = (row.kind == Row::Kind::Downloading)
			|| (row.kind == Row::Kind::Paused);
		const auto textLeft = padding.left()
			+ thumbSize
			+ st::downloadsBigThumbSkip;
		const auto actionWidth = hasAction
			? (st::downloadsBigActionSize + st::downloadsBigActionSkip)
			: 0;
		const auto textWidth = std::max(
			newWidth - padding.right() - actionWidth - textLeft,
			0);
		row.titleHeight = row.titleText.countHeight(textWidth, true);
		const auto statusHeight = st::downloadsBigStatusFont->height;
		const auto contentHeight = row.titleHeight
			+ st::downloadsBigStatusTop
			+ statusHeight;
		const auto innerHeight = std::max(thumbSize, contentHeight);
		const auto rowHeight = innerHeight + padding.top() + padding.bottom();
		row.geometry = QRect(0, top, newWidth, rowHeight);
		row.text = QRect(
			textLeft,
			top + padding.top() + (innerHeight - contentHeight) / 2,
			textWidth,
			contentHeight);
		row.action = hasAction
			? QRect(
				newWidth - padding.right() - st::downloadsBigActionSize,
				top + (rowHeight - st::downloadsBigActionSize) / 2,
				st::downloadsBigActionSize,
				st::downloadsBigActionSize)
			: QRect();
		top += rowHeight;
	}
	_layoutWidth = newWidth;
	_layoutVersion = _rowsVersion;
	_layoutHeight = top + st::downloadsBigPadding.bottom();
	return _layoutHeight;
}

int InnerWidget::rowAt(QPoint position) const {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].geometry.contains(position)) {
			return i;
		}
	}
	return -1;
}

void InnerWidget::updateRow(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	update(_rows[index].geometry);
}

void InnerWidget::activateRow(int index) {
	const auto &row = _rows[index];
	if (row.collapsible) {
		_downloadingCollapsed = !_downloadingCollapsed;
		rebuild();
		return;
	}
	if (!row.action.isEmpty() && row.action.contains(_lastClick)) {
		if (row.kind == Row::Kind::Downloading && row.item) {
			Core::App().downloadManager().pauseOne(row.item);
		} else if (row.kind == Row::Kind::Paused) {
			resumeRow(row);
		}
		return;
	}
	if (!row.clickable()) {
		return;
	}
	if (row.kind == Row::Kind::Paused) {
		resumeRow(row);
	} else if (row.kind == Row::Kind::Completed
		|| row.kind == Row::Kind::Downloading) {
		jumpToRow(row);
	}
}

void InnerWidget::resumeRow(const Row &row) {
	if (!Core::App().downloadManager().resumeOne(row.paused)) {
		Ui::Toast::Show(Tr(
			tr::lng_downloads_resume_failed(tr::now),
			u"无法继续该下载。"_q));
	}
}

void InnerWidget::jumpToRow(const Row &row) {
	if (!row.itemId.peer) {
		return;
	}
	_controller->showPeerHistory(
		row.itemId.peer,
		Window::SectionShow(Window::SectionShow::Way::Forward),
		row.itemId.msg);
}

QRect InnerWidget::toggleRect(const Row &row) const {
	const auto size = st::downloadsBigToggleSize;
	return QRect(
		row.geometry.width() - st::downloadsBigPadding.right() - size,
		row.geometry.top()
			+ (row.geometry.height() - size) / 2,
		size,
		size);
}

void InnerWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_rows.empty()) {
		p.setFont(st::downloadsBigEmptyFont);
		p.setPen(st::downloadsBigEmptyFg);
		p.drawText(
			QRect(
				0,
				st::downloadsBigEmptyTop,
				width(),
				st::downloadsBigEmptyFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			Tr(
				tr::lng_downloads_big_empty(tr::now),
				u"还没有下载任务"_q));
		return;
	}
	const auto clip = e->rect();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		if (!row.geometry.intersects(clip)) {
			continue;
		}
		const auto hovered = (_hovered == i) && row.hoverable();
		if (hovered) {
			p.fillRect(row.geometry, st::downloadsBigRowBgOver);
		}
		if (row.small()) {
			paintSmall(p, row);
		} else {
			paintItem(p, row, hovered);
		}
	}
}

void InnerWidget::paintSmall(Painter &p, const Row &row) const {
	const auto &padding = st::downloadsBigPadding;
	const auto available = std::max(
		row.geometry.width() - padding.left() - padding.right(),
		0);
	if (row.kind == Row::Kind::Header) {
		p.setFont(st::downloadsBigHeaderFont);
		p.setPen(st::downloadsBigHeaderFg);
		p.drawText(
			QRect(
				padding.left(),
				row.geometry.top(),
				available,
				row.geometry.height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::downloadsBigHeaderFont->elided(row.title, available));
		if (row.collapsible) {
			const auto rect = toggleRect(row);
			if (row.collapsed) {
				p.save();
				p.translate(rect.center());
				p.rotate(180.);
				st::downloadsBigToggle.paintInCenter(
					p,
					QRect(
						-rect.width() / 2,
						-rect.height() / 2,
						rect.width(),
						rect.height()));
				p.restore();
			} else {
				st::downloadsBigToggle.paintInCenter(p, rect);
			}
		}
	} else {
		p.setFont(st::downloadsBigStatusFont);
		p.setPen(st::downloadsBigStatusFg);
		p.drawText(
			QRect(
				padding.left(),
				row.geometry.top(),
				available,
				row.geometry.height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::downloadsBigStatusFont->elided(row.title, available));
	}
}

void InnerWidget::paintItem(Painter &p, const Row &row, bool hovered) const {
	const auto &padding = st::downloadsBigRowPadding;
	const auto thumbSize = st::downloadsBigThumbSize;

	auto thumbRect = QRect(
		row.text.left() - st::downloadsBigThumbSkip - thumbSize,
		row.geometry.top()
			+ (row.geometry.height() - thumbSize) / 2,
		thumbSize,
		thumbSize);
	if (row.thumb.isNull()) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigThumbBg);
		p.drawRoundedRect(
			thumbRect,
			st::downloadsBigThumbRadius,
			st::downloadsBigThumbRadius);
		st::downloadsBigThumbIcon.paintInCenter(p, thumbRect);
	} else {
		p.drawImage(thumbRect, row.thumb);
	}

	p.setPen(st::downloadsBigNameFg);
	DrawTitle(p, row.titleText, QRect(
		row.text.x(),
		row.text.y(),
		row.text.width(),
		row.titleHeight));

	const auto statusTop = row.text.y()
		+ row.titleHeight
		+ st::downloadsBigStatusTop;
	p.setFont(st::downloadsBigStatusFont);
	p.setPen(st::downloadsBigStatusFg);
	p.drawText(
		QRect(row.text.x(), statusTop, row.text.width(), row.text.height()
			- row.titleHeight - st::downloadsBigStatusTop),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::downloadsBigStatusFont->elided(row.status, row.text.width()));

	if (row.action.isEmpty()) {
		return;
	}
	if (hovered) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigActionBgOver);
		p.drawRoundedRect(
			row.action,
			st::downloadsBigActionRadius,
			st::downloadsBigActionRadius);
	}
	const auto &icon = (row.kind == Row::Kind::Downloading)
		? st::downloadsBigActionPause
		: st::downloadsBigActionResume;
	icon.paintInCenter(p, row.action);
}

void InnerWidget::mouseMoveEvent(QMouseEvent *e) {
	const auto previous = _hovered;
	const auto index = rowAt(e->pos());
	const auto hovered = (index >= 0 && _rows[index].hoverable())
		? index
		: -1;
	if (hovered == _hovered) {
		return;
	}
	_hovered = hovered;
	setCursor((_hovered >= 0) ? style::cur_pointer : style::cur_default);
	updateRow(previous);
	updateRow(_hovered);
}

void InnerWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_lastClick = e->pos();
	const auto index = rowAt(e->pos());
	if (index < 0) {
		return;
	}
	activateRow(index);
}

void InnerWidget::leaveEventHook(QEvent *) {
	const auto previous = _hovered;
	_hovered = -1;
	setCursor(style::cur_default);
	updateRow(previous);
}

} // namespace

Memento::Memento(not_null<UserData*> self)
: ContentMemento(DownloadsBig::Tag()) {
}

Memento::~Memento() = default;

object_ptr<ContentWidget> Memento::createWidget(
		QWidget *parent,
		not_null<Controller*> controller,
		const QRect &geometry) {
	auto result = object_ptr<Widget>(parent, controller);
	result->setInternalState(geometry, this);
	return result;
}

Section Memento::section() const {
	return Section(Section::Type::DownloadsBig);
}

Widget::Widget(QWidget *parent, not_null<Controller*> controller)
: ContentWidget(parent, controller) {
	setInnerWidget(object_ptr<InnerWidget>(this, controller));
	_actionBar = Ui::CreateChild<ActionBar>(
		this,
		[=] { Core::App().downloadManager().pauseAll(); },
		[=] { resumeAllPaused(); });
	// The bar sits in the gap that the scroll area leaves above itself.
	setScrollTopSkip(st::downloadsBigBarHeight);
}

void Widget::resizeEvent(QResizeEvent *e) {
	ContentWidget::resizeEvent(e);
	if (_actionBar) {
		_actionBar->setGeometry(0, 0, width(), st::downloadsBigBarHeight);
		_actionBar->raise();
	}
}

bool Widget::showInternal(not_null<ContentMemento*> memento) {
	if (const auto downloadsMemento = dynamic_cast<Memento*>(memento.get())) {
		restoreState(downloadsMemento);
		return true;
	}
	return false;
}

void Widget::setInternalState(
		const QRect &geometry,
		not_null<Memento*> memento) {
	setGeometry(geometry);
	Ui::SendPendingMoveResizeEvents(this);
	restoreState(memento);
}

rpl::producer<QString> Widget::title() {
	return tr::lng_downloads_section();
}

void Widget::saveState(not_null<Memento*> memento) {
	memento->setScrollTop(scrollTopSave());
}

void Widget::restoreState(not_null<Memento*> memento) {
	scrollTopRestore(memento->scrollTop());
}

void Widget::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	auto &manager = Core::App().downloadManager();
	if (manager.loadingCount() > 0) {
		addAction(
			Tr(tr::lng_downloads_pause_all(tr::now), u"全部暂停"_q),
			[=] { Core::App().downloadManager().pauseAll(); },
			&st::menuIconDownload);
	}
	if (manager.pausedCount() > 0) {
		addAction(
			Tr(tr::lng_downloads_resume_all(tr::now), u"全部继续"_q),
			[=] { resumeAllPaused(); },
			&st::menuIconDownload);
	}
	addAction(
		Tr(tr::lng_downloads_export_list(tr::now), u"导出下载列表"_q),
		[=] { exportPausedList(); },
		&st::menuIconExport);
	addAction(
		Tr(tr::lng_downloads_import_list(tr::now), u"导入下载列表"_q),
		[=] { importPausedList(); },
		&st::menuIconImportTheme);
}

void Widget::resumeAllPaused() {
	auto &manager = Core::App().downloadManager();
	auto descriptors = std::vector<Data::PausedDownloadDescriptor>();
	for (const auto descriptor : manager.pausedList()) {
		descriptors.push_back(*descriptor);
	}
	auto resumed = 0;
	for (const auto &descriptor : descriptors) {
		if (manager.resumeOne(descriptor)) {
			++resumed;
		}
	}
	if (!resumed && !descriptors.empty()) {
		Ui::Toast::Show(Tr(
			tr::lng_downloads_resume_failed(tr::now),
			u"无法继续下载：媒体消息还没加载完成。"_q));
	}
}

void Widget::exportPausedList() {
	auto &manager = Core::App().downloadManager();
	const auto json = manager.exportPausedJson();
	if (json.isEmpty()) {
		Ui::Toast::Show(Tr(
			tr::lng_downloads_export_empty(tr::now),
			u"没有可导出的下载任务。"_q));
		return;
	}
	const auto count = manager.pausedCount() + manager.loadingCount();
	const auto data = json.toUtf8();
	auto filter = u"JSON (*.json);;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetWritePath(
		this,
		Tr(tr::lng_downloads_export_list(tr::now), u"导出下载列表"_q),
		filter,
		filedialogDefaultName(u"downloads"_q, u".json"_q),
		crl::guard(this, [=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			auto file = QFile(path);
			const auto written = file.open(QIODevice::WriteOnly)
				&& (file.write(data) == data.size());
			file.close();
			Ui::Toast::Show(written
				? Tr(
					tr::lng_downloads_export_done(tr::now, lt_count, count),
					u"已导出 %1 个下载任务"_q.arg(count))
				: Tr(
					tr::lng_downloads_export_failed(tr::now),
					u"无法保存文件。"_q));
		}));
}

void Widget::importPausedList() {
	auto filter = u"JSON (*.json);;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetOpenPath(
		this,
		Tr(tr::lng_downloads_import_list(tr::now), u"导入下载列表"_q),
		filter,
		crl::guard(this, [=](FileDialog::OpenResult &&result) {
			if (!result.paths.isEmpty()) {
				auto file = QFile(result.paths.front());
				if (file.open(QIODevice::ReadOnly)) {
					importPausedListDone(QString::fromUtf8(file.readAll()));
				} else {
					Ui::Toast::Show(Tr(
						tr::lng_downloads_import_failed(tr::now),
						u"这个文件不是下载列表。"_q));
				}
			} else if (!result.remoteContent.isEmpty()) {
				importPausedListDone(QString::fromUtf8(result.remoteContent));
			}
		}));
}

void Widget::importPausedListDone(const QString &json) {
	const auto added = Core::App().downloadManager().importPausedJson(json);
	Ui::Toast::Show(!added.has_value()
		? Tr(
			tr::lng_downloads_import_failed(tr::now),
			u"这个文件不是下载列表。"_q)
		: (*added
			? Tr(
				tr::lng_downloads_import_done(tr::now, lt_count, *added),
				u"已导入 %1 个下载任务"_q.arg(*added))
			: Tr(
				tr::lng_downloads_import_empty(tr::now),
				u"没有新的下载任务被添加。"_q)));
}

std::shared_ptr<ContentMemento> Widget::doCreateMemento() {
	auto result = std::make_shared<Memento>(
		controller()->session().user());
	saveState(result.get());
	return result;
}

std::shared_ptr<Info::Memento> Make(not_null<UserData*> self) {
	return std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<ContentMemento>>(
			1,
			std::make_shared<Memento>(self)));
}

} // namespace Info::DownloadsBig
