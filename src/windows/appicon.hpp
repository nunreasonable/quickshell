#pragma once

#include <qpixmap.h>
#include <qsize.h>
#include <qstring.h>

namespace qs::windows {

[[nodiscard]] QPixmap iconForKey(const QString& key, const QSize& size);

} // namespace qs::windows
