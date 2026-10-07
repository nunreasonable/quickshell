#include "qmlcache.hpp"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <private/qv4compileddata_p.h>
#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qendian.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmutex.h>
#include <qqmlprivate.h>
#include <qset.h>
#include <qstring.h>
#include <qtenvironmentvariables.h>
#include <qurl.h>

#include "logcat.hpp"

namespace qs::qmlcache {

namespace {

QS_LOGGING_CATEGORY(logQmlCache, "quickshell.qmlcache", QtInfoMsg);

constexpr char BUNDLE_MAGIC[8] = {'q', 's', 'q', 'm', 'l', 'c', 'b', '1'};
constexpr quint32 BUNDLE_FORMAT = 1;
constexpr qint64 BUNDLE_MAX_SIZE = qint64(1) << 30;
constexpr quint32 UNIT_ALIGNMENT = 16;

struct BundleHeader {
	char magic[8];
	quint32_le format;
	quint32_le count;
	char qtVersion[16];
	quint32_le stringsOffset;
	quint32_le stringsSize;
	quint32_le reserved[2];
};

static_assert(sizeof(BundleHeader) == 48);

struct BundleEntry {
	quint32_le pathOffset;
	quint32_le pathSize;
	quint32_le unitOffset;
	quint32_le unitSize;
	char sourceMd5[16];
};

static_assert(sizeof(BundleEntry) == 32);

const QQmlPrivate::AOTCompiledFunction NO_AOT_FUNCTIONS[] = {{0, 0, nullptr, nullptr}};

struct Unit {
	QQmlPrivate::CachedQmlUnit cached {};
	QByteArray sourceMd5;
};

struct Bundle {
	char* data = nullptr;
	std::vector<Unit> units;
	QHash<QString, qsizetype> index;
};

struct BundleKey {
	QString path;
	qint64 size = -1;
	QDateTime modified;

	bool operator==(const BundleKey& other) const = default;
};

struct State {
	QMutex mutex;
	QDir root;
	BundleKey key;
	Bundle* bundle = nullptr;
	QHash<QString, QByteArray> hashes;
	QSet<QString> intercepts;
};

State& state() {
	static auto* state = new State();
	return *state;
}

QByteArray hashFile(const QString& path) {
	auto file = QFile(path);
	if (!file.open(QFile::ReadOnly)) return QByteArray();
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Md5);
}

bool rangeInside(quint64 offset, quint64 size, quint64 total) {
	return offset <= total && size <= total - offset;
}

bool validUnit(const char* data, qint64 total, const BundleEntry& entry, QString* error) {
	if (!rangeInside(entry.unitOffset, entry.unitSize, total)) {
		*error = QStringLiteral("unit outside the file");
		return false;
	}

	if ((reinterpret_cast<quintptr>(data) + entry.unitOffset) % UNIT_ALIGNMENT != 0) {
		*error = QStringLiteral("misaligned unit");
		return false;
	}

	if (entry.unitSize < sizeof(QV4::CompiledData::Unit)) {
		*error = QStringLiteral("truncated unit");
		return false;
	}

	const auto* unit = reinterpret_cast<const QV4::CompiledData::Unit*>(data + entry.unitOffset);

	if (std::memcmp(unit->magic, QV4::CompiledData::magic_str, sizeof(unit->magic)) != 0) {
		*error = QStringLiteral("bad unit magic");
		return false;
	}

	if (unit->version != quint32(QV4_DATA_STRUCTURE_VERSION)) {
		*error = QStringLiteral("unit data structure version %1, this Qt expects %2")
		             .arg(quint32(unit->version), 0, 16)
		             .arg(QV4_DATA_STRUCTURE_VERSION, 0, 16);
		return false;
	}

	if (unit->unitSize != entry.unitSize) {
		*error = QStringLiteral("unit size mismatch");
		return false;
	}

	if (!(unit->flags & QV4::CompiledData::Unit::StaticData)) {
		*error = QStringLiteral("unit is not static data");
		return false;
	}

	if (unit->sourceTimeStamp != 0) {
		*error = QStringLiteral("unit carries a source time stamp");
		return false;
	}

	return true;
}

Bundle* parseBundle(char* data, qint64 size, QString* error) {
	if (size < qint64(sizeof(BundleHeader))) {
		*error = QStringLiteral("file too small");
		return nullptr;
	}

	BundleHeader header {};
	std::memcpy(&header, data, sizeof(header));

	if (std::memcmp(header.magic, BUNDLE_MAGIC, sizeof(BUNDLE_MAGIC)) != 0) {
		*error = QStringLiteral("bad magic");
		return nullptr;
	}

	if (header.format != BUNDLE_FORMAT) {
		*error = QStringLiteral("unsupported format %1").arg(quint32(header.format));
		return nullptr;
	}

	auto qtVersion =
	    QString::fromLatin1(header.qtVersion, qstrnlen(header.qtVersion, sizeof(header.qtVersion)));
	if (qtVersion != QLatin1StringView(qVersion())) {
		*error = QStringLiteral("built for Qt %1, running Qt %2")
		             .arg(qtVersion, QString::fromLatin1(qVersion()));
		return nullptr;
	}

	const auto total = quint64(size);
	const quint64 count = header.count;
	if (!rangeInside(sizeof(BundleHeader), count * sizeof(BundleEntry), total)
	    || !rangeInside(header.stringsOffset, header.stringsSize, total))
	{
		*error = QStringLiteral("index outside the file");
		return nullptr;
	}

	auto* bundle = new Bundle();
	bundle->data = data;
	bundle->units.resize(count);
	bundle->index.reserve(static_cast<qsizetype>(count));

	const char* strings = data + header.stringsOffset;

	for (quint64 i = 0; i != count; i++) {
		BundleEntry entry {};
		std::memcpy(&entry, data + sizeof(BundleHeader) + i * sizeof(BundleEntry), sizeof(entry));

		if (!rangeInside(entry.pathOffset, entry.pathSize, header.stringsSize)) {
			*error = QStringLiteral("path outside the string table");
			delete bundle;
			return nullptr;
		}

		auto path = QString::fromUtf8(strings + entry.pathOffset, entry.pathSize);

		QString unitError;
		if (!validUnit(data, size, entry, &unitError)) {
			*error = path % QStringLiteral(": ") % unitError;
			delete bundle;
			return nullptr;
		}

		auto& unit = bundle->units[i];
		unit.cached.qmlData = reinterpret_cast<const QV4::CompiledData::Unit*>(data + entry.unitOffset);
		unit.cached.aotCompiledFunctions = NO_AOT_FUNCTIONS;
		unit.sourceMd5 = QByteArray(entry.sourceMd5, sizeof(entry.sourceMd5));
		bundle->index.insert(path, static_cast<qsizetype>(i));
	}

	return bundle;
}

Bundle* loadBundle(const QString& path) {
	auto file = QFile(path);
	if (!file.open(QFile::ReadOnly)) {
		qCWarning(logQmlCache) << "Could not open QML bundle" << path << file.errorString();
		return nullptr;
	}

	auto size = file.size();
	if (size <= 0 || size > BUNDLE_MAX_SIZE) {
		qCWarning(logQmlCache) << "Ignoring QML bundle" << path << "with unexpected size" << size;
		return nullptr;
	}

	auto* data = static_cast<char*>(std::malloc(static_cast<size_t>(size)));
	if (!data) return nullptr;

	if (file.read(data, size) != size) {
		qCWarning(logQmlCache) << "Could not read QML bundle" << path << file.errorString();
		std::free(data);
		return nullptr;
	}

	QString error;
	auto* bundle = parseBundle(data, size, &error);
	if (!bundle) {
		qCWarning(logQmlCache).noquote() << "Ignoring QML bundle" << path << "-" << error;
		std::free(data);
		return nullptr;
	}

	qCInfo(logQmlCache) << "Loaded QML bundle with" << bundle->units.size() << "units from" << path;
	return bundle;
}

const QQmlPrivate::CachedQmlUnit* lookup(const QUrl& url) {
	if (url.scheme() != QLatin1StringView("qs")) return nullptr;

	auto path = url.path();
	if (!path.startsWith(QLatin1StringView("@/qs/"))) return nullptr;

	auto relative = QDir::cleanPath(path.sliced(5));

	auto& state = qs::qmlcache::state();
	auto locker = QMutexLocker(&state.mutex);

	if (!state.bundle) return nullptr;

	auto it = state.bundle->index.constFind(relative);
	if (it == state.bundle->index.constEnd()) {
		qCDebug(logQmlCache) << "No cached unit for" << relative;
		return nullptr;
	}

	auto file = QDir::cleanPath(state.root.filePath(relative));
	if (state.intercepts.contains(file)) {
		qCDebug(logQmlCache) << "Not using cached unit for preprocessed file" << relative;
		return nullptr;
	}

	auto hash = state.hashes.value(file);
	if (hash.isEmpty()) hash = hashFile(file);

	const auto& unit = state.bundle->units.at(*it);
	if (hash != unit.sourceMd5) {
		qCDebug(logQmlCache) << "Cached unit is stale for" << relative;
		return nullptr;
	}

	qCDebug(logQmlCache) << "Using cached unit for" << relative;
	return &unit.cached;
}

} // namespace

void activate(
    const QDir& configRoot,
    const QHash<QString, QByteArray>& fileHashes,
    const QHash<QString, QString>& fileIntercepts
) {
	static const bool disabled = qEnvironmentVariableIsSet("QS_DISABLE_QMLCACHE");
	if (disabled) return;

	auto key = BundleKey();
	if (configRoot.isAbsolute()) {
		auto path = configRoot.filePath(QStringLiteral(".qmlcache/bundle.bin"));
		auto info = QFileInfo(path);
		if (info.isFile()) key = {.path = path, .size = info.size(), .modified = info.lastModified()};
	}

	auto hashes = QHash<QString, QByteArray>();
	auto intercepts = QSet<QString>();
	if (!key.path.isEmpty()) {
		hashes.reserve(fileHashes.size());
		for (auto it = fileHashes.constBegin(); it != fileHashes.constEnd(); ++it) {
			hashes.insert(QDir::cleanPath(it.key()), it.value());
		}

		for (auto it = fileIntercepts.constBegin(); it != fileIntercepts.constEnd(); ++it) {
			intercepts.insert(QDir::cleanPath(it.key()));
		}
	}

	auto& state = qs::qmlcache::state();
	auto active = false;

	{
		auto locker = QMutexLocker(&state.mutex);

		if (key != state.key) {
			state.key = key;
			state.bundle = key.path.isEmpty() ? nullptr : loadBundle(key.path);
		}

		state.root = configRoot;
		state.hashes = std::move(hashes);
		state.intercepts = std::move(intercepts);
		active = state.bundle != nullptr;
	}

	if (!active) return;

	static std::once_flag registered;
	std::call_once(registered, [] {
		auto registration = QQmlPrivate::RegisterQmlUnitCacheHook {
		    .structVersion = 0,
		    .lookupCachedQmlUnit = &lookup,
		};

		QQmlPrivate::qmlregister(QQmlPrivate::QmlUnitCacheHookRegistration, &registration);
	});
}

} // namespace qs::qmlcache
