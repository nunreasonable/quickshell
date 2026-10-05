#pragma once

#include <qstring.h>

#include <winrt/Windows.Storage.Streams.h>

namespace qs::windows::services::mpris {

QString cacheThumbnail(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
    const QString& trackKey
);

} // namespace qs::windows::services::mpris
