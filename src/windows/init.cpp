#include <qcoreapplication.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qelapsedtimer.h>
#include <qfont.h>
#include <qfontdatabase.h>
#include <qicon.h>
#include <qguiapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qqml.h>
#include <qstring.h>

#include "../core/logcat.hpp"
#include "../core/plugin.hpp"
#include "desktopentry_backend.hpp"
#include "panel_window.hpp"
#include "util.hpp"

namespace {

QS_LOGGING_CATEGORY(logStartup, "quickshell.startup", QtWarningMsg);

constexpr const char* PERUSER_FONT_PREFIX = "jetbrainsmononerdfont-";
constexpr const char* PERUSER_FONT_FAMILY = "JetBrainsMono Nerd Font";

bool allPeruserFontsPresent(const QDir& dir, const QDir& userFontsDir) {
	if (!userFontsDir.exists()) return false;

	auto iter = QDirIterator(
	    dir.path(),
	    {"*.ttf", "*.otf", "*.ttc"},
	    QDir::Files,
	    QDirIterator::Subdirectories
	);
	auto any = false;

	while (iter.hasNext()) {
		iter.next();
		auto name = iter.fileName();
		if (!name.startsWith(PERUSER_FONT_PREFIX, Qt::CaseInsensitive)) continue;
		any = true;
		if (!userFontsDir.exists(name)) return false;
	}

	return any;
}

void loadBundledFonts() {
	auto dir = QDir(QCoreApplication::applicationDirPath() + "/fonts");
	if (!dir.exists()) return;

	auto userFontsDir = QDir(qEnvironmentVariable("LocalAppData") + "/Microsoft/Windows/Fonts");
	auto skipPeruser = allPeruserFontsPresent(dir, userFontsDir);

	enum class Select { All, PeruserOnly, ExceptPeruser };

	auto load = [&](Select which) {
		auto iter = QDirIterator(
		    dir.path(),
		    {"*.ttf", "*.otf", "*.ttc"},
		    QDir::Files,
		    QDirIterator::Subdirectories
		);

		while (iter.hasNext()) {
			auto path = iter.next();
			auto isPeruser = iter.fileName().startsWith(PERUSER_FONT_PREFIX, Qt::CaseInsensitive);
			if (which == Select::PeruserOnly && !isPeruser) continue;
			if (which == Select::ExceptPeruser && isPeruser) continue;

			if (QFontDatabase::addApplicationFont(path) == -1) {
				qWarning() << "Failed to load bundled font" << path;
			}
		}
	};

	load(skipPeruser ? Select::ExceptPeruser : Select::All);

	if (skipPeruser && !QFontDatabase::families().contains(QString(PERUSER_FONT_FAMILY))) {
		load(Select::PeruserOnly);
	}

	QFont::insertSubstitution("JetBrains Mono NF", PERUSER_FONT_FAMILY);
	QFont::insertSubstitution("JetBrains Mono", PERUSER_FONT_FAMILY);
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
		qs::windows::optOutOfProcessPowerThrottling();
		auto timer = QElapsedTimer();
		timer.start();
		loadBundledFonts();
		qCDebug(logStartup) << "Loaded bundled fonts in" << timer.restart() << "ms";
		addBundledIconPath();
		qs::windows::WindowsDesktopEntryBackend::install();
		qCDebug(logStartup) << "Installed the desktop entry backend in" << timer.restart() << "ms";
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
