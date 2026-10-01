#pragma once

#include <memory>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qvector.h>

#include "../../../core/model.hpp"
#include "pw_link.hpp"
#include "pw_node.hpp"

namespace qs::windows::services::pipewire {

class PwBackend;

///! Contains links to all Core Audio objects, standing in for `Quickshell.Services.Pipewire`'s
/// `Pipewire` singleton.
///
/// Backed by Windows Core Audio (WASAPI device enumeration, `IAudioSessionManager2` for
/// per-application streams) rather than pipewire; see @@PwNode for how devices/sessions map
/// onto nodes. `links` is always empty -- Core Audio has no per-channel routing graph -- use
/// @@linkGroups instead, which is populated for capture devices currently being recorded from.
class Pipewire: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(UntypedObjectModel* nodes READ nodes CONSTANT);
	Q_PROPERTY(UntypedObjectModel* links READ links CONSTANT);
	Q_PROPERTY(UntypedObjectModel* linkGroups READ linkGroups CONSTANT);
	Q_PROPERTY(qs::windows::services::pipewire::PwNode* defaultAudioSink READ defaultAudioSink NOTIFY defaultAudioSinkChanged);
	Q_PROPERTY(qs::windows::services::pipewire::PwNode* defaultAudioSource READ defaultAudioSource NOTIFY defaultAudioSourceChanged);
	Q_PROPERTY(qs::windows::services::pipewire::PwNode* preferredDefaultAudioSink READ defaultAudioSink WRITE setPreferredDefaultAudioSink NOTIFY defaultAudioSinkChanged);
	Q_PROPERTY(qs::windows::services::pipewire::PwNode* preferredDefaultAudioSource READ defaultAudioSource WRITE setPreferredDefaultAudioSource NOTIFY defaultAudioSourceChanged);
	Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged);
	// clang-format on
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Pipewire(QObject* parent = nullptr);
	~Pipewire() override;
	Q_DISABLE_COPY_MOVE(Pipewire);

	[[nodiscard]] UntypedObjectModel* nodes() { return &this->mNodes; }
	[[nodiscard]] UntypedObjectModel* links() { return &this->mLinks; }
	[[nodiscard]] UntypedObjectModel* linkGroups() { return &this->mLinkGroups; }

	[[nodiscard]] PwNode* defaultAudioSink() const { return this->mDefaultSink; }
	[[nodiscard]] PwNode* defaultAudioSource() const { return this->mDefaultSource; }

	void setPreferredDefaultAudioSink(PwNode* node);
	void setPreferredDefaultAudioSource(PwNode* node);

	[[nodiscard]] bool isReady() const { return this->mReady; }

	// Non-QML accessor used by PwNodeLinkTracker.
	[[nodiscard]] const QList<PwLinkGroup*>& linkGroupList() const { return this->mLinkGroups.valueList(); }

	// The one Pipewire instance (the QML engine only ever creates one, being QML_SINGLETON);
	// null before that happens. Used by PwNodeLinkTracker, which upstream can also construct
	// standalone before any Pipewire access.
	static Pipewire* instance();

	// --- Called by PwBackend (GUI thread only) ---
	void backendAddNode(PwNode* node);
	void backendRemoveNode(PwNode* node);
	void backendAddLinkGroup(PwLinkGroup* group);
	void backendRemoveLinkGroup(PwLinkGroup* group);
	void backendSetDefaultSink(PwNode* node);
	void backendSetDefaultSource(PwNode* node);
	void backendSetReady(bool ready);

signals:
	void defaultAudioSinkChanged();
	void defaultAudioSourceChanged();
	void readyChanged();

private:
	ObjectModel<PwNode> mNodes {this};
	ObjectModel<PwLink> mLinks {this};
	ObjectModel<PwLinkGroup> mLinkGroups {this};

	PwNode* mDefaultSink = nullptr;
	PwNode* mDefaultSource = nullptr;
	bool mReady = false;

	std::unique_ptr<PwBackend> backend;
};

} // namespace qs::windows::services::pipewire
