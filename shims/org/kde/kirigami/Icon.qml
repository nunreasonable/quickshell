pragma ComponentBehavior: Bound
// Windows shim for org.kde.kirigami's Icon (the only Kirigami type ii uses).
// Real backend: nothing special is needed here long-term -- this is plain
// QtQuick (Image + an optional MultiEffect recolor pass), it just isn't
// available as "org.kde.kirigami" outside real Kirigami. Reproduces the
// subset of behaviour ii relies on: a source/fallback pair with automatic
// fallback-on-load-error, and isMask/color recoloring for monochrome icons.
import QtQuick
import QtQuick.Effects

Item {
    id: root

    property url source: ""
    property url fallback: ""
    property color color: "transparent"
    property bool isMask: false
    property bool roundToIconSize: true
    property bool animated: false

    implicitWidth: 32
    implicitHeight: 32

    property url _effectiveSource: root.source
    onSourceChanged: root._effectiveSource = root.source

    Image {
        id: img
        anchors.fill: parent
        source: root._effectiveSource
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        onStatusChanged: {
            if (status === Image.Error && String(root.fallback) !== "" && source !== root.fallback) {
                root._effectiveSource = root.fallback;
            }
        }
        layer.enabled: root.isMask
        layer.effect: MultiEffect {
            colorization: 1.0
            colorizationColor: root.color
        }
    }
}
