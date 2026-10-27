// SPDX-License-Identifier: GPL-3.0-or-later

// The flight recorder: every iteration, the processes that hold most of the CPU, the largest by memory and by I/O,
// and the processes that started or exited, become a frame (apps_recorder_format.h). Frames are packed in chunks
// of APPS_REC_FRAMES_PER_CHUNK and the last APPS_REC_RING_CHUNKS sealed chunks stay in RAM, so the recent past of
// the process table survives the processes themselves.
//
// A save request (APPS_RECORDER_SAVE_FUNCTION, called only by the daemon) writes the chunks around a moment to the
// disk store (apps_recorder_store.h); the part of the window still in the future is written as its chunks seal.
// A chunk that cannot be written (a full disk) stays wanted: every seal tries the unwritten chunks again, and when
// the ring needs its slot it moves to a hold of the same size, so a full disk loses nothing until the hold is full
// too. Queries report such windows as not saved, with the reason.
//
// All functions here run under apps_and_stdout_mutex: the collection loop and the Function workers hold it.

#include "apps_recorder.h"
#include "apps_recorder_store.h"

#if defined(OS_LINUX)

#define APPS_REC_RING_CHUNKS        30
#define APPS_REC_CPU_COVERAGE       0.95
#define APPS_REC_CPU_ROWS_MIN       50
#define APPS_REC_CPU_ROWS_MAX       150
#define APPS_REC_MEM_ROWS           10
#define APPS_REC_IO_ROWS            10
#define APPS_REC_ROWS_MAX           (APPS_REC_CPU_ROWS_MAX + APPS_REC_MEM_ROWS + APPS_REC_IO_ROWS)
#define APPS_REC_PENDING_MAX        16
#define APPS_REC_HELD_MAX           APPS_REC_RING_CHUNKS
#define APPS_REC_LOST_MAX           16

bool apps_recorder_enabled = true;
bool apps_recorder_record_cmdline = false;
int apps_recorder_disk_mb = APPS_RECORDER_DISK_MB_DEFAULT;

typedef struct {
    int32_t pid;
    uint64_t starttime;
    struct pid_stat *p;     // valid only during the iteration that collected it
} REC_PID;

typedef struct {
    APPS_REC_CHUNK_HDR *hdr;
    size_t size;
    bool wanted;            // a saved window covers it
    bool stored;            // already in the disk store
    APPS_REC_STORE_RESULT failure;  // why the last attempt to write it failed
} REC_SEALED;

typedef struct {
    usec_t from_ut;
    usec_t to_ut;
} REC_WINDOW;

typedef struct {
    usec_t from_ut;
    usec_t to_ut;
    APPS_REC_STORE_RESULT reason;
} REC_LOST;

static struct {
    bool initialized;
    uint8_t boot_id[16];
    APPS_REC_BUILDER builder;

    REC_SEALED ring[APPS_REC_RING_CHUNKS];
    size_t ring_next;           // the slot the next sealed chunk goes to (the oldest when the ring is full)

    APPS_REC_STORE store;
    bool store_open;
    APPS_REC_STORE_RESULT last_failure; // why the newest failed write failed

    // wanted chunks that could not be written and left the ring, oldest first
    REC_SEALED held[APPS_REC_HELD_MAX];
    size_t held_used;

    // wanted chunks dropped from the hold unwritten, newest last
    REC_LOST lost[APPS_REC_LOST_MAX];
    size_t lost_used;

    // saved windows that end after the newest sealed chunk
    REC_WINDOW pending[APPS_REC_PENDING_MAX];
    size_t pending_used;

    REC_PID *prev, *cur;
    size_t prev_used, prev_size, cur_size;
    bool have_prev;

    struct pid_stat **sorted;
    size_t sorted_size;

    APPS_REC_START *starts;
    size_t starts_size;
    APPS_REC_EXIT *exits;
    size_t exits_size;
    APPS_REC_ROW rows[APPS_REC_ROWS_MAX];

    // the last frame, for the self-monitoring charts
    struct {
        uint16_t rows, starts, exits;
        uint32_t cpu_coverage;  // 1/100 of a percent
    } last;
} rec = { 0 };

// --------------------------------------------------------------------------------------------------------------------

static void read_boot_id(uint8_t boot_id[16]) {
    char filename[FILENAME_MAX + 1];
    snprintfz(filename, FILENAME_MAX, "%s/proc/sys/kernel/random/boot_id", netdata_configured_host_prefix);

    char buf[UUID_STR_LEN + 1] = "";
    if(read_txt_file(filename, buf, sizeof(buf)) == 0) {
        const char *s = trim(buf);
        nd_uuid_t uuid;
        if(s && uuid_parse(s, uuid) == 0) {
            memcpy(boot_id, uuid, 16);
            return;
        }
    }

    memset(boot_id, 0, 16);
}

static STRING *pid_exe(struct pid_stat *p) {
    if(p->exe_starttime == p->starttime && p->exe_starttime)
        return p->exe;

    string_freez(p->exe);
    p->exe = NULL;
    p->exe_starttime = p->starttime;

    char filename[FILENAME_MAX + 1];
    snprintfz(filename, FILENAME_MAX, "%s/proc/%d/exe", netdata_configured_host_prefix, p->pid);

    char target[FILENAME_MAX + 1];
    ssize_t len = readlink(filename, target, FILENAME_MAX);
    if(len > 0) {
        target[len] = '\0';
        p->exe = string_strdupz(target);
    }

    return p->exe;
}

static inline kernel_uint_t pid_cpu(struct pid_stat *p) {
    return p->values[PDF_UTIME] + p->values[PDF_STIME] + p->values[PDF_GTIME] +
           p->values[PDF_CUTIME] + p->values[PDF_CSTIME] + p->values[PDF_CGTIME];
}

static inline kernel_uint_t pid_io(struct pid_stat *p) {
    return p->values[PDF_LREAD] + p->values[PDF_LWRITE];
}

// nanosecondcores to 1/10000 of a core
static inline uint32_t cpu_permyriad(kernel_uint_t v) {
    uint64_t r = v / (NSEC_PER_SEC / 10000);
    return (uint32_t)MIN(r, UINT32_MAX);
}

static inline uint32_t kib(kernel_uint_t bytes) {
    uint64_t r = bytes / 1024;
    return (uint32_t)MIN(r, UINT32_MAX);
}

// RATES_DETAIL bytes/s to KiB/s
static inline uint32_t kibs(kernel_uint_t rate) {
    uint64_t r = rate / RATES_DETAIL / 1024;
    return (uint32_t)MIN(r, UINT32_MAX);
}

static int cmp_cpu_desc(const void *a, const void *b) {
    kernel_uint_t x = pid_cpu(*(struct pid_stat * const *)a), y = pid_cpu(*(struct pid_stat * const *)b);
    return (x < y) - (x > y);
}

static int cmp_rec_pid(const void *a, const void *b) {
    const REC_PID *x = a, *y = b;
    if(x->pid != y->pid) return (x->pid > y->pid) - (x->pid < y->pid);
    return (x->starttime > y->starttime) - (x->starttime < y->starttime);
}

static void ensure(void **ptr, size_t *size, size_t needed, size_t item) {
    if(needed <= *size) return;
    *size = needed * 2;
    *ptr = reallocz(*ptr, *size * item);
}

// --------------------------------------------------------------------------------------------------------------------

static inline bool chunk_overlaps(const APPS_REC_CHUNK_HDR *hdr, usec_t from_ut, usec_t to_ut) {
    return hdr->first_ut <= to_ut && hdr->last_ut >= from_ut;
}

// making room on a full disk must not delete what is still in RAM: it would only be written again (or reported
// saved while it is not)
static uint64_t oldest_in_ram_ut(void) {
    uint64_t oldest = UINT64_MAX;
    for(size_t i = 0; i < rec.held_used; i++)
        oldest = MIN(oldest, rec.held[i].hdr->first_ut);
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++)
        if(rec.ring[i].hdr)
            oldest = MIN(oldest, rec.ring[i].hdr->first_ut);
    return oldest;
}

static bool persist_slot(REC_SEALED *slot) {
    slot->wanted = true;
    if(slot->stored)
        return true;

    if(!rec.store_open) {
        slot->failure = APPS_REC_STORE_ERROR;
        return false;
    }

    if(apps_rec_store_has(&rec.store, slot->hdr->first_ut))
        slot->stored = true;
    else {
        APPS_REC_STORE_RESULT r = apps_rec_store_write(&rec.store, slot->hdr, slot->size, oldest_in_ram_ut());
        slot->stored = (r == APPS_REC_STORE_OK);
        if(!slot->stored)
            slot->failure = rec.last_failure = r;
    }

    return slot->stored;
}

static inline bool slot_unwritten(const REC_SEALED *slot) {
    return slot->hdr && slot->wanted && !slot->stored;
}

// the unwritten chunks, oldest first: the hold, then the ring; stops at the first failure, the disk is still full
static void retry_unwritten(void) {
    if(!rec.store_open)
        return;

    size_t kept = 0;
    bool failed = false;
    for(size_t i = 0; i < rec.held_used; i++) {
        REC_SEALED *h = &rec.held[i];
        if(!failed && persist_slot(h)) {
            freez(h->hdr);
            continue;
        }
        failed = true;
        rec.held[kept++] = *h;
    }
    rec.held_used = kept;

    for(size_t k = 0; k < APPS_REC_RING_CHUNKS && !failed; k++) {
        REC_SEALED *slot = &rec.ring[(rec.ring_next + k) % APPS_REC_RING_CHUNKS];
        if(slot_unwritten(slot) && !persist_slot(slot))
            failed = true;
    }
}

static void lost_add(const REC_SEALED *slot) {
    if(rec.lost_used && rec.lost[rec.lost_used - 1].to_ut >= slot->hdr->first_ut - (usec_t)update_every * 2 * USEC_PER_SEC &&
       rec.lost[rec.lost_used - 1].reason == slot->failure) {
        rec.lost[rec.lost_used - 1].to_ut = MAX(rec.lost[rec.lost_used - 1].to_ut, slot->hdr->last_ut);
        return;
    }

    if(rec.lost_used == APPS_REC_LOST_MAX) {
        memmove(&rec.lost[0], &rec.lost[1], (APPS_REC_LOST_MAX - 1) * sizeof(rec.lost[0]));
        rec.lost_used--;
    }
    rec.lost[rec.lost_used++] = (REC_LOST){
        .from_ut = slot->hdr->first_ut, .to_ut = slot->hdr->last_ut, .reason = slot->failure };
}

// an unwritten chunk leaving the ring goes to the hold; when the hold is full, its oldest chunk is lost
static void hold_slot(REC_SEALED *slot) {
    if(rec.held_used == APPS_REC_HELD_MAX) {
        REC_SEALED *oldest = &rec.held[0];
        lost_add(oldest);
        nd_log_limit_static_global_var(erl, 60, 0);
        nd_log_limit(&erl, NDLS_COLLECTORS, NDLP_ERR,
                     "apps.plugin recorder: dropping saved process history from %llu to %llu that could not be written (%s)",
                     (unsigned long long)(oldest->hdr->first_ut / USEC_PER_SEC),
                     (unsigned long long)(oldest->hdr->last_ut / USEC_PER_SEC),
                     apps_rec_store_result_str(oldest->failure));
        freez(oldest->hdr);
        memmove(&rec.held[0], &rec.held[1], (APPS_REC_HELD_MAX - 1) * sizeof(rec.held[0]));
        rec.held_used--;
    }
    rec.held[rec.held_used++] = *slot;
    slot->hdr = NULL;
}

// a newly sealed chunk is written when a pending window covers it; windows it completes are dropped
static void pending_windows_on_seal(REC_SEALED *slot) {
    size_t kept = 0;
    for(size_t i = 0; i < rec.pending_used; i++) {
        REC_WINDOW *w = &rec.pending[i];
        if(chunk_overlaps(slot->hdr, w->from_ut, w->to_ut))
            persist_slot(slot);

        if(w->to_ut > slot->hdr->last_ut)
            rec.pending[kept++] = *w;
    }
    rec.pending_used = kept;
}

static void pending_windows_add(usec_t from_ut, usec_t to_ut) {
    for(size_t i = 0; i < rec.pending_used; i++) {
        REC_WINDOW *w = &rec.pending[i];
        if(from_ut <= w->to_ut && to_ut >= w->from_ut) {
            w->from_ut = MIN(w->from_ut, from_ut);
            w->to_ut = MAX(w->to_ut, to_ut);
            return;
        }
    }

    if(rec.pending_used < APPS_REC_PENDING_MAX)
        rec.pending[rec.pending_used++] = (REC_WINDOW){ .from_ut = from_ut, .to_ut = to_ut };
    else {
        // extend the window that ends last; the windows only grow, so nothing asked for is lost
        REC_WINDOW *w = &rec.pending[rec.pending_used - 1];
        w->from_ut = MIN(w->from_ut, from_ut);
        w->to_ut = MAX(w->to_ut, to_ut);
    }
}

static void ring_push(APPS_REC_CHUNK_HDR *hdr, size_t size) {
    retry_unwritten();

    REC_SEALED *slot = &rec.ring[rec.ring_next];
    if(slot_unwritten(slot))
        hold_slot(slot);
    freez(slot->hdr);
    *slot = (REC_SEALED){ .hdr = hdr, .size = size };
    rec.ring_next = (rec.ring_next + 1) % APPS_REC_RING_CHUNKS;
    pending_windows_on_seal(slot);
}

static void seal_chunk(void) {
    size_t size;
    APPS_REC_CHUNK_HDR *hdr = apps_rec_builder_seal(&rec.builder, &size);
    if(hdr)
        ring_push(hdr, size);
}

typedef struct {
    int32_t ppid;
    uint32_t uid;
    uint16_t comm, exe, group, cmdline;
} REC_IDENTITY;

static REC_IDENTITY pid_identity(struct pid_stat *p) {
    return (REC_IDENTITY){
        .ppid = p->ppid,
        .uid = (uint32_t)p->uid,
        .comm = apps_rec_builder_string(&rec.builder, p->comm),
        .exe = apps_rec_builder_string(&rec.builder, pid_exe(p)),
        .group = apps_rec_builder_string(&rec.builder, p->target ? p->target->name : NULL),
        .cmdline = apps_recorder_record_cmdline ? apps_rec_builder_string(&rec.builder, p->cmdline) : APPS_REC_STRING_NONE,
    };
}

static void add_row(uint16_t *nrows, struct pid_stat *p, uint8_t why) {
    for(uint16_t i = 0; i < *nrows; i++) {
        if(rec.rows[i].pid == p->pid && rec.rows[i].starttime == p->starttime) {
            rec.rows[i].why |= why;
            return;
        }
    }

    if(*nrows >= APPS_REC_ROWS_MAX)
        return;

    APPS_REC_ROW *r = &rec.rows[(*nrows)++];
    REC_IDENTITY id = pid_identity(p);
    r->pid = p->pid;
    r->ppid = id.ppid;
    r->starttime = p->starttime;
    r->uid = id.uid;
    r->comm = id.comm;
    r->exe = id.exe;
    r->group = id.group;
    r->cmdline = id.cmdline;
    r->cpu = cpu_permyriad(pid_cpu(p));
    r->rss_kib = kib(p->values[PDF_VMRSS]);
    r->swap_kib = kib(p->values[PDF_VMSWAP]);
    r->lread_kibs = kibs(p->values[PDF_LREAD]);
    r->lwrite_kibs = kibs(p->values[PDF_LWRITE]);
    r->pread_kibs = kibs(p->values[PDF_PREAD]);
    r->pwrite_kibs = kibs(p->values[PDF_PWRITE]);
    r->threads = (uint16_t)MIN(p->values[PDF_THREADS], UINT16_MAX);
    r->state = (uint8_t)p->state;
    r->why = why;
}

// the k processes with the largest non-zero values, largest first, without sorting the whole array
static size_t top_k(struct pid_stat **pids, size_t n, kernel_uint_t (*value)(struct pid_stat *),
                    struct pid_stat **out, size_t k) {
    size_t used = 0;
    for(size_t i = 0; i < n; i++) {
        kernel_uint_t v = value(pids[i]);
        if(!v) continue;

        if(used < k)
            out[used++] = pids[i];
        else if(v > value(out[used - 1]))
            out[used - 1] = pids[i];
        else
            continue;

        // keep out[] descending
        for(size_t j = used - 1; j > 0 && value(out[j]) > value(out[j - 1]); j--) {
            struct pid_stat *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    }
    return used;
}

static kernel_uint_t pid_rss(struct pid_stat *p) {
    return p->values[PDF_VMRSS];
}

void apps_recorder_collect(void) {
    if(!apps_recorder_enabled)
        return;

    if(!rec.initialized)
        apps_recorder_init();

    size_t n = 0;
    for(struct pid_stat *p = root_of_pids(); p; p = p->next)
        if(p->updated) n++;

    ensure((void **)&rec.sorted, &rec.sorted_size, n + 1, sizeof(*rec.sorted));
    ensure((void **)&rec.cur, &rec.cur_size, n + 1, sizeof(*rec.cur));

    kernel_uint_t procs_cpu = 0, procs_rss = 0;
    n = 0;
    for(struct pid_stat *p = root_of_pids(); p; p = p->next) {
        if(!p->updated) continue;
        rec.sorted[n] = p;
        rec.cur[n] = (REC_PID){ .pid = p->pid, .starttime = p->starttime, .p = p };
        procs_cpu += pid_cpu(p);
        procs_rss += p->values[PDF_VMRSS];
        n++;
    }

    // starts and exits: the difference between this iteration's and the previous iteration's processes
    qsort(rec.cur, n, sizeof(*rec.cur), cmp_rec_pid);
    struct pid_stat **started = NULL;
    size_t nstarted = 0, nexits = 0;
    if(rec.have_prev) {
        ensure((void **)&rec.exits, &rec.exits_size, rec.prev_used + 1, sizeof(*rec.exits));
        started = mallocz((n + 1) * sizeof(*started));

        size_t i = 0, j = 0;
        while(i < n || j < rec.prev_used) {
            int c = (i >= n) ? 1 : (j >= rec.prev_used) ? -1 : cmp_rec_pid(&rec.cur[i], &rec.prev[j]);
            if(c == 0) { i++; j++; }
            else if(c < 0) started[nstarted++] = rec.cur[i++].p;
            else {
                rec.exits[nexits++] = (APPS_REC_EXIT){ .pid = rec.prev[j].pid, .starttime = rec.prev[j].starttime };
                j++;
            }
        }
    }
    nstarted = MIN(nstarted, (size_t)UINT16_MAX);
    nexits = MIN(nexits, (size_t)UINT16_MAX);

    // a frame interns up to 4 strings per start and per row; seal first when the chunk cannot take them
    if(apps_rec_builder_full(&rec.builder, (nstarted + APPS_REC_ROWS_MAX) * 4))
        seal_chunk();

    ensure((void **)&rec.starts, &rec.starts_size, nstarted + 1, sizeof(*rec.starts));
    for(size_t i = 0; i < nstarted; i++) {
        struct pid_stat *p = started[i];
        REC_IDENTITY id = pid_identity(p);
        rec.starts[i] = (APPS_REC_START){
            .pid = p->pid, .ppid = id.ppid, .starttime = p->starttime, .uid = id.uid,
            .comm = id.comm, .exe = id.exe, .group = id.group, .cmdline = id.cmdline,
        };
    }
    freez(started);

    // rows: by CPU until the coverage target, then the largest by memory and by I/O
    uint16_t nrows = 0;
    kernel_uint_t rows_cpu = 0;
    qsort(rec.sorted, n, sizeof(*rec.sorted), cmp_cpu_desc);
    for(size_t i = 0; i < n && i < APPS_REC_CPU_ROWS_MAX; i++) {
        if(i >= APPS_REC_CPU_ROWS_MIN && (NETDATA_DOUBLE)rows_cpu >= (NETDATA_DOUBLE)procs_cpu * APPS_REC_CPU_COVERAGE)
            break;
        add_row(&nrows, rec.sorted[i], APPS_REC_WHY_CPU);
        rows_cpu += pid_cpu(rec.sorted[i]);
    }

    struct pid_stat *top[MAX(APPS_REC_MEM_ROWS, APPS_REC_IO_ROWS)];
    size_t ntop = top_k(rec.sorted, n, pid_rss, top, APPS_REC_MEM_ROWS);
    for(size_t i = 0; i < ntop; i++) {
        uint16_t before = nrows;
        add_row(&nrows, top[i], APPS_REC_WHY_MEM);
        if(nrows > before) rows_cpu += pid_cpu(top[i]);
    }

    ntop = top_k(rec.sorted, n, pid_io, top, APPS_REC_IO_ROWS);
    for(size_t i = 0; i < ntop; i++) {
        uint16_t before = nrows;
        add_row(&nrows, top[i], APPS_REC_WHY_IO);
        if(nrows > before) rows_cpu += pid_cpu(top[i]);
    }

    APPS_REC_FRAME_HDR totals = {
        .time_ut = now_realtime_usec(),
        .pids = (uint32_t)n,
        .host_cpu = cpu_permyriad(global_utime + global_stime + global_gtime),
        .procs_cpu = cpu_permyriad(procs_cpu),
        .rows_cpu = cpu_permyriad(rows_cpu),
        .procs_rss_kib = procs_rss / 1024,
    };
    rec.last.rows = nrows;
    rec.last.starts = (uint16_t)nstarted;
    rec.last.exits = (uint16_t)nexits;
    rec.last.cpu_coverage = procs_cpu ? (uint32_t)MIN((uint64_t)rows_cpu * 10000 / procs_cpu, 10000) : 10000;

    apps_rec_builder_add_frame(&rec.builder, &totals,
                               rec.starts, (uint16_t)nstarted,
                               rec.exits, (uint16_t)nexits,
                               rec.rows, nrows);

    if(apps_rec_builder_full(&rec.builder, 0))
        seal_chunk();

    // this iteration's processes become the previous ones
    REC_PID *t = rec.prev;
    size_t ts = rec.prev_size;
    rec.prev = rec.cur;
    rec.prev_size = rec.cur_size;
    rec.prev_used = n;
    rec.cur = t;
    rec.cur_size = ts;
    rec.have_prev = true;
}

void apps_recorder_send_charts_to_netdata(usec_t dt) {
    if(!apps_recorder_enabled || !rec.initialized)
        return;

    static bool created = false;
    if(!created) {
        created = true;
        fprintf(stdout,
                "CHART netdata.apps_recorder_memory '' 'Apps Plugin Flight Recorder Memory' 'bytes' apps.plugin netdata.apps_recorder.memory stacked 140040 %1$d\n"
                "DIMENSION sealed '' absolute 1 1\n"
                "DIMENSION building '' absolute 1 1\n"
                "DIMENSION unwritten '' absolute 1 1\n"
                "CHART netdata.apps_recorder_frame '' 'Apps Plugin Flight Recorder Frame' 'processes' apps.plugin netdata.apps_recorder.frame line 140041 %1$d\n"
                "DIMENSION rows '' absolute 1 1\n"
                "DIMENSION starts '' absolute 1 1\n"
                "DIMENSION exits '' absolute 1 1\n"
                "CHART netdata.apps_recorder_coverage '' 'Apps Plugin Flight Recorder CPU Coverage' 'percentage' apps.plugin netdata.apps_recorder.coverage line 140042 %1$d\n"
                "DIMENSION cpu '' absolute 1 100\n"
                "CHART netdata.apps_recorder_disk '' 'Apps Plugin Flight Recorder Disk Store' 'bytes' apps.plugin netdata.apps_recorder.disk line 140043 %1$d\n"
                "DIMENSION used '' absolute 1 1\n"
                "DIMENSION cap '' absolute 1 1\n"
                , update_every);
    }

    size_t sealed = 0, unwritten = 0;
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++) {
        sealed += rec.ring[i].size;
        if(slot_unwritten(&rec.ring[i])) unwritten += rec.ring[i].size;
    }
    for(size_t i = 0; i < rec.held_used; i++)
        unwritten += rec.held[i].size;

    fprintf(stdout,
            "BEGIN netdata.apps_recorder_memory %"PRIu64"\n"
            "SET sealed = %zu\n"
            "SET building = %zu\n"
            "SET unwritten = %zu\n"
            "END\n"
            "BEGIN netdata.apps_recorder_frame %"PRIu64"\n"
            "SET rows = %u\n"
            "SET starts = %u\n"
            "SET exits = %u\n"
            "END\n"
            "BEGIN netdata.apps_recorder_coverage %"PRIu64"\n"
            "SET cpu = %u\n"
            "END\n"
            "BEGIN netdata.apps_recorder_disk %"PRIu64"\n"
            "SET used = %"PRIu64"\n"
            "SET cap = %"PRIu64"\n"
            "END\n"
            , dt, sealed, buffer_strlen(rec.builder.payload), unwritten
            , dt, (unsigned)rec.last.rows, (unsigned)rec.last.starts, (unsigned)rec.last.exits
            , dt, (unsigned)rec.last.cpu_coverage
            , dt, rec.store_open ? rec.store.bytes : 0, rec.store_open ? rec.store.cap_bytes : 0);
}

// --------------------------------------------------------------------------------------------------------------------
// saving

static void save_window(usec_t from_ut, usec_t to_ut, size_t *stored, size_t *unwritten, bool *pending) {
    *stored = 0;
    *unwritten = 0;
    usec_t newest_sealed_ut = 0;
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++) {
        REC_SEALED *slot = &rec.ring[i];
        if(!slot->hdr) continue;
        newest_sealed_ut = MAX(newest_sealed_ut, slot->hdr->last_ut);
        if(!chunk_overlaps(slot->hdr, from_ut, to_ut)) continue;
        if(persist_slot(slot)) (*stored)++;
        else (*unwritten)++;
    }

    *pending = to_ut > newest_sealed_ut;
    if(*pending)
        pending_windows_add(from_ut, to_ut);
}

bool apps_recorder_parse_time(const char *value, time_t now_s, time_t *out) {
    char *end = NULL;
    long long v = strtoll(value, &end, 10);
    if(!end || end == value || *end)
        return false;

    // zero and negative values are relative to now, like the rest of the Netdata API
    *out = (v <= 0) ? now_s + (time_t)v : (time_t)v;
    return true;
}

static void save_error(const char *transaction, int code, const char *msg) {
    pluginsd_function_json_error_to_stdout(transaction, code, msg);
}

void apps_recorder_function_save(const char *transaction, char *function,
                                 usec_t *stop_monotonic_ut __maybe_unused, bool *cancelled __maybe_unused,
                                 BUFFER *payload __maybe_unused, HTTP_ACCESS access __maybe_unused,
                                 const char *source __maybe_unused, void *data __maybe_unused) {
    time_t now_s = now_realtime_sec();
    time_t at_s = now_s;
    long long before_s = APPS_RECORDER_SAVE_BEFORE_DEFAULT, after_s = APPS_RECORDER_SAVE_AFTER_DEFAULT;
    const char *reason = "unspecified";
    bool seal = false;

    char *words[PLUGINSD_MAX_WORDS] = { NULL };
    size_t num_words = quoted_strings_splitter_whitespace(function, words, PLUGINSD_MAX_WORDS);
    for(size_t i = 1; i < num_words; i++) {
        const char *w = get_word(words, num_words, i);
        if(!w) break;

        bool ok = true;
        if(strncmp(w, "at:", 3) == 0)
            ok = apps_recorder_parse_time(&w[3], now_s, &at_s);
        else if(strncmp(w, "before:", 7) == 0)
            ok = (before_s = str2ll(&w[7], NULL)) >= 0;
        else if(strncmp(w, "after:", 6) == 0)
            ok = (after_s = str2ll(&w[6], NULL)) >= 0 && after_s <= APPS_RECORDER_SAVE_AFTER_MAX;
        else if(strncmp(w, "reason:", 7) == 0)
            reason = &w[7];
        else if(strcmp(w, "seal:yes") == 0)
            seal = true;
        else
            ok = false;

        if(!ok) {
            netdata_mutex_lock(&apps_and_stdout_mutex);
            save_error(transaction, HTTP_RESP_BAD_REQUEST, "invalid parameter; use at:, before:, after:, reason:, seal:yes");
            netdata_mutex_unlock(&apps_and_stdout_mutex);
            return;
        }
    }

    // the past is bounded by what the ring can hold
    long long ring_s = (long long)APPS_REC_RING_CHUNKS * APPS_REC_FRAMES_PER_CHUNK * update_every;
    before_s = MIN(before_s, ring_s);

    netdata_mutex_lock(&apps_and_stdout_mutex);

    if(!apps_recorder_enabled || !rec.initialized) {
        save_error(transaction, HTTP_RESP_SERVICE_UNAVAILABLE, "the process flight recorder is disabled");
        netdata_mutex_unlock(&apps_and_stdout_mutex);
        return;
    }

    if(!rec.store_open) {
        save_error(transaction, HTTP_RESP_SERVICE_UNAVAILABLE, "the process flight recorder has no disk store");
        netdata_mutex_unlock(&apps_and_stdout_mutex);
        return;
    }

    // seal:yes writes the chunk still being recorded now, instead of when it fills (the agent is about to stop us)
    if(seal && rec.builder.frames)
        seal_chunk();

    usec_t from_ut = (usec_t)MAX(at_s - (time_t)before_s, 0) * USEC_PER_SEC;
    usec_t to_ut = (usec_t)(at_s + (time_t)after_s) * USEC_PER_SEC;
    size_t stored, unwritten;
    bool pending;
    save_window(from_ut, to_ut, &stored, &unwritten, &pending);

    nd_log(NDLS_COLLECTORS, unwritten ? NDLP_WARNING : NDLP_INFO,
           "apps.plugin recorder: saving processes from %lld to %lld, reason '%s': %zu chunks written%s%s%s",
           (long long)(from_ut / USEC_PER_SEC), (long long)(to_ut / USEC_PER_SEC), reason, stored,
           unwritten ? ", the rest kept in RAM until it can be written: " : "",
           unwritten ? apps_rec_store_result_str(rec.last_failure) : "",
           pending ? ", the rest as it is recorded" : "");

    BUFFER *wb = buffer_create(1024, NULL);
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_MINIFY);
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_time_t(wb, "from", (time_t)(from_ut / USEC_PER_SEC));
    buffer_json_member_add_time_t(wb, "to", (time_t)(to_ut / USEC_PER_SEC));
    buffer_json_member_add_uint64(wb, "chunks_written", stored);
    buffer_json_member_add_uint64(wb, "chunks_unwritten", unwritten);
    if(unwritten)
        buffer_json_member_add_string(wb, "unwritten_reason", apps_rec_store_result_str(rec.last_failure));
    buffer_json_member_add_boolean(wb, "pending", pending);
    buffer_json_member_add_object(wb, "store");
    {
        buffer_json_member_add_uint64(wb, "chunks", rec.store.used);
        buffer_json_member_add_uint64(wb, "bytes", rec.store.bytes);
        buffer_json_member_add_uint64(wb, "cap_bytes", rec.store.cap_bytes);
    }
    buffer_json_object_close(wb);
    buffer_json_finalize(wb);

    wb->response_code = HTTP_RESP_OK;
    wb->content_type = CT_APPLICATION_JSON;
    wb->expires = now_s;
    pluginsd_function_result_to_stdout(transaction, wb);
    buffer_free(wb);

    netdata_mutex_unlock(&apps_and_stdout_mutex);
}

// --------------------------------------------------------------------------------------------------------------------
// reading

typedef struct {
    enum { SRC_RING, SRC_HELD, SRC_STORE, SRC_BUILDING } type;
    uint64_t first_ut;
    uint64_t last_ut;
    size_t slot;            // in the ring or the hold
} REC_SOURCE;

typedef struct {
    usec_t after_ut, before_ut;
    apps_rec_frame_cb_t cb;
    void *data;
    bool stopped;
    size_t delivered;
} REC_WALK;

static bool walk_frame(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    REC_WALK *w = data;
    if(f->hdr->time_ut < w->after_ut)
        return true;

    if(f->hdr->time_ut > w->before_ut) {
        w->stopped = true;
        return false;
    }

    w->delivered++;
    if(!w->cb(f, strings, w->data)) {
        w->stopped = true;
        return false;
    }

    return true;
}

static int cmp_source(const void *a, const void *b) {
    const REC_SOURCE *x = a, *y = b;
    return (x->first_ut > y->first_ut) - (x->first_ut < y->first_ut);
}

static bool ring_has_chunk(uint64_t first_ut) {
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++)
        if(rec.ring[i].hdr && rec.ring[i].hdr->first_ut == first_ut)
            return true;
    return false;
}

APPS_REC_SOURCE apps_recorder_foreach_frame(usec_t after_ut, usec_t before_ut, apps_rec_frame_cb_t cb, void *data) {
    if(!rec.initialized || after_ut > before_ut)
        return 0;

    size_t max = APPS_REC_RING_CHUNKS + rec.held_used + 1 + (rec.store_open ? rec.store.used : 0);
    REC_SOURCE *src = mallocz(max * sizeof(*src));
    size_t n = 0;

    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++) {
        const APPS_REC_CHUNK_HDR *h = rec.ring[i].hdr;
        if(h && chunk_overlaps(h, after_ut, before_ut))
            src[n++] = (REC_SOURCE){ .type = SRC_RING, .first_ut = h->first_ut, .last_ut = h->last_ut, .slot = i };
    }

    for(size_t i = 0; i < rec.held_used; i++) {
        const APPS_REC_CHUNK_HDR *h = rec.held[i].hdr;
        if(chunk_overlaps(h, after_ut, before_ut))
            src[n++] = (REC_SOURCE){ .type = SRC_HELD, .first_ut = h->first_ut, .last_ut = h->last_ut, .slot = i };
    }

    if(rec.builder.frames && rec.builder.first_ut <= before_ut && rec.builder.last_ut >= after_ut)
        src[n++] = (REC_SOURCE){ .type = SRC_BUILDING, .first_ut = rec.builder.first_ut, .last_ut = rec.builder.last_ut };

    if(rec.store_open) {
        for(size_t i = 0; i < rec.store.used; i++) {
            const APPS_REC_STORE_ITEM *it = &rec.store.items[i];
            if(it->first_ut <= before_ut && it->last_ut >= after_ut && !ring_has_chunk(it->first_ut))
                src[n++] = (REC_SOURCE){ .type = SRC_STORE, .first_ut = it->first_ut, .last_ut = it->last_ut };
        }
    }

    qsort(src, n, sizeof(*src), cmp_source);

    REC_WALK w = { .after_ut = after_ut, .before_ut = before_ut, .cb = cb, .data = data };
    APPS_REC_SOURCE used = 0;
    for(size_t i = 0; i < n && !w.stopped; i++) {
        size_t delivered = w.delivered;
        switch(src[i].type) {
            case SRC_RING:
            case SRC_HELD: {
                REC_SEALED *slot = (src[i].type == SRC_RING) ? &rec.ring[src[i].slot] : &rec.held[src[i].slot];
                apps_rec_chunk_foreach_frame(slot->hdr, slot->size, walk_frame, &w);
                break;
            }

            case SRC_BUILDING:
                apps_rec_builder_foreach_frame(&rec.builder, walk_frame, &w);
                break;

            case SRC_STORE: {
                size_t size;
                APPS_REC_CHUNK_HDR *hdr = apps_rec_store_read(&rec.store, src[i].first_ut, &size);
                if(hdr)
                    apps_rec_chunk_foreach_frame(hdr, size, walk_frame, &w);
                freez(hdr);
                break;
            }
        }

        if(w.delivered > delivered)
            used |= (src[i].type == SRC_STORE) ? APPS_REC_SOURCE_DISK : APPS_REC_SOURCE_RAM;
    }

    freez(src);
    return used;
}

void apps_recorder_span(usec_t *first_ut, usec_t *last_ut) {
    *first_ut = 0;
    *last_ut = 0;
    if(!rec.initialized)
        return;

    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++) {
        const APPS_REC_CHUNK_HDR *h = rec.ring[i].hdr;
        if(!h) continue;
        if(!*first_ut || h->first_ut < *first_ut) *first_ut = h->first_ut;
        *last_ut = MAX(*last_ut, h->last_ut);
    }

    for(size_t i = 0; i < rec.held_used; i++) {
        const APPS_REC_CHUNK_HDR *h = rec.held[i].hdr;
        if(!*first_ut || h->first_ut < *first_ut) *first_ut = h->first_ut;
        *last_ut = MAX(*last_ut, h->last_ut);
    }

    if(rec.builder.frames) {
        if(!*first_ut || rec.builder.first_ut < *first_ut) *first_ut = rec.builder.first_ut;
        *last_ut = MAX(*last_ut, rec.builder.last_ut);
    }

    if(rec.store_open && rec.store.used) {
        if(!*first_ut || rec.store.items[0].first_ut < *first_ut) *first_ut = rec.store.items[0].first_ut;
        *last_ut = MAX(*last_ut, rec.store.items[rec.store.used - 1].last_ut);
    }
}

APPS_REC_SAVE apps_recorder_save_status(usec_t from_ut, usec_t to_ut, const char **reason) {
    *reason = NULL;
    if(!rec.initialized || from_ut > to_ut)
        return APPS_REC_SAVE_NOT_REQUESTED;

    for(size_t i = 0; i < rec.held_used; i++) {
        if(chunk_overlaps(rec.held[i].hdr, from_ut, to_ut)) {
            *reason = apps_rec_store_result_str(rec.held[i].failure);
            return APPS_REC_SAVE_IN_RAM;
        }
    }

    bool saved = false;
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++) {
        const REC_SEALED *slot = &rec.ring[i];
        if(!slot->hdr || !chunk_overlaps(slot->hdr, from_ut, to_ut))
            continue;

        if(slot_unwritten(slot)) {
            *reason = apps_rec_store_result_str(slot->failure);
            return APPS_REC_SAVE_IN_RAM;
        }
        saved = saved || slot->stored;
    }

    for(size_t i = rec.lost_used; i > 0; i--) {
        const REC_LOST *l = &rec.lost[i - 1];
        if(l->from_ut <= to_ut && l->to_ut >= from_ut) {
            *reason = apps_rec_store_result_str(l->reason);
            return APPS_REC_SAVE_LOST;
        }
    }

    for(size_t i = 0; i < rec.pending_used; i++)
        if(rec.pending[i].from_ut <= to_ut && rec.pending[i].to_ut >= from_ut)
            return APPS_REC_SAVE_PENDING;

    for(size_t i = 0; !saved && rec.store_open && i < rec.store.used; i++)
        saved = rec.store.items[i].first_ut <= to_ut && rec.store.items[i].last_ut >= from_ut;

    return saved ? APPS_REC_SAVE_OK : APPS_REC_SAVE_NOT_REQUESTED;
}

const char *apps_recorder_save_str(APPS_REC_SAVE s) {
    switch(s) {
        case APPS_REC_SAVE_OK:          return "ok";
        case APPS_REC_SAVE_PENDING:     return "pending";
        case APPS_REC_SAVE_IN_RAM:      return "not_saved";
        case APPS_REC_SAVE_LOST:        return "not_saved";
        default:
        case APPS_REC_SAVE_NOT_REQUESTED: return "not_requested";
    }
}

// --------------------------------------------------------------------------------------------------------------------

void apps_recorder_init(void) {
    if(rec.initialized || !apps_recorder_enabled)
        return;

    read_boot_id(rec.boot_id);
    apps_rec_builder_init(&rec.builder, rec.boot_id);

    if(apps_recorder_disk_mb > 0) {
        const char *cache_dir = getenv("NETDATA_CACHE_DIR");
        if(!cache_dir || !*cache_dir)
            cache_dir = CACHE_DIR;

        char dir[FILENAME_MAX + 1];
        snprintfz(dir, sizeof(dir), "%s/apps-recorder", cache_dir);
        rec.store_open = apps_rec_store_open(&rec.store, dir, (uint64_t)apps_recorder_disk_mb * 1024 * 1024);
    }

    rec.initialized = true;
}

void apps_recorder_cleanup(void) {
    if(!rec.initialized)
        return;

    // a window still being recorded keeps what it has
    if(rec.pending_used && rec.builder.frames)
        seal_chunk();

    if(rec.store_open)
        apps_rec_store_close(&rec.store);

    apps_rec_builder_destroy(&rec.builder);
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++)
        freez(rec.ring[i].hdr);
    for(size_t i = 0; i < rec.held_used; i++)
        freez(rec.held[i].hdr);

    freez(rec.prev);
    freez(rec.cur);
    freez(rec.sorted);
    freez(rec.starts);
    freez(rec.exits);
    memset(&rec, 0, sizeof(rec));
}

#endif
