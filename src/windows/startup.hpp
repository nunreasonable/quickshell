#pragma once

#include <functional>

class QObject;
class QQuickWindow;

namespace qs::windows::startup {

void afterFirstFrame(QObject* context, std::function<void()> callback);
void watchWindow(QQuickWindow* window);
[[nodiscard]] bool settled();

} // namespace qs::windows::startup
