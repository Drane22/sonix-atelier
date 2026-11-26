#include "firmware.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"


// The card is scanned once and every name compared case-insensitively,
// rather than guessing at spellings: FAT is case-insensitive but exFAT and
// ext4 are not, and a file written by Windows can land as UPDATE.UPT while
// the stock player only ever looks for update.upt. One readdir covers them
// all -- and there is no cheaper way to be right about it.
static bool find_in_dir(const char *root, const char *stem, char *out, size_t out_size) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) != 0) {
			continue;
		}
		// Recovery must see the same file we hashed on case-sensitive cards.
		if (found) {
			found = false;
			break;
		}
		if (snprintf(out, out_size, "%s/%s", root, de->d_name) >= (int)out_size) break;

		struct stat st;
		if (lstat(out, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 17 * 2048) break;
		found = true;
	}

	closedir(dir);
	if (!found) {
		out[0] = '\0';
	}
	return found;
}

const char *firmware_update_stem(void) {
	// ONLY the name of THIS model -- deliberately NOT the stock binary's
	// "update.upt" fallback.
	//
	// The recovery kernel does not check what it is given: a .upt built for
	// another HiBy model, dropped on the card under the generic name, would be
	// written to the flash exactly the same way, and the device would not come
	// back. "update.upt" is a name anyone might use for anything; "r3proii.upt"
	// is a claim about which player the file is for.
	//
	// An unrecognised player therefore gets no name at all: a fallback would
	// hand an R1 the R3 Pro II's image. The config key is the way out for
	// anyone who knows what they are doing.
	const char *name = config_get("firmware", "name", "");
	if (name && *name) {
		return name;
	}
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		fprintf(stderr, "firmware: device-name in system-info.json names no player this build knows"
						" ('%s'); no update file will be looked for\n",
				sysinfo_device_name());
		return NULL;
	}
	return model->update_stem;
}

bool firmware_update_file_find(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *root = storage_sd_root();
	if (!root || !*root) {
		return false;
	}

	const char *name = firmware_update_stem();
	if (!name) {
		return false;
	}
	return find_in_dir(root, name, out, out_size);
}

// Only the GUI starts an update. The worker hashes on a separate thread so
// card I/O never stalls input or the audio completion timer.
static char verify_path[512];
static const char *verify_model;
static struct stat verified_file;
static _Atomic int verify_state;

static bool same_file(void) {
	struct stat st;
	return lstat(verify_path, &st) == 0 && S_ISREG(st.st_mode) &&
		st.st_dev == verified_file.st_dev && st.st_ino == verified_file.st_ino &&
		st.st_size == verified_file.st_size && st.st_mtime == verified_file.st_mtime &&
		st.st_ctime == verified_file.st_ctime
#ifdef __APPLE__
		&& st.st_mtimespec.tv_nsec == verified_file.st_mtimespec.tv_nsec
		&& st.st_ctimespec.tv_nsec == verified_file.st_ctimespec.tv_nsec;
#else
		&& st.st_mtim.tv_nsec == verified_file.st_mtim.tv_nsec && st.st_ctim.tv_nsec == verified_file.st_ctim.tv_nsec;
#endif
}

static void *verify_worker(void *unused) {
	(void)unused;
#ifdef HOST_BUILD
	const char *script = "../sonix-packer/verify-upt.sh";
	const char *mode = "--host";
#else
	const char *script = "/usr/bin/sonix_verify_upt.sh";
	const char *mode = "device";
#endif
	pid_t pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", script, verify_model, verify_path, mode, (char *)NULL);
		_exit(127);
	}
	int status = 0;
	pid_t done = -1;
	if (pid > 0) {
		do { done = waitpid(pid, &status, 0); } while (done < 0 && errno == EINTR);
	}
	verify_state = done == pid && pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && same_file()
		? FIRMWARE_VERIFIED : FIRMWARE_INVALID;
	return NULL;
}

bool firmware_verify_start(const char *path) {
	const sysinfo_model_t *model = sysinfo_model();
	if (verify_state == FIRMWARE_VERIFYING || !model || !path || strlen(path) >= sizeof(verify_path)) return false;
	verify_state = FIRMWARE_INVALID;
	if (lstat(path, &verified_file) != 0 || !S_ISREG(verified_file.st_mode)) return false;
	snprintf(verify_path, sizeof(verify_path), "%s", path);
	verify_model = model->update_stem;
	verify_state = FIRMWARE_VERIFYING;
	pthread_t thread;
	if (pthread_create(&thread, NULL, verify_worker, NULL) != 0) {
		verify_state = FIRMWARE_INVALID;
		return false;
	}
	pthread_detach(thread);
	return true;
}

firmware_verify_state_t firmware_verify_state(void) { return verify_state; }

bool firmware_update_start(void) {
	char path[sizeof(verify_path)];
	if (verify_state != FIRMWARE_VERIFIED || !firmware_update_file_find(path, sizeof(path)) ||
		strcmp(path, verify_path) != 0 || !same_file()) return false;
	verify_state = FIRMWARE_INVALID;
#ifdef HOST_BUILD
	printf("firmware: (host build) would arm recovery and reboot\n");
	return true;
#else
	printf("firmware: arming recovery\n");

	// The screen fades out the way the power key puts it out, first: what
	// follows holds this thread for seconds, and a frozen page reads as a
	// device that has hung. A dark one reads as a device that is restarting.
	power_screen_off();

	// Playback down first: the card is about to be handed to the recovery
	// kernel, and the decoder still has it open.
	audio_stop();

	// The streaming caches (tracks and covers) survive no power cycle, the
	// update's included.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// The clock goes into the RTC on the way out, like every other exit path
	// -- recovery reboots the device a second time when it is done, and the
	// time would otherwise be whatever the RTC last knew.
	clock_shutdown();

	// The recovery kernel reads the update off the card: it finds it clean.
	storage_release_for_shutdown();
	sync();
	sleep(1);

	// Two commands: erase the first block of /dev/mtd5, write "ota:kernel2"
	// into it. That is the whole of the stock firmware's bootmode.sh, and it
	// is what U-Boot reads to decide which kernel to start.
	int rc = system("/usr/bin/bootmode.sh Recovery");
	if (rc != 0) {
		printf("firmware: bootmode.sh Recovery failed (rc=%d)\n", rc);
	}

	sync();
	sleep(1);

	rc = system("reboot");
	(void)rc;

	// Spins here rather than returning, as the stock binary does: with recovery
	// already armed there is nothing sensible left to do, and going back to the
	// settings page would leave the device one power-cycle away from an update
	// the user is no longer expecting.
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;) {
		sleep(1);
	}
#endif
}
