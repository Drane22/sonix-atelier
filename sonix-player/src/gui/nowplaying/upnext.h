#ifndef UPNEXT_H
#define UPNEXT_H

#include <stdbool.h>
#include <stddef.h>

// Authoritative, non-mutating metadata resolver for adjacent carousel tracks
// (Previous / Next) and the subtle Up Next queue bearing on Now Playing.
// Uses indexed SQLite lookups first and falls back to a low-priority worker
// thread so the GUI thread never blocks on SD card tag reads.

void upnext_init(void (*on_updated)(void));

// Re-evaluates device_state_peek_adjacent(-1 / +1) and device_state_peek_up_next()
// against the active playback queue and schedules async tag reads if needed.
void upnext_refresh(void);
void upnext_poll(void);

// Fills resolved path, title, and artist for dir = -1 (previous) or +1 (next).
// Returns false when no deterministic adjacent track exists in that direction.
bool upnext_get_adjacent(int dir, char *path_out, size_t path_sz, char *title_out, size_t title_sz,
						 char *artist_out, size_t artist_sz);

// Fills resolved title and artist for the upcoming auto-advance track (for the
// compact unobtrusive Up Next label). Returns false when unavailable or when
// streaming/podcast metadata has not arrived yet.
bool upnext_get_auto(char *title_out, size_t title_sz, char *artist_out, size_t artist_sz, bool *is_repeat_one);

#endif /* UPNEXT_H */
