#pragma once

#include <qobject.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qt_windows.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

///! Current and installed keyboard input layouts, for indicators that show/switch them.
/// There is no global "layout changed" broadcast on Windows, so this refreshes on
/// `EVENT_SYSTEM_FOREGROUND` (covers switching to a window with a different per-window layout)
/// and a cheap poll while @@active (covers switching layout within the same window, e.g. with
/// the language hotkey).
class Keyboard: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	/// BCP-47 tag of the active layout's locale (e.g. "en-US"), for the foreground window's thread.
	Q_PROPERTY(QString currentLayoutCode READ default NOTIFY currentLayoutChanged BINDABLE bindableCurrentLayoutCode);
	/// Localized display name of the active layout's locale.
	Q_PROPERTY(QString currentLayoutName READ default NOTIFY currentLayoutChanged BINDABLE bindableCurrentLayoutName);
	/// BCP-47 tags of every installed layout.
	Q_PROPERTY(QStringList layoutCodes READ default NOTIFY layoutsChanged BINDABLE bindableLayoutCodes);
	/// Whether polling for in-window layout changes is enabled. Defaults to true.
	Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged);
	// clang-format on

public:
	explicit Keyboard(QObject* parent = nullptr);
	~Keyboard() override;
	Q_DISABLE_COPY_MOVE(Keyboard);

	[[nodiscard]] bool active() const { return this->mActive; }
	void setActive(bool active);

	[[nodiscard]] QBindable<QString> bindableCurrentLayoutCode() const {
		return &this->bCurrentLayoutCode;
	}
	[[nodiscard]] QBindable<QString> bindableCurrentLayoutName() const {
		return &this->bCurrentLayoutName;
	}
	[[nodiscard]] QBindable<QStringList> bindableLayoutCodes() const {
		return &this->bLayoutCodes;
	}

	/// Switches the foreground window's layout to `code` (one of @@layoutCodes).
	Q_INVOKABLE void activateLayout(const QString& code);

	// Called from the global WinEvent hook (keyboard.cpp); public so the free function callback
	// can reach the singleton without being a friend.
	void refresh();

signals:
	void currentLayoutChanged();
	void layoutsChanged();
	void activeChanged();

private:
	void refreshLayoutList();

	bool mActive = true;
	QTimer pollTimer;
	HWINEVENTHOOK hook = nullptr;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QString, bCurrentLayoutCode, &Keyboard::currentLayoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QString, bCurrentLayoutName, &Keyboard::currentLayoutChanged);
	Q_OBJECT_BINDABLE_PROPERTY(Keyboard, QStringList, bLayoutCodes, &Keyboard::layoutsChanged);
	// clang-format on
};

} // namespace qs::windows::sys
