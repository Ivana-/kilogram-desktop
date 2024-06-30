/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_session.h"

#include "apiwrap.h"

#include <iostream> // kg
#include "data/data_histories.h" // kg

#include "api/api_peer_colors.h"
#include "api/api_updates.h"
#include "api/api_user_privacy.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"
#include "main/main_app_config.h"
#include "main/session/send_as_peers.h"
#include "mtproto/mtproto_config.h"
#include "chat_helpers/stickers_emoji_pack.h"
#include "chat_helpers/stickers_dice_pack.h"
#include "chat_helpers/stickers_gift_box_pack.h"
#include "history/view/reactions/history_view_reactions_strip.h"
#include "history/history.h"
#include "history/history_item.h"
#include "inline_bots/bot_attach_web_view.h"
#include "storage/file_download.h"
#include "storage/download_manager_mtproto.h"
#include "storage/file_upload.h"
#include "storage/storage_account.h"
#include "storage/storage_facade.h"
#include "data/components/factchecks.h"
#include "data/components/recent_peers.h"
#include "data/components/scheduled_messages.h"
#include "data/components/sponsored_messages.h"
#include "data/components/top_peers.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_download_manager.h"
#include "data/stickers/data_stickers.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/window_lock_widgets.h"
#include "base/unixtime.h"
#include "calls/calls_instance.h"
#include "support/support_helper.h"
#include "lang/lang_keys.h"
#include "core/application.h"
#include "ui/text/text_utilities.h"
#include "ui/layers/generic_box.h"
#include "styles/style_layers.h"

#ifndef TDESKTOP_DISABLE_SPELLCHECK
#include "chat_helpers/spellchecker_common.h"
#endif // TDESKTOP_DISABLE_SPELLCHECK

namespace Main {
namespace {

constexpr auto kTmpPasswordReserveTime = TimeId(10);

[[nodiscard]] QString ValidatedInternalLinksDomain(
		not_null<const Session*> session) {
	// This domain should start with 'http[s]://' and end with '/'.
	// Like 'https://telegram.me/' or 'https://t.me/'.
	const auto &domain = session->serverConfig().internalLinksDomain;
	const auto prefixes = {
		u"https://"_q,
		u"http://"_q,
	};
	for (const auto &prefix : prefixes) {
		if (domain.startsWith(prefix, Qt::CaseInsensitive)) {
			return domain.endsWith('/')
				? domain
				: MTP::ConfigFields().internalLinksDomain;
		}
	}
	return MTP::ConfigFields().internalLinksDomain;
}

} // namespace

Session::Session(
	not_null<Account*> account,
	const MTPUser &user,
	std::unique_ptr<SessionSettings> settings)
: _userId(user.c_user().vid())
, _account(account)
, _settings(std::move(settings))
, _changes(std::make_unique<Data::Changes>(this))
, _api(std::make_unique<ApiWrap>(this))
, _updates(std::make_unique<Api::Updates>(this))
, _sendProgressManager(std::make_unique<Api::SendProgressManager>(this))
, _downloader(std::make_unique<Storage::DownloadManagerMtproto>(_api.get()))
, _uploader(std::make_unique<Storage::Uploader>(_api.get()))
, _storage(std::make_unique<Storage::Facade>())
, _data(std::make_unique<Data::Session>(this))
, _user(_data->processUser(user))
, _emojiStickersPack(std::make_unique<Stickers::EmojiPack>(this))
, _diceStickersPacks(std::make_unique<Stickers::DicePacks>(this))
, _giftBoxStickersPacks(std::make_unique<Stickers::GiftBoxPack>(this))
, _sendAsPeers(std::make_unique<SendAsPeers>(this))
, _attachWebView(std::make_unique<InlineBots::AttachWebView>(this))
, _recentPeers(std::make_unique<Data::RecentPeers>(this))
, _scheduledMessages(std::make_unique<Data::ScheduledMessages>(this))
, _sponsoredMessages(std::make_unique<Data::SponsoredMessages>(this))
, _topPeers(std::make_unique<Data::TopPeers>(this))
, _factchecks(std::make_unique<Data::Factchecks>(this))
, _cachedReactionIconFactory(std::make_unique<ReactionIconFactory>())
, _supportHelper(Support::Helper::Create(this))
, _saveSettingsTimer([=] { saveSettings(); }) {
	Expects(_settings != nullptr);

	const auto constructor = [&]() { // kg - move all the initial constructor code under lambda

	_api->requestTermsUpdate();
	_api->requestFullPeer(_user);

	_api->instance().setUserPhone(_user->phone());

	// Load current userpic and keep it loaded.
	_user->loadUserpic();
	changes().peerFlagsValue(
		_user,
		Data::PeerUpdate::Flag::Photo
	) | rpl::start_with_next([=] {
		auto view = Ui::PeerUserpicView{ .cloud = _selfUserpicView };
		[[maybe_unused]] const auto image = _user->userpicCloudImage(view);
		_selfUserpicView = view.cloud;
	}, lifetime());

	crl::on_main(this, [=] {
		using Flag = Data::PeerUpdate::Flag;
		changes().peerUpdates(
			_user,
			Flag::Name
			| Flag::Username
			| Flag::Photo
			| Flag::About
			| Flag::PhoneNumber
		) | rpl::start_with_next([=](const Data::PeerUpdate &update) {
			local().writeSelf();

			if (update.flags & Flag::PhoneNumber) {
				const auto phone = _user->phone();
				_api->instance().setUserPhone(phone);
				if (!phone.isEmpty()) {
					_api->instance().requestConfig();
				}
			}
		}, _lifetime);

#ifndef OS_MAC_STORE
		appConfig().value(
		) | rpl::start_with_next([=] {
			_premiumPossible = !appConfig().get<bool>(
				u"premium_purchase_blocked"_q,
				true);
		}, _lifetime);
#endif // OS_MAC_STORE

		if (_settings->hadLegacyCallsPeerToPeerNobody()) {
			api().userPrivacy().save(
				Api::UserPrivacy::Key::CallsPeer2Peer,
				Api::UserPrivacy::Rule{
					.option = Api::UserPrivacy::Option::Nobody
				});
			saveSettingsDelayed();
		}

		// Storage::Account uses Main::Account::session() in those methods.
		// So they can't be called during Main::Session construction.
		local().readInstalledStickers();
		local().readInstalledMasks();
		local().readInstalledCustomEmoji();
		local().readFeaturedStickers();
		local().readFeaturedCustomEmoji();
		local().readRecentStickers();
		local().readRecentMasks();
		local().readFavedStickers();
		local().readSavedGifs();
		data().stickers().notifyUpdated(Data::StickersType::Stickers);
		data().stickers().notifyUpdated(Data::StickersType::Masks);
		data().stickers().notifyUpdated(Data::StickersType::Emoji);
		data().stickers().notifySavedGifsUpdated();
	});

#ifndef TDESKTOP_DISABLE_SPELLCHECK
	Spellchecker::Start(this);
#endif // TDESKTOP_DISABLE_SPELLCHECK

	_api->requestNotifySettings(MTP_inputNotifyUsers());
	_api->requestNotifySettings(MTP_inputNotifyChats());
	_api->requestNotifySettings(MTP_inputNotifyBroadcasts());

	Core::App().downloadManager().trackSession(this);


	for (auto& id : _blockedPeersIDs) {
		std::cout << id << "\n";
    }
	std::cout << "Total: " << _blockedPeersIDs.size() << "\n";

	};

    // const auto query_blocked_users = [&]() {

	std::cout << "Request 100 Blocked Peers STARTED:\n";

	_api->request(MTPcontacts_GetBlocked(
		MTP_flags(0),
		MTP_int(0), // offset
		MTP_int(100) // limit
	)).done([=](const MTPcontacts_Blocked &result) {

	    std::cout << "Success!\n";

		const auto process = [&](const QVector<MTPPeerBlocked> &list) {
			int i = 0;
			for (const auto &contact : list) {
				contact.match([&](const MTPDpeerBlocked &data) {

					i++;
					std::cout << "BlockedPeer " << i << ": " << peerFromMTP(data.vpeer_id()).value << " " << data.vdate().v << "\n";

					_blockedPeersIDs.insert(peerFromMTP(data.vpeer_id()).value);
				});
				_blockedPeersIDs.insert(317834996); // Liscript Bot
			}
			return 0;
		};
		result.match([&](const MTPDcontacts_blockedSlice &data) {
			return process(data.vblocked().v);
		}, [&](const MTPDcontacts_blocked &data) {
			return process(data.vblocked().v);
		});

        constructor();
	}).fail([=] {
	    std::cout << "Fail query_blocked_users!...\n";
        constructor();
	}).send();
	// }; // query_blocked_users

	// _api->request(MTPusers_GetFullUser(_user->asUser()->inputUser
	// )).done([=](const MTPusers_UserFull &result) {
	// 	result.match([&](const MTPDusers_userFull &data) {
	// 		data.vfull_user().match([&](const MTPDuserFull &fields) {
	// 			//Data::ApplyUserUpdate(user, fields);
	// 			std::cout << "Ok GetFullUser\n";
	// 			QString about = qs(fields.vabout().value_or_empty());
	// 			int last_space_pos = 0;
	// 			// for (const auto &callback : callbacks)
	// 			for (int i=0; i<about.size(); i++) if (about[i] == QChar(' ')) last_space_pos = i;
	// 			for (int i=last_space_pos+1; i<about.size(); i++) {
	// 				std::cout << QString(about[i]).toStdString() << "\n";
	// 			}
	// 		});
	// 	});

	// 	query_blocked_users();
	// }).fail([=] {
	//     std::cout << "Fail GetFullUser!...\n";
    //     constructor();
	// }).send();

// kg end
}

void Session::setTmpPassword(const QByteArray &password, TimeId validUntil) {
	if (_tmpPassword.isEmpty() || validUntil > _tmpPasswordValidUntil) {
		_tmpPassword = password;
		_tmpPasswordValidUntil = validUntil;
	}
}

QByteArray Session::validTmpPassword() const {
	return (_tmpPasswordValidUntil
		>= base::unixtime::now() + kTmpPasswordReserveTime)
		? _tmpPassword
		: QByteArray();
}

// Can be called only right before ~Session.
void Session::finishLogout() {
	unlockTerms();
	data().clear();
	data().clearLocalStorage();
}

Session::~Session() {
	unlockTerms();
	data().clear();
	ClickHandler::clearActive();
	ClickHandler::unpressed();
}

Account &Session::account() const {
	return *_account;
}

Storage::Account &Session::local() const {
	return _account->local();
}

Domain &Session::domain() const {
	return _account->domain();
}

Storage::Domain &Session::domainLocal() const {
	return _account->domainLocal();
}

AppConfig &Session::appConfig() const {
	return _account->appConfig();
}

void Session::notifyDownloaderTaskFinished() {
	downloader().notifyTaskFinished();
}

rpl::producer<> Session::downloaderTaskFinished() const {
	return downloader().taskFinished();
}

bool Session::premium() const {
	return _user->isPremium();
}

bool Session::premiumPossible() const {
	return premium() || premiumCanBuy();
}

bool Session::premiumBadgesShown() const {
	return supportMode() || premiumPossible();
}

rpl::producer<bool> Session::premiumPossibleValue() const {
	using namespace rpl::mappers;

	auto premium = _user->flagsValue(
	) | rpl::filter([=](UserData::Flags::Change change) {
		return (change.diff & UserDataFlag::Premium);
	}) | rpl::map([=] {
		return _user->isPremium();
	});
	return rpl::combine(
		std::move(premium),
		_premiumPossible.value(),
		_1 || _2);
}

bool Session::premiumCanBuy() const {
	return _premiumPossible.current();
}

rpl::producer<uint64> Session::creditsValue() const {
	return _credits.value();
}

void Session::setCredits(uint64 credits) {
	_credits = credits;
}

bool Session::isTestMode() const {
	return mtp().isTestMode();
}

uint64 Session::uniqueId() const {
	// See also Account::willHaveSessionUniqueId.
	return userId().bare
		| (isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
}

UserId Session::userId() const {
	return _userId;
}

PeerId Session::userPeerId() const {
	return _userId;
}

bool Session::validateSelf(UserId id) {
	if (id != userId()) {
		LOG(("Auth Error: wrong self user received."));
		crl::on_main(this, [=] { _account->logOut(); });
		return false;
	}
	return true;
}

void Session::saveSettings() {
	local().writeSessionSettings();
}

void Session::saveSettingsDelayed(crl::time delay) {
	_saveSettingsTimer.callOnce(delay);
}

void Session::saveSettingsNowIfNeeded() {
	if (_saveSettingsTimer.isActive()) {
		_saveSettingsTimer.cancel();
		saveSettings();
	}
}

MTP::DcId Session::mainDcId() const {
	return _account->mtp().mainDcId();
}

MTP::Instance &Session::mtp() const {
	return _account->mtp();
}

const MTP::ConfigFields &Session::serverConfig() const {
	return _account->mtp().configValues();
}

void Session::lockByTerms(const Window::TermsLock &data) {
	if (!_termsLock || *_termsLock != data) {
		_termsLock = std::make_unique<Window::TermsLock>(data);
		_termsLockChanges.fire(true);
	}
}

void Session::unlockTerms() {
	if (_termsLock) {
		_termsLock = nullptr;
		_termsLockChanges.fire(false);
	}
}

void Session::termsDeleteNow() {
	api().request(MTPaccount_DeleteAccount(
		MTP_flags(0),
		MTP_string("Decline ToS update"),
		MTPInputCheckPasswordSRP()
	)).send();
}

std::optional<Window::TermsLock> Session::termsLocked() const {
	return _termsLock ? base::make_optional(*_termsLock) : std::nullopt;
}

rpl::producer<bool> Session::termsLockChanges() const {
	return _termsLockChanges.events();
}

rpl::producer<bool> Session::termsLockValue() const {
	return rpl::single(
		_termsLock != nullptr
	) | rpl::then(termsLockChanges());
}

QString Session::createInternalLink(const QString &query) const {
	return createInternalLink(TextWithEntities{ .text = query }).text;
}

QString Session::createInternalLinkFull(const QString &query) const {
	return createInternalLinkFull(TextWithEntities{ .text = query }).text;
}

TextWithEntities Session::createInternalLink(
		const TextWithEntities &query) const {
	const auto result = createInternalLinkFull(query);
	const auto prefixes = {
		u"https://"_q,
		u"http://"_q,
	};
	for (auto &prefix : prefixes) {
		if (result.text.startsWith(prefix, Qt::CaseInsensitive)) {
			return Ui::Text::Mid(result, prefix.size());
		}
	}
	LOG(("Warning: bad internal url '%1'").arg(result.text));
	return result;
}

TextWithEntities Session::createInternalLinkFull(
		TextWithEntities query) const {
	return TextWithEntities::Simple(ValidatedInternalLinksDomain(this))
		.append(std::move(query));
}

bool Session::supportMode() const {
	return (_supportHelper != nullptr);
}

Support::Helper &Session::supportHelper() const {
	Expects(supportMode());

	return *_supportHelper;
}

Support::Templates& Session::supportTemplates() const {
	return supportHelper().templates();
}

void Session::addWindow(not_null<Window::SessionController*> controller) {
	_windows.emplace(controller);
	controller->lifetime().add([=] {
		_windows.remove(controller);
	});
	updates().addActiveChat(controller->activeChatChanges(
	) | rpl::map([=](Dialogs::Key chat) {
		return chat.peer();
	}) | rpl::distinct_until_changed());
}

bool Session::uploadsInProgress() const {
	return !!_uploader->currentUploadId();
}

void Session::uploadsStopWithConfirmation(Fn<void()> done) {
	const auto id = _uploader->currentUploadId();
	const auto message = data().message(id);
	const auto exists = (message != nullptr);
	const auto window = message
		? Core::App().windowFor(message->history()->peer)
		: Core::App().activePrimaryWindow();
	if (!window) {
		done();
		return;
	}
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				tr::lng_upload_sure_stop(),
				st::boxLabel),
			st::boxPadding + QMargins(0, 0, 0, st::boxPadding.bottom()));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_selected_upload_stop(), [=] {
			box->closeBox();

			uploadsStop();
			if (done) {
				done();
			}
		}, st::attentionBoxButton);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		if (exists) {
			box->addLeftButton(tr::lng_upload_show_file(), [=] {
				box->closeBox();

				if (const auto item = data().message(id)) {
					if (const auto window = tryResolveWindow()) {
						window->showMessage(item);
					}
				}
			});
		}
	});
	window->show(std::move(box));
	window->activate();
}

void Session::uploadsStop() {
	_uploader->cancelAll();
}

auto Session::windows() const
-> const base::flat_set<not_null<Window::SessionController*>> & {
	return _windows;
}

Window::SessionController *Session::tryResolveWindow(
		PeerData *forPeer) const {
	if (forPeer) {
		auto primary = (Window::SessionController*)nullptr;
		for (const auto &window : _windows) {
			if (window->singlePeer() == forPeer) {
				return window;
			} else if (window->isPrimary()) {
				primary = window;
			}
		}
		if (primary) {
			return primary;
		}
	}
	if (_windows.empty() || forPeer) {
		domain().activate(_account);
		if (_windows.empty()) {
			return nullptr;
		}
	}
	for (const auto &window : _windows) {
		if (window->isPrimary()) {
			return window;
		}
	}
	return _windows.front();
}

auto Session::colorIndicesValue()
-> rpl::producer<Ui::ColorIndicesCompressed> {
	return api().peerColors().indicesValue();
}

// kg begin
void Session::toggleKgMode() {
	_kgMode = !_kgMode;

	std::cout << "Session::toggleKgMode:" << _kgMode << "\n";

	data().histories().kgRefreshAll(false);

	// for (const auto &window : _windows) {
	// 	if (window->singlePeer() == forPeer) {
	// 		return window;
	// 	} else if (window->isPrimary()) {
	// 		primary = window;
	// 	}
	// }
}

void Session::addUserToBlocked(BareId value) {
	_blockedPeersIDs.insert(value);
	data().histories().kgRefreshAll(true);
}

void Session::removeUserFromBlocked(BareId value) {
	_blockedPeersIDs.erase(value);
	data().histories().kgRefreshAll(true);
}
// kg end

} // namespace Main
