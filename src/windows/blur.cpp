#include "blur.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qjsvalue.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlengine.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qstandardpaths.h>
#include <qstring.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwineventnotifier.h>

#include "../core/generation.hpp"
#include "blur_shapes.hpp"
#include "panel_window.hpp"

// Windows Runtime and DWM headers last: they pull in the rpc headers, which define macros like
// `small`. unknwn.h before winrt/base.h enables C++/WinRT's classic COM interop (`as<>` on the
// composition interop interfaces).
#include <unknwn.h>
#include <inspectable.h>

#include <DispatcherQueue.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>

#include <dwmapi.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace wucd = winrt::Windows::UI::Composition::Desktop;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using AbiDesktopInterop = ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop;
using AbiDesktopWindowTarget = ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget;
using AbiDispatcherQueueController = ABI::Windows::System::IDispatcherQueueController;

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logBlur, "quickshell.windows.blur", QtInfoMsg);

constexpr auto USER_FILE = "illogical-impulse/layerrules.json";
constexpr auto DEFAULT_FILE = "defaults/windows/layerrules.json";
constexpr auto PERSONALIZE_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr auto BACKDROP_CLASS = L"QuickshellBlurBackdrop";

bool gManagerDestroyed = false; // NOLINT

QString hresultString(const winrt::hresult_error& error) {
	return QString("0x%1 %2")
	    .arg(static_cast<quint32>(error.code().value), 8, 16, QChar('0'))
	    .arg(QString::fromWCharArray(error.message().c_str()));
}

} // namespace

// BackdropWindow

// The native half of a panel's blur: a window holding a composition visual tree with one host
// backdrop sprite per shape.
class BackdropWindow {
public:
	BackdropWindow() = default;
	~BackdropWindow();
	Q_DISABLE_COPY_MOVE(BackdropWindow);

	// Null on failure. `unsupported` is set when this Windows build can't do it at all.
	static std::unique_ptr<BackdropWindow> create(
	    const wuc::Compositor& compositor,
	    bool layered,
	    bool tint,
	    QString& error,
	    bool& unsupported
	);

	[[nodiscard]] HWND hwnd() const { return this->mHwnd; }
	void setPanel(HWND panel) { this->mPanel = panel; }

	// False when composition failed, after which the backdrop is useless.
	bool setShapes(const QList<BlurShape>& shapes);

	// Directly below the panel in the z-order (and in the same topmost band), at its rect.
	void show();
	void hide();

private:
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static bool registerClass();
	void updateRegion(const QList<BlurShape>& shapes);

	struct Slot {
		// clipped to the shape's clip rect (an ancestor item's clip, or the window)
		wuc::ContainerVisual holder {nullptr};
		// the backdrop, clipped to the rounded rectangle
		wuc::SpriteVisual sprite {nullptr};
		wuc::CompositionRoundedRectangleGeometry geometry {nullptr};
	};

	HWND mHwnd = nullptr;
	HWND mPanel = nullptr;
	bool layered = false;
	// set while this process moves the window, which WM_WINDOWPOSCHANGING lets through
	bool syncing = false;
	bool shown = false;
	RECT lastRect {};
	QRegion region;
	bool hasRegion = false;

	wuc::Compositor compositor {nullptr};
	wucd::DesktopWindowTarget target {nullptr};
	wuc::ContainerVisual root {nullptr};
	wuc::CompositionBrush brush {nullptr};
	wuc::CompositionBrush tint {nullptr};
	std::vector<Slot> visuals;
};

bool BackdropWindow::registerClass() {
	static auto registered = false; // NOLINT
	if (registered) return true;

	WNDCLASSEXW wndClass {};
	wndClass.cbSize = sizeof(wndClass);
	wndClass.lpfnWndProc = &BackdropWindow::wndProc;
	wndClass.hInstance = GetModuleHandleW(nullptr);
	wndClass.lpszClassName = BACKDROP_CLASS;

	registered = RegisterClassExW(&wndClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	return registered;
}

std::unique_ptr<BackdropWindow> BackdropWindow::create(
    const wuc::Compositor& compositor,
    bool layered,
    bool tint,
    QString& error,
    bool& unsupported
) {
	unsupported = false;

	if (!registerClass()) {
		error = QString("RegisterClassEx failed (%1)").arg(GetLastError());
		return nullptr;
	}

	// No redirection surface: the composition target is the window's only content, everything
	// outside the shapes is see-through. WS_EX_TRANSPARENT keeps it out of hit testing (a window
	// without a redirection surface needs no WS_EX_LAYERED for that), WS_EX_NOACTIVATE and
	// WS_EX_TOOLWINDOW keep it out of activation, the taskbar and Alt+Tab.
	DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
	if (layered) exStyle |= WS_EX_LAYERED;

	auto* hwnd = CreateWindowExW(
	    exStyle,
	    BACKDROP_CLASS,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);

	if (hwnd == nullptr) {
		error = QString("CreateWindowEx failed (%1)").arg(GetLastError());
		return nullptr;
	}

	auto backdrop = std::make_unique<BackdropWindow>();
	backdrop->mHwnd = hwnd;
	backdrop->layered = layered;
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backdrop.get()));

	// a layered window stays invisible until it has attributes
	if (layered) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

	BOOL enable = TRUE;
	auto hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
	if (FAILED(hr)) {
		error = QString("DWMWA_USE_HOSTBACKDROPBRUSH was refused (0x%1), Windows 11 is required")
		            .arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
		unsupported = true;
		return nullptr;
	}

	// Square corners (rounding would cut the shapes at the window's corners), no border, no
	// show/hide animation, not hidden by Aero Peek: the same as the panel above it.
	auto corner = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_DONOTROUND);
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
	COLORREF border = DWMWA_COLOR_NONE;
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enable, sizeof(enable));
	DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &enable, sizeof(enable));

	try {
		backdrop->compositor = compositor;

		auto interop = compositor.as<AbiDesktopInterop>();
		winrt::check_hresult(interop->CreateDesktopWindowTarget(
		    hwnd,
		    TRUE,
		    reinterpret_cast<AbiDesktopWindowTarget**>(winrt::put_abi(backdrop->target)) // NOLINT
		));

		backdrop->root = compositor.CreateContainerVisual();
		backdrop->root.RelativeSizeAdjustment(float2(1.0f, 1.0f));
		backdrop->target.Root(backdrop->root);

		// DWM provides the host backdrop already blurred (it is what acrylic is made of).
		backdrop->brush = compositor.CreateHostBackdropBrush();

		if (tint) {
			backdrop->tint = compositor.CreateColorBrush(
			    winrt::Windows::UI::Color {.A = 0x60, .R = 0xff, .G = 0x20, .B = 0x20}
			);
		}
	} catch (const winrt::hresult_error& e) {
		error = "Composition setup failed: " + hresultString(e);
		return nullptr;
	}

	return backdrop;
}

BackdropWindow::~BackdropWindow() {
	try {
		this->visuals.clear();
		if (this->root != nullptr) this->root.Children().RemoveAll();

		if (this->target != nullptr) {
			this->target.Root(nullptr);
			this->target.Close();
		}
	} catch (const winrt::hresult_error& e) {
		// expected after the compositor was closed at shutdown
		qCDebug(logBlur) << "Releasing a backdrop:" << hresultString(e);
	}

	this->tint = nullptr;
	this->brush = nullptr;
	this->root = nullptr;
	this->target = nullptr;
	this->compositor = nullptr;

	if (this->mHwnd != nullptr) {
		SetWindowLongPtrW(this->mHwnd, GWLP_USERDATA, 0);
		DestroyWindow(this->mHwnd);
		this->mHwnd = nullptr;
	}
}

bool BackdropWindow::setShapes(const QList<BlurShape>& shapes) {
	try {
		auto children = this->root.Children();
		auto count = static_cast<size_t>(shapes.size());

		while (this->visuals.size() < count) {
			Slot slot;
			slot.holder = this->compositor.CreateContainerVisual();
			// zero insets: clips the children to the holder's size
			slot.holder.Clip(this->compositor.CreateInsetClip());

			slot.sprite = this->compositor.CreateSpriteVisual();
			slot.sprite.Brush(this->brush);
			slot.geometry = this->compositor.CreateRoundedRectangleGeometry();
			slot.sprite.Clip(this->compositor.CreateGeometricClip(slot.geometry));

			if (this->tint != nullptr) {
				auto overlay = this->compositor.CreateSpriteVisual();
				overlay.Brush(this->tint);
				overlay.RelativeSizeAdjustment(float2(1.0f, 1.0f));
				slot.sprite.Children().InsertAtTop(overlay);
			}

			slot.holder.Children().InsertAtTop(slot.sprite);
			children.InsertAtTop(slot.holder);
			this->visuals.push_back(std::move(slot));
		}

		while (this->visuals.size() > count) {
			children.Remove(this->visuals.back().holder);
			this->visuals.pop_back();
		}

		for (size_t i = 0; i < count; i++) {
			const auto& shape = shapes.at(static_cast<qsizetype>(i));
			auto& slot = this->visuals.at(i);

			auto size = float2(static_cast<float>(shape.rect.width()), static_cast<float>(shape.rect.height()));
			auto radius = static_cast<float>(shape.radius);

			slot.holder.Offset(float3(static_cast<float>(shape.clip.x()), static_cast<float>(shape.clip.y()), 0));
			slot.holder.Size(float2(static_cast<float>(shape.clip.width()), static_cast<float>(shape.clip.height())));

			slot.sprite.Offset(float3(
			    static_cast<float>(shape.rect.x() - shape.clip.x()),
			    static_cast<float>(shape.rect.y() - shape.clip.y()),
			    0
			));
			slot.sprite.Size(size);
			slot.geometry.Size(size);
			slot.geometry.CornerRadius(float2(radius, radius));
		}
	} catch (const winrt::hresult_error& e) {
		qCWarning(logBlur) << "Updating the blur shapes failed:" << hresultString(e);
		return false;
	}

	if (!this->layered) this->updateRegion(shapes);
	return true;
}

void BackdropWindow::updateRegion(const QList<BlurShape>& shapes) {
	// The window covers the whole panel, whose margins are click-through. WS_EX_TRANSPARENT should
	// already keep this window out of hit testing; in case it doesn't without WS_EX_LAYERED, a
	// window region limited to the blurred rectangles (inside the panel's input mask, where the
	// panel takes the clicks itself) keeps it out of the way. Rounded outwards, so it never clips
	// the anti-aliased edges of the shapes.
	QRegion next;
	for (const auto& shape: shapes) next += shape.rect.intersected(shape.clip).toAlignedRect();

	if (this->hasRegion && next == this->region) return;

	auto* hrgn = CreateRectRgn(0, 0, 0, 0);
	for (const auto& rect: next) {
		auto* part = CreateRectRgn(rect.x(), rect.y(), rect.x() + rect.width(), rect.y() + rect.height());
		CombineRgn(hrgn, hrgn, part, RGN_OR);
		DeleteObject(part);
	}

	this->syncing = true;
	// the system owns the region from here on
	if (SetWindowRgn(this->mHwnd, hrgn, FALSE) == 0) DeleteObject(hrgn);
	this->syncing = false;

	this->region = next;
	this->hasRegion = true;
}

void BackdropWindow::show() {
	if (this->mHwnd == nullptr || this->mPanel == nullptr) return;

	RECT rect {};
	if (!GetWindowRect(this->mPanel, &rect)) {
		this->hide();
		return;
	}

	auto panelTopmost = (GetWindowLongPtrW(this->mPanel, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	auto ownTopmost = (GetWindowLongPtrW(this->mHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

	// Called for every frame and every move of the panel; most of the time nothing changed.
	if (this->shown && IsWindowVisible(this->mHwnd) && EqualRect(&rect, &this->lastRect)
	    && panelTopmost == ownTopmost && GetWindow(this->mPanel, GW_HWNDNEXT) == this->mHwnd)
	{
		return;
	}

	this->syncing = true;
	constexpr UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;

	// A window placed after a topmost one stays in its own band, so switch bands first.
	if (panelTopmost != ownTopmost) {
		SetWindowPos(
		    this->mHwnd,
		    panelTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
		    0,
		    0,
		    0,
		    0,
		    flags | SWP_NOMOVE | SWP_NOSIZE
		);
	}

	SetWindowPos(
	    this->mHwnd,
	    this->mPanel,
	    rect.left,
	    rect.top,
	    rect.right - rect.left,
	    rect.bottom - rect.top,
	    flags | SWP_SHOWWINDOW
	);

	this->syncing = false;
	this->lastRect = rect;
	this->shown = true;
}

void BackdropWindow::hide() {
	this->shown = false;
	if (this->mHwnd == nullptr || !IsWindowVisible(this->mHwnd)) return;

	this->syncing = true;
	SetWindowPos(
	    this->mHwnd,
	    nullptr,
	    0,
	    0,
	    0,
	    0,
	    SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
	this->syncing = false;
}

LRESULT CALLBACK BackdropWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// NOLINTNEXTLINE(performance-no-int-to-ptr)
	auto* self = reinterpret_cast<BackdropWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	// Belt and braces: WS_EX_TRANSPARENT already keeps it out of hit testing.
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;
	// Placed in physical pixels over its panel, whatever the monitor's DPI.
	case WM_DPICHANGED: return 0;
	case WM_WINDOWPOSCHANGING:
		if (self != nullptr && !self->syncing && self->mPanel != nullptr && IsWindow(self->mPanel)) {
			auto* pos = reinterpret_cast<WINDOWPOS*>(lParam); // NOLINT(performance-no-int-to-ptr)
			// Only the panel decides where its backdrop goes. Anything else restacking it would
			// leave the blur above the panel, hiding the panel's contents.
			if ((pos->flags & SWP_NOZORDER) == 0) pos->hwndInsertAfter = self->mPanel;
		}
		break;
	default: break;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// BlurManager

struct BlurManager::Composition {
	winrt::Windows::System::DispatcherQueueController controller {nullptr};
	wuc::Compositor compositor {nullptr};
};

BlurManager* BlurManager::instance() {
	static QPointer<BlurManager> manager; // NOLINT

	if (gManagerDestroyed) return nullptr;

	if (manager.isNull()) {
		// owned by the application; composition is released at aboutToQuit
		manager = new BlurManager(QCoreApplication::instance());
	}

	return manager.data();
}

BlurManager::BlurManager(QObject* parent): QObject(parent) {
	auto env = qEnvironmentVariable("QS_WINDOWS_BLUR").trimmed().toLower();
	this->envEnabled = !(env == "0" || env == "false" || env == "off" || env == "no");
	this->envLayered = qEnvironmentVariableIntValue("QS_WINDOWS_BLUR_LAYERED") != 0;
	this->mDebugTint = qEnvironmentVariableIntValue("QS_WINDOWS_BLUR_DEBUG") != 0;

	if (!this->envEnabled) qCInfo(logBlur) << "Blur behind panels is turned off by QS_WINDOWS_BLUR.";

	this->reloadTimer.setSingleShot(true);
	this->reloadTimer.setInterval(200);
	QObject::connect(&this->reloadTimer, &QTimer::timeout, this, &BlurManager::loadFile);

	// clang-format off
	QObject::connect(&this->watcher, &QFileSystemWatcher::fileChanged, this, &BlurManager::scheduleReload);
	QObject::connect(&this->watcher, &QFileSystemWatcher::directoryChanged, this, &BlurManager::scheduleReload);
	// clang-format on

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &BlurManager::shutdown);
	}

	this->checkSystem();
	this->watchRegistry();
}

BlurManager::~BlurManager() {
	gManagerDestroyed = true;

	// Without aboutToQuit (no event loop exit), COM may already be uninitialized: leak the
	// composition objects instead of releasing them.
	if (this->composition != nullptr) {
		winrt::detach_abi(this->composition->compositor);
		winrt::detach_abi(this->composition->controller);
	}

	delete this->registryNotifier;
	this->registryNotifier = nullptr;
	if (this->registryEvent != nullptr) CloseHandle(this->registryEvent);
	if (this->personalizeKey != nullptr) RegCloseKey(this->personalizeKey);
}

void BlurManager::shutdown() {
	if (this->mShutDown) return;
	this->mShutDown = true;

	for (auto* panel: QList(this->panels)) panel->updateActive();

	if (this->composition != nullptr) {
		this->closeCompositor();
		// A DispatcherQueue of the current thread wants ShutdownQueueAsync and a running message
		// loop to go away cleanly. The process is exiting; leave it to the system.
		winrt::detach_abi(this->composition->controller);
		this->composition.reset();
	}
}

void BlurManager::closeCompositor() {
	if (this->composition == nullptr || this->composition->compositor == nullptr) return;

	try {
		this->composition->compositor.Close();
	} catch (const winrt::hresult_error& e) {
		qCDebug(logBlur) << "Closing the compositor:" << hresultString(e);
	}

	this->composition->compositor = nullptr;
}

BlurManager::Composition* BlurManager::ensureComposition() {
	if (this->unsupported || this->mShutDown) return nullptr;

	if (this->composition == nullptr) this->composition = std::make_unique<Composition>();
	auto* state = this->composition.get();
	if (state->compositor != nullptr) return state;

	try {
		// Windows.UI.Composition needs a DispatcherQueue on the thread that uses it. Everything
		// here runs on the gui thread, whose message loop (Qt's) also runs the queue: the
		// compositor's calls don't block and nothing here is asynchronous, so unlike the other
		// WinRT backends this needs no worker thread or apartment of its own (COM is already
		// initialized on this thread by Qt).
		if (state->controller == nullptr
		    && winrt::Windows::System::DispatcherQueue::GetForCurrentThread() == nullptr)
		{
			DispatcherQueueOptions options {
			    .dwSize = sizeof(DispatcherQueueOptions),
			    .threadType = DQTYPE_THREAD_CURRENT,
			    .apartmentType = DQTAT_COM_NONE,
			};

			winrt::check_hresult(CreateDispatcherQueueController(
			    options,
			    reinterpret_cast<AbiDispatcherQueueController**>(winrt::put_abi(state->controller)) // NOLINT
			));
		}

		state->compositor = wuc::Compositor();
	} catch (const winrt::hresult_error& e) {
		// retried on the next state change
		qCWarning(logBlur).noquote() << "Could not create a compositor for blur:" << hresultString(e);
		state->compositor = nullptr;
		return nullptr;
	}

	qCDebug(logBlur) << "Compositor created.";
	return state;
}

void BlurManager::markUnsupported(const QString& reason) {
	if (this->unsupported) return;
	this->unsupported = true;

	qCWarning(logBlur).noquote() << "Blur behind panels is unavailable:" << reason;
	emit this->stateChanged();
	this->scheduleNotifyPanels();
}

void BlurManager::registerPanel(PanelBlur* panel) {
	if (!this->panels.contains(panel)) this->panels.append(panel);
}

void BlurManager::unregisterPanel(PanelBlur* panel) { this->panels.removeOne(panel); }

void BlurManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	emit this->stateChanged();
	this->scheduleNotifyPanels();
}

bool BlurManager::available() const {
	return !this->unsupported && !this->mShutDown && this->envEnabled && this->fileEnabled
	    && this->transparencyEffects && !this->highContrast && !this->energySaver;
}

bool BlurManager::layeredBackdrops() const { return this->envLayered || this->fileLayered; }

void BlurManager::scheduleNotifyPanels() {
	if (this->notifyPending) return;
	this->notifyPending = true;

	// queued: rule lookups from inside PanelBlur::updateActive can load the rules file
	QMetaObject::invokeMethod(
	    this,
	    [this]() {
		    this->notifyPending = false;
		    this->notifyPanels();
	    },
	    Qt::QueuedConnection
	);
}

void BlurManager::notifyPanels() {
	for (auto* panel: QList(this->panels)) panel->updateActive();
}

// System settings

void BlurManager::scheduleSystemCheck(bool recreate) {
	if (recreate) this->recreatePending = true;
	if (this->systemCheckPending) return;
	this->systemCheckPending = true;

	// Settings broadcasts reach every panel, often several in a row: check once.
	QTimer::singleShot(0, this, [this]() {
		this->systemCheckPending = false;
		this->checkSystem();

		if (this->recreatePending) {
			this->recreatePending = false;
			if (this->mShutDown) return;

			qCInfo(logBlur) << "Desktop composition changed, recreating the blur backdrops.";

			for (auto* panel: QList(this->panels)) panel->recreate();

			// The compositor may not survive a DWM restart either. Backdrops are gone, so it can
			// be dropped here and created again when the panels come back.
			this->closeCompositor();
			this->notifyPanels();
		}
	});
}

void BlurManager::checkSystem() {
	DWORD value = 1;
	DWORD size = sizeof(value);
	auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    PERSONALIZE_KEY,
	    L"EnableTransparency",
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &value,
	    &size
	);
	// missing means the default, which is on
	auto transparency = status != ERROR_SUCCESS || value != 0;

	HIGHCONTRASTW contrast {};
	contrast.cbSize = sizeof(contrast);
	auto highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
	                 && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;

	// Energy saver (battery saver) turns transparency effects off system wide.
	SYSTEM_POWER_STATUS power {};
	auto energySaver = GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1;

	if (transparency == this->transparencyEffects && highContrast == this->highContrast
	    && energySaver == this->energySaver)
	{
		return;
	}

	this->transparencyEffects = transparency;
	this->highContrast = highContrast;
	this->energySaver = energySaver;

	qCInfo(logBlur) << "System state: transparency effects" << transparency << "high contrast"
	                << highContrast << "energy saver" << energySaver;

	emit this->stateChanged();
	this->scheduleNotifyPanels();
}

void BlurManager::watchRegistry() {
	auto status = RegOpenKeyExW(
	    HKEY_CURRENT_USER,
	    PERSONALIZE_KEY,
	    0,
	    KEY_NOTIFY | KEY_QUERY_VALUE,
	    &this->personalizeKey
	);

	if (status != ERROR_SUCCESS) {
		// Settings broadcasts to the panels still trigger checks.
		this->personalizeKey = nullptr;
		qCDebug(logBlur) << "Can't watch the Personalize key:" << status;
		return;
	}

	this->registryEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (this->registryEvent == nullptr) return;

	this->registryNotifier = new QWinEventNotifier(this->registryEvent, this);
	QObject::connect(this->registryNotifier, &QWinEventNotifier::activated, this, [this]() {
		this->armRegistryNotification();
		this->scheduleSystemCheck();
	});

	this->armRegistryNotification();
}

void BlurManager::armRegistryNotification() {
	if (this->personalizeKey == nullptr || this->registryEvent == nullptr) return;

	// one shot: re-armed after every change
	RegNotifyChangeKeyValue(
	    this->personalizeKey,
	    FALSE,
	    REG_NOTIFY_CHANGE_LAST_SET,
	    this->registryEvent,
	    TRUE
	);
}

// Rules file

void BlurManager::setShellDir(const QString& shellDir) {
	if (shellDir.isEmpty() || (this->loaded && shellDir == this->shellDir)) return;

	this->shellDir = shellDir;
	this->loaded = true;
	this->parsed = false;
	this->loadFile();
}

void BlurManager::reload() {
	this->parsed = false;
	this->loadFile();
}

QString BlurManager::userFilePath() const {
	auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	if (base.isEmpty()) return QString();
	return base + '/' + USER_FILE;
}

QString BlurManager::defaultFilePath() const {
	if (this->shellDir.isEmpty()) return QString();
	return this->shellDir + '/' + DEFAULT_FILE;
}

void BlurManager::scheduleReload() { this->reloadTimer.start(); }

void BlurManager::loadFile() {
	auto path = this->userFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) path = this->defaultFilePath();

	QByteArray data;
	auto file = QFile(path);
	if (path.isEmpty() || !file.open(QFile::ReadOnly)) path.clear();
	else data = file.readAll();

	// Watched directories also report unrelated files, and editors touch files without changes.
	if (!this->parsed || path != this->mConfigPath || data != this->loadedData) {
		if (path.isEmpty()) {
			qCInfo(logBlur) << "No layer rules file found (looked for" << this->userFilePath() << "and"
			                << this->defaultFilePath() << "), blur behind panels is off.";
		}

		auto wasLayered = this->layeredBackdrops();
		auto wasAvailable = this->available();

		this->parsed = true;
		this->mConfigPath = path;
		this->loadedData = data;
		this->parse(data, path);

		if (this->layeredBackdrops() != wasLayered) {
			for (auto* panel: QList(this->panels)) panel->recreate();
		}

		emit this->rulesChanged();
		if (this->available() != wasAvailable) emit this->stateChanged();
		this->scheduleNotifyPanels();
	}

	this->updateWatches();
}

void BlurManager::updateWatches() {
	// Editors save by replacing the file, which drops it from the watcher; also watch the
	// directory so the user file can appear, be replaced or disappear.
	auto userPath = this->userFilePath();
	auto userDir = QFileInfo(userPath).absolutePath();

	QStringList wanted;
	if (!this->mConfigPath.isEmpty()) wanted.append(this->mConfigPath);
	if (QFileInfo::exists(userDir)) wanted.append(userDir);
	else if (!userPath.isEmpty()) {
		// %LOCALAPPDATA% itself until illogical-impulse\ exists
		wanted.append(QFileInfo(userDir).absolutePath());
	}

	auto current = this->watcher.files() + this->watcher.directories();
	for (const auto& path: current) {
		if (!wanted.contains(path)) this->watcher.removePath(path);
	}

	for (const auto& path: wanted) {
		if (!current.contains(path) && QFileInfo::exists(path)) this->watcher.addPath(path);
	}
}

void BlurManager::parse(const QByteArray& data, const QString& path) {
	this->rules.clear();
	this->fileEnabled = true;
	this->fileLayered = false;
	if (data.isEmpty()) return;

	QJsonParseError parseError {};
	auto document = QJsonDocument::fromJson(data, &parseError);
	if (document.isNull()) {
		qCWarning(logBlur) << "Failed to parse" << path << ":" << parseError.errorString() << "at offset"
		                   << parseError.offset;
		return;
	}

	auto root = document.object();
	this->fileEnabled = root.value("enable").toBool(true);
	this->fileLayered = root.value("layered").toBool(false);

	auto list = root.value("rules").toArray();

	for (auto i = 0; i < list.size(); i++) {
		auto object = list.at(i).toObject();
		auto where = QString("%1 rule %2").arg(path).arg(i);

		auto rule = Rule();
		rule.pattern = object.value("namespace").toString();

		if (rule.pattern.isEmpty()) {
			qCWarning(logBlur).noquote() << where << ": no namespace";
			continue;
		}

		// Hyprland matches the whole namespace (RE2::FullMatch).
		rule.regex = QRegularExpression(QRegularExpression::anchoredPattern(rule.pattern));
		if (!rule.regex.isValid()) {
			qCWarning(logBlur).noquote() << where << ": invalid regex" << rule.pattern << ":"
			                             << rule.regex.errorString();
			continue;
		}

		if (object.contains("blur")) rule.blur = object.value("blur").toBool();

		if (object.contains("ignoreAlpha")) {
			auto value = object.value("ignoreAlpha");

			if (value.isNull()) {
				rule.ignoreAlpha.emplace();
			} else if (value.isDouble()) {
				rule.ignoreAlpha.emplace(std::clamp(value.toDouble(), 0.0, 1.0));
			} else {
				qCWarning(logBlur).noquote() << where << ": ignoreAlpha must be a number or null";
			}
		}

		this->rules.append(rule);
	}

	qCInfo(logBlur).noquote() << "Loaded" << this->rules.size() << "layer rules from" << path;
}

BlurRule BlurManager::ruleFor(const QString& ns) const {
	auto result = BlurRule();

	for (const auto& rule: this->rules) {
		if (!rule.regex.match(ns).hasMatch()) continue;
		if (rule.blur.has_value()) result.blur = *rule.blur;
		if (rule.ignoreAlpha.has_value()) result.ignoreAlpha = *rule.ignoreAlpha;
	}

	return result;
}

QVariantList BlurManager::rulesInfo() const {
	QVariantList list;

	for (const auto& rule: this->rules) {
		QVariantMap map;
		map["namespace"] = rule.pattern;
		if (rule.blur.has_value()) map["blur"] = *rule.blur;

		if (rule.ignoreAlpha.has_value()) {
			map["ignoreAlpha"] = rule.ignoreAlpha->has_value() ? QVariant(**rule.ignoreAlpha)
			                                                   : QVariant::fromValue(nullptr);
		}

		list.append(map);
	}

	return list;
}

// PanelBlur

PanelBlur::PanelBlur(WinPanelWindow* panel): QObject(panel), panel(panel) {
	if (auto* manager = BlurManager::instance()) manager->registerPanel(this);
	QObject::connect(panel, &WinPanelWindow::namespaceChanged, this, &PanelBlur::updateActive);
}

PanelBlur::~PanelBlur() {
	this->disconnectFrames();
	this->backdrop.reset();
	if (auto* manager = BlurManager::instance()) manager->unregisterPanel(this);
}

void PanelBlur::adopt(PanelBlur* other) {
	if (other == nullptr || other == this) return;

	other->disconnectFrames();
	this->destroyBackdrop();

	this->backdrop = std::move(other->backdrop);
	this->shapes = std::move(other->shapes);
	this->active = this->backdrop != nullptr && other->active;
	this->stale = other->stale;
	this->panelWasShown = other->panelWasShown;
	// replaced on the new panel's first polish
	this->mask = other->mask;
	this->hasMask = other->hasMask;

	other->shapes.clear();
	other->active = false;
}

void PanelBlur::attach() {
	this->mWindow = this->panel->backingWindow();
	if (this->backdrop != nullptr) this->backdrop->setPanel(this->panel->hwnd());

	this->updateActive();
	this->syncPlacement();
}

void PanelBlur::release() {
	this->disconnectFrames();
	this->destroyBackdrop();
	this->active = false;
	this->mWindow = nullptr;
	this->panelWasShown = false;
}

void PanelBlur::recreate() {
	// The caller re-evaluates (BlurManager::notifyPanels) once everything is torn down.
	this->disconnectFrames();
	this->destroyBackdrop();
	this->active = false;
}

void PanelBlur::destroyBackdrop() {
	this->backdrop.reset();
	this->shapes.clear();
	this->stale = true;
}

void PanelBlur::connectFrames() {
	auto* window = this->mWindow.data();
	if (window == nullptr) return;
	if (this->frameConnection && this->framesWindow == window) return;

	this->disconnectFrames();

	// Emitted on the gui thread for every frame the window renders, after polish: geometry
	// is final for the frame. Nothing renders while the panel is idle.
	this->frameConnection =
	    QObject::connect(window, &QQuickWindow::afterAnimating, this, &PanelBlur::onFrame);
	this->framesWindow = window;
}

void PanelBlur::disconnectFrames() {
	if (this->frameConnection) QObject::disconnect(this->frameConnection);
	this->frameConnection = {};
	this->framesWindow = nullptr;
}

void PanelBlur::updateActive() {
	auto* manager = BlurManager::instance();
	auto* hwnd = this->panel->hwnd();

	if (manager != nullptr && !manager->shutDown()) {
		if (auto* generation = EngineGeneration::findObjectGeneration(this->panel)) {
			manager->setShellDir(generation->rootPath.path());
		}

		this->rule = manager->ruleFor(this->panel->ns());
	} else {
		this->rule = BlurRule();
	}

	// ignoreAlpha 1 leaves nothing to blur (ii uses it to opt namespaces out).
	auto wanted = manager != nullptr && manager->enabled() && manager->available() && this->rule.blur
	           && (!this->rule.ignoreAlpha.has_value() || *this->rule.ignoreAlpha < 1.0)
	           && hwnd != nullptr && this->mWindow != nullptr;

	if (!wanted) {
		if (this->active) qCDebug(logBlur) << "Blur off for" << this->panel->ns();
		this->active = false;
		this->disconnectFrames();
		this->destroyBackdrop();
		return;
	}

	if (!this->active) qCDebug(logBlur) << "Blur on for" << this->panel->ns();
	this->active = true;
	if (this->backdrop != nullptr) this->backdrop->setPanel(hwnd);
	this->connectFrames();
	this->scheduleShapes();
}

bool PanelBlur::ensureBackdrop() {
	if (this->backdrop != nullptr) return true;

	auto* manager = BlurManager::instance();
	auto* composition = manager != nullptr ? manager->ensureComposition() : nullptr;
	if (composition == nullptr) return false;

	QString error;
	auto unsupported = false;

	this->backdrop = BackdropWindow::create(
	    composition->compositor,
	    manager->layeredBackdrops(),
	    manager->debugTint(),
	    error,
	    unsupported
	);

	if (this->backdrop == nullptr) {
		if (unsupported) manager->markUnsupported(error);
		else qCWarning(logBlur).noquote() << "No blur for" << this->panel->ns() << ":" << error;
		return false;
	}

	this->backdrop->setPanel(this->panel->hwnd());
	this->shapes.clear();
	return true;
}

void PanelBlur::setInputMask(const QRegion& region, bool hasMask) {
	if (hasMask == this->hasMask && region == this->mask) return;

	this->mask = region;
	this->hasMask = hasMask;
	if (this->active) this->scheduleShapes();
}

void PanelBlur::onFrame() { this->updateShapes(); }

void PanelBlur::scheduleShapes() {
	if (this->shapesPending) return;
	this->shapesPending = true;

	QTimer::singleShot(0, this, [this]() {
		this->shapesPending = false;
		this->updateShapes();
	});
}

bool PanelBlur::panelShown() const {
	auto* hwnd = this->panel->hwnd();
	return hwnd != nullptr && IsWindowVisible(hwnd) && !IsIconic(hwnd);
}

void PanelBlur::updateShapes() {
	if (!this->active) return;

	QList<BlurShape> next;
	if (this->panelShown()) this->collectShapes(next);

	// The native backdrop only exists once there is something to blur: panels without
	// transparency never get one.
	if (this->backdrop == nullptr) {
		if (next.isEmpty()) return;

		if (!this->ensureBackdrop()) {
			// retried on the next rule or system change
			this->active = false;
			this->disconnectFrames();
			return;
		}
	}

	auto same = next.size() == this->shapes.size()
	         && std::equal(next.begin(), next.end(), this->shapes.begin(), [](const auto& a, const auto& b) {
		            return a.fuzzyEquals(b);
	            });

	if (!same) {
		if (!this->backdrop->setShapes(next)) {
			// Composition is broken for this backdrop; the next rule or system change retries.
			this->active = false;
			this->disconnectFrames();
			this->destroyBackdrop();
			return;
		}

		if (logBlur().isDebugEnabled()) {
			QStringList list;
			for (const auto& shape: next) {
				list.append(QString("%1x%2+%3+%4 r%5")
				                .arg(shape.rect.width())
				                .arg(shape.rect.height())
				                .arg(shape.rect.x())
				                .arg(shape.rect.y())
				                .arg(shape.radius));
			}

			qCDebug(logBlur).noquote() << "Blur shapes of" << this->panel->ns() << ":"
			                           << (list.isEmpty() ? "none" : list.join(", "));
		}

		this->shapes = std::move(next);
	}

	this->stale = false;
	this->syncPlacement();
}

void PanelBlur::syncPlacement() {
	auto shown = this->panelShown();

	if (shown && !this->panelWasShown) {
		// Shapes are from before the panel was hidden: recompute them before showing.
		this->stale = true;
		if (this->active) this->scheduleShapes();
	}

	this->panelWasShown = shown;
	if (this->backdrop == nullptr) return;

	if (!shown || !this->active || this->stale || this->shapes.isEmpty()) this->backdrop->hide();
	else this->backdrop->show();
}

void PanelBlur::collectShapes(QList<BlurShape>& shapes) {
	auto* window = this->mWindow.data();
	if (window == nullptr) return;

	auto query = BlurShapeQuery {
	    .ignoreAlpha = this->rule.ignoreAlpha,
	    .mask = this->mask,
	    .hasMask = this->hasMask,
	    .dpr = window->devicePixelRatio(),
	};

	auto result = collectBlurShapes(window, query);
	shapes = std::move(result.shapes);

	if (result.truncated && !this->truncatedWarned) {
		this->truncatedWarned = true;
		qCWarning(logBlur) << "Item tree of" << this->panel->ns()
		                   << "is too large to walk every frame, blur only covers part of it.";
	}
}

// BackdropBlur

BackdropBlur::BackdropBlur(QObject* parent): QObject(parent) {
	if (auto* manager = BlurManager::instance()) {
		// clang-format off
		QObject::connect(manager, &BlurManager::stateChanged, this, &BackdropBlur::stateChanged);
		QObject::connect(manager, &BlurManager::rulesChanged, this, &BackdropBlur::rulesChanged);
		// clang-format on
	}
}

BackdropBlur* BackdropBlur::create(QQmlEngine* engine, QJSEngine* /*jsEngine*/) {
	auto* blur = new BackdropBlur(nullptr);

	auto* manager = BlurManager::instance();
	auto* generation = EngineGeneration::findEngineGeneration(engine);
	if (manager != nullptr && generation != nullptr) manager->setShellDir(generation->rootPath.path());

	return blur;
}

bool BackdropBlur::enabled() {
	auto* manager = BlurManager::instance();
	return manager != nullptr && manager->enabled();
}

void BackdropBlur::setEnabled(bool enabled) {
	if (auto* manager = BlurManager::instance()) manager->setEnabled(enabled);
}

bool BackdropBlur::available() {
	auto* manager = BlurManager::instance();
	return manager != nullptr && manager->available();
}

QString BackdropBlur::configPath() {
	auto* manager = BlurManager::instance();
	return manager != nullptr ? manager->configPath() : QString();
}

QVariantList BackdropBlur::rules() {
	auto* manager = BlurManager::instance();
	return manager != nullptr ? manager->rulesInfo() : QVariantList();
}

void BackdropBlur::reload() {
	if (auto* manager = BlurManager::instance()) manager->reload();
}

} // namespace qs::windows
