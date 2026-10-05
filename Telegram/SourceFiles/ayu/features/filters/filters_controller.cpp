// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026

#include "ayu/features/filters/filters_controller.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "ayu/features/filters/filters_utils.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "unicode/regex.h"

#include <memory>
#include <unordered_set>

namespace FiltersController {

std::unordered_set<long long> showingFilteredMessages;

bool filterBlocked(const not_null<HistoryItem*> item) {
	if (item->from() != item->history()->peer) {
		if (isBlocked(item)) {
			return true;
		}
	}
	if (const auto bot = item->viaBot()) {
		if (isBlocked(bot)) {
			return true;
		}
	}
	return false;
}

std::optional<bool> isFiltered(
		const QString &str,
		long long dialogId,
		const std::shared_ptr<const FiltersCacheController::Cache> &cache) {
	if (str.isEmpty()) {
		return std::nullopt;
	}

	const auto icuStr = icu::UnicodeString(reinterpret_cast<const UChar*>(str.constData()), str.length());

	const auto matches = [&](const ReversiblePattern &pattern)
	{
		UErrorCode status = U_ZERO_ERROR;

		const auto matcher = std::unique_ptr<icu::RegexMatcher>(pattern.pattern->matcher(icuStr, status));
		if (U_FAILURE(status) || !matcher) {
			LOG(("FILTER FAILED: %1").arg(u_errorName(status)));
			return false;
		}

		const auto match = matcher->find();
		const auto reversed = pattern.reversed;

		if ((!reversed && match) || (reversed && !match)) {
			return true;
		}
		return false;
	};

	if (const auto i = cache->patternsByDialogId.find(dialogId); i != cache->patternsByDialogId.end()) {
		for (const auto &pattern : i->second) {
			if (matches(pattern)) {
				return true;
			}
		}
	}

	const auto exclusions = cache->exclusionsByDialogId.find(dialogId);
	if (!cache->sharedPatterns.empty()) {
		for (const auto &pattern : cache->sharedPatterns) {
			if (exclusions != cache->exclusionsByDialogId.end() && exclusions->second.contains(pattern)) {
				continue;
			}
			if (matches(pattern.pattern)) {
				return true;
			}
		}
	}
	return false;
}

bool isEnabled(not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled() && (settings.filtersEnabledInChats() || peer->isBroadcast());
}

bool blockedPlaceholder(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (!AyuSettings::getInstance().hideFromBlocked()
		|| !(peer->isChat() || peer->isMegagroup() || peer->isGigagroup())) {
		return false;
	}
	const auto blocked = [](PeerData *sender) {
		const auto user = sender ? sender->asUser() : nullptr;
		return user && user->isBlocked();
	};
	const auto containsBlocked = [&](not_null<HistoryItem*> message) {
		if (blocked(message->from()) || blocked(message->viaBot())) {
			return true;
		}
		if (const auto forwarded = message->Get<HistoryMessageForwarded>()) {
			if (blocked(forwarded->originalSender)) {
				return true;
			}
		}
		return false;
	};
	if (containsBlocked(item)) {
		return true;
	}
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto member : group->items) {
			if (containsBlocked(member)) {
				return true;
			}
		}
	}
	return false;
}

bool blockedReply(
		not_null<HistoryItem*> item,
		not_null<HistoryMessageReply*> reply) {
	const auto peer = item->history()->peer;
	if (!AyuSettings::getInstance().hideFromBlocked()
		|| !(peer->isChat() || peer->isMegagroup() || peer->isGigagroup())) {
		return false;
	}
	const auto blocked = [](PeerData *sender) {
		const auto user = sender ? sender->asUser() : nullptr;
		return user && user->isBlocked();
	};
	if (const auto id = reply->fields().externalSenderId) {
		if (blocked(peer->owner().peerLoaded(id))) {
			return true;
		}
	}
	if (const auto message = reply->resolvedMessage.get()) {
		if (blocked(message->from())
			|| blocked(message->viaBot())
			|| blocked(message->displayFrom())
			|| blockedPlaceholder(message)) {
			return true;
		}
		if (const auto forwarded = message->Get<HistoryMessageForwarded>()) {
			return blocked(forwarded->originalSender);
		}
	}
	return false;
}

bool isBlocked(const not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return false;
	}
	const auto isShadowBanned = [&](PeerData *peer) {
		return peer
			&& (peer->isUser() || peer->isBroadcast())
			&& settings.isShadowBanned(getDialogIdFromPeer(peer));
	};
	if (isShadowBanned(item->from())
		&& item->from()->id != item->history()->peer->id) {
		return true;
	}
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		return isShadowBanned(forwarded->originalSender);
	}

	return false;
}

bool isBlocked(const not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled()
		&& (peer->isUser() || peer->isBroadcast())
		&& settings.isShadowBanned(getDialogIdFromPeer(peer));
}

bool filtered(const not_null<HistoryItem*> item) {
	if (showingFilteredMessages.contains(item->history()->peer->id.value)) {
		return false;
	}

	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return false;
	}

	if (item->out()) {
		return false;
	}

	if (filterBlocked(item)) {
		FiltersCacheController::putHiddenBlockedMessage(item);
		return true;
	}

	if (!isEnabled(item->history()->peer)) return false;

	const auto cached = FiltersCacheController::isFiltered(item);
	if (cached.has_value()) {
		return cached.value();
	}
	const auto group = item->history()->owner().groups().find(item);
	const auto cache = FiltersCacheController::snapshot();
	const auto res = isFiltered(
		FilterUtils::extractAllText(item, group),
		getDialogIdFromPeer(item->history()->peer),
		cache);

	// sometimes item has empty text.
	// so we cache result only if
	// processed item is filterable
	if (res.has_value()) {
		FiltersCacheController::putFiltered(item, group, res.value(), cache);
		return res.value();
	}
	return false;
}

std::optional<bool> filteredMessagesShown(not_null<PeerData*> peer) {
	if (!showingFilteredMessages.contains(peer->id.value)
		&& !FiltersCacheController::hasFilteredMessages(peer)) {
		return std::nullopt;
	}
	return showingFilteredMessages.contains(peer->id.value);
}

void toggleFilteredMessagesShown(not_null<PeerData*> peer) {
	if (showingFilteredMessages.contains(peer->id.value)) {
		showingFilteredMessages.erase(peer->id.value);
	} else {
		showingFilteredMessages.insert(peer->id.value);
	}
	FiltersCacheController::fireUpdate();
}

void invalidate(not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return;
	}

	FiltersCacheController::invalidate(item);
}

}
