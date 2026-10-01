// Windows stand-in for a top level window, see ToplevelManager.
import QtQuick

QtObject {
	property string appId: ""
	property string title: ""
	property Toplevel parent: null
	property bool activated: false
	property list<QtObject> screens: []
	property bool maximized: false
	property bool minimized: false
	property bool fullscreen: false

	signal closed()

	function activate() {}
	function close() {}
	function fullscreenOn(screen) {}
	function setRectangle(window, rect) {}
	function unsetRectangle() {}
}
