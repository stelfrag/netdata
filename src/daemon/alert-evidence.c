// SPDX-License-Identifier: GPL-3.0-or-later

#include "common.h"
#include "alert-evidence.h"

#ifdef ENABLE_ZSTD
#include <zstd.h>
#define AE_SUFFIX ".json.zst"
#else
#define AE_SUFFIX ".json"
#endif

#define AE_DIR_NAME         "alert-evidence"
#define AE_PREFIX           "connections-"
#define AE_TMP_SUFFIX       ".tmp"
#define AE_RAW_MAX          (64ULL * 1024 * 1024)   // the largest snapshot a reader accepts
#define AE_CONNECTIONS_CMD  "network-connections sockets:detailed"
#define AE_HELD_MAX         4                       // snapshots kept in RAM while they cannot be written
#define AE_HELD_BYTES       (16ULL * 1024 * 1024)   // ... and their total size
#define AE_LOST_MAX         16
#define AE_RETRY_EVERY_S    10

typedef enum {
    AE_WRITE_OK = 0,
    AE_WRITE_DISK_FULL,         // ENOSPC or EDQUOT, also after making room
    AE_WRITE_ERROR,
} AE_WRITE_RESULT;

static const char *ae_write_result_str(AE_WRITE_RESULT r) {
    return r == AE_WRITE_DISK_FULL ? "disk_full" : "write_error";
}

typedef struct {
    time_t t;
    BUFFER *json;
    AE_WRITE_RESULT reason;     // why its last write failed
} AE_HELD;

typedef struct {
    time_t t;
    AE_WRITE_RESULT reason;
} AE_LOST;

static struct {
    bool initialized;               // set once by alert_evidence_init(), read by the Function threads
    bool enabled;
    char dir[FILENAME_MAX + 1];
    netdata_mutex_t mutex;          // serializes the store, the hold and the lost list

    // snapshots that could not be written, oldest first, retried by alert_evidence_retry()
    AE_HELD held[AE_HELD_MAX];
    size_t held_used;
    uint64_t held_bytes;
    AE_WRITE_RESULT last_write;
    time_t last_retry_s;

    // snapshots dropped from the hold unwritten, newest last
    AE_LOST lost[AE_LOST_MAX];
    size_t lost_used;
} ae = { 0 };

// the write() the store uses; the unit test replaces it to simulate a full filesystem
static ssize_t (*ae_write_fn)(int fd, const void *buf, size_t count) = write;

const char *alert_evidence_connections_status_str(ALERT_EVIDENCE_CONNECTIONS_STATUS status) {
    switch(status) {
        case ALERT_EVIDENCE_CONNECTIONS_OK:             return "ok";
        case ALERT_EVIDENCE_CONNECTIONS_DISABLED:       return "disabled";
        case ALERT_EVIDENCE_CONNECTIONS_EXPIRED:        return "expired";
        case ALERT_EVIDENCE_CONNECTIONS_NOT_SAVED:      return "not_saved";
        default:
        case ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED:   return "not_captured";
    }
}

// --------------------------------------------------------------------------------------------------------------------
// the store: one file per snapshot, named by its capture time

typedef struct {
    time_t t;
    uint64_t size;
} AE_FILE;

static void store_filename(const char *dir, time_t t, char *dst, size_t size) {
    snprintfz(dst, size, "%s/" AE_PREFIX "%020lld" AE_SUFFIX, dir, (long long)t);
}

static int ae_file_cmp(const void *a, const void *b) {
    const AE_FILE *x = a, *y = b;
    return (x->t > y->t) - (x->t < y->t);
}

// the snapshots in the directory, oldest first; the caller frees *files
static size_t store_list(const char *dir, AE_FILE **files) {
    *files = NULL;
    DIR *d = opendir(dir);
    if(!d)
        return 0;

    size_t used = 0, size = 0;
    struct dirent *de;
    while((de = readdir(d))) {
        const char *name = de->d_name;
        if(strncmp(name, AE_PREFIX, sizeof(AE_PREFIX) - 1) != 0)
            continue;

        char *end = NULL;
        long long t = strtoll(name + sizeof(AE_PREFIX) - 1, &end, 10);
        if(!end || strcmp(end, AE_SUFFIX) != 0 || t <= 0)
            continue;

        char filename[FILENAME_MAX + 1];
        snprintfz(filename, sizeof(filename), "%s/%s", dir, name);
        struct stat st;
        if(stat(filename, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if(used == size) {
            size = size ? size * 2 : 64;
            *files = reallocz(*files, size * sizeof(**files));
        }
        (*files)[used++] = (AE_FILE){ .t = (time_t)t, .size = (uint64_t)st.st_size };
    }
    closedir(d);

    if(used)
        qsort(*files, used, sizeof(**files), ae_file_cmp);

    return used;
}

static void store_evict(const char *dir, uint64_t cap_bytes) {
    AE_FILE *files;
    size_t n = store_list(dir, &files);
    uint64_t total = 0;
    for(size_t i = 0; i < n; i++)
        total += files[i].size;

    for(size_t i = 0; i < n && total > cap_bytes; i++) {
        char filename[FILENAME_MAX + 1];
        store_filename(dir, files[i].t, filename, sizeof(filename));
        if(unlink(filename) == 0 || errno == ENOENT)
            total -= files[i].size;
    }
    freez(files);
}

static bool write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while(len) {
        ssize_t rc = ae_write_fn(fd, p, len);
        if(rc < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        p += rc;
        len -= (size_t)rc;
    }
    return true;
}

static inline bool errno_is_disk_full(int e) {
    return e == ENOSPC || e == EDQUOT;
}

// one attempt; on failure *err holds the errno of the step that failed
static bool store_write_once(const char *filename, const char *tmp, const void *data, size_t size, int *err) {
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if(fd == -1) {
        *err = errno;
        return false;
    }

    bool ok = write_all(fd, data, size);
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

// deletes our own oldest snapshots until at least `need` bytes are freed; returns how many were deleted
static size_t store_make_room(const char *dir, uint64_t need) {
    AE_FILE *files;
    size_t n = store_list(dir, &files), deleted = 0;
    uint64_t freed = 0;
    for(size_t i = 0; i < n && freed < need; i++) {
        char filename[FILENAME_MAX + 1];
        store_filename(dir, files[i].t, filename, sizeof(filename));
        if(unlink(filename) == 0) {
            freed += files[i].size;
            deleted++;
        }
    }
    freez(files);
    return deleted;
}

// make_room: delete our own oldest snapshots when the filesystem is full; once per disk-full episode, so retries
// during it delete nothing more (a process still filling the disk would take whatever we free)
static AE_WRITE_RESULT store_save(const char *dir, uint64_t cap_bytes, time_t t, const char *json, size_t len, bool make_room) {
    if(mkdir(dir, 0770) != 0 && errno != EEXIST)
        return errno_is_disk_full(errno) ? AE_WRITE_DISK_FULL : AE_WRITE_ERROR;

    const void *data = json;
    size_t size = len;
#ifdef ENABLE_ZSTD
    size_t bound = ZSTD_compressBound(len);
    void *compressed = mallocz(bound);
    size = ZSTD_compress(compressed, bound, json, len, 3);
    if(ZSTD_isError(size)) {
        freez(compressed);
        return AE_WRITE_ERROR;
    }
    data = compressed;
#endif

    char filename[FILENAME_MAX + 1], tmp[FILENAME_MAX + 1];
    store_filename(dir, t, filename, sizeof(filename));
    snprintfz(tmp, sizeof(tmp), "%s" AE_TMP_SUFFIX, filename);

    int err;
    bool ok = store_write_once(filename, tmp, data, size, &err);

    // a full filesystem: make room by deleting our own oldest snapshots, then try once more
    if(!ok && errno_is_disk_full(err) && make_room) {
        size_t deleted = store_make_room(dir, size);
        if(deleted) {
            nd_log(NDLS_DAEMON, NDLP_WARNING,
                   "ALERT EVIDENCE: the filesystem of '%s' is full; deleted the %zu oldest connection snapshots to make room",
                   dir, deleted);
            ok = store_write_once(filename, tmp, data, size, &err);
        }
    }

#ifdef ENABLE_ZSTD
    freez(compressed);
#endif

    if(!ok)
        return errno_is_disk_full(err) ? AE_WRITE_DISK_FULL : AE_WRITE_ERROR;

    store_evict(dir, cap_bytes);
    return AE_WRITE_OK;
}

static bool store_read(const char *dir, time_t t, BUFFER *dst) {
    char filename[FILENAME_MAX + 1];
    store_filename(dir, t, filename, sizeof(filename));

    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if(fd == -1)
        return false;

    struct stat st;
    if(fstat(fd, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > AE_RAW_MAX) {
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    char *data = mallocz(size);
    size_t got = 0;
    while(got < size) {
        ssize_t rc = read(fd, data + got, size - got);
        if(rc < 0 && errno == EINTR) continue;
        if(rc <= 0) break;
        got += (size_t)rc;
    }
    close(fd);

    bool ok = (got == size);
#ifdef ENABLE_ZSTD
    if(ok) {
        unsigned long long raw = ZSTD_getFrameContentSize(data, size);
        ok = (raw != ZSTD_CONTENTSIZE_ERROR && raw != ZSTD_CONTENTSIZE_UNKNOWN && raw > 0 && raw <= AE_RAW_MAX);
        if(ok) {
            buffer_flush(dst);
            buffer_need_bytes(dst, (size_t)raw + 1);
            size_t rc = ZSTD_decompress(dst->buffer, (size_t)raw, data, size);
            ok = !ZSTD_isError(rc) && rc == raw;
            dst->len = ok ? (uint32_t)rc : 0;
            buffer_need_bytes(dst, 1);
            dst->buffer[dst->len] = '\0';
        }
    }
#else
    if(ok) {
        buffer_flush(dst);
        buffer_memcat(dst, data, size);
    }
#endif

    freez(data);
    return ok;
}

static bool held_find(time_t from_s, time_t to_s, size_t *pos) {
    for(size_t i = 0; i < ae.held_used; i++) {
        if(ae.held[i].t >= from_s && ae.held[i].t <= to_s) {
            *pos = i;
            return true;
        }
    }
    return false;
}

static bool lost_find(time_t from_s, time_t to_s, size_t *pos) {
    for(size_t i = 0; i < ae.lost_used; i++) {
        if(ae.lost[i].t >= from_s && ae.lost[i].t <= to_s) {
            *pos = i;
            return true;
        }
    }
    return false;
}

// the first snapshot captured in the window after the moment, from the disk, the hold or the lost list;
// a held one is answered from RAM and *unsaved_reason says why it is not on disk
static ALERT_EVIDENCE_CONNECTIONS_STATUS store_load(const char *dir, time_t at_s, BUFFER *dst, time_t *captured_s,
                                                    const char **unsaved_reason) {
    *unsaved_reason = NULL;
    const time_t from_s = at_s - ALERT_EVIDENCE_CONNECTIONS_BEFORE_S, to_s = at_s + ALERT_EVIDENCE_CONNECTIONS_AFTER_S;

    AE_FILE *files;
    size_t n = store_list(dir, &files);

    size_t disk = n;
    for(size_t i = 0; i < n; i++) {
        if(files[i].t < from_s)
            continue;
        if(files[i].t <= to_s)
            disk = i;
        break;
    }

    size_t held, lost;
    bool have_held = held_find(from_s, to_s, &held), have_lost = lost_find(from_s, to_s, &lost);
    time_t disk_t = (disk < n) ? files[disk].t : 0;

    ALERT_EVIDENCE_CONNECTIONS_STATUS status = ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED;
    if(have_held && (!disk_t || ae.held[held].t < disk_t) && (!have_lost || ae.held[held].t < ae.lost[lost].t)) {
        buffer_flush(dst);
        buffer_fast_strcat(dst, buffer_tostring(ae.held[held].json), buffer_strlen(ae.held[held].json));
        *captured_s = ae.held[held].t;
        *unsaved_reason = ae_write_result_str(ae.held[held].reason);
        status = ALERT_EVIDENCE_CONNECTIONS_OK;
    }
    else if(have_lost && (!disk_t || ae.lost[lost].t < disk_t)) {
        *captured_s = ae.lost[lost].t;
        *unsaved_reason = ae_write_result_str(ae.lost[lost].reason);
        status = ALERT_EVIDENCE_CONNECTIONS_NOT_SAVED;
    }
    else if(disk_t && store_read(dir, disk_t, dst)) {
        // a damaged file is not evidence
        *captured_s = disk_t;
        status = ALERT_EVIDENCE_CONNECTIONS_OK;
    }

    if(status == ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED && n && to_s < files[0].t)
        status = ALERT_EVIDENCE_CONNECTIONS_EXPIRED;

    freez(files);
    return status;
}

// a snapshot that could not be written stays in RAM; a full hold loses its oldest snapshot
static void lost_add(time_t t, AE_WRITE_RESULT reason) {
    if(ae.lost_used == AE_LOST_MAX) {
        memmove(&ae.lost[0], &ae.lost[1], (AE_LOST_MAX - 1) * sizeof(ae.lost[0]));
        ae.lost_used--;
    }
    ae.lost[ae.lost_used++] = (AE_LOST){ .t = t, .reason = reason };
}

static void held_add(time_t t, const char *json, size_t len, AE_WRITE_RESULT reason) {
    ae.last_write = reason;

    // larger than the whole hold: lost by itself, without pushing out the snapshots held
    if(len > AE_HELD_BYTES) {
        lost_add(t, reason);
        return;
    }

    while(ae.held_used && (ae.held_used == AE_HELD_MAX || ae.held_bytes + len > AE_HELD_BYTES)) {
        AE_HELD *oldest = &ae.held[0];
        lost_add(oldest->t, oldest->reason);
        ae.held_bytes -= buffer_strlen(oldest->json);
        buffer_free(oldest->json);
        memmove(&ae.held[0], &ae.held[1], (AE_HELD_MAX - 1) * sizeof(ae.held[0]));
        ae.held_used--;
    }

    AE_HELD *h = &ae.held[ae.held_used++];
    h->t = t;
    h->reason = reason;
    h->json = buffer_create(len + 1, NULL);
    buffer_fast_strcat(h->json, json, len);
    ae.held_bytes += len;
}

// the held snapshots, oldest first; stops at the first failure, the disk is still full
static void held_retry(void) {
    size_t kept = 0;
    bool failed = false;
    for(size_t i = 0; i < ae.held_used; i++) {
        AE_HELD *h = &ae.held[i];
        if(!failed) {
            AE_WRITE_RESULT r = store_save(ae.dir, ALERT_EVIDENCE_CONNECTIONS_CAP_BYTES, h->t,
                                           buffer_tostring(h->json), buffer_strlen(h->json),
                                           ae.last_write != AE_WRITE_DISK_FULL);
            ae.last_write = r;
            if(r == AE_WRITE_OK) {
                ae.held_bytes -= buffer_strlen(h->json);
                buffer_free(h->json);
                continue;
            }
            h->reason = r;
            failed = true;
        }
        ae.held[kept++] = *h;
    }
    ae.held_used = kept;
}

// --------------------------------------------------------------------------------------------------------------------
// capture and load

void alert_evidence_init(void) {
    if(__atomic_load_n(&ae.initialized, __ATOMIC_ACQUIRE))
        return;

    ae.enabled = inicfg_get_boolean(&netdata_config, CONFIG_SECTION_HEALTH, "capture connections on alerts", true);
    snprintfz(ae.dir, sizeof(ae.dir), "%s/" AE_DIR_NAME, netdata_configured_cache_dir);
    netdata_mutex_init(&ae.mutex);
    __atomic_store_n(&ae.initialized, true, __ATOMIC_RELEASE);
}

struct ae_capture {
    BUFFER *wb;
    time_t t;
};

static void capture_result(BUFFER *wb __maybe_unused, int code, void *data) {
    struct ae_capture *c = data;

    // a snapshot larger than a reader accepts is not kept
    if(code == HTTP_RESP_OK && buffer_strlen(c->wb) && buffer_strlen(c->wb) <= AE_RAW_MAX) {
        netdata_mutex_lock(&ae.mutex);
        held_retry();       // older snapshots first, so the files stay in capture order
        AE_WRITE_RESULT r = ae.held_used ? ae.last_write :
            store_save(ae.dir, ALERT_EVIDENCE_CONNECTIONS_CAP_BYTES, c->t, buffer_tostring(c->wb), buffer_strlen(c->wb),
                       ae.last_write != AE_WRITE_DISK_FULL);
        if(!ae.held_used)
            ae.last_write = r;
        if(r != AE_WRITE_OK || ae.held_used)
            held_add(c->t, buffer_tostring(c->wb), buffer_strlen(c->wb), r);
        netdata_mutex_unlock(&ae.mutex);

        if(r != AE_WRITE_OK) {
            nd_log_limit_static_global_var(erl, 3600, 0);
            nd_log_limit(&erl, NDLS_DAEMON, NDLP_ERR,
                         "ALERT EVIDENCE: cannot save a connections snapshot in '%s' (%s); keeping it in RAM and retrying",
                         ae.dir, ae_write_result_str(r));
        }
    }
    else {
        nd_log_limit_static_global_var(erl, 3600, 0);
        nd_log_limit(&erl, NDLS_DAEMON, NDLP_INFO,
                     "ALERT EVIDENCE: network-connections answered %d (is network-viewer.plugin running?)", code);
    }

    buffer_free(c->wb);
    freez(c);
}

void alert_evidence_capture_connections(void) {
    if(!__atomic_load_n(&ae.initialized, __ATOMIC_ACQUIRE) || !ae.enabled)
        return;

    struct ae_capture *c = callocz(1, sizeof(*c));
    c->wb = buffer_create(0, NULL);
    c->t = now_realtime_sec();

    nrpc_call(&(struct nrpc_call_spec) {
        .owner = rrdhost_nrpc_owner(localhost),
        .result_wb = c->wb,
        .cmd = AE_CONNECTIONS_CMD,
        .source = "alert-evidence",
        .user_access = HTTP_ACCESS_ALL,
        .timeout_s = 30,
        .wait = false,
        .allow_restricted = false,
        .result.cb = capture_result,
        .result.data = c,
    });
}

ALERT_EVIDENCE_CONNECTIONS_STATUS alert_evidence_connections_load(time_t at_s, BUFFER *dst, time_t *captured_s,
                                                                   const char **unsaved_reason) {
    *unsaved_reason = NULL;
    if(!__atomic_load_n(&ae.initialized, __ATOMIC_ACQUIRE) || !ae.enabled)
        return ALERT_EVIDENCE_CONNECTIONS_DISABLED;

    netdata_mutex_lock(&ae.mutex);
    ALERT_EVIDENCE_CONNECTIONS_STATUS status = store_load(ae.dir, at_s, dst, captured_s, unsaved_reason);
    netdata_mutex_unlock(&ae.mutex);
    return status;
}

void alert_evidence_retry(void) {
    if(!__atomic_load_n(&ae.initialized, __ATOMIC_ACQUIRE) || !ae.enabled)
        return;

    time_t now_s = now_realtime_sec();
    netdata_mutex_lock(&ae.mutex);
    if(ae.held_used && now_s - ae.last_retry_s >= AE_RETRY_EVERY_S) {
        ae.last_retry_s = now_s;
        held_retry();
    }
    netdata_mutex_unlock(&ae.mutex);
}

// --------------------------------------------------------------------------------------------------------------------
// unit test

// fails the next n writes with ENOSPC (-1: all of them), then writes normally
static int ae_fake_fail_writes = 0;
static ssize_t ae_fake_write(int fd, const void *buf, size_t count) {
    if(ae_fake_fail_writes) {
        if(ae_fake_fail_writes > 0) ae_fake_fail_writes--;
        errno = ENOSPC;
        return -1;
    }
    return write(fd, buf, count);
}

static void ae_fake_capture(time_t t, const char *json) {
    struct ae_capture *c = callocz(1, sizeof(*c));
    c->wb = buffer_create(0, NULL);
    buffer_strcat(c->wb, json);
    c->t = t;
    capture_result(NULL, HTTP_RESP_OK, c);
}

static size_t ae_files_count(const char *dir, time_t *oldest) {
    AE_FILE *files;
    size_t n = store_list(dir, &files);
    *oldest = n ? files[0].t : 0;
    freez(files);
    return n;
}

static int ae_check(bool ok, const char *what) {
    fprintf(stderr, "  alert evidence store (%s): %s\n", what, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

int alert_evidence_unittest(void) {
    int errors = 0;
    char dir[] = "/tmp/netdata-alert-evidence-test-XXXXXX";
    if(!mkdtemp(dir))
        return ae_check(false, "temporary directory");

    // pseudo-random letters, so the first snapshot stays larger than the small cap below even when compressed
    char json[4096];
    uint32_t seed = 12345;
    for(size_t i = 0; i < sizeof(json) - 1; i++) {
        seed = seed * 1103515245 + 12345;
        json[i] = (char)('a' + ((seed >> 16) % 26));
    }
    json[sizeof(json) - 1] = '\0';

    errors += ae_check(store_save(dir, 1ULL << 30, 1000, json, strlen(json), true) == AE_WRITE_OK, "save 1000");
    errors += ae_check(store_save(dir, 1ULL << 30, 1100, "{\"b\":1}", 7, true) == AE_WRITE_OK, "save 1100");
    errors += ae_check(store_save(dir, 1ULL << 30, 1400, "{\"c\":1}", 7, true) == AE_WRITE_OK, "save 1400");

    BUFFER *b = buffer_create(0, NULL);
    time_t captured = 0;
    const char *reason;

    errors += ae_check(store_load(dir, 995, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1000 &&
                       !strcmp(buffer_tostring(b), json), "round trip, first after the moment");
    errors += ae_check(store_load(dir, 1095, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1100 &&
                       !strcmp(buffer_tostring(b), "{\"b\":1}"), "the nearest one after");
    errors += ae_check(store_load(dir, 1250, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1400,
                       "within the window after");
    errors += ae_check(store_load(dir, 2000, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED, "too late");
    errors += ae_check(store_load(dir, 500, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_EXPIRED, "older than kept");

    // a cap that holds the two small snapshots only: the large, oldest one goes
    errors += ae_check(store_save(dir, 200, 1500, "{\"d\":1}", 7, true) == AE_WRITE_OK, "save under a small cap");
    AE_FILE *files;
    size_t n = store_list(dir, &files);
    errors += ae_check(n >= 1 && files[0].t > 1000, "oldest evicted over the cap");
    freez(files);

    // a damaged file is not evidence
    char filename[FILENAME_MAX + 1];
    store_filename(dir, 1600, filename, sizeof(filename));
    FILE *fp = fopen(filename, "w");
    if(fp) { fputs("garbage", fp); fclose(fp); }
#ifdef ENABLE_ZSTD
    errors += ae_check(store_load(dir, 1595, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED, "damaged file");
#endif

    // a full filesystem: the store deletes its own oldest snapshot, and the second attempt succeeds
    unlink(filename);
    time_t oldest;
    size_t before = ae_files_count(dir, &oldest);
    ae_write_fn = ae_fake_write;
    ae_fake_fail_writes = 1;
    time_t evicted = oldest;
    errors += ae_check(store_save(dir, 1ULL << 30, 1700, "{\"e\":1}", 7, true) == AE_WRITE_OK, "disk full: made room");
    errors += ae_check(ae_files_count(dir, &oldest) == before && oldest > evicted, "disk full: oldest deleted");

    // still full after making room: the capture stays in RAM and answers from there
    strncpyz(ae.dir, dir, sizeof(ae.dir) - 1);
    netdata_mutex_init(&ae.mutex);
    ae.enabled = true;
    ae_fake_fail_writes = -1;
    size_t files_before = ae_files_count(dir, &oldest);
    ae_fake_capture(2000, "{\"f\":1}");
    size_t files_after = ae_files_count(dir, &oldest);
    errors += ae_check(ae.held_used == 1 && files_after == files_before - 1, "disk full: held in RAM, room made once");
    errors += ae_check(store_load(dir, 1995, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 2000 &&
                       reason && !strcmp(reason, "disk_full") && !strcmp(buffer_tostring(b), "{\"f\":1}"),
                       "disk full: answered from RAM");

    // more captures than the hold keeps: the oldest is lost, and says why
    for(time_t t = 2100; t < 2100 + AE_HELD_MAX * 100; t += 100)
        ae_fake_capture(t, "{\"g\":1}");
    errors += ae_check(ae.held_used == AE_HELD_MAX && ae.lost_used == 1, "disk full: hold bounded");
    errors += ae_check(ae_files_count(dir, &oldest) == files_after, "disk full: retries delete nothing more");
    errors += ae_check(store_load(dir, 1995, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_NOT_SAVED &&
                       captured == 2000 && reason && !strcmp(reason, "disk_full"), "disk full: lost reported");

    // a snapshot larger than the whole hold is lost by itself; the held ones stay
    size_t big_len = AE_HELD_BYTES + 1;
    char *big = mallocz(big_len + 1);
    memset(big, 'x', big_len);
    big[big_len] = '\0';
    ae_fake_capture(2600, big);
    freez(big);
    errors += ae_check(ae.held_used == AE_HELD_MAX && ae.lost_used == 2 && ae.lost[1].t == 2600,
                       "disk full: oversized snapshot lost alone");

    // space comes back: the held snapshots are written, oldest first, and answer from disk
    ae_fake_fail_writes = 0;
    held_retry();
    errors += ae_check(ae.held_used == 0 && ae_files_count(dir, &oldest) == files_after + AE_HELD_MAX &&
                       ae.last_write == AE_WRITE_OK, "space back: held written");
    errors += ae_check(store_load(dir, 2095, b, &captured, &reason) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 2100 &&
                       !reason, "space back: answered from disk");

    ae_write_fn = write;
    ae.lost_used = 0;
    ae.enabled = false;
    netdata_mutex_destroy(&ae.mutex);
    buffer_free(b);

    n = store_list(dir, &files);
    for(size_t i = 0; i < n; i++) {
        store_filename(dir, files[i].t, filename, sizeof(filename));
        unlink(filename);
    }
    freez(files);
    rmdir(dir);

    return errors;
}
