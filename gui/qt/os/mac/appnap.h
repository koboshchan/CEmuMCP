#ifndef APPNAP_H
#define APPNAP_H

// Opts the whole process out of macOS App Nap / background throttling for
// its entire lifetime, so the emulator keeps running at full, chosen speed
// even while its window isn't focused or is fully occluded - e.g. while an
// MCP client is driving it without ever giving it real window focus, or a
// large file transfer is running in the background.
void disableAppNap();

#endif
