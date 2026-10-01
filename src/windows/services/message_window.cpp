#include "message_window.hpp"

#include <qt_windows.h>

#include <qcoreapplication.h>

namespace qs::windows::services {

namespace {
const wchar_t* const WINDOW_CLASS_NAME = L"QuickshellServiceMessageWindow";
}

ServiceMessageWindow::ServiceMessageWindow() {
	WNDCLASSEXW wc {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &ServiceMessageWindow::wndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = WINDOW_CLASS_NAME;

	this->mClassAtom = RegisterClassExW(&wc);

	// HWND_MESSAGE: a message-only window, never shown, not part of any z-order.
	this->mHwnd = CreateWindowExW(
	    0,
	    WINDOW_CLASS_NAME,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    this
	);
}

ServiceMessageWindow::~ServiceMessageWindow() {
	if (this->mHwnd != nullptr) DestroyWindow(this->mHwnd);
	if (this->mClassAtom != 0) UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandleW(nullptr));
}

ServiceMessageWindow* ServiceMessageWindow::instance() {
	static auto* instance = new ServiceMessageWindow(); // NOLINT
	return instance;
}

void ServiceMessageWindow::addHandler(UINT msg, Handler handler) {
	this->handlers[msg].push_back(std::move(handler));
}

LRESULT CALLBACK
ServiceMessageWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_CREATE) {
		auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam); // NOLINT
		SetWindowLongPtrW(
		    hwnd,
		    GWLP_USERDATA,
		    reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams) // NOLINT
		);
		return 0;
	}

	auto* self =
	    reinterpret_cast<ServiceMessageWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); // NOLINT

	if (self != nullptr) {
		auto iter = self->handlers.constFind(msg);
		if (iter != self->handlers.constEnd()) {
			for (const auto& handler: iter.value()) {
				handler(wParam, lParam);
			}
		}
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace qs::windows::services
