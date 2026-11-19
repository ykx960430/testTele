// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/clipboard_rules.h"
#include "ayu/utils/clipboard_rules_data.h"

#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_text.h"
#include "ui/text/text_utilities.h"

#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cstdint>

namespace Ayu::ClipboardRules {
namespace {

Rules gRules;
bool gRulesLoaded = false;

QString ResolvePath() {
	const auto flag = QString::fromLatin1("--gt-rule-file");
	const auto args = QCoreApplication::arguments();
	for (auto i = 0; i < args.size(); ++i) {
		if (args[i] == flag && i + 1 < args.size()) {
			return args[i + 1];
		} else if (args[i].startsWith(flag + u"="_q)) {
			return args[i].mid(flag.size() + 1);
		}
	}
	const auto env = qEnvironmentVariable("GT_RULE_FILE");
	if (!env.isEmpty()) {
		return env;
	}
	const auto exeDir = QCoreApplication::applicationDirPath();
	const auto candidates = {
		exeDir + u"/ayu_clipboard_rules.json"_q,
		exeDir + u"/../ayu_clipboard_rules.json"_q,
		QDir::currentPath() + u"/ayu_clipboard_rules.json"_q,
	};
	for (const auto &candidate : candidates) {
		if (QFile::exists(candidate)) {
			return candidate;
		}
	}
	// WHY: the rules are meant to be edited without rebuilding, so the
	// defaults are seeded next to the executable on first run.
	return exeDir + u"/ayu_clipboard_rules.json"_q;
}

void EnsureFile(const QString &path) {
	if (path.isEmpty() || QFile::exists(path)) {
		return;
	}
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	file.write(kDefaultJson, int(sizeof(kDefaultJson) - 1));
	file.close();
}

std::vector<std::pair<QString, std::vector<QString>>> ReadWordMap(
		const QJsonObject &source) {
	std::vector<std::pair<QString, std::vector<QString>>> result;
	for (auto it = source.begin(); it != source.end(); ++it) {
		std::vector<QString> words;
		for (const auto &word : it.value().toArray()) {
			if (const auto text = word.toString(); !text.isEmpty()) {
				words.push_back(text);
			}
		}
		if (!words.empty()) {
			result.emplace_back(it.key(), std::move(words));
		}
	}
	return result;
}

Rules Parse(const QJsonObject &object) {
	Rules rules;
	rules.typeRules = ReadWordMap(object.value(u"typeRules"_q).toObject());
	rules.replaceRule = ReadWordMap(object.value(u"replaceRule"_q).toObject());
	const auto simplified = object.value(u"simplized"_q).toArray();
	if (simplified.size() == 2) {
		rules.traditional = simplified.at(0).toString();
		rules.simplified = simplified.at(1).toString();
	}
	const auto options = object.value(u"options"_q).toObject();
	for (const auto &step : options.value(u"steps"_q).toArray()) {
		if (const auto name = step.toString(); !name.isEmpty()) {
			rules.steps.push_back(name);
		}
	}
	rules.maxLength = options.value(u"maxLength"_q).toInt(80);
	rules.keepHashtags = options.value(u"keepHashtags"_q).toBool(false);
	return rules;
}

void Reload() {
	const auto path = ResolvePath();
	if (path.isEmpty()) {
		return;
	}
	EnsureFile(path);
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto data = file.readAll();
	file.close();
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(data, &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	auto rules = Parse(doc.object());
	if (rules.typeRules.empty() && rules.replaceRule.empty()) {
		return;
	}
	gRules = std::move(rules);
	gRulesLoaded = true;
}

const Rules &Fallback() {
	static const Rules result = [] {
		QJsonParseError error{};
		const auto doc = QJsonDocument::fromJson(
			QByteArray::fromRawData(
				kDefaultJson,
				int(sizeof(kDefaultJson) - 1)),
			&error);
		return (error.error == QJsonParseError::NoError && doc.isObject())
			? Parse(doc.object())
			: Rules{};
	}();
	return result;
}

bool IsEmojiCodePoint(uint32_t code) {
	return (code >= 0x2700 && code <= 0x27BF)
		|| code >= 0x1F000
		|| code == 0x3299 || code == 0x3297 || code == 0x303D || code == 0x3030
		|| code == 0x24C2
		|| code == 0x203C || code == 0x2049
		|| (code >= 0x25AA && code <= 0x25AB)
		|| code == 0x25B6 || code == 0x25C0
		|| (code >= 0x25FB && code <= 0x25FE)
		|| code == 0x00A9 || code == 0x00AE || code == 0x2122 || code == 0x2139
		|| (code >= 0x2600 && code <= 0x26FF)
		|| code == 0x2B05 || code == 0x2B06 || code == 0x2B07
		|| code == 0x2B1B || code == 0x2B1C || code == 0x2B50 || code == 0x2B55
		|| code == 0x231A || code == 0x231B || code == 0x2328 || code == 0x23CF
		|| (code >= 0x23E9 && code <= 0x23F3)
		|| (code >= 0x23F8 && code <= 0x23FA)
		|| code == 0x2934 || code == 0x2935
		|| (code >= 0x2190 && code <= 0x21FF);
}

// Same as the emoji branch of removeEmoji() in GT重命名.js, but walking
// code points instead of UTF-16 units so surrogate pairs stay intact.
QString RemoveEmoji(const QString &text) {
	QString result;
	result.reserve(text.size());
	for (auto i = 0; i < text.size();) {
		const auto high = text[i];
		auto code = uint32_t(high.unicode());
		auto length = 1;
		if (high.isHighSurrogate() && i + 1 < text.size()) {
			const auto low = text[i + 1];
			if (low.isLowSurrogate()) {
				code = QChar::surrogateToUcs4(high, low);
				length = 2;
			}
		}
		if (code >= 0x23 && code <= 0x39) {
			auto j = i + length;
			if (j < text.size() && text[j].unicode() == 0xFE0F) {
				++j;
			}
			if (j < text.size() && text[j].unicode() == 0x20E3) {
				i = j + 1;
				continue;
			}
		}
		if (!IsEmojiCodePoint(code)) {
			result.append(text.mid(i, length));
		}
		i += length;
	}
	return result;
}

QString Simplized(const QString &text, const Rules &rules) {
	if (rules.traditional.isEmpty()) {
		return text;
	}
	QString result;
	result.reserve(text.size());
	for (const auto &ch : text) {
		const auto index = rules.traditional.indexOf(ch);
		if (index >= 0 && index < rules.simplified.size()) {
			result.append(rules.simplified.at(index));
		} else {
			result.append(ch);
		}
	}
	return result;
}

QString ReplaceWord(const QString &text, const Rules &rules) {
	auto result = text;
	for (const auto &[replacement, words] : rules.replaceRule) {
		for (const auto &word : words) {
			result.replace(word, replacement);
		}
	}
	return result;
}

QStringList TakeType(
		const QStringList &tags,
		const QString &text,
		const Rules &rules) {
	auto result = tags;
	for (const auto &[category, words] : rules.typeRules) {
		const auto found = std::any_of(
			words.begin(),
			words.end(),
			[&](const QString &word) { return text.contains(word); });
		if (found && !result.contains(category)) {
			result.push_back(category);
		}
	}
	return result;
}

QString RemoveByRule(const QString &text, const QString &rule) {
	auto result = text;
	result.remove(QRegularExpression(rule));
	return result;
}

QString RemoveLinks(const QString &text) {
	return RemoveByRule(text, uR"((https?://|www\.)\S+\.?\b)"_q);
}

QString RemoveMentions(const QString &text) {
	return RemoveByRule(text, uR"(@[\w]+)"_q);
}

QString RemoveHashtags(const QString &text) {
	return RemoveByRule(text, uR"(#[^\s]+)"_q);
}

QString CollapseWhitespace(const QString &text) {
	return text
		.replace(QRegularExpression(uR"(\s+)"_q), u" "_q)
		.trimmed();
}

QString RemoveIllegalChars(const QString &text) {
	return RemoveByRule(text, uR"([\/:*?"<>|])"_q);
}

QString ToBaseName(const QString &text, const Rules &rules) {
	auto result = text;
	result = Simplized(result, rules);
	result = RemoveEmoji(result);
	result = ReplaceWord(result, rules);
	result = RemoveLinks(result);
	result.remove(u"\\n"_q);
	result = RemoveMentions(result);
	return result;
}

QString ApplySteps(const QString &text, const Rules &rules) {
	auto result = text;
	auto steps = rules.steps;
	if (steps.empty()) {
		steps = {
			u"simplized"_q,
			u"replaceWord"_q,
			u"removeEmoji"_q,
			u"removeLinks"_q,
			u"removeMentions"_q,
			u"collapseWhitespace"_q,
			u"removeIllegalChars"_q,
			u"truncate"_q,
		};
	}
	for (const auto &step : steps) {
		if (step == u"simplized"_q) {
			result = Simplized(result, rules);
		} else if (step == u"replaceWord"_q) {
			result = ReplaceWord(result, rules);
		} else if (step == u"removeEmoji"_q) {
			result = RemoveEmoji(result);
		} else if (step == u"removeLinks"_q) {
			result = RemoveLinks(result);
		} else if (step == u"removeMentions"_q) {
			result = RemoveMentions(result);
		} else if (step == u"collapseWhitespace"_q) {
			result = CollapseWhitespace(result);
		} else if (step == u"removeIllegalChars"_q) {
			result = RemoveIllegalChars(result);
		} else if (step == u"truncate"_q
			&& result.size() > rules.maxLength) {
			result = result.left(rules.maxLength).trimmed();
		}
	}
	return result;
}

} // namespace

const Rules &Current() {
	if (!gRulesLoaded) {
		Reload();
	}
	return gRulesLoaded ? gRules : Fallback();
}

void StartWatcher() {
	static QFileSystemWatcher *watcher = nullptr;
	if (!watcher) {
		if (const auto app = QCoreApplication::instance()) {
			watcher = new QFileSystemWatcher(app);
			QObject::connect(watcher, &QFileSystemWatcher::fileChanged, [] {
				Reload();
			});
			QObject::connect(
				watcher,
				&QFileSystemWatcher::directoryChanged,
				[](const QString &) { Reload(); });
		}
	}
	const auto path = ResolvePath();
	if (watcher && !path.isEmpty()) {
		watcher->addPath(path);
		watcher->addPath(QFileInfo(path).absolutePath());
	}
	if (!gRulesLoaded) {
		Reload();
	}
}

QString ToFilename(const QString &text) {
	const auto &rules = Current();
	// WHY: the original script reads the "[tags]" prefix off the file name
	// and re-derives it from the text, so a copied caption keeps the tags.
	const auto close = text.indexOf(']');
	auto tags = QStringList();
	const auto baseStart = (close > 0) ? close + 1 : 0;
	if (close > 0) {
		tags = text.mid(1, close - 1).split(' ', Qt::KeepEmptyParts);
	}
	auto result = ApplySteps(text.mid(baseStart), rules);
	// WHY: the steps strip a message made only of a link, an emoji or a
	// mention down to nothing, and an empty clipboard reads as "copying
	// did nothing", so keep the text itself as the name.
	if (result.isEmpty() && baseStart < text.size()) {
		result = CollapseWhitespace(RemoveIllegalChars(text));
	}
	// WHY: a hashtag can carry the type tag, so they are stripped only
	// after the tags have been read off the text.
	const auto typeTags = TakeType(tags, result, rules);
	if (!rules.keepHashtags) {
		result = CollapseWhitespace(RemoveHashtags(result));
	}
	if (typeTags.isEmpty()) {
		return result;
	}
	return u"["_q + typeTags.join(u' ') + u"]"_q + result;
}

void CopyItemTextToClipboard(
		not_null<HistoryItem*> item,
		bool asGroup) {
	const auto text = [&] {
		if (asGroup) {
			const auto group = item->history()->owner().groups().find(
				item.get());
			if (group) {
				const auto groupText = HistoryGroupText(group);
				if (!groupText.empty()) {
					return groupText.expanded;
				}
			}
		}
		return HistoryItemText(item).expanded;
	}();
	TextUtilities::SetClipboardText(
		TextForMimeData::Simple(ToFilename(text)));
}

QString ToCleanName(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	const auto &rules = Current();
	const auto dot = text.lastIndexOf('.');
	const auto suffix = (dot >= 0)
		? text.mid(dot)
		: text.mid(text.size() - 1);
	const auto close = text.indexOf(']');
	auto tags = QStringList();
	if (close > 0) {
		tags = text.mid(1, close - 1).split(' ', Qt::KeepEmptyParts);
	}
	const auto size = text.size();
	const auto baseStart = (close >= 0) ? close + 1 : 0;
	const auto stop = (dot >= 0)
		? dot
		: (size > baseStart ? size - 1 : baseStart);
	const auto length = (stop > baseStart) ? (stop - baseStart) : 0;
	auto result = ToBaseName(text.mid(baseStart, length), rules);
	auto typeTags = TakeType(tags, result, rules);
	if (!rules.keepHashtags) {
		result = CollapseWhitespace(RemoveHashtags(result));
	}
	if (typeTags.empty()) {
		return result + suffix;
	}
	return u"["_q + typeTags.join(u' ') + u"]"_q + result + suffix;
}

} // namespace Ayu::ClipboardRules
