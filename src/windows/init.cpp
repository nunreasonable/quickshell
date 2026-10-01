#include <qlist.h>
#include <qstring.h>

#include "../core/plugin.hpp"

namespace {

// Windows backend plugin. Registered after _Window so module overlays apply in the right order,
// exactly like the wayland and x11 plugins.
//
// TODO(windows): register the Quickshell.Windows module (PanelWindow backend etc.) here.
class WindowsPlugin: public QsEnginePlugin {
	QString name() override { return "windows"; }
	QList<QString> dependencies() override { return {"window"}; }
};

QS_REGISTER_PLUGIN(WindowsPlugin);

} // namespace
