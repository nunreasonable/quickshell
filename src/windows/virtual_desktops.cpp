#include "virtual_desktops.hpp"

#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qstring.h>
#include <qthread.h>
#include <qtypes.h>

#include "util.hpp"

// last: pulls in the rpc headers, which define macros like `small`
#include <objbase.h>
#include <shobjidl_core.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logDesktops, "quickshell.windows.desktops", QtWarningMsg);

constexpr const wchar_t* DESKTOPS_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr const wchar_t* SESSION_KEY_FORMAT =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\%1\\VirtualDesktops";

// Message VirtualDesktopAccessor posts to the listener window when the desktop changes.
constexpr UINT ACCESSOR_MESSAGE = WM_APP + 0x100;
constexpr const wchar_t* LISTENER_CLASS = L"QuickshellVirtualDesktops";

// Hyprland style configs happily reference workspace 10 with three desktops open; creating
// desktops on demand is the mapping, but keep a sane upper bound.
constexpr qsizetype MAX_DESKTOPS = 20;

bool readBinary(HKEY key, const wchar_t* name, QByteArray& out) {
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
	if (type != REG_BINARY || size == 0) return false;

	out.resize(static_cast<qsizetype>(size));
	auto* data = reinterpret_cast<BYTE*>(out.data());
	return RegQueryValueExW(key, name, nullptr, nullptr, data, &size) == ERROR_SUCCESS;
}

QString readString(HKEY key, const wchar_t* name) {
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return {};
	if (type != REG_SZ || size < sizeof(wchar_t)) return {};

	auto buffer = QByteArray(static_cast<qsizetype>(size), '\0');
	auto* data = reinterpret_cast<BYTE*>(buffer.data());
	if (RegQueryValueExW(key, name, nullptr, nullptr, data, &size) != ERROR_SUCCESS) return {};

	auto* chars = reinterpret_cast<const wchar_t*>(buffer.constData());
	return QString::fromWCharArray(chars, static_cast<qsizetype>(size / sizeof(wchar_t))).trimmed()
	    .remove(QChar('\0'));
}

GUID guidAt(const QByteArray& bytes, qsizetype offset) {
	GUID guid {};
	memcpy(&guid, bytes.constData() + offset, sizeof(GUID));
	return guid;
}

bool guidIsNull(const GUID& guid) {
	static const GUID null {};
	return IsEqualGUID(guid, null);
}

bool isOwnWindow(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

} // namespace

// --- RegistryWatcher ---------------------------------------------------------------------------

RegistryWatcher::RegistryWatcher(QList<QString> subkeys, QObject* parent)
    : QThread(parent)
    , subkeys(std::move(subkeys)) {
	this->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

RegistryWatcher::~RegistryWatcher() {
	this->stop();
	if (this->stopEvent != nullptr) CloseHandle(this->stopEvent);
}

void RegistryWatcher::stop() {
	if (!this->isRunning()) return;
	SetEvent(this->stopEvent);
	this->wait();
}

void RegistryWatcher::run() {
	QList<HKEY> keys;
	QList<HANDLE> events;
	QList<bool> armed;

	for (const auto& subkey: this->subkeys) {
		HKEY key = nullptr;
		auto path = subkey.toStdWString();
		if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) {
			qCDebug(logDesktops) << "Not watching missing registry key" << subkey;
			continue;
		}

		keys.append(key);
		events.append(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		armed.append(false);
	}

	if (keys.isEmpty()) {
		qCWarning(logDesktops) << "No virtual desktop registry keys to watch.";
		return;
	}

	QList<HANDLE> handles;
	handles.append(this->stopEvent);
	handles.append(events);

	while (true) {
		for (qsizetype i = 0; i < keys.length(); i++) {
			if (armed[i]) continue;
			// One notification per call: the key is re-armed after its event fires.
			auto status = RegNotifyChangeKeyValue(
			    keys[i],
			    TRUE,
			    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
			    events[i],
			    TRUE
			);
			armed[i] = status == ERROR_SUCCESS;
		}

		auto result = WaitForMultipleObjects(
		    static_cast<DWORD>(handles.length()),
		    handles.constData(),
		    FALSE,
		    INFINITE
		);

		if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;

		auto index = static_cast<qsizetype>(result - WAIT_OBJECT_0 - 1);
		if (index >= 0 && index < armed.length()) armed[index] = false;
		emit this->changed();
	}

	for (auto* key: keys) RegCloseKey(key);
	for (auto* event: events) CloseHandle(event);
}

// --- VirtualDesktops ---------------------------------------------------------------------------

VirtualDesktops* VirtualDesktops::instance() {
	static auto* instance = new VirtualDesktops(); // NOLINT
	return instance;
}

VirtualDesktops::VirtualDesktops() {
	// Qt's platform plugin initializes COM on the GUI thread; this is a no-op there and makes
	// the console probes work without one.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	// CLSID_VirtualDesktopManager; spelled out to avoid pulling in shobjidl.h / uuid.lib.
	static constexpr GUID clsid = {
	    0xaa509086,
	    0x5ca9,
	    0x4c25,
	    {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}
	};

	auto hr = CoCreateInstance(
	    clsid,
	    nullptr,
	    CLSCTX_ALL,
	    __uuidof(IVirtualDesktopManager),
	    reinterpret_cast<void**>(&this->manager)
	);

	if (FAILED(hr)) {
		qCWarning(logDesktops) << "IVirtualDesktopManager unavailable:" << Qt::hex << hr;
		this->manager = nullptr;
	}

	ProcessIdToSessionId(GetCurrentProcessId(), &this->sessionId);
	this->loadAccessor();
	this->installListener();
	this->refresh();

	auto sessionKey = QString::fromWCharArray(SESSION_KEY_FORMAT).arg(this->sessionId);
	this->watcher = new RegistryWatcher({QString::fromWCharArray(DESKTOPS_KEY), sessionKey}, this);
	QObject::connect(this->watcher, &RegistryWatcher::changed, this, &VirtualDesktops::refresh);
	this->watcher->start();

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this->watcher, &RegistryWatcher::stop);
	}
}

VirtualDesktops::~VirtualDesktops() {
	if (this->watcher != nullptr) this->watcher->stop();

	if (this->listener != nullptr) {
		if (this->accessor.unregisterPostMessageHook != nullptr) {
			this->accessor.unregisterPostMessageHook(this->listener);
		}

		DestroyWindow(this->listener);
	}

	if (this->manager != nullptr) this->manager->Release();
	if (this->accessor.module != nullptr) FreeLibrary(this->accessor.module);
}

void VirtualDesktops::loadAccessor() {
	auto path = QCoreApplication::applicationDirPath() + "/VirtualDesktopAccessor.dll";
	auto wpath = path.toStdWString();
	auto* module = LoadLibraryW(wpath.c_str());

	if (module == nullptr) {
		qCInfo(logDesktops) << "VirtualDesktopAccessor.dll not found next to the executable;"
		                    << "desktop switching falls back to keyboard shortcuts.";
		return;
	}

	auto& a = this->accessor;
	a.module = module;

	auto load = [module](auto& fn, const char* name) {
		fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
		return fn != nullptr;
	};

	auto ok = load(a.getCurrentDesktopNumber, "GetCurrentDesktopNumber")
	       && load(a.getDesktopCount, "GetDesktopCount")
	       && load(a.goToDesktopNumber, "GoToDesktopNumber")
	       && load(a.moveWindowToDesktopNumber, "MoveWindowToDesktopNumber")
	       && load(a.isWindowOnCurrentVirtualDesktop, "IsWindowOnCurrentVirtualDesktop");

	// Optional on older builds of the dll.
	load(a.getDesktopIdByNumber, "GetDesktopIdByNumber");
	load(a.getWindowDesktopNumber, "GetWindowDesktopNumber");
	load(a.createDesktop, "CreateDesktop");
	load(a.registerPostMessageHook, "RegisterPostMessageHook");
	load(a.unregisterPostMessageHook, "UnregisterPostMessageHook");
	load(a.isPinnedWindow, "IsPinnedWindow");
	load(a.pinWindow, "PinWindow");
	load(a.unPinWindow, "UnPinWindow");

	// The dll talks to undocumented COM interfaces that change between Windows builds; a build
	// mismatch shows up as -1 from everything, in which case it is as good as absent.
	if (!ok || a.getDesktopCount() <= 0) {
		qCWarning(logDesktops) << "VirtualDesktopAccessor.dll" << path
		                       << "does not work on this Windows build; ignoring it.";
		FreeLibrary(module);
		a = Accessor();
		return;
	}

	a.loaded = true;
	qCInfo(logDesktops) << "Loaded" << path;
}

LRESULT CALLBACK VirtualDesktops::listenerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (msg == ACCESSOR_MESSAGE) {
		// wparam/lparam are the old and new desktop numbers; the registry read is cheap and keeps
		// a single source of truth, so just refresh.
		VirtualDesktops::instance()->refresh();
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void VirtualDesktops::installListener() {
	if (this->accessor.registerPostMessageHook == nullptr) return;

	auto* module = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.lpfnWndProc = &VirtualDesktops::listenerProc;
	cls.hInstance = module;
	cls.lpszClassName = LISTENER_CLASS;
	RegisterClassW(&cls); // fails harmlessly if already registered

	this->listener = CreateWindowExW(
	    0,
	    LISTENER_CLASS,
	    L"",
	    0,
	    0,
	    0,
	    0,
	    0,
	    HWND_MESSAGE,
	    nullptr,
	    module,
	    nullptr
	);

	if (this->listener == nullptr) {
		qCWarning(logDesktops) << "Failed to create the desktop change listener window:"
		                       << GetLastError();
		return;
	}

	this->accessor.registerPostMessageHook(this->listener, ACCESSOR_MESSAGE);
}

QString VirtualDesktops::readDesktopName(const GUID& id) const {
	// Names only exist for desktops the user renamed.
	if (guidIsNull(id)) return {};

	HKEY key = nullptr;
	auto sub = QString::fromWCharArray(DESKTOPS_KEY) + "\\Desktops\\" + guidToString(id);
	auto wsub = sub.toStdWString();
	if (RegOpenKeyExW(HKEY_CURRENT_USER, wsub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return {};
	}

	auto name = readString(key, L"Name");
	RegCloseKey(key);
	return name;
}

bool VirtualDesktops::readRegistry(QList<GUID>& ids, GUID& current) const {
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOPS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return false;
	}

	// Absent until a second desktop has been created at least once.
	QByteArray blob;
	if (readBinary(key, L"VirtualDesktopIDs", blob)) {
		for (qsizetype offset = 0; offset + static_cast<qsizetype>(sizeof(GUID)) <= blob.length();
		     offset += static_cast<qsizetype>(sizeof(GUID)))
		{
			ids.append(guidAt(blob, offset));
		}
	}

	// Since Windows 10 1903 the live value is per session; the global one is a stale copy.
	QByteArray currentBytes;
	HKEY sessionKey = nullptr;
	auto sessionPath = QString::fromWCharArray(SESSION_KEY_FORMAT).arg(this->sessionId).toStdWString();
	if (RegOpenKeyExW(HKEY_CURRENT_USER, sessionPath.c_str(), 0, KEY_READ, &sessionKey)
	    == ERROR_SUCCESS)
	{
		readBinary(sessionKey, L"CurrentVirtualDesktop", currentBytes);
		RegCloseKey(sessionKey);
	}

	if (currentBytes.length() < static_cast<qsizetype>(sizeof(GUID))) {
		readBinary(key, L"CurrentVirtualDesktop", currentBytes);
	}

	if (currentBytes.length() >= static_cast<qsizetype>(sizeof(GUID))) {
		current = guidAt(currentBytes, 0);
	}

	RegCloseKey(key);
	return true;
}

void VirtualDesktops::refresh() {
	QList<GUID> ids;
	GUID current {};

	if (!this->readRegistry(ids, current)) {
		qCWarning(logDesktops) << "Unable to read the virtual desktop registry keys.";
	}

	// The accessor asks the shell directly, which beats registry values that are absent on a
	// fresh profile or might lag behind.
	const auto& a = this->accessor;
	if (a.loaded) {
		auto count = a.getDesktopCount();
		if (count > 0) {
			ids.resize(count);
			if (a.getDesktopIdByNumber != nullptr) {
				for (auto i = 0; i < count; i++) {
					if (guidIsNull(ids[i])) ids[i] = a.getDesktopIdByNumber(i);
				}
			}
		}
	}

	// A fresh profile has a single unnamed desktop and no registry entries yet.
	if (ids.isEmpty()) ids.append(GUID {});

	QList<Desktop> desktops;
	for (const auto& id: ids) desktops.append(Desktop {.id = id, .name = this->readDesktopName(id)});

	auto currentIndex = qsizetype(-1);
	for (qsizetype i = 0; i < desktops.length(); i++) {
		if (IsEqualGUID(desktops[i].id, current)) {
			currentIndex = i;
			break;
		}
	}

	if (a.loaded) {
		auto number = a.getCurrentDesktopNumber();
		if (number >= 0 && number < desktops.length()) {
			currentIndex = number;
			current = desktops[number].id;
		}
	}

	if (currentIndex == -1) currentIndex = 0;

	auto listChanged = desktops.length() != this->mDesktops.length();
	if (!listChanged) {
		for (qsizetype i = 0; i < desktops.length(); i++) {
			if (!IsEqualGUID(desktops[i].id, this->mDesktops[i].id)
			    || desktops[i].name != this->mDesktops[i].name)
			{
				listChanged = true;
				break;
			}
		}
	}

	auto currentChanged = currentIndex != this->mCurrent || !IsEqualGUID(current, this->mCurrentId);

	this->mDesktops = desktops;
	this->mCurrent = currentIndex;
	this->mCurrentId = current;

	if (listChanged) {
		qCDebug(logDesktops) << "Desktop list changed:" << desktops.length() << "desktops";
		emit this->desktopsChanged();
	}

	if (currentChanged) {
		qCDebug(logDesktops) << "Current desktop:" << currentIndex;
		emit this->currentChanged();
	}
}

qsizetype VirtualDesktops::indexOf(const GUID& id) const {
	if (guidIsNull(id)) return -1;

	for (qsizetype i = 0; i < this->mDesktops.length(); i++) {
		if (IsEqualGUID(this->mDesktops[i].id, id)) return i;
	}

	return -1;
}

GUID VirtualDesktops::windowDesktopId(HWND hwnd) const {
	GUID id {};
	if (this->manager == nullptr || hwnd == nullptr) return id;
	if (FAILED(this->manager->GetWindowDesktopId(hwnd, &id))) return GUID {};
	return id;
}

qsizetype VirtualDesktops::windowDesktopIndex(HWND hwnd) const {
	auto index = this->indexOf(this->windowDesktopId(hwnd));
	if (index != -1) return index;

	// Public API unanswered (fresh profile without registry ids, or a stale list): ask the shell.
	const auto& a = this->accessor;
	if (a.loaded && a.getWindowDesktopNumber != nullptr) {
		auto number = a.getWindowDesktopNumber(hwnd);
		if (number >= 0 && number < this->count()) return number;
	}

	return -1;
}

bool VirtualDesktops::isWindowOnCurrent(HWND hwnd) const {
	if (this->manager == nullptr || hwnd == nullptr) return true;
	BOOL onCurrent = TRUE;
	if (FAILED(this->manager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent))) return true;
	return onCurrent != FALSE;
}

void VirtualDesktops::sendShortcut(WORD key, int times) const {
	// Ctrl+Win+<key>, as the user would press it. Only usable without the accessor dll.
	QList<INPUT> inputs;
	auto push = [&inputs](WORD vk, bool up) {
		INPUT input {};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = vk;
		input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
		inputs.append(input);
	};

	for (auto i = 0; i < times; i++) {
		push(VK_LCONTROL, false);
		push(VK_LWIN, false);
		push(key, false);
		push(key, true);
		push(VK_LWIN, true);
		push(VK_LCONTROL, true);
	}

	SendInput(static_cast<UINT>(inputs.length()), inputs.data(), sizeof(INPUT));
}

bool VirtualDesktops::switchTo(qsizetype index) {
	if (index < 0 || index >= this->count()) return false;
	if (index == this->mCurrent) return true;

	if (this->accessor.loaded) {
		if (this->accessor.goToDesktopNumber(static_cast<int>(index)) == -1) {
			qCWarning(logDesktops) << "GoToDesktopNumber" << index << "failed";
			return false;
		}

		this->refresh();
		return true;
	}

	auto delta = index - this->mCurrent;
	this->sendShortcut(delta > 0 ? VK_RIGHT : VK_LEFT, static_cast<int>(qAbs(delta)));
	return true;
}

bool VirtualDesktops::waitForCount(qsizetype count) {
	// Desktop creation is rare and explorer writes the registry within a few ms; a short
	// synchronous wait keeps "create then switch" sequences simple.
	for (auto i = 0; i < 40; i++) {
		this->refresh();
		if (this->count() >= count) return true;
		QThread::msleep(25);
	}

	return this->count() >= count;
}

bool VirtualDesktops::ensureCount(qsizetype count) {
	if (count > MAX_DESKTOPS) {
		qCWarning(logDesktops) << "Refusing to create more than" << MAX_DESKTOPS << "desktops";
		return false;
	}

	while (this->count() < count) {
		auto before = this->count();

		if (this->accessor.loaded && this->accessor.createDesktop != nullptr) {
			if (this->accessor.createDesktop() == -1) {
				qCWarning(logDesktops) << "CreateDesktop failed";
				return false;
			}
		} else {
			// Ctrl+Win+D creates a desktop and switches to it.
			this->sendShortcut('D', 1);
		}

		if (!this->waitForCount(before + 1)) {
			qCWarning(logDesktops) << "Desktop creation did not show up in the registry";
			return false;
		}
	}

	return true;
}

bool VirtualDesktops::moveWindow(HWND hwnd, qsizetype index) {
	if (hwnd == nullptr || index < 0 || index >= this->count()) return false;

	if (isOwnWindow(hwnd) && this->manager != nullptr) {
		return SUCCEEDED(this->manager->MoveWindowToDesktop(hwnd, this->mDesktops[index].id));
	}

	if (this->accessor.loaded) {
		return this->accessor.moveWindowToDesktopNumber(hwnd, static_cast<int>(index)) != -1;
	}

	qCWarning(logDesktops) << "Moving another application's window to a desktop needs"
	                       << "VirtualDesktopAccessor.dll";
	return false;
}

bool VirtualDesktops::pinWindow(HWND hwnd, bool pinned) {
	auto& a = this->accessor;
	if (!a.loaded || a.pinWindow == nullptr || a.unPinWindow == nullptr) return false;
	return (pinned ? a.pinWindow(hwnd) : a.unPinWindow(hwnd)) != -1;
}

bool VirtualDesktops::isWindowPinned(HWND hwnd) const {
	const auto& a = this->accessor;
	if (!a.loaded || a.isPinnedWindow == nullptr) return false;
	return a.isPinnedWindow(hwnd) == 1;
}

QString VirtualDesktops::guidToString(const GUID& guid) {
	wchar_t buffer[40] {};
	StringFromGUID2(guid, buffer, 40);
	return QString::fromWCharArray(buffer);
}

} // namespace qs::windows
