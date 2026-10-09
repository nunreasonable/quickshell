#include "account_age.hpp"
#include <thread>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qt_windows.h>

#include <lm.h>
#include <lmcons.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include "../../core/logcat.hpp"
#include "../startup.hpp"

namespace qs::windows::sys {

namespace {

QS_LOGGING_CATEGORY(logAccountAge, "quickshell.windows.accountage", QtWarningMsg);

QString lookUpKind() {
	wchar_t name[UNLEN + 1] {};
	DWORD size = UNLEN + 1;
	if (!GetUserNameW(name, &size)) return QStringLiteral("unknown");

	LPBYTE buffer = nullptr;
	auto status = NetUserGetInfo(nullptr, name, 24, &buffer);
	if (status != NERR_Success || buffer == nullptr) {
		if (buffer != nullptr) NetApiBufferFree(buffer);
		qCInfo(logAccountAge) << "Not a local SAM user (status" << status << "), treating it as a work, school or domain account";
		return QStringLiteral("other");
	}

	auto* info = reinterpret_cast<USER_INFO_24*>(buffer);
	auto kind = QStringLiteral("local");
	if (info->usri24_internet_identity) {
		auto provider = info->usri24_internet_provider_name != nullptr
		                  ? QString::fromWCharArray(info->usri24_internet_provider_name)
		                  : QString();
		kind = provider.compare(QStringLiteral("MicrosoftAccount"), Qt::CaseInsensitive) == 0
		         ? QStringLiteral("microsoft")
		         : QStringLiteral("other");
	}

	NetApiBufferFree(buffer);
	return kind;
}

QString lookUpAge() {
	auto apartment = true;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (const winrt::hresult_error&) {
		apartment = false;
	}

	auto result = QStringLiteral("unknown");
	try {
		using namespace winrt::Windows::System;
		auto users = User::FindAllAsync().get();
		User user = nullptr;
		for (const auto& candidate: users) {
			if (candidate.Type() == UserType::LocalUser) {
				user = candidate;
				break;
			}
		}
		if (user == nullptr && users.Size() > 0) user = users.GetAt(0);

		if (user != nullptr && !user.try_as<IUser2>()) {
			result = QStringLiteral("unsupported");
		} else if (user != nullptr) {
			switch (user.CheckUserAgeConsentGroupAsync(UserAgeConsentGroup::Adult).get()) {
			case UserAgeConsentResult::Included: result = QStringLiteral("adult"); break;
			case UserAgeConsentResult::NotIncluded: result = QStringLiteral("notAdult"); break;
			default: result = QStringLiteral("unknown"); break;
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCWarning(logAccountAge) << "Couldn't check the account's age group:"
		                      << QString::fromWCharArray(e.message().c_str());
	} catch (...) {
		qCWarning(logAccountAge) << "Couldn't check the account's age group";
	}

	if (apartment) winrt::uninit_apartment();
	return result;
}

} // namespace

AccountAge::AccountAge(QObject* parent): QObject(parent) {
	startup::afterFirstFrame(this, [this]() { this->start(); });
}

void AccountAge::refresh() { this->start(); }

void AccountAge::start() {
	if (this->mRunning) return;
	this->mRunning = true;

	auto self = QPointer<AccountAge>(this);
	std::thread([self]() {
		auto kind = lookUpKind();
		auto age = kind == QStringLiteral("microsoft") ? lookUpAge() : QStringLiteral("unknown");
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [self, kind, age]() {
			    if (self) self->apply(kind, age);
		    },
		    Qt::QueuedConnection
		);
	}).detach();
}

void AccountAge::apply(const QString& kind, const QString& age) {
	this->mRunning = false;
	this->mReady = true;
	this->mKind = kind;
	this->mAge = age;
	qCInfo(logAccountAge) << "Account kind" << kind << "age group" << age;
	emit this->changed();
}

bool AccountAge::spicyAllowed() const {
	return this->mReady && this->mKind == QStringLiteral("microsoft") && this->mAge == QStringLiteral("adult");
}

QString AccountAge::spicyRestriction() const {
	if (!this->mReady) return QStringLiteral("Checking this Windows account...");
	if (this->mKind == QStringLiteral("local")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. This Windows user is a local account.");
	}
	if (this->mKind != QStringLiteral("microsoft")) {
		return QStringLiteral("Spicy Stuff needs a personal Microsoft account aged 18 or older.");
	}
	if (this->mAge == QStringLiteral("notAdult")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. Windows says this account is not an adult account.");
	}
	if (this->mAge == QStringLiteral("unsupported")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. This version of Windows can't confirm an account's age; Windows 11 can.");
	}
	if (this->mAge != QStringLiteral("adult")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. Windows couldn't confirm this account's age.");
	}
	return QString();
}

} // namespace qs::windows::sys
