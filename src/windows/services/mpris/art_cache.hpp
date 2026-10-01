#pragma once

#include <qstring.h>

#include <winrt/Windows.Storage.Streams.h>

namespace qs::windows::services::mpris {

///! Reads a GSMTC thumbnail and caches it to disk as a PNG, worker thread only.
/// `trackKey` identifies the track (title+artist+album); the same key always maps to the same
/// cache file, so a track's art is only ever decoded and written once. Returns a `file:///` URL
/// for the cached image, or an empty string if there's no thumbnail or reading/decoding it
/// failed. Also prunes the cache directory down to ~50 files after a successful write.
QString cacheThumbnail(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
    const QString& trackKey
);

} // namespace qs::windows::services::mpris
