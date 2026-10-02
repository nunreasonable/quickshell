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
	// Reverse of parsingNameForId: the DesktopEntry id whose parsing name token is `token`,
	// compared case-insensitively (toast and window AUMIDs don't always match the Apps folder's
	// casing). Empty if there is none. Lets notifications map a sender's AUMID to its entry.
	static QString idForParsingName(const QString& token);
	// Launches an Apps folder item by its parsing name token (an AUMID for packaged apps and
	// Win32 apps that registered one) on a worker thread, same as execute() does for entries.
	static void launch(const QString& token, const QString& workingDirectory = QString());

private:
	static QMutex sRegistryMutex;
	static QHash<QString, QString> sRegistry; // id (lowercase) -> parsing name token
};

} // namespace qs::windows
