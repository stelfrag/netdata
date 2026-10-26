// SPDX-License-Identifier: GPL-3.0-or-later

// `processes` with a time parameter answers from the flight recorder instead of the live process table:
//
//   at:T                the table of the recorded sample at or before T
//   after:A before:B    the processes seen in [A, B]: average and peak CPU, peak memory, first and last seen,
//                       started and exited within the range
//
// Times are unix epochs, or relative to now when zero or negative. The recorder keeps only the processes that hold
// most of the CPU, the largest by memory and by I/O, and every start and exit, so each answer reports the share of
// process CPU its samples covered.

#include "apps_recorder.h"

#if defined(OS_LINUX)

#define APPS_REC_QUERY_RANGE_DEFAULT_S  600
#define APPS_REC_QUERY_AT_LOOKBACK_S    120
#define APPS_REC_QUERY_ROWS_MAX         1000

typedef struct {
    usec_t sample_ut;
    size_t frames;
    uint64_t rows_cpu_sum;          // 1/10000 of a core, summed over frames
    uint64_t procs_cpu_sum;
    double coverage_min;

    // the frame totals: all processes, not only the rows kept
    uint32_t pids_min, pids_max;
    uint64_t pids_sum;
    uint32_t procs_cpu_max;
    uint64_t procs_rss_kib_sum, procs_rss_kib_max;
    size_t host_frames;             // frames that know the host CPU
    uint64_t host_cpu_sum;
    uint32_t host_cpu_max;
} REC_COVERAGE;

static void coverage_add(REC_COVERAGE *c, const APPS_REC_FRAME_HDR *h) {
    c->frames++;
    c->rows_cpu_sum += h->rows_cpu;
    c->procs_cpu_sum += h->procs_cpu;

    if(c->frames == 1 || h->pids < c->pids_min) c->pids_min = h->pids;
    if(h->pids > c->pids_max) c->pids_max = h->pids;
    c->pids_sum += h->pids;
    if(h->procs_cpu > c->procs_cpu_max) c->procs_cpu_max = h->procs_cpu;
    c->procs_rss_kib_sum += h->procs_rss_kib;
    if(h->procs_rss_kib > c->procs_rss_kib_max) c->procs_rss_kib_max = h->procs_rss_kib;
    if(h->host_cpu) {
        c->host_frames++;
        c->host_cpu_sum += h->host_cpu;
        if(h->host_cpu > c->host_cpu_max) c->host_cpu_max = h->host_cpu;
    }

    double cov = h->procs_cpu ? MIN((double)h->rows_cpu / (double)h->procs_cpu, 1.0) : 1.0;
    if(c->frames == 1 || cov < c->coverage_min)
        c->coverage_min = cov;
}

static double coverage_avg(const REC_COVERAGE *c) {
    return c->procs_cpu_sum ? MIN((double)c->rows_cpu_sum / (double)c->procs_cpu_sum, 1.0) : 1.0;
}

static STRING *dup_string(const APPS_REC_STRINGS *strings, uint16_t id) {
    const char *s = apps_rec_string(strings, id);
    return s ? string_strdupz(s) : NULL;
}

// --------------------------------------------------------------------------------------------------------------------
// table output

typedef struct {
    const char *key;
    const char *name;
    RRDF_FIELD_TYPE type;
    RRDF_FIELD_TRANSFORM transform;
    size_t decimals;
    const char *units;
    RRDF_FIELD_SUMMARY summary;
    RRDF_FIELD_FILTER filter;
    RRDF_FIELD_OPTIONS options;
} REC_COLUMN;

#define COL_STR(key, name, opts) { key, name, RRDF_FIELD_TYPE_STRING, RRDF_FIELD_TRANSFORM_NONE, 0, NULL, \
    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT, opts }
#define COL_INT(key, name, opts) { key, name, RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_TRANSFORM_NUMBER, 0, NULL, \
    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT, opts }
#define COL_NUM(key, name, dec, units, summary, opts) { key, name, RRDF_FIELD_TYPE_BAR_WITH_INTEGER, \
    RRDF_FIELD_TRANSFORM_NUMBER, dec, units, summary, RRDF_FIELD_FILTER_RANGE, opts }
#define COL_TIME(key, name, opts) { key, name, RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_TRANSFORM_DATETIME_USEC, 0, \
    NULL, RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE, opts }

#define OPT_VIS     RRDF_FIELD_OPTS_VISIBLE
#define OPT_STICKY  (RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_STICKY)

static void emit_columns(BUFFER *wb, const REC_COLUMN *cols, size_t n, bool show_cmdline) {
    buffer_json_member_add_object(wb, "columns");
    size_t field_id = 0;
    for(size_t i = 0; i < n; i++) {
        if(!show_cmdline && strcmp(cols[i].key, "CmdLine") == 0)
            continue;

        buffer_rrdf_table_add_field(wb, field_id++, cols[i].key, cols[i].name, cols[i].type,
                                    RRDF_FIELD_VISUAL_VALUE, cols[i].transform, cols[i].decimals, cols[i].units,
                                    NAN, RRDF_FIELD_SORT_DESCENDING, NULL, cols[i].summary, cols[i].filter,
                                    cols[i].options, NULL);
    }
    buffer_json_object_close(wb);
}

static void emit_recorded(BUFFER *wb, const char *mode, usec_t from_ut, usec_t to_ut,
                          const REC_COVERAGE *c, APPS_REC_SOURCE sources) {
    buffer_json_member_add_object(wb, "recorded");
    {
        buffer_json_member_add_string(wb, "mode", mode);
        buffer_json_member_add_uint64(wb, "from_ut", from_ut);
        buffer_json_member_add_uint64(wb, "to_ut", to_ut);
        if(c->sample_ut)
            buffer_json_member_add_uint64(wb, "sample_ut", c->sample_ut);
        buffer_json_member_add_uint64(wb, "samples", c->frames);
        buffer_json_member_add_double(wb, "cpu_coverage", coverage_avg(c) * 100.0);
        buffer_json_member_add_double(wb, "cpu_coverage_min", (c->frames ? c->coverage_min : 1.0) * 100.0);

        // every process, not only the rows kept; CPU in % of one core like the CPU columns, memory in MiB
        if(c->frames) {
            const double n = (double)c->frames;
            buffer_json_member_add_object(wb, "totals");
            {
                buffer_json_member_add_object(wb, "processes");
                buffer_json_member_add_uint64(wb, "min", c->pids_min);
                buffer_json_member_add_double(wb, "avg", (double)c->pids_sum / n);
                buffer_json_member_add_uint64(wb, "max", c->pids_max);
                buffer_json_object_close(wb);

                buffer_json_member_add_object(wb, "processes_cpu");
                buffer_json_member_add_double(wb, "avg", (double)c->procs_cpu_sum / n / 100.0);
                buffer_json_member_add_double(wb, "max", (double)c->procs_cpu_max / 100.0);
                buffer_json_object_close(wb);

                if(c->host_frames) {
                    buffer_json_member_add_object(wb, "host_cpu");
                    buffer_json_member_add_double(wb, "avg", (double)c->host_cpu_sum / (double)c->host_frames / 100.0);
                    buffer_json_member_add_double(wb, "max", (double)c->host_cpu_max / 100.0);
                    buffer_json_object_close(wb);
                }

                buffer_json_member_add_object(wb, "processes_rss");
                buffer_json_member_add_double(wb, "avg", (double)c->procs_rss_kib_sum / n / 1024.0);
                buffer_json_member_add_double(wb, "max", (double)c->procs_rss_kib_max / 1024.0);
                buffer_json_object_close(wb);
            }
            buffer_json_object_close(wb);
        }
        buffer_json_member_add_array(wb, "source");
        if(sources & APPS_REC_SOURCE_DISK) buffer_json_add_array_item_string(wb, "disk");
        if(sources & APPS_REC_SOURCE_RAM) buffer_json_add_array_item_string(wb, "ram");
        buffer_json_array_close(wb);
    }
    buffer_json_object_close(wb);
}

static BUFFER *response_begin(const char *help) {
    BUFFER *wb = buffer_create(4096, NULL);
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_MINIFY);
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_string(wb, "type", "table");
    buffer_json_member_add_time_t(wb, "update_every", update_every);
    buffer_json_member_add_boolean(wb, "has_history", false);
    buffer_json_member_add_string(wb, "help", help);
    return wb;
}

static void response_send(const char *transaction, BUFFER *wb, const char *default_sort) {
    time_t now_s = now_realtime_sec();
    buffer_json_member_add_string(wb, "default_sort_column", default_sort);
    buffer_json_member_add_time_t(wb, "expires", now_s + update_every);
    buffer_json_finalize(wb);
    wb->response_code = HTTP_RESP_OK;
    wb->content_type = CT_APPLICATION_JSON;
    wb->expires = now_s + update_every;
    pluginsd_function_result_to_stdout(transaction, wb);
    buffer_free(wb);
}

static const char *why_string(uint8_t why) {
    static const char *names[] = {
        [0] = "",
        [APPS_REC_WHY_CPU] = "cpu",
        [APPS_REC_WHY_MEM] = "memory",
        [APPS_REC_WHY_CPU | APPS_REC_WHY_MEM] = "cpu,memory",
        [APPS_REC_WHY_IO] = "io",
        [APPS_REC_WHY_CPU | APPS_REC_WHY_IO] = "cpu,io",
        [APPS_REC_WHY_MEM | APPS_REC_WHY_IO] = "memory,io",
        [APPS_REC_WHY_CPU | APPS_REC_WHY_MEM | APPS_REC_WHY_IO] = "cpu,memory,io",
    };
    return names[why & 0x07];
}

static void add_string_or_dash(BUFFER *wb, STRING *s) {
    buffer_json_add_array_item_string(wb, s ? string2str(s) : "-");
}

// --------------------------------------------------------------------------------------------------------------------
// at:T

typedef struct {
    APPS_REC_ROW row;
    STRING *comm, *exe, *group, *cmdline;
} REC_AT_ROW;

typedef struct {
    REC_AT_ROW *rows;
    size_t used;
    size_t size;
    REC_COVERAGE coverage;
    APPS_REC_FRAME_HDR hdr;
} REC_AT;

static void at_rows_free(REC_AT *a) {
    for(size_t i = 0; i < a->used; i++) {
        string_freez(a->rows[i].comm);
        string_freez(a->rows[i].exe);
        string_freez(a->rows[i].group);
        string_freez(a->rows[i].cmdline);
    }
    a->used = 0;
}

// keeps a copy of the newest frame walked; frames arrive oldest first
static bool at_frame(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    REC_AT *a = data;
    at_rows_free(a);

    if(f->hdr->rows > a->size) {
        a->size = f->hdr->rows;
        a->rows = reallocz(a->rows, a->size * sizeof(*a->rows));
    }

    for(uint16_t i = 0; i < f->hdr->rows; i++) {
        APPS_REC_ROW r = f->rows[i];
        a->rows[a->used++] = (REC_AT_ROW){
            .row = r,
            .comm = dup_string(strings, r.comm),
            .exe = dup_string(strings, r.exe),
            .group = dup_string(strings, r.group),
            .cmdline = dup_string(strings, r.cmdline),
        };
    }

    a->hdr = *f->hdr;
    return true;
}

static const REC_COLUMN at_columns[] = {
    COL_INT("PID", "Process ID", RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_STICKY | RRDF_FIELD_OPTS_UNIQUE_KEY),
    COL_STR("Cmd", "Process Name", OPT_STICKY),
    COL_STR("Exe", "Executable", OPT_VIS),
    COL_STR("CmdLine", "Command Line", RRDF_FIELD_OPTS_NONE),
    COL_INT("PPID", "Parent Process ID", RRDF_FIELD_OPTS_NONE),
    COL_STR("Category", "Category (apps_groups.conf)", OPT_STICKY),
    COL_INT("Uid", "User ID", RRDF_FIELD_OPTS_NONE),
    COL_NUM("CPU", "Total CPU Time (100% = 1 core)", 2, "%", RRDF_FIELD_SUMMARY_SUM, OPT_VIS),
    COL_NUM("RSS", "Resident Set Size", 2, "MiB", RRDF_FIELD_SUMMARY_SUM, OPT_VIS),
    COL_NUM("Swap", "Swap Memory", 2, "MiB", RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_OPTS_NONE),
    COL_NUM("LReads", "Logical I/O Reads", 0, "KiB/s", RRDF_FIELD_SUMMARY_SUM, OPT_VIS),
    COL_NUM("LWrites", "Logical I/O Writes", 0, "KiB/s", RRDF_FIELD_SUMMARY_SUM, OPT_VIS),
    COL_NUM("PReads", "Physical I/O Reads", 0, "KiB/s", RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_OPTS_NONE),
    COL_NUM("PWrites", "Physical I/O Writes", 0, "KiB/s", RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_OPTS_NONE),
    COL_NUM("Threads", "Threads", 0, "threads", RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_OPTS_NONE),
    COL_STR("State", "Process State", RRDF_FIELD_OPTS_NONE),
    COL_STR("RecordedFor", "Why the Recorder Kept It", RRDF_FIELD_OPTS_NONE),
};

static void query_at(const char *transaction, usec_t at_ut, bool show_cmdline) {
    REC_AT a = { 0 };

    // the newest frame at or before T, looking back a little first
    usec_t lookback_ut = (usec_t)APPS_REC_QUERY_AT_LOOKBACK_S * update_every * USEC_PER_SEC;
    APPS_REC_SOURCE sources = apps_recorder_foreach_frame(at_ut > lookback_ut ? at_ut - lookback_ut : 0, at_ut, at_frame, &a);
    if(!a.hdr.time_ut)
        sources = apps_recorder_foreach_frame(0, at_ut, at_frame, &a);

    if(!a.hdr.time_ut) {
        pluginsd_function_json_error_to_stdout(transaction, HTTP_RESP_NOT_FOUND,
                                               "The process flight recorder has no sample at or before this time.");
        freez(a.rows);
        return;
    }

    coverage_add(&a.coverage, &a.hdr);
    a.coverage.sample_ut = a.hdr.time_ut;

    BUFFER *wb = response_begin("The recorded processes table at a moment in the past.");
    emit_recorded(wb, "at", at_ut, at_ut, &a.coverage, sources);

    buffer_json_member_add_array(wb, "data");
    for(size_t i = 0; i < a.used; i++) {
        const REC_AT_ROW *r = &a.rows[i];
        char state[2] = { r->row.state ? (char)r->row.state : '-', '\0' };

        buffer_json_add_array_item_array(wb);
        buffer_json_add_array_item_uint64(wb, (uint64_t)r->row.pid);
        add_string_or_dash(wb, r->comm);
        add_string_or_dash(wb, r->exe);
        if(show_cmdline) add_string_or_dash(wb, r->cmdline);
        buffer_json_add_array_item_uint64(wb, (uint64_t)r->row.ppid);
        add_string_or_dash(wb, r->group);
        buffer_json_add_array_item_uint64(wb, r->row.uid);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)r->row.cpu / 100.0);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)r->row.rss_kib / 1024.0);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)r->row.swap_kib / 1024.0);
        buffer_json_add_array_item_uint64(wb, r->row.lread_kibs);
        buffer_json_add_array_item_uint64(wb, r->row.lwrite_kibs);
        buffer_json_add_array_item_uint64(wb, r->row.pread_kibs);
        buffer_json_add_array_item_uint64(wb, r->row.pwrite_kibs);
        buffer_json_add_array_item_uint64(wb, r->row.threads);
        buffer_json_add_array_item_string(wb, state);
        buffer_json_add_array_item_string(wb, why_string(r->row.why));
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    emit_columns(wb, at_columns, _countof(at_columns), show_cmdline);
    response_send(transaction, wb, "CPU");

    at_rows_free(&a);
    freez(a.rows);
}

// --------------------------------------------------------------------------------------------------------------------
// after:A before:B

typedef struct {
    int32_t pid;
    uint64_t starttime;
    uint64_t boot;              // the first 8 bytes of the boot id
    bool used;

    int32_t ppid;
    uint32_t uid;
    STRING *comm, *exe, *group, *cmdline;

    size_t samples;
    uint64_t cpu_sum;           // 1/10000 of a core
    uint32_t cpu_max;
    uint32_t rss_max_kib;
    usec_t first_ut, last_ut;
    bool started, exited;
} REC_AGG;

typedef struct {
    REC_AGG *slots;
    size_t size;                // power of 2
    size_t used;
    REC_COVERAGE coverage;
    size_t exits_unknown;       // exits of processes the range has no identity for
} REC_RANGE;

static inline size_t agg_slot(int32_t pid, uint64_t starttime, uint64_t boot, size_t size) {
    uint64_t h = ((uint64_t)(uint32_t)pid * 0x9e3779b97f4a7c15ULL) ^ (starttime * 0xc2b2ae3d27d4eb4fULL) ^ boot;
    h ^= h >> 31;
    return (size_t)h & (size - 1);
}

static REC_AGG *agg_find(REC_RANGE *r, int32_t pid, uint64_t starttime, uint64_t boot, bool create);

static void agg_grow(REC_RANGE *r) {
    REC_AGG *old = r->slots;
    size_t old_size = r->size;
    r->size = old_size ? old_size * 2 : 1024;
    r->slots = callocz(r->size, sizeof(*r->slots));
    r->used = 0;
    for(size_t i = 0; i < old_size; i++) {
        if(!old[i].used) continue;
        REC_AGG *a = agg_find(r, old[i].pid, old[i].starttime, old[i].boot, true);
        *a = old[i];
    }
    freez(old);
}

static REC_AGG *agg_find(REC_RANGE *r, int32_t pid, uint64_t starttime, uint64_t boot, bool create) {
    if(create && (r->used + 1) * 2 > r->size)
        agg_grow(r);

    if(!r->size)
        return NULL;

    size_t h = agg_slot(pid, starttime, boot, r->size);
    while(r->slots[h].used) {
        if(r->slots[h].pid == pid && r->slots[h].starttime == starttime && r->slots[h].boot == boot)
            return &r->slots[h];
        h = (h + 1) & (r->size - 1);
    }

    if(!create)
        return NULL;

    REC_AGG *a = &r->slots[h];
    *a = (REC_AGG){ .pid = pid, .starttime = starttime, .boot = boot, .used = true };
    r->used++;
    return a;
}

static void agg_identity(REC_AGG *a, const APPS_REC_STRINGS *strings, int32_t ppid, uint32_t uid,
                         uint16_t comm, uint16_t exe, uint16_t group, uint16_t cmdline) {
    a->ppid = ppid;
    a->uid = uid;
    if(!a->comm) a->comm = dup_string(strings, comm);
    if(!a->exe) a->exe = dup_string(strings, exe);
    if(!a->group) a->group = dup_string(strings, group);
    if(!a->cmdline) a->cmdline = dup_string(strings, cmdline);
}

static bool range_frame(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    REC_RANGE *r = data;
    usec_t t = f->hdr->time_ut;
    uint64_t boot;
    memcpy(&boot, f->boot_id, sizeof(boot));
    coverage_add(&r->coverage, f->hdr);

    for(uint16_t i = 0; i < f->hdr->starts; i++) {
        APPS_REC_START s = f->starts[i];
        REC_AGG *a = agg_find(r, s.pid, s.starttime, boot, true);
        if(!a->first_ut) a->first_ut = t;
        a->last_ut = MAX(a->last_ut, t);
        a->started = true;
        agg_identity(a, strings, s.ppid, s.uid, s.comm, s.exe, s.group, s.cmdline);
    }

    for(uint16_t i = 0; i < f->hdr->rows; i++) {
        APPS_REC_ROW row = f->rows[i];
        REC_AGG *a = agg_find(r, row.pid, row.starttime, boot, true);
        if(!a->first_ut) a->first_ut = t;
        a->last_ut = MAX(a->last_ut, t);
        a->samples++;
        a->cpu_sum += row.cpu;
        a->cpu_max = MAX(a->cpu_max, row.cpu);
        a->rss_max_kib = MAX(a->rss_max_kib, row.rss_kib);
        agg_identity(a, strings, row.ppid, row.uid, row.comm, row.exe, row.group, row.cmdline);
    }

    for(uint16_t i = 0; i < f->hdr->exits; i++) {
        APPS_REC_EXIT e = f->exits[i];
        REC_AGG *a = agg_find(r, e.pid, e.starttime, boot, false);
        if(!a) {
            r->exits_unknown++;
            continue;
        }
        a->exited = true;
        a->last_ut = MAX(a->last_ut, t);
    }

    return true;
}

static int cmp_agg_cpu_desc(const void *x, const void *y) {
    const REC_AGG *a = *(REC_AGG * const *)x, *b = *(REC_AGG * const *)y;
    if(a->cpu_sum != b->cpu_sum) return (a->cpu_sum < b->cpu_sum) - (a->cpu_sum > b->cpu_sum);
    return (a->rss_max_kib < b->rss_max_kib) - (a->rss_max_kib > b->rss_max_kib);
}

static const REC_COLUMN range_columns[] = {
    COL_STR("Key", "Process Identity (pid:starttime:boot)", RRDF_FIELD_OPTS_UNIQUE_KEY | RRDF_FIELD_OPTS_DUMMY),
    COL_INT("PID", "Process ID", OPT_STICKY),
    COL_STR("Cmd", "Process Name", OPT_STICKY),
    COL_STR("Exe", "Executable", OPT_VIS),
    COL_STR("CmdLine", "Command Line", RRDF_FIELD_OPTS_NONE),
    COL_INT("PPID", "Parent Process ID", RRDF_FIELD_OPTS_NONE),
    COL_STR("Category", "Category (apps_groups.conf)", OPT_STICKY),
    COL_INT("Uid", "User ID", RRDF_FIELD_OPTS_NONE),
    COL_NUM("AvgCPU", "Average CPU over the range (100% = 1 core; 0 while not recorded)", 2, "%", RRDF_FIELD_SUMMARY_SUM, OPT_VIS),
    COL_NUM("PeakCPU", "Peak CPU (100% = 1 core)", 2, "%", RRDF_FIELD_SUMMARY_MAX, OPT_VIS),
    COL_NUM("PeakRSS", "Peak Resident Set Size", 2, "MiB", RRDF_FIELD_SUMMARY_MAX, OPT_VIS),
    COL_NUM("Samples", "Samples the process was recorded in", 0, "samples", RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_OPTS_NONE),
    COL_TIME("FirstSeen", "First Seen in the range", OPT_VIS),
    COL_TIME("LastSeen", "Last Seen in the range", OPT_VIS),
    COL_STR("Started", "Started within the range", OPT_VIS),
    COL_STR("Exited", "Exited within the range", OPT_VIS),
};

static void query_range(const char *transaction, usec_t after_ut, usec_t before_ut, bool show_cmdline) {
    REC_RANGE r = { 0 };
    APPS_REC_SOURCE sources = apps_recorder_foreach_frame(after_ut, before_ut, range_frame, &r);

    if(!r.coverage.frames) {
        pluginsd_function_json_error_to_stdout(transaction, HTTP_RESP_NOT_FOUND,
                                               "The process flight recorder has no samples in this time range.");
        freez(r.slots);
        return;
    }

    REC_AGG **sorted = mallocz((r.used + 1) * sizeof(*sorted));
    size_t n = 0;
    for(size_t i = 0; i < r.size; i++)
        if(r.slots[i].used) sorted[n++] = &r.slots[i];
    qsort(sorted, n, sizeof(*sorted), cmp_agg_cpu_desc);
    size_t emitted = MIN(n, (size_t)APPS_REC_QUERY_ROWS_MAX);

    BUFFER *wb = response_begin("The processes recorded in a past time range.");
    emit_recorded(wb, "range", after_ut, before_ut, &r.coverage, sources);
    buffer_json_member_add_uint64(wb, "processes_total", n);
    buffer_json_member_add_uint64(wb, "processes_shown", emitted);
    buffer_json_member_add_uint64(wb, "exits_unidentified", r.exits_unknown);

    buffer_json_member_add_array(wb, "data");
    for(size_t i = 0; i < emitted; i++) {
        const REC_AGG *a = sorted[i];
        char key[80];
        snprintfz(key, sizeof(key), "%d:%" PRIu64 ":%016" PRIx64, a->pid, a->starttime, a->boot);

        buffer_json_add_array_item_array(wb);
        buffer_json_add_array_item_string(wb, key);
        buffer_json_add_array_item_uint64(wb, (uint64_t)a->pid);
        add_string_or_dash(wb, a->comm);
        add_string_or_dash(wb, a->exe);
        if(show_cmdline) add_string_or_dash(wb, a->cmdline);
        buffer_json_add_array_item_uint64(wb, (uint64_t)a->ppid);
        add_string_or_dash(wb, a->group);
        buffer_json_add_array_item_uint64(wb, a->uid);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)a->cpu_sum / (NETDATA_DOUBLE)r.coverage.frames / 100.0);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)a->cpu_max / 100.0);
        buffer_json_add_array_item_double(wb, (NETDATA_DOUBLE)a->rss_max_kib / 1024.0);
        buffer_json_add_array_item_uint64(wb, a->samples);
        buffer_json_add_array_item_uint64(wb, a->first_ut);
        buffer_json_add_array_item_uint64(wb, a->last_ut);
        buffer_json_add_array_item_string(wb, a->started ? "yes" : "no");
        buffer_json_add_array_item_string(wb, a->exited ? "yes" : "no");
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    emit_columns(wb, range_columns, _countof(range_columns), show_cmdline);
    response_send(transaction, wb, "AvgCPU");

    for(size_t i = 0; i < r.size; i++) {
        if(!r.slots[i].used) continue;
        string_freez(r.slots[i].comm);
        string_freez(r.slots[i].exe);
        string_freez(r.slots[i].group);
        string_freez(r.slots[i].cmdline);
    }
    freez(sorted);
    freez(r.slots);
}

// --------------------------------------------------------------------------------------------------------------------

bool apps_recorder_processes_wants_history(char **words, size_t num_words) {
    for(size_t i = 1; i < num_words; i++) {
        const char *w = get_word(words, num_words, i);
        if(!w) break;
        if(strncmp(w, "at:", 3) == 0 || strncmp(w, "after:", 6) == 0 || strncmp(w, "before:", 7) == 0)
            return true;
    }
    return false;
}

void apps_recorder_function_processes(const char *transaction, char **words, size_t num_words, bool show_cmdline) {
    if(!apps_recorder_enabled) {
        pluginsd_function_json_error_to_stdout(transaction, HTTP_RESP_SERVICE_UNAVAILABLE,
                                               "The process flight recorder is disabled.");
        return;
    }

    time_t now_s = now_realtime_sec();
    time_t at_s = 0, after_s = 0, before_s = 0;
    bool have_at = false, have_after = false, have_before = false, ok = true;

    for(size_t i = 1; i < num_words && ok; i++) {
        const char *w = get_word(words, num_words, i);
        if(!w) break;
        if(strncmp(w, "at:", 3) == 0)
            ok = have_at = apps_recorder_parse_time(&w[3], now_s, &at_s);
        else if(strncmp(w, "after:", 6) == 0)
            ok = have_after = apps_recorder_parse_time(&w[6], now_s, &after_s);
        else if(strncmp(w, "before:", 7) == 0)
            ok = have_before = apps_recorder_parse_time(&w[7], now_s, &before_s);
    }

    if(!ok || (have_at && (have_after || have_before))) {
        pluginsd_function_json_error_to_stdout(transaction, HTTP_RESP_BAD_REQUEST,
                                               "Use either at:TIME, or after:TIME and/or before:TIME.");
        return;
    }

    if(have_at) {
        query_at(transaction, (usec_t)MAX(at_s, 0) * USEC_PER_SEC, show_cmdline);
        return;
    }

    if(!have_before) before_s = now_s;
    if(!have_after) after_s = before_s - APPS_REC_QUERY_RANGE_DEFAULT_S;
    if(after_s < 0 || after_s >= before_s) {
        pluginsd_function_json_error_to_stdout(transaction, HTTP_RESP_BAD_REQUEST,
                                               "after: must be earlier than before:.");
        return;
    }

    query_range(transaction, (usec_t)after_s * USEC_PER_SEC, (usec_t)before_s * USEC_PER_SEC, show_cmdline);
}

#endif
