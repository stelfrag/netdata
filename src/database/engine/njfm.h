// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_NJFM_H
#define NETDATA_NJFM_H

#include "rrdengine.h"

/*
 * NJFM - Netdata Journal File MRG
 *
 * A file format that pre-computes the union of metric retention data from
 * multiple journal v2 files to accelerate MRG population during startup.
 *
 * File Layout:
 * +--------------------------------------------------+
 * | HEADER SECTION (4096 bytes)                      |
 * |   - njfm_header (56 bytes)                       |
 * |   - Padding to 4096 bytes                        |
 * +--------------------------------------------------+
 * | METRIC ENTRIES SECTION                           |
 * |   - Array of njfm_metric_entry (40 bytes each)   |
 * +--------------------------------------------------+
 * | FILE TRAILER                                     |
 * |   - CRC32 of entire file (4 bytes)               |
 * +--------------------------------------------------+
 *
 * File naming: njfm-<min_fileno>-<max_fileno>.njfm
 * The tier is determined by the directory (dbengine/ or dbengine-tierN/)
 */

#define NJFM_PREFIX "njfm-"
#define NJFM_EXTENSION ".njfm"

#define NJFM_MAGIC           (0x4E4A464D)  // "NJFM" in little-endian
#define NJFM_VERSION         (1)

// Reserve the first 4 KiB for the header
#define NJFM_HEADER_SIZE     (RRDENG_BLOCK_SIZE)

// Chunk size: number of datafiles per .njfm file
// This controls:
// 1. How often new .njfm files are built (every NJFM_CHUNK_SIZE new jv2 files)
// 2. The maximum number of individual jv2 files that need to be replayed on startup
//    (those not yet covered by an .njfm file)
#define NJFM_CHUNK_SIZE      (20)

// Header structure (56 bytes, padded to 4096)
struct njfm_header {
    uint32_t magic;              // NJFM_MAGIC
    uint32_t version;            // NJFM_VERSION

    uint32_t min_fileno;         // First datafile covered (inclusive)
    uint32_t max_fileno;         // Last datafile covered (inclusive)

    uint32_t metric_count;       // Number of metric entries
    uint32_t metric_offset;      // Offset to metric section (always NJFM_HEADER_SIZE)

    usec_t   start_time_ut;      // Earliest timestamp in covered files (microseconds)
    usec_t   end_time_ut;        // Latest timestamp in covered files (microseconds)

    usec_t   created_ut;         // When this file was created (microseconds)

    uint32_t file_size;          // Total file size including trailer
    uint32_t header_crc;         // CRC32 of header (bytes 0 to offsetof(header_crc))
};

// Metric entry structure (40 bytes)
struct njfm_metric_entry {
    nd_uuid_t uuid;              // 16 bytes - Metric UUID
    time_t    first_time_s;      // 8 bytes - Earliest data point
    time_t    last_time_s;       // 8 bytes - Latest data point
    uint32_t  update_every_s;    // 4 bytes - Collection frequency
    uint32_t  reserved;          // 4 bytes - Reserved for future use
};

// File trailer structure
struct njfm_trailer {
    union {
        uint8_t checksum[CHECKSUM_SZ];
        uint32_t crc;
    };
};

// In-memory representation of a discovered .njfm file
typedef struct njfm_file {
    char path[RRDENG_PATH_MAX];
    uint32_t min_fileno;
    uint32_t max_fileno;
    uint32_t file_size;
    bool valid;
} NJFM_FILE;

// ----------------------------------------------------------------------------
// API Functions

// Scan the dbfiles_path directory for .njfm files
// Returns the number of files found, populates *files_out (caller must free)
size_t njfm_scan_files(struct rrdengine_instance *ctx, NJFM_FILE **files_out);

// Validate an .njfm file
// Checks: magic, version, CRC, and that all datafiles in range exist
bool njfm_file_is_valid(struct rrdengine_instance *ctx, const char *path,
                        uint32_t *min_fileno_out, uint32_t *max_fileno_out);

// Load an .njfm file and populate MRG with its contents
// Returns the number of metrics processed, or -1 on error
// Also accounts for disk space via ctx_current_disk_space_increase()
ssize_t njfm_load_and_populate_mrg(struct rrdengine_instance *ctx, const char *path);

// Mark datafiles in the given range as already populated (skip jv2 loading)
void njfm_mark_datafiles_populated(struct rrdengine_instance *ctx,
                                   uint32_t min_fileno, uint32_t max_fileno);

// Build an .njfm file from journal v2 files in the specified range
// Returns 0 on success, -1 on error
// Also accounts for disk space via ctx_current_disk_space_increase()
int njfm_build_from_journals(struct rrdengine_instance *ctx,
                             uint32_t min_fileno, uint32_t max_fileno);

// Delete an .njfm file and account for disk space
// Returns 0 on success, -1 on error
int njfm_delete_file(struct rrdengine_instance *ctx, const char *path);

// Delete all invalid .njfm files in the dbfiles_path
void njfm_delete_invalid_files(struct rrdengine_instance *ctx);

// Build .njfm file(s) for the current tier if beneficial
// Called during shutdown or periodically
// skip_if_slow: if true, skip building if it would take too long
void njfm_build_for_tier(struct rrdengine_instance *ctx, bool skip_if_slow);

// Generate the path for an .njfm file
void njfm_generate_path(struct rrdengine_instance *ctx, uint32_t min_fileno,
                        uint32_t max_fileno, char *path, size_t path_size);

// Free an array of NJFM_FILE structures
void njfm_free_files(NJFM_FILE *files, size_t count);

// ----------------------------------------------------------------------------
// Periodic/Incremental Building API

// Check if we should build a new .njfm chunk
// Called after a new jv2 file is created
// Returns true if a new chunk should be built
bool njfm_should_build_chunk(struct rrdengine_instance *ctx);

// Build the next .njfm chunk if needed
// Called periodically or after jv2 creation
// Returns 0 on success (or if nothing to build), -1 on error
int njfm_build_next_chunk(struct rrdengine_instance *ctx);

// Called when a new jv2 file is finalized to trigger periodic building
void njfm_on_journal_v2_creation(struct rrdengine_instance *ctx);

#endif /* NETDATA_NJFM_H */
