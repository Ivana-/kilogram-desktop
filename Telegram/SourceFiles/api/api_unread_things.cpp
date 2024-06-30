/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_unread_things.h"

#include <iostream> // kg

#include "data/data_peer.h"
#include "data/data_channel.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_unread_things.h"
#include "apiwrap.h"

namespace Api {
namespace {

constexpr auto kPreloadIfLess = 5;
constexpr auto kFirstRequestLimit = 10;
constexpr auto kNextRequestLimit = 100;

} // namespace

UnreadThings::UnreadThings(not_null<ApiWrap*> api) : _api(api) {
}

bool UnreadThings::trackMentions(Data::Thread *thread) const {
	const auto peer = thread ? thread->peer().get() : nullptr;
	return peer && (peer->isChat() || peer->isMegagroup());
}

bool UnreadThings::trackReactions(Data::Thread *thread) const {
	const auto peer = thread ? thread->peer().get() : nullptr;
	return peer && (peer->isUser() || peer->isChat() || peer->isMegagroup());
}

void UnreadThings::preloadEnough(Data::Thread *thread) {
	if (trackMentions(thread)) {
		preloadEnoughMentions(thread);
	}
	if (trackReactions(thread)) {
		preloadEnoughReactions(thread);
	}
}

void UnreadThings::mediaAndMentionsRead(
		const base::flat_set<MsgId> &readIds,
		ChannelData *channel) {
	for (const auto &msgId : readIds) {
		_api->requestMessageData(channel, msgId, [=] {
			const auto item = channel
				? _api->session().data().message(channel->id, msgId)
				: _api->session().data().nonChannelMessage(msgId);
			if (item && item->mentionsMe()) {
				item->markMediaAndMentionRead();
			}
		});
	}
}

void UnreadThings::preloadEnoughMentions(not_null<Data::Thread*> thread) {
	const auto fullCount = thread->unreadMentions().count();
	const auto loadedCount = thread->unreadMentions().loadedCount();
	const auto allLoaded = (fullCount >= 0) && (loadedCount >= fullCount);
	if (fullCount >= 0 && loadedCount < kPreloadIfLess && !allLoaded) {
		requestMentions(thread, loadedCount);
	}
}

void UnreadThings::preloadEnoughReactions(not_null<Data::Thread*> thread) {
	const auto fullCount = thread->unreadReactions().count();
	const auto loadedCount = thread->unreadReactions().loadedCount();
	const auto allLoaded = (fullCount >= 0) && (loadedCount >= fullCount);
	if (fullCount >= 0 && loadedCount < kPreloadIfLess && !allLoaded) {
		requestReactions(thread, loadedCount);
	}
}

void UnreadThings::cancelRequests(not_null<Data::Thread*> thread) {
	if (const auto requestId = _mentionsRequests.take(thread)) {
		_api->request(*requestId).cancel();
	}
	if (const auto requestId = _reactionsRequests.take(thread)) {
		_api->request(*requestId).cancel();
	}
}

void UnreadThings::requestMentions(
		not_null<Data::Thread*> thread,
		int loaded) {
	if (_mentionsRequests.contains(thread)) {
		return;
	}
	const auto offsetId = std::max(
		thread->unreadMentions().maxLoaded(),
		MsgId(1));
	const auto limit = loaded ? kNextRequestLimit : kFirstRequestLimit;
	const auto addOffset = loaded ? -(limit + 1) : -limit;
	const auto maxId = 0;
	const auto minId = 0;
	const auto history = thread->owningHistory();
	const auto topic = thread->asTopic();
	using Flag = MTPmessages_GetUnreadMentions::Flag;
	const auto requestId = _api->request(MTPmessages_GetUnreadMentions(
		MTP_flags(topic ? Flag::f_top_msg_id : Flag()),
		history->peer->input,
		MTP_int(topic ? topic->rootId() : 0),
		MTP_int(offsetId),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(maxId),
		MTP_int(minId)
	)).done([=](const MTPmessages_Messages &result) {
		_mentionsRequests.remove(thread);
		thread->unreadMentions().addSlice(result, loaded);
	}).fail([=] {
		_mentionsRequests.remove(thread);
	}).send();
	_mentionsRequests.emplace(thread, requestId);
}

void UnreadThings::requestReactions(
		not_null<Data::Thread*> thread,
		int loaded) {
	if (_reactionsRequests.contains(thread)) {
		return;
	}
	const auto offsetId = loaded
		? std::max(thread->unreadReactions().maxLoaded(), MsgId(1))
		: MsgId(1);
	const auto limit = loaded ? kNextRequestLimit : kFirstRequestLimit;
	const auto addOffset = loaded ? -(limit + 1) : -limit;
	const auto maxId = 0;
	const auto minId = 0;
	const auto history = thread->owningHistory();
	const auto topic = thread->asTopic();
	using Flag = MTPmessages_GetUnreadReactions::Flag;
	const auto requestId = _api->request(MTPmessages_GetUnreadReactions(
		MTP_flags(topic ? Flag::f_top_msg_id : Flag()),
		history->peer->input,
		MTP_int(topic ? topic->rootId() : 0),
		MTP_int(offsetId),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(maxId),
		MTP_int(minId)
	)).done([=](const MTPmessages_Messages &result) {
		_reactionsRequests.remove(thread);
		thread->unreadReactions().addSlice(result, loaded);
	}).fail([=] {
		_reactionsRequests.remove(thread);
	}).send();
	_reactionsRequests.emplace(thread, requestId);
}

// kg begin

void UnreadThings::resetUnreadMentionsCountExcludingBlockedUsers(
		not_null<Data::Thread*> thread,
		const int total_unread_mentions_count) {
	if (!_api->session().kgMode()
	    || total_unread_mentions_count == 0
		|| !trackMentions(thread)
		|| _mentionsRequests.contains(thread)) {
		thread->unreadMentions().setCount(total_unread_mentions_count);
		return;
	}
	const auto offsetId = MsgId(1);
	const auto limit = total_unread_mentions_count; // kNextRequestLimit; ?
	const auto addOffset = -limit;
	const auto maxId = 0;
	const auto minId = 0;
	const auto history = thread->owningHistory();
	const auto topic = thread->asTopic();
	using Flag = MTPmessages_GetUnreadMentions::Flag;
	const auto requestId = _api->request(MTPmessages_GetUnreadMentions(
		MTP_flags(topic ? Flag::f_top_msg_id : Flag()),
		history->peer->input,
		MTP_int(topic ? topic->rootId() : 0),
		MTP_int(offsetId),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(maxId),
		MTP_int(minId)
	)).done([=](const MTPmessages_Messages &result) {
		_mentionsRequests.remove(thread);

		std::cout << "UnreadThings::resetUnreadMentionsCountExcludingBlockedUsers done:\n";

		const auto list = result.match([](const MTPDmessages_messagesNotModified &) {
			return (const QVector<MTPMessage>*)nullptr;
		}, [](const auto &data) {
			return &data.vmessages().v;
		});
		auto ids_to_read_contents = QVector<MTPint>();
		if (list) {
			for (const auto &message : *list) {

				std::cout << "Author ID : " << AuthorIDFromMessage(message) << "\n";

				if (_api->session().kgModeAndUserIsBlocked(AuthorIDFromMessage(message))) {
					ids_to_read_contents.push_back(MTP_int(IdFromMessage(message)));
				}
			}
		}
		readMessageContents(thread->peer(), ids_to_read_contents);
        const auto updated_count = total_unread_mentions_count - ids_to_read_contents.size();

		std::cout << "unread_mentions_count_from_non_blocked : " << updated_count << "\n";

		thread->unreadMentions().setCount(updated_count);
	}).fail([=] {
		_mentionsRequests.remove(thread);
		thread->unreadMentions().setCount(total_unread_mentions_count);
	}).send();

	_mentionsRequests.emplace(thread, requestId);
}

void UnreadThings::resetUnreadReactionsCountExcludingBlockedUsers(
		not_null<Data::Thread*> thread,
		const int total_unread_reactions_count) {
	if (!_api->session().kgMode()
	    || total_unread_reactions_count == 0
		|| !trackReactions(thread)
		|| _reactionsRequests.contains(thread)) {
		thread->unreadReactions().setCount(total_unread_reactions_count);
		return;
	}
	const auto offsetId = MsgId(1);
	const auto limit = total_unread_reactions_count; // kNextRequestLimit; ?
	const auto addOffset = -limit;
	const auto maxId = 0;
	const auto minId = 0;
	const auto history = thread->owningHistory();
	const auto topic = thread->asTopic();
	using Flag = MTPmessages_GetUnreadReactions::Flag;
	const auto requestId = _api->request(MTPmessages_GetUnreadReactions(
		MTP_flags(topic ? Flag::f_top_msg_id : Flag()),
		history->peer->input,
		MTP_int(topic ? topic->rootId() : 0),
		MTP_int(offsetId),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(maxId),
		MTP_int(minId)
	)).done([=](const MTPmessages_Messages &result) {
		_reactionsRequests.remove(thread);

		std::cout << "UnreadThings::resetUnreadReactionsCountExcludingBlockedUsers done:\n";

		const auto list = result.match([](const MTPDmessages_messagesNotModified &) {
			return (const QVector<MTPMessage>*)nullptr;
		}, [](const auto &data) {
			return &data.vmessages().v;
		});
		if (!list || list->size() == 0) {
			thread->unreadReactions().setCount(total_unread_reactions_count);
			return;
		}

		// request MTPmessages_GetMessagesReactions with messages ids from list

		auto ids = QVector<MTPint>();
		ids.reserve(list->size());
		for (const auto &message : *list) {
			// std::cout << "Author ID : " << AuthorIDFromMessage(message) << "\n";
			ids.push_back(MTP_int(IdFromMessage(message)));
		}
		// or this way
		// ranges::transform(
		// 	*list,
		// 	ranges::back_inserter(ids),
		// 	[](const auto &message) { return MTP_int(IdFromMessage(message)); });

		_api->request(MTPmessages_GetMessagesReactions(
			history->peer->input,
			MTP_vector<MTPint>(ids)
		)).done([=](const MTPUpdates &updates) {
			const auto processUpdates = [&](const auto &updates) {
				auto ids_to_read_contents = QVector<MTPint>();
				for (const auto &update : updates.v) {
					if (update.type() == mtpc_updateMessageReactions) {
						const auto &d = update.c_updateMessageReactions();

						std::cout << "mtpc_updateMessageReactions - message ID : " << d.vmsg_id().v << "\n";

						if (!hasUnreadReactionFromNonBlockedUser(d.vreactions())) {
							ids_to_read_contents.push_back(MTP_int(d.vmsg_id().v));
						}
					}
				}
				readMessageContents(thread->peer(), ids_to_read_contents);
				const auto updated_count = total_unread_reactions_count - ids_to_read_contents.size();

				std::cout << "unread_reactions_count_from_non_blocked : " << updated_count << "\n";

				thread->unreadReactions().setCount(updated_count);
			};

			switch (updates.type()) {
			case mtpc_updates: {
				auto &d = updates.c_updates();
				// std::cout << "mtpc_updates:" << "\n";
				processUpdates(d.vupdates());
			} break;

			case mtpc_updatesCombined: {
				auto &d = updates.c_updatesCombined();
				// std::cout << "mtpc_updatesCombined:" << "\n";
				processUpdates(d.vupdates());
			} break;

			// case mtpc_updateShort: {
			// 	auto &d = updates.c_updateShort();
			// 	std::cout << "mtpc_updateShort:" << "\n";
			// 	feedUpdate(d.vupdate());
			// } break;

			// case mtpc_updateShortMessage: {
			// 	auto &d = updates.c_updateShortMessage();
			// 	std::cout << "mtpc_updateShortMessage:" << "\n";
			// } break;

			// case mtpc_updateShortChatMessage: {
			// 	auto &d = updates.c_updateShortChatMessage();
			// 	std::cout << "mtpc_updateShortChatMessage:" << "\n";
			// } break;

			// case mtpc_updateShortSentMessage: {
			// 	auto &d = updates.c_updateShortSentMessage();
			// 	std::cout << "mtpc_updateShortSentMessage:" << "\n";
			// } break;

			// case mtpc_updatesTooLong: {
			// 	std::cout << "mtpc_updateShortSentMessage:" << "\n";
			// } break;
			}
		}).fail([=] {
			thread->unreadReactions().setCount(total_unread_reactions_count);
		}).send();
	}).fail([=] {
		_reactionsRequests.remove(thread);
		thread->unreadReactions().setCount(total_unread_reactions_count);
	}).send();

	_reactionsRequests.emplace(thread, requestId);
}

// see Data::Reactions::HasUnread(d.vreactions())
bool UnreadThings::hasUnreadReactionFromNonBlockedUser(const MTPMessageReactions &data) {
	return data.match([&](const MTPDmessageReactions &data) {
		if (const auto &recent = data.vrecent_reactions()) {
			for (const auto &one : recent->v) {
				if (one.match([&](const MTPDmessagePeerReaction &data) {
					const auto peerId = peerFromMTP(data.vpeer_id());

					std::cout << "Reactor ID : " << peerId.value << "\n";

					return data.is_unread() && !_api->session().kgModeAndUserIsBlocked(peerId.value);
				})) {
					return true;
				}
			}
		}
		return false;
	});
}

void UnreadThings::readMessageContents(not_null<PeerData*> peer, const QVector<MTPint>& ids) {
	if (ids.isEmpty()) return;

	if (const auto channel = peer->asChannel()) {

		for (const auto &id : ids) {
			std::cout << "UnreadThings::readMessageContents from channel : " << id.v << "\n";
		}

		_api->request(MTPchannels_ReadMessageContents(
			channel->inputChannel,
			MTP_vector<MTPint>(ids)
		)).done([=](const auto &result) {
			std::cout << "ReadMessageContents DONE by channel\n";
		}).send();
	} else {

		for (const auto &id : ids) {
			std::cout << "UnreadThings::readMessageContents : " << id.v << "\n";
		}

		_api->request(MTPmessages_ReadMessageContents(
			MTP_vector<MTPint>(ids)
		)).done([=](const MTPmessages_AffectedMessages &result) {
			std::cout << "ReadMessageContents DONE by message\n";
		}).send();
	}
}

// kg end

} // namespace UnreadThings
