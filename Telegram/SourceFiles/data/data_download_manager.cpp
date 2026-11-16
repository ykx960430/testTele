/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_download_manager.h"

#include "data/data_session.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_web_page.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_channel.h"
#include "data/data_file_origin.h"
#include "base/debug_log.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "lang/lang_keys.h"
#include "storage/storage_account.h"
#include "iv/iv_rich_page.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "core/application.h"
#include "core/mime_type.h"
#include "ui/controls/download_bar.h"
#include "ui/text/format_song_document_name.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "storage/serialize_common.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "styles/style_layers.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Data {
namespace {

constexpr auto kClearLoadingTimeout = 5 * crl::time(1000);
constexpr auto kMaxFileSize = 4000 * int64(1024 * 1024);
constexpr auto kMaxResolvePerAttempt = 100;

constexpr auto ByItem = [](const auto &entry) {
	if constexpr (std::is_same_v<decltype(entry), const DownloadingId&>) {
		return entry.object.item;
	} else {
		const auto resolved = entry.object.get();
		return resolved ? resolved->item.get() : nullptr;
	}
};

constexpr auto ByDocument = [](const auto &entry) {
	return entry.object.document;
};

[[nodiscard]] uint64 PeerAccessHash(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->accessHash();
	} else if (const auto channel = peer->asChannel()) {
		return channel->accessHash();
	}
	return 0;
}

[[nodiscard]] bool ItemContainsMedia(const DownloadObject &object) {
	if (const auto iv = object.item->Get<HistoryMessageMediaForInstantView>()) {
		if (object.document && iv->documents.contains(object.document)) {
			return true;
		} else if (object.photo && iv->photos.contains(object.photo)) {
			return true;
		}
	}
	if (const auto photo = object.photo) {
		if (const auto media = object.item->media()) {
			if (const auto page = media->webpage()) {
				if (page->photo == photo) {
					return true;
				}
				for (const auto &item : page->collage.items) {
					if (const auto v = std::get_if<PhotoData*>(&item)) {
						if ((*v) == photo) {
							return true;
						}
					}
				}
			} else {
				return (media->photo() == photo);
			}
		}
	} else if (const auto document = object.document) {
		if (const auto media = object.item->media()) {
			if (const auto page = media->webpage()) {
				if (page->document == document) {
					return true;
				}
				for (const auto &item : page->collage.items) {
					if (const auto v = std::get_if<DocumentData*>(&item)) {
						if ((*v) == document) {
							return true;
						}
					}
				}
			} else {
				return (media->document() == document);
			}
		}
	}
	return false;
}

[[nodiscard]] PausedDownloadDescriptor MakePausedDescriptor(
	const DownloadObject &object,
	const QString &path,
	int64 ready,
	int64 total,
	int64 started,
	PauseReason reason) {
	auto descriptor = PausedDownloadDescriptor();
	descriptor.path = path;
	descriptor.ready = ready;
	descriptor.total = total;
	descriptor.started = started;
	descriptor.reason = reason;
	if (const auto document = object.document) {
		descriptor.documentId = document->id;
		descriptor.downloadType = DownloadType::Document;
	} else if (const auto photo = object.photo) {
		descriptor.photoId = photo->id;
		descriptor.downloadType = DownloadType::Photo;
	}
	const auto item = object.item;
	descriptor.itemIdPeer = item->history()->peer->id.value;
	descriptor.itemIdMsg = item->id.bare;
	descriptor.peerAccessHash = PeerAccessHash(item->history()->peer);
	descriptor.sourceTitle = item->history()->peer->name();
	descriptor.displayName = QFileInfo(path).fileName();
	if (descriptor.displayName.isEmpty() && object.document) {
		descriptor.displayName = object.document->filename();
	}
	return descriptor;
}

[[nodiscard]] QByteArray SerializePausedDownloads(
	const std::vector<PausedDownloadDescriptor> &list) {
	auto result = QByteArray();
	const auto count = list.size();
	auto constant = sizeof(quint64) // documentId
		+ sizeof(quint64) // photoId
		+ sizeof(qint64) // itemIdPeer
		+ sizeof(qint64) // itemIdMsg
		+ sizeof(quint64) // peerAccessHash
		+ sizeof(qint32) // downloadType
		+ sizeof(qint64) // ready
		+ sizeof(qint64) // total
		+ sizeof(qint64) // started
		+ sizeof(qint32); // reason
	auto size = sizeof(qint32) // version
		+ sizeof(qint32) // count
		+ count * constant;
	for (const auto &d : list) {
		size += Serialize::stringSize(d.path);
		size += Serialize::stringSize(d.displayName);
		size += Serialize::stringSize(d.sourceTitle);
	}
	result.reserve(size);
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << qint32(count);
	for (const auto &d : list) {
		stream
			<< quint64(d.documentId)
			<< quint64(d.photoId)
			<< qint64(d.itemIdPeer)
			<< qint64(d.itemIdMsg)
			<< quint64(d.peerAccessHash)
			<< qint32(d.downloadType)
			<< qint64(d.ready)
			<< qint64(d.total)
			<< qint64(d.started)
			<< qint32(d.reason)
			<< d.path
			<< d.displayName
			<< d.sourceTitle;
	}
	stream.device()->close();
	return result;
}

[[nodiscard]] bool DeserializePausedDownload(
	QDataStream &stream,
	PausedDownloadDescriptor &d) {
	auto reason = qint32();
	auto downloadType = qint32();
	stream
		>> d.documentId
		>> d.photoId
		>> d.itemIdPeer
		>> d.itemIdMsg
		>> d.peerAccessHash
		>> downloadType
		>> d.ready
		>> d.total
		>> d.started
		>> reason
		>> d.path
		>> d.displayName
		>> d.sourceTitle;
	d.downloadType = DownloadType(downloadType);
	d.reason = PauseReason(reason);
	return stream.status() == QDataStream::Ok;
}

[[nodiscard]] bool SamePausedRecord(
	const PausedDownloadDescriptor &a,
	const PausedDownloadDescriptor &b) {
	return (a.documentId == b.documentId)
		&& (a.itemIdPeer == b.itemIdPeer)
		&& (a.itemIdMsg == b.itemIdMsg);
}

struct DocumentDescriptor {
	uint64 sessionUniqueId = 0;
	DocumentId documentId = 0;
	FullMsgId itemId;
};

[[nodiscard]] QString RichExportFolderToRemove(const QString &filePath) {
	if (!filePath.endsWith(u".html"_q, Qt::CaseInsensitive)) {
		return QString();
	}
	const auto info = QFileInfo(filePath);
	const auto folder = info.absolutePath();
	if (QDir(folder).dirName() != info.completeBaseName()) {
		return QString();
	}
	auto file = QFile(filePath);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto head = QString::fromUtf8(file.read(2048));
	if (!head.contains(Iv::RichExportGeneratorMarker())) {
		return QString();
	}
	const auto entries = QDir(folder).entryList(
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const auto &entry : entries) {
		if ((entry != info.fileName()) && (entry != u"media"_q)) {
			return QString();
		}
	}
	return folder;
}

} // namespace

struct DownloadManager::DeleteFilesDescriptor {
	base::flat_set<not_null<Main::Session*>> sessions;
	base::flat_map<QString, DocumentDescriptor> files;
};

DownloadManager::DownloadManager()
: _clearLoadingTimer([=] { clearLoading(); }) {
}

DownloadManager::~DownloadManager() = default;

bool DownloadManager::empty() const {
	for (const auto &[session, data] : _sessions) {
		if (!data.downloading.empty() || !data.downloaded.empty()) {
			return false;
		}
	}
	return true;
}

void DownloadManager::trackSession(not_null<Main::Session*> session) {
	auto &data = _sessions.emplace(session, SessionData()).first->second;
	data.downloaded = deserialize(session);
	data.resolveNeeded = data.downloaded.size();
	deserializePaused(
		session->account().local().pausedDownloadsSerialized());
	// WHY: a crash skips the pause-on-quit chain, so the still running
	// downloads are only recoverable from their own snapshot. The snapshot is
	// one global list, so restore it once instead of duplicating per account.
	if (!_inflightRestored) {
		_inflightRestored = true;
		deserializeInflight(
			session->account().local().inflightDownloadsSerialized());
	}
	validatePausedDownloads();

	session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> document) {
		return _loadingDocuments.contains(document);
	}) | rpl::on_next([=](not_null<DocumentData*> document) {
		check(document);
	}, data.lifetime);

	session->data().itemLayoutChanged(
	) | rpl::filter([=](not_null<const HistoryItem*> item) {
		return _loading.contains(item);
	}) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		check(item);
	}, data.lifetime);

	session->data().itemViewRefreshRequest(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		changed(item);
	}, data.lifetime);

	session->changes().messageUpdates(
		MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const MessageUpdate &update) {
		removed(update.item);
	}, data.lifetime);

	session->account().sessionChanges(
	) | rpl::filter(
		rpl::mappers::_1 != session
	) | rpl::take(1) | rpl::on_next([=] {
		untrack(session);
	}, data.lifetime);
}

void DownloadManager::itemVisibilitiesUpdated(
		not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	if (i == end(_sessions)
		|| i->second.downloading.empty()
		|| !i->second.downloading.front().hiddenByView) {
		return;
	}
	for (const auto &id : i->second.downloading) {
		if (!id.done
			&& !session->data().queryItemVisibility(id.object.item)) {
			for (auto &id : i->second.downloading) {
				id.hiddenByView = false;
			}
			_loadingListChanges.fire({});
			return;
		}
	}
}

int64 DownloadManager::computeNextStartDate() {
	const auto now = base::unixtime::now();
	if (_lastStartedBase != now) {
		_lastStartedBase = now;
		_lastStartedAdded = 0;
	} else {
		++_lastStartedAdded;
	}
	return int64(_lastStartedBase) * 1000 + _lastStartedAdded;
}

void DownloadManager::addLoading(DownloadObject object) {
	Expects(object.item != nullptr);
	Expects(object.document != nullptr);

	const auto item = object.item;
	auto &data = sessionData(item);

	const auto already = ranges::find(data.downloading, item, ByItem);
	if (already != end(data.downloading)) {
		const auto document = already->object.document;
		const auto photo = already->object.photo;
		if (document == object.document && photo == object.photo) {
			check(item);
			return;
		}
		remove(data, already);
	}

	const auto size = object.document->size;
	const auto path = object.document->loadingFilePath();
	if (path.isEmpty()) {
		return;
	}

	const auto shownExists = !data.downloading.empty()
		&& !data.downloading.front().hiddenByView;
	data.downloading.push_back({
		.object = object,
		.started = computeNextStartDate(),
		.path = path,
		.total = size,
		.hiddenByView = (!shownExists
			&& item->history()->owner().queryItemVisibility(item)),
	});
	_loading.emplace(item);
	_loadingDocuments.emplace(object.document);
	_loadingProgress = DownloadProgress{
		.ready = _loadingProgress.current().ready,
		.total = _loadingProgress.current().total + size,
	};
	_loadingListChanges.fire({});
	_clearLoadingTimer.cancel();
	writeInflightPostponed();

	check(item);
}

not_null<HistoryItem*> DownloadManager::generateExternalItem(
		not_null<DocumentData*> document) {
	_generatedDocuments.emplace(document);
	return generateFakeItem(document);
}

void DownloadManager::addLoadingExternal(
		DownloadObject object,
		const QString &path,
		int64 total,
		Fn<void()> cancel) {
	Expects(object.item != nullptr);
	Expects(object.document != nullptr);
	Expects(cancel != nullptr);

	const auto item = object.item;
	auto &data = sessionData(item);

	data.downloading.push_back({
		.object = object,
		.started = computeNextStartDate(),
		.path = path,
		.externalCancel = std::move(cancel),
		.total = total,
	});
	_loading.emplace(item);
	_loadingProgress = DownloadProgress{
		.ready = _loadingProgress.current().ready,
		.total = _loadingProgress.current().total + total,
	};
	_loadingListChanges.fire({});
	_clearLoadingTimer.cancel();
	writeInflightPostponed();
}

auto DownloadManager::lookupExternal(not_null<const HistoryItem*> item)
-> std::pair<SessionData*, std::vector<DownloadingId>::iterator> {
	const auto session = &item->history()->session();
	const auto i = _sessions.find(session);
	if (i == end(_sessions)) {
		return {};
	}
	const auto j = ranges::find(i->second.downloading, item, ByItem);
	if (j == end(i->second.downloading) || !j->externalCancel) {
		return {};
	}
	return { &i->second, j };
}

void DownloadManager::updateLoadingExternal(
		not_null<const HistoryItem*> item,
		int64 ready,
		int64 total) {
	const auto [data, i] = lookupExternal(item);
	if (!data || i->done) {
		return;
	}
	const auto readyChange = ready - i->ready;
	const auto totalChange = total - i->total;
	if (!readyChange && !totalChange) {
		return;
	}
	i->ready += readyChange;
	i->total += totalChange;
	_loadingProgress = DownloadProgress{
		.ready = _loadingProgress.current().ready + readyChange,
		.total = _loadingProgress.current().total + totalChange,
	};
	writeInflightPostponed();
	item->history()->owner().requestItemRepaint(item);
}

auto DownloadManager::loadingExternalState(
	not_null<const HistoryItem*> item)
-> std::optional<ExternalLoadingState> {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return std::nullopt;
	}
	return ExternalLoadingState{
		.ready = i->ready,
		.total = i->total,
		.done = i->done,
	};
}

void DownloadManager::cancelLoadingExternal(
		not_null<const HistoryItem*> item) {
	const auto [data, i] = lookupExternal(item);
	if (data) {
		cancel(*data, i);
	}
}

void DownloadManager::finishLoadingExternal(
		not_null<const HistoryItem*> item,
		const QString &path) {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return;
	}
	const auto object = i->object;
	const auto started = i->started;
	if (!i->done) {
		const auto readyChange = i->total - i->ready;
		i->ready += readyChange;
		i->done = true;
		_loading.remove(object.item);
		_loadingDone.emplace(object.item);
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total,
		};
		_loadingListChanges.fire({});
		if (_loading.empty()) {
			_clearLoadingTimer.callOnce(kClearLoadingTimeout);
		}
		item->history()->owner().requestItemRepaint(item);
	}
	addLoaded(object, path, started);
}

void DownloadManager::removeLoadingExternal(
		not_null<const HistoryItem*> item) {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return;
	}
	i->externalCancel = nullptr;
	remove(*data, i);
}

void DownloadManager::check(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	Assert(i != end(data.downloading));
	check(data, i);
}

void DownloadManager::check(not_null<DocumentData*> document) {
	auto &data = sessionData(document);
	const auto i = ranges::find(
		data.downloading,
		document.get(),
		ByDocument);
	Assert(i != end(data.downloading));
	check(data, i);
}

void DownloadManager::check(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	auto &entry = *i;

	if (entry.externalCancel) {
		return;
	} else if (!ItemContainsMedia(entry.object)) {
		cancel(data, i);
		return;
	}
	const auto document = entry.object.document;

	// Load with progress only documents for now.
	Assert(document != nullptr);

	const auto path = document->filepath(true);
	if (!path.isEmpty()) {
		if (_loading.contains(entry.object.item)) {
			addLoaded(entry.object, path, entry.started);
		}
	} else if (!document->loading()) {
		remove(data, i);
	} else {
		const auto totalChange = document->size - entry.total;
		const auto readyChange = document->loadOffset() - entry.ready;
		if (!readyChange && !totalChange) {
			return;
		}
		entry.ready += readyChange;
		entry.total += totalChange;
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total + totalChange,
		};
		writeInflightPostponed();
	}
}

void DownloadManager::addLoaded(
		DownloadObject object,
		const QString &path,
		DownloadDate started) {
	Expects(object.item != nullptr);
	Expects(object.document || object.photo);

	const auto size = QFileInfo(path).size();
	if (size <= 0 || size > kMaxFileSize) {
		return;
	}

	const auto item = object.item;
	auto &data = sessionData(item);

	const auto id = object.document
		? DownloadId{ object.document->id, DownloadType::Document }
		: DownloadId{ object.photo->id, DownloadType::Photo };
	data.downloaded.push_back({
		.download = id,
		.started = started,
		.path = path,
		.size = size,
		.itemId = item->fullId(),
		.peerAccessHash = PeerAccessHash(item->history()->peer),
		.object = std::make_unique<DownloadObject>(object),
	});
	_loaded.emplace(item);
	_loadedAdded.fire(&data.downloaded.back());

	writePostponed(&item->history()->session());

	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i != end(data.downloading)) {
		auto &entry = *i;
		const auto document = entry.object.document;
		if (document) {
			_loadingDocuments.remove(document);
		}
		const auto j = _loading.find(entry.object.item);
		if (j == end(_loading)) {
			return;
		}
		const auto totalChange = document->size - entry.total;
		const auto readyChange = document->size - entry.ready;
		entry.ready += readyChange;
		entry.total += totalChange;
		entry.done = true;
		_loading.erase(j);
		_loadingDone.emplace(entry.object.item);
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total + totalChange,
		};
		_loadingListChanges.fire({});
		if (_loading.empty()) {
			_clearLoadingTimer.callOnce(kClearLoadingTimeout);
		}
	}
}

void DownloadManager::clearIfFinished() {
	if (_clearLoadingTimer.isActive()) {
		_clearLoadingTimer.cancel();
		clearLoading();
	}
}

void DownloadManager::deleteFiles(const std::vector<GlobalMsgId> &ids) {
	auto descriptor = DeleteFilesDescriptor();
	for (const auto &id : ids) {
		if (const auto item = MessageByGlobalId(id)) {
			const auto session = &item->history()->session();
			const auto i = _sessions.find(session);
			if (i == end(_sessions)) {
				continue;
			}
			auto &data = i->second;
			const auto j = ranges::find(
				data.downloading,
				not_null{ item },
				ByItem);
			if (j != end(data.downloading)) {
				cancel(data, j);
			}

			// Addressed by item, and one item may own several downloaded
			// files, so every one of them goes. Removing just the first left
			// the rest on disk while loadedRemoved already hid the row.
			auto erased = false;
			for (auto k = 0; k != int(data.downloaded.size());) {
				if (ByItem(data.downloaded[k]) != item) {
					++k;
					continue;
				}
				const auto &entry = data.downloaded[k];
				const auto document = entry.object->document;
				descriptor.files.emplace(entry.path, DocumentDescriptor{
					.sessionUniqueId = id.sessionUniqueId,
					.documentId = document ? document->id : DocumentId(),
					.itemId = id.itemId,
				});
				if (document) {
					_generatedDocuments.remove(document);
				}
				data.downloaded.erase(begin(data.downloaded) + k);
				erased = true;
			}
			if (erased) {
				_generated.remove(item);
				_loaded.remove(item);
				_loadedRemoved.fire_copy(item);

				descriptor.sessions.emplace(session);
			}
		}
	}
	finishFilesDelete(std::move(descriptor));
}

void DownloadManager::deleteAll() {
	auto descriptor = DeleteFilesDescriptor();
	for (auto &[session, data] : _sessions) {
		if (!data.downloaded.empty()) {
			descriptor.sessions.emplace(session);
		} else if (data.downloading.empty()) {
			continue;
		}
		const auto sessionUniqueId = session->uniqueId();
		while (!data.downloading.empty()) {
			cancel(data, data.downloading.end() - 1);
		}
		for (auto &id : base::take(data.downloaded)) {
			const auto object = id.object.get();
			const auto document = object ? object->document : nullptr;
			descriptor.files.emplace(id.path, DocumentDescriptor{
				.sessionUniqueId = sessionUniqueId,
				.documentId = document ? document->id : DocumentId(),
				.itemId = id.itemId,
			});
			if (document) {
				_generatedDocuments.remove(document);
			}
			if (const auto item = object ? object->item.get() : nullptr) {
				_loaded.remove(item);
				_generated.remove(item);
				_loadedRemoved.fire_copy(item);
			}
		}
	}
	for (const auto &session : descriptor.sessions) {
		writePostponed(session);
	}
	finishFilesDelete(std::move(descriptor));
}

void DownloadManager::finishFilesDelete(DeleteFilesDescriptor &&descriptor) {
	for (const auto &session : descriptor.sessions) {
		writePostponed(session);
	}
	crl::async([files = std::move(descriptor.files)]{
		for (const auto &file : files) {
			const auto folder = RichExportFolderToRemove(file.first);
			if (!folder.isEmpty()) {
				QDir(folder).removeRecursively();
			} else {
				QFile(file.first).remove();
			}
			crl::on_main([descriptor = file.second] {
				if (const auto session = SessionByUniqueId(
						descriptor.sessionUniqueId)) {
					if (const auto id = descriptor.documentId) {
						[[maybe_unused]] const auto location
							= session->data().document(id)->location(true);
					}
					const auto itemId = descriptor.itemId;
					if (const auto item = session->data().message(itemId)) {
						session->data().requestItemRepaint(item);
					}
				}
			});
		}
	});
}

bool DownloadManager::loadedHasNonCloudFile() const {
	for (const auto &[session, data] : _sessions) {
		for (const auto &id : data.downloaded) {
			if (const auto object = id.object.get()) {
				if (!object->item->isHistoryEntry()) {
					return true;
				}
			}
		}
	}
	return false;
}

auto DownloadManager::loadingList() const
-> ranges::any_view<const DownloadingId*, ranges::category::input> {
	return ranges::views::all(
		_sessions
	) | ranges::views::transform([=](const auto &pair) {
		return ranges::views::all(
			pair.second.downloading
		) | ranges::views::transform([](const DownloadingId &id) {
			return &id;
		});
	}) | ranges::views::join;
}

DownloadProgress DownloadManager::loadingProgress() const {
	return _loadingProgress.current();
}

rpl::producer<> DownloadManager::loadingListChanges() const {
	return _loadingListChanges.events();
}

auto DownloadManager::loadingProgressValue() const
-> rpl::producer<DownloadProgress> {
	return _loadingProgress.value();
}

bool DownloadManager::loadingInProgress(Main::Session *onlyInSession) const {
	return lookupLoadingItem(onlyInSession) != nullptr;
}

HistoryItem *DownloadManager::lookupLoadingItem(
		Main::Session *onlyInSession) const {
	constexpr auto find = [](const SessionData &data) {
		constexpr auto proj = &DownloadingId::done;
		const auto i = ranges::find(data.downloading, false, proj);
		return (i != end(data.downloading)) ? i->object.item.get() : nullptr;
	};
	if (onlyInSession) {
		const auto i = _sessions.find(onlyInSession);
		return (i != end(_sessions)) ? find(i->second) : nullptr;
	} else {
		for (const auto &[session, data] : _sessions) {
			if (const auto result = find(data)) {
				return result;
			}
		}
	}
	return nullptr;
}

void DownloadManager::loadingStopWithConfirmation(
		Fn<void()> callback,
		Main::Session *onlyInSession) {
	const auto item = lookupLoadingItem(onlyInSession);
	if (!item) {
		return;
	}
	const auto window = Core::App().windowFor(
		not_null(&item->history()->session().account()));
	if (!window) {
		return;
	}
	const auto weak = base::make_weak(&item->history()->session());
	const auto id = item->fullId();
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				tr::lng_download_sure_stop(),
				st::boxLabel),
			st::boxPadding + QMargins(0, 0, 0, st::boxPadding.bottom()));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_selected_upload_stop(), [=] {
			box->closeBox();

			if (!onlyInSession || weak.get()) {
				loadingStop(onlyInSession);
			}
			if (callback) {
				callback();
			}
		}, st::attentionBoxButton);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->addLeftButton(tr::lng_upload_show_file(), [=] {
			box->closeBox();

			if (const auto strong = weak.get()) {
				if (const auto item = strong->data().message(id)) {
					if (const auto window = strong->tryResolveWindow()) {
						window->showMessage(item);
					}
				}
			}
		});
	});
	window->show(std::move(box));
	window->activate();
}

void DownloadManager::loadingStop(Main::Session *onlyInSession) {
	const auto stopInSession = [&](SessionData &data) {
		while (!data.downloading.empty()) {
			cancel(data, data.downloading.end() - 1);
		}
	};
	if (onlyInSession) {
		const auto i = _sessions.find(onlyInSession);
		if (i != end(_sessions)) {
			stopInSession(i->second);
		}
	} else {
		for (auto &[session, data] : _sessions) {
			stopInSession(data);
		}
	}
}

void DownloadManager::loadingStopSilent(Main::Session *onlyInSession) {
	pauseAll(onlyInSession);
}

void DownloadManager::pauseAll(Main::Session *onlyInSession) {
	// WHY: pausing a loader pushes a document progress update, which re-enters
	// this manager through check() and drops the entry being paused, so an
	// iterator into `downloading` must never survive the pause call.
	const auto dropEntry = [&](
			SessionData &data,
			not_null<const HistoryItem*> item) {
		const auto i = ranges::find(data.downloading, item, ByItem);
		if (i != end(data.downloading)) {
			remove(data, i);
		}
	};
	const auto pauseInSession = [&](SessionData &data) {
		while (!data.downloading.empty()) {
			const auto &entry = data.downloading.back();
			const auto object = entry.object;
			const auto item = object.item;
			const auto path = entry.path;
			const auto total = entry.total;
			const auto started = entry.started;

			auto descriptor = MakePausedDescriptor(
				object,
				path,
				0,
				total,
				started,
				PauseReason::Auto);
			auto completed = false;
			if (const auto document = object.document) {
				completed = !document->filepath(true).isEmpty();
				document->pauseLoad(&descriptor.ready);
			} else if (const auto photo = object.photo) {
				photo->cancel();
			}
			if (!item->isAdminLogEntry() && !completed) {
				_paused.push_back(std::move(descriptor));
			}
			dropEntry(data, item);
		}
	};
	if (onlyInSession) {
		const auto j = _sessions.find(onlyInSession);
		if (j != end(_sessions)) {
			pauseInSession(j->second);
		}
	} else {
		for (auto &[session, data] : _sessions) {
			pauseInSession(data);
		}
	}
	if (!_paused.empty()) {
		_pausedListChanges.fire({});
		flushPaused();
	}
}

void DownloadManager::pauseOne(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto it = ranges::find_if(data.downloading, [&](const auto &d) {
		return (d.object.item == item);
	});
	if (it == data.downloading.end()) {
		return;
	}
	const auto object = it->object;
	const auto path = it->path;
	const auto total = it->total;
	const auto started = it->started;

	auto descriptor = MakePausedDescriptor(
		object,
		path,
		0,
		total,
		started,
		PauseReason::User);
	if (const auto document = object.document) {
		document->pauseLoad(&descriptor.ready);
	} else if (const auto photo = object.photo) {
		photo->cancel();
	}
	_paused.push_back(std::move(descriptor));

	// pauseLoad() re-enters through check() and may have dropped the entry.
	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i != end(data.downloading)) {
		remove(data, i);
	}
	_pausedListChanges.fire({});
	flushPaused();
}

bool DownloadManager::resumeOne(const PausedDownloadDescriptor &descriptor) {
	if (descriptor.downloadType != DownloadType::Document) {
		return false;
	}
	const auto i = ranges::find_if(_paused, [&](const auto &d) {
		return SamePausedRecord(d, descriptor);
	});
	if (i == _paused.end() || i->path.isEmpty()) {
		return false;
	}
	const auto paused = *i;
	const auto itemId = FullMsgId(
		PeerId(paused.itemIdPeer),
		MsgId(paused.itemIdMsg));
	auto item = (HistoryItem*)nullptr;
	auto document = (DocumentData*)nullptr;
	for (auto &entry : _sessions) {
		const auto found = entry.first->data().message(itemId);
		const auto candidate = (found && found->media())
			? found->media()->document()
			: nullptr;
		if (candidate && (candidate->id == paused.documentId)) {
			item = found;
			document = candidate;
			break;
		}
	}
	if (!document) {
		// WHY: a restarted app has no message in memory, so the document can't
		// be reached until the server sends that message.
		DEBUG_LOG(("[DL] resume: msg %1 not in memory, requesting"
			).arg(paused.itemIdMsg));
		requestPausedResume(paused);
		return true;
	}
	return resumePaused(paused, item, document);
}

bool DownloadManager::resumePaused(
		const PausedDownloadDescriptor &paused,
		HistoryItem *item,
		DocumentData *document) {
	const auto i = ranges::find_if(_paused, [&](const auto &d) {
		return SamePausedRecord(d, paused);
	});
	if (i == _paused.end()) {
		return true;
	}
	// WHY: the download may have finished or been restarted from the chat, in
	// which case resuming is impossible and the record would never go away.
	if (document->loading() || !document->filepath(true).isEmpty()) {
		_paused.erase(i);
		_pausedListChanges.fire({});
		flushPaused();
		return true;
	}
	// WHY: resuming needs the temp file as the already downloaded prefix, at
	// least as large as the recorded ready offset, or the loader is asked for
	// offsets past the end of a fresh empty file and leaves a zero gap in it.
	const auto partial = QFileInfo(paused.path + u".temp"_q);
	const auto partialSize = partial.exists() ? partial.size() : 0;
	if (paused.ready > 0 && partialSize < paused.ready) {
		DEBUG_LOG(("[DL] resume: clamp %1 ready %2 -> %3, total %4"
			).arg(paused.path
			).arg(paused.ready
			).arg(partialSize
			).arg(paused.total));
		// WHY: keep the shown progress in sync with what is actually on disk.
		i->ready = partialSize;
		_pausedListChanges.fire({});
		flushPaused();
		return false;
	}
	DEBUG_LOG(("[DL] resume: start %1 ready %2 total %3"
		).arg(paused.path
		).arg(paused.ready
		).arg(paused.total));
	document->resumeLoad(paused.ready);
	document->save(Data::FileOrigin(FullMsgId(
		PeerId(paused.itemIdPeer),
		MsgId(paused.itemIdMsg))), paused.path);
	if (!document->loading()) {
		// WHY: the download did not start (for example the temp file was too
		// short to resume from), so keep the paused record instead of
		// dropping it silently.
		DEBUG_LOG(("[DL] resume: %1 did not start, ready %2"
			).arg(paused.path
			).arg(paused.ready));
		return false;
	}
	_paused.erase(i);
	_pausedListChanges.fire({});
	flushPaused();
	addLoading({
		.item = item,
		.document = document,
	});
	return true;
}

void DownloadManager::requestPausedResume(
		const PausedDownloadDescriptor &paused) {
	if (!IsServerMsgId(paused.itemIdMsg)) {
		return;
	}
	const auto itemId = FullMsgId(
		PeerId(paused.itemIdPeer),
		MsgId(paused.itemIdMsg));
	const auto isChannel = peerIsChannel(itemId.peer);
	const auto ids = QVector<MTPInputMessage>{
		MTP_inputMessageID(MTP_int(MsgId(paused.itemIdMsg).bare)),
	};
	for (auto &[session, data] : _sessions) {
		const auto weak = base::make_weak(session);
		const auto done = [=](const MTPmessages_Messages &result) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->data().processExistingMessages(
				isChannel
					? strong->data().channelLoaded(peerToChannel(itemId.peer))
					: nullptr,
				result);
			const auto found = strong->data().message(itemId);
			const auto media = found ? found->media() : nullptr;
			const auto document = media ? media->document() : nullptr;
			if (!document || document->id != paused.documentId) {
				DEBUG_LOG(("[DL] resume: msg %1 fetched without document"
					).arg(paused.itemIdMsg));
				return;
			}
			resumePaused(paused, found, document);
		};
		const auto fail = [=](const MTP::Error &error) {
			DEBUG_LOG(("[DL] resume: getMessages failed: %1"
				).arg(error.type()));
		};
		if (isChannel) {
			session->api().request(MTPchannels_GetMessages(
				MTP_inputChannel(
					MTP_long(peerToChannel(itemId.peer).bare),
					MTP_long(paused.peerAccessHash)),
				MTP_vector<MTPInputMessage>(ids)
			)).done(done).fail(fail).send();
		} else {
			session->api().request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(ids)
			)).done(done).fail(fail).send();
		}
	}
}

void DownloadManager::cancelPaused(
		not_null<const PausedDownloadDescriptor*> descriptor) {
	const auto i = ranges::find_if(_paused, [&](const auto &d) {
		return (&d == descriptor);
	});
	if (i != _paused.end()) {
		_paused.erase(i);
		_pausedListChanges.fire({});
		flushPaused();
	}
}

int DownloadManager::loadingCount(Main::Session *onlyInSession) const {
	int result = 0;
	if (onlyInSession) {
		const auto i = _sessions.find(onlyInSession);
		if (i != end(_sessions)) {
			result += i->second.downloading.size();
		}
	} else {
		for (const auto &[session, data] : _sessions) {
			result += data.downloading.size();
		}
	}
	return result;
}

int DownloadManager::pausedCount() const {
	return _paused.size();
}

int DownloadManager::loadedCount() const {
	int result = 0;
	for (const auto &[session, data] : _sessions) {
		result += data.downloaded.size();
	}
	return result;
}

auto DownloadManager::pausedList() const
-> ranges::any_view<const PausedDownloadDescriptor*,
		ranges::category::input> {
	return ranges::views::all(
		_paused
	) | ranges::views::transform([](const PausedDownloadDescriptor &d) {
		return &d;
	});
}

rpl::producer<> DownloadManager::pausedListChanges() const {
	return _pausedListChanges.events();
}

void DownloadManager::clearLoading() {
	Expects(_loading.empty());

	for (auto &[session, data] : _sessions) {
		while (!data.downloading.empty()) {
			remove(data, data.downloading.end() - 1);
		}
	}
}

auto DownloadManager::loadedList()
-> ranges::any_view<const DownloadedId*, ranges::category::input> {
	for (auto &[session, data] : _sessions) {
		resolve(session, data);
	}
	return ranges::views::all(
		_sessions
	) | ranges::views::transform([=](const auto &pair) {
		return ranges::views::all(
			pair.second.downloaded
		) | ranges::views::filter([](const DownloadedId &id) {
			return (id.object != nullptr);
		}) | ranges::views::transform([](const DownloadedId &id) {
			return &id;
		});
	}) | ranges::views::join;
}

rpl::producer<> DownloadManager::loadedResolveDone() const {
	using namespace rpl::mappers;
	return _loadedResolveDone.value() | rpl::filter(_1) | rpl::to_empty;
}

void DownloadManager::resolve(
		not_null<Main::Session*> session,
		SessionData &data) {
	const auto guard = gsl::finally([&] {
		checkFullResolveDone();
	});
	if (data.resolveSentTotal >= data.resolveNeeded
		|| data.resolveSentTotal >= kMaxResolvePerAttempt) {
		return;
	}
	struct Prepared {
		uint64 peerAccessHash = 0;
		QVector<MTPInputMessage> ids;
	};
	const auto &owner = session->data();
	auto prepared = base::flat_map<PeerId, Prepared>();
	auto last = begin(data.downloaded);
	auto from = last + (data.resolveNeeded - data.resolveSentTotal);
	for (auto i = from; i != last;) {
		auto &id = *--i;
		const auto msgId = id.itemId.msg;
		const auto info = QFileInfo(id.path);
		if (!info.exists() || info.size() != id.size) {
			// Mark as deleted.
			id.path = QString();
		} else if (!owner.message(id.itemId) && IsServerMsgId(msgId)) {
			const auto groupByPeer = peerIsChannel(id.itemId.peer)
				? id.itemId.peer
				: session->userPeerId();
			auto &perPeer = prepared[groupByPeer];
			if (peerIsChannel(id.itemId.peer) && !perPeer.peerAccessHash) {
				perPeer.peerAccessHash = id.peerAccessHash;
			}
			perPeer.ids.push_back(MTP_inputMessageID(MTP_int(msgId.bare)));
		}
		if (++data.resolveSentTotal >= kMaxResolvePerAttempt) {
			break;
		}
	}
	const auto check = [=] {
		auto &data = sessionData(session);
		if (!data.resolveSentRequests) {
			resolveRequestsFinished(session, data);
		}
	};
	const auto requestFinished = [=] {
		--sessionData(session).resolveSentRequests;
		check();
	};
	for (auto &[peer, perPeer] : prepared) {
		if (const auto channelId = peerToChannel(peer)) {
			session->api().request(MTPchannels_GetMessages(
				MTP_inputChannel(
					MTP_long(channelId.bare),
					MTP_long(perPeer.peerAccessHash)),
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(
					session->data().channelLoaded(channelId),
					result);
				requestFinished();
			}).fail(requestFinished).send();
		} else {
			session->api().request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(nullptr, result);
				requestFinished();
			}).fail(requestFinished).send();
		}
	}
	data.resolveSentRequests += prepared.size();
	check();
}

void DownloadManager::resolveRequestsFinished(
		not_null<Main::Session*> session,
		SessionData &data) {
	const auto &owner = session->data();
	for (; data.resolveSentTotal > 0; --data.resolveSentTotal) {
		const auto i = begin(data.downloaded) + (--data.resolveNeeded);
		if (i->path.isEmpty()) {
			data.downloaded.erase(i);
			continue;
		}
		const auto item = owner.message(i->itemId);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (i->download.type == DownloadType::Document
			&& (!document || document->id != i->download.objectId)) {
			generateEntry(session, *i);
		} else if (i->download.type == DownloadType::Photo
			&& (!photo || photo->id != i->download.objectId)) {
			generateEntry(session, *i);
		} else {
			i->object = std::make_unique<DownloadObject>(DownloadObject{
				.item = item,
				.document = document,
				.photo = photo,
			});
			_loaded.emplace(item);
		}
		_loadedAdded.fire(&*i);
	}
	crl::on_main(session, [=] {
		resolve(session, sessionData(session));
	});
}

void DownloadManager::checkFullResolveDone() {
	if (_loadedResolveDone.current()) {
		return;
	}
	for (const auto &[session, data] : _sessions) {
		if (data.resolveSentTotal < data.resolveNeeded
			|| data.resolveSentRequests > 0) {
			return;
		}
	}
	_loadedResolveDone = true;
}

void DownloadManager::generateEntry(
		not_null<Main::Session*> session,
		DownloadedId &id) {
	Expects(!id.object);

	const auto info = QFileInfo(id.path);
	const auto document = session->data().document(
		base::RandomValue<DocumentId>(),
		0, // accessHash
		QByteArray(), // fileReference
		TimeId(id.started / 1000),
		QVector<MTPDocumentAttribute>(
			1,
			MTP_documentAttributeFilename(
				MTP_string(info.fileName()))),
		Core::MimeTypeForFile(info).name(),
		InlineImageLocation(), // inlineThumbnail
		ImageWithLocation(), // thumbnail
		ImageWithLocation(), // videoThumbnail
		false, // isPremiumSticker
		0, // dc
		id.size);
	document->setLocation(Core::FileLocation(info));
	_generatedDocuments.emplace(document);

	id.object = std::make_unique<DownloadObject>(DownloadObject{
		.item = generateFakeItem(document),
		.document = document,
	});
	_loaded.emplace(id.object->item);
}

auto DownloadManager::loadedAdded() const
-> rpl::producer<not_null<const DownloadedId*>> {
	return _loadedAdded.events();
}

auto DownloadManager::loadedRemoved() const
-> rpl::producer<not_null<const HistoryItem*>> {
	return _loadedRemoved.events();
}

void DownloadManager::remove(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	const auto now = DownloadProgress{
		.ready = _loadingProgress.current().ready - i->ready,
		.total = _loadingProgress.current().total - i->total,
	};
	_loading.remove(i->object.item);
	_loadingDone.remove(i->object.item);
	if (const auto document = i->object.document) {
		_loadingDocuments.remove(document);
		if (i->externalCancel && !_loaded.contains(i->object.item)) {
			_generated.remove(i->object.item);
			_generatedDocuments.remove(document);
		}
	}
	data.downloading.erase(i);
	_loadingListChanges.fire({});
	_loadingProgress = now;
	writeInflightPostponed();
	if (_loading.empty() && !_loadingDone.empty()) {
		_clearLoadingTimer.callOnce(kClearLoadingTimeout);
	}
}

void DownloadManager::cancel(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	const auto object = i->object;
	const auto item = object.item;
	if (auto external = base::take(i->externalCancel)) {
		remove(data, i);
		external();
		return;
	}
	remove(data, i);
	if (!item->isAdminLogEntry()) {
		if (const auto document = object.document) {
			document->cancel();
		} else if (const auto photo = object.photo) {
			photo->cancel();
		}
	}
}

void DownloadManager::changed(not_null<const HistoryItem*> item) {
	if (_loaded.contains(item)) {
		auto &data = sessionData(item);

		// One item may be referenced by several downloaded entries, each
		// with its own object, so all of them have to be re-checked.
		// Indexed, because detach() emits and the list can move under us.
		auto found = false;
		for (auto i = 0; i != int(data.downloaded.size()); ++i) {
			if (ByItem(data.downloaded[i]) != item.get()) {
				continue;
			}
			found = true;
			if (!ItemContainsMedia(*data.downloaded[i].object)) {
				detach(data, data.downloaded[i]);
			}
		}
		Assert(found);
	}
	if (_loading.contains(item) || _loadingDone.contains(item)) {
		check(item);
	}
}

void DownloadManager::removed(not_null<const HistoryItem*> item) {
	if (_loaded.contains(item)) {
		auto &data = sessionData(item);
		auto i = ranges::find(data.downloaded, item.get(), ByItem);
		Assert(i != end(data.downloaded));

		// One item may be referenced by several downloaded entries, and
		// every one of them must be detached, or the rest keep pointing
		// at the destroyed HistoryItem.
		do {
			detach(data, *i);
			i = ranges::find(data.downloaded, item.get(), ByItem);
		} while (i != end(data.downloaded));
	}
	if (_loading.contains(item) || _loadingDone.contains(item)) {
		auto &data = sessionData(item);
		const auto i = ranges::find(data.downloading, item, ByItem);
		Assert(i != end(data.downloading));

		// We don't want to download files without messages.
		// For example, there is no way to refresh a file reference for them.
		//entry.object.item = nullptr;
		cancel(data, i);
	}
}

not_null<HistoryItem*> DownloadManager::regenerateItem(
		const DownloadObject &previous) {
	return generateItem(previous.item, previous.document, previous.photo);
}

not_null<HistoryItem*> DownloadManager::generateFakeItem(
		not_null<DocumentData*> document) {
	return generateItem(nullptr, document, nullptr);
}

not_null<HistoryItem*> DownloadManager::generateItem(
		HistoryItem *previousItem,
		DocumentData *document,
		PhotoData *photo) {
	Expects(document || photo);

	const auto session = document
		? &document->session()
		: &photo->session();
	const auto history = previousItem
		? previousItem->history()
		: session->data().history(session->user());
	;
	const auto caption = TextWithEntities();
	const auto make = [&](const auto media) {
		return history->makeMessage({
			.id = history->nextNonHistoryEntryId(),
			.flags = MessageFlag::FakeHistoryItem,
			.from = (previousItem
				? previousItem->from()->id
				: session->userPeerId()),
			.date = base::unixtime::now(),
		}, media, caption);
	};
	const auto result = document ? make(document) : make(photo);
	_generated.emplace(result);
	return result;
}

void DownloadManager::detach(SessionData &data, DownloadedId &id) {
	Expects(id.object != nullptr);
	Expects(!_generated.contains(id.object->item));

	// Maybe generate new document?
	const auto was = id.object->item;
	const auto now = regenerateItem(*id.object);
	id.object->item = now;
	_loaded.emplace(now);

	// One item may have several downloaded entries, so it leaves _loaded -
	// and the downloads list - only when the last of them was detached.
	// Reporting it removed while another entry still holds it would drop a
	// row that is still backed by a file.
	if (!ranges::contains(data.downloaded, was.get(), ByItem)) {
		_loaded.remove(was);
		_loadedRemoved.fire_copy(was);
	}

	_loadedAdded.fire_copy(&id);
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));
	return i->second;
}

const DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<Main::Session*> session) const {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));
	return i->second;
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<const HistoryItem*> item) {
	return sessionData(&item->history()->session());
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<DocumentData*> document) {
	return sessionData(&document->session());
}

void DownloadManager::writePostponed(Main::Session *session) {
	if (!session) {
		for (auto &[s, data] : _sessions) {
			writePostponed(s);
		}
		return;
	}
	session->account().local().updateDownloads(serializator(session));
	session->account().local().updatePausedDownloads(
		[this, weak = base::make_weak(session)]()
		-> std::optional<QByteArray> {
		const auto strong = weak.get();
		if (!strong) {
			return std::nullopt;
		}
		return serializePaused();
	});
}

void DownloadManager::flushPaused() {
	writePostponed(nullptr);
	for (auto &[session, data] : _sessions) {
		session->account().local().writeLocationsForce();
	}
}

void DownloadManager::writeInflightPostponed() {
	for (auto &[session, data] : _sessions) {
		session->account().local().updateInflightDownloads(
			[this, weak = base::make_weak(session)]()
			-> std::optional<QByteArray> {
				const auto strong = weak.get();
				if (!strong) {
					return std::nullopt;
				}
				return serializeInflight();
			});
	}
}

Fn<std::optional<QByteArray>()> DownloadManager::serializator(
		not_null<Main::Session*> session) const {
	return [this, weak = base::make_weak(session)]()
		-> std::optional<QByteArray> {
		const auto strong = weak.get();
		if (!strong) {
			return std::nullopt;
		} else if (!_sessions.contains(strong)) {
			return QByteArray();
		}
		auto result = QByteArray();
		const auto &data = sessionData(strong);
		const auto count = data.downloaded.size();
		const auto constant = sizeof(quint64) // download.objectId
			+ sizeof(qint32) // download.type
			+ sizeof(qint64) // started
			+ sizeof(quint32) // size
			+ sizeof(quint64) // itemId.peer
			+ sizeof(qint64) // itemId.msg
			+ sizeof(quint64); // peerAccessHash
		auto size = sizeof(qint32) // count
			+ count * constant;
		for (const auto &id : data.downloaded) {
			size += Serialize::stringSize(id.path);
		}
		result.reserve(size);

		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(count);
		for (const auto &id : data.downloaded) {
			stream
				<< quint64(id.download.objectId)
				<< qint32(id.download.type)
				<< qint64(id.started)
				// FileSize: Right now any file size fits 32 bit.
				<< quint32(id.size)
				<< quint64(id.itemId.peer.value)
				<< qint64(id.itemId.msg.bare)
				<< quint64(id.peerAccessHash)
				<< id.path;
		}
		stream.device()->close();

		return result;
	};
}

std::vector<DownloadedId> DownloadManager::deserialize(
		not_null<Main::Session*> session) const {
	const auto serialized = session->account().local().downloadsSerialized();
	if (serialized.isEmpty()) {
		return {};
	}

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok || count <= 0 || count > 99'999) {
		return {};
	}
	auto result = std::vector<DownloadedId>();
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto downloadObjectId = quint64();
		auto uncheckedDownloadType = qint32();
		auto started = qint64();
		// FileSize: Right now any file size fits 32 bit.
		auto size = quint32();
		auto itemIdPeer = quint64();
		auto itemIdMsg = qint64();
		auto peerAccessHash = quint64();
		auto path = QString();
		stream
			>> downloadObjectId
			>> uncheckedDownloadType
			>> started
			>> size
			>> itemIdPeer
			>> itemIdMsg
			>> peerAccessHash
			>> path;
		const auto downloadType = DownloadType(uncheckedDownloadType);
		if (stream.status() != QDataStream::Ok
			|| path.isEmpty()
			|| size <= 0
			|| size > kMaxFileSize
			|| (downloadType != DownloadType::Document
				&& downloadType != DownloadType::Photo)) {
			return {};
		}
		result.push_back({
			.download = {
				.objectId = downloadObjectId,
				.type = downloadType,
			},
			.started = started,
			.path = path,
			.size = int64(size),
			.itemId = { PeerId(itemIdPeer), MsgId(itemIdMsg) },
			.peerAccessHash = peerAccessHash,
		});
	}
	return result;
}

QByteArray DownloadManager::serializePaused() const {
	return SerializePausedDownloads(_paused);
}

QByteArray DownloadManager::serializeInflight() const {
	auto list = std::vector<PausedDownloadDescriptor>();
	for (const auto &[session, data] : _sessions) {
		for (const auto &entry : data.downloading) {
			if (entry.done || entry.object.item->isAdminLogEntry()) {
				continue;
			}
			list.push_back(MakePausedDescriptor(
				entry.object,
				entry.path,
				entry.ready,
				entry.total,
				entry.started,
				PauseReason::Auto));
		}
	}
	DEBUG_LOG(("[DL] inflight snapshot: %1 downloading entries"
		).arg(list.size()));
	return SerializePausedDownloads(list);
}

void DownloadManager::deserializePaused(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return;
	}
	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok || version < 1 || count <= 0) {
		return;
	}
	auto list = std::vector<PausedDownloadDescriptor>();
	list.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto d = PausedDownloadDescriptor();
		if (!DeserializePausedDownload(stream, d)) {
			return;
		}
		// WHY: builds before the path fix stored the .temp path as the final
		// destination, which would make a resumed download write to <name>.temp.
		if (d.path.endsWith(u".temp"_q)
			&& d.ready > 0
			&& QFileInfo(d.path).exists()
			&& !QFileInfo(d.path + u".temp"_q).exists()) {
			d.path.chop(5);
		}
		list.push_back(std::move(d));
	}
	_paused = std::move(list);
}

void DownloadManager::deserializeInflight(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return;
	}
	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok || version < 1 || count <= 0) {
		return;
	}
	auto list = std::vector<PausedDownloadDescriptor>();
	list.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto d = PausedDownloadDescriptor();
		if (!DeserializePausedDownload(stream, d)) {
			return;
		}
		list.push_back(std::move(d));
	}
	// WHY: an in-flight download could not be paused before the crash, so it
	// is restored in the paused list, where the user can resume it.
	DEBUG_LOG(("[DL] inflight restore: %1 entries"
		).arg(list.size()));
	for (auto &d : list) {
		_paused.push_back(std::move(d));
	}
}

void DownloadManager::validatePausedDownloads() {
	for (auto &d : _paused) {
		if (d.path.isEmpty()) {
			continue;
		}
		const auto partial = QFileInfo(d.path + u".temp"_q);
		const auto complete = QFileInfo(d.path);
		const auto hasTemp = partial.exists();
		const auto partialSize = hasTemp ? partial.size() : int64(0);
		const auto readyBefore = d.ready;
		if (!hasTemp) {
			// WHY: without the temp file the parts would be requested at their
			// original offsets past the end of a fresh empty file, leaving it
			// zero-filled up to the recorded offset.
			d.ready = 0;
		} else if (const auto actual = partial.size(); actual < d.ready) {
			d.ready = actual;
		}
		DEBUG_LOG(("[DL] validate: %1 ready %2 -> %3, total %4, temp %5"
			).arg(d.path
			).arg(readyBefore
			).arg(d.ready
			).arg(d.total
			).arg(partialSize));
		if (d.displayName.isEmpty()) {
			d.displayName = complete.fileName();
		}
	}
}

QString DownloadManager::exportPausedJson() const {
	auto items = QJsonArray();
	const auto append = [&](
			uint64 documentId,
			uint64 photoId,
			DownloadType type,
			int64 itemIdPeer,
			int64 itemIdMsg,
			uint64 accessHash,
			int64 ready,
			int64 total,
			DownloadDate started,
			PauseReason reason,
			const QString &path,
			const QString &displayName,
			const QString &sourceTitle) {
		auto object = QJsonObject();
		object.insert(u"documentId"_q, QString::number(documentId));
		object.insert(u"photoId"_q, QString::number(photoId));
		object.insert(u"downloadType"_q, int(type));
		object.insert(u"itemIdPeer"_q, QString::number(itemIdPeer));
		object.insert(u"itemIdMsg"_q, QString::number(itemIdMsg));
		object.insert(u"peerAccessHash"_q, QString::number(accessHash));
		object.insert(u"ready"_q, QString::number(ready));
		object.insert(u"total"_q, QString::number(total));
		object.insert(u"started"_q, QString::number(started));
		object.insert(u"reason"_q, int(reason));
		object.insert(u"path"_q, path);
		object.insert(u"displayName"_q, displayName);
		object.insert(u"sourceTitle"_q, sourceTitle);
		items.append(object);
	};
	for (const auto &d : _paused) {
		append(
			d.documentId,
			d.photoId,
			d.downloadType,
			d.itemIdPeer,
			d.itemIdMsg,
			d.peerAccessHash,
			d.ready,
			d.total,
			d.started,
			d.reason,
			d.path,
			d.displayName,
			d.sourceTitle);
	}
	// Running downloads are exported too, or a backup taken mid-download is empty.
	for (const auto &[session, data] : _sessions) {
		for (const auto &entry : data.downloading) {
			const auto &object = entry.object;
			const auto document = object.document;
			const auto photo = object.photo;
			const auto peer = object.item->history()->peer;
			const auto saveAs = QFileInfo(entry.path).fileName();
			append(
				document ? document->id : 0,
				photo ? photo->id : 0,
				document ? DownloadType::Document : DownloadType::Photo,
				peer->id.value,
				object.item->id.bare,
				PeerAccessHash(peer),
				entry.ready,
				entry.total,
				entry.started,
				PauseReason::Auto,
				entry.path,
				!saveAs.isEmpty()
					? saveAs
					: (document ? document->filename() : QString()),
				peer->name());
		}
	}
	// WHY: a backup listing only unfinished tasks says nothing about what is
	// already on disk; completed entries are record-only, restoring needs the
	// source message in memory, which JSON cannot carry.
	auto completed = QJsonArray();
	for (const auto &[session, data] : _sessions) {
		for (const auto &id : data.downloaded) {
			if (!id.object || id.path.isEmpty()) {
				continue;
			}
			const auto &object = *id.object;
			const auto peer = object.item->history()->peer;
			const auto saveAs = QFileInfo(id.path).fileName();
			auto entry = QJsonObject();
			entry.insert(u"documentId"_q, QString::number(
				id.download.type == DownloadType::Document
					? id.download.objectId : 0));
			entry.insert(u"photoId"_q, QString::number(
				id.download.type == DownloadType::Photo
					? id.download.objectId : 0));
			entry.insert(u"itemIdPeer"_q, QString::number(id.itemId.peer.value));
			entry.insert(u"itemIdMsg"_q, QString::number(id.itemId.msg.bare));
			entry.insert(u"size"_q, QString::number(id.size));
			entry.insert(u"started"_q, QString::number(id.started));
			entry.insert(u"path"_q, id.path);
			entry.insert(u"displayName"_q, saveAs);
			entry.insert(u"sourceTitle"_q, peer->name());
			completed.append(entry);
		}
	}
	if (items.isEmpty() && completed.isEmpty()) {
		return QString();
	}
	auto root = QJsonObject();
	root.insert(u"app"_q, u"AyuGram"_q);
	root.insert(u"format"_q, u"downloads"_q);
	root.insert(u"version"_q, 1);
	root.insert(u"exportedAt"_q, double(base::unixtime::now()));
	root.insert(u"items"_q, items);
	root.insert(u"completed"_q, completed);
	return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

std::optional<int> DownloadManager::importPausedJson(const QString &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto root = document.object();
	if (root.value(u"format"_q).toString() != u"downloads"_q) {
		return std::nullopt;
	}
	if (root.value(u"version"_q).toInt() != 1) {
		return std::nullopt;
	}
	const auto items = root.value(u"items"_q).toArray();
	const auto readInt = [](const QJsonObject &object, const QString &key) {
		const auto value = object.value(key);
		if (value.isString()) {
			return qint64(value.toString().toLongLong());
		}
		return qint64(value.toDouble());
	};
	auto added = 0;
	for (const auto &value : items) {
		const auto object = value.toObject();
		auto descriptor = PausedDownloadDescriptor();
		descriptor.documentId = object.value(
			u"documentId"_q).toString().toULongLong();
		descriptor.photoId = object.value(u"photoId"_q).toString().toULongLong();
		const auto downloadType = readInt(object, u"downloadType"_q);
		// WHY: photos cannot be resumed and there is no way to dismiss a paused
		// photo row, so importing one would leave a permanently stuck entry.
		if (downloadType != int(DownloadType::Document)) {
			continue;
		}
		descriptor.downloadType = DownloadType(downloadType);
		descriptor.itemIdPeer = object.value(
			u"itemIdPeer"_q).toString().toLongLong();
		descriptor.itemIdMsg = object.value(
			u"itemIdMsg"_q).toString().toLongLong();
		descriptor.peerAccessHash = object.value(
			u"peerAccessHash"_q).toString().toULongLong();
		descriptor.ready = readInt(object, u"ready"_q);
		descriptor.total = readInt(object, u"total"_q);
		descriptor.started = readInt(object, u"started"_q);
		const auto reason = readInt(object, u"reason"_q);
		if (reason != int(PauseReason::User)
			&& reason != int(PauseReason::Auto)
			&& reason != int(PauseReason::Network)) {
			continue;
		}
		descriptor.reason = PauseReason(reason);
		descriptor.path = object.value(u"path"_q).toString();
		descriptor.displayName = object.value(u"displayName"_q).toString();
		descriptor.sourceTitle = object.value(u"sourceTitle"_q).toString();
		if (descriptor.path.isEmpty() || !descriptor.documentId) {
			continue;
		}
		const auto duplicate = ranges::any_of(_paused, [&](
				const PausedDownloadDescriptor &d) {
			return (d.documentId == descriptor.documentId)
				&& (d.photoId == descriptor.photoId)
				&& (d.itemIdPeer == descriptor.itemIdPeer)
				&& (d.itemIdMsg == descriptor.itemIdMsg);
		});
		if (duplicate) {
			continue;
		}
		_paused.push_back(std::move(descriptor));
		++added;
	}
	if (!added) {
		return added;
	}
	validatePausedDownloads();
	_pausedListChanges.fire({});
	flushPaused();
	return added;
}

void DownloadManager::untrack(not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));

	for (const auto &entry : i->second.downloaded) {
		if (const auto resolved = entry.object.get()) {
			const auto item = resolved->item;
			_loaded.remove(item);
			_generated.remove(item);
			if (const auto document = resolved->document) {
				_generatedDocuments.remove(document);
			}
		}
	}
	while (!i->second.downloading.empty()) {
		remove(i->second, i->second.downloading.end() - 1);
	}
	_sessions.erase(i);
}

rpl::producer<Ui::DownloadBarProgress> MakeDownloadBarProgress() {
	return Core::App().downloadManager().loadingProgressValue(
	) | rpl::map([=](const DownloadProgress &progress) {
		return Ui::DownloadBarProgress{
			.ready = progress.ready,
			.total = progress.total,
		};
	});
}

rpl::producer<Ui::DownloadBarContent> MakeDownloadBarContent() {
	return [](auto consumer) {
		auto lifetime = rpl::lifetime();

		struct State {
			DocumentData *document = nullptr;
			std::shared_ptr<Data::DocumentMedia> media;
			rpl::lifetime downloadTaskLifetime;
			QImage thumbnail;

			base::has_weak_ptr guard;
			bool scheduled = false;
			Fn<void()> push;
		};

		const auto state = lifetime.make_state<State>();
		auto &manager = Core::App().downloadManager();

		const auto resolveThumbnailRecursive = [=](auto &&self) -> bool {
			if (state->document && !state->document->hasThumbnail()) {
				state->media = nullptr;
			}
			if (!state->media) {
				state->downloadTaskLifetime.destroy();
				if (!state->thumbnail.isNull()) {
					return false;
				}
				state->thumbnail = QImage();
				return true;
			}
			if (const auto image = state->media->thumbnail()) {
				state->thumbnail = image->original();
				state->downloadTaskLifetime.destroy();
				state->media = nullptr;
				return true;
			} else if (const auto embed = state->media->thumbnailInline()) {
				if (!state->thumbnail.isNull()) {
					return false;
				}
				state->thumbnail = Images::Prepare(embed->original(), 0, {
					.options = Images::Option::Blur,
				});
			} else if (!state->downloadTaskLifetime) {
				state->document->session().downloaderTaskFinished(
				) | rpl::filter([=] {
					return self(self);
				}) | rpl::on_next(
					state->push,
					state->downloadTaskLifetime);
			}
			return !state->thumbnail.isNull();
		};
		const auto resolveThumbnail = [=] {
			return resolveThumbnailRecursive(resolveThumbnailRecursive);
		};

		const auto notify = [=, &manager] {
			auto content = Ui::DownloadBarContent();
			auto single = (const Data::DownloadObject*) nullptr;
			for (const auto id : manager.loadingList()) {
				if (id->hiddenByView) {
					break;
				}
				if (!single) {
					single = &id->object;
				}
				++content.count;
				if (id->done) {
					++content.done;
				}
			}
			if (content.count == 1) {
				const auto document = single->document;
				const auto thumbnailed = (single->item
					&& document->hasThumbnail())
					? document
					: nullptr;
				if (state->document != thumbnailed) {
					state->document = thumbnailed;
					state->media = thumbnailed
						? thumbnailed->createMediaView()
						: nullptr;
					if (const auto raw = state->media.get()) {
						raw->thumbnailWanted(single->item->fullId());
					}
					state->thumbnail = QImage();
					resolveThumbnail();
				}
				content.singleName = Ui::Text::FormatDownloadsName(
					document);
				content.singleThumbnail = state->thumbnail;
			}
			consumer.put_next(std::move(content));
		};
		state->push = [=] {
			if (state->scheduled) {
				return;
			}
			state->scheduled = true;
			Ui::PostponeCall(&state->guard, [=] {
				state->scheduled = false;
				notify();
			});
		};

		manager.loadingListChanges(
		) | rpl::filter([=] {
			return !state->scheduled;
		}) | rpl::on_next(state->push, lifetime);

		notify();
		return lifetime;
	};
}

} // namespace Data
