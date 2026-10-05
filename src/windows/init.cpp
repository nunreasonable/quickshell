#include <qcoreapplication.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qfont.h>
#include <qfontdatabase.h>
#include <qicon.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qqml.h>
#include <qstring.h>

#include "../core/plugin.hpp"
#include "desktopentry_backend.hpp"
#include "panel_window.hpp"

namespace {

void loadBundledFonts() {
	auto dir = QDir(QCoreApplication::applicationDirPath() + "/fonts");
	if (!dir.exists()) return;

	auto iter = QDirIterator(
	    dir.path(),
	    {"*.ttf", "*.otf", "*.ttc"},
	    QDir::Files,
	    QDirIterator::Subdirectories
	);

	while (iter.hasNext()) {
		auto path = iter.next();
		if (QFontDatabase::addApplicationFont(path) == -1) {
			qWarning() << "Failed to load bundled font" << path;
		}
	}

	QFont::insertSubstitution("JetBrains Mono NF", "JetBrainsMono Nerd Font");
	QFont::insertSubstitution("JetBrains Mono", "JetBrainsMono Nerd Font");
}

void addBundledIconPath() {
	auto dir = QCoreApplication::applicationDirPath() + "/icons";
	if (!QDir(dir).exists()) return;

	auto paths = QIcon::fallbackSearchPaths();
	if (!paths.contains(dir)) paths.prepend(dir);
	QIcon::setFallbackSearchPaths(paths);
}

class WindowsPlugin: public QsEnginePlugin {
	QString name() override { return "windows"; }
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override {
		loadBundledFonts();
		addBundledIconPath();
		qs::windows::WindowsDesktopEntryBackend::install();
	}

	void registerTypes() override {
		qmlRegisterType<qs::windows::WinPanelInterface>(
		    "Quickshell._WindowsOverlay",
		    1,
		    0,
		    "PanelWindow"
		);

		qmlRegisterModuleImport(
		    "Quickshell",
		    QQmlModuleImportModuleAny,
		    "Quickshell._WindowsOverlay",
		    QQmlModuleImportLatest
		);
	}
};

QS_REGISTER_PLUGIN(WindowsPlugin);

} // namespace
