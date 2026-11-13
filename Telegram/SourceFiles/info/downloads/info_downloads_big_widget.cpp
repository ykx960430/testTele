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
#include "data/data_download_manager.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "base/algorithm.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "styles/style_info_downloads_big.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Info::DownloadsBig {
namespace {

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
	QString status;
	int64 ready = 0;
	int64 total = 0;
	Data::PausedDownloadDescriptor paused;
	QString path;
	QRect geometry;

	[[nodiscard]] bool clickable() const {
		// Photos are only tracked, their download is neither paused nor
		// resumable, so a paused photo row must not look clickable.
		return (kind == Kind::Completed)
			|| (kind == Kind::Paused
				&& paused.downloadType == Data::DownloadType::Document);
	}
	[[nodiscard]] bool small() const {
		return (kind == Kind::Header) || (kind == Kind::Stats);
	}
};

[[nodiscard]] QString ObjectName(const Data::DownloadObject &object) {
	const auto document = object.document;
	return document ? document->filename() : QString();
}

[[nodiscard]] QString ProgressText(int64 ready, int64 total) {
	return (total > 0)
		? tr::lng_downloads_big_progress(
			tr::now,
			lt_ready,
			Ui::FormatSizeText(ready),
			lt_total,
			Ui::FormatSizeText(total))
		: Ui::FormatSizeText(ready);
}

} // namespace

namespace {

class InnerWidget final : public Ui::RpWidget {
public:
	explicit InnerWidget(QWidget *parent);

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
	void paintSmall(Painter &p, const Row &row) const;
	void paintItem(Painter &p, const Row &row) const;
	void paintEmpty(Painter &p) const;

	std::vector<Row> _rows;
	int _downloadingBegin = 0;
	int _downloadingCount = 0;
	int _hovered = -1;
	bool _rebuilding = false;

};

InnerWidget::InnerWidget(QWidget *parent)
: RpWidget(parent) {
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

	const auto addHeader = [&](QString title, int count) {
		if (!count) {
			return;
		}
		auto row = Row();
		row.kind = Row::Kind::Header;
		row.title = std::move(title)
			+ u" ("_q
			+ QString::number(count)
			+ u")"_q;
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
		row.title = ObjectName(id->object);
		if (row.title.isEmpty()) {
			row.title = QFileInfo(id->path).fileName();
		}
		row.status = ProgressText(id->ready, id->total);
		loading.push_back(std::move(row));
	}

	auto paused = std::vector<Row>();
	for (const auto descriptor : manager.pausedList()) {
		auto row = Row();
		row.kind = Row::Kind::Paused;
		row.paused = *descriptor;
		row.title = descriptor->displayName;
		if (row.title.isEmpty()) {
			row.title = QFileInfo(descriptor->path).fileName();
		}
		if (row.title.isEmpty()) {
			row.title = tr::lng_downloads_big_paused(tr::now);
		}
		row.ready = descriptor->ready;
		row.total = descriptor->total;
		row.status = ProgressText(row.ready, row.total);
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
		row.title = ObjectName(*id->object);
		if (row.title.isEmpty()) {
			row.title = QFileInfo(id->path).fileName();
		}
		row.status = Ui::FormatSizeText(id->size);
		totalSize += id->size;
		completed.push_back(std::move(row));
	}

	if (!completed.empty()) {
		auto stats = Row();
		stats.kind = Row::Kind::Stats;
		stats.title = tr::lng_downloads_big_total(
			tr::now,
			lt_size,
			Ui::FormatSizeText(totalSize));
		result.push_back(std::move(stats));
	}
	addHeader(tr::lng_downloads_big_downloading(tr::now), int(loading.size()));
	_downloadingBegin = int(result.size());
	_downloadingCount = int(loading.size());
	for (auto &row : loading) {
		result.push_back(std::move(row));
	}
	addHeader(tr::lng_downloads_big_paused(tr::now), int(paused.size()));
	for (auto &row : paused) {
		result.push_back(std::move(row));
	}
	addHeader(tr::lng_downloads_big_completed(tr::now), int(completed.size()));
	for (auto &row : completed) {
		result.push_back(std::move(row));
	}

	_rows = std::move(result);
}

void InnerWidget::updateProgress() {
	auto &manager = Core::App().downloadManager();
	auto index = _downloadingBegin;
	auto count = 0;
	for (const auto id : manager.loadingList()) {
		if (id->done) {
			continue;
		}
		if (count++ >= _downloadingCount) {
			rebuild();
			return;
		}
		auto &row = _rows[index++];
		row.ready = id->ready;
		row.total = id->total;
		row.status = ProgressText(row.ready, row.total);
	}
	if (count != _downloadingCount) {
		rebuild();
		return;
	}
	if (!_downloadingCount) {
		return;
	}
	// WHY: only the downloading band changed, repainting the whole list would
	// also touch the completed rows on every progress tick.
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
		} else {
			row.geometry = QRect(0, top, newWidth, st::downloadsBigRowHeight);
			top += row.geometry.height();
		}
	}
	return top + st::downloadsBigPadding.bottom();
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

void InnerWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_rows.empty()) {
		paintEmpty(p);
		return;
	}
	const auto clip = e->rect();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		if (!row.geometry.intersects(clip)) {
			continue;
		}
		if (_hovered == i && row.clickable()) {
			p.fillRect(row.geometry, st::downloadsBigRowBgOver);
		}
		if (row.small()) {
			paintSmall(p, row);
		} else {
			paintItem(p, row);
		}
	}
}

void InnerWidget::paintEmpty(Painter &p) const {
	p.setFont(st::downloadsBigEmptyFont);
	p.setPen(st::downloadsBigEmptyFg);
	p.drawText(
		QRect(
			0,
			st::downloadsBigEmptyTop,
			width(),
			st::downloadsBigEmptyFont->height),
		Qt::AlignHCenter | Qt::AlignVCenter,
		tr::lng_downloads_big_empty(tr::now));
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

void InnerWidget::paintItem(Painter &p, const Row &row) const {
	const auto &padding = st::downloadsBigPadding;
	const auto iconSize = st::downloadsBigIconSize;

	auto iconRect = QRect(
		row.geometry.left() + padding.left(),
		row.geometry.top() + (row.geometry.height() - iconSize) / 2,
		iconSize,
		iconSize);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::downloadsBigIconBg);
		p.drawRoundedRect(
			iconRect,
			st::downloadsBigIconRadius,
			st::downloadsBigIconRadius);
	}
	st::downloadsBigItemIcon.paintInCenter(p, iconRect);

	const auto textLeft = iconRect.left() + iconSize + st::downloadsBigIconSkip;
	const auto textWidth = std::max(
		row.geometry.width() - padding.right() - textLeft,
		0);
	const auto nameHeight = st::downloadsBigNameFont->height;
	const auto statusHeight = st::downloadsBigStatusFont->height;
	const auto barHeight = st::downloadsBigProgressHeight;
	const auto textHeight = nameHeight
		+ st::downloadsBigStatusTop
		+ statusHeight;

	auto progress = double(-1.);
	if ((row.kind == Row::Kind::Downloading)
		|| (row.kind == Row::Kind::Paused)) {
		if (row.total > 0) {
			progress = std::clamp(
				double(row.ready) / double(row.total),
				0.,
				1.);
		}
	}
	const auto barSpace = (progress >= 0.)
		? (barHeight + st::downloadsBigProgressTop)
		: 0;
	const auto contentHeight = row.geometry.height()
		- padding.top()
		- padding.bottom();
	const auto textTop = row.geometry.top()
		+ padding.top()
		+ (contentHeight - textHeight - barSpace) / 2;

	p.setFont(st::downloadsBigNameFont);
	p.setPen(st::downloadsBigNameFg);
	p.drawText(
		QRect(textLeft, textTop, textWidth, nameHeight),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::downloadsBigNameFont->elided(
			row.title,
			textWidth,
			Qt::ElideMiddle));

	p.setFont(st::downloadsBigStatusFont);
	p.setPen(st::downloadsBigStatusFg);
	p.drawText(
		QRect(
			textLeft,
			textTop + nameHeight + st::downloadsBigStatusTop,
			textWidth,
			statusHeight),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::downloadsBigStatusFont->elided(row.status, textWidth));

	if (progress < 0.) {
		return;
	}
	const auto barRect = QRect(
		textLeft,
		textTop + textHeight + st::downloadsBigProgressTop,
		textWidth,
		barHeight);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::downloadsBigProgressBg);
	p.drawRoundedRect(barRect, barHeight / 2., barHeight / 2.);
	if (progress > 0.) {
		auto filled = barRect;
		filled.setWidth(std::min(
			barRect.width(),
			std::max(
				int(base::SafeRound(barRect.width() * progress)),
				barHeight)));
		p.setBrush(st::downloadsBigProgressFg);
		p.drawRoundedRect(filled, barHeight / 2., barHeight / 2.);
	}
}

void InnerWidget::mouseMoveEvent(QMouseEvent *e) {
	const auto previous = _hovered;
	const auto index = rowAt(e->pos());
	const auto hovered = (index >= 0 && _rows[index].clickable())
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
	const auto index = rowAt(e->pos());
	if (index < 0 || !_rows[index].clickable()) {
		return;
	}
	const auto &row = _rows[index];
	if (row.kind == Row::Kind::Paused) {
		if (!Core::App().downloadManager().resumeOne(row.paused)) {
			Ui::Toast::Show(this, tr::lng_downloads_resume_failed(tr::now));
		}
	} else if (row.kind == Row::Kind::Completed) {
		File::ShowInFolder(row.path);
	}
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
	setInnerWidget(object_ptr<InnerWidget>(this));
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
			tr::lng_downloads_pause_all(tr::now),
			[=] { Core::App().downloadManager().pauseAll(); },
			&st::menuIconDownload);
	}
	if (manager.pausedCount() > 0) {
		addAction(
			tr::lng_downloads_resume_all(tr::now),
			[=] { resumeAllPaused(); },
			&st::menuIconDownload);
	}
	addAction(
		tr::lng_downloads_export_list(tr::now),
		[=] { exportPausedList(); },
		&st::menuIconExport);
	addAction(
		tr::lng_downloads_import_list(tr::now),
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
		Ui::Toast::Show(this, tr::lng_downloads_resume_failed(tr::now));
	}
}

void Widget::exportPausedList() {
	auto &manager = Core::App().downloadManager();
	const auto json = manager.exportPausedJson();
	if (json.isEmpty()) {
		Ui::Toast::Show(this, tr::lng_downloads_export_empty(tr::now));
		return;
	}
	const auto count = manager.pausedCount();
	const auto data = json.toUtf8();
	auto filter = u"JSON (*.json);;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetWritePath(
		this,
		tr::lng_downloads_export_list(tr::now),
		filter,
		filedialogDefaultName(u"downloads"_q, u".json"_q),
		crl::guard(this, [=](const QString &path) {
			if (path.isEmpty()) {
				return;
			}
			auto file = QFile(path);
			const auto written = file.open(QIODevice::WriteOnly)
				&& (file.write(data) == data.size());
			file.close();
			Ui::Toast::Show(this, written
				? tr::lng_downloads_export_done(tr::now, lt_count, count)
				: tr::lng_downloads_export_failed(tr::now));
		}));
}

void Widget::importPausedList() {
	auto filter = u"JSON (*.json);;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetOpenPath(
		this,
		tr::lng_downloads_import_list(tr::now),
		filter,
		crl::guard(this, [=](FileDialog::OpenResult result) {
			if (!result.paths.isEmpty()) {
				auto file = QFile(result.paths.front());
				if (file.open(QIODevice::ReadOnly)) {
					importPausedListDone(QString::fromUtf8(file.readAll()));
				} else {
					Ui::Toast::Show(
						this,
						tr::lng_downloads_import_failed(tr::now));
				}
			} else if (!result.remoteContent.isEmpty()) {
				importPausedListDone(QString::fromUtf8(result.remoteContent));
			}
		}));
}

void Widget::importPausedListDone(const QString &json) {
	const auto added = Core::App().downloadManager().importPausedJson(json);
	Ui::Toast::Show(this, !added.has_value()
		? tr::lng_downloads_import_failed(tr::now)
		: (*added
			? tr::lng_downloads_import_done(tr::now, lt_count, *added)
			: tr::lng_downloads_import_empty(tr::now)));
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
