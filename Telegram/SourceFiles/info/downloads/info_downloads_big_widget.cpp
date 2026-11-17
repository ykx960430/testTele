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
#include "base/timer.h"
#include "boxes/abstract_box.h"
#include "ui/boxes/confirm_box.h"
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
#include "styles/style_info.h"
#include "styles/style_info_downloads_big.h"
#include "styles/style_menu_icons.h"

#include <algorithm>
#include <memory>

#include <QtCore/QDateTime>
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

struct StatsCard {
	const style::icon *icon = nullptr;
	QString label;
	QString value;
};

struct Row {
	enum class Kind {
		Stats,
		Header,
		ActionBar,
		Downloading,
		Paused,
		Completed,
	};
	Kind kind = Kind::Header;
	QString title;
	Ui::Text::String titleText;
	QString status;
	QString source;
	std::vector<StatsCard> cards;
	int64 ready = 0;
	int64 total = 0;
	int titleHeight = 0;
	int titleWidth = -1;
	Data::DownloadDate started = 0;
	Data::PausedDownloadDescriptor paused;
	QString path;
	QImage thumb;
	FullMsgId itemId;
	HistoryItem *item = nullptr;
	QRect geometry;
	QRect text;
	QRect statusRect;
	QRect progress;
	QRect action;
	QRect badge;
	QRect folder;
	bool collapsible = false;
	bool collapsed = false;

	[[nodiscard]] bool clickable() const {
		return (kind == Kind::Completed)
			|| (kind == Kind::Downloading)
			|| (kind == Kind::Paused);
	}
	[[nodiscard]] bool hoverable() const {
		return clickable() || collapsible || !action.isEmpty()
			|| (kind == Kind::ActionBar);
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

[[nodiscard]] QString StartedText(Data::DownloadDate started) {
	return QDateTime::fromMSecsSinceEpoch(started).toString(u"yyyyMMddHHmm"_q);
}

// WHY: the search box has to match the same fields the row shows, so the
// source name and the start time are kept alongside the title.
[[nodiscard]] bool MatchesQuery(const Row &row, const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	const auto needle = query.toLower();
	return row.title.toLower().contains(needle)
		|| row.source.toLower().contains(needle)
		|| StartedText(row.started).contains(needle);
}

// WHY: Ui::FormatSizeText stops at MB, so a multi-GB total is unreadable.
[[nodiscard]] QString SizeText(int64 size) {
	if (size >= 1024 * 1024 * 1024) {
		const auto sizeTenthGb = (size * 10) / (1024 * 1024 * 1024);
		return QString::number(sizeTenthGb / 10)
			+ '.'
			+ QString::number(sizeTenthGb % 10)
			+ u" GB"_q;
	}
	return Ui::FormatSizeText(size);
}

[[nodiscard]] QString ProgressText(int64 ready, int64 total) {
	const auto readyText = SizeText(ready);
	if (total <= 0) {
		return readyText;
	}
	const auto totalText = SizeText(total);
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

class ActionBar final {
public:
	ActionBar(
		Fn<void()> pauseAll,
		Fn<void()> resumeAll,
		Fn<void()> repaint);

	void paintIn(Painter &p, const QRect &rect) const;
	void mouseMove(QPoint position, const QRect &rect);
	void mousePress(QPoint position, const QRect &rect);
	void mouseRelease(QPoint position, const QRect &rect);
	void leave();
	void refresh();

	[[nodiscard]] bool isHovered() const;

private:
	enum class Action {
		None,
		Pause,
		Resume,
	};
	struct ButtonRects {
		QRect pause;
		QRect resume;
	};

	[[nodiscard]] ButtonRects buttonRects(const QRect &rect) const;
	[[nodiscard]] Action actionAt(QPoint position, const QRect &rect) const;
	[[nodiscard]] QRect rectFor(Action action, const QRect &rect) const;
	[[nodiscard]] QString textFor(Action action) const;
	[[nodiscard]] bool enabled(Action action) const;

	Fn<void()> _pauseAll;
	Fn<void()> _resumeAll;
	Fn<void()> _repaint;
	QString _pauseText;
	QString _resumeText;
	Action _hovered = Action::None;
	Action _pressed = Action::None;
	int _loadingCount = 0;
	int _pausedCount = 0;
	rpl::lifetime _lifetime;

};

ActionBar::ActionBar(
	Fn<void()> pauseAll,
	Fn<void()> resumeAll,
	Fn<void()> repaint)
: _pauseAll(std::move(pauseAll))
, _resumeAll(std::move(resumeAll))
, _repaint(std::move(repaint))
, _pauseText(Tr(
	tr::lng_downloads_pause_all(tr::now),
	u"全部暂停"_q))
, _resumeText(Tr(
	tr::lng_downloads_resume_all(tr::now),
	u"全部继续"_q)) {
	auto &manager = Core::App().downloadManager();
	_loadingCount = manager.loadingCount();
	_pausedCount = manager.pausedCount();
	rpl::merge(
		manager.loadingListChanges() | rpl::map_to(true),
		manager.pausedListChanges() | rpl::map_to(true)
	) | rpl::on_next([=] {
		refresh();
	}, _lifetime);
}

bool ActionBar::isHovered() const {
	return _hovered != Action::None;
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
	}
	_repaint();
}

ActionBar::ButtonRects ActionBar::buttonRects(const QRect &rect) const {
	const auto &font = st::downloadsBigBarButtonFont;
	const auto &padding = st::downloadsBigBarButtonPadding;
	const auto buttonHeight = font->height + padding.top() + padding.bottom();
	const auto top = rect.y() + (rect.height() - buttonHeight) / 2;
	const auto left = rect.x() + st::downloadsBigBarPadding.left();
	const auto widthOf = [&](const QString &text) {
		return font->width(text) + padding.left() + padding.right();
	};
	const auto pause = QRect(left, top, widthOf(_pauseText), buttonHeight);
	const auto resume = QRect(
		pause.right() + st::downloadsBigBarButtonSkip,
		top,
		widthOf(_resumeText),
		buttonHeight);
	return { pause, resume };
}

QRect ActionBar::rectFor(Action action, const QRect &rect) const {
	const auto buttons = buttonRects(rect);
	return (action == Action::Pause) ? buttons.pause : buttons.resume;
}

QString ActionBar::textFor(Action action) const {
	return (action == Action::Pause) ? _pauseText : _resumeText;
}

bool ActionBar::enabled(Action action) const {
	return (action == Action::Pause)
		? (_loadingCount > 0)
		: (_pausedCount > 0);
}

ActionBar::Action ActionBar::actionAt(
		QPoint position,
		const QRect &rect) const {
	const auto buttons = buttonRects(rect);
	if (buttons.pause.contains(position)) {
		return Action::Pause;
	} else if (buttons.resume.contains(position)) {
		return Action::Resume;
	}
	return Action::None;
}

void ActionBar::paintIn(Painter &p, const QRect &rect) const {
	p.fillRect(rect, st::downloadsBigBarBg);
	p.fillRect(
		QRect(
			rect.x(),
			rect.y() + rect.height() - st::downloadsBigBarBorder,
			rect.width(),
			st::downloadsBigBarBorder),
		st::downloadsBigBarBorderFg);

	p.setFont(st::downloadsBigBarButtonFont);
	for (auto action : { Action::Pause, Action::Resume }) {
		const auto button = rectFor(action, rect);
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

void ActionBar::mouseMove(QPoint position, const QRect &rect) {
	const auto action = actionAt(position, rect);
	const auto hovered = enabled(action) ? action : Action::None;
	if (hovered == _hovered) {
		return;
	}
	_hovered = hovered;
	_repaint();
}

void ActionBar::mousePress(QPoint position, const QRect &rect) {
	_pressed = actionAt(position, rect);
}

void ActionBar::mouseRelease(QPoint position, const QRect &rect) {
	const auto action = actionAt(position, rect);
	const auto pressed = _pressed;
	_pressed = Action::None;
	if (action != pressed || !enabled(action)) {
		return;
	}
	if (action == Action::Pause) {
		_pauseAll();
	} else if (action == Action::Resume) {
		_resumeAll();
	}
}

void ActionBar::leave() {
	if (_hovered == Action::None) {
		return;
	}
	_hovered = Action::None;
	_repaint();
}

} // namespace

namespace {

class InnerWidget final : public Ui::RpWidget {
public:
	InnerWidget(
		QWidget *parent,
		not_null<Info::Controller*> controller,
		Fn<void()> resumeAll);

	void setSearchQuery(const QString &query);

private:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
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
	void showInFolder(const Row &row) const;
	[[nodiscard]] QRect toggleRect(const Row &row) const;
	void paintSmall(Painter &p, const Row &row) const;
	void paintStats(Painter &p, const Row &row) const;
	void paintItem(Painter &p, const Row &row, bool hovered) const;

	not_null<Info::Controller*> _controller;
	style::TextStyle _nameStyle;
	ActionBar _actionBar;
	std::vector<Row> _rows;
	base::flat_map<uint64, QImage> _thumbs;
	int _downloadingBegin = 0;
	int _downloadingCount = 0;
	int _pausedCount = 0;
	int _hovered = -1;
	QPoint _lastClick;
	bool _downloadingCollapsed = false;
	QString _query;
	bool _rebuilding = false;
	base::Timer _rebuildTimer;
	int _rowsVersion = 0;
	int _layoutVersion = -1;
	int _layoutWidth = -1;
	int _layoutHeight = 0;

};

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Info::Controller*> controller,
	Fn<void()> resumeAll)
: RpWidget(parent)
, _controller(controller)
, _nameStyle(MakeNameStyle())
, _actionBar(
	[=] { Core::App().downloadManager().pauseAll(); },
	std::move(resumeAll),
	[=] { update(); }) {
	setMouseTracking(true);

	_rebuildTimer.setCallback([=] { rebuild(); });

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
		// WHY: loadedAdded() fires once per resolved entry, so rebuilding on
		// every event re-runs the whole resolve() (disk + MTProto) N times.
		_rebuildTimer.callOnce(crl::time(0));
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

void InnerWidget::setSearchQuery(const QString &query) {
	if (_query == query) {
		return;
	}
	_query = query;
	rebuild();
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
		row.itemId = id->object.item->fullId();
		row.title = TitleFor(id->path, DocumentName(&id->object));
		row.titleText.setText(_nameStyle, row.title);
		row.thumb = ThumbnailFor(id->object, _thumbs);
		row.status = JoinStatus(
			SourceText(id->object.item->history()->peer->name()),
			ProgressText(id->ready, id->total));
		row.source = id->object.item->history()->peer->name();
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
		row.itemId = FullMsgId(
			PeerId(descriptor->itemIdPeer),
			MsgId(descriptor->itemIdMsg));
		row.status = JoinStatus(
			Tr(tr::lng_downloads_big_paused(tr::now), u"已暂停"_q),
			JoinStatus(
				SourceText(descriptor->sourceTitle),
				ProgressText(row.ready, row.total)));
		row.source = descriptor->sourceTitle;
		paused.push_back(std::move(row));
	}

	auto completed = std::vector<Row>();
	auto totalFiles = int64(0);
	auto totalSize = int64(0);
	auto videoFiles = int64(0);
	auto videoSize = int64(0);
	auto imageFiles = int64(0);
	auto imageSize = int64(0);
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
		row.source = PeerTitleOf(*id);
		row.status = JoinStatus(
			SizeText(id->size),
			SourceText(row.source));
		++totalFiles;
		totalSize += id->size;
		const auto document = id->object ? id->object->document : nullptr;
		if (document
			&& (document->isVideoMessage() || document->isVideoFile())) {
			++videoFiles;
			videoSize += id->size;
		} else if (document && document->isImage()) {
			++imageFiles;
			imageSize += id->size;
		}
		completed.push_back(std::move(row));
	}
	std::sort(completed.begin(), completed.end(), [](const Row &a, const Row &b) {
		return a.started > b.started;
	});

	if (!completed.empty()) {
		auto stats = Row();
		stats.kind = Row::Kind::Stats;
		stats.cards = {
			{
				&st::downloadsBigStatsIconTotal,
				Tr(u"Total"_q, u"全部"_q),
				Tr(u"%1 files  %2"_q, u"%1 个文件  %2"_q)
					.arg(totalFiles)
					.arg(SizeText(totalSize)),
			},
			{
				&st::downloadsBigStatsIconVideo,
				Tr(u"Videos"_q, u"视频"_q),
				Tr(u"%1 files  %2"_q, u"%1 个文件  %2"_q)
					.arg(videoFiles)
					.arg(SizeText(videoSize)),
			},
			{
				&st::downloadsBigStatsIconImage,
				Tr(u"Photos"_q, u"图片"_q),
				Tr(u"%1 files  %2"_q, u"%1 个文件  %2"_q)
					.arg(imageFiles)
					.arg(SizeText(imageSize)),
			},
		};
		result.push_back(std::move(stats));
	}
	if (!_query.isEmpty()) {
		const auto filter = [&](std::vector<Row> &list) {
			auto kept = std::vector<Row>();
			for (auto &row : list) {
				if (MatchesQuery(row, _query)) {
					kept.push_back(std::move(row));
				}
			}
			list = std::move(kept);
		};
		filter(loading);
		filter(paused);
		filter(completed);
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
		if (activeCount > 0) {
			auto bar = Row();
			bar.kind = Row::Kind::ActionBar;
			result.push_back(std::move(bar));
			_downloadingBegin = int(result.size());
		}
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
	if (_downloadingCollapsed || !_query.isEmpty()) {
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
		if (row.kind == Row::Kind::ActionBar) {
			row.geometry = QRect(
				0,
				top,
				newWidth,
				st::downloadsBigBarHeight);
			top += row.geometry.height();
			continue;
		}
		if (row.kind == Row::Kind::Stats) {
			top += st::downloadsBigHeaderTop;
			row.geometry = QRect(
				0,
				top,
				newWidth,
				st::downloadsBigStatsHeight);
			top += row.geometry.height() + st::downloadsBigHeaderBottom;
			continue;
		}
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
		const auto hasFolder = (row.kind == Row::Kind::Completed);
		const auto textLeft = padding.left()
			+ thumbSize
			+ st::downloadsBigThumbSkip;
		const auto actionWidth = (hasAction || hasFolder)
			? (st::downloadsBigActionSize + st::downloadsBigActionSkip)
			: 0;
		const auto textWidth = std::max(
			newWidth - padding.right() - actionWidth - textLeft,
			0);
		// WHY: a long unbroken name is split into per-grapheme words only
		// when the parser gets a real resize width; the default kQFixedMax
		// keeps it on one line, so the status lands on the second painted line.
		if (row.titleWidth != textWidth) {
			row.titleWidth = textWidth;
			row.titleText = Ui::Text::String(
				_nameStyle,
				row.title,
				kDefaultTextOptions,
				textWidth);
		}
		row.titleHeight = row.titleText.countHeight(textWidth, true);
		const auto statusHeight = st::downloadsBigStatusFont->height;
		const auto hasProgress = (row.kind == Row::Kind::Downloading)
			|| (row.kind == Row::Kind::Paused);
		const auto progressSpace = hasProgress
			? (st::downloadsBigProgressHeight + st::downloadsBigProgressTop)
			: 0;
		const auto contentHeight = row.titleHeight
			+ st::downloadsBigStatusTop
			+ statusHeight
			+ progressSpace;
		const auto innerHeight = std::max(thumbSize, contentHeight);
		const auto rowHeight = innerHeight + padding.top() + padding.bottom();
		row.geometry = QRect(0, top, newWidth, rowHeight);
		row.text = QRect(
			textLeft,
			top + padding.top() + (innerHeight - contentHeight) / 2,
			textWidth,
			row.titleHeight);
		row.statusRect = QRect(
			textLeft,
			row.text.y() + row.titleHeight + st::downloadsBigStatusTop,
			textWidth,
			statusHeight);
		row.progress = hasProgress
			? QRect(
				textLeft,
				row.statusRect.y() + statusHeight
					+ st::downloadsBigProgressTop,
				textWidth,
				st::downloadsBigProgressHeight)
			: QRect();
		const auto actionTop = top
			+ (rowHeight - st::downloadsBigActionSize) / 2;
		const auto actionLeft = newWidth
			- padding.right()
			- st::downloadsBigActionSize;
		const auto actionGeometry = QRect(
			actionLeft,
			actionTop,
			st::downloadsBigActionSize,
			st::downloadsBigActionSize);
		row.action = hasAction ? actionGeometry : QRect();
		row.folder = hasFolder ? actionGeometry : QRect();
		const auto badgeSize = st::downloadsBigThumbBadgeSize;
		const auto thumbTop = top + (rowHeight - thumbSize) / 2;
		row.badge = hasAction
			? QRect(
				padding.left() + (thumbSize - badgeSize) / 2,
				thumbTop + (thumbSize - badgeSize) / 2,
				badgeSize,
				badgeSize)
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
	if (!row.folder.isEmpty() && row.folder.contains(_lastClick)) {
		showInFolder(row);
		return;
	}
	if (!row.clickable()) {
		return;
	}
	jumpToRow(row);
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

void InnerWidget::showInFolder(const Row &row) const {
	if (row.path.isEmpty()) {
		return;
	}
	File::ShowInFolder(row.path);
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
		// WHY: the loaded entries only appear once resolve() has filled them,
		// so an empty list right after opening means "still loading".
		const auto loading = _query.isEmpty()
			&& (Core::App().downloadManager().loadedCount() > 0);
		p.setFont(st::downloadsBigEmptyFont);
		p.setPen(st::downloadsBigEmptyFg);
		p.drawText(
			QRect(
				0,
				st::downloadsBigEmptyTop,
				width(),
				st::downloadsBigEmptyFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			loading
				? Tr(
					tr::lng_downloads_big_loading(tr::now),
					u"正在读取下载记录…"_q)
				: (_query.isEmpty()
					? Tr(
						tr::lng_downloads_big_empty(tr::now),
						u"还没有下载任务"_q)
					: Tr(u"No matches"_q, u"没有匹配的任务"_q)));
		return;
	}
	const auto clip = e->rect();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		if (!row.geometry.intersects(clip)) {
			continue;
		}
		if (row.kind == Row::Kind::ActionBar) {
			_actionBar.paintIn(p, row.geometry);
			continue;
		}
		if (row.kind == Row::Kind::Stats) {
			paintStats(p, row);
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

void InnerWidget::paintStats(Painter &p, const Row &row) const {
	const auto count = int(row.cards.size());
	if (!count) {
		return;
	}
	const auto outer = st::downloadsBigPadding.left();
	const auto inner = std::max(row.geometry.width() - 2 * outer, 0);
	const auto cardWidth = std::max(
		(inner - (count - 1) * st::downloadsBigStatsSkip) / count,
		0);
	const auto padding = st::downloadsBigStatsPadding;
	const auto iconSize = st::downloadsBigStatsIconSize;
	const auto labelHeight = st::downloadsBigStatsLabelFont->height;
	const auto valueHeight = st::downloadsBigStatsValueFont->height;
	const auto contentHeight = labelHeight
		+ st::downloadsBigStatsLabelSkip
		+ valueHeight;
	auto left = row.geometry.left() + outer;
	for (const auto &card : row.cards) {
		const auto rect = QRect(
			left,
			row.geometry.top(),
			cardWidth,
			row.geometry.height());
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::downloadsBigStatsBg);
			p.drawRoundedRect(
				rect,
				st::downloadsBigStatsRadius,
				st::downloadsBigStatsRadius);
		}
		card.icon->paintInCenter(p, QRect(
			rect.x() + padding.left(),
			rect.y() + (rect.height() - iconSize.height()) / 2,
			iconSize.width(),
			iconSize.height()));
		const auto textLeft = rect.x() + padding.left() + iconSize.width()
			+ st::downloadsBigStatsIconSkip;
		const auto textWidth = rect.right() - padding.right() - textLeft;
		const auto textTop = rect.y() + (rect.height() - contentHeight) / 2;
		p.setFont(st::downloadsBigStatsLabelFont);
		p.setPen(st::downloadsBigStatsLabelFg);
		p.drawText(
			QRect(textLeft, textTop, textWidth, labelHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::downloadsBigStatsLabelFont->elided(card.label, textWidth));
		p.setFont(st::downloadsBigStatsValueFont);
		p.setPen(st::downloadsBigStatsValueFg);
		p.drawText(
			QRect(
				textLeft,
				textTop + labelHeight + st::downloadsBigStatsLabelSkip,
				textWidth,
				valueHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::downloadsBigStatsValueFont->elided(card.value, textWidth));
		left += cardWidth + st::downloadsBigStatsSkip;
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
	if (!row.badge.isEmpty()) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigThumbBadgeBg);
		p.drawRoundedRect(
			row.badge,
			st::downloadsBigThumbBadgeRadius,
			st::downloadsBigThumbBadgeRadius);
		const auto &icon = (row.kind == Row::Kind::Downloading)
			? st::downloadsBigActionPause
			: st::downloadsBigActionResume;
		icon.paintInCenter(p, row.badge);
	}

	p.setPen(st::downloadsBigNameFg);
	DrawTitle(p, row.titleText, row.text);

	p.setFont(st::downloadsBigStatusFont);
	p.setPen(st::downloadsBigStatusFg);
	p.drawText(
		row.statusRect,
		Qt::AlignLeft | Qt::AlignVCenter,
		st::downloadsBigStatusFont->elided(row.status, row.text.width()));

	if (!row.progress.isNull()) {
		const auto progress = (row.total > 0)
			? std::clamp(double(row.ready) / double(row.total), 0., 1.)
			: 0.;
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigProgressBg);
		p.drawRoundedRect(
			row.progress,
			st::downloadsBigProgressHeight / 2.,
			st::downloadsBigProgressHeight / 2.);
		if (progress > 0.) {
			auto filled = row.progress;
			filled.setWidth(std::max(
				int(base::SafeRound(row.progress.width() * progress)),
				st::downloadsBigProgressHeight));
			p.setBrush(st::downloadsBigProgressFg);
			p.drawRoundedRect(
				filled,
				st::downloadsBigProgressHeight / 2.,
				st::downloadsBigProgressHeight / 2.);
		}
	}

	const auto &action = row.action.isEmpty() ? row.folder : row.action;
	if (action.isEmpty()) {
		return;
	}
	if (hovered) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigActionBgOver);
		p.drawRoundedRect(
			action,
			st::downloadsBigActionRadius,
			st::downloadsBigActionRadius);
	}
	if (!row.action.isEmpty()) {
		const auto &icon = (row.kind == Row::Kind::Downloading)
			? st::downloadsBigActionPause
			: st::downloadsBigActionResume;
		icon.paintInCenter(p, action);
	} else {
		st::menuIconShowInFolder.paintInCenter(p, action);
	}
}

void InnerWidget::mouseMoveEvent(QMouseEvent *e) {
	const auto previous = _hovered;
	const auto index = rowAt(e->pos());
	const auto hovered = (index >= 0 && _rows[index].hoverable())
		? index
		: -1;
	if (hovered != _hovered) {
		_hovered = hovered;
		updateRow(previous);
		updateRow(_hovered);
	}
	const auto overBar = (index >= 0)
		&& (_rows[index].kind == Row::Kind::ActionBar);
	if (overBar) {
		_actionBar.mouseMove(e->pos(), _rows[index].geometry);
	} else {
		_actionBar.leave();
	}
	setCursor((overBar ? _actionBar.isHovered() : (_hovered >= 0))
		? style::cur_pointer
		: style::cur_default);
}

void InnerWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = rowAt(e->pos());
	if (index < 0 || _rows[index].kind != Row::Kind::ActionBar) {
		return;
	}
	_actionBar.mousePress(e->pos(), _rows[index].geometry);
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
	if (_rows[index].kind == Row::Kind::ActionBar) {
		_actionBar.mouseRelease(e->pos(), _rows[index].geometry);
		return;
	}
	activateRow(index);
}

void InnerWidget::leaveEventHook(QEvent *) {
	const auto previous = _hovered;
	_hovered = -1;
	_actionBar.leave();
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
	auto inner = object_ptr<InnerWidget>(
		this,
		controller,
		[=] { resumeAllPaused(); });
	const auto raw = inner.get();
	setInnerWidget(std::move(inner));

	if (controller->searchFieldController()) {
		controller->setSearchEnabledByContent(true);
		controller->searchQueryValue(
		) | rpl::on_next([=](const QString &query) {
			raw->setSearchQuery(query);
		}, lifetime());
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
	const auto result = Core::App().downloadManager().importPausedJson(
		json,
		&controller()->session());
	if (!result) {
		Ui::Toast::Show(Tr(
			tr::lng_downloads_import_failed(tr::now),
			u"这个文件不是下载列表。"_q));
		return;
	}
	auto text = TextWithEntities();
	const auto append = [&](const QString &plain) {
		text.text += plain;
	};
	const auto appendBold = [&](const QString &plain) {
		const auto offset = int(text.text.size());
		text.text += plain;
		text.entities.push_back(EntityInText(
			EntityType::Semibold,
			offset,
			int(plain.size())));
	};
	append(Tr(u"Backup: "_q, u"备份文件："_q));
	appendBold(QString::number(result->downloading));
	append(Tr(u" in progress, "_q, u" 条下载中，"_q));
	appendBold(QString::number(result->completed));
	append(Tr(u" downloaded."_q, u" 条已下载。"_q));
	if (!result->finishedSkipped.empty()) {
		append(u"\n\n"_q);
		appendBold(Tr(
			u"Already on disk, skipped:"_q,
			u"已下载、未计入下载中："_q));
		for (auto i = 0; i != int(result->finishedSkipped.size()); ++i) {
			append(u"\n%1. %2"_q
				.arg(i + 1)
				.arg(result->finishedSkipped[i]));
		}
	}
	Ui::show(Ui::MakeInformBox(std::move(text)));
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
