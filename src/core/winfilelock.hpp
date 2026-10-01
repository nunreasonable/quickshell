#pragma once

// Advisory file locks for Windows, used where POSIX builds use fcntl() locks.
//
// LockFileEx locks are mandatory: a byte range locked by one handle cannot be read through any
// other handle. Instead of locking the whole file, a single byte far beyond any real content is
// locked, so the data region stays readable by everyone while the lock state can still be probed
// with a non-blocking try-lock (the equivalent of F_GETLK) or waited on (F_SETLKW).

#include <qfile.h>

namespace qs::core::winlock {

// Takes the exclusive marker lock on an open file. The lock is held until the file (handle) is
// closed or the process exits. Returns false if another process holds it or on error.
bool lockExclusive(QFile& file);

// Returns true if some other process currently holds the exclusive marker lock on the file.
bool isLocked(QFile& file);

// Blocks until no process holds the exclusive marker lock on the file. Returns false on error.
bool waitUnlocked(QFile& file);

} // namespace qs::core::winlock
