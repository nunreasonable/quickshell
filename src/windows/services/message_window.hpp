#pragma once

#include <functional>
#include <vector>

#include <qt_windows.h>

#include <qhash.h>
#include <qobject.h>
#include <qtclasshelpermacros.h>

namespace qs::windows::services {

// One hidden message-only window shared by every native service backend that needs an HWND
// to receive window messages (RegisterPowerSettingNotification, AddClipboardFormatListener,
// ...). Avoids each singleton creating and pumping its own top level window.
class ServiceMessageWindow {
public:
	using Handler = std::function<void(WPARAM, LPARAM)>;

	static ServiceMessageWindow* instance();

	[[nodiscard]] HWND hwnd() const { return this->mHwnd; }

	// Appends a handler invoked for every message with id `msg`. Handlers are never removed
	// (every subscriber here is a process-lifetime singleton).
	void addHandler(UINT msg, Handler handler);

private:
	ServiceMessageWindow();
	~ServiceMessageWindow();
	Q_DISABLE_COPY_MOVE(ServiceMessageWindow);

	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	HWND mHwnd = nullptr;
	ATOM mClassAtom = 0;
	QHash<UINT, std::vector<Handler>> handlers;
};

} // namespace qs::windows::services
