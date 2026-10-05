#pragma once

#include <qhash.h>
#include <qlist.h>
#include <qmutex.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvector.h>

#include "../core/desktopentry.hpp"

namespace qs::windows {

class WindowsDesktopEntryBackend: public DesktopEntryBackend {
public:
	QList<ParsedDesktopEntryData> scan() override;
	QStringList watchPaths() override;
	void execute(const QVector<QString>& command, const QString& workingDirectory) override;

	static void install();

	static QString parsingNameForId(const QString& id);
	static QString idForParsingName(const QString& token);
	static void launch(const QString& token, const QString& workingDirectory = QString());

private:
	static QMutex sRegistryMutex;
	static QHash<QString, QString> sRegistry;
};

} // namespace qs::windows
