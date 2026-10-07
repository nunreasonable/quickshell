#include "keyboard.hpp"

#include <algorithm>
#include <vector>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstring.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logKeyboard, "quickshell.windows.keyboard", QtWarningMsg);

Keyboard* g_instance = nullptr;

QString localeNameFromHkl(HKL hkl) {
	auto langId = LOWORD(reinterpret_cast<quintptr>(hkl)); // NOLINT
	auto lcid = MAKELCID(langId, SORT_DEFAULT);

	wchar_t buffer[LOCALE_NAME_MAX_LENGTH] {};
	if (LCIDToLocaleName(lcid, buffer, LOCALE_NAME_MAX_LENGTH, 0) == 0) return QString();
	return QString::fromWCharArray(buffer);
}

QString displayNameForLocale(const QString& localeName) {
	if (localeName.isEmpty()) return QString();

	wchar_t buffer[256] {};
	auto wName = localeName.toStdWString();

	if (GetLocaleInfoEx(wName.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, buffer, 256) > 0) {
		return QString::fromWCharArray(buffer);
	}
	if (GetLocaleInfoEx(wName.c_str(), LOCALE_SENGLISHLANGUAGENAME, buffer, 256) > 0) {
		return QString::fromWCharArray(buffer);
	}
	return localeName;
}

std::vector<HKL> installedLayouts() {
	auto count = GetKeyboardLayoutList(0, nullptr);
	if (count <= 0) return {};

	std::vector<HKL> layouts(static_cast<size_t>(count));
	auto got = GetKeyboardLayoutList(count, layouts.data());
	layouts.resize(static_cast<size_t>(std::max(got, 0)));
	return layouts;
}

void CALLBACK winEventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND /*hwnd*/,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*threadId*/,
    DWORD /*time*/
) {
	if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && g_instance != nullptr) {
		g_instance->refresh();
	}
}

} // namespace

Keyboard::Keyboard(QObject* parent): QObject(parent) {
	g_instance = this;

	this->pollTimer.setInterval(500);
	QObject::connect(&this->pollTimer, &QTimer::timeout, this, &Keyboard::refresh);

	QTimer::singleShot(0, this, &Keyboard::initDeferred);
}

void Keyboard::initDeferred() {
	this->hook = SetWinEventHook(
	    EVENT_SYSTEM_FOREGROUND,
	    EVENT_SYSTEM_FOREGROUND,
	    nullptr,
	    &winEventProc,
	    0,
	    0,
	    WINEVENT_OUTOFCONTEXT
	);
	if (this->hook == nullptr) {
		qCWarning(logKeyboard) << "SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed:" << GetLastError();
	}

	this->refreshLayoutList();
	this->refresh();

	if (this->mActive) this->pollTimer.start();
}

Keyboard::~Keyboard() {
	if (this->hook != nullptr) UnhookWinEvent(this->hook);
	if (g_instance == this) g_instance = nullptr;
}

void Keyboard::setActive(bool active) {
	if (this->mActive == active) return;
	this->mActive = active;
	emit this->activeChanged();

	if (active) {
		this->refresh();
		this->pollTimer.start();
	} else {
		this->pollTimer.stop();
	}
}

void Keyboard::refreshLayoutList() {
	QStringList codes;
	for (auto* hkl: installedLayouts()) {
		auto code = localeNameFromHkl(hkl);
		if (!code.isEmpty() && !codes.contains(code)) codes.append(code);
	}
	this->bLayoutCodes = codes;
}

void Keyboard::refresh() {
	auto* foreground = GetForegroundWindow();
	if (foreground == nullptr) return;

	auto threadId = GetWindowThreadProcessId(foreground, nullptr);
	if (threadId == 0) return;

	auto hkl = GetKeyboardLayout(threadId);
	auto code = localeNameFromHkl(hkl);
	if (code.isEmpty() || code == this->bCurrentLayoutCode.value()) return;

	this->bCurrentLayoutCode = code;
	this->bCurrentLayoutName = displayNameForLocale(code);
}

void Keyboard::activateLayout(const QString& code) {
	for (auto* hkl: installedLayouts()) {
		if (localeNameFromHkl(hkl) != code) continue;

		auto* foreground = GetForegroundWindow();
		if (foreground == nullptr) return;

		PostMessageW(
		    foreground,
		    WM_INPUTLANGCHANGEREQUEST,
		    INPUTLANGCHANGE_FORWARD,
		    reinterpret_cast<LPARAM>(hkl) // NOLINT
		);
		return;
	}

	qCWarning(logKeyboard) << "activateLayout: no installed layout matches" << code;
}

} // namespace qs::windows::sys
