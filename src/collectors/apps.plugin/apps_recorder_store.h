// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_APPS_RECORDER_STORE_H
#define NETDATA_APPS_RECORDER_STORE_H

#include "apps_recorder_format.h"

// The on-disk store of the flight recorder: one file per sealed chunk, named by the chunk's first sample time.
// Overlapping saved windows share chunks, so they merge by construction. Files are written to a temporary name and
// renamed; readers validate every chunk (length and checksum), so a torn write is dropped, never decoded.
// When the store exceeds its cap, the oldest chunks are deleted.

#define APPS_REC_STORE_SUFFIX ".ndpr"

typedef struct {
    uint64_t first_ut;
    uint64_t last_ut;
    uint64_t size;
} APPS_REC_STORE_ITEM;

typedef struct {
    char dir[FILENAME_MAX + 1];
    uint64_t cap_bytes;
    uint64_t bytes;

    APPS_REC_STORE_ITEM *items;     // sorted by first_ut
    size_t used;
    size_t size;
} APPS_REC_STORE;

// creates the directory when missing, indexes the chunks found, deletes leftovers and anything over the cap
bool apps_rec_store_open(APPS_REC_STORE *st, const char *dir, uint64_t cap_bytes);
void apps_rec_store_close(APPS_REC_STORE *st);

bool apps_rec_store_has(const APPS_REC_STORE *st, uint64_t first_ut);

// writes a sealed chunk, then evicts the oldest chunks over the cap
bool apps_rec_store_write(APPS_REC_STORE *st, const APPS_REC_CHUNK_HDR *hdr, size_t size);

// reads and validates a chunk; an invalid chunk is deleted. Free the result with freez().
APPS_REC_CHUNK_HDR *apps_rec_store_read(APPS_REC_STORE *st, uint64_t first_ut, size_t *size);

#endif //NETDATA_APPS_RECORDER_STORE_H
