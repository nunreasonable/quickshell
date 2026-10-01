pragma Singleton

// Windows stand-in until the native window tracker (SetWinEventHook) backs it.
import QtQuick

QtObject {
	readonly property QtObject toplevels: QtObject {
		readonly property list<QtObject> values: []
	}
	readonly property Toplevel activeToplevel: null
}
