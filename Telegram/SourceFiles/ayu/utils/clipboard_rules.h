// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

#include <QString>
#include <utility>
#include <vector>

namespace Ayu::ClipboardRules {

struct Rules {
	std::vector<std::pair<QString, std::vector<QString>>> typeRules;
	std::vector<std::pair<QString, std::vector<QString>>> replaceRule;
	QString traditional;
	QString simplified;
	std::vector<QString> steps;
	int maxLength = 80;
	bool keepHashtags = true;
};

// Watches the rules file and reloads it on change (idempotent, main thread).
void StartWatcher();

[[nodiscard]] const Rules &Current();

// Text -> file name, without extension.
[[nodiscard]] QString ToFilename(const QString &text);

// Existing file name -> cleaned name (same as handleName() in GT重命名.js).
[[nodiscard]] QString ToCleanName(const QString &text);

} // namespace Ayu::ClipboardRules
