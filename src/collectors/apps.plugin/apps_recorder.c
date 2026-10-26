// SPDX-License-Identifier: GPL-3.0-or-later

// The flight recorder: every iteration, the processes that hold most of the CPU, the largest by memory and by I/O,
// and the processes that started or exited, become a frame (apps_recorder_format.h). Frames are packed in chunks
// of APPS_REC_FRAMES_PER_CHUNK and the last APPS_REC_RING_CHUNKS sealed chunks stay in RAM, so the recent past of
// the process table survives the processes themselves.
//
// All functions here run under apps_and_stdout_mutex: the collection loop and the Function workers hold it.

#include "apps_plugin.h"
#include "apps_recorder_format.h"

#if defined(OS_LINUX)

#define APPS_REC_RING_CHUNKS        30
#define APPS_REC_CPU_COVERAGE       0.95
#define APPS_REC_CPU_ROWS_MIN       50
#define APPS_REC_CPU_ROWS_MAX       150
#define APPS_REC_MEM_ROWS           10
#define APPS_REC_IO_ROWS            10
#define APPS_REC_ROWS_MAX           (APPS_REC_CPU_ROWS_MAX + APPS_REC_MEM_ROWS + APPS_REC_IO_ROWS)

bool apps_recorder_enabled = true;
bool apps_recorder_record_cmdline = false;

typedef struct {
    int32_t pid;
    uint64_t starttime;
    struct pid_stat *p;     // valid only during the iteration that collected it
} REC_PID;

typedef struct {
    APPS_REC_CHUNK_HDR *hdr;
    size_t size;
} REC_SEALED;

static struct {
    bool initialized;
    uint8_t boot_id[16];
    APPS_REC_BUILDER builder;

    REC_SEALED ring[APPS_REC_RING_CHUNKS];
    size_t ring_next;           // the slot the next sealed chunk goes to (the oldest when the ring is full)

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

static void ring_push(APPS_REC_CHUNK_HDR *hdr, size_t size) {
    REC_SEALED *slot = &rec.ring[rec.ring_next];
    freez(slot->hdr);
    slot->hdr = hdr;
    slot->size = size;
    rec.ring_next = (rec.ring_next + 1) % APPS_REC_RING_CHUNKS;
}

static void seal_chunk(void) {
    size_t size;
    APPS_REC_CHUNK_HDR *hdr = apps_rec_builder_seal(&rec.builder, rec.boot_id, &size);
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
                "CHART netdata.apps_recorder_frame '' 'Apps Plugin Flight Recorder Frame' 'processes' apps.plugin netdata.apps_recorder.frame line 140041 %1$d\n"
                "DIMENSION rows '' absolute 1 1\n"
                "DIMENSION starts '' absolute 1 1\n"
                "DIMENSION exits '' absolute 1 1\n"
                "CHART netdata.apps_recorder_coverage '' 'Apps Plugin Flight Recorder CPU Coverage' 'percentage' apps.plugin netdata.apps_recorder.coverage line 140042 %1$d\n"
                "DIMENSION cpu '' absolute 1 100\n"
                , update_every);
    }

    size_t sealed = 0;
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++)
        sealed += rec.ring[i].size;

    fprintf(stdout,
            "BEGIN netdata.apps_recorder_memory %"PRIu64"\n"
            "SET sealed = %zu\n"
            "SET building = %zu\n"
            "END\n"
            "BEGIN netdata.apps_recorder_frame %"PRIu64"\n"
            "SET rows = %u\n"
            "SET starts = %u\n"
            "SET exits = %u\n"
            "END\n"
            "BEGIN netdata.apps_recorder_coverage %"PRIu64"\n"
            "SET cpu = %u\n"
            "END\n"
            , dt, sealed, buffer_strlen(rec.builder.payload)
            , dt, (unsigned)rec.last.rows, (unsigned)rec.last.starts, (unsigned)rec.last.exits
            , dt, (unsigned)rec.last.cpu_coverage);
}

void apps_recorder_init(void) {
    if(rec.initialized)
        return;

    read_boot_id(rec.boot_id);
    apps_rec_builder_init(&rec.builder);
    rec.initialized = true;
}

void apps_recorder_cleanup(void) {
    if(!rec.initialized)
        return;

    apps_rec_builder_destroy(&rec.builder);
    for(size_t i = 0; i < APPS_REC_RING_CHUNKS; i++)
        freez(rec.ring[i].hdr);

    freez(rec.prev);
    freez(rec.cur);
    freez(rec.sorted);
    freez(rec.starts);
    freez(rec.exits);
    memset(&rec, 0, sizeof(rec));
}

#endif
