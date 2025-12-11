// SPDX-License-Identifier: GPL-3.0-or-later

#include "njfm.h"
#include "journalfile.h"

// ----------------------------------------------------------------------------
// Path generation

void njfm_generate_path(struct rrdengine_instance *ctx, uint32_t min_fileno,
                        uint32_t max_fileno, char *path, size_t path_size) {
    snprintfz(path, path_size, "%s/" NJFM_PREFIX "%09u-%09u" NJFM_EXTENSION,
              ctx->config.dbfiles_path, min_fileno, max_fileno);
}

// ----------------------------------------------------------------------------
// File scanning

// Parse an njfm filename to extract min and max fileno
// Returns true on success, false on parse error
static bool njfm_parse_filename(const char *filename, uint32_t *min_fileno, uint32_t *max_fileno) {
    unsigned min_fn, max_fn;
    int ret = sscanf(filename, NJFM_PREFIX "%u-%u" NJFM_EXTENSION, &min_fn, &max_fn);
    if (ret != 2)
        return false;

    *min_fileno = min_fn;
    *max_fileno = max_fn;
    return true;
}

size_t njfm_scan_files(struct rrdengine_instance *ctx, NJFM_FILE **files_out) {
    uv_fs_t req;
    uv_dirent_t dent;
    size_t count = 0;
    size_t capacity = 16;
    NJFM_FILE *files = NULL;

    *files_out = NULL;

    int ret = uv_fs_scandir(NULL, &req, ctx->config.dbfiles_path, 0, NULL);
    if (ret < 0) {
        uv_fs_req_cleanup(&req);
        return 0;
    }

    files = mallocz(capacity * sizeof(NJFM_FILE));

    while (UV_EOF != uv_fs_scandir_next(&req, &dent)) {
        // Check if this looks like an njfm file
        if (strncmp(dent.name, NJFM_PREFIX, strlen(NJFM_PREFIX)) != 0)
            continue;

        // Check extension
        const char *ext = strrchr(dent.name, '.');
        if (!ext || strcmp(ext, NJFM_EXTENSION) != 0)
            continue;

        uint32_t min_fn, max_fn;
        if (!njfm_parse_filename(dent.name, &min_fn, &max_fn))
            continue;

        // Grow array if needed
        if (count >= capacity) {
            capacity *= 2;
            files = reallocz(files, capacity * sizeof(NJFM_FILE));
        }

        NJFM_FILE *f = &files[count];
        snprintfz(f->path, sizeof(f->path), "%s/%s", ctx->config.dbfiles_path, dent.name);
        f->min_fileno = min_fn;
        f->max_fileno = max_fn;
        f->file_size = 0;
        f->valid = false;  // Will be validated later
        count++;
    }

    uv_fs_req_cleanup(&req);

    if (count == 0) {
        freez(files);
        return 0;
    }

    *files_out = files;
    return count;
}

void njfm_free_files(NJFM_FILE *files, size_t count __maybe_unused) {
    freez(files);
}

// ----------------------------------------------------------------------------
// File validation

// Check if all datafiles in the range [min_fileno, max_fileno] exist
static bool njfm_datafiles_exist_in_range(struct rrdengine_instance *ctx,
                                          uint32_t min_fileno, uint32_t max_fileno) {
    netdata_rwlock_rdlock(&ctx->datafiles.rwlock);

    bool all_exist = true;
    for (uint32_t fileno = min_fileno; fileno <= max_fileno && all_exist; fileno++) {
        Pvoid_t *Pvalue = JudyLGet(ctx->datafiles.JudyL, (Word_t)fileno, PJE0);
        if (!Pvalue || !*Pvalue)
            all_exist = false;
    }

    netdata_rwlock_rdunlock(&ctx->datafiles.rwlock);
    return all_exist;
}

bool njfm_file_is_valid(struct rrdengine_instance *ctx, const char *path,
                        uint32_t *min_fileno_out, uint32_t *max_fileno_out) {
    int fd;
    struct stat statbuf;
    struct njfm_header header;
    bool valid = false;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    if (fstat(fd, &statbuf) != 0) {
        close(fd);
        return false;
    }

    size_t file_size = (size_t)statbuf.st_size;
    if (file_size < NJFM_HEADER_SIZE + sizeof(struct njfm_trailer)) {
        nd_log_daemon(NDLP_WARNING, "NJFM: file \"%s\" too small (%zu bytes)", path, file_size);
        close(fd);
        return false;
    }

    // Read header
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    if (bytes_read != sizeof(header)) {
        nd_log_daemon(NDLP_WARNING, "NJFM: failed to read header from \"%s\"", path);
        close(fd);
        return false;
    }

    // Check magic and version
    if (header.magic != NJFM_MAGIC) {
        nd_log_daemon(NDLP_WARNING, "NJFM: invalid magic in \"%s\" (0x%08X, expected 0x%08X)",
                      path, header.magic, NJFM_MAGIC);
        close(fd);
        return false;
    }

    if (header.version != NJFM_VERSION) {
        nd_log_daemon(NDLP_WARNING, "NJFM: unsupported version in \"%s\" (%u, expected %u)",
                      path, header.version, NJFM_VERSION);
        close(fd);
        return false;
    }

    // Verify header CRC
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *)&header, offsetof(struct njfm_header, header_crc));
    if (crc32cmp(&header.header_crc, crc)) {
        nd_log_daemon(NDLP_WARNING, "NJFM: header CRC mismatch in \"%s\"", path);
        close(fd);
        return false;
    }

    // Verify file size matches header
    if (header.file_size != file_size) {
        nd_log_daemon(NDLP_WARNING, "NJFM: file size mismatch in \"%s\" (header: %u, actual: %zu)",
                      path, header.file_size, file_size);
        close(fd);
        return false;
    }

    // Verify all datafiles in range exist
    if (!njfm_datafiles_exist_in_range(ctx, header.min_fileno, header.max_fileno)) {
        nd_log_daemon(NDLP_INFO, "NJFM: not all datafiles exist for \"%s\" (range %u-%u)",
                      path, header.min_fileno, header.max_fileno);
        close(fd);
        return false;
    }

    // All checks passed
    valid = true;
    if (min_fileno_out)
        *min_fileno_out = header.min_fileno;
    if (max_fileno_out)
        *max_fileno_out = header.max_fileno;

    close(fd);
    return valid;
}

// ----------------------------------------------------------------------------
// Load and populate MRG

ssize_t njfm_load_and_populate_mrg(struct rrdengine_instance *ctx, const char *path) {
    int fd;
    struct stat statbuf;
    uint8_t *data_start = NULL;
    ssize_t metrics_loaded = -1;

    usec_t started_ut = now_monotonic_usec();

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to open \"%s\": %s", path, strerror(errno));
        return -1;
    }

    if (fstat(fd, &statbuf) != 0) {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to stat \"%s\": %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    size_t file_size = (size_t)statbuf.st_size;

    // Memory map the file
    data_start = nd_mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data_start == MAP_FAILED) {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to mmap \"%s\": %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct njfm_header *header = (struct njfm_header *)data_start;

    // Validate header (quick checks)
    if (header->magic != NJFM_MAGIC || header->version != NJFM_VERSION) {
        nd_log_daemon(NDLP_ERR, "NJFM: invalid header in \"%s\"", path);
        goto cleanup;
    }

    // Verify file CRC (trailer)
    struct njfm_trailer *trailer = (struct njfm_trailer *)(data_start + file_size - sizeof(struct njfm_trailer));
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data_start, file_size - sizeof(struct njfm_trailer));
    if (crc32cmp(&trailer->crc, crc)) {
        nd_log_daemon(NDLP_ERR, "NJFM: file CRC mismatch in \"%s\"", path);
        goto cleanup;
    }

    // Account for disk space
    ctx_current_disk_space_increase(ctx, file_size);

    // Populate MRG from metric entries
    struct njfm_metric_entry *entries = (struct njfm_metric_entry *)(data_start + header->metric_offset);
    uint32_t metric_count = header->metric_count;
    time_t now_s = max_acceptable_collected_time();
    uint64_t journal_samples = 0;

    for (uint32_t i = 0; i < metric_count; i++) {
        struct njfm_metric_entry *entry = &entries[i];

        mrg_update_metric_retention_and_granularity_by_uuid(
            main_mrg,
            (Word_t)ctx,
            &entry->uuid,
            entry->first_time_s,
            entry->last_time_s,
            entry->update_every_s,
            now_s,
            &journal_samples);
    }

    __atomic_add_fetch(&ctx->atomic.samples, journal_samples, __ATOMIC_RELAXED);

    metrics_loaded = (ssize_t)metric_count;

    usec_t ended_ut = now_monotonic_usec();
    nd_log_daemon(NDLP_INFO, "NJFM: loaded \"%s\" - %u metrics, %.2f MiB, %.2f ms",
                  path, metric_count,
                  (double)file_size / 1024.0 / 1024.0,
                  (double)(ended_ut - started_ut) / USEC_PER_MS);

cleanup:
    if (data_start && data_start != MAP_FAILED)
        nd_munmap(data_start, file_size);
    close(fd);
    return metrics_loaded;
}

// ----------------------------------------------------------------------------
// Mark datafiles as populated

void njfm_mark_datafiles_populated(struct rrdengine_instance *ctx,
                                   uint32_t min_fileno, uint32_t max_fileno) {
    netdata_rwlock_rdlock(&ctx->datafiles.rwlock);

    for (uint32_t fileno = min_fileno; fileno <= max_fileno; fileno++) {
        Pvoid_t *Pvalue = JudyLGet(ctx->datafiles.JudyL, (Word_t)fileno, PJE0);
        if (Pvalue && *Pvalue) {
            struct rrdengine_datafile *datafile = *Pvalue;
            spinlock_lock(&datafile->populate_mrg.spinlock);
            datafile->populate_mrg.populated = true;
            spinlock_unlock(&datafile->populate_mrg.spinlock);
        }
    }

    netdata_rwlock_rdunlock(&ctx->datafiles.rwlock);
}

// ----------------------------------------------------------------------------
// Delete file

int njfm_delete_file(struct rrdengine_instance *ctx, const char *path) {
    struct stat statbuf;

    if (stat(path, &statbuf) != 0)
        return -1;

    size_t file_size = (size_t)statbuf.st_size;

    int ret = unlink(path);
    if (ret == 0) {
        // Decrease disk space accounting
        ctx_current_disk_space_decrease(ctx, file_size);
        nd_log_daemon(NDLP_INFO, "NJFM: deleted \"%s\"", path);
    } else {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to delete \"%s\": %s", path, strerror(errno));
    }

    return ret;
}

void njfm_delete_invalid_files(struct rrdengine_instance *ctx) {
    NJFM_FILE *files = NULL;
    size_t count = njfm_scan_files(ctx, &files);

    if (count == 0)
        return;

    for (size_t i = 0; i < count; i++) {
        uint32_t min_fn, max_fn;
        if (!njfm_file_is_valid(ctx, files[i].path, &min_fn, &max_fn)) {
            nd_log_daemon(NDLP_INFO, "NJFM: removing invalid file \"%s\"", files[i].path);
            njfm_delete_file(ctx, files[i].path);
        }
    }

    njfm_free_files(files, count);
}

// ----------------------------------------------------------------------------
// Build from journals

// Structure for collecting metrics during build
typedef struct njfm_build_metric {
    nd_uuid_t uuid;
    time_t first_time_s;
    time_t last_time_s;
    uint32_t update_every_s;
} NJFM_BUILD_METRIC;

// Comparison function for sorting metrics by UUID
static int njfm_metric_compare(const void *a, const void *b) {
    const NJFM_BUILD_METRIC *m1 = a;
    const NJFM_BUILD_METRIC *m2 = b;
    return memcmp(&m1->uuid, &m2->uuid, sizeof(nd_uuid_t));
}

int njfm_build_from_journals(struct rrdengine_instance *ctx,
                             uint32_t min_fileno, uint32_t max_fileno) {
    char path[RRDENG_PATH_MAX];
    char temp_path[RRDENG_PATH_MAX];
    Pvoid_t metrics_judy = NULL;  // JudyL: uuid_hash -> NJFM_BUILD_METRIC*
    int ret = -1;
    usec_t started_ut = now_monotonic_usec();
    time_t global_first_time_s = LONG_MAX;
    time_t global_last_time_s = 0;
    size_t total_metrics = 0;

    // Track actual min/max of datafiles that have jv2 data
    uint32_t actual_min_fileno = UINT32_MAX;
    uint32_t actual_max_fileno = 0;

    nd_log_daemon(NDLP_DEBUG, "NJFM: scanning datafiles %u-%u for jv2 data",
                  min_fileno, max_fileno);

    // Iterate through all datafiles in the range
    netdata_rwlock_rdlock(&ctx->datafiles.rwlock);

    for (uint32_t fileno = min_fileno; fileno <= max_fileno; fileno++) {
        Pvoid_t *Pvalue = JudyLGet(ctx->datafiles.JudyL, (Word_t)fileno, PJE0);
        if (!Pvalue || !*Pvalue)
            continue;

        struct rrdengine_datafile *datafile = *Pvalue;
        struct rrdengine_journalfile *journalfile = datafile->journalfile;

        if (!journalfile)
            continue;

        // Check if jv2 data is available for this journalfile
        if (!journalfile_v2_data_available(journalfile))
            continue;

        // Acquire journal v2 data
        size_t data_size = 0;
        struct journal_v2_header *j2_header = journalfile_v2_data_acquire(journalfile, &data_size, 0, 0);
        if (!j2_header)
            continue;

        // Track actual range of datafiles with jv2
        if (fileno < actual_min_fileno)
            actual_min_fileno = fileno;
        if (fileno > actual_max_fileno)
            actual_max_fileno = fileno;

        uint8_t *data_start = (uint8_t *)j2_header;
        time_t header_start_time_s = (time_t)(j2_header->start_time_ut / USEC_PER_SEC);

        // Process each metric in the journal
        struct journal_metric_list *metric = (struct journal_metric_list *)(data_start + j2_header->metric_offset);
        for (uint32_t i = 0; i < j2_header->metric_count; i++) {
            time_t start_time_s = header_start_time_s + metric->delta_start_s;
            time_t end_time_s = header_start_time_s + metric->delta_end_s;

            // Use first 8 bytes of UUID as hash key (good enough for JudyL)
            Word_t uuid_key;
            memcpy(&uuid_key, &metric->uuid, sizeof(uuid_key));

            Pvoid_t *PMetric = JudyLIns(&metrics_judy, uuid_key, PJE0);
            if (PMetric && PMetric != PJERR) {
                NJFM_BUILD_METRIC *build_metric = *PMetric;

                if (!build_metric) {
                    // New metric
                    build_metric = mallocz(sizeof(NJFM_BUILD_METRIC));
                    memcpy(&build_metric->uuid, &metric->uuid, sizeof(nd_uuid_t));
                    build_metric->first_time_s = start_time_s;
                    build_metric->last_time_s = end_time_s;
                    build_metric->update_every_s = metric->update_every_s;
                    *PMetric = build_metric;
                    total_metrics++;
                } else {
                    // Merge retention - check UUID matches (handle collisions)
                    if (memcmp(&build_metric->uuid, &metric->uuid, sizeof(nd_uuid_t)) == 0) {
                        // Same UUID - merge
                        if (start_time_s < build_metric->first_time_s)
                            build_metric->first_time_s = start_time_s;
                        if (end_time_s > build_metric->last_time_s)
                            build_metric->last_time_s = end_time_s;
                        if (metric->update_every_s && !build_metric->update_every_s)
                            build_metric->update_every_s = metric->update_every_s;
                    }
                    // If UUIDs don't match (hash collision), just skip - the first one wins
                    // This is acceptable as the file is just an optimization
                }
            }

            // Update global time range
            if (start_time_s < global_first_time_s)
                global_first_time_s = start_time_s;
            if (end_time_s > global_last_time_s)
                global_last_time_s = end_time_s;

            metric++;
        }

        journalfile_v2_data_release(journalfile);
    }

    netdata_rwlock_rdunlock(&ctx->datafiles.rwlock);

    // Check if we found any datafiles with jv2 data
    if (actual_min_fileno == UINT32_MAX) {
        nd_log_daemon(NDLP_INFO, "NJFM: no datafiles with jv2 data in range %u-%u",
                      min_fileno, max_fileno);
        ret = 0;
        goto cleanup;
    }

    if (total_metrics == 0) {
        nd_log_daemon(NDLP_INFO, "NJFM: no metrics found in datafiles %u-%u",
                      actual_min_fileno, actual_max_fileno);
        ret = 0;
        goto cleanup;
    }

    // Now generate the path using the ACTUAL range of datafiles with jv2 data
    njfm_generate_path(ctx, actual_min_fileno, actual_max_fileno, path, sizeof(path));
    snprintfz(temp_path, sizeof(temp_path), "%s.tmp", path);

    // Fix up times if not set
    if (global_first_time_s == LONG_MAX)
        global_first_time_s = 0;

    // Calculate file size
    size_t metrics_section_size = total_metrics * sizeof(struct njfm_metric_entry);
    size_t total_file_size = NJFM_HEADER_SIZE + metrics_section_size + sizeof(struct njfm_trailer);

    // Create temporary file with mmap
    int fd;
    uint8_t *data_start = nd_mmap_advanced(temp_path, total_file_size, MAP_SHARED, 0, false, true, &fd);
    if (!data_start) {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to create \"%s\"", temp_path);
        goto cleanup;
    }

    // Zero out header area
    memset(data_start, 0, NJFM_HEADER_SIZE);

    // Build header - use actual_min/max_fileno (only datafiles that had jv2 data)
    struct njfm_header header = {
        .magic = NJFM_MAGIC,
        .version = NJFM_VERSION,
        .min_fileno = actual_min_fileno,
        .max_fileno = actual_max_fileno,
        .metric_count = (uint32_t)total_metrics,
        .metric_offset = NJFM_HEADER_SIZE,
        .start_time_ut = global_first_time_s * USEC_PER_SEC,
        .end_time_ut = global_last_time_s * USEC_PER_SEC,
        .created_ut = now_realtime_usec(),
        .file_size = (uint32_t)total_file_size,
        .header_crc = 0  // Will be calculated below
    };

    // Collect all metrics into an array for sorting
    NJFM_BUILD_METRIC *metric_array = mallocz(total_metrics * sizeof(NJFM_BUILD_METRIC));
    size_t idx = 0;
    Word_t judy_idx = 0;
    Pvoid_t *PValue;
    bool first_then_next = true;

    while ((PValue = JudyLFirstThenNext(metrics_judy, &judy_idx, &first_then_next))) {
        NJFM_BUILD_METRIC *m = *PValue;
        if (m && idx < total_metrics) {
            metric_array[idx++] = *m;
        }
    }

    // Sort by UUID for consistency
    qsort(metric_array, total_metrics, sizeof(NJFM_BUILD_METRIC), njfm_metric_compare);

    // Write metric entries
    struct njfm_metric_entry *entries = (struct njfm_metric_entry *)(data_start + NJFM_HEADER_SIZE);
    for (size_t i = 0; i < total_metrics; i++) {
        memcpy(&entries[i].uuid, &metric_array[i].uuid, sizeof(nd_uuid_t));
        entries[i].first_time_s = metric_array[i].first_time_s;
        entries[i].last_time_s = metric_array[i].last_time_s;
        entries[i].update_every_s = metric_array[i].update_every_s;
        entries[i].reserved = 0;
    }

    freez(metric_array);

    // Calculate header CRC
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *)&header, offsetof(struct njfm_header, header_crc));
    header.header_crc = (uint32_t)crc;

    // Write header
    memcpy(data_start, &header, sizeof(header));

    // Calculate and write file CRC (trailer)
    struct njfm_trailer *trailer = (struct njfm_trailer *)(data_start + total_file_size - sizeof(struct njfm_trailer));
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data_start, total_file_size - sizeof(struct njfm_trailer));
    crc32set(&trailer->crc, crc);

    // Sync and close
    msync(data_start, total_file_size, MS_SYNC);
    nd_munmap(data_start, total_file_size);
    close(fd);

    // Rename temp file to final name
    if (rename(temp_path, path) != 0) {
        nd_log_daemon(NDLP_ERR, "NJFM: failed to rename \"%s\" to \"%s\": %s",
                      temp_path, path, strerror(errno));
        unlink(temp_path);
        goto cleanup;
    }

    // Account for disk space
    ctx_current_disk_space_increase(ctx, total_file_size);

    usec_t ended_ut = now_monotonic_usec();
    nd_log_daemon(NDLP_INFO, "NJFM: built \"%s\" - %zu metrics, %.2f MiB, %.2f ms",
                  path, total_metrics,
                  (double)total_file_size / 1024.0 / 1024.0,
                  (double)(ended_ut - started_ut) / USEC_PER_MS);

    ret = 0;

cleanup:
    // Free JudyL entries
    judy_idx = 0;
    first_then_next = true;
    while ((PValue = JudyLFirstThenNext(metrics_judy, &judy_idx, &first_then_next))) {
        freez(*PValue);
    }
    JudyLFreeArray(&metrics_judy, PJE0);

    return ret;
}

// ----------------------------------------------------------------------------
// Periodic/Incremental Building

// Helper: find the highest max_fileno covered by existing .njfm files
static uint32_t njfm_get_max_covered_fileno(struct rrdengine_instance *ctx) {
    NJFM_FILE *files = NULL;
    size_t count = njfm_scan_files(ctx, &files);

    uint32_t max_covered = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t min_fn, max_fn;
        if (njfm_file_is_valid(ctx, files[i].path, &min_fn, &max_fn)) {
            if (max_fn > max_covered)
                max_covered = max_fn;
        }
    }

    njfm_free_files(files, count);
    return max_covered;
}

// Helper: get the current max fileno (latest datafile)
static uint32_t njfm_get_current_max_fileno(struct rrdengine_instance *ctx) {
    netdata_rwlock_rdlock(&ctx->datafiles.rwlock);

    Word_t last_fileno = (Word_t)-1;
    Pvoid_t *PLast = JudyLLast(ctx->datafiles.JudyL, &last_fileno, PJE0);

    netdata_rwlock_rdunlock(&ctx->datafiles.rwlock);

    return PLast ? (uint32_t)last_fileno : 0;
}

// Helper: get the current min fileno (oldest datafile)
static uint32_t njfm_get_current_min_fileno(struct rrdengine_instance *ctx) {
    netdata_rwlock_rdlock(&ctx->datafiles.rwlock);

    Word_t first_fileno = 0;
    Pvoid_t *PFirst = JudyLFirst(ctx->datafiles.JudyL, &first_fileno, PJE0);

    netdata_rwlock_rdunlock(&ctx->datafiles.rwlock);

    return PFirst ? (uint32_t)first_fileno : 0;
}

bool njfm_should_build_chunk(struct rrdengine_instance *ctx) {
    if (!ctx || !ctx->datafiles.JudyL)
        return false;

    uint32_t max_covered = njfm_get_max_covered_fileno(ctx);
    uint32_t current_max = njfm_get_current_max_fileno(ctx);

    if (current_max == 0)
        return false;

    // Calculate how many datafiles are not covered by .njfm files
    uint32_t uncovered = (current_max > max_covered) ? (current_max - max_covered) : 0;

    // Build a new chunk when we have NJFM_CHUNK_SIZE uncovered files
    return uncovered >= NJFM_CHUNK_SIZE;
}

int njfm_build_next_chunk(struct rrdengine_instance *ctx) {
    if (!ctx || !ctx->datafiles.JudyL)
        return 0;

    uint32_t max_covered = njfm_get_max_covered_fileno(ctx);
    uint32_t current_min = njfm_get_current_min_fileno(ctx);
    uint32_t current_max = njfm_get_current_max_fileno(ctx);

    if (current_max == 0)
        return 0;

    // Determine the range for the next chunk
    // Start from the file after max_covered (or from current_min if nothing covered)
    uint32_t chunk_start = (max_covered > 0) ? (max_covered + 1) : current_min;

    // If chunk_start is before current_min (old files were deleted), adjust
    if (chunk_start < current_min)
        chunk_start = current_min;

    // Calculate how many files are uncovered
    if (chunk_start > current_max)
        return 0;  // Everything is covered

    uint32_t uncovered = current_max - chunk_start + 1;

    // Only build if we have at least NJFM_CHUNK_SIZE files to cover
    // Leave the last NJFM_CHUNK_SIZE files uncovered (they're still being written to)
    if (uncovered < NJFM_CHUNK_SIZE * 2)
        return 0;  // Not enough files yet

    // Build a chunk covering NJFM_CHUNK_SIZE files
    uint32_t chunk_end = chunk_start + NJFM_CHUNK_SIZE - 1;

    nd_log_daemon(NDLP_INFO, "NJFM: building chunk for tier %d, datafiles %u-%u",
                  ctx->config.tier, chunk_start, chunk_end);

    return njfm_build_from_journals(ctx, chunk_start, chunk_end);
}

void njfm_on_journal_v2_creation(struct rrdengine_instance *ctx) {
    // Check if we should build a new chunk
    if (njfm_should_build_chunk(ctx)) {
        // Build in a non-blocking way - just trigger the build
        // The actual work is done by the dbengine event loop
        njfm_build_next_chunk(ctx);
    }
}

// ----------------------------------------------------------------------------
// Build for tier (shutdown)

void njfm_build_for_tier(struct rrdengine_instance *ctx, bool skip_if_slow) {
    if (!ctx || !ctx->datafiles.JudyL)
        return;

    uint32_t current_min = njfm_get_current_min_fileno(ctx);
    uint32_t current_max = njfm_get_current_max_fileno(ctx);

    if (current_max == 0 || current_min > current_max) {
        nd_log_daemon(NDLP_DEBUG, "NJFM: no datafiles to build from for tier %d", ctx->config.tier);
        return;
    }

    // Count total datafiles
    size_t datafile_count = (size_t)(current_max - current_min + 1);

    // First, delete invalid files (might free up some coverage)
    njfm_delete_invalid_files(ctx);

    // Get max covered after cleanup
    uint32_t max_covered = njfm_get_max_covered_fileno(ctx);

    // Calculate uncovered files
    uint32_t chunk_start = (max_covered > 0 && max_covered >= current_min) ? (max_covered + 1) : current_min;
    if (chunk_start < current_min)
        chunk_start = current_min;

    if (chunk_start > current_max) {
        nd_log_daemon(NDLP_DEBUG, "NJFM: all datafiles already covered for tier %d", ctx->config.tier);
        return;
    }

    uint32_t uncovered = current_max - chunk_start + 1;

    // If skip_if_slow and we have many uncovered files, only build chunks
    if (skip_if_slow && uncovered > NJFM_CHUNK_SIZE * 2) {
        nd_log_daemon(NDLP_INFO, "NJFM: building single chunk for tier %d (skip_if_slow, %u uncovered)",
                      ctx->config.tier, uncovered);
        // Build just one chunk
        uint32_t chunk_end = chunk_start + NJFM_CHUNK_SIZE - 1;
        if (chunk_end > current_max)
            chunk_end = current_max;
        njfm_build_from_journals(ctx, chunk_start, chunk_end);
        return;
    }

    // Build chunks to cover all uncovered files
    nd_log_daemon(NDLP_INFO, "NJFM: building chunks for tier %d, datafiles %u-%u (%u uncovered)",
                  ctx->config.tier, chunk_start, current_max, uncovered);

    while (chunk_start <= current_max) {
        uint32_t chunk_end = chunk_start + NJFM_CHUNK_SIZE - 1;
        if (chunk_end > current_max)
            chunk_end = current_max;

        njfm_build_from_journals(ctx, chunk_start, chunk_end);

        chunk_start = chunk_end + 1;
    }
}
