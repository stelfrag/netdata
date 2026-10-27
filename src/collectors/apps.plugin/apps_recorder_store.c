// SPDX-License-Identifier: GPL-3.0-or-later

#include "apps_recorder_store.h"

#define APPS_REC_STORE_TMP_SUFFIX ".tmp"

const char *apps_rec_store_result_str(APPS_REC_STORE_RESULT r) {
    switch(r) {
        case APPS_REC_STORE_OK:         return "ok";
        case APPS_REC_STORE_DISK_FULL:  return "disk_full";
        default:
        case APPS_REC_STORE_ERROR:      return "write_error";
    }
}

static void store_filename(const APPS_REC_STORE *st, uint64_t first_ut, char *dst, size_t size) {
    snprintfz(dst, size, "%s/%020" PRIu64 APPS_REC_STORE_SUFFIX, st->dir, first_ut);
}

static size_t store_find(const APPS_REC_STORE *st, uint64_t first_ut, bool *found) {
    size_t lo = 0, hi = st->used;
    while(lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if(st->items[mid].first_ut < first_ut) lo = mid + 1;
        else hi = mid;
    }
    *found = (lo < st->used && st->items[lo].first_ut == first_ut);
    return lo;
}

static void store_index_add(APPS_REC_STORE *st, const APPS_REC_STORE_ITEM *item) {
    bool found;
    size_t pos = store_find(st, item->first_ut, &found);
    if(found) {
        st->bytes -= st->items[pos].size;
        st->items[pos] = *item;
        st->bytes += item->size;
        return;
    }

    if(st->used == st->size) {
        st->size = st->size ? st->size * 2 : 64;
        st->items = reallocz(st->items, st->size * sizeof(*st->items));
    }
    memmove(&st->items[pos + 1], &st->items[pos], (st->used - pos) * sizeof(*st->items));
    st->items[pos] = *item;
    st->used++;
    st->bytes += item->size;
}

static void store_index_del(APPS_REC_STORE *st, size_t pos) {
    st->bytes -= st->items[pos].size;
    memmove(&st->items[pos], &st->items[pos + 1], (st->used - pos - 1) * sizeof(*st->items));
    st->used--;
}

static void store_delete(APPS_REC_STORE *st, size_t pos) {
    char filename[FILENAME_MAX + 1];
    store_filename(st, st->items[pos].first_ut, filename, sizeof(filename));
    if(unlink(filename) != 0 && errno != ENOENT)
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: cannot delete '%s'", filename);
    store_index_del(st, pos);
}

static void store_evict(APPS_REC_STORE *st) {
    while(st->used && st->bytes > st->cap_bytes)
        store_delete(st, 0);
}

static bool name_has_suffix(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

// indexes one file by its header; false when it is not a chunk this version can read
static bool store_index_file(APPS_REC_STORE *st, const char *path, bool *other_format) {
    *other_format = false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd == -1)
        return false;

    APPS_REC_CHUNK_HDR hdr;
    struct stat stbuf;
    bool ok = fstat(fd, &stbuf) == 0 && read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
              hdr.magic == APPS_REC_MAGIC;
    *other_format = ok && hdr.version != APPS_REC_VERSION;
    ok = ok && hdr.version == APPS_REC_VERSION &&
              (uint64_t)stbuf.st_size == sizeof(hdr) + (uint64_t)hdr.stored_len && hdr.first_ut <= hdr.last_ut;
    close(fd);

    if(ok) {
        char expected[FILENAME_MAX + 1];
        store_filename(st, hdr.first_ut, expected, sizeof(expected));
        ok = strcmp(expected, path) == 0;
    }

    if(ok)
        store_index_add(st, &(APPS_REC_STORE_ITEM){
            .first_ut = hdr.first_ut, .last_ut = hdr.last_ut, .size = (uint64_t)stbuf.st_size });

    return ok;
}

bool apps_rec_store_open(APPS_REC_STORE *st, const char *dir, uint64_t cap_bytes) {
    memset(st, 0, sizeof(*st));
    if(!dir || !*dir || !cap_bytes)
        return false;

    strncpyz(st->dir, dir, sizeof(st->dir) - 1);
    st->cap_bytes = cap_bytes;
    st->write_fn = write;

    if(mkdir(st->dir, 0770) != 0 && errno != EEXIST) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: cannot create directory '%s'", st->dir);
        return false;
    }

    DIR *d = opendir(st->dir);
    if(!d) {
        nd_log(NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: cannot open directory '%s'", st->dir);
        return false;
    }

    size_t other_formats = 0;
    struct dirent *de;
    while((de = readdir(d))) {
        if(de->d_name[0] == '.')
            continue;

        char path[FILENAME_MAX + 1];
        snprintfz(path, sizeof(path), "%s/%s", st->dir, de->d_name);

        // leftovers of interrupted writes, and chunks this version cannot read: the store is a cache
        bool other_format = false;
        if(name_has_suffix(de->d_name, APPS_REC_STORE_TMP_SUFFIX) ||
            (name_has_suffix(de->d_name, APPS_REC_STORE_SUFFIX) && !store_index_file(st, path, &other_format))) {
            if(other_format)
                other_formats++;
            if(unlink(path) != 0)
                nd_log(NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: cannot delete '%s'", path);
        }
    }
    closedir(d);

    if(other_formats)
        nd_log(NDLS_COLLECTORS, NDLP_NOTICE,
               "apps.plugin recorder: deleted %zu saved chunks of another record format from '%s'",
               other_formats, st->dir);

    store_evict(st);
    return true;
}

void apps_rec_store_close(APPS_REC_STORE *st) {
    freez(st->items);
    memset(st, 0, sizeof(*st));
}

bool apps_rec_store_has(const APPS_REC_STORE *st, uint64_t first_ut) {
    bool found;
    store_find(st, first_ut, &found);
    return found;
}

static bool write_all(APPS_REC_STORE *st, int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while(len) {
        ssize_t w = st->write_fn(fd, p, len);
        if(w < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

static inline bool errno_is_disk_full(int e) {
    return e == ENOSPC || e == EDQUOT;
}

// one attempt; on failure *err holds the errno of the step that failed
static bool store_write_once(APPS_REC_STORE *st, const APPS_REC_CHUNK_HDR *hdr, size_t size,
                             const char *filename, const char *tmp, int *err) {
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if(fd == -1) {
        *err = errno;
        return false;
    }

    bool ok = write_all(st, fd, hdr, size);
    *err = ok ? 0 : errno;
    if(close(fd) != 0 && ok) {
        ok = false;
        *err = errno;
    }

    if(ok && rename(tmp, filename) != 0) {
        ok = false;
        *err = errno;
    }

    if(!ok)
        unlink(tmp);

    return ok;
}

APPS_REC_STORE_RESULT apps_rec_store_write(APPS_REC_STORE *st, const APPS_REC_CHUNK_HDR *hdr, size_t size,
                                           uint64_t keep_from_ut) {
    if(!st->cap_bytes || size > st->cap_bytes)
        return APPS_REC_STORE_ERROR;

    char filename[FILENAME_MAX + 1], tmp[FILENAME_MAX + 1];
    store_filename(st, hdr->first_ut, filename, sizeof(filename));
    snprintfz(tmp, sizeof(tmp), "%s" APPS_REC_STORE_TMP_SUFFIX, filename);

    int err;
    bool ok = store_write_once(st, hdr, size, filename, tmp, &err);

    // a full filesystem: make room by deleting our own oldest chunks, then try once more (once per episode)
    if(!ok && errno_is_disk_full(err) && !st->disk_full) {
        uint64_t freed = 0;
        size_t deleted = 0;
        while(st->used && freed < size && st->items[0].first_ut < keep_from_ut) {
            freed += st->items[0].size;
            store_delete(st, 0);
            deleted++;
        }

        if(deleted) {
            nd_log(NDLS_COLLECTORS, NDLP_WARNING,
                   "apps.plugin recorder: the filesystem of '%s' is full; deleted the %zu oldest saved chunks to make room",
                   st->dir, deleted);

            ok = store_write_once(st, hdr, size, filename, tmp, &err);
        }
    }

    // the episode ends with a successful write; another kind of failure does not end it
    if(ok) st->disk_full = false;
    else if(errno_is_disk_full(err)) st->disk_full = true;
    if(!ok) {
        nd_log_limit_static_global_var(erl, 60, 0);
        nd_log_limit(&erl, NDLS_COLLECTORS, NDLP_ERR, "apps.plugin recorder: cannot write '%s': %s",
                     filename, strerror(err));
        return errno_is_disk_full(err) ? APPS_REC_STORE_DISK_FULL : APPS_REC_STORE_ERROR;
    }

    store_index_add(st, &(APPS_REC_STORE_ITEM){ .first_ut = hdr->first_ut, .last_ut = hdr->last_ut, .size = size });
    store_evict(st);
    return APPS_REC_STORE_OK;
}

APPS_REC_CHUNK_HDR *apps_rec_store_read(APPS_REC_STORE *st, uint64_t first_ut, size_t *size) {
    *size = 0;

    bool found;
    size_t pos = store_find(st, first_ut, &found);
    if(!found)
        return NULL;

    char filename[FILENAME_MAX + 1];
    store_filename(st, first_ut, filename, sizeof(filename));

    size_t expected = st->items[pos].size;
    APPS_REC_CHUNK_HDR *hdr = mallocz(expected);
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    ssize_t got = (fd == -1) ? -1 : read(fd, hdr, expected);
    if(fd != -1) close(fd);

    if(got != (ssize_t)expected || !apps_rec_chunk_valid(hdr, expected)) {
        nd_log(NDLS_COLLECTORS, NDLP_WARNING, "apps.plugin recorder: dropping invalid chunk '%s'", filename);
        freez(hdr);
        store_delete(st, pos);
        return NULL;
    }

    *size = expected;
    return hdr;
}
