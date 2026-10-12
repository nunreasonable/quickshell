#include "equalizer_apo.hpp"

#include <qt_windows.h>

#include <qbytearray.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qprocess.h>
#include <qstandardpaths.h>
#include <qstringlist.h>
#include <quuid.h>

namespace qs::windows::sys {

namespace {

// Equalizer APO's own APO CLSIDs (public: registered by its installer under
// HKLM\SOFTWARE\Classes\CLSID\{guid}\InprocServer32 pointing at
// EqualizerAPO.dll). Either can show up in an endpoint's FxProperties
// depending on whether it was wired in as the pre-mix (GFX) or post-mix APO.
constexpr auto kApoClsidGfx = u"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
constexpr auto kApoClsidPostMix = u"{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";

QString readRegString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
	wchar_t buffer[1024];
	DWORD size = sizeof(buffer);
	auto status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, buffer, &size);
	if (status != ERROR_SUCCESS) return QString();
	return QString::fromWCharArray(buffer);
}

bool writeFileAtomically(const QString& path, const QByteArray& data) {
	auto dir = QFileInfo(path).absoluteDir();
	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return false;

	auto tempPath = path + u".tmp-" + QUuid::createUuid().toString(QUuid::Id128);
	QFile tempFile(tempPath);
	if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	if (tempFile.write(data) != data.size()) {
		tempFile.close();
		QFile::remove(tempPath);
		return false;
	}
	tempFile.close();

	QFile::remove(path);
	if (!QFile::rename(tempPath, path)) {
		QFile::remove(tempPath);
		return false;
	}
	return true;
}

// Doubles embedded single quotes so `text` can be dropped into a PowerShell
// single-quoted string literal ('...') without closing it early.
QString psSingleQuote(const QString& text) {
	QString escaped = text;
	escaped.replace(u'\'', QStringLiteral("''"));
	return escaped;
}

} // namespace

EqualizerApo::EqualizerApo(QObject* parent): QObject(parent) {
	this->detectInstall();
	this->detectWritable();
	this->detectHooked();
}

QString EqualizerApo::configFilePath() const {
	if (this->mConfigPath.isEmpty()) return QString();
	return QDir(this->mConfigPath).filePath(QStringLiteral("config.txt"));
}

QString EqualizerApo::includeFilePath() const {
	if (this->mConfigPath.isEmpty()) return QString();
	return QDir(this->mConfigPath).filePath(QStringLiteral("ii-equalizer.txt"));
}

void EqualizerApo::detectInstall() {
	auto installPath =
	    readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"InstallPath");
	this->mInstalled = !installPath.isEmpty() && QDir(installPath).exists();
	this->mInstallPath = installPath;

	if (!this->mInstalled) {
		this->mConfigPath = QString();
		return;
	}

	// Equalizer APO's installer lets the user move the config folder
	// elsewhere (ConfigPath); falls back to the default <install>\config
	// when that value is absent (fresh/default installs).
	auto configPath =
	    readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"ConfigPath");
	this->mConfigPath = configPath.isEmpty() ? QDir(installPath).filePath(QStringLiteral("config"))
	                                          : configPath;
}

void EqualizerApo::detectWritable() {
	auto path = this->configFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		this->mConfigWritable = false;
		return;
	}

	// Opening in Append mode (no truncate, no write) is a non-mutating probe
	// of the real ACL - Windows itself decides whether this succeeds, so
	// there's no need to re-implement an AccessCheck against the file's
	// security descriptor ourselves.
	QFile file(path);
	auto ok = file.open(QIODevice::Append);
	if (ok) file.close();
	this->mConfigWritable = ok;
}

void EqualizerApo::detectHooked() {
	auto path = this->configFilePath();
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		this->mHooked = false;
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		this->mHooked = false;
		return;
	}
	auto text = QString::fromUtf8(file.readAll());
	file.close();

	// Lenient on purpose: matches whether our include file's name shows up
	// anywhere in an (uncommented-or-not) Include line, rather than
	// requiring an exact "Include: <absolute path>" match - tolerates the
	// user having it on a relative path, with extra whitespace, etc.
	auto includeName = QFileInfo(this->includeFilePath()).fileName();
	this->mHooked = false;
	for (const auto& rawLine: text.split(u'\n')) {
		auto line = rawLine.trimmed();
		if (!line.startsWith(QStringLiteral("Include:"), Qt::CaseInsensitive)) continue;
		if (line.contains(includeName, Qt::CaseInsensitive)) {
			this->mHooked = true;
			break;
		}
	}
}

void EqualizerApo::detectActiveOnDevice() {
	if (this->mDefaultDeviceId.isEmpty()) {
		this->mActiveOnDevice = false;
		return;
	}

	auto subkey = QStringLiteral(
	                  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"
	              )
	              + this->mDefaultDeviceId + QStringLiteral("\\FxProperties");
	auto wsubkey = subkey.toStdWString();

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wsubkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		this->mActiveOnDevice = false;
		return;
	}

	bool found = false;
	wchar_t valueName[512];
	BYTE valueData[4096];
	for (DWORD index = 0;; index++) {
		DWORD nameLen = sizeof(valueName) / sizeof(wchar_t);
		DWORD dataLen = sizeof(valueData);
		DWORD type = 0;

		auto status =
		    RegEnumValueW(key, index, valueName, &nameLen, nullptr, &type, valueData, &dataLen);
		if (status == ERROR_NO_MORE_ITEMS) break;
		if (status != ERROR_SUCCESS) break;
		if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

		auto value = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(valueData));
		if (value.compare(kApoClsidGfx, Qt::CaseInsensitive) == 0
		    || value.compare(kApoClsidPostMix, Qt::CaseInsensitive) == 0)
		{
			found = true;
			break;
		}
	}

	RegCloseKey(key);
	this->mActiveOnDevice = found;
}

void EqualizerApo::refresh() {
	this->detectInstall();
	this->detectWritable();
	this->detectHooked();
	this->detectActiveOnDevice();
	emit this->changed();
}

void EqualizerApo::setDefaultDeviceId(const QString& id) {
	if (this->mDefaultDeviceId == id) return;
	this->mDefaultDeviceId = id;
	this->detectActiveOnDevice();
	emit this->changed();
}

bool EqualizerApo::writeCurve(const QString& content) const {
	auto path = this->includeFilePath();
	if (path.isEmpty()) return false;
	return writeFileAtomically(path, content.toUtf8());
}

bool EqualizerApo::hookDirectly() {
	if (!this->mConfigWritable) return false;
	if (this->mHooked) return true;

	// Make sure the included file exists before pointing config.txt at it -
	// Equalizer APO logs (and some builds refuse to load) a config that
	// Includes a missing file.
	if (!QFileInfo::exists(this->includeFilePath())) {
		this->writeCurve(QStringLiteral("Preamp: 0 dB\n"));
	}

	QFile file(this->configFilePath());
	if (!file.open(QIODevice::Append | QIODevice::Text)) return false;
	auto line = QStringLiteral("\nInclude: %1\n").arg(this->includeFilePath());
	auto written = file.write(line.toUtf8());
	file.close();
	if (written <= 0) return false;

	this->detectHooked();
	emit this->changed();
	return this->mHooked;
}

void EqualizerApo::hookElevated() {
	if (this->mConfigPath.isEmpty()) return;

	if (!QFileInfo::exists(this->includeFilePath())) {
		this->writeCurve(QStringLiteral("Preamp: 0 dB\n"));
	}

	// The elevated step only ever runs this generated script - never an
	// arbitrary inline command - so the config.txt/include paths (which may
	// contain spaces) never need to survive multiple layers of shell/argv
	// quoting.
	auto script = QStringLiteral(
	                   "$configPath = '%1'\n"
	                   "$line = 'Include: %2'\n"
	                   "$text = $null\n"
	                   "try { $text = Get-Content -LiteralPath $configPath -Raw -ErrorAction Stop } catch { $text = '' }\n"
	                   "if ($null -eq $text) { $text = '' }\n"
	                   "if ($text -notmatch [regex]::Escape($line)) {\n"
	                   "    Add-Content -LiteralPath $configPath -Value \"`n$line\"\n"
	                   "}\n"
	               )
	                   .arg(psSingleQuote(this->configFilePath()), psSingleQuote(this->includeFilePath()));

	auto scriptDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	auto scriptPath = QDir(scriptDir).filePath(QStringLiteral("ii-equalizer-hook.ps1"));
	if (!writeFileAtomically(scriptPath, script.toUtf8())) return;

	// One elevated step: an outer, unprivileged powershell asks Windows to
	// relaunch a second powershell -Verb RunAs (the actual UAC prompt),
	// which runs nothing but -File <our generated script>. Never cmd /c, so
	// the quoting pitfalls docs/AGENTS.md warns about don't apply here.
	auto outerCommand = QStringLiteral(
	                         "Start-Process -FilePath 'powershell.exe' "
	                         "-ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%1' "
	                         "-Verb RunAs"
	                     )
	                         .arg(scriptPath);

	QProcess::startDetached(
	    QStringLiteral("powershell.exe"),
	    {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), outerCommand}
	);
}

} // namespace qs::windows::sys
