#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

namespace qs::windows::services::pipewire {

///! Binds pipewire objects.
/// Upstream uses this to make unbound nodes' properties/audio valid. Core Audio has no
/// "unbound" state -- every @@PwNode's properties and @@PwNode.audio are always live -- so on
/// Windows this is just an inert holder for whatever QML assigns to @@objects.
class PwObjectTracker: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QList<QObject*> objects READ objects WRITE setObjects NOTIFY objectsChanged);
	QML_ELEMENT;

public:
	explicit PwObjectTracker(QObject* parent = nullptr): QObject(parent) {}
	~PwObjectTracker() override;
	Q_DISABLE_COPY_MOVE(PwObjectTracker);

	[[nodiscard]] QList<QObject*> objects() const { return this->mObjects; }
	void setObjects(const QList<QObject*>& objects);

signals:
	void objectsChanged();

private slots:
	void onObjectDestroyed(QObject* object);

private:
	QList<QObject*> mObjects;
};

} // namespace qs::windows::services::pipewire
