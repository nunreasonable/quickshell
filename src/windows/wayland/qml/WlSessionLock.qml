// Windows has no client side session lock: locking hands over to the real Windows lock
// screen (LockWorkStation) and the lock is released right away on this side.
import QtQuick
import Quickshell

QtObject {
	property bool locked: false
	readonly property bool secure: false
	property Component surface: null

	signal unlock()

	onLockedChanged: {
		if (!locked) return;
		Quickshell.execDetached(["rundll32.exe", "user32.dll,LockWorkStation"]);
		Qt.callLater(() => { locked = false; });
	}
}
