#include "desktopentry_backend.hpp"

#include <utility>

#include <qdebug.h>
#include <qfileinfo.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qthreadpool.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "../core/desktopentry.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logAppsFolder, "quickshell.windows.appsfolder", QtWarningMsg);

// RAII COM apartment for the lifetime of a worker-thread scan or launch. Never construct this
// on the Qt GUI thread: Qt already initializes it as STA there, and per docs/AGENTS.md,
// apartment (re-)init must happen on its own worker thread.
struct ComApartment {
	explicit ComApartment(DWORD coinit): hr(CoInitializeEx(nullptr, coinit)) {}

	ComApartment(const ComApartment&) = delete;
	ComApartment(ComApartment&&) = delete;
	ComApartment& operator=(const ComApartment&) = delete;
	ComApartment& operator=(ComApartment&&) = delete;

	~ComApartment() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

QString takeComString(PWSTR raw) {
	if (raw == nullptr) return QString();
	auto str = QString::fromWCharArray(raw);
	CoTaskMemFree(raw);
	return str;
}

const auto APPS_FOLDER_PREFIX = QStringLiteral("shell:AppsFolder\\");

} // namespace

QMutex WindowsDesktopEntryBackend::sRegistryMutex;
QHash<QString, QString> WindowsDesktopEntryBackend::sRegistry; // NOLINT

QString WindowsDesktopEntryBackend::parsingNameForId(const QString& id) {
	QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
	return WindowsDesktopEntryBackend::sRegistry.value(id);
}

void WindowsDesktopEntryBackend::install() {
	static WindowsDesktopEntryBackend backend; // NOLINT
	DesktopEntryManager::installBackend(&backend);
}

QStringList WindowsDesktopEntryBackend::watchPaths() {
	auto paths = QStringList();

	// Start-menu shortcuts live here (machine-wide and per-user); the Apps folder itself picks
	// up both these and packaged apps, but it has no directory to watch, so a change to either
	// of these is used as the trigger to rescan it.
	auto programData = qEnvironmentVariable("ProgramData");
	if (!programData.isEmpty()) {
		paths << programData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs");
	}

	auto appData = qEnvironmentVariable("AppData");
	if (!appData.isEmpty()) {
		paths << appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs");
	}

	return paths;
}

QList<ParsedDesktopEntryData> WindowsDesktopEntryBackend::scan() {
	auto results = QList<ParsedDesktopEntryData>();

	auto com = ComApartment(COINIT_MULTITHREADED);
	if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
		qCWarning(logAppsFolder) << "CoInitializeEx failed:" << Qt::hex << com.hr;
		return results;
	}

	ComPtr<IShellItem> appsFolder;
	auto hr = SHGetKnownFolderItem(
	    FOLDERID_AppsFolder,
	    KF_FLAG_DONT_VERIFY,
	    nullptr,
	    IID_PPV_ARGS(&appsFolder)
	);

	if (FAILED(hr)) {
		qCWarning(logAppsFolder) << "SHGetKnownFolderItem(FOLDERID_AppsFolder) failed:" << Qt::hex
		                         << hr;
		return results;
	}

	ComPtr<IEnumShellItems> enumItems;
	hr = appsFolder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumItems));

	if (FAILED(hr)) {
		qCWarning(logAppsFolder) << "BindToHandler(BHID_EnumItems) failed:" << Qt::hex << hr;
		return results;
	}

	auto registry = QHash<QString, QString>();

	ComPtr<IShellItem> item;
	while (enumItems->Next(1, item.GetAddressOf(), nullptr) == S_OK) {
		auto current = item;
		item.Reset();

		PWSTR rawParsing = nullptr;
		if (FAILED(current->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &rawParsing))) continue;
		auto parsingName = takeComString(rawParsing);

		auto token = parsingName;
		if (token.startsWith(APPS_FOLDER_PREFIX, Qt::CaseInsensitive)) {
			token = token.sliced(APPS_FOLDER_PREFIX.length());
		}

		if (token.isEmpty()) continue;

		PWSTR rawDisplay = nullptr;
		auto displayHr = current->GetDisplayName(SIGDN_NORMALDISPLAY, &rawDisplay);
		auto displayName = SUCCEEDED(displayHr) ? takeComString(rawDisplay) : token;

		PWSTR rawFsPath = nullptr;
		auto fsHr = current->GetDisplayName(SIGDN_FILESYSPATH, &rawFsPath);
		auto fsPath = SUCCEEDED(fsHr) ? takeComString(rawFsPath) : QString();

		// Packaged (UWP) apps have no filesystem path through the Apps folder; Win32 ones do.
		auto packaged = fsPath.isEmpty();

		QString id;
		QString workingDirectory;
		QVector<QString> keywords;

		if (packaged) {
			id = token.toLower();
			keywords = {token};
		} else {
			auto info = QFileInfo(fsPath);
			id = info.completeBaseName().toLower();
			workingDirectory = info.absolutePath();
			keywords = {info.completeBaseName(), info.fileName()};
		}

		if (id.isEmpty() || registry.contains(id)) continue;

		ParsedDesktopEntryData data;
		data.id = id;
		data.name = displayName;
		data.comment = packaged ? QString() : fsPath;
		data.icon = QStringLiteral("appicon:") + id;
		data.execString = token;
		data.command = {token};
		data.workingDirectory = workingDirectory;
		data.categories = {packaged ? QStringLiteral("PackagedApp") : QStringLiteral("Application")
		};
		data.keywords = keywords;

		registry.insert(id, token);
		results.append(std::move(data));
	}

	{
		QMutexLocker locker(&WindowsDesktopEntryBackend::sRegistryMutex);
		WindowsDesktopEntryBackend::sRegistry = registry;
	}

	qCDebug(logAppsFolder) << "Scanned" << results.size() << "apps from the Windows Apps folder";
	return results;
}

namespace {

void launchToken(const QString& token, const QString& workingDirectory) {
	// Shell activation (both of the paths below) wants an STA, which Qt already sets up for
	// its own GUI thread; this always runs on a throwaway worker thread instead so a slow
	// cold start doesn't stall the GUI.
	auto com = ComApartment(COINIT_APARTMENTTHREADED);

	ComPtr<IApplicationActivationManager> activationManager;
	auto hr = CoCreateInstance(
	    CLSID_ApplicationActivationManager,
	    nullptr,
	    CLSCTX_INPROC_SERVER,
	    IID_PPV_ARGS(&activationManager)
	);

	if (SUCCEEDED(hr)) {
		DWORD pid = 0;
		auto wtoken = token.toStdWString();
		hr = activationManager->ActivateApplication(wtoken.c_str(), nullptr, AO_NONE, &pid);
		if (SUCCEEDED(hr)) return;

		qCDebug(logAppsFolder) << "ActivateApplication failed for" << token << ":" << Qt::hex << hr
		                       << "- falling back to ShellExecuteExW";
	} else {
		qCDebug(logAppsFolder) << "CoCreateInstance(ApplicationActivationManager) failed:"
		                       << Qt::hex << hr;
	}

	// Works for both Win32 and packaged apps alike (the task description's own suggestion):
	// the shell resolves the same "AppsFolder\<token>" path ActivateApplication would.
	auto shellPath = APPS_FOLDER_PREFIX + token;
	auto wpath = shellPath.toStdWString();
	auto wdir = workingDirectory.toStdWString();

	auto info = SHELLEXECUTEINFOW {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC;
	info.lpVerb = L"open";
	info.lpFile = wpath.c_str();
	info.lpDirectory = wdir.empty() ? nullptr : wdir.c_str();
	info.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&info)) {
		qCWarning(logAppsFolder) << "ShellExecuteExW failed for" << shellPath << ":"
		                         << GetLastError();
	}
}

class LaunchTask: public QRunnable {
public:
	LaunchTask(QString token, QString workingDirectory)
	    : token(std::move(token))
	    , workingDirectory(std::move(workingDirectory)) {}

	void run() override { launchToken(this->token, this->workingDirectory); }

private:
	QString token;
	QString workingDirectory;
};

} // namespace

void WindowsDesktopEntryBackend::execute(
    const QVector<QString>& command,
    const QString& workingDirectory
) {
	if (command.isEmpty()) {
		qCWarning(logAppsFolder) << "Cannot launch: empty command";
		return;
	}

	auto* task = new LaunchTask(command.first(), workingDirectory);
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

} // namespace qs::windows
