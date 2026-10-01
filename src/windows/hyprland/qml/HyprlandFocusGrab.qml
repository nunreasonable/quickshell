// Quickshell.Hyprland's HyprlandFocusGrab on Windows: the native Quickshell.Windows FocusGrab.
// While active, a mouse press outside every listed window (or their popups), or another
// process taking the foreground, clears the grab and emits cleared(), like hyprland_focus_grab_v1.
// Same properties: `active`, `windows`; same signal: `cleared()`.
import Quickshell.Windows

FocusGrab {}
