// Windows stand-in until Windows.Graphics.Capture backs it. Shows nothing.
import QtQuick

Item {
	property QtObject captureSource: null
	property bool paintCursor: false
	property bool live: false
	readonly property bool hasContent: false
	readonly property size sourceSize: Qt.size(0, 0)
	property size constraintSize: Qt.size(0, 0)

	signal stopped()

	function captureFrame() {}
}
