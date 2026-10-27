// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_STORE_H
#define NETDATA_APPS_RECORDER_STORE_H

#include "apps_recorder_format.h"

// The on-disk store of the flight recorder: one file per sealed chunk, named by the chunk's first sample time.
// Overlapping saved windows share chunks, so they merge by construction. Files are written to a temporary name and
// renamed; readers validate every chunk (length and checksum), so a torn write is dropped, never decoded.
// When the store exceeds its cap, the oldest chunks are deleted. When the filesystem is full, the store deletes its
// own oldest chunks to make room and tries once more, once per disk-full episode; it never deletes anything else, nor
// a chunk the recorder still holds in RAM.

#define APPS_REC_STORE_SUFFIX ".ndpr"

typedef struct {
    uint64_t first_ut;
    uint64_t last_ut;
    uint64_t size;
} APPS_REC_STORE_ITEM;

typedef enum {
    APPS_REC_STORE_OK = 0,
    APPS_REC_STORE_DISK_FULL,       // ENOSPC or EDQUOT, also after making room
    APPS_REC_STORE_ERROR,           // any other failure
} APPS_REC_STORE_RESULT;

const char *apps_rec_store_result_str(APPS_REC_STORE_RESULT r);

typedef struct {
    char dir[FILENAME_MAX + 1];
    uint64_t cap_bytes;
    uint64_t bytes;

    APPS_REC_STORE_ITEM *items;     // sorted by first_ut
    size_t used;
    size_t size;

    // the last write failed for a full filesystem; room is made once per episode, so retries during it delete
    // nothing more (a process still filling the disk would take whatever we free)
    bool disk_full;

    // the write() the store uses; tests replace it to simulate a full filesystem
    ssize_t (*write_fn)(int fd, const void *buf, size_t count);
} APPS_REC_STORE;

// creates the directory when missing, indexes the chunks found, deletes leftovers and anything over the cap
bool apps_rec_store_open(APPS_REC_STORE *st, const char *dir, uint64_t cap_bytes);
void apps_rec_store_close(APPS_REC_STORE *st);

bool apps_rec_store_has(const APPS_REC_STORE *st, uint64_t first_ut);

// writes a sealed chunk, then evicts the oldest chunks over the cap. On a full filesystem, room is made only from
// chunks starting before keep_from_ut: the caller still holds the newer ones in RAM and would only write them again.
APPS_REC_STORE_RESULT apps_rec_store_write(APPS_REC_STORE *st, const APPS_REC_CHUNK_HDR *hdr, size_t size,
                                           uint64_t keep_from_ut);

// reads and validates a chunk; an invalid chunk is deleted. Free the result with freez().
APPS_REC_CHUNK_HDR *apps_rec_store_read(APPS_REC_STORE *st, uint64_t first_ut, size_t *size);

#endif //NETDATA_APPS_RECORDER_STORE_H
