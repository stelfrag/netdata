// SPDX-License-Identifier: GPL-3.0-or-later

// `processes` with a time parameter answers from the flight recorder instead of the live process table:
//
//   at:T                the table of the recorded sample at or before T
//   after:A before:B    the processes seen in [A, B]: average and peak CPU, peak memory, first and last seen,
//                       started and exited within the range
//
// Times are unix epochs, or relative to now when zero or negative. The recorder keeps only the processes that hold
// most of the CPU, the largest by memory and by I/O, and every start and exit, so each answer reports the share of
// process CPU its samples covered. Both answers add `files`: the regular files the I/O rows held open for writing.
// Range answers add `connection_events`: the TCP connections processes opened and closed.

#include "apps_recorder.h"
#include "apps_recorder_conns.h"

#if defined(OS_LINUX)

#define APPS_REC_QUERY_RANGE_DEFAULT_S  600
#define APPS_REC_QUERY_AT_LOOKBACK_S    120
#define APPS_REC_QUERY_ROWS_MAX         1000
#define APPS_REC_QUERY_GAPS_LISTED      16
#define APPS_REC_QUERY_FILES_MAX        100
#define APPS_REC_QUERY_CONNS_MAX        1000    // the newest connection events listed
#define APPS_REC_QUERY_CONN_PROCS_MAX   100

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

// time in a range with no samples (nothing saved, or the agent was not running): exits there are unknown
typedef struct {
    usec_t threshold_ut;            // a step between samples longer than this is a gap
    usec_t prev_ut;                 // the last sample walked
    usec_t last_gap_from_ut;        // where the latest gap began (the sample before it); 0 without gaps
    size_t count;
    usec_t total_ut;
    struct { usec_t from_ut, to_ut; } listed[APPS_REC_QUERY_GAPS_LISTED];
} REC_GAPS;

static void gaps_add(REC_GAPS *g, usec_t from_ut, usec_t to_ut) {
    if(to_ut <= from_ut || to_ut - from_ut <= g->threshold_ut)
        return;

    if(g->count < APPS_REC_QUERY_GAPS_LISTED) {
        g->listed[g->count].from_ut = from_ut;
        g->listed[g->count].to_ut = to_ut;
    }
    g->count++;
    g->total_ut += to_ut - from_ut;
    g->last_gap_from_ut = from_ut;
}

// whether the time asked is on disk; "reason" and "in_ram" only when a save covered it and could not be written
static void emit_save(BUFFER *wb, usec_t from_ut, usec_t to_ut) {
    const char *reason;
    APPS_REC_SAVE save = apps_recorder_save_status(from_ut, to_ut, &reason);
    buffer_json_member_add_object(wb, "save");
    buffer_json_member_add_string(wb, "status", apps_recorder_save_str(save));
    if(reason) {
        buffer_json_member_add_string(wb, "reason", reason);
        buffer_json_member_add_boolean(wb, "in_ram", save == APPS_REC_SAVE_IN_RAM);
    }
    buffer_json_object_close(wb);
}

// a 404 that still says whether a save covered the time and why it is not on disk
static void not_found(const char *transaction, const char *msg, usec_t from_ut, usec_t to_ut) {
    BUFFER *wb = buffer_create(256, NULL);
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_MINIFY);
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_NOT_FOUND);
    buffer_json_member_add_string(wb, "error_message", msg);
    emit_save(wb, from_ut, to_ut);
    buffer_json_finalize(wb);
    wb->response_code = HTTP_RESP_NOT_FOUND;
    wb->content_type = CT_APPLICATION_JSON;
    wb->expires = now_realtime_sec();
    pluginsd_function_result_to_stdout(transaction, wb);
    buffer_free(wb);
}

static void emit_recorded(BUFFER *wb, const char *mode, usec_t from_ut, usec_t to_ut,
                          const REC_COVERAGE *c, APPS_REC_SOURCE sources, const REC_GAPS *gaps) {
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
        if(gaps) {
            buffer_json_member_add_object(wb, "gaps");
            buffer_json_member_add_uint64(wb, "count", gaps->count);
            buffer_json_member_add_double(wb, "seconds", (double)gaps->total_ut / USEC_PER_SEC);
            buffer_json_member_add_array(wb, "ranges");
            for(size_t i = 0; i < gaps->count && i < APPS_REC_QUERY_GAPS_LISTED; i++) {
                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_uint64(wb, "from_ut", gaps->listed[i].from_ut);
                buffer_json_member_add_uint64(wb, "to_ut", gaps->listed[i].to_ut);
                buffer_json_object_close(wb);
            }
            buffer_json_array_close(wb);
            buffer_json_object_close(wb);
        }

        buffer_json_member_add_array(wb, "source");
        if(sources & APPS_REC_SOURCE_DISK) buffer_json_add_array_item_string(wb, "disk");
        if(sources & APPS_REC_SOURCE_RAM) buffer_json_add_array_item_string(wb, "ram");
        buffer_json_array_close(wb);

        // at: from the sample used to the moment asked, so a sample older than the moment reports what lies between
        emit_save(wb, c->sample_ut ? MIN(c->sample_ut, from_ut) : from_ut, to_ut);
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
    APPS_REC_FILE file;
    STRING *path, *mount;
} REC_AT_FILE;

typedef struct {
    REC_AT_ROW *rows;
    size_t used;
    size_t size;
    REC_AT_FILE *files;
    size_t files_used;
    size_t files_size;
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

    for(size_t i = 0; i < a->files_used; i++) {
        string_freez(a->files[i].path);
        string_freez(a->files[i].mount);
    }
    a->files_used = 0;
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

    if(f->hdr->files > a->files_size) {
        a->files_size = f->hdr->files;
        a->files = reallocz(a->files, a->files_size * sizeof(*a->files));
    }

    for(uint16_t i = 0; i < f->hdr->files; i++) {
        APPS_REC_FILE file = f->files[i];
        a->files[a->files_used++] = (REC_AT_FILE){
            .file = file,
            .path = dup_string(strings, file.path),
            .mount = dup_string(strings, file.mount),
        };
    }

    a->hdr = *f->hdr;
    return true;
}

static void emit_at_files(BUFFER *wb, const REC_AT *a) {
    buffer_json_member_add_object(wb, "files");
    buffer_json_member_add_boolean(wb, "recorded", a->hdr.flags & APPS_REC_FRAME_FILES);
    buffer_json_member_add_array(wb, "list");
    for(size_t i = 0; i < a->files_used; i++) {
        const REC_AT_FILE *f = &a->files[i];
        const REC_AT_ROW *r = &a->rows[f->file.row];   // the reader checked the row index

        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_uint64(wb, "pid", (uint64_t)r->row.pid);
        buffer_json_member_add_string(wb, "process", r->comm ? string2str(r->comm) : "-");
        buffer_json_member_add_string(wb, "path", f->path ? string2str(f->path) : "-");
        buffer_json_member_add_string_or_omit(wb, "mount", f->mount ? string2str(f->mount) : NULL);
        buffer_json_member_add_uint64(wb, "bytes", f->file.size);
        buffer_json_member_add_boolean(wb, "deleted", f->file.flags & APPS_REC_FILE_DELETED);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
    buffer_json_object_close(wb);
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
    COL_NUM("Sockets", "Open Sockets", 0, "sockets", RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_OPTS_NONE),
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
        not_found(transaction, "The process flight recorder has no sample at or before this time.", at_ut, at_ut);
        freez(a.rows);
        freez(a.files);
        return;
    }

    coverage_add(&a.coverage, &a.hdr);
    a.coverage.sample_ut = a.hdr.time_ut;

    BUFFER *wb = response_begin("The recorded processes table at a moment in the past.");
    emit_recorded(wb, "at", at_ut, at_ut, &a.coverage, sources, NULL);

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
        buffer_json_add_array_item_uint64(wb, r->row.sockets);
        buffer_json_add_array_item_string(wb, state);
        buffer_json_add_array_item_string(wb, why_string(r->row.why));
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    emit_at_files(wb, &a);
    emit_columns(wb, at_columns, _countof(at_columns), show_cmdline);
    response_send(transaction, wb, "CPU");

    at_rows_free(&a);
    freez(a.rows);
    freez(a.files);
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
    uint16_t sockets_max;
    usec_t first_ut, last_ut;
    bool started, exited;
} REC_AGG;

// a connection event of the range, with its time and process identity
typedef struct {
    usec_t time_ut;
    uint64_t boot;
    APPS_REC_CONN c;
    STRING *comm;
} REC_RANGE_CONN;

// the connections of one process (or of the unknown ones) over the range
typedef struct {
    int32_t pid;
    uint64_t starttime;
    uint64_t boot;
    STRING *comm;
    size_t opens, closes;
    uint64_t bytes_sent, bytes_received;    // of the closes
    bool used;
} REC_CONN_PROC;

// a file a process held open for writing during the range
typedef struct {
    int32_t pid;
    uint64_t starttime;
    uint64_t boot;
    STRING *path;               // NULL: a free slot
    STRING *mount;
    uint64_t bytes_first, bytes_last;
    usec_t first_ut, last_ut;
    bool deleted;               // in the last sample
} REC_RANGE_FILE;

typedef struct {
    REC_AGG *slots;
    size_t size;                // power of 2
    size_t used;
    REC_COVERAGE coverage;
    REC_GAPS gaps;
    size_t exits_unknown;       // exits of processes the range has no identity for

    REC_RANGE_FILE *files;
    size_t files_size;          // power of 2
    size_t files_used;
    bool files_recorded;        // a frame of the range looked at files

    REC_RANGE_CONN *conns;      // ring of the newest APPS_REC_QUERY_CONNS_MAX
    size_t conns_total;
    size_t conns_lost;
    uint16_t netns_other;
    bool conns_recorded;
    REC_CONN_PROC *conn_procs;  // open addressing by process
    size_t conn_procs_size;     // power of 2
    size_t conn_procs_used;
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

static inline size_t range_file_slot(int32_t pid, uint64_t starttime, uint64_t boot, STRING *path, size_t size) {
    uint64_t h = ((uint64_t)(uint32_t)pid * 0x9e3779b97f4a7c15ULL) ^ (starttime * 0xc2b2ae3d27d4eb4fULL) ^ boot ^
                 ((uint64_t)(uintptr_t)path * 0xff51afd7ed558ccdULL);
    h ^= h >> 31;
    return (size_t)h & (size - 1);
}

static REC_RANGE_FILE *range_file_find(REC_RANGE *r, int32_t pid, uint64_t starttime, uint64_t boot, STRING *path) {
    if((r->files_used + 1) * 2 > r->files_size) {
        REC_RANGE_FILE *old = r->files;
        size_t old_size = r->files_size;
        r->files_size = old_size ? old_size * 2 : 256;
        r->files = callocz(r->files_size, sizeof(*r->files));
        for(size_t i = 0; i < old_size; i++) {
            if(!old[i].path) continue;
            size_t h = range_file_slot(old[i].pid, old[i].starttime, old[i].boot, old[i].path, r->files_size);
            while(r->files[h].path) h = (h + 1) & (r->files_size - 1);
            r->files[h] = old[i];
        }
        freez(old);
    }

    size_t h = range_file_slot(pid, starttime, boot, path, r->files_size);
    while(r->files[h].path) {
        REC_RANGE_FILE *x = &r->files[h];
        if(x->path == path && x->pid == pid && x->starttime == starttime && x->boot == boot)
            return x;
        h = (h + 1) & (r->files_size - 1);
    }

    r->files[h] = (REC_RANGE_FILE){ .pid = pid, .starttime = starttime, .boot = boot, .path = string_dup(path) };
    r->files_used++;
    return &r->files[h];
}

static void range_files(REC_RANGE *r, const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, uint64_t boot) {
    if(f->hdr->flags & APPS_REC_FRAME_FILES)
        r->files_recorded = true;

    usec_t t = f->hdr->time_ut;
    for(uint16_t i = 0; i < f->hdr->files; i++) {
        APPS_REC_FILE file = f->files[i];
        APPS_REC_ROW row = f->rows[file.row];   // the reader checked the row index

        STRING *path = dup_string(strings, file.path);
        if(!path)
            continue;

        REC_RANGE_FILE *x = range_file_find(r, row.pid, row.starttime, boot, path);
        string_freez(path);

        if(!x->first_ut) {
            x->first_ut = t;
            x->bytes_first = file.size;
        }
        x->last_ut = t;
        x->bytes_last = file.size;
        x->deleted = file.flags & APPS_REC_FILE_DELETED;
        if(!x->mount)
            x->mount = dup_string(strings, file.mount);
    }
}

static inline uint64_t range_file_growth(const REC_RANGE_FILE *x) {
    return x->bytes_last > x->bytes_first ? x->bytes_last - x->bytes_first : 0;
}

static int cmp_range_file_desc(const void *a, const void *b) {
    const REC_RANGE_FILE *x = *(REC_RANGE_FILE * const *)a, *y = *(REC_RANGE_FILE * const *)b;
    uint64_t gx = range_file_growth(x), gy = range_file_growth(y);
    if(gx != gy) return (gx < gy) - (gx > gy);
    return (x->bytes_last < y->bytes_last) - (x->bytes_last > y->bytes_last);
}

// the files of the range, the most grown first, then the largest
static void emit_range_files(BUFFER *wb, REC_RANGE *r) {
    REC_RANGE_FILE **sorted = mallocz((r->files_used + 1) * sizeof(*sorted));
    size_t n = 0;
    for(size_t i = 0; i < r->files_size; i++)
        if(r->files[i].path) sorted[n++] = &r->files[i];
    qsort(sorted, n, sizeof(*sorted), cmp_range_file_desc);

    buffer_json_member_add_object(wb, "files");
    buffer_json_member_add_boolean(wb, "recorded", r->files_recorded);
    buffer_json_member_add_uint64(wb, "total", n);
    buffer_json_member_add_array(wb, "list");
    for(size_t i = 0; i < n && i < APPS_REC_QUERY_FILES_MAX; i++) {
        const REC_RANGE_FILE *x = sorted[i];
        const REC_AGG *a = agg_find(r, x->pid, x->starttime, x->boot, false);
        char key[80];
        snprintfz(key, sizeof(key), "%d:%" PRIu64 ":%016" PRIx64, x->pid, x->starttime, x->boot);

        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_string(wb, "key", key);
        buffer_json_member_add_uint64(wb, "pid", (uint64_t)x->pid);
        buffer_json_member_add_string(wb, "process", (a && a->comm) ? string2str(a->comm) : "-");
        buffer_json_member_add_string(wb, "path", string2str(x->path));
        buffer_json_member_add_string_or_omit(wb, "mount", x->mount ? string2str(x->mount) : NULL);
        buffer_json_member_add_uint64(wb, "bytes_first", x->bytes_first);
        buffer_json_member_add_uint64(wb, "bytes_last", x->bytes_last);
        buffer_json_member_add_uint64(wb, "first_ut", x->first_ut);
        buffer_json_member_add_uint64(wb, "last_ut", x->last_ut);
        buffer_json_member_add_boolean(wb, "deleted", x->deleted);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
    buffer_json_object_close(wb);

    freez(sorted);
}

static void range_files_free(REC_RANGE *r) {
    for(size_t i = 0; i < r->files_size; i++) {
        string_freez(r->files[i].path);
        string_freez(r->files[i].mount);
    }
    freez(r->files);
}

static REC_CONN_PROC *conn_proc_find(REC_RANGE *r, int32_t pid, uint64_t starttime, uint64_t boot) {
    if((r->conn_procs_used + 1) * 2 > r->conn_procs_size) {
        REC_CONN_PROC *old = r->conn_procs;
        size_t old_size = r->conn_procs_size;
        r->conn_procs_size = old_size ? old_size * 2 : 64;
        r->conn_procs = callocz(r->conn_procs_size, sizeof(*r->conn_procs));
        for(size_t i = 0; i < old_size; i++) {
            if(!old[i].used) continue;
            size_t h = agg_slot(old[i].pid, old[i].starttime, old[i].boot, r->conn_procs_size);
            while(r->conn_procs[h].used) h = (h + 1) & (r->conn_procs_size - 1);
            r->conn_procs[h] = old[i];
        }
        freez(old);
    }

    size_t h = agg_slot(pid, starttime, boot, r->conn_procs_size);
    while(r->conn_procs[h].used) {
        REC_CONN_PROC *x = &r->conn_procs[h];
        if(x->pid == pid && x->starttime == starttime && x->boot == boot)
            return x;
        h = (h + 1) & (r->conn_procs_size - 1);
    }

    r->conn_procs[h] = (REC_CONN_PROC){ .pid = pid, .starttime = starttime, .boot = boot, .used = true };
    r->conn_procs_used++;
    return &r->conn_procs[h];
}

static void range_conns(REC_RANGE *r, const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, uint64_t boot) {
    if(!(f->hdr->flags & APPS_REC_FRAME_CONNS))
        return;

    r->conns_recorded = true;
    r->conns_lost += f->hdr->conns_lost;
    r->netns_other = MAX(r->netns_other, f->hdr->netns_other);

    if(!r->conns && f->hdr->conns)
        r->conns = callocz(APPS_REC_QUERY_CONNS_MAX, sizeof(*r->conns));

    for(uint16_t i = 0; i < f->hdr->conns; i++) {
        APPS_REC_CONN c;
        memcpy(&c, &f->conns[i], sizeof(c));

        // the unknown processes are one entry (pid 0)
        REC_CONN_PROC *p = conn_proc_find(r, c.pid, c.pid ? c.starttime : 0, c.pid ? boot : 0);
        if(!p->comm && c.pid)
            p->comm = dup_string(strings, c.comm);
        if(c.event == APPS_REC_CONN_OPEN)
            p->opens++;
        else {
            p->closes++;
            p->bytes_sent += c.bytes_sent;
            p->bytes_received += c.bytes_received;
        }

        REC_RANGE_CONN *x = &r->conns[r->conns_total % APPS_REC_QUERY_CONNS_MAX];
        string_freez(x->comm);
        *x = (REC_RANGE_CONN){ .time_ut = f->hdr->time_ut, .boot = boot, .c = c, .comm = dup_string(strings, c.comm) };
        r->conns_total++;
    }
}

static int cmp_conn_proc_desc(const void *a, const void *b) {
    const REC_CONN_PROC *x = *(REC_CONN_PROC * const *)a, *y = *(REC_CONN_PROC * const *)b;
    size_t ex = x->opens + x->closes, ey = y->opens + y->closes;
    if(ex != ey) return (ex < ey) - (ex > ey);
    uint64_t bx = x->bytes_sent + x->bytes_received, by = y->bytes_sent + y->bytes_received;
    return (bx < by) - (bx > by);
}

static void emit_conn_process(BUFFER *wb, int32_t pid, uint64_t starttime, uint64_t boot, STRING *comm) {
    if(!pid) {
        buffer_json_member_add_string(wb, "process", "unknown");
        return;
    }

    char key[80];
    snprintfz(key, sizeof(key), "%d:%" PRIu64 ":%016" PRIx64, pid, starttime, boot);
    buffer_json_member_add_string(wb, "key", key);
    buffer_json_member_add_uint64(wb, "pid", (uint64_t)pid);
    buffer_json_member_add_string(wb, "process", comm ? string2str(comm) : "-");
}

// the connections of the range: per process the most active first, then the newest events, oldest first
static void emit_range_conns(BUFFER *wb, REC_RANGE *r) {
    buffer_json_member_add_object(wb, "connection_events");
    buffer_json_member_add_boolean(wb, "recorded", r->conns_recorded);
    buffer_json_member_add_uint64(wb, "total", r->conns_total);
    buffer_json_member_add_uint64(wb, "lost", r->conns_lost);
    buffer_json_member_add_uint64(wb, "netns_other", r->netns_other);

    REC_CONN_PROC **sorted = mallocz((r->conn_procs_used + 1) * sizeof(*sorted));
    size_t n = 0;
    for(size_t i = 0; i < r->conn_procs_size; i++)
        if(r->conn_procs[i].used) sorted[n++] = &r->conn_procs[i];
    qsort(sorted, n, sizeof(*sorted), cmp_conn_proc_desc);

    buffer_json_member_add_array(wb, "by_process");
    for(size_t i = 0; i < n && i < APPS_REC_QUERY_CONN_PROCS_MAX; i++) {
        const REC_CONN_PROC *p = sorted[i];
        buffer_json_add_array_item_object(wb);
        emit_conn_process(wb, p->pid, p->starttime, p->boot, p->comm);
        buffer_json_member_add_uint64(wb, "opens", p->opens);
        buffer_json_member_add_uint64(wb, "closes", p->closes);
        buffer_json_member_add_uint64(wb, "bytes_sent", p->bytes_sent);
        buffer_json_member_add_uint64(wb, "bytes_received", p->bytes_received);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
    freez(sorted);

    size_t listed = MIN(r->conns_total, (size_t)APPS_REC_QUERY_CONNS_MAX);
    buffer_json_member_add_array(wb, "list");
    for(size_t i = r->conns_total - listed; i < r->conns_total; i++) {
        const REC_RANGE_CONN *x = &r->conns[i % APPS_REC_QUERY_CONNS_MAX];
        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_uint64(wb, "time_ut", x->time_ut);
        emit_conn_process(wb, x->c.pid, x->c.starttime, x->boot, x->comm);
        apps_rec_conn_to_json(wb, &x->c);
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
    buffer_json_member_add_boolean(wb, "truncated", r->conns_total > listed);
    buffer_json_object_close(wb);
}

static void range_conns_free(REC_RANGE *r) {
    if(r->conns)
        for(size_t i = 0; i < APPS_REC_QUERY_CONNS_MAX; i++)
            string_freez(r->conns[i].comm);
    freez(r->conns);
    for(size_t i = 0; i < r->conn_procs_size; i++)
        string_freez(r->conn_procs[i].comm);
    freez(r->conn_procs);
}

static bool range_frame(const APPS_REC_FRAME_VIEW *f, const APPS_REC_STRINGS *strings, void *data) {
    REC_RANGE *r = data;
    usec_t t = f->hdr->time_ut;
    uint64_t boot;
    memcpy(&boot, f->boot_id, sizeof(boot));
    coverage_add(&r->coverage, f->hdr);

    // the frames arrive in time order
    gaps_add(&r->gaps, r->gaps.prev_ut, t);
    r->gaps.prev_ut = MAX(r->gaps.prev_ut, t);

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
        a->sockets_max = MAX(a->sockets_max, row.sockets);
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

    range_files(r, f, strings, boot);
    range_conns(r, f, strings, boot);
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
    COL_NUM("PeakSockets", "Peak Open Sockets", 0, "sockets", RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_OPTS_NONE),
    COL_NUM("Samples", "Samples the process was recorded in", 0, "samples", RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_OPTS_NONE),
    COL_TIME("FirstSeen", "First Seen in the range", OPT_VIS),
    COL_TIME("LastSeen", "Last Seen in the range", OPT_VIS),
    COL_STR("Started", "Started within the range", OPT_VIS),
    COL_STR("Exited", "Exited within the range (unknown: last seen before a time with no samples)", OPT_VIS),
};

static void query_range(const char *transaction, usec_t after_ut, usec_t before_ut, bool show_cmdline) {
    REC_RANGE r = { 0 };
    r.gaps.threshold_ut = (usec_t)MAX(3 * update_every, 5) * USEC_PER_SEC;
    r.gaps.prev_ut = after_ut;      // a gap at the start of the range
    APPS_REC_SOURCE sources = apps_recorder_foreach_frame(after_ut, before_ut, range_frame, &r);

    // a gap at the end of the range, up to now
    gaps_add(&r.gaps, r.gaps.prev_ut, MIN(before_ut, now_realtime_usec()));

    if(!r.coverage.frames) {
        not_found(transaction, "The process flight recorder has no samples in this time range.", after_ut, before_ut);
        freez(r.slots);
        range_files_free(&r);
        range_conns_free(&r);
        return;
    }

    REC_AGG **sorted = mallocz((r.used + 1) * sizeof(*sorted));
    size_t n = 0;
    for(size_t i = 0; i < r.size; i++)
        if(r.slots[i].used) sorted[n++] = &r.slots[i];
    qsort(sorted, n, sizeof(*sorted), cmp_agg_cpu_desc);
    size_t emitted = MIN(n, (size_t)APPS_REC_QUERY_ROWS_MAX);

    BUFFER *wb = response_begin("The processes recorded in a past time range.");
    emit_recorded(wb, "range", after_ut, before_ut, &r.coverage, sources, &r.gaps);
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
        buffer_json_add_array_item_uint64(wb, a->sockets_max);
        buffer_json_add_array_item_uint64(wb, a->samples);
        buffer_json_add_array_item_uint64(wb, a->first_ut);
        buffer_json_add_array_item_uint64(wb, a->last_ut);
        buffer_json_add_array_item_string(wb, a->started ? "yes" : "no");
        // every exit is recorded while sampling; after a gap the process may have exited unseen
        buffer_json_add_array_item_string(wb, a->exited ? "yes" : (r.gaps.last_gap_from_ut >= a->last_ut ? "unknown" : "no"));
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    emit_range_files(wb, &r);
    emit_range_conns(wb, &r);
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
    range_files_free(&r);
    range_conns_free(&r);
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
