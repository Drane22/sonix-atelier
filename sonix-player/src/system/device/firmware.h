#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>

// The SD and online routes both verify the local archive before entering recovery.
typedef enum { FIRMWARE_INVALID, FIRMWARE_VERIFYING, FIRMWARE_VERIFIED } firmware_verify_state_t;
bool firmware_verify_start(const char *path);
firmware_verify_state_t firmware_verify_state(void);

// The name of this model's update file without ".upt": [firmware] name in
// the config when set, otherwise the model table's update_stem ("r3proii",
// "r1"). NULL when system-info.json names no model this build knows.
const char *firmware_update_stem(void);

// Looks for the update file on the card. Returns true and fills `out` with its
// full path when one is there. The name must be the device's own --
// "r3proii.upt" -- in any mixture of upper and lower case; nothing else is
// accepted, "update.upt" included.
bool firmware_update_file_find(char *out, size_t out_size);

// A changed or unverified file returns false before any shutdown action.
//
// Stops playback and flushes the filesystems first: the card is about to be
// read by the recovery kernel, and an unflushed write on it is how a good
// .upt file turns into a failed update.
//
// A no-op that only logs on the host build -- writing to /dev/mtd5 on the
// developer's own machine is not a mistake worth being able to make.
bool firmware_update_start(void);

#endif /* FIRMWARE_H */
