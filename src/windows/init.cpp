#include <qcoreapplication.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qfontdatabase.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qqml.h>
#include <qstring.h>

#include "../core/plugin.hpp"
#include "panel_window.hpp"

namespace {

// Fonts shipped next to the executable (<exe dir>/fonts, recursively). Shell configurations
// usually depend on icon and UI fonts that are not installed system wide on Windows.
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
}

// Windows backend plugin. Registered after _Window so module overlays apply in the right order,
// exactly like the wayland and x11 plugins.
class WindowsPlugin: public QsEnginePlugin {
	QString name() override { return "windows"; }
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override { loadBundledFonts(); }

	void registerTypes() override {
		qmlRegisterType<qs::windows::WinPanelInterface>(
		    "Quickshell._WindowsOverlay",
		    1,
		    0,
		    "PanelWindow"
		);

		// Same trick as the wayland and x11 backends: the overlay module replaces the
		// uncreatable PanelWindow of Quickshell._Window.
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
