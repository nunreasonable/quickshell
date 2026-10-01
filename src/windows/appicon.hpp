#pragma once

#include <qpixmap.h>
#include <qsize.h>
#include <qstring.h>

namespace qs::windows {

// Resolves a Windows-specific icon key for IconImageProvider: either "appicon:<id>" (an id
// from DesktopEntries, see WindowsDesktopEntryBackend) or an absolute Windows path
// ("C:/Program Files/.../app.exe", forward or backward slashes) to an exe or other
// shell-iconable file. Returns a null QPixmap (never IconImageProvider::missingPixmap) when
// the key isn't a Windows icon key or the icon can't be loaded, so the caller falls through to
// the normal theme lookup and then the missing-icon placeholder, same as any other
// unresolvable icon name.
[[nodiscard]] QPixmap iconForKey(const QString& key, const QSize& size);

} // namespace qs::windows
