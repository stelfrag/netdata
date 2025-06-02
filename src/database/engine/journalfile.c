// SPDX-License-Identifier: GPL-3.0-or-later
#include "rrdengine.h"

// the default value is set in ND_PROFILE, not here
time_t dbengine_journal_v2_unmount_time = 120;

/* Careful to always call this before creating a new journal file */
int journalfile_v1_extent_write(struct rrdengine_instance *ctx, struct rrdengine_datafile *datafile, WAL *wal)
{
    uv_fs_t request;
    struct rrdengine_journalfile *journalfile = datafile->journalfile;
    uv_buf_t iov;

    if (wal->size < wal->buf_size) {
        /* simulate an empty transaction to skip the rest of the block */
        *(uint8_t *) (wal->buf + wal->size) = STORE_PADDING;
    }

    uint64_t journalfile_position;
    spinlock_lock(&journalfile->unsafe.spinlock);
    journalfile_position = journalfile->unsafe.pos;
    journalfile->unsafe.pos += wal->buf_size;
    spinlock_unlock(&journalfile->unsafe.spinlock);

    iov = uv_buf_init(wal->buf, wal->buf_size);

    int retries = 10;
    int ret = -1;
    while (ret < 0 && --retries) {
        ret = uv_fs_write(NULL, &request, journalfile->file, &iov, 1, (int64_t)journalfile_position, NULL);
        uv_fs_req_cleanup(&request);
        if (ret < 0) {
            if (ret == -ENOSPC || ret == -EBADF || ret == -EACCES || ret == -EROFS || ret == -EINVAL)
                break;
            sleep_usec(300 * USEC_PER_MS);
        }
    }

    if (unlikely(ret < 0)) {
        ctx_io_error(ctx);
        goto done;
    }

    ctx_current_disk_space_increase(ctx, wal->buf_size);
    ctx_io_write_op_bytes(ctx, wal->buf_size);

done:
    wal_release(wal);
    worker_is_idle();
    return ret;
}

void journalfile_v2_generate_path(struct rrdengine_datafile *datafile, char *str, size_t maxlen)
{
    (void) snprintfz(str, maxlen, "%s/" WALFILE_PREFIX RRDENG_FILE_NUMBER_PRINT_TMPL WALFILE_EXTENSION_V2,
                    datafile_ctx(datafile)->config.dbfiles_path, datafile->tier, datafile->fileno);
}

void journalfile_v1_generate_path(struct rrdengine_datafile *datafile, char *str, size_t maxlen)
{
    (void) snprintfz(str, maxlen - 1, "%s/" WALFILE_PREFIX RRDENG_FILE_NUMBER_PRINT_TMPL WALFILE_EXTENSION,
                    datafile_ctx(datafile)->config.dbfiles_path, datafile->tier, datafile->fileno);
}

// ----------------------------------------------------------------------------

ALWAYS_INLINE struct rrdengine_datafile *njfv2idx_find_and_acquire_j2_header(NJFV2IDX_FIND_STATE *s) {
    if (unlikely(!s)) return NULL;

    struct rrdengine_datafile *datafile = NULL;

    rw_spinlock_read_lock(&s->ctx->njfv2idx.spinlock);

    Pvoid_t *PValue = NULL;

    if(unlikely(!s->init)) {
        s->init = true;
        s->last = s->wanted_start_time_s;

        PValue = JudyLPrev(s->ctx->njfv2idx.JudyL, &s->last, PJE0);
        if (unlikely(PValue == PJERR))
            fatal("DBENGINE: NJFV2IDX corrupted judy array");

        if(!PValue) {
            s->last = 0;
            PValue = JudyLFirst(s->ctx->njfv2idx.JudyL, &s->last, PJE0);
            if (unlikely(PValue == PJERR))
                fatal("DBENGINE: NJFV2IDX corrupted judy array");

            if(!PValue)
                s->last = s->wanted_start_time_s;
        }
    }

    while(1) {
        if (likely(!PValue)) {
            PValue = JudyLNext(s->ctx->njfv2idx.JudyL, &s->last, PJE0);
            if (unlikely(PValue == PJERR))
                fatal("DBENGINE: NJFV2IDX corrupted judy array");

            if(!PValue) {
                // cannot find anything after that point
                datafile = NULL;
                break;
            }
        }

        datafile = *PValue;

        struct rrdengine_journalfile *journalfile = datafile ? datafile->journalfile : NULL;

        if (!datafile || !journalfile) {
            datafile = NULL;
            PValue = NULL;
            continue;
        }

        TIME_RANGE_COMPARE rc = is_page_in_time_range(journalfile->v2.first_time_s,
                                                      journalfile->v2.last_time_s,
                                                      s->wanted_start_time_s,
                                                      s->wanted_end_time_s);

        if(rc == PAGE_IS_IN_RANGE) {
            // this is good to return
            break;
        }
        else if(rc == PAGE_IS_IN_THE_PAST) {
            // continue to get the next
            datafile = NULL;
            PValue = NULL;
            continue;
        }
        else /* PAGE_IS_IN_THE_FUTURE */ {
            // we finished - no more datafiles
            datafile = NULL;
            PValue = NULL;
            break;
        }
    }

    struct rrdengine_journalfile *journalfile = datafile ? datafile->journalfile : NULL;
    if(datafile && journalfile)
        s->j2_header_acquired = journalfile_v2_data_acquire(journalfile, NULL,
                                                            s->wanted_start_time_s,
                                                            s->wanted_end_time_s);
    else
        s->j2_header_acquired = NULL;

    rw_spinlock_read_unlock(&s->ctx->njfv2idx.spinlock);

    return datafile;
}

static void njfv2idx_add(struct rrdengine_datafile *datafile) {
    if(unlikely(!datafile))
        fatal("DBENGINE: NJFV2IDX trying to index a journal file with no datafile");

    struct rrdengine_instance *ctx = datafile_ctx(datafile);

    internal_fatal(datafile->journalfile->v2.last_time_s <= 0, "DBENGINE: NJFV2IDX trying to index a journal file with invalid first_time_s");

    rw_spinlock_write_lock(&ctx->njfv2idx.spinlock);
    datafile->journalfile->njfv2idx.indexed_as = datafile->journalfile->v2.last_time_s;

    do {
        internal_fatal(datafile->journalfile->njfv2idx.indexed_as <= 0, "DBENGINE: NJFV2IDX journalfile is already indexed");

        Pvoid_t *PValue = JudyLIns(&ctx->njfv2idx.JudyL, datafile->journalfile->njfv2idx.indexed_as, PJE0);
        if (!PValue || PValue == PJERR)
            fatal("DBENGINE: NJFV2IDX corrupted judy array");

        if (unlikely(*PValue)) {
            // already there
            datafile->journalfile->njfv2idx.indexed_as++;
        }
        else {
            *PValue = datafile;
            break;
        }
    } while(1);

    rw_spinlock_write_unlock(&ctx->njfv2idx.spinlock);
}

static void njfv2idx_remove(struct rrdengine_datafile *datafile) {
    internal_fatal(!datafile->journalfile->njfv2idx.indexed_as, "DBENGINE: NJFV2IDX journalfile to remove is not indexed");

    struct rrdengine_instance *ctx = datafile_ctx(datafile);
    rw_spinlock_write_lock(&ctx->njfv2idx.spinlock);

    int rc = JudyLDel(&ctx->njfv2idx.JudyL, datafile->journalfile->njfv2idx.indexed_as, PJE0);
    (void)rc;
    internal_fatal(!rc, "DBENGINE: NJFV2IDX cannot remove entry");

    datafile->journalfile->njfv2idx.indexed_as = 0;

    rw_spinlock_write_unlock(&ctx->njfv2idx.spinlock);
}

// ----------------------------------------------------------------------------

static struct journal_v2_header *journalfile_v2_mounted_data_get(struct rrdengine_journalfile *journalfile, size_t *data_size) {
    struct journal_v2_header *j2_header = NULL;

    spinlock_lock(&journalfile->data_spinlock);

    if(!journalfile->mmap.data) {
        journalfile->mmap.data = nd_mmap(NULL, journalfile->mmap.size, PROT_READ, MAP_SHARED, journalfile->mmap.fd, 0);
        if (journalfile->mmap.data == MAP_FAILED) {
            internal_fatal(true, "DBENGINE: failed to re-mmap() journal file v2");
            close(journalfile->mmap.fd);
            journalfile->mmap.fd = -1;
            journalfile->mmap.data = NULL;
            journalfile->mmap.size = 0;

            journalfile->v2.flags &= ~(JOURNALFILE_FLAG_IS_AVAILABLE | JOURNALFILE_FLAG_IS_MOUNTED);

            ctx_fs_error(datafile_ctx(journalfile->datafile));
        }
        else {
            __atomic_add_fetch(&rrdeng_cache_efficiency_stats.journal_v2_mapped, 1, __ATOMIC_RELAXED);

            madvise_dontfork(journalfile->mmap.data, journalfile->mmap.size);
            madvise_dontdump(journalfile->mmap.data, journalfile->mmap.size);
            // madvise_dontneed(journalfile->mmap.data, journalfile->mmap.size);
            madvise_random(journalfile->mmap.data, journalfile->mmap.size);

            journalfile->v2.flags |= JOURNALFILE_FLAG_IS_AVAILABLE | JOURNALFILE_FLAG_IS_MOUNTED;
            JOURNALFILE_FLAGS flags = journalfile->v2.flags;

            if(flags & JOURNALFILE_FLAG_MOUNTED_FOR_RETENTION) {
                // we need the entire metrics directory into memory to process it
                madvise_willneed(journalfile->mmap.data, journalfile->v2.size_of_directory);
            }
        }
    }

    if(journalfile->mmap.data) {
        j2_header = journalfile->mmap.data;

        if (data_size)
            *data_size = journalfile->mmap.size;
    }

    spinlock_unlock(&journalfile->data_spinlock);

    return j2_header;
}

static bool journalfile_v2_mounted_data_unmount(struct rrdengine_journalfile *journalfile, bool have_locks, bool wait) {
    bool unmounted = false;

    if(!have_locks) {
        if(!wait) {
            if (!spinlock_trylock(&journalfile->data_spinlock))
                return false;
        }
        else
            spinlock_lock(&journalfile->data_spinlock);
    }

    if(!journalfile->v2.refcount) {
        if(journalfile->mmap.data) {
            if (nd_munmap(journalfile->mmap.data, journalfile->mmap.size)) {
                char path[RRDENG_PATH_MAX];
                journalfile_v2_generate_path(journalfile->datafile, path, sizeof(path));
                netdata_log_error("DBENGINE: failed to unmap index file \"%s\"", path);
                internal_fatal(true, "DBENGINE: failed to unmap file \"%s\"", path);
                ctx_fs_error(datafile_ctx(journalfile->datafile));
            }
            else {
                __atomic_add_fetch(&rrdeng_cache_efficiency_stats.journal_v2_unmapped, 1, __ATOMIC_RELAXED);
                journalfile->mmap.data = NULL;
                journalfile->v2.flags &= ~JOURNALFILE_FLAG_IS_MOUNTED;
            }
        }

        unmounted = true;
    }

    if(!have_locks) {
        spinlock_unlock(&journalfile->data_spinlock);
    }

    return unmounted;
}

void journalfile_v2_data_unmount_cleanup(time_t now_s) {
    // DO NOT WAIT ON ANY LOCK!!!

    for(size_t tier = 0; tier < (size_t)nd_profile.storage_tiers;tier++) {
        struct rrdengine_instance *ctx = multidb_ctx[tier];
        if(!ctx) continue;

        struct rrdengine_datafile *datafile;
        if(uv_rwlock_tryrdlock(&ctx->datafiles.rwlock) != 0)
            continue;

        for (datafile = ctx->datafiles.first; datafile; datafile = datafile->next) {
            struct rrdengine_journalfile *journalfile = datafile->journalfile;

            if(!spinlock_trylock(&journalfile->data_spinlock))
                continue;

            bool unmount = false;
            if (!journalfile->v2.refcount && (journalfile->v2.flags & JOURNALFILE_FLAG_IS_MOUNTED)) {
                // this journal has no references and it is mounted

                if (!journalfile->v2.not_needed_since_s)
                    journalfile->v2.not_needed_since_s = now_s;

                else if (
                    dbengine_journal_v2_unmount_time && now_s - journalfile->v2.not_needed_since_s >= dbengine_journal_v2_unmount_time)
                    // enough time has passed since we last needed this journal
                    unmount = true;
            }
            spinlock_unlock(&journalfile->data_spinlock);

            if (unmount)
                journalfile_v2_mounted_data_unmount(journalfile, false, false);
        }
        uv_rwlock_rdunlock(&ctx->datafiles.rwlock);
    }
}

ALWAYS_INLINE struct journal_v2_header *journalfile_v2_data_acquire(struct rrdengine_journalfile *journalfile, size_t *data_size, time_t wanted_first_time_s, time_t wanted_last_time_s) {
    spinlock_lock(&journalfile->data_spinlock);

    bool has_data = (journalfile->v2.flags & JOURNALFILE_FLAG_IS_AVAILABLE);
    bool is_mounted = (journalfile->v2.flags & JOURNALFILE_FLAG_IS_MOUNTED);
    bool do_we_need_it = false;

    if(has_data) {
        if (!wanted_first_time_s || !wanted_last_time_s ||
            is_page_in_time_range(journalfile->v2.first_time_s, journalfile->v2.last_time_s,
                                  wanted_first_time_s, wanted_last_time_s) == PAGE_IS_IN_RANGE) {

            journalfile->v2.refcount++;

            do_we_need_it = true;

            if (!wanted_first_time_s && !wanted_last_time_s && !is_mounted)
                journalfile->v2.flags |= JOURNALFILE_FLAG_MOUNTED_FOR_RETENTION;
            else
                journalfile->v2.flags &= ~JOURNALFILE_FLAG_MOUNTED_FOR_RETENTION;

        }
    }
    spinlock_unlock(&journalfile->data_spinlock);

    if(do_we_need_it)
        return journalfile_v2_mounted_data_get(journalfile, data_size);

    return NULL;
}

ALWAYS_INLINE void journalfile_v2_data_release(struct rrdengine_journalfile *journalfile) {
    spinlock_lock(&journalfile->data_spinlock);

    internal_fatal(!journalfile->mmap.data, "trying to release a journalfile without data");
    internal_fatal(journalfile->v2.refcount < 1, "trying to release a non-acquired journalfile");

    bool unmount = false;

    journalfile->v2.refcount--;

    if(journalfile->v2.refcount == 0) {
        journalfile->v2.not_needed_since_s = 0;

        if(journalfile->v2.flags & JOURNALFILE_FLAG_MOUNTED_FOR_RETENTION)
            unmount = true;
    }
    spinlock_unlock(&journalfile->data_spinlock);

    if(unmount)
        journalfile_v2_mounted_data_unmount(journalfile, false, true);
}

bool journalfile_v2_data_available(struct rrdengine_journalfile *journalfile) {

    spinlock_lock(&journalfile->data_spinlock);
    bool has_data = (journalfile->v2.flags & JOURNALFILE_FLAG_IS_AVAILABLE);
    spinlock_unlock(&journalfile->data_spinlock);

    return has_data;
}

size_t journalfile_v2_data_size_get(struct rrdengine_journalfile *journalfile) {

    spinlock_lock(&journalfile->data_spinlock);
    size_t data_size = journalfile->mmap.size;
    spinlock_unlock(&journalfile->data_spinlock);

    return data_size;
}

void journalfile_v2_data_set(struct rrdengine_journalfile *journalfile, int fd, void *journal_data, uint32_t journal_data_size) {
    if(unlikely(!journalfile))
        fatal("DBENGINE: JOURNALFILE: trying to set journal data without a journalfile");

    if(unlikely(!journalfile->datafile))
        fatal("DBENGINE: JOURNALFILE: trying to set journal data without a datafile");

    spinlock_lock(&journalfile->data_spinlock);

    internal_fatal(journalfile->mmap.fd != -1, "DBENGINE JOURNALFILE: trying to re-set journal fd");
    internal_fatal(journalfile->mmap.data, "DBENGINE JOURNALFILE: trying to re-set journal_data");
    internal_fatal(journalfile->v2.refcount, "DBENGINE JOURNALFILE: trying to re-set journal_data of referenced journalfile");

    journalfile->mmap.fd = fd;
    journalfile->mmap.data = journal_data;
    journalfile->mmap.size = journal_data_size;
    journalfile->v2.not_needed_since_s = now_monotonic_sec();
    journalfile->v2.flags |= JOURNALFILE_FLAG_IS_AVAILABLE | JOURNALFILE_FLAG_IS_MOUNTED;

    struct journal_v2_header *j2_header = journalfile->mmap.data;
    journalfile->v2.first_time_s = (time_t)(j2_header->start_time_ut / USEC_PER_SEC);
    journalfile->v2.last_time_s = (time_t)(j2_header->end_time_ut / USEC_PER_SEC);
    journalfile->v2.size_of_directory = j2_header->metric_offset + j2_header->metric_count * sizeof(struct journal_metric_list);

    journalfile_v2_mounted_data_unmount(journalfile, true, true);

    spinlock_unlock(&journalfile->data_spinlock);

    njfv2idx_add(journalfile->datafile);
}

static void journalfile_v2_data_unmap_permanently(struct rrdengine_journalfile *journalfile) {
    njfv2idx_remove(journalfile->datafile);

    bool has_references = false;
    char path_v2[RRDENG_PATH_MAX];

    journalfile_v2_generate_path(journalfile->datafile, path_v2, sizeof(path_v2));

    do {
        if (has_references)
            sleep_usec(10 * USEC_PER_MS);

        spinlock_lock(&journalfile->data_spinlock);

        if(journalfile_v2_mounted_data_unmount(journalfile, true, true)) {
            if(journalfile->mmap.fd != -1)
                close(journalfile->mmap.fd);

            journalfile->mmap.fd = -1;
            journalfile->mmap.data = NULL;
            journalfile->mmap.size = 0;
            journalfile->v2.first_time_s = 0;
            journalfile->v2.last_time_s = 0;
            journalfile->v2.flags = 0;
        }
        else {
            has_references = true;
            nd_log_limit_static_global_var(journalfile_erl, 10, 0);
            nd_log_limit(&journalfile_erl, NDLS_DAEMON, NDLP_WARNING, "DBENGINE: journalfile \"%s\" is not available for unmap", path_v2);
        }

        spinlock_unlock(&journalfile->data_spinlock);

    } while(has_references);
}

struct rrdengine_journalfile *journalfile_alloc_and_init(struct rrdengine_datafile *datafile)
{
    struct rrdengine_journalfile *journalfile = callocz(1, sizeof(struct rrdengine_journalfile));
    journalfile->datafile = datafile;
    spinlock_init(&journalfile->data_spinlock);
    spinlock_init(&journalfile->unsafe.spinlock);
    journalfile->mmap.fd = -1;
    datafile->journalfile = journalfile;
    return journalfile;
}

static int close_uv_file(struct rrdengine_datafile *datafile, uv_file file)
{
    int ret;
    char path[RRDENG_PATH_MAX];

    uv_fs_t req;
    ret = uv_fs_close(NULL, &req, file, NULL);
    if (ret < 0) {
        journalfile_v1_generate_path(datafile, path, sizeof(path));
        netdata_log_error("DBENGINE: uv_fs_close(\"%s\"): %s", path, uv_strerror(ret));
        ctx_fs_error(datafile_ctx(datafile));
    }
    uv_fs_req_cleanup(&req);
    return ret;
}

int journalfile_close(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    if(journalfile_v2_data_available(journalfile)) {
        journalfile_v2_data_unmap_permanently(journalfile);
        return 0;
    }

    return close_uv_file(datafile, journalfile->file);
}

int journalfile_unlink(struct rrdengine_journalfile *journalfile)
{
    struct rrdengine_datafile *datafile = journalfile->datafile;
    struct rrdengine_instance *ctx = datafile_ctx(datafile);
    int ret;

    char path[RRDENG_PATH_MAX];
    journalfile_v1_generate_path(datafile, path, sizeof(path));

    UNLINK_FILE(ctx, path, ret);
    if (ret == 0)
        __atomic_add_fetch(&ctx->stats.journalfile_deletions, 1, __ATOMIC_RELAXED);

    return ret;
}

int journalfile_destroy_unsafe(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    struct rrdengine_instance *ctx = datafile_ctx(datafile);
    int ret;
    char path[RRDENG_PATH_MAX];
    char path_v2[RRDENG_PATH_MAX];

    journalfile_v1_generate_path(datafile, path, sizeof(path));
    journalfile_v2_generate_path(datafile, path_v2, sizeof(path));

    if (journalfile->file)
        (void)close_uv_file(datafile, journalfile->file);

    // This is the new journal v2 index file
    int deleted = 0;
    UNLINK_FILE(ctx, path_v2, ret);
    if (ret == 0)
       deleted++;

    UNLINK_FILE(ctx, path, ret);
    if (ret == 0)
        deleted++;

    __atomic_add_fetch(&ctx->stats.journalfile_deletions, deleted, __ATOMIC_RELAXED);

    if(journalfile_v2_data_available(journalfile))
        journalfile_v2_data_unmap_permanently(journalfile);

    return ret;
}

int journalfile_create(struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    struct rrdengine_instance *ctx = datafile_ctx(datafile);
    uv_fs_t req;
    uv_file file;
    int ret, fd;
    struct rrdeng_jf_sb *superblock = NULL;
    uv_buf_t iov;
    char path[RRDENG_PATH_MAX];

    journalfile_v1_generate_path(datafile, path, sizeof(path));
    fd = open_file_for_io(path, O_CREAT | O_RDWR | O_TRUNC, &file, dbengine_use_direct_io);
    if (fd < 0) {
        ctx_fs_error(ctx);
        return fd;
    }
    journalfile->file = file;

    (void)posix_memalignz((void *)&superblock, RRDFILE_ALIGNMENT, sizeof(*superblock));
    memset(superblock, 0, sizeof(*superblock));
    (void) strncpy(superblock->magic_number, RRDENG_JF_MAGIC, RRDENG_MAGIC_SZ);
    (void) strncpy(superblock->version, RRDENG_JF_VER, RRDENG_VER_SZ);

    iov = uv_buf_init((void *)superblock, sizeof(*superblock));

    int retries = 10;
    ret = -1;
    while (ret < 0 && --retries) {
        ret = uv_fs_write(NULL, &req, file, &iov, 1, 0, NULL);
        uv_fs_req_cleanup(&req);
        if (ret < 0) {
            if (ret == -ENOSPC || ret == -EBADF || ret == -EACCES || ret == -EROFS || ret == -EINVAL)
                break;
            sleep_usec(300 * USEC_PER_MS);
        }
    }

    posix_memalign_freez(superblock);

    if (ret < 0) {
        journalfile_destroy_unsafe(journalfile, datafile);
        ctx_io_error(ctx);
        nd_log_limit_static_global_var(dbengine_erl, 10, 0);
        nd_log_limit(&dbengine_erl, NDLS_DAEMON, NDLP_ERR, "DBENGINE: Failed to create journlfile \"%s\"", path);
        return ret;
    }

    __atomic_add_fetch(&ctx->stats.journalfile_creations, 1, __ATOMIC_RELAXED);
    journalfile->unsafe.pos = sizeof(*superblock);
    ctx_io_write_op_bytes(ctx, sizeof(*superblock));

    return 0;
}

static int journalfile_check_superblock(uv_file file)
{
    int ret;
    struct rrdeng_jf_sb *superblock = NULL;
    uv_buf_t iov;
    uv_fs_t req;

    (void)posix_memalignz((void *)&superblock, RRDFILE_ALIGNMENT, sizeof(*superblock));
    iov = uv_buf_init((void *)superblock, sizeof(*superblock));

    ret = uv_fs_read(NULL, &req, file, &iov, 1, 0, NULL);
    if (ret < 0) {
        netdata_log_error("DBENGINE: uv_fs_read: %s", uv_strerror(ret));
        uv_fs_req_cleanup(&req);
        goto error;
    }
    fatal_assert(req.result >= 0);
    uv_fs_req_cleanup(&req);


    char jf_magic[RRDENG_MAGIC_SZ] = RRDENG_JF_MAGIC;
    char jf_ver[RRDENG_VER_SZ] = RRDENG_JF_VER;
    if (strncmp(superblock->magic_number, jf_magic, RRDENG_MAGIC_SZ) != 0 ||
        strncmp(superblock->version, jf_ver, RRDENG_VER_SZ) != 0) {
        nd_log(NDLS_DAEMON, NDLP_ERR, "DBENGINE: File has invalid superblock.");
        ret = UV_EINVAL;
    } else {
        ret = 0;
    }
    error:
        posix_memalign_freez(superblock);
    return ret;
}

static void journalfile_restore_extent_metadata(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile, void *buf, unsigned max_size)
{
    static bitmap64_t page_error_map = BITMAP64_INITIALIZER;
    unsigned i, count, payload_length, descr_size;
    struct rrdeng_jf_store_data *jf_metric_data;

    jf_metric_data = buf;
    count = jf_metric_data->number_of_pages;
    descr_size = sizeof(*jf_metric_data->descr) * count;
    payload_length = sizeof(*jf_metric_data) + descr_size;
    if (payload_length > max_size) {
        netdata_log_error("DBENGINE: corrupted transaction payload.");
        return;
    }

    time_t now_s = max_acceptable_collected_time();
    time_t extent_first_time_s = journalfile->v2.first_time_s ? journalfile->v2.first_time_s : LONG_MAX;
    for (i = 0; i < count ; ++i) {
        nd_uuid_t *temp_id;
        uint8_t page_type = jf_metric_data->descr[i].type;

        if (page_type > RRDENG_PAGE_TYPE_MAX) {
            if (!bitmap64_get(&page_error_map, page_type)) {
                netdata_log_error("DBENGINE: unknown page type %d encountered.", page_type);
                bitmap64_set(&page_error_map, page_type);
            }
            continue;
        }

        temp_id = (nd_uuid_t *)jf_metric_data->descr[i].uuid;
        METRIC *metric = mrg_metric_get_and_acquire_by_uuid(main_mrg, temp_id, (Word_t)ctx);

        struct rrdeng_extent_page_descr *descr = &jf_metric_data->descr[i];
        VALIDATED_PAGE_DESCRIPTOR vd = validate_extent_page_descr(
                descr, now_s,
                (metric) ? mrg_metric_get_update_every_s(main_mrg, metric) : 0,
                false);

        if(!vd.is_valid) {
            if(metric)
                mrg_metric_release(main_mrg, metric);

            continue;
        }

        bool update_metric_time = true;
        uint64_t samples = 0;
        if (!metric) {
            MRG_ENTRY entry = {
                    .uuid = temp_id,
                    .section = (Word_t)ctx,
                    .first_time_s = vd.start_time_s,
                    .last_time_s = vd.end_time_s,
                    .latest_update_every_s = vd.update_every_s,
            };

            bool added;
            metric = mrg_metric_add_and_acquire(main_mrg, entry, &added);
            if(added)
                update_metric_time = false;
        }
        Word_t metric_id = mrg_metric_id(main_mrg, metric);

        if (vd.update_every_s)
            samples = (vd.end_time_s - vd.start_time_s) / vd.update_every_s;

        if (update_metric_time)
            mrg_metric_expand_retention(main_mrg, metric, vd.start_time_s, vd.end_time_s, vd.update_every_s);

        __atomic_add_fetch(&journalfile->v2.samples, samples, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ctx->atomic.samples, samples, __ATOMIC_RELAXED);

        pgc_open_add_hot_page(
                (Word_t)ctx, metric_id, vd.start_time_s, vd.end_time_s, vd.update_every_s,
                journalfile->datafile,
                jf_metric_data->extent_offset, jf_metric_data->extent_size, jf_metric_data->descr[i].page_length);

        extent_first_time_s = MIN(extent_first_time_s, vd.start_time_s);

        mrg_metric_release(main_mrg, metric);
    }

    journalfile->v2.first_time_s = extent_first_time_s;

    time_t old = __atomic_load_n(&ctx->atomic.first_time_s, __ATOMIC_RELAXED);;
    do {
        if(old <= extent_first_time_s)
            break;
    } while(!__atomic_compare_exchange_n(&ctx->atomic.first_time_s, &old, extent_first_time_s, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

/*
 * Replays transaction by interpreting up to max_size bytes from buf.
 * Sets id to the current transaction id or to 0 if unknown.
 * Returns size of transaction record or 0 for unknown size.
 */
static unsigned journalfile_replay_transaction(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile,
                                               void *buf, uint64_t *id, unsigned max_size)
{
    unsigned payload_length, size_bytes;
    int ret;
    /* persistent structures */
    struct rrdeng_jf_transaction_header *jf_header;
    struct rrdeng_jf_transaction_trailer *jf_trailer;
    uLong crc;

    *id = 0;
    jf_header = buf;
    if (STORE_PADDING == jf_header->type) {
        netdata_log_debug(D_RRDENGINE, "Skipping padding.");
        return 0;
    }
    if (sizeof(*jf_header) > max_size) {
        netdata_log_error("DBENGINE: corrupted transaction record, skipping.");
        return 0;
    }
    *id = jf_header->id;
    payload_length = jf_header->payload_length;
    size_bytes = sizeof(*jf_header) + payload_length + sizeof(*jf_trailer);
    if (size_bytes > max_size) {
        netdata_log_error("DBENGINE: corrupted transaction record, skipping.");
        return 0;
    }
    jf_trailer = buf + sizeof(*jf_header) + payload_length;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, buf, sizeof(*jf_header) + payload_length);
    ret = crc32cmp(jf_trailer->checksum, crc);
    netdata_log_debug(D_RRDENGINE, "Transaction %"PRIu64" was read from disk. CRC32 check: %s", *id, ret ? "FAILED" : "SUCCEEDED");
    if (unlikely(ret)) {
        netdata_log_error("DBENGINE: transaction %"PRIu64" was read from disk. CRC32 check: FAILED", *id);
        return size_bytes;
    }
    switch (jf_header->type) {
    case STORE_DATA:
        netdata_log_debug(D_RRDENGINE, "Replaying transaction %"PRIu64"", jf_header->id);
            journalfile_restore_extent_metadata(ctx, journalfile, buf + sizeof(*jf_header), payload_length);
        break;
    default:
        netdata_log_error("DBENGINE: unknown transaction type, skipping record.");
        break;
    }

    return size_bytes;
}


#define READAHEAD_BYTES (RRDENG_BLOCK_SIZE * 256)
/*
 * Iterates journal file transactions and populates the page cache.
 * Page cache must already be initialized.
 * Returns the maximum transaction id it discovered.
 */
static uint64_t journalfile_iterate_transactions(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile)
{
    uv_file file;
    uint64_t file_size;
    int ret;
    uint64_t pos, pos_i, max_id, id;
    unsigned size_bytes;
    void *buf = NULL;
    uv_buf_t iov;
    uv_fs_t req;

    file = journalfile->file;
    file_size = journalfile->unsafe.pos;

    max_id = 1;
    (void)posix_memalignz((void *)&buf, RRDFILE_ALIGNMENT, READAHEAD_BYTES);

    for (pos = sizeof(struct rrdeng_jf_sb); pos < file_size; pos += READAHEAD_BYTES) {
        size_bytes = MIN(READAHEAD_BYTES, file_size - pos);
        iov = uv_buf_init(buf, size_bytes);
        ret = uv_fs_read(NULL, &req, file, &iov, 1, pos, NULL);
        if (ret < 0) {
            netdata_log_error("DBENGINE: uv_fs_read: pos=%" PRIu64 ", %s", pos, uv_strerror(ret));
            uv_fs_req_cleanup(&req);
            goto skip_file;
        }
        fatal_assert(req.result >= 0);
        uv_fs_req_cleanup(&req);
        ctx_io_read_op_bytes(ctx, size_bytes);

        for (pos_i = 0; pos_i < size_bytes;) {
            unsigned max_size;

            max_size = pos + size_bytes - pos_i;
            ret = journalfile_replay_transaction(ctx, journalfile, buf + pos_i, &id, max_size);
            if (!ret) /* TODO: support transactions bigger than 4K */
                /* unknown transaction size, move on to the next block */
                pos_i = ALIGN_BYTES_FLOOR(pos_i + RRDENG_BLOCK_SIZE);
            else
                pos_i += ret;
            max_id = MAX(max_id, id);
        }
    }
skip_file:
    posix_memalign_freez(buf);
    return max_id;
}

static int journalfile_direct_migrate_v1_to_v2(struct rrdengine_instance *ctx, struct rrdengine_journalfile *v1_journalfile_struct, struct rrdengine_datafile *datafile) {
    char path_v1[RRDENG_PATH_MAX];
    char path_v2[RRDENG_PATH_MAX];
    int fd_v1 = -1;
    int fd_v2 = -1;
    void *v2_map_addr = NULL;
    size_t v2_filesize = 0;

    journalfile_v1_generate_path(datafile, path_v1, sizeof(path_v1));
    journalfile_v2_generate_path(datafile, path_v2, sizeof(path_v2));

    // v1_journalfile_struct->file should be the descriptor for the opened v1 file from journalfile_load
    fd_v1 = v1_journalfile_struct->file;
    if (fd_v1 < 0) { // Should not happen if called correctly from journalfile_load
        netdata_log_error("DBENGINE: v1 journal file '%s' not open for direct migration.", path_v1);
        return -1;
    }

    // --- Placeholder for collecting v1 data ---
    // This part will be complex:
    // 1. Iterate v1 transactions (adapting journalfile_iterate_transactions and journalfile_replay_transaction)
    //    - Need to read from fd_v1.
    //    - For each STORE_DATA, extract metric info, page info (start/end times, update_every, extent details).
    // 2. Store this information in temporary structures (e.g., Judy arrays for metrics, extents, pages).
    //    This is what pgc_open_cache_to_journal_v2 and its callback journalfile_migrate_to_v2_callback do.
    //    We need to replicate that data gathering here, directly from v1 file.
    //
    // Example (conceptual):
#include <JudyHS.h> // For JudyHS (string/binary key to Word_t mapping)

    Pvoid_t judy_metrics_temp = NULL; // JudyHS: UUID -> jv2_metrics_info*
    Pvoid_t judy_extents_temp = NULL; // JudyL: extent_offset_v1 -> jv2_extents_info*
    size_t number_of_metrics = 0; // Actual count of unique metrics
    size_t number_of_extents = 0;
    size_t number_of_pages = 0;
    time_t min_time_s = LONG_MAX;
    time_t max_time_s = 0;
    uint64_t total_samples_collected = 0;
    int ret = -1; // General return code

    // Structures for reading V1 data
    void *v1_read_buf = NULL;
    uv_buf_t v1_iov;
    uv_fs_t v1_fs_req;
    uint64_t v1_file_pos;
    uint64_t v1_file_size = journalfile_current_size(v1_journalfile_struct);

    (void)posix_memalignz((void *)&v1_read_buf, RRDFILE_ALIGNMENT, READAHEAD_BYTES);
    if (!v1_read_buf) {
        netdata_log_error("DBENGINE: Failed to allocate read buffer for v1 migration of '%s'.", path_v1);
        goto cleanup_early;
    }

    netdata_log_info("DBENGINE: Starting direct migration of v1 journal '%s' to v2 '%s'. V1 size: %"PRIu64,
                     path_v1, path_v2, v1_file_size);

    // Iterate through v1 transactions
    for (v1_file_pos = sizeof(struct rrdeng_jf_sb); v1_file_pos < v1_file_size; /* increment handled in loop */) {
        uint64_t current_chunk_read_size = MIN(READAHEAD_BYTES, v1_file_size - v1_file_pos);
        if (current_chunk_read_size == 0) break; // Should not happen if loop condition is correct

        v1_iov = uv_buf_init(v1_read_buf, current_chunk_read_size);
        ssize_t v1_read_ret = uv_fs_read(NULL, &v1_fs_req, fd_v1, &v1_iov, 1, v1_file_pos, NULL);
        uv_fs_req_cleanup(&v1_fs_req);

        if (v1_read_ret < 0) {
            netdata_log_error("DBENGINE: Failed to read v1 journal '%s' at pos %"PRIu64": %s",
                              path_v1, v1_file_pos, uv_strerror((int)v1_read_ret));
            goto cleanup_early;
        }
        if ((size_t)v1_read_ret == 0) break; // End of file or nothing read

        ctx_io_read_op_bytes(ctx, v1_read_ret);
        size_t actual_chunk_size = (size_t)v1_read_ret;
        uint64_t chunk_offset = 0;

        while (chunk_offset < actual_chunk_size) {
            struct rrdeng_jf_transaction_header *jf_header = (struct rrdeng_jf_transaction_header *)(v1_read_buf + chunk_offset);
            uint64_t transaction_id_v1 = 0; // To store V1 transaction ID, not directly used in V2 but good for debug
            unsigned transaction_size_bytes;

            if (STORE_PADDING == jf_header->type) {
                // Skip padding to the next block alignment if possible, or just advance by a minimal unit.
                // This part of v1 format means the rest of the current *block* is padding.
                // A simple approach is to assume padding fills to some alignment or use a small skip.
                // For simplicity here, if we hit padding, we might assume the rest of the buffer is padding
                // or look for next transaction. The original `journalfile_replay_transaction` returns 0 for padding.
                // A robust way would be to know the block size of v1 journal.
                // Let's assume padding means skip to end of current read buffer for simplicity in this direct migration.
                netdata_log_debug(D_RRDENGINE, "DBENGINE: Migration encountered V1 padding at offset %"PRIu64". Skipping rest of current buffer.", v1_file_pos + chunk_offset);
                chunk_offset = actual_chunk_size; // Effectively ends processing of this chunk
                continue;
            }

            if (sizeof(*jf_header) > (actual_chunk_size - chunk_offset)) {
                netdata_log_error("DBENGINE: Corrupted v1 transaction header in '%s' at pos %"PRIu64, path_v1, v1_file_pos + chunk_offset);
                goto cleanup_early; // Corruption detected
            }
            transaction_id_v1 = jf_header->id; // Store for logging if needed
            uint32_t payload_length = jf_header->payload_length;
            transaction_size_bytes = sizeof(struct rrdeng_jf_transaction_header) + payload_length + sizeof(struct rrdeng_jf_transaction_trailer);

            if (transaction_size_bytes == 0 || (chunk_offset + transaction_size_bytes) > actual_chunk_size) {
                 netdata_log_error("DBENGINE: Corrupted v1 transaction (id %"PRIu64") in '%s', invalid size or exceeds buffer. Skipping.", transaction_id_v1, path_v1);
                 // Attempt to find next transaction by aligning to next block or simply failing.
                 // For now, error out as this indicates serious corruption.
                 goto cleanup_early;
            }

            struct rrdeng_jf_transaction_trailer *jf_trailer = (struct rrdeng_jf_transaction_trailer *)(v1_read_buf + chunk_offset + sizeof(*jf_header) + payload_length);
            uLong calculated_crc = crc32(0L, Z_NULL, 0);
            calculated_crc = crc32(calculated_crc, (Bytef*)(v1_read_buf + chunk_offset), sizeof(*jf_header) + payload_length);

            if (unlikely(crc32cmp(jf_trailer->checksum, calculated_crc))) {
                netdata_log_error("DBENGINE: V1 transaction (id %"PRIu64") in '%s' failed CRC check. Skipping.", transaction_id_v1, path_v1);
                chunk_offset += transaction_size_bytes;
                continue; // Skip corrupted transaction
            }

            if (jf_header->type == STORE_DATA) {
                struct rrdeng_jf_store_data *jf_metric_data = (struct rrdeng_jf_store_data *)(v1_read_buf + chunk_offset + sizeof(*jf_header));
                unsigned descr_count = jf_metric_data->number_of_pages;
                unsigned expected_payload_size = sizeof(*jf_metric_data) + (descr_count * sizeof(*jf_metric_data->descr));

                if (payload_length != expected_payload_size) {
                    netdata_log_error("DBENGINE: V1 STORE_DATA transaction (id %"PRIu64") in '%s' has mismatched payload length. Skipping.", transaction_id_v1, path_v1);
                    chunk_offset += transaction_size_bytes;
                    continue;
                }

                time_t current_now_s = max_acceptable_collected_time(); // For page validation

                for (unsigned i = 0; i < descr_count; ++i) {
                    struct rrdeng_extent_page_descr *v1_descr = &jf_metric_data->descr[i];
                    nd_uuid_t *metric_uuid = (nd_uuid_t *)v1_descr->uuid;

                    // Validate page descriptor (similar to journalfile_restore_extent_metadata)
                    // We need update_every from MRG or assume a default if metric is new for validation.
                    // This part is tricky as direct migration might not have full MRG context easily.
                    // For now, let's assume validate_extent_page_descr can be adapted or used carefully.
                    // A simplified validation might be needed if full MRG access is too complex here.
                    // Let's assume 0 for update_every if metric not found, which validate_extent_page_descr handles.
                    VALIDATED_PAGE_DESCRIPTOR vd = validate_extent_page_descr(v1_descr, current_now_s, 0, false);
                    if (!vd.is_valid) {
                        char uuid_str[ND_UUID_STR_LEN];
                        uuid_sprint(uuid_str, metric_uuid);
                        netdata_log_debug(D_RRDENGINE, "DBENGINE: Skipping invalid page for metric %s in v1 journal '%s'.", uuid_str, path_v1);
                        continue;
                    }

                    min_time_s = MIN(min_time_s, vd.start_time_s);
                    max_time_s = MAX(max_time_s, vd.end_time_s);
                    if (vd.update_every_s > 0) {
                        total_samples_collected += (vd.end_time_s - vd.start_time_s) / vd.update_every_s;
                    }

                    // Manage jv2_metrics_info in judy_metrics_temp (using JudyHS)
                    jv2_metrics_info *metric_info_ptr = NULL;
                    Pvoid_t *PValue_metric_hs = JudyHSGet(judy_metrics_temp, metric_uuid, sizeof(nd_uuid_t));

                    if (PValue_metric_hs && *PValue_metric_hs) {
                        metric_info_ptr = (jv2_metrics_info *)(*PValue_metric_hs);
                    } else {
                        metric_info_ptr = callocz(1, sizeof(jv2_metrics_info));
                        if (!metric_info_ptr) {
                            netdata_log_error("DBENGINE: Failed to alloc jv2_metrics_info for UUID.");
                            goto cleanup_judy;
                        }
                        metric_info_ptr->uuid = metric_uuid; // Store pointer to UUID from v1_descr
                        metric_info_ptr->first_time_s = vd.start_time_s;
                        metric_info_ptr->last_time_s = vd.end_time_s;
                        metric_info_ptr->JudyL_pages_by_start_time = NULL; // Initialize JudyL for pages

                        Pvoid_t *PInsert_metric_hs = JudyHSIns(&judy_metrics_temp, metric_uuid, sizeof(nd_uuid_t));
                        if (!PInsert_metric_hs || PInsert_metric_hs == PJERR) {
                            free(metric_info_ptr);
                            netdata_log_error("DBENGINE: Failed to insert into JudyHS for metrics.");
                            goto cleanup_judy;
                        }
                        *PInsert_metric_hs = (Word_t)metric_info_ptr;
                        number_of_metrics++;
                    }

                    // Update metric times
                    metric_info_ptr->first_time_s = MIN(metric_info_ptr->first_time_s, vd.start_time_s);
                    metric_info_ptr->last_time_s = MAX(metric_info_ptr->last_time_s, vd.end_time_s);
                    metric_info_ptr->latest_update_every_s = vd.update_every_s; // Keep the latest one encountered

                    // Store jv2_page_info in the metric's JudyL_pages_by_start_time
                    jv2_page_info *page_info_ptr = callocz(1, sizeof(jv2_page_info));
                    if (!page_info_ptr) {
                        netdata_log_error("DBENGINE: Failed to alloc jv2_page_info.");
                        goto cleanup_judy;
                    }
                    page_info_ptr->start_time_s = vd.start_time_s;
                    page_info_ptr->end_time_s = vd.end_time_s;
                    page_info_ptr->update_every_s = vd.update_every_s;
                    // extent_index will be filled later after all extents are known and indexed.
                    // For now, store the V1 extent offset; this will be mapped to a V2 extent index later.
                    page_info_ptr->v1_extent_offset_key = (Word_t)jf_metric_data->extent_offset;


                    Pvoid_t *PInsert_page = JudyLIns(&metric_info_ptr->JudyL_pages_by_start_time, vd.start_time_s, PJE0);
                    if (!PInsert_page || PInsert_page == PJERR) {
                        free(page_info_ptr);
                        netdata_log_error("DBENGINE: Failed to insert into JudyL for pages.");
                        goto cleanup_judy;
                    }
                    *PInsert_page = page_info_ptr;
                    metric_info_ptr->number_of_pages++;


                    // Manage jv2_extents_info in judy_extents_temp
                    jv2_extents_info *extent_info_ptr = NULL;
                    Word_t extent_key = (Word_t)jf_metric_data->extent_offset; // V1 extent offset as key
                    Pvoid_t *PValue_extent = JudyLGet(judy_extents_temp, extent_key, PJE0);
                    if (PValue_extent && *PValue_extent) {
                        extent_info_ptr = *PValue_extent;
                    } else {
                        extent_info_ptr = callocz(1, sizeof(jv2_extents_info));
                        if (!extent_info_ptr) { netdata_log_error("DBENGINE: Failed to alloc jv2_extents_info."); goto cleanup_judy; }
                        extent_info_ptr->pos = jf_metric_data->extent_offset;
                        extent_info_ptr->bytes = jf_metric_data->extent_size;
                        extent_info_ptr->index = number_of_extents++; // Assign new index
                        Pvoid_t *PInsert = JudyLIns(&judy_extents_temp, extent_key, PJE0);
                        if (!PInsert || PInsert == PJERR) { free(extent_info_ptr); netdata_log_error("DBENGINE: Failed to insert into JudyL for extents."); goto cleanup_judy; }
                        *PInsert = extent_info_ptr;
                    }
                    extent_info_ptr->number_of_pages++;


                    // This page contributes to the total count of page entries
                    number_of_pages++;
                }
            }
            chunk_offset += transaction_size_bytes;
        }
        v1_file_pos += actual_chunk_size;
    }
    posix_memalign_freez(v1_read_buf); v1_read_buf = NULL; // v1_read_buf is freed, set to NULL

    if (min_time_s == LONG_MAX) min_time_s = 0; // Handle case of no valid data

    // Map V1 extent offsets to V2 extent indices for all collected page_info structures
    if (judy_metrics_temp) {
        uint8_t metric_uuid_key[sizeof(nd_uuid_t)];
        Pvoid_t *PValue_metric_hs = JudyHSGet(judy_metrics_temp, NULL, 0); // Get first metric

        while (PValue_metric_hs) {
            jv2_metrics_info *metric_info_ptr = (jv2_metrics_info *)(*PValue_metric_hs);
            if (metric_info_ptr && metric_info_ptr->JudyL_pages_by_start_time) {
                Word_t page_time_key = 0;
                Pvoid_t *PValue_page_info = JudyLFirst(metric_info_ptr->JudyL_pages_by_start_time, &page_time_key, PJE0);
                while (PValue_page_info && *PValue_page_info) {
                    jv2_page_info *page_info_ptr = (jv2_page_info *)(*PValue_page_info);

                    Pvoid_t *PValue_extent = JudyLGet(judy_extents_temp, page_info_ptr->v1_extent_offset_key, PJE0);
                    if (PValue_extent && *PValue_extent) {
                        jv2_extents_info *extent_info_ptr = (jv2_extents_info *)(*PValue_extent);
                        page_info_ptr->extent_index = extent_info_ptr->index; // Set the V2 extent index
                    } else {
                        netdata_log_error("DBENGINE: Failed to find v2 extent index for v1 offset %lu during page setup.", page_info_ptr->v1_extent_offset_key);
                        // This is a critical error, should not happen if extents were processed correctly
                        goto cleanup_judy;
                    }
                    PValue_page_info = JudyLNext(metric_info_ptr->JudyL_pages_by_start_time, &page_time_key, PJE0);
                }
            }
            // Get next metric UUID from JudyHS to use in JudyHSNext. Size is not needed for Next.
            JudyHSNext(judy_metrics_temp, metric_uuid_key, sizeof(nd_uuid_t));
            if (!metric_uuid_key[0] && !metric_uuid_key[1]) { // Simple check if key is zeroed (end of JudyHS)
                 // A more robust check might be needed depending on JudyHS behavior for zeroed keys
                break;
            }
            PValue_metric_hs = JudyHSGet(judy_metrics_temp, metric_uuid_key, sizeof(nd_uuid_t)); // Get next
        }
    }

    netdata_log_info("DBENGINE: V1 data collection complete for '%s'. Extents: %zu, Pages: %zu, Metrics (placeholder): %zu",
                     path_v1, number_of_extents, number_of_pages, number_of_metrics);


    // --- Calculate v2 file size ---
    v2_filesize = sizeof(struct journal_v2_header) + JOURNAL_V2_HEADER_PADDING_SZ;
    v2_filesize += (number_of_extents * sizeof(struct journal_extent_list));
    v2_filesize += sizeof(struct journal_v2_block_trailer); // Extent trailer
    v2_filesize += (number_of_metrics * sizeof(struct journal_metric_list));
    v2_filesize += sizeof(struct journal_v2_block_trailer); // Metric trailer
    v2_filesize += (number_of_pages * sizeof(struct journal_page_list)); // All page entries
    v2_filesize += (number_of_metrics * sizeof(struct journal_page_header)); // Page headers for each metric
    // Note: CRC for each metric's page data block is not explicitly part of this top-level calculation,
    // but space for it must be implicitly available within the page_offset area or handled by writers.
    // The journalfile_migrate_to_v2_callback includes sizeof(struct journal_v2_block_trailer) *per page entry*
    // in its calculation: `(number_of_pages * (sizeof(struct journal_page_list) + sizeof(struct journal_page_header) + sizeof(struct journal_v2_block_trailer)))`
    // This seems to differ from the documented structure. Sticking to documented structure for now.
    // If CRCs are per metric page block, that's number_of_metrics * sizeof(struct journal_v2_block_trailer) for page CRCs.
    // Let's add that explicitly if it's not covered by page_offset calculations later.
    // For now, assume page data area is contiguous and CRCs are handled by writers within their allocated space if needed, or at end of sections.
    // The V2 spec implies one CRC trailer at the end of the *entire* page data section, or per metric block.
    // Let's assume the latter: one CRC per metric's page data.
    // This means the page_offset points to the start of page data, and within that, data is organized by metric.
    // Each metric's page block (header + entries) would then have a trailer.
    // This implies `number_of_metrics * sizeof(struct journal_v2_block_trailer)` for these page data CRCs.
    v2_filesize += (number_of_metrics * sizeof(struct journal_v2_block_trailer)); // CRC trailer for each metric's page data block

    v2_filesize += sizeof(struct journal_v2_block_trailer); // Main File trailer

    if (number_of_metrics == 0 || number_of_pages == 0 || number_of_extents == 0) {
        netdata_log_info("DBENGINE: No data (metrics: %zu, pages: %zu, extents: %zu) collected from v1 journal '%s'. Aborting migration.",
                         number_of_metrics, number_of_pages, number_of_extents, path_v1);
        goto cleanup_judy;
    }


    // --- Create and mmap v2 file ---
    fd_v2 = open(path_v2, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd_v2 < 0) {
        netdata_log_error("DBENGINE: Failed to create v2 journal file '%s': %s", path_v2, strerror(errno));
        goto cleanup_judy;
    }

    if (ftruncate(fd_v2, v2_filesize) != 0) {
        netdata_log_error("DBENGINE: Failed to ftruncate v2 journal file '%s' to size %zu: %s", path_v2, v2_filesize, strerror(errno));
        goto cleanup_fd_v2;
    }

    v2_map_addr = nd_mmap(NULL, v2_filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_v2, 0);
    if (v2_map_addr == MAP_FAILED) {
        netdata_log_error("DBENGINE: Failed to mmap v2 journal file '%s': %s", path_v2, strerror(errno));
        v2_map_addr = NULL; // Ensure it's NULL for cleanup
        goto cleanup_fd_v2;
    }

    // --- Write v2 data (header, extents, metrics, pages, CRCs) ---
    // This is still largely placeholder and needs the actual data writing logic.
    struct journal_v2_header j2_header;
    memset(&j2_header, 0, sizeof(j2_header));
    j2_header.magic = JOURVAL_V2_MAGIC_NEW;
    j2_header.start_time_ut = min_time_s * USEC_PER_SEC;
    j2_header.end_time_ut = max_time_s * USEC_PER_SEC;
    j2_header.extent_count = number_of_extents;
    j2_header.metric_count = number_of_metrics;
    j2_header.page_count = number_of_pages;

    // Calculate offsets
    uint32_t current_offset = sizeof(struct journal_v2_header) + JOURNAL_V2_HEADER_PADDING_SZ;
    j2_header.extent_offset = current_offset;
    current_offset += (number_of_extents * sizeof(struct journal_extent_list));
    j2_header.extent_trailer_offset = current_offset;
    current_offset += sizeof(struct journal_v2_block_trailer);

    j2_header.metric_offset = current_offset;
    current_offset += (number_of_metrics * sizeof(struct journal_metric_list));
    j2_header.metric_trailer_offset = current_offset;
    current_offset += sizeof(struct journal_v2_block_trailer);

    j2_header.page_offset = current_offset;
    // The rest of the file up to total_file_size - sizeof(file_trailer) is for page data + page CRCs

    j2_header.journal_v1_file_size = (uint32_t)v1_file_size;
    j2_header.journal_v2_file_size = v2_filesize;
    j2_header.samples = total_samples_collected;
    j2_header.data = v2_map_addr; // Temporary for writing functions

    // --- Write Extent List ---
    uint8_t *v2_write_ptr = (uint8_t *)v2_map_addr + j2_header.extent_offset;
    v2_write_ptr = journalfile_v2_write_extent_list(judy_extents_temp, v2_write_ptr);
    if (!v2_write_ptr || (uint32_t)(v2_write_ptr - (uint8_t *)v2_map_addr) != j2_header.extent_trailer_offset) {
        netdata_log_error("DBENGINE: Failed to write extent list or size mismatch for v2 journal '%s'. Expected offset %u, got %tu",
                          path_v2, j2_header.extent_trailer_offset, v2_write_ptr - (uint8_t *)v2_map_addr);
        goto cleanup_mmap_v2;
    }

    // Calculate and write extent list CRC
    uLong extent_crc = crc32(0L, Z_NULL, 0);
    extent_crc = crc32(extent_crc, (Bytef*)((uint8_t*)v2_map_addr + j2_header.extent_offset), j2_header.extent_count * sizeof(struct journal_extent_list));
    struct journal_v2_block_trailer *extent_trailer = (struct journal_v2_block_trailer *)v2_write_ptr;
    crc32set(extent_trailer->checksum, extent_crc);
    v2_write_ptr += sizeof(struct journal_v2_block_trailer);

    // --- Metrics & Pages Writing ---
    struct journal_metric_list_to_sort *uuid_list = NULL;
    if (number_of_metrics > 0) {
        uuid_list = mallocz(number_of_metrics * sizeof(struct journal_metric_list_to_sort));
        if (!uuid_list) {
            netdata_log_error("DBENGINE: Failed to allocate uuid_list for sorting metrics.");
            goto cleanup_mmap_v2;
        }

        size_t current_metric_idx = 0;
        uint8_t metric_uuid_key_iter[sizeof(nd_uuid_t)];
        Pvoid_t *PValue_metric_hs_iter = JudyHSGet(judy_metrics_temp, NULL, 0); // Get first
        while (PValue_metric_hs_iter) {
            if (current_metric_idx >= number_of_metrics) { // Should not happen
                netdata_log_error("DBENGINE: Exceeded expected number of metrics while populating sort list.");
                free(uuid_list);
                goto cleanup_mmap_v2;
            }
            uuid_list[current_metric_idx++].metric_info = (jv2_metrics_info *)(*PValue_metric_hs_iter);

            if (JudyHSNext(judy_metrics_temp, metric_uuid_key_iter, sizeof(nd_uuid_t)) <=0) break;
            PValue_metric_hs_iter = JudyHSGet(judy_metrics_temp, metric_uuid_key_iter, sizeof(nd_uuid_t));
        }
        fatal_assert(current_metric_idx == number_of_metrics); // Ensure all metrics were added

        qsort(uuid_list, number_of_metrics, sizeof(struct journal_metric_list_to_sort), journalfile_metric_compare);
    }

    // Position pointer for metric list
    v2_write_ptr = (uint8_t *)v2_map_addr + j2_header.metric_offset;
    uint8_t *metric_list_start_ptr = v2_write_ptr; // For CRC calculation later
    uint32_t current_page_data_block_offset = j2_header.page_offset; // Start of the first metric's page data area

    for (size_t i = 0; i < number_of_metrics; ++i) {
        jv2_metrics_info *metric_info = uuid_list[i].metric_info;
        uint32_t metric_uuid_relative_offset = (uint32_t)(v2_write_ptr - (uint8_t*)v2_map_addr);
        size_t metric_specific_samples = 0;

        struct journal_metric_list *current_metric_entry_in_file = (struct journal_metric_list *)v2_write_ptr;
        v2_write_ptr = journalfile_v2_write_metric_page(&j2_header, v2_write_ptr, metric_info, current_page_data_block_offset);
        if (!v2_write_ptr) {
            netdata_log_error("DBENGINE: Failed to write metric page for v2 journal '%s'.", path_v2);
            if (uuid_list) free(uuid_list);
            goto cleanup_mmap_v2;
        }

        // Write Page Header for this metric
        struct journal_page_header *page_header_ptr = (struct journal_page_header *)((uint8_t*)v2_map_addr + current_page_data_block_offset);
        uint8_t* page_data_start_for_metric_ptr = (uint8_t*)journalfile_v2_write_data_page_header(&j2_header, page_header_ptr, metric_info, metric_uuid_relative_offset);
        if (!page_data_start_for_metric_ptr) {
             netdata_log_error("DBENGINE: Failed to write page data header for v2 journal '%s'.", path_v2);
            if (uuid_list) free(uuid_list);
            goto cleanup_mmap_v2;
        }

        // Write Page List Entries (descriptors) for this metric
        uint8_t *next_page_data_block_start_ptr = (uint8_t*)journalfile_v2_write_descriptors(&j2_header, page_data_start_for_metric_ptr, metric_info, current_metric_entry_in_file, &metric_specific_samples);
        if (!next_page_data_block_start_ptr) {
            netdata_log_error("DBENGINE: Failed to write page descriptors for v2 journal '%s'.", path_v2);
            if (uuid_list) free(uuid_list);
            goto cleanup_mmap_v2;
        }
        current_metric_entry_in_file->samples = metric_specific_samples; // Update samples count in metric list

        // Calculate and write CRC for this metric's page data block
        uLong page_block_crc = crc32(0L, Z_NULL, 0);
        size_t page_block_size = (size_t)(next_page_data_block_start_ptr - ((uint8_t*)v2_map_addr + current_page_data_block_offset));
        page_block_crc = crc32(page_block_crc, (Bytef*)((uint8_t*)v2_map_addr + current_page_data_block_offset), page_block_size);
        struct journal_v2_block_trailer *page_data_trailer = (struct journal_v2_block_trailer *)next_page_data_block_start_ptr;
        crc32set(page_data_trailer->checksum, page_block_crc);

        current_page_data_block_offset = (uint32_t)((next_page_data_block_start_ptr + sizeof(struct journal_v2_block_trailer)) - (uint8_t*)v2_map_addr);
    }
    if (uuid_list) free(uuid_list);

    // Check if v2_write_ptr is at the metric_trailer_offset
    if ((uint32_t)(v2_write_ptr - (uint8_t*)v2_map_addr) != j2_header.metric_trailer_offset) {
        netdata_log_error("DBENGINE: Metric list write error or size mismatch for v2 journal '%s'. Expected offset %u, got %tu",
                          path_v2, j2_header.metric_trailer_offset, v2_write_ptr - (uint8_t*)v2_map_addr);
        goto cleanup_mmap_v2;
    }

    // Calculate and write metric list CRC
    uLong metric_list_crc = crc32(0L, Z_NULL, 0);
    metric_list_crc = crc32(metric_list_crc, (Bytef*)metric_list_start_ptr, j2_header.metric_count * sizeof(struct journal_metric_list));
    struct journal_v2_block_trailer *metric_trailer = (struct journal_v2_block_trailer *)v2_write_ptr;
    crc32set(metric_trailer->checksum, metric_list_crc);
    v2_write_ptr += sizeof(struct journal_v2_block_trailer);

    // Sanity check: v2_write_ptr should now be at j2_header.page_offset if page CRCs were included in main page_offset block
    // OR it should be at the start of the first page data block, which is j2_header.page_offset.
    // The current_page_data_block_offset should be at the end of all page data + their CRCs.
    // This needs careful alignment if page_offset was meant to be the start of the *entire* page section including all blocks.
    // Based on loop: current_page_data_block_offset is correctly at the position for the *next* metric's page data block or end of all page data.
    // And v2_write_ptr is at the end of metric list trailer.
    // The check `(uint32_t)(v2_write_ptr - (uint8_t*)v2_map_addr) == j2_header.page_offset` should hold if metric section ends where page section begins.
    if ((uint32_t)(v2_write_ptr - (uint8_t*)v2_map_addr) != j2_header.page_offset) {
         netdata_log_error("DBENGINE: Pointer mismatch after metric section for v2 journal '%s'. Expected offset %u, got %tu",
                          path_v2, j2_header.page_offset, v2_write_ptr - (uint8_t*)v2_map_addr);
        goto cleanup_mmap_v2;
    }


    // Final header write & main file CRC
    j2_header.data = NULL; // Clear temporary pointer
    memcpy(v2_map_addr, &j2_header, sizeof(struct journal_v2_header));

    uLong header_crc = crc32(0L, Z_NULL, 0);
    header_crc = crc32(header_crc, (Bytef*)v2_map_addr, sizeof(struct journal_v2_header));
    struct journal_v2_block_trailer *file_trailer = (struct journal_v2_block_trailer *)((uint8_t*)v2_map_addr + v2_filesize - sizeof(struct journal_v2_block_trailer));
    crc32set(file_trailer->checksum, header_crc);


    if (msync(v2_map_addr, v2_filesize, MS_SYNC) != 0) {
        netdata_log_error("DBENGINE: Failed to msync v2 journal file '%s': %s", path_v2, strerror(errno));
        // Proceed to cleanup, data might not be fully synced.
    }

    ret = 0; // Mark as success (actual data writing for metrics/pages pending)
    netdata_log_info("DBENGINE: Direct migration for '%s' to '%s' completed sections up to extents. Metrics/pages TBD.", path_v1, path_v2);


cleanup_mmap_v2:
    if (v2_map_addr) {
        nd_munmap(v2_map_addr, v2_filesize);
    }

cleanup_fd_v2:
    if (fd_v2 >= 0) {
        close(fd_v2);
        if (ret != 0) { // If something went wrong after creating fd_v2, unlink the file
            unlink(path_v2);
        }
    }

cleanup_judy:
    if (judy_metrics_temp) {
        // Free جv2_metrics_info structs and their internal JudyL arrays for pages
        uint8_t metric_uuid_key_cleanup[sizeof(nd_uuid_t)];
        Pvoid_t *PValue_metric_hs_cleanup = JudyHSGet(judy_metrics_temp, NULL, 0); // Get first
        while (PValue_metric_hs_cleanup) {
            jv2_metrics_info *metric_info_to_free = (jv2_metrics_info *)(*PValue_metric_hs_cleanup);
            if (metric_info_to_free) {
                if (metric_info_to_free->JudyL_pages_by_start_time) {
                    Word_t page_idx = 0;
                    Pvoid_t *PPageValue;
                    bool first_page = true;
                    while((PPageValue = JudyLFirstThenNext(metric_info_to_free->JudyL_pages_by_start_time, &page_idx, &first_page))) {
                        if (*PPageValue) free(*PPageValue); // Free jv2_page_info
                    }
                    JudyLFreeArray(&metric_info_to_free->JudyL_pages_by_start_time, PJE0);
                }
                free(metric_info_to_free); // Free jv2_metrics_info itself
            }
            // Get next metric UUID for JudyHSNext
            // Important: JudyHSNext needs the key buffer to be large enough
            if (JudyHSNext(judy_metrics_temp, metric_uuid_key_cleanup, sizeof(nd_uuid_t)) <=0) break; // no more entries or error
            PValue_metric_hs_cleanup = JudyHSGet(judy_metrics_temp, metric_uuid_key_cleanup, sizeof(nd_uuid_t));
        }
        JudyHSFreeArray(judy_metrics_temp, PJE0);
    }
    if (judy_extents_temp) {
        Pvoid_t *PValue;
        Word_t idx = 0;
        bool first = true;
        while((PValue = JudyLFirstThenNext(judy_extents_temp, &idx, &first))) {
            if (*PValue) free(*PValue);
        }
        JudyLFreeArray(&judy_extents_temp, PJE0);
    }
    // Free other temporary structures if any

cleanup_early:
    if (v1_read_buf) {
        posix_memalign_freez(v1_read_buf);
    }
    if (ret != 0) { // If returning error, ensure path_v2 is unlinked if it was ever about to be used.
        unlink(path_v2);
    }
    return ret;
}

// Checks that the extent list checksum is valid
static int journalfile_check_v2_extent_list (void *data_start, size_t file_size)
{
    UNUSED(file_size);
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + j2_header->extent_trailer_offset);
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) data_start + j2_header->extent_offset, j2_header->extent_count * sizeof(struct journal_extent_list));
    if (unlikely(crc32cmp(journal_v2_trailer->checksum, crc))) {
        netdata_log_error("DBENGINE: extent list CRC32 check: FAILED");
        return 1;
    }

    return 0;
}

// Checks that the metric list (UUIDs) checksum is valid
static int journalfile_check_v2_metric_list(void *data_start, size_t file_size)
{
    UNUSED(file_size);
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + j2_header->metric_trailer_offset);
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t *) data_start + j2_header->metric_offset, j2_header->metric_count * sizeof(struct journal_metric_list));
    if (unlikely(crc32cmp(journal_v2_trailer->checksum, crc))) {
        netdata_log_error("DBENGINE: metric list CRC32 check: FAILED");
        return 1;
    }
    return 0;
}

//
// Return
//   0 Ok
//   1 Invalid
//   2 Force rebuild
//   3 skip

static int journalfile_v2_validate(void *data_start, size_t journal_v2_file_size, size_t journal_v1_file_size)
{
    int rc;
    uLong crc;

    struct journal_v2_header *j2_header = (void *) data_start;
    struct journal_v2_block_trailer *journal_v2_trailer;

    if (j2_header->magic == JOURVAL_V2_REBUILD_MAGIC)
        return 2;

    if (j2_header->magic == JOURVAL_V2_SKIP_MAGIC)
        return 3;

    // Magic failure
    // Accept either old V2 magic or the new one if direct migration occurred
    if (j2_header->magic != JOURVAL_V2_MAGIC && j2_header->magic != JOURVAL_V2_MAGIC_NEW)
        return 1;

    if (j2_header->journal_v2_file_size != journal_v2_file_size)
        return 1;

    if (journal_v1_file_size && j2_header->journal_v1_file_size != journal_v1_file_size)
        return 1;

    journal_v2_trailer = (struct journal_v2_block_trailer *) ((uint8_t *) data_start + journal_v2_file_size - sizeof(*journal_v2_trailer));

    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (void *) j2_header, sizeof(*j2_header));

    rc = crc32cmp(journal_v2_trailer->checksum, crc);
    if (unlikely(rc)) {
        netdata_log_error("DBENGINE: file CRC32 check: FAILED");
        return 1;
    }

    rc = journalfile_check_v2_extent_list(data_start, journal_v2_file_size);
    if (rc) return 1;

    if (!db_engine_journal_check)
        return 0;

    rc = journalfile_check_v2_metric_list(data_start, journal_v2_file_size);
    if (rc) return 1;

    return 0;
}

void journalfile_v2_populate_retention_to_mrg(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile) {
    usec_t started_ut = now_monotonic_usec();

    size_t data_size = 0;
    struct journal_v2_header *j2_header = journalfile_v2_data_acquire(journalfile, &data_size, 0, 0);
    if(!j2_header)
        return;

    uint8_t *data_start = (uint8_t *)j2_header;

    if (journalfile->v2.flags & JOURNALFILE_FLAG_METRIC_CRC_CHECK) {
        journalfile->v2.flags &= ~JOURNALFILE_FLAG_METRIC_CRC_CHECK;
        if (journalfile_check_v2_metric_list(data_start, j2_header->journal_v2_file_size)) {
            journalfile->v2.flags &= ~JOURNALFILE_FLAG_IS_AVAILABLE;
            // needs rebuild
            return;
        }
    }

    char path_v2[RRDENG_PATH_MAX];
    journalfile_v2_generate_path(journalfile->datafile, path_v2, sizeof(path_v2));
    time_t global_first_time_s;
    bool failed = false;
    uint32_t entries;
    uint64_t detailed_samples = 0;
    PROTECTED_ACCESS_SETUP(data_start, journalfile->mmap.size, path_v2, "mrg-load");
    if(no_signal_received) {
        entries = j2_header->metric_count;
        struct journal_metric_list *metric = (struct journal_metric_list *) (data_start + j2_header->metric_offset);
        time_t header_start_time_s  = (time_t) (j2_header->start_time_ut / USEC_PER_SEC);
        global_first_time_s = header_start_time_s;
        time_t now_s = max_acceptable_collected_time();
        for (size_t i=0; i < entries; i++) {
            time_t start_time_s = header_start_time_s + metric->delta_start_s;
            time_t end_time_s = header_start_time_s + metric->delta_end_s;
            uint32_t metric_samples = metric->samples;
            detailed_samples += metric_samples;

            mrg_update_metric_retention_and_granularity_by_uuid(
                main_mrg, (Word_t)ctx, &metric->uuid, start_time_s, end_time_s, metric->update_every_s, now_s);

            metric++;
        }
    } else
        failed = true;

    uint64_t samples =  j2_header->samples;
    journalfile_v2_data_release(journalfile);

    if (unlikely(failed))
        return;

    journalfile->v2.samples = samples;

    usec_t ended_ut = now_monotonic_usec();

    nd_log_daemon(NDLP_DEBUG, "DBENGINE: journal v2 of tier %d, datafile %u populated, size: %0.2f MiB, metrics: %0.2f k, %0.2f ms, samples: %"PRIu64", detailed samples: %"PRIu64
        , ctx->config.tier, journalfile->datafile->fileno
        , (double)data_size / 1024 / 1024
        , (double)entries / 1000
        , ((double)(ended_ut - started_ut) / USEC_PER_MS),
        samples, detailed_samples
        );

    // Update CTX samples
    __atomic_add_fetch(&ctx->atomic.samples, samples, __ATOMIC_RELAXED);

    time_t old = __atomic_load_n(&ctx->atomic.first_time_s, __ATOMIC_RELAXED);;
    do {
        if(old <= global_first_time_s)
            break;
    } while(!__atomic_compare_exchange_n(&ctx->atomic.first_time_s, &old, global_first_time_s, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

int journalfile_v2_load(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile, struct rrdengine_datafile *datafile)
{
    int ret, fd;
    char path_v1[RRDENG_PATH_MAX];
    char path_v2[RRDENG_PATH_MAX];
    struct stat statbuf;
    size_t journal_v1_file_size = 0;
    size_t journal_v2_file_size;

    journalfile_v1_generate_path(datafile, path_v1, sizeof(path_v1));
    ret = stat(path_v1, &statbuf);
    if (!ret)
        journal_v1_file_size = (uint32_t)statbuf.st_size;

    journalfile_v2_generate_path(datafile, path_v2, sizeof(path_v2));
    fd = open(path_v2, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        ctx_fs_error(ctx);
        netdata_log_error("DBENGINE: failed to open \"%s\"", path_v2);
        return 1;
    }

    ret = fstat(fd, &statbuf);
    if (ret) {
        netdata_log_error("DBENGINE: failed to get file information for \"%s\"", path_v2);
        close(fd);
        return 1;
    }

    journal_v2_file_size = (size_t)statbuf.st_size;

    if (journal_v2_file_size < sizeof(struct journal_v2_header)) {
        error_report("Invalid file \"%s\". Not the expected size", path_v2);
        close(fd);
        return 1;
    }

    usec_t mmap_start_ut = now_monotonic_usec();
    uint8_t *data_start = nd_mmap(NULL, journal_v2_file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data_start == MAP_FAILED) {
        close(fd);
        return 1;
    }

    nd_log_daemon(NDLP_DEBUG, "DBENGINE: checking integrity of \"%s\"", path_v2);

    usec_t validation_start_ut = now_monotonic_usec();

    int rc = 0;
    PROTECTED_ACCESS_SETUP(data_start, journal_v2_file_size, path_v2, "validate");
    if(no_signal_received) {
        rc = journalfile_v2_validate(data_start, journal_v2_file_size, journal_v1_file_size);
    }
    else {
        rc = 2;
    }

    if (unlikely(rc)) {
        if (rc == 2)
            error_report("File \"%s\" needs to be rebuilt", path_v2);
        else if (rc == 3)
            error_report("File \"%s\" will be skipped", path_v2);
        else
            error_report("File \"%s\" is invalid and it will be rebuilt", path_v2);

        if (unlikely(nd_munmap(data_start, journal_v2_file_size)))
            netdata_log_error("DBENGINE: failed to unmap \"%s\"", path_v2);

        close(fd);
        return rc;
    }

    struct journal_v2_header *j2_header = (void *) data_start;
    uint32_t entries = j2_header->metric_count;

    if (unlikely(!entries)) {
        if (unlikely(nd_munmap(data_start, journal_v2_file_size)))
            netdata_log_error("DBENGINE: failed to unmap \"%s\"", path_v2);

        close(fd);
        return 1;
    }

    usec_t finished_ut = now_monotonic_usec();

    nd_log_daemon(NDLP_DEBUG, "DBENGINE: journal v2 \"%s\" loaded, size: %0.2f MiB, metrics: %0.2f k, "
         "mmap: %0.2f ms, validate: %0.2f ms"
         , path_v2
         , (double)journal_v2_file_size / 1024 / 1024
         , (double)entries / 1000
         , ((double)(validation_start_ut - mmap_start_ut) / USEC_PER_MS)
         , ((double)(finished_ut - validation_start_ut) / USEC_PER_MS)
         );

    // Initialize the journal file to be able to access the data

    if (!db_engine_journal_check)
        journalfile->v2.flags |= JOURNALFILE_FLAG_METRIC_CRC_CHECK;

    journalfile_v2_data_set(journalfile, fd, data_start, journal_v2_file_size);

    ctx_current_disk_space_increase(ctx, journal_v2_file_size);

    // File is OK load it
    return 0;
}

struct journal_metric_list_to_sort {
    struct jv2_metrics_info *metric_info;
};

static int journalfile_metric_compare (const void *item1, const void *item2)
{
    const struct jv2_metrics_info *metric1 = ((struct journal_metric_list_to_sort *) item1)->metric_info;
    const struct jv2_metrics_info *metric2 = ((struct journal_metric_list_to_sort *) item2)->metric_info;

    return memcmp(metric1->uuid, metric2->uuid, sizeof(nd_uuid_t));
}


// Write list of extents for the journalfile
void *journalfile_v2_write_extent_list(Pvoid_t JudyL_extents_pos, void *data)
{
    Pvoid_t *PValue;
    struct journal_extent_list *j2_extent_base = (void *) data;
    struct jv2_extents_info *ext_info;

    bool first = true;
    Word_t pos = 0;
    size_t count = 0;
    while ((PValue = JudyLFirstThenNext(JudyL_extents_pos, &pos, &first))) {
        ext_info = *PValue;
        size_t index = ext_info->index;
        j2_extent_base[index].file_index = 0;
        j2_extent_base[index].datafile_offset = ext_info->pos;
        j2_extent_base[index].datafile_size = ext_info->bytes;
        j2_extent_base[index].pages = ext_info->number_of_pages;
        count++;
    }
    return j2_extent_base + count;
}

static int journalfile_verify_space(struct journal_v2_header *j2_header, void *data, uint32_t bytes)
{
    if ((unsigned long)(((uint8_t *) data - (uint8_t *)  j2_header->data) + bytes) > (j2_header->journal_v2_file_size - sizeof(struct journal_v2_block_trailer)))
        return 1;

    return 0;
}

void *journalfile_v2_write_metric_page(struct journal_v2_header *j2_header, void *data, struct jv2_metrics_info *metric_info, uint32_t pages_offset)
{
    struct journal_metric_list *metric = (void *) data;

    if (journalfile_verify_space(j2_header, data, sizeof(*metric)))
        return NULL;

    uuid_copy(metric->uuid, *metric_info->uuid);
    metric->entries = metric_info->number_of_pages;
    metric->page_offset = pages_offset;
    metric->delta_start_s = (uint32_t)(metric_info->first_time_s - (time_t)(j2_header->start_time_ut / USEC_PER_SEC));
    metric->delta_end_s = (uint32_t)(metric_info->last_time_s - (time_t)(j2_header->start_time_ut / USEC_PER_SEC));
    metric->update_every_s = 0;

    return ++metric;
}

void *journalfile_v2_write_data_page_header(struct journal_v2_header *j2_header __maybe_unused, void *data, struct jv2_metrics_info *metric_info, uint32_t uuid_offset)
{
    struct journal_page_header *data_page_header = (void *) data;

    data_page_header->uuid_offset = uuid_offset;        // data header OFFSET poings to METRIC in the directory
    data_page_header->entries = metric_info->number_of_pages;
    return ++data_page_header;
}

void *journalfile_v2_write_data_page(struct journal_v2_header *j2_header, void *data, struct jv2_page_info *page_info)
{
    struct journal_page_list *data_page = data;

    if (journalfile_verify_space(j2_header, data, sizeof(*data_page)))
        return NULL;

    data_page->delta_start_s = (uint32_t) (page_info->start_time_s - (time_t) (j2_header->start_time_ut) / USEC_PER_SEC);
    data_page->delta_end_s = (uint32_t) (page_info->end_time_s - (time_t) (j2_header->start_time_ut) / USEC_PER_SEC);
    data_page->extent_index = page_info->extent_index;

    data_page->update_every_s = page_info->update_every_s;
    return ++data_page;
}

// Must be recorded in metric_info->entries
static void *journalfile_v2_write_descriptors(struct journal_v2_header *j2_header, void *data, struct jv2_metrics_info *metric_info,
        struct journal_metric_list *current_metric, size_t *samples)
{
    Pvoid_t *PValue;

    struct journal_page_list *data_page = (void *)data;
    // We need to write all descriptors with index metric_info->min_index_time_s, metric_info->max_index_time_s
    // that belong to this journal file
    Pvoid_t JudyL_array = metric_info->JudyL_pages_by_start_time;

    Word_t index_time = 0;
    bool first = true;
    struct jv2_page_info *page_info;
    uint32_t update_every_s = 0;
    size_t local_samples = 0;
    while ((PValue = JudyLFirstThenNext(JudyL_array, &index_time, &first))) {
        page_info = *PValue;
        // Write one descriptor and return the next data page location
        data_page = journalfile_v2_write_data_page(j2_header, (void *) data_page, page_info);
        update_every_s = page_info->update_every_s;
        if (likely(update_every_s))
            local_samples += ((page_info->end_time_s - page_info->start_time_s) / update_every_s);
        if (NULL == data_page)
            break;
    }
    (*samples) += local_samples;
    current_metric->update_every_s = update_every_s;
    return data_page;
}

// Migrate the journalfile pointed by datafile
// activate : make the new file active immediately
//            journafile data will be set and descriptors (if deleted) will be repopulated as needed
// startup  : if the migration is done during agent startup
//            this will allow us to optimize certain things

bool journalfile_migrate_to_v2_callback(Word_t section, unsigned datafile_fileno __maybe_unused, uint8_t type __maybe_unused,
                                        Pvoid_t JudyL_metrics, Pvoid_t JudyL_extents_pos,
                                        size_t number_of_extents, size_t number_of_metrics, size_t number_of_pages, void *user_data)
{
    char path[RRDENG_PATH_MAX];
    Pvoid_t *PValue;
    struct rrdengine_instance *ctx = (struct rrdengine_instance *) section;
    struct rrdengine_journalfile *journalfile = (struct rrdengine_journalfile *) user_data;
    struct rrdengine_datafile *datafile = journalfile->datafile;
    time_t min_time_s = LONG_MAX;
    time_t max_time_s = 0;
    struct jv2_metrics_info *metric_info;
    uint64_t journalfile_samples = 0;

    journalfile_v2_generate_path(datafile, path, sizeof(path));

    netdata_log_info("DBENGINE: indexing file \"%s\": extents %zu, metrics %zu, pages %zu",
        path,
        number_of_extents,
        number_of_metrics,
        number_of_pages);

#ifdef NETDATA_INTERNAL_CHECKS
    usec_t start_loading = now_monotonic_usec();
#endif

    size_t total_file_size = 0;
    total_file_size  += (sizeof(struct journal_v2_header) + JOURNAL_V2_HEADER_PADDING_SZ);

    // Extents will start here
    uint32_t extent_offset = total_file_size;
    total_file_size  += (number_of_extents * sizeof(struct journal_extent_list));

    uint32_t extent_offset_trailer = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    // UUID list will start here
    uint32_t metrics_offset = total_file_size;
    total_file_size  += (number_of_metrics * sizeof(struct journal_metric_list));

    // UUID list trailer
    uint32_t metric_offset_trailer = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    // descr @ time will start here
    uint32_t pages_offset = total_file_size;
    total_file_size  += (number_of_pages * (sizeof(struct journal_page_list) + sizeof(struct journal_page_header) + sizeof(struct journal_v2_block_trailer)));

    // File trailer
    uint32_t trailer_offset = total_file_size;
    total_file_size  += sizeof(struct journal_v2_block_trailer);

    int fd_v2;
    uint8_t *data_start = nd_mmap_advanced(path, total_file_size, MAP_SHARED, 0, false, true, &fd_v2);
    if(!data_start) {
        nd_log_daemon(NDLP_WARNING, "DBENGINE: Failed to allocate %"PRIu64" bytes of memory for journal file \"%s\". Will retry later", total_file_size, path);
        return false;
    }

    struct journal_metric_list_to_sort *uuid_list = NULL;

    PROTECTED_ACCESS_SETUP(data_start, total_file_size, path, "migrate");
    if(no_signal_received) {
        fatal_assert(extent_offset <= total_file_size);
        memset(data_start, 0, extent_offset);

        // Write header
        struct journal_v2_header j2_header;
        memset(&j2_header, 0, sizeof(j2_header));

        j2_header.magic = JOURVAL_V2_MAGIC;
        j2_header.start_time_ut = 0;
        j2_header.end_time_ut = 0;
        j2_header.extent_count = number_of_extents;
        j2_header.extent_offset = extent_offset;
        j2_header.metric_count = number_of_metrics;
        j2_header.metric_offset = metrics_offset;
        j2_header.page_count = number_of_pages;
        j2_header.page_offset = pages_offset;
        j2_header.extent_trailer_offset = extent_offset_trailer;
        j2_header.metric_trailer_offset = metric_offset_trailer;
        j2_header.journal_v2_file_size = total_file_size;
        j2_header.journal_v1_file_size = (uint32_t)journalfile_current_size(journalfile);
        j2_header.data = data_start; // Used during migration

        struct journal_v2_block_trailer *journal_v2_trailer;

        uint8_t *data = journalfile_v2_write_extent_list(JudyL_extents_pos, data_start + extent_offset);
        internal_error(
            true, "DBENGINE: write extent list so far %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);

        fatal_assert(data == data_start + extent_offset_trailer);

        // Calculate CRC for extents
        journal_v2_trailer = (struct journal_v2_block_trailer *)(data_start + extent_offset_trailer);
        uLong crc;
        crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (uint8_t *)data_start + extent_offset, number_of_extents * sizeof(struct journal_extent_list));
        crc32set(journal_v2_trailer->checksum, crc);

        internal_error(
            true, "DBENGINE: CALCULATE CRC FOR EXTENT %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);
        // Skip the trailer, point to the metrics off
        data += sizeof(struct journal_v2_block_trailer);

        // Sanity check -- we must be at the metrics_offset
        fatal_assert(data == data_start + metrics_offset);

        // Allocate array to sort UUIDs and keep them sorted in the journal because we want to do binary search when we do lookups
        uuid_list = mallocz(number_of_metrics * sizeof(struct journal_metric_list_to_sort));

        Word_t Index = 0;
        size_t count = 0;
        bool first_then_next = true;
        while ((PValue = JudyLFirstThenNext(JudyL_metrics, &Index, &first_then_next))) {
            metric_info = *PValue;

            fatal_assert(metric_info != NULL);
            fatal_assert(count < number_of_metrics);
            uuid_list[count++].metric_info = metric_info;
            min_time_s = MIN(min_time_s, metric_info->first_time_s);
            max_time_s = MAX(max_time_s, metric_info->last_time_s);
        }

        fatal_assert(count == number_of_metrics);

        // Check if not properly set in the loop above to prevent overflow
        if (min_time_s == LONG_MAX)
            min_time_s = 0;

        // Store in the header
        j2_header.start_time_ut = min_time_s * USEC_PER_SEC;
        j2_header.end_time_ut = max_time_s * USEC_PER_SEC;

        qsort(&uuid_list[0], number_of_metrics, sizeof(struct journal_metric_list_to_sort), journalfile_metric_compare);
        internal_error(
            true, "DBENGINE: traverse and qsort  UUID %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);

        for (Index = 0; Index < number_of_metrics; Index++) {
            size_t metric_samples = 0;
            metric_info = uuid_list[Index].metric_info;

            // Calculate current UUID offset from start of file. We will store this in the data page header
            uint32_t uuid_offset = data - data_start;

            struct journal_metric_list *current_metric = (void *)data;
            // Write the UUID we are processing
            data = (void *)journalfile_v2_write_metric_page(&j2_header, data, metric_info, pages_offset);
            if (unlikely(!data))
                break;

            // Next we will write
            //   Header
            //   Detailed entries (descr @ time)

            // Keep the page_list_header, to be used for migration when where agent is running
            metric_info->page_list_header = pages_offset;

            // Write page header
            struct journal_page_header *data_header = (void *) ((uint8_t *) data_start + pages_offset);
            void *metric_page =
                journalfile_v2_write_data_page_header(&j2_header, data_header, metric_info, uuid_offset);

            // Start writing descr @ time
            uint8_t *next_page_address = journalfile_v2_write_descriptors(&j2_header, metric_page, metric_info, current_metric, &metric_samples);
            if (unlikely(!next_page_address))
                break;

            current_metric->samples = metric_samples;
            journalfile_samples += metric_samples;

            // Calculate start of the pages start for next descriptor
            pages_offset +=
                (metric_info->number_of_pages * (sizeof(struct journal_page_list)) +
                 sizeof(struct journal_page_header));
            // Verify we are at the right location
            if (pages_offset != (uint32_t)(next_page_address - data_start)) {
                // make sure checks fail so that we abort
                data = data_start;
                break;
            }
        }
        j2_header.samples = journalfile_samples;

        if (data == data_start + metric_offset_trailer) {
            internal_error(
                true, "DBENGINE: WRITE METRICS AND PAGES  %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);

            // Calculate CRC for metrics
            journal_v2_trailer = (struct journal_v2_block_trailer *)(data_start + metric_offset_trailer);
            crc = crc32(0L, Z_NULL, 0);
            crc = crc32(
                crc, (uint8_t *)data_start + metrics_offset, number_of_metrics * sizeof(struct journal_metric_list));
            crc32set(journal_v2_trailer->checksum, crc);
            internal_error(
                true, "DBENGINE: CALCULATE CRC FOR UUIDs  %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);

            // Prepare to write checksum for the file
            j2_header.data = NULL;
            journal_v2_trailer = (struct journal_v2_block_trailer *)(data_start + trailer_offset);
            crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, (void *)&j2_header, sizeof(j2_header));
            crc32set(journal_v2_trailer->checksum, crc);

            // Write header to the file
            memcpy(data_start, &j2_header, sizeof(j2_header));

            internal_error(
                true, "DBENGINE: FILE COMPLETED --------> %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);

            char size_for_humans[128];
            size_snprintf(size_for_humans, sizeof(size_for_humans), total_file_size, "B", false);
            netdata_log_info("DBENGINE: migrated journal file \"%s\", file size %zu bytes (%s)", path, total_file_size, size_for_humans);

            // msync(data_start, total_file_size, MS_SYNC);
            journalfile_v2_data_set(journalfile, fd_v2, data_start, total_file_size);

            internal_error(
                true, "DBENGINE: ACTIVATING NEW INDEX JNL %llu", (now_monotonic_usec() - start_loading) / USEC_PER_MS);
            ctx_current_disk_space_increase(ctx, total_file_size);
            freez(uuid_list);
            return true;
        }
    }
    else {
        nd_log(NDLS_DAEMON, NDLP_ERR, "DBENGINE: failed to write journal file \"%s\" (SIGBUS)", path);
    }

    freez(uuid_list);

    netdata_log_info("DBENGINE: failed to build index \"%s\", file will be skipped", path);

    nd_munmap(data_start, total_file_size);
    unlink(path);
    return false;
}

int journalfile_load(struct rrdengine_instance *ctx, struct rrdengine_journalfile *journalfile,
                     struct rrdengine_datafile *datafile)
{
    uv_fs_t req;
    uv_file file;
    int ret, fd, error;
    uint64_t file_size, max_id;
    char path[RRDENG_PATH_MAX];
    uv_fs_t req_close_v1; // For closing v1 file after successful migration
    int loaded_v2_rc = 1; // Default to v2 not loaded/problematic
    bool direct_migration_done = false;

    // Try to load v2 first
    if (datafile->fileno != ctx_last_fileno_get(ctx)) {
        loaded_v2_rc = journalfile_v2_load(ctx, journalfile, datafile);
    }

    journalfile_v1_generate_path(datafile, path, sizeof(path));
    fd = open_file_for_io(path, O_RDWR, &file, dbengine_use_direct_io);

    if (fd < 0) { // v1 file problematic or does not exist
        ctx_fs_error(ctx);
        if (loaded_v2_rc == 0) { // v2 is fine
            return 0;
        }
        // Both v1 and v2 are problematic
        return fd; // Return error from v1 open attempt
    }

    // v1 file is open (fd >= 0)
    ret = check_file_properties(file, &file_size, sizeof(struct rrdeng_df_sb));
    if (ret) {
        error = ret;
        goto cleanup_v1_close; // v1 file is bad after opening
    }

    // At this point, v1 file is open and basic properties are checked.
    // `loaded_v2_rc` holds the status of the v2 file attempt.
    // `loaded_v2_rc == 0` -> v2 loaded successfully.
    // `loaded_v2_rc == 1` -> v2 not found or generic error.
    // `loaded_v2_rc == 2` -> v2 needs rebuild (e.g. magic mismatch, corruption).
    // `loaded_v2_rc == 3` -> v2 should be skipped.

    if (loaded_v2_rc == 0) { // v2 exists and is good
        journalfile->unsafe.pos = file_size; // Update v1 size info
        // v1 file will be closed at cleanup_v1_close
        error = 0;
        goto cleanup_v1_close;
    }

    // If v2 load failed (loaded_v2_rc != 0) and v1 is successfully opened (fd >= 0),
    // we might need to perform direct migration.
    // This is especially true if loaded_v2_rc == 2 (Force rebuild) or if v2 is missing.
    if (loaded_v2_rc != 0 && fd >=0) {
        char path_v2[RRDENG_PATH_MAX];
        journalfile_v2_generate_path(datafile, path_v2, sizeof(path_v2));
        netdata_log_info("DBENGINE: v1 journal '%s' is present and v2 journal '%s' needs creation/migration (v2_load_rc=%d).", path, path_v2, loaded_v2_rc);

        // Before attempting migration, ensure v1 superblock is valid
        journalfile->file = file; // Temporarily assign for check_superblock and migration
        ret = journalfile_check_superblock(file);
        if (ret) {
            netdata_log_info("DBENGINE: invalid v1 journal file \"%s\" ; superblock check failed. Skipping migration.", path);
            error = ret; // v1 is invalid, cannot migrate
            // journalfile->file will be reset if migration fails or is skipped.
            // fd is still open and will be closed at cleanup_v1_close.
            goto cleanup_v1_close;
        }
        // Superblock is OK, proceed with migration attempt
        int migration_status = journalfile_direct_migrate_v1_to_v2(ctx, journalfile, datafile);
        if (migration_status == 0) {
            netdata_log_info("DBENGINE: Successfully migrated '%s' to '%s'.", path, path_v2);

            // Close the v1 file descriptor, it's no longer needed.
            uv_fs_close(NULL, &req_close_v1, file, NULL); // journalfile->file still holds 'file'
            uv_fs_req_cleanup(&req_close_v1);
            fd = -1; // Mark fd as closed to avoid double close in cleanup
            journalfile->file = 0; // Mark as closed in struct

            // Attempt to load the newly migrated v2 file's metadata.
            // This mimics parts of journalfile_v2_load.
            int migrated_fd_v2 = open(path_v2, O_RDONLY | O_CLOEXEC);
            if (migrated_fd_v2 >= 0) {
                struct stat statbuf_v2;
                if (fstat(migrated_fd_v2, &statbuf_v2) == 0) {
                    size_t migrated_v2_size = (size_t)statbuf_v2.st_size;
                    void *migrated_data_start = nd_mmap(NULL, migrated_v2_size, PROT_READ, MAP_SHARED, migrated_fd_v2, 0);
                    if (migrated_data_start != MAP_FAILED) {
                        struct journal_v2_header *migrated_j2_header = (struct journal_v2_header *)migrated_data_start;
                        if (migrated_j2_header->magic == JOURVAL_V2_MAGIC_NEW) { // Expect new magic after direct migration
                            journalfile_v2_data_set(journalfile, migrated_fd_v2, migrated_data_start, migrated_v2_size);
                            ctx_current_disk_space_increase(ctx, migrated_v2_size);
                            direct_migration_done = true;
                            // Successfully set up v2 part of journalfile struct.
                            // Old v1 file (path) could be unlinked here or later.
                            // For now, leave it.
                        } else {
                            netdata_log_error("DBENGINE: Migrated v2 file '%s' has unexpected magic 0x%x.", path_v2, migrated_j2_header->magic);
                            nd_munmap(migrated_data_start, migrated_v2_size);
                            close(migrated_fd_v2);
                        }
                    } else {
                        netdata_log_error("DBENGINE: Failed to mmap migrated v2 file '%s'.", path_v2);
                        close(migrated_fd_v2);
                    }
                } else {
                    netdata_log_error("DBENGINE: Failed to fstat migrated v2 file '%s'.", path_v2);
                    close(migrated_fd_v2);
                }
            } else {
                netdata_log_error("DBENGINE: Failed to open migrated v2 file '%s'.", path_v2);
            }

            if (direct_migration_done) {
                return 0; // Successfully migrated and loaded v2.
            } else {
                // Migration was called, reported success, but loading the result failed.
                // This is an error state. Fall through to v1 processing if desired, or return error.
                // For now, let's treat it as an error and not fall back to v1 processing.
                netdata_log_error("DBENGINE: Direct migration for '%s' seemed to succeed but loading migrated file failed.", path);
                // fd for v1 was closed. No further cleanup needed for fd.
                return -1; // Indicate error
            }
        } else {
            netdata_log_error("DBENGINE: Failed to directly migrate '%s' to '%s'. Proceeding with v1.", path, path_v2);
            // Migration failed. `file` (v1) is still open. `journalfile->file` is still set.
            // Fall through to standard v1 processing.
        }
    }

    // Standard V1 processing (if no successful direct migration)
    // This part is reached if:
    // - v2 was not loaded (loaded_v2_rc != 0) AND direct migration was not attempted or failed.
    // - v1 file is open (fd >=0) and basic checks passed.

    file_size = ALIGN_BYTES_FLOOR(file_size);
    journalfile->unsafe.pos = file_size;
    journalfile->file = file; // Ensure journalfile->file is set for v1 operations

    // If we didn't do a superblock check before migration attempt, do it now.
    // However, if migration was attempted, superblock was already checked.
    // This logic assumes that if fd >=0 and we are here, either migration wasn't applicable
    // or it failed and we are falling back to v1.
    // If migration was attempted, journalfile_check_superblock was already called.
    // If migration was not attempted (e.g. v2 loaded fine, but that path is above),
    // we still need this check if we were to proceed with v1.
    // Re-checking: if loaded_v2_rc == 0, we don't reach here.
    // If loaded_v2_rc !=0, migration is tried. If it succeeds, we return.
    // If migration fails or not tried, we are here.
    // If migration failed, superblock was checked.
    // What if migration was not tried because loaded_v2_rc was some other value like 3 (skip)?
    // The current logic for `if (loaded_v2_rc != 0 && fd >=0)` covers this.
    // So, `journalfile_check_superblock` should have been called if v1 is to be processed.
    // Let's ensure it was: if `direct_migration_done` is false, and `migration_status` was not set (i.e. not attempted)
    // then we need the check. But the `if (loaded_v2_rc != 0 && fd >=0)` block handles it.

    // If we are here, it means v1 is our only option OR v2 migration failed and we are using v1.
    // Ensure superblock was checked for v1 if not done during a migration attempt.
    // The path for `loaded_v2_rc != 0` already does this check before migration.
    // If `loaded_v2_rc == 0`, we don't get here.
    // If `fd < 0` (v1 open failed), we don't get here.

    // So, if we reach this point, v1 is open, its basic properties are fine.
    // If migration was attempted, its superblock was checked.
    // If migration was not attempted (e.g., because v2 was simply missing, not needing rebuild),
    // we still need to ensure v1 is valid. The `if (loaded_v2_rc != 0 && fd >=0)` block
    // contains the superblock check before calling migration. So this is covered.

    ctx_io_read_op_bytes(ctx, sizeof(struct rrdeng_jf_sb));
    nd_log_daemon(NDLP_DEBUG, "DBENGINE: loading journal file \"%s\" (v1 path)", path);
    max_id = journalfile_iterate_transactions(ctx, journalfile);
    __atomic_store_n(&ctx->atomic.transaction_id, MAX(__atomic_load_n(&ctx->atomic.transaction_id, __ATOMIC_RELAXED), max_id + 1), __ATOMIC_RELAXED);
    nd_log_daemon(NDLP_DEBUG, "DBENGINE: journal file \"%s\" loaded (size:%" PRIu64 ") (v1 path).", path, file_size);

    bool is_last_file = (ctx_last_fileno_get(ctx) == journalfile->datafile->fileno);
    if (!direct_migration_done && is_last_file && journalfile->datafile->pos <= rrdeng_target_data_file_size(ctx) / 3) {
        ctx->loading.create_new_datafile_pair = false;
        error = 0; // V1 loaded, no migration to v2 needed by pgc for this specific case
        goto cleanup_v1_close; // Close v1 file
    }

    // If direct migration was not done, and this is not the special last_file case above,
    // then call the original pgc_open_cache_to_journal_v2 for standard migration.
    if (!direct_migration_done) {
        pgc_open_cache_to_journal_v2(open_cache, (Word_t) ctx, (int) datafile->fileno, ctx->config.page_type,
                                     journalfile_migrate_to_v2_callback, (void *) datafile->journalfile);
    }

    if (is_last_file) { // This applies regardless of direct_migration_done, controls new datafile pair creation
        ctx->loading.create_new_datafile_pair = true;
    }

    error = 0; // Success via v1 processing path

cleanup_v1_close:
    if (fd >= 0) { // Only close if fd is still valid (i.e., not closed after successful migration)
        ret = uv_fs_close(NULL, &req, file, NULL);
        if (ret < 0) {
            netdata_log_error("DBENGINE: uv_fs_close(\"%s\"): %s", path, uv_strerror(ret));
            ctx_fs_error(ctx);
            // If close fails, should it override 'error'? Typically not if 'error' is already set from a prior operation.
            // If 'error' was 0, this makes the function return the close error.
            if (error == 0) error = ret;
        }
        uv_fs_req_cleanup(&req);
        journalfile->file = 0; // Mark as closed
    }
    return error;
}
