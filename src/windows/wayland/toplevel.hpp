#pragma once

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include "../../core/doc.hpp"
#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "../../core/util.hpp"
#include "../window_tracker.hpp"

// Windows stand-ins for the wayland toplevel management types, with the same API, backed by the
// process wide window tracker. Same namespace as the wayland module so attached objects and
// documentation line up.
namespace qs::wayland::toplevel {

class ToplevelManager;

///! Window from another application.
/// A window/toplevel from another application, retrievable from
/// the @@ToplevelManager.
class Toplevel: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QString appId READ appId NOTIFY appIdChanged);
	Q_PROPERTY(QString title READ title NOTIFY titleChanged);
	/// Owner window if it is also tracked (dialogs with WS_EX_APPWINDOW), otherwise null.
	Q_PROPERTY(qs::wayland::toplevel::Toplevel* parent READ parent NOTIFY parentChanged);
	/// If the window is currently the foreground window.
	///
	/// Activation can be requested with the @@activate() function.
	Q_PROPERTY(bool activated READ activated NOTIFY activatedChanged);
	/// Screen the toplevel is currently on (always a single entry on Windows).
	Q_PROPERTY(QList<QuickshellScreenInfo*> screens READ screens NOTIFY screensChanged);
	/// If the window is currently maximized. Setting it requests a change.
	Q_PROPERTY(bool maximized READ maximized WRITE setMaximized NOTIFY maximizedChanged);
	/// If the window is currently minimized. Setting it requests a change.
	Q_PROPERTY(bool minimized READ minimized WRITE setMinimized NOTIFY minimizedChanged);
	/// If the window covers its whole screen. Setting it is best effort, see @@fullscreenOn().
	Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged);
	QML_ELEMENT;
	QML_UNCREATABLE("Toplevels must be acquired from the ToplevelManager.");

public:
	explicit Toplevel(qs::windows::TrackedWindow* window, QObject* parent);

	/// Restores the window if minimized, switches to its virtual desktop and raises it.
	Q_INVOKABLE void activate();

	/// Asks the window to close (WM_CLOSE). The application may ignore it.
	Q_INVOKABLE void close();

	/// Makes the window borderless and covers the given screen; `null` undoes it.
	Q_INVOKABLE void fullscreenOn(QuickshellScreenInfo* screen);

	/// Accepted for compatibility; Windows has no minimize target hint.
	Q_INVOKABLE void setRectangle(QObject* window, QRect rect);
	Q_INVOKABLE void unsetRectangle();

	[[nodiscard]] QString appId() const;
	[[nodiscard]] QString title() const;
	[[nodiscard]] Toplevel* parent() const;
	[[nodiscard]] bool activated() const;
	[[nodiscard]] QList<QuickshellScreenInfo*> screens() const;

	[[nodiscard]] bool maximized() const;
	void setMaximized(bool maximized);

	[[nodiscard]] bool minimized() const;
	void setMinimized(bool minimized);

	[[nodiscard]] bool fullscreen() const;
	void setFullscreen(bool fullscreen);

	[[nodiscard]] qs::windows::TrackedWindow* window() const { return this->mWindow; }

signals:
	void closed();
	void appIdChanged();
	void titleChanged();
	void parentChanged();
	void activatedChanged();
	void screensChanged();
	void maximizedChanged();
	void minimizedChanged();
	void fullscreenChanged();

private:
	qs::windows::TrackedWindow* mWindow;

	friend class ToplevelManager;
};

class ToplevelManager: public QObject {
	Q_OBJECT;

public:
	static ToplevelManager* instance();

	[[nodiscard]] Toplevel* forWindow(qs::windows::TrackedWindow* window) const;
	[[nodiscard]] Toplevel* forAddress(quint64 address) const;

	[[nodiscard]] ObjectModel<Toplevel>* toplevels();

signals:
	void activeToplevelChanged();

private slots:
	void onWindowAdded(qs::windows::TrackedWindow* window);
	void onWindowRemoved(qs::windows::TrackedWindow* window);
	void onActiveWindowChanged();

private:
	explicit ToplevelManager();

	ObjectModel<Toplevel> mToplevels {this};
	QHash<qs::windows::TrackedWindow*, Toplevel*> byWindow;
	Toplevel* mActiveToplevel = nullptr;

	DECLARE_PRIVATE_MEMBER(
	    ToplevelManager,
	    activeToplevel,
	    setActiveToplevel,
	    mActiveToplevel,
	    activeToplevelChanged
	);
};

///! Exposes a list of Toplevels.
/// Exposes the windows other applications would show in Alt+Tab as @@Toplevel$s.
class ToplevelManagerQml: public QObject {
	Q_OBJECT;
	// clang-format off
	/// All toplevel windows.
	QSDOC_TYPE_OVERRIDE(ObjectModel<qs::wayland::toplevel::Toplevel>*);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	/// The foreground window or null. Stays on the last application window while one of this
	/// process' own windows has the foreground.
	Q_PROPERTY(qs::wayland::toplevel::Toplevel* activeToplevel READ activeToplevel NOTIFY activeToplevelChanged);
	// clang-format on
	QML_NAMED_ELEMENT(ToplevelManager);
	QML_SINGLETON;

public:
	explicit ToplevelManagerQml(QObject* parent = nullptr);

	[[nodiscard]] static ObjectModel<Toplevel>* toplevels();
	[[nodiscard]] static Toplevel* activeToplevel();

signals:
	void activeToplevelChanged();
};

} // namespace qs::wayland::toplevel
