#pragma once
#include <QStringList>

namespace glassboot {

// Raw disk access needs root (Linux) or administrator (Windows).
bool isElevated();

// Start a new elevated copy of this program with `args` (pkexec on Linux, UAC on Windows).
// Returns true if the elevated copy was launched; the caller should then exit.
bool relaunchElevated(const QStringList& args);

} // namespace glassboot
