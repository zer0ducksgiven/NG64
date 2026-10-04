// Update check against GitHub's latest release (update.c).
#ifndef NG64_UPDATE_H
#define NG64_UPDATE_H
#include <stddef.h>

enum { NG64_UPDATE_UNKNOWN = 0, NG64_UPDATE_CURRENT = 1, NG64_UPDATE_NEWER = 2 };

int ng64_version_newer(const char *tag, const char *current);   // "v0.1.5" vs "0.1.4": true if the tag is newer
void ng64_update_start(const char *exeDir);                    // a background check now and every 12 hours
// UNKNOWN until the first answer; NEWER fills in the new release's tag and the version running
int ng64_update_state(char *tag, size_t tagSize, char *current, size_t currentSize);

#endif
