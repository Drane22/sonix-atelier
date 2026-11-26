#include "upnext.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/gui.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"

typedef struct {
	bool valid;
	bool resolved;
	bool has_text;
	bool is_repeat_one;
	bool is_streaming;
	bool is_podcast;
	char path[512];
	char title[256];
	char artist[256];
	bool needs_read;
} slot_meta_t;

// 0 = prev (-1), 1 = next (+1), 2 = auto up-next
static slot_meta_t slots[3];
static void (*updated_cb)(void);

typedef struct {
	char path[512];
	bool is_streaming;
	bool is_podcast;
	song_metadata_t meta;
} meta_job_t;

// The GUI selects work from the three current slots. The worker only sees
// the selected path. Skipping again replaces pending work at no I/O cost.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_wake = PTHREAD_COND_INITIALIZER;
static meta_job_t job;
static bool job_pending, job_done;
static bool worker_started, job_busy; // GUI thread only

static void schedule_read(void);

static void basename_stem(const char *path, char *out, size_t size) {
	if (!size) {
		return;
	}
	const char *slash = strrchr(path, '/');
	snprintf(out, size, "%.*s", (int)(size - 1), slash ? slash + 1 : path);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

void upnext_poll(void) {
	if (!job_busy) return;
	pthread_mutex_lock(&job_lock);
	bool done = job_done;
	job_done = false;
	pthread_mutex_unlock(&job_lock);
	if (!done) return;
	char resolved_title[256] = "";
	char resolved_artist[256] = "";
	bool have_title = false;

	if (job.meta.title[0]) {
		snprintf(resolved_title, sizeof(resolved_title), "%s", job.meta.title);
		have_title = true;
	} else if (!job.is_streaming && !job.is_podcast) {
		basename_stem(job.path, resolved_title, sizeof(resolved_title));
		have_title = resolved_title[0] != '\0';
	}

	const char *artist = metadata_shown_artist(&job.meta);
	if (job.is_podcast && (!artist || !artist[0])) {
		artist = job.meta.album;
	}
	if (have_title && artist && artist[0]) {
		snprintf(resolved_artist, sizeof(resolved_artist), "%s", artist);
	}

	bool any_updated = false;
	for (int i = 0; i < 3; i++) {
		slot_meta_t *s = &slots[i];
		if (!s->valid || strcmp(s->path, job.path) != 0) {
			continue;
		}
		s->needs_read = false;
		if (have_title) {
			snprintf(s->title, sizeof(s->title), "%s", resolved_title);
			snprintf(s->artist, sizeof(s->artist), "%s", resolved_artist);
			s->has_text = true;
			s->resolved = true;
		} else {
			s->title[0] = '\0';
			s->artist[0] = '\0';
			s->has_text = false;
			s->resolved = false;
		}
		any_updated = true;
	}
	job_busy = false;
	schedule_read();

	if (any_updated && updated_cb) {
		updated_cb();
	}
}

static void meta_arrived(void *unused) {
	(void)unused;
	upnext_poll();
}

static void *meta_worker(void *unused) {
	(void)unused;
	thread_be_low_priority("upnextmeta");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!job_pending) pthread_cond_wait(&job_wake, &job_lock);
		job_pending = false;
		pthread_mutex_unlock(&job_lock);

		metadata_read_for_preview(job.path, &job.meta);
		pthread_mutex_lock(&job_lock);
		job_done = true;
		pthread_mutex_unlock(&job_lock);
		gui_post(meta_arrived, NULL); // The player's poll also collects it if the GUI queue is full.
	}
	return NULL;
}

static void schedule_read(void) {
	if (job_busy) return;
	for (int i = 2; i >= 0; i--) {
		if (!slots[i].needs_read) continue;
		if (!worker_started) {
			pthread_t thread;
			if (pthread_create(&thread, NULL, meta_worker, NULL) != 0) return;
			pthread_detach(thread);
			worker_started = true;
		}
		job_busy = true;
		pthread_mutex_lock(&job_lock);
		snprintf(job.path, sizeof(job.path), "%s", slots[i].path);
		job.is_streaming = slots[i].is_streaming;
		job.is_podcast = slots[i].is_podcast;
		job_pending = true;
		pthread_cond_signal(&job_wake);
		pthread_mutex_unlock(&job_lock);
		break;
	}
}

static bool resolve_slot(int idx, const device_up_next_t *info) {
	slot_meta_t *s = &slots[idx];
	if (!info || !info->valid || !info->path[0]) {
		bool had = s->valid;
		memset(s, 0, sizeof(*s));
		return had;
	}

	if (s->valid && strcmp(s->path, info->path) == 0 && s->is_repeat_one == info->is_repeat_one) {
		return false;
	}

	memset(s, 0, sizeof(*s));
	s->valid = true;
	s->is_repeat_one = info->is_repeat_one;
	s->is_streaming = info->is_streaming;
	s->is_podcast = info->is_podcast;
	snprintf(s->path, sizeof(s->path), "%s", info->path);

	// Fast path 1: same file as currently loaded track (e.g. Repeat One or duplicate queue entry).
	const char *cur_file = NULL;
	const song_metadata_t *cur_meta = device_state_loaded_metadata(&cur_file);
	if (cur_file && strcmp(cur_file, info->path) == 0 && cur_meta) {
		if (cur_meta->title[0]) {
			snprintf(s->title, sizeof(s->title), "%s", cur_meta->title);
		} else if (!info->is_streaming && !info->is_podcast) {
			basename_stem(info->path, s->title, sizeof(s->title));
		}
		const char *artist = metadata_shown_artist(cur_meta);
		if (info->is_podcast && (!artist || !artist[0])) {
			artist = cur_meta->album;
		}
		snprintf(s->artist, sizeof(s->artist), "%s", artist ? artist : "");
		s->has_text = s->title[0] != '\0';
		s->resolved = s->has_text;
		return true;
	}

	// Fast path 2: reuse already-authoritatively-resolved sibling slot with identical path.
	for (int other = 0; other < 3; other++) {
		if (other != idx && slots[other].valid && slots[other].resolved &&
			strcmp(slots[other].path, info->path) == 0) {
			memcpy(s->title, slots[other].title, sizeof(s->title));
			memcpy(s->artist, slots[other].artist, sizeof(s->artist));
			s->has_text = true;
			s->resolved = true;
			return true;
		}
	}

	// Fast path 3: indexed local library SQLite lookup.
	if (!info->is_streaming && !info->is_podcast && library_is_open()) {
		if (library_track_names(info->path, s->title, sizeof(s->title), s->artist, sizeof(s->artist)) &&
			s->title[0]) {
			s->has_text = true;
			s->resolved = true;
			return true;
		}
	}

	// Fallback: temporary stem for local files while worker reads full tags.
	if (!info->is_streaming && !info->is_podcast) {
		basename_stem(info->path, s->title, sizeof(s->title));
		s->has_text = s->title[0] != '\0';
	}

	s->needs_read = true;
	return true;
}

void upnext_init(void (*on_updated)(void)) {
	updated_cb = on_updated;
}

void upnext_refresh(void) {
	device_up_next_t prev_info, next_info, auto_info;
	device_state_peek_adjacent(-1, &prev_info);
	device_state_peek_adjacent(1, &next_info);
	device_state_peek_up_next(&auto_info);

	bool c0 = resolve_slot(0, &prev_info);
	bool c1 = resolve_slot(1, &next_info);
	bool c2 = resolve_slot(2, &auto_info);
	schedule_read();

	if ((c0 || c1 || c2) && updated_cb) {
		updated_cb();
	}
}

bool upnext_get_adjacent(int dir, char *path_out, size_t path_sz, char *title_out, size_t title_sz,
						 char *artist_out, size_t artist_sz) {
	int idx = (dir < 0) ? 0 : 1;
	const slot_meta_t *s = &slots[idx];
	if (path_out && path_sz) path_out[0] = '\0';
	if (title_out && title_sz) title_out[0] = '\0';
	if (artist_out && artist_sz) artist_out[0] = '\0';

	if (!s->valid || !s->path[0]) {
		return false;
	}
	if (path_out && path_sz) {
		snprintf(path_out, path_sz, "%s", s->path);
	}
	if (s->has_text) {
		if (title_out && title_sz) snprintf(title_out, title_sz, "%s", s->title);
		if (artist_out && artist_sz) snprintf(artist_out, artist_sz, "%s", s->artist);
	}
	return true;
}

bool upnext_get_auto(char *title_out, size_t title_sz, char *artist_out, size_t artist_sz, bool *is_repeat_one) {
	const slot_meta_t *s = &slots[2];
	if (title_out && title_sz) title_out[0] = '\0';
	if (artist_out && artist_sz) artist_out[0] = '\0';
	if (is_repeat_one) *is_repeat_one = false;

	if (!s->valid || !s->has_text || !s->title[0]) {
		return false;
	}
	if (title_out && title_sz) snprintf(title_out, title_sz, "%s", s->title);
	if (artist_out && artist_sz) snprintf(artist_out, artist_sz, "%s", s->artist);
	if (is_repeat_one) *is_repeat_one = s->is_repeat_one;
	return true;
}
