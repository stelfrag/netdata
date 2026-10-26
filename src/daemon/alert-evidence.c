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

static struct {
    bool initialized;               // set once by alert_evidence_init(), read by the Function threads
    bool enabled;
    char dir[FILENAME_MAX + 1];
    netdata_mutex_t mutex;          // serializes the store between the capture callback and the readers
} ae = { 0 };

const char *alert_evidence_connections_status_str(ALERT_EVIDENCE_CONNECTIONS_STATUS status) {
    switch(status) {
        case ALERT_EVIDENCE_CONNECTIONS_OK:             return "ok";
        case ALERT_EVIDENCE_CONNECTIONS_DISABLED:       return "disabled";
        case ALERT_EVIDENCE_CONNECTIONS_EXPIRED:        return "expired";
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
        ssize_t rc = write(fd, p, len);
        if(rc < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        p += rc;
        len -= (size_t)rc;
    }
    return true;
}

static bool store_save(const char *dir, uint64_t cap_bytes, time_t t, const char *json, size_t len) {
    if(mkdir(dir, 0770) != 0 && errno != EEXIST)
        return false;

    const void *data = json;
    size_t size = len;
#ifdef ENABLE_ZSTD
    size_t bound = ZSTD_compressBound(len);
    void *compressed = mallocz(bound);
    size = ZSTD_compress(compressed, bound, json, len, 3);
    if(ZSTD_isError(size)) {
        freez(compressed);
        return false;
    }
    data = compressed;
#endif

    char filename[FILENAME_MAX + 1], tmp[FILENAME_MAX + 1];
    store_filename(dir, t, filename, sizeof(filename));
    snprintfz(tmp, sizeof(tmp), "%s" AE_TMP_SUFFIX, filename);

    bool ok = false;
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if(fd != -1) {
        ok = write_all(fd, data, size);
        ok = (close(fd) == 0) && ok;
        if(!ok || rename(tmp, filename) != 0) {
            unlink(tmp);
            ok = false;
        }
    }

#ifdef ENABLE_ZSTD
    freez(compressed);
#endif

    if(ok)
        store_evict(dir, cap_bytes);

    return ok;
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

static ALERT_EVIDENCE_CONNECTIONS_STATUS store_load(const char *dir, time_t at_s, BUFFER *dst, time_t *captured_s) {
    AE_FILE *files;
    size_t n = store_list(dir, &files);

    ALERT_EVIDENCE_CONNECTIONS_STATUS status = ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED;
    for(size_t i = 0; i < n; i++) {
        if(files[i].t < at_s - ALERT_EVIDENCE_CONNECTIONS_BEFORE_S)
            continue;

        if(files[i].t > at_s + ALERT_EVIDENCE_CONNECTIONS_AFTER_S)
            break;

        // the first one captured after the moment; a damaged file is not evidence
        if(store_read(dir, files[i].t, dst)) {
            *captured_s = files[i].t;
            status = ALERT_EVIDENCE_CONNECTIONS_OK;
        }
        break;
    }

    if(status != ALERT_EVIDENCE_CONNECTIONS_OK && n && at_s + ALERT_EVIDENCE_CONNECTIONS_AFTER_S < files[0].t)
        status = ALERT_EVIDENCE_CONNECTIONS_EXPIRED;

    freez(files);
    return status;
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
        bool ok = store_save(ae.dir, ALERT_EVIDENCE_CONNECTIONS_CAP_BYTES, c->t, buffer_tostring(c->wb), buffer_strlen(c->wb));
        netdata_mutex_unlock(&ae.mutex);

        if(!ok) {
            nd_log_limit_static_global_var(erl, 3600, 0);
            nd_log_limit(&erl, NDLS_DAEMON, NDLP_ERR, "ALERT EVIDENCE: cannot save a connections snapshot in '%s'", ae.dir);
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

ALERT_EVIDENCE_CONNECTIONS_STATUS alert_evidence_connections_load(time_t at_s, BUFFER *dst, time_t *captured_s) {
    if(!__atomic_load_n(&ae.initialized, __ATOMIC_ACQUIRE) || !ae.enabled)
        return ALERT_EVIDENCE_CONNECTIONS_DISABLED;

    netdata_mutex_lock(&ae.mutex);
    ALERT_EVIDENCE_CONNECTIONS_STATUS status = store_load(ae.dir, at_s, dst, captured_s);
    netdata_mutex_unlock(&ae.mutex);
    return status;
}

// --------------------------------------------------------------------------------------------------------------------
// unit test

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

    errors += ae_check(store_save(dir, 1ULL << 30, 1000, json, strlen(json)), "save 1000");
    errors += ae_check(store_save(dir, 1ULL << 30, 1100, "{\"b\":1}", 7), "save 1100");
    errors += ae_check(store_save(dir, 1ULL << 30, 1400, "{\"c\":1}", 7), "save 1400");

    BUFFER *b = buffer_create(0, NULL);
    time_t captured = 0;

    errors += ae_check(store_load(dir, 995, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1000 &&
                       !strcmp(buffer_tostring(b), json), "round trip, first after the moment");
    errors += ae_check(store_load(dir, 1095, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1100 &&
                       !strcmp(buffer_tostring(b), "{\"b\":1}"), "the nearest one after");
    errors += ae_check(store_load(dir, 1250, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_OK && captured == 1400,
                       "within the window after");
    errors += ae_check(store_load(dir, 2000, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED, "too late");
    errors += ae_check(store_load(dir, 500, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_EXPIRED, "older than kept");

    // a cap that holds the two small snapshots only: the large, oldest one goes
    errors += ae_check(store_save(dir, 200, 1500, "{\"d\":1}", 7), "save under a small cap");
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
    errors += ae_check(store_load(dir, 1595, b, &captured) == ALERT_EVIDENCE_CONNECTIONS_NOT_CAPTURED, "damaged file");
#endif

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
