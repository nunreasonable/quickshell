#include "brightness.hpp"

#include <algorithm>
#include <vector>

#include <qt_windows.h>

#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qrunnable.h>
#include <qscreen.h>
#include <qthreadpool.h>

#include "../../core/logcat.hpp"
#include "../util.hpp"
#include "gamma.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logBrightness, "quickshell.windows.brightness", QtWarningMsg);

bool ddcQuery(HMONITOR hMonitor, qreal& outBrightness) {
	if (hMonitor == nullptr) return false;

	DWORD count = 0;
	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) return false;

	std::vector<PHYSICAL_MONITOR> monitors(count);
	if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors.data())) return false;

	auto ok = false;
	MC_VCP_CODE_TYPE vcpType {};
	DWORD current = 0;
	DWORD maxValue = 0;

	if (GetVCPFeatureAndVCPFeatureReply(monitors[0].hPhysicalMonitor, 0x10, &vcpType, &current, &maxValue)
	    && maxValue > 0)
	{
		outBrightness = qreal(current) / qreal(maxValue);
		ok = true;
	}

	DestroyPhysicalMonitors(count, monitors.data());
	return ok;
}

bool ddcSet(HMONITOR hMonitor, qreal value) {
	if (hMonitor == nullptr) return false;

	DWORD count = 0;
	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) return false;

	std::vector<PHYSICAL_MONITOR> monitors(count);
	if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors.data())) return false;

	MC_VCP_CODE_TYPE vcpType {};
	DWORD current = 0;
	DWORD maxValue = 100;
	GetVCPFeatureAndVCPFeatureReply(monitors[0].hPhysicalMonitor, 0x10, &vcpType, &current, &maxValue);

	auto target = static_cast<DWORD>(std::clamp(value, 0.0, 1.0) * qreal(maxValue) + 0.5);
	auto ok = SetVCPFeature(monitors[0].hPhysicalMonitor, 0x10, target) != FALSE;

	DestroyPhysicalMonitors(count, monitors.data());
	return ok;
}

struct WmiSession {
	bool initializedCom = false;
	IWbemLocator* locator = nullptr;
	IWbemServices* services = nullptr;

	bool open() {
		auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		this->initializedCom = hr == S_OK || hr == S_FALSE;
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

		hr = CoCreateInstance(
		    CLSID_WbemLocator,
		    nullptr,
		    CLSCTX_INPROC_SERVER,
		    IID_IWbemLocator,
		    reinterpret_cast<LPVOID*>(&this->locator) // NOLINT
		);
		if (FAILED(hr)) return false;

		auto* ns = SysAllocString(L"ROOT\\WMI");
		hr = this->locator->ConnectServer(
		    ns,
		    nullptr,
		    nullptr,
		    nullptr,
		    0,
		    nullptr,
		    nullptr,
		    &this->services
		);
		SysFreeString(ns);
		if (FAILED(hr)) return false;

		CoSetProxyBlanket(
		    this->services,
		    RPC_C_AUTHN_WINNT,
		    RPC_C_AUTHZ_NONE,
		    nullptr,
		    RPC_C_AUTHN_LEVEL_CALL,
		    RPC_C_IMP_LEVEL_IMPERSONATE,
		    nullptr,
		    EOAC_NONE
		);

		return true;
	}

	~WmiSession() {
		if (this->services != nullptr) this->services->Release();
		if (this->locator != nullptr) this->locator->Release();
		if (this->initializedCom) CoUninitialize();
	}

	WmiSession() = default;
	WmiSession(const WmiSession&) = delete;
	WmiSession& operator=(const WmiSession&) = delete;
	WmiSession(WmiSession&&) = delete;
	WmiSession& operator=(WmiSession&&) = delete;
};

bool queryWmiBrightness(int index, qreal& outBrightness, QString* outInstanceName = nullptr) {
	WmiSession session;
	if (!session.open()) return false;

	auto* query = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
	auto* language = SysAllocString(L"WQL");
	IEnumWbemClassObject* enumerator = nullptr;
	auto hr = session.services->ExecQuery(
	    language,
	    query,
	    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    &enumerator
	);
	SysFreeString(query);
	SysFreeString(language);
	if (FAILED(hr) || enumerator == nullptr) return false;

	auto found = false;
	IWbemClassObject* obj = nullptr;
	ULONG returned = 0;
	auto current = 0;

	while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
		if (current == index) {
			VARIANT brightness {};
			VariantInit(&brightness);
			if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &brightness, nullptr, nullptr))) {
				outBrightness = qreal(brightness.bVal) / 100.0;
				found = true;
			}
			VariantClear(&brightness);

			if (found && outInstanceName != nullptr) {
				VARIANT instanceName {};
				VariantInit(&instanceName);
				if (SUCCEEDED(obj->Get(L"InstanceName", 0, &instanceName, nullptr, nullptr))
				    && instanceName.vt == VT_BSTR)
				{
					*outInstanceName = QString::fromWCharArray(instanceName.bstrVal);
				}
				VariantClear(&instanceName);
			}

			obj->Release();
			break;
		}

		obj->Release();
		current++;
	}

	enumerator->Release();
	return found;
}

bool setWmiBrightness(int index, qreal value) {
	QString instanceName;
	qreal unused = 0.0;
	if (!queryWmiBrightness(index, unused, &instanceName) || instanceName.isEmpty()) return false;

	WmiSession session;
	if (!session.open()) return false;

	auto escaped = instanceName;
	escaped.replace(QStringLiteral("'"), QStringLiteral("\\'"));
	auto queryString =
	    QStringLiteral("SELECT * FROM WmiMonitorBrightnessMethods WHERE InstanceName = '%1'")
	        .arg(escaped);
	auto* queryText = SysAllocString(reinterpret_cast<const wchar_t*>(queryString.utf16()));
	auto* language = SysAllocString(L"WQL");

	IEnumWbemClassObject* enumerator = nullptr;
	auto hr = session.services->ExecQuery(
	    language,
	    queryText,
	    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    &enumerator
	);
	SysFreeString(queryText);
	SysFreeString(language);
	if (FAILED(hr) || enumerator == nullptr) return false;

	IWbemClassObject* obj = nullptr;
	ULONG returned = 0;
	auto ok = false;

	if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
		VARIANT path {};
		VariantInit(&path);
		obj->Get(L"__PATH", 0, &path, nullptr, nullptr);

		IWbemClassObject* classObj = nullptr;
		IWbemClassObject* inSignature = nullptr;
		auto* className = SysAllocString(L"WmiMonitorBrightnessMethods");
		auto* methodName = SysAllocString(L"WmiSetBrightness");

		if (SUCCEEDED(session.services->GetObject(className, 0, nullptr, &classObj, nullptr))
		    && SUCCEEDED(classObj->GetMethod(methodName, 0, &inSignature, nullptr)))
		{
			IWbemClassObject* instance = nullptr;
			if (SUCCEEDED(inSignature->SpawnInstance(0, &instance))) {
				VARIANT timeout {};
				VariantInit(&timeout);
				timeout.vt = VT_UI4;
				timeout.ulVal = 0;
				instance->Put(L"Timeout", 0, &timeout, 0);

				VARIANT brightness {};
				VariantInit(&brightness);
				brightness.vt = VT_UI1;
				brightness.bVal = static_cast<BYTE>(std::clamp(value, 0.0, 1.0) * 100.0 + 0.5);
				instance->Put(L"Brightness", 0, &brightness, 0);

				IWbemClassObject* outParams = nullptr;
				if (path.vt == VT_BSTR) {
					hr = session.services->ExecMethod(
					    path.bstrVal,
					    methodName,
					    0,
					    nullptr,
					    instance,
					    &outParams,
					    nullptr
					);
					ok = SUCCEEDED(hr);
				}

				if (outParams != nullptr) outParams->Release();
				instance->Release();
			}
			if (inSignature != nullptr) inSignature->Release();
		}
		if (classObj != nullptr) classObj->Release();
		SysFreeString(className);
		SysFreeString(methodName);
		VariantClear(&path);
		obj->Release();
	}

	enumerator->Release();
	return ok;
}

} // namespace

void Brightness::query(const QString& screenName) {
	QScreen* target = nullptr;
	for (auto* screen: QGuiApplication::screens()) {
		if (screen->name() == screenName) {
			target = screen;
			break;
		}
	}

	auto hMonitor = target != nullptr ? qs::windows::monitorForScreen(target) : nullptr;
	auto cachedRoute = this->screenRoute.value(screenName, -2);
	auto resolved = cachedRoute != -2;
	auto knownDdc = cachedRoute == -1;
	auto wmiCandidateIndex = cachedRoute >= 0 ? cachedRoute : this->nextWmiInstanceIndex;

	auto* self = this;
	auto* task = QRunnable::create([self, screenName, hMonitor, resolved, knownDdc, wmiCandidateIndex]() {
		auto brightness = 0.0;
		auto available = false;
		auto isDdc = false;
		auto usedWmiIndex = -1;

		if (resolved && knownDdc) {
			available = ddcQuery(hMonitor, brightness);
			isDdc = true;
		} else if (resolved && !knownDdc) {
			available = queryWmiBrightness(wmiCandidateIndex, brightness);
			usedWmiIndex = wmiCandidateIndex;
		} else if (ddcQuery(hMonitor, brightness)) {
			available = true;
			isDdc = true;
		} else if (queryWmiBrightness(wmiCandidateIndex, brightness)) {
			available = true;
			usedWmiIndex = wmiCandidateIndex;
		}

		QMetaObject::invokeMethod(
		    self,
		    [self, screenName, available, isDdc, brightness, usedWmiIndex]() {
			    if (!self->screenRoute.contains(screenName)) {
				    if (isDdc) {
					    self->screenRoute.insert(screenName, -1);
				    } else if (usedWmiIndex >= 0) {
					    self->screenRoute.insert(screenName, usedWmiIndex);
					    self->nextWmiInstanceIndex =
					        std::max(self->nextWmiInstanceIndex, usedWmiIndex + 1);
				    }
			    }
			    emit self->queried(screenName, available, isDdc, brightness);
		    },
		    Qt::QueuedConnection
		);
	});

	QThreadPool::globalInstance()->start(task);
}

void Brightness::probe(const QString& screenName) {
	auto cachedRoute = this->screenRoute.value(screenName, -2);
	auto wmiCandidateIndex = cachedRoute >= 0 ? cachedRoute : this->nextWmiInstanceIndex;

	auto* self = this;
	auto* task = QRunnable::create([self, screenName, cachedRoute, wmiCandidateIndex]() {
		auto brightness = 1.0;
		auto internal = cachedRoute != -1 && queryWmiBrightness(wmiCandidateIndex, brightness);

		QMetaObject::invokeMethod(
		    self,
		    [self, screenName, internal, brightness, wmiCandidateIndex]() {
			    if (internal && !self->screenRoute.contains(screenName)) {
				    self->screenRoute.insert(screenName, wmiCandidateIndex);
				    self->nextWmiInstanceIndex =
				        std::max(self->nextWmiInstanceIndex, wmiCandidateIndex + 1);
			    }

			    if (internal) emit self->queried(screenName, true, false, brightness);
			    else emit self->queried(screenName, false, true, 1.0);
		    },
		    Qt::QueuedConnection
		);
	});

	QThreadPool::globalInstance()->start(task);
}

void Brightness::setBrightness(const QString& screenName, bool isDdc, qreal value) {
	QScreen* target = nullptr;
	for (auto* screen: QGuiApplication::screens()) {
		if (screen->name() == screenName) {
			target = screen;
			break;
		}
	}

	auto hMonitor = target != nullptr ? qs::windows::monitorForScreen(target) : nullptr;
	auto wmiIndex = this->screenRoute.value(screenName, -1);
	auto clamped = std::clamp(value, 0.0, 1.0);

	if (isDdc && this->software.contains(screenName)) {
		GammaController::instance()->setBrightness(screenName, clamped);
		emit this->brightnessSetFinished(screenName, true);
		return;
	}

	auto* self = this;
	auto* task = QRunnable::create([self, screenName, hMonitor, isDdc, wmiIndex, clamped]() {
		auto ok = false;
		if (isDdc) {
			ok = ddcSet(hMonitor, clamped);
		} else if (wmiIndex >= 0) {
			ok = setWmiBrightness(wmiIndex, clamped);
		}

		QMetaObject::invokeMethod(
		    self,
		    [self, screenName, isDdc, clamped, ok]() {
			    if (isDdc && !ok) {
				    qCInfo(logBrightness) << screenName
				                          << "doesn't take DDC/CI brightness; dimming it in software";
				    self->software.insert(screenName, true);
				    GammaController::instance()->setBrightness(screenName, clamped);
				    emit self->brightnessSetFinished(screenName, true);
				    return;
			    }

			    emit self->brightnessSetFinished(screenName, ok);
		    },
		    Qt::QueuedConnection
		);
	});

	QThreadPool::globalInstance()->start(task);
}

} // namespace qs::windows::sys
