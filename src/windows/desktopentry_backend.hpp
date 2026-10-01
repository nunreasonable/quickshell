#pragma once

#include <qhash.h>
#include <qlist.h>
#include <qmutex.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvector.h>

#include "../core/desktopentry.hpp"

namespace qs::windows {

// Backs Quickshell's DesktopEntries singleton with the Windows "Apps" folder
// (shell:AppsFolder / FOLDERID_AppsFolder), which covers both Start-menu Win32 shortcuts and
// installed UWP/packaged apps. See docs/AGENTS.md for the overall approach.
class WindowsDesktopEntryBackend: public DesktopEntryBackend {
public:
	QList<ParsedDesktopEntryData> scan() override;
	QStringList watchPaths() override;
	void execute(const QVector<QString>& command, const QString& workingDirectory) override;

	// Installs the backend into DesktopEntryManager. Call once, before any QML engine exists
	// (WindowsPlugin::init() runs early enough; see src/windows/init.cpp).
	static void install();

	// The shell "parsing name" token (SIGDN_DESKTOPABSOLUTEPARSING with the
	// "shell:AppsFolder\" prefix stripped - an AUMID for packaged apps, an opaque shell token
	// for Win32 ones) for a given DesktopEntry id, as recorded by the last scan(). Empty if
	// `id` is unknown. Used by the icon provider to resolve `appicon:<id>` keys back to a
	// shell item without sharing COM pointers across threads.
	static QString parsingNameForId(const QString& id);

private:
	static QMutex sRegistryMutex;
	static QHash<QString, QString> sRegistry; // id (lowercase) -> parsing name token
};

} // namespace qs::windows
