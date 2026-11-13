/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"

namespace Ui {
struct DownloadBarProgress;
struct DownloadBarContent;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Data {

// Used in serialization!
enum class DownloadType {
	Document,
	Photo,
};

// unixtime * 1000.
using DownloadDate = int64;

[[nodiscard]] inline TimeId DateFromDownloadDate(DownloadDate date) {
	return date / 1000;
}

struct DownloadId {
	uint64 objectId = 0;
	DownloadType type = DownloadType::Document;
};

struct DownloadProgress {
	int64 ready = 0;
	int64 total = 0;
};
inline constexpr bool operator==(
		const DownloadProgress &a,
		const DownloadProgress &b) {
	return (a.ready == b.ready) && (a.total == b.total);
}

struct DownloadObject {
	not_null<HistoryItem*> item;
	DocumentData *document = nullptr;
	PhotoData *photo = nullptr;
};

struct DownloadedId {
	DownloadId download;
	DownloadDate started = 0;
	QString path;
	int64 size = 0;
	FullMsgId itemId;
	uint64 peerAccessHash = 0;

	std::unique_ptr<DownloadObject> object;
};

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

enum class PauseReason : uchar {
	User,
	Auto,
	Network,
};

struct PausedDownloadDescriptor {
	uint64 documentId = 0;
	uint64 photoId = 0;
	int64 itemIdPeer = 0;
	int64 itemIdMsg = 0;
	uint64 peerAccessHash = 0;
	DownloadType downloadType = DownloadType::Document;

	int64 ready = 0;
	int64 total = 0;
	QString path;

	QString displayName;
	QString sourceTitle;
	int64 started = 0;
	PauseReason reason = PauseReason::Auto;
};

class DownloadManager final {
public:
	DownloadManager();
	~DownloadManager();

	[[nodiscard]] bool empty() const;

	void trackSession(not_null<Main::Session*> session);
	void itemVisibilitiesUpdated(not_null<Main::Session*> session);

	[[nodiscard]] DownloadDate computeNextStartDate();

	void addLoading(DownloadObject object);
	void addLoaded(
		DownloadObject object,
		const QString &path,
		DownloadDate started);

	struct ExternalLoadingState {
		int64 ready = 0;
		int64 total = 0;
		bool done = false;
	};
	[[nodiscard]] not_null<HistoryItem*> generateExternalItem(
		not_null<DocumentData*> document);
	void addLoadingExternal(
		DownloadObject object,
		const QString &path,
		int64 total,
		Fn<void()> cancel);
	void updateLoadingExternal(
		not_null<const HistoryItem*> item,
		int64 ready,
		int64 total);
	[[nodiscard]] auto loadingExternalState(
		not_null<const HistoryItem*> item)
	-> std::optional<ExternalLoadingState>;
	void cancelLoadingExternal(not_null<const HistoryItem*> item);
	void finishLoadingExternal(
		not_null<const HistoryItem*> item,
		const QString &path);
	void removeLoadingExternal(not_null<const HistoryItem*> item);

	void clearIfFinished();
	void deleteFiles(const std::vector<GlobalMsgId> &ids);
	void deleteAll();
	[[nodiscard]] bool loadedHasNonCloudFile() const;

	[[nodiscard]] auto loadingList() const
		-> ranges::any_view<const DownloadingId*, ranges::category::input>;
	[[nodiscard]] DownloadProgress loadingProgress() const;
	[[nodiscard]] rpl::producer<> loadingListChanges() const;
	[[nodiscard]] auto loadingProgressValue() const
		-> rpl::producer<DownloadProgress>;

	[[nodiscard]] bool loadingInProgress(
		Main::Session *onlyInSession = nullptr) const;
	void loadingStopWithConfirmation(
		Fn<void()> callback,
		Main::Session *onlyInSession = nullptr);
	void loadingStopSilent(Main::Session *onlyInSession = nullptr);

	void pauseAll(Main::Session *onlyInSession = nullptr);
	void pauseOne(not_null<const HistoryItem*> item);
	bool resumeOne(const PausedDownloadDescriptor &descriptor);
	void cancelPaused(not_null<const PausedDownloadDescriptor*> descriptor);

	// Exports the paused downloads as JSON text (empty if there are none) and
	// merges a previously exported list back, returning the added count or
	// std::nullopt if the text is not a download list.
	[[nodiscard]] QString exportPausedJson() const;
	[[nodiscard]] std::optional<int> importPausedJson(const QString &json);

	[[nodiscard]] int loadingCount(
		Main::Session *onlyInSession = nullptr) const;
	[[nodiscard]] int pausedCount() const;
	[[nodiscard]] auto pausedList() const
		-> ranges::any_view<const PausedDownloadDescriptor*,
			ranges::category::input>;
	[[nodiscard]] rpl::producer<> pausedListChanges() const;

	[[nodiscard]] auto loadedList()
		-> ranges::any_view<const DownloadedId*, ranges::category::input>;
	[[nodiscard]] auto loadedAdded() const
		-> rpl::producer<not_null<const DownloadedId*>>;
	[[nodiscard]] auto loadedRemoved() const
		-> rpl::producer<not_null<const HistoryItem*>>;
	[[nodiscard]] rpl::producer<> loadedResolveDone() const;

private:
	struct DeleteFilesDescriptor;
	struct SessionData {
		std::vector<DownloadedId> downloaded;
		std::vector<DownloadingId> downloading;
		int resolveNeeded = 0;
		int resolveSentRequests = 0;
		int resolveSentTotal = 0;
		rpl::lifetime lifetime;
	};

	void check(not_null<const HistoryItem*> item);
	void check(not_null<DocumentData*> document);
	void check(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	[[nodiscard]] auto lookupExternal(not_null<const HistoryItem*> item)
		-> std::pair<SessionData*, std::vector<DownloadingId>::iterator>;
	void changed(not_null<const HistoryItem*> item);
	void removed(not_null<const HistoryItem*> item);
	void detach(SessionData &data, DownloadedId &id);
	void untrack(not_null<Main::Session*> session);
	void remove(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void cancel(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void clearLoading();

	[[nodiscard]] SessionData &sessionData(not_null<Main::Session*> session);
	[[nodiscard]] const SessionData &sessionData(
		not_null<Main::Session*> session) const;
	[[nodiscard]] SessionData &sessionData(
		not_null<const HistoryItem*> item);
	[[nodiscard]] SessionData &sessionData(not_null<DocumentData*> document);

	void resolve(not_null<Main::Session*> session, SessionData &data);
	void resolveRequestsFinished(
		not_null<Main::Session*> session,
		SessionData &data);
	void checkFullResolveDone();

	[[nodiscard]] not_null<HistoryItem*> regenerateItem(
		const DownloadObject &previous);
	[[nodiscard]] not_null<HistoryItem*> generateFakeItem(
		not_null<DocumentData*> document);
	[[nodiscard]] not_null<HistoryItem*> generateItem(
		HistoryItem *previousItem,
		DocumentData *document,
		PhotoData *photo);
	void generateEntry(not_null<Main::Session*> session, DownloadedId &id);

	[[nodiscard]] HistoryItem *lookupLoadingItem(
		Main::Session *onlyInSession) const;
	void loadingStop(Main::Session *onlyInSession);

	void finishFilesDelete(DeleteFilesDescriptor &&descriptor);
	void writePostponed(Main::Session *session);
	void flushPaused();
	[[nodiscard]] Fn<std::optional<QByteArray>()> serializator(
		not_null<Main::Session*> session) const;
	[[nodiscard]] std::vector<DownloadedId> deserialize(
		not_null<Main::Session*> session) const;

	[[nodiscard]] QByteArray serializePaused() const;
	void deserializePaused(const QByteArray &data);
	void validatePausedDownloads();

	base::flat_map<not_null<Main::Session*>, SessionData> _sessions;
	base::flat_set<not_null<const HistoryItem*>> _loading;
	base::flat_set<not_null<DocumentData*>> _loadingDocuments;
	base::flat_set<not_null<const HistoryItem*>> _loadingDone;
	base::flat_set<not_null<const HistoryItem*>> _loaded;
	base::flat_set<not_null<HistoryItem*>> _generated;
	base::flat_set<not_null<DocumentData*>> _generatedDocuments;

	std::vector<PausedDownloadDescriptor> _paused;

	TimeId _lastStartedBase = 0;
	int _lastStartedAdded = 0;

	rpl::event_stream<> _loadingListChanges;
	rpl::variable<DownloadProgress> _loadingProgress;

	rpl::event_stream<not_null<const DownloadedId*>> _loadedAdded;
	rpl::event_stream<not_null<const HistoryItem*>> _loadedRemoved;
	rpl::variable<bool> _loadedResolveDone;

	rpl::event_stream<> _pausedListChanges;

	base::Timer _clearLoadingTimer;

};

[[nodiscard]] auto MakeDownloadBarProgress()
-> rpl::producer<Ui::DownloadBarProgress>;

[[nodiscard]] rpl::producer<Ui::DownloadBarContent> MakeDownloadBarContent();

} // namespace Data
