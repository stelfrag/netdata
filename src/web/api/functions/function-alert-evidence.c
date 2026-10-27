// SPDX-License-Identifier: GPL-3.0-or-later

#include "function-alert-evidence.h"
#include "daemon/alert-evidence.h"

// With a transition, the answer is a JSON document, not a table: one section per kind of evidence, each with a status.
// The processes come from the apps.plugin flight recorder, the connections from the snapshots taken when the save was
// triggered, the host changes from the systemd-journal Function; the journal range and the correlated metrics are
// pointers. Without a transition (the dashboard lists the Function and calls it bare), the answer is a table with an
// alert picker: by default this agent's recent WARNING and CRITICAL transitions, or for one picked alert the processes
// around it.

#define AE_HOST_CHANGE_MSGID        "adfd0357c3d6490fb5c7cd9337492f11"
#define AE_PROCESS_ROWS_MAX         50
#define AE_CONNECTION_ROWS_MAX      1000
#define AE_FILE_ROWS_MAX            20
#define AE_CONN_EVENTS_MAX          100     // the newest connection events of the window
#define AE_CONN_PROCESSES_MAX       20
#define AE_HOST_CHANGES_MAX         100
#define AE_AT_MAX_AGE_S             60      // processes_at answers with the newest sample before T: older is not evidence
#define AE_LIST_WINDOW_S            86400   // the transitions table without a transition
#define AE_LIST_ROWS_MAX            500
#define AE_PICKER_OPTIONS_MAX       100     // alerts offered in the dashboard's picker

// what the inner calls may see: the access this Function requires; command lines stay hidden (no VIEW_AGENT_CONFIG)
#define AE_INNER_ACCESS (HTTP_ACCESS_SIGNED_ID | HTTP_ACCESS_SAME_SPACE | HTTP_ACCESS_SENSITIVE_DATA)

struct ae_transition {
    bool found;
    nd_uuid_t host_id;
    char *alert;
    char *chart;
    char *context;
    char *units;
    char *summary;
    char *classification;
    char *component;
    char *type;
    time_t when;
    int old_status;
    int new_status;
    NETDATA_DOUBLE old_value;
    NETDATA_DOUBLE new_value;
};

static void ae_transition_cb(struct sql_alert_transition_data *t, void *data) {
    struct ae_transition *tr = data;
    if(tr->found)
        return;

    tr->found = true;
    uuid_copy(tr->host_id, *t->host_id);
    tr->alert = strdupz(t->alert_name ? t->alert_name : "");
    tr->chart = strdupz(t->chart ? t->chart : "");
    tr->context = strdupz(t->chart_context ? t->chart_context : "");
    tr->units = strdupz(t->units ? t->units : "");
    tr->summary = strdupz(t->summary ? t->summary : "");
    tr->classification = strdupz(t->classification ? t->classification : "");
    tr->component = strdupz(t->component ? t->component : "");
    tr->type = strdupz(t->type ? t->type : "");
    tr->when = t->when_key;
    tr->old_status = t->old_status;
    tr->new_status = t->new_status;
    tr->old_value = t->old_value;
    tr->new_value = t->new_value;
}

static void ae_transition_free(struct ae_transition *tr) {
    freez(tr->alert);
    freez(tr->chart);
    freez(tr->context);
    freez(tr->units);
    freez(tr->summary);
    freez(tr->classification);
    freez(tr->component);
    freez(tr->type);
}

static int ae_error(BUFFER *wb, int status, const char *error) {
    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);
    buffer_json_member_add_uint64(wb, "status", status);
    buffer_json_member_add_string(wb, "type", "alert-evidence");
    buffer_json_member_add_string(wb, "help", FUNCTION_ALERT_EVIDENCE_HELP);
    buffer_json_member_add_string(wb, "error", error);
    buffer_json_member_add_string(wb, "errorMessage", error); // what the dashboard shows
    buffer_json_finalize(wb);
    return status;
}

// --------------------------------------------------------------------------------------------------------------------
// json-c to BUFFER

static void ae_json_value(BUFFER *wb, const char *key, struct json_object *v) {
    switch(json_object_get_type(v)) {
        case json_type_boolean:
            if(key) buffer_json_member_add_boolean(wb, key, json_object_get_boolean(v));
            else buffer_json_add_array_item_boolean(wb, json_object_get_boolean(v));
            break;

        case json_type_int:
            if(key) buffer_json_member_add_int64(wb, key, json_object_get_int64(v));
            else buffer_json_add_array_item_int64(wb, json_object_get_int64(v));
            break;

        case json_type_double:
            if(key) buffer_json_member_add_double(wb, key, json_object_get_double(v));
            else buffer_json_add_array_item_double(wb, json_object_get_double(v));
            break;

        case json_type_string:
            if(key) buffer_json_member_add_string(wb, key, json_object_get_string(v));
            else buffer_json_add_array_item_string(wb, json_object_get_string(v));
            break;

        case json_type_array:
            if(key) buffer_json_member_add_array(wb, key);
            else buffer_json_add_array_item_array(wb);
            for(size_t i = 0; i < json_object_array_length(v); i++)
                ae_json_value(wb, NULL, json_object_array_get_idx(v, i));
            buffer_json_array_close(wb);
            break;

        case json_type_object:
            if(key) buffer_json_member_add_object(wb, key);
            else buffer_json_add_array_item_object(wb);
            json_object_object_foreach(v, k, child)
                ae_json_value(wb, k, child);
            buffer_json_object_close(wb);
            break;

        default:
        case json_type_null:
            if(key) buffer_json_member_add_string(wb, key, NULL);
            else buffer_json_add_array_item_string(wb, NULL);
            break;
    }
}

struct ae_sort_row {
    struct json_object *row;
    double value;
};

static int ae_sort_row_cmp(const void *a, const void *b) {
    const struct ae_sort_row *x = a, *y = b;
    return (x->value < y->value) - (x->value > y->value);
}

// a table answer (columns + data rows) as an array of objects, largest sort_key first; only the listed columns
// when keys is not NULL; command lines never; returns the number of rows in the answer
static size_t ae_table_rows(BUFFER *wb, const char *key, struct json_object *answer, const char *sort_key,
                            const char *const *keys, size_t max_rows, bool *truncated) {
    struct json_object *columns = NULL, *data = NULL;
    *truncated = false;

    buffer_json_member_add_array(wb, key);
    if(!json_object_object_get_ex(answer, "columns", &columns) || !json_object_is_type(columns, json_type_object) ||
       !json_object_object_get_ex(answer, "data", &data) || !json_object_is_type(data, json_type_array)) {
        buffer_json_array_close(wb);
        return 0;
    }

    size_t ncols = 0;
    json_object_object_foreach(columns, cname, cdef) {
        (void)cname; (void)cdef;
        ncols++;
    }

    const char **names = callocz(ncols ? ncols : 1, sizeof(*names));
    int64_t *indexes = callocz(ncols ? ncols : 1, sizeof(*indexes));
    int64_t sort_index = -1;
    size_t used = 0;
    json_object_object_foreach(columns, name, def) {
        struct json_object *idx;
        if(!json_object_object_get_ex(def, "index", &idx))
            continue;

        bool wanted = !keys;
        for(size_t k = 0; keys && keys[k]; k++)
            if(!strcmp(keys[k], name)) { wanted = true; break; }

        if(sort_key && !strcmp(sort_key, name))
            sort_index = json_object_get_int64(idx);

        // decision 3: command lines need VIEW_AGENT_CONFIG; apps.plugin options can lift that for `processes`, not here
        if(!wanted || !strcmp(name, "CmdLine"))
            continue;

        names[used] = name;
        indexes[used] = json_object_get_int64(idx);
        used++;
    }

    size_t rows = json_object_array_length(data);
    struct ae_sort_row *sorted = callocz(rows ? rows : 1, sizeof(*sorted));
    for(size_t r = 0; r < rows; r++) {
        sorted[r].row = json_object_array_get_idx(data, r);
        struct json_object *v = (sort_index >= 0) ? json_object_array_get_idx(sorted[r].row, sort_index) : NULL;
        sorted[r].value = v ? json_object_get_double(v) : 0;
    }
    if(sort_index >= 0)
        qsort(sorted, rows, sizeof(*sorted), ae_sort_row_cmp);

    for(size_t r = 0; r < rows && r < max_rows; r++) {
        if(!json_object_is_type(sorted[r].row, json_type_array))
            continue;

        buffer_json_add_array_item_object(wb);
        for(size_t c = 0; c < used; c++) {
            struct json_object *v = json_object_array_get_idx(sorted[r].row, indexes[c]);
            if(v && !json_object_is_type(v, json_type_object) && !json_object_is_type(v, json_type_array))
                ae_json_value(wb, names[c], v);
        }
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);

    *truncated = rows > max_rows;

    freez(sorted);
    freez(indexes);
    freez(names);
    return rows;
}

// --------------------------------------------------------------------------------------------------------------------
// inner calls

// calls a Function of this agent; *jobj is the parsed answer on success. *available is false when the Function is
// not registered (its plugin is not running): unavailable evidence, not an error.
// error_body, when not NULL, receives the parsed body of an answer other than 200 (the caller puts it)
static int ae_call(const char *cmd, BUFFER *payload, int timeout_s, const char *source, struct json_object **jobj,
                   bool *available, struct json_object **error_body) {
    *jobj = NULL;
    if(error_body)
        *error_body = NULL;
    CLEAN_BUFFER *result = buffer_create(0, NULL);

    int code = nrpc_method_authorize(rrdhost_nrpc_owner(localhost), result, cmd, AE_INNER_ACCESS, false, NULL);
    *available = (code == HTTP_RESP_OK);
    if(!*available)
        return code;

    buffer_flush(result);
    code = nrpc_call(&(struct nrpc_call_spec) {
        .owner = rrdhost_nrpc_owner(localhost),
        .result_wb = result,
        .cmd = cmd,
        .source = source,
        .user_access = AE_INNER_ACCESS,
        .timeout_s = timeout_s,
        .wait = true,
        .allow_restricted = false,
        .payload = payload,
    });

    if(code == HTTP_RESP_OK)
        *jobj = json_tokener_parse(buffer_tostring(result));
    else if(error_body && buffer_strlen(result))
        *error_body = json_tokener_parse(buffer_tostring(result));

    return code;
}

static const char *ae_json_str(struct json_object *obj, const char *key) {
    struct json_object *v;
    return (obj && json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)) ?
           json_object_get_string(v) : NULL;
}

static const char *ae_status_from_code(int code, bool available, bool parsed) {
    if(!available) return "unavailable";
    if(code == HTTP_RESP_OK && !parsed) return "error";
    switch(code) {
        case HTTP_RESP_OK:                  return "ok";
        case HTTP_RESP_NOT_FOUND:           return "not_saved";
        case HTTP_RESP_SERVICE_UNAVAILABLE: return "unavailable";
        default:                            return "error";
    }
}

static int64_t ae_json_int(struct json_object *obj, const char *key, int64_t def) {
    struct json_object *v;
    return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_int64(v) : def;
}

// min_sample_ut: when not 0, a sample older than this is not evidence for the moment asked;
// answer, when not NULL, receives the recorder's answer for the other sections that read it (put it);
// returns the section's status
static const char *ae_section_processes(BUFFER *wb, const char *key, const char *cmd, const char *sort_key,
                                        usec_t min_sample_ut, const char *source, struct json_object **answer) {
    if(answer)
        *answer = NULL;

    struct json_object *jobj = NULL, *error_body = NULL, *save = NULL;
    bool available;
    int code = ae_call(cmd, NULL, 20, source, &jobj, &available, &error_body);

    // an apps.plugin without the flight recorder answers with the live table: that is not evidence for the past
    struct json_object *recorded = NULL;
    if(jobj && !json_object_object_get_ex(jobj, "recorded", &recorded)) {
        json_object_put(jobj);
        jobj = NULL;
        available = false;
    }

    // whether the recorded time is on disk: a save that could not be written says why (disk_full, write_error)
    if(jobj && recorded && json_object_object_get_ex(recorded, "save", &save))
        json_object_get(save);
    else if(error_body && json_object_object_get_ex(error_body, "save", &save))
        json_object_get(save);

    // the recorder falls back to its newest sample before T, however old; nothing saved near T is not_saved
    if(jobj && min_sample_ut && (usec_t)ae_json_int(recorded, "sample_ut", 0) < min_sample_ut) {
        json_object_put(jobj);
        jobj = NULL;
        code = HTTP_RESP_NOT_FOUND;

        // the plugin's save covered [old sample, T]: only a failed save explains the missing time
        const char *st = ae_json_str(save, "status");
        if(save && (!st || strcmp(st, "not_saved") != 0)) {
            json_object_put(save);
            save = NULL;
        }
    }

    const char *status = ae_status_from_code(code, available, jobj != NULL);
    buffer_json_member_add_object(wb, key);
    buffer_json_member_add_string(wb, "status", status);
    buffer_json_member_add_string(wb, "function", cmd);
    if(save) {
        ae_json_value(wb, "save", save);
        const char *reason = ae_json_str(save, "reason");
        if(!jobj && reason)
            buffer_json_member_add_string(wb, "reason", reason);
    }
    if(!jobj)
        buffer_json_member_add_int64(wb, "code", code);
    else {
        ae_json_value(wb, "recorded", recorded);

        bool truncated;
        size_t rows = ae_table_rows(wb, "rows", jobj, sort_key, NULL, AE_PROCESS_ROWS_MAX, &truncated);

        // running: every process on the host (the recorder's frame totals); recorded_processes: the ones the
        // recorder kept in detail (the busiest by CPU, memory and I/O, and every start and exit), capped by apps.plugin
        struct json_object *totals = NULL, *running = NULL;
        if(json_object_object_get_ex(recorded, "totals", &totals) &&
           json_object_object_get_ex(totals, "processes", &running)) {
            if(min_sample_ut)
                buffer_json_member_add_int64(wb, "running", ae_json_int(running, "max", 0));
            else
                ae_json_value(wb, "running", running);
        }

        int64_t total = ae_json_int(jobj, "processes_total", (int64_t)rows);
        buffer_json_member_add_uint64(wb, "recorded_processes", (uint64_t)total);
        buffer_json_member_add_boolean(wb, "truncated", truncated || (uint64_t)total > rows);

        // the window answer marks processes that started or exited inside it; counted over the rows it returned
        struct json_object *columns, *data, *col, *idx;
        if(json_object_object_get_ex(jobj, "columns", &columns) && json_object_object_get_ex(jobj, "data", &data)) {
            const char *flags[] = { "Started", "Exited" };
            for(size_t f = 0; f < _countof(flags); f++) {
                if(!json_object_object_get_ex(columns, flags[f], &col) || !json_object_object_get_ex(col, "index", &idx))
                    continue;

                size_t count = 0, at = (size_t)json_object_get_int64(idx);
                for(size_t r = 0; r < json_object_array_length(data); r++) {
                    struct json_object *v = json_object_array_get_idx(json_object_array_get_idx(data, r), at);
                    if(v && json_object_is_type(v, json_type_string) && !strcmp(json_object_get_string(v), "yes"))
                        count++;
                }
                buffer_json_member_add_uint64(wb, f == 0 ? "started" : "exited", count);
            }
            if(json_object_object_get_ex(columns, "Started", &col))
                buffer_json_member_add_boolean(wb, "counts_partial", (uint64_t)total > json_object_array_length(data));
        }

        if(answer)
            *answer = json_object_get(jobj);
    }
    buffer_json_object_close(wb);

    if(jobj)
        json_object_put(jobj);
    if(error_body)
        json_object_put(error_body);
    if(save)
        json_object_put(save);

    return status;
}

// an object member of the window answer, NULL when missing
static struct json_object *ae_answer_object(struct json_object *answer, const char *key) {
    struct json_object *o;
    return (answer && json_object_object_get_ex(answer, key, &o) && json_object_is_type(o, json_type_object)) ? o : NULL;
}

// the regular files the busiest processes by I/O held open for writing in the window, from the window answer;
// window_status is the status of that answer
static void ae_section_files(BUFFER *wb, struct json_object *answer, const char *window_status) {
    struct json_object *files = ae_answer_object(answer, "files"), *recorded = NULL, *list = NULL;
    bool looked = files && json_object_object_get_ex(files, "recorded", &recorded) && json_object_get_boolean(recorded);

    buffer_json_member_add_object(wb, "files");
    buffer_json_member_add_string(wb, "source", "processes_window");
    if(strcmp(window_status, "ok") != 0)
        buffer_json_member_add_string(wb, "status", window_status);
    else if(!looked) {
        // an older apps.plugin, file charts off (with-files), or without-recorder-files
        buffer_json_member_add_string(wb, "status", "unavailable");
        buffer_json_member_add_string(wb, "message",
            "apps.plugin did not record open files: it needs file charts (with-files) and the recorder's file option");
    }
    else {
        buffer_json_member_add_string(wb, "status", "ok");
        size_t n = 0;
        buffer_json_member_add_array(wb, "list");
        if(json_object_object_get_ex(files, "list", &list) && json_object_is_type(list, json_type_array)) {
            n = json_object_array_length(list);
            for(size_t i = 0; i < n && i < AE_FILE_ROWS_MAX; i++)
                ae_json_value(wb, NULL, json_object_array_get_idx(list, i));
        }
        buffer_json_array_close(wb);

        int64_t total = ae_json_int(files, "total", (int64_t)n);
        buffer_json_member_add_int64(wb, "total", total);
        buffer_json_member_add_boolean(wb, "truncated", (uint64_t)total > MIN(n, (size_t)AE_FILE_ROWS_MAX));
    }
    buffer_json_object_close(wb);
}

// the TCP connections processes opened and closed in the window, from the window answer
static void ae_section_connection_events(BUFFER *wb, struct json_object *answer, const char *window_status) {
    struct json_object *events = ae_answer_object(answer, "connection_events"), *recorded = NULL, *a;
    bool looked = events && json_object_object_get_ex(events, "recorded", &recorded) && json_object_get_boolean(recorded);

    buffer_json_member_add_object(wb, "connection_events");
    buffer_json_member_add_string(wb, "source", "processes_window");
    if(strcmp(window_status, "ok") != 0)
        buffer_json_member_add_string(wb, "status", window_status);
    else if(!looked) {
        // an older apps.plugin, file charts off (with-files), without-recorder-connections, or a kernel without
        // TCP close notifications
        buffer_json_member_add_string(wb, "status", "unavailable");
        buffer_json_member_add_string(wb, "message",
            "apps.plugin did not record TCP connections: it needs file charts (with-files), the recorder's "
            "connections option and a kernel that reports TCP closes");
    }
    else {
        buffer_json_member_add_string(wb, "status", "ok");
        buffer_json_member_add_int64(wb, "total", ae_json_int(events, "total", 0));
        buffer_json_member_add_int64(wb, "lost", ae_json_int(events, "lost", 0));

        // the sockets of processes in other network namespaces are not in apps.plugin's dumps
        int64_t netns_other = ae_json_int(events, "netns_other", 0);
        buffer_json_member_add_int64(wb, "netns_other", netns_other);
        if(netns_other)
            buffer_json_member_add_string(wb, "coverage",
                "processes in other network namespaces (containers) are not covered");

        size_t n = 0;
        buffer_json_member_add_array(wb, "by_process");
        if(json_object_object_get_ex(events, "by_process", &a) && json_object_is_type(a, json_type_array)) {
            n = json_object_array_length(a);
            for(size_t i = 0; i < n && i < AE_CONN_PROCESSES_MAX; i++)
                ae_json_value(wb, NULL, json_object_array_get_idx(a, i));
        }
        buffer_json_array_close(wb);

        // the list is oldest first: keep its newest entries
        size_t listed = 0;
        buffer_json_member_add_array(wb, "list");
        if(json_object_object_get_ex(events, "list", &a) && json_object_is_type(a, json_type_array)) {
            size_t total = json_object_array_length(a);
            listed = MIN(total, (size_t)AE_CONN_EVENTS_MAX);
            for(size_t i = total - listed; i < total; i++)
                ae_json_value(wb, NULL, json_object_array_get_idx(a, i));
        }
        buffer_json_array_close(wb);
        buffer_json_member_add_boolean(wb, "truncated",
            (uint64_t)ae_json_int(events, "total", 0) > listed || n > AE_CONN_PROCESSES_MAX);
    }
    buffer_json_object_close(wb);
}

static void ae_section_connections(BUFFER *wb, time_t when) {
    CLEAN_BUFFER *snapshot = buffer_create(0, NULL);
    time_t captured = 0;
    const char *unsaved_reason;
    ALERT_EVIDENCE_CONNECTIONS_STATUS status = alert_evidence_connections_load(when, snapshot, &captured, &unsaved_reason);
    struct json_object *jobj = (status == ALERT_EVIDENCE_CONNECTIONS_OK) ? json_tokener_parse(buffer_tostring(snapshot)) : NULL;

    buffer_json_member_add_object(wb, "connections");
    buffer_json_member_add_string(wb, "status", (status == ALERT_EVIDENCE_CONNECTIONS_OK && !jobj) ? "error" :
                                                    alert_evidence_connections_status_str(status));
    if(unsaved_reason) {
        // same shape as the processes section: captured but not on disk, and whether RAM still holds it
        buffer_json_member_add_object(wb, "save");
        buffer_json_member_add_string(wb, "status", "not_saved");
        buffer_json_member_add_string(wb, "reason", unsaved_reason);
        buffer_json_member_add_boolean(wb, "in_ram", status == ALERT_EVIDENCE_CONNECTIONS_OK);
        buffer_json_object_close(wb);
        if(status == ALERT_EVIDENCE_CONNECTIONS_NOT_SAVED) {
            buffer_json_member_add_time_t(wb, "captured", captured);
            buffer_json_member_add_string(wb, "reason", unsaved_reason);
        }
    }
    if(jobj) {
        buffer_json_member_add_time_t(wb, "captured", captured);
        bool truncated;
        size_t total = ae_table_rows(wb, "rows", jobj, NULL, NULL, AE_CONNECTION_ROWS_MAX, &truncated);
        buffer_json_member_add_uint64(wb, "total", total);
        buffer_json_member_add_boolean(wb, "truncated", truncated);
        json_object_put(jobj);
    }
    buffer_json_object_close(wb);
}

// The changes found at startup are logged once the agent is running, after the alerts of its first evaluations.
// `startup_before`, when not 0, extends the window past T to catch them.
static void ae_section_host_changes(BUFFER *wb, time_t after, time_t before, time_t startup_before, const char *source) {
    // without the journal the events are in log files, where the systemd-journal Function cannot see them
    if(!nd_log_source_logs_to_journal(NDLS_DAEMON)) {
        buffer_json_member_add_object(wb, "host_changes");
        buffer_json_member_add_string(wb, "status", "unavailable");
        buffer_json_member_add_string(wb, "message",
            "this agent does not log to the systemd journal; its host changes are in its daemon log");
        buffer_json_object_close(wb);
        return;
    }

    if(startup_before > before)
        before = startup_before;

    CLEAN_BUFFER *payload = buffer_create(0, NULL);
    buffer_json_initialize(payload, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_MINIFY);
    buffer_json_member_add_time_t(payload, "after", after);
    buffer_json_member_add_time_t(payload, "before", before);
    buffer_json_member_add_uint64(payload, "last", AE_HOST_CHANGES_MAX);
    buffer_json_member_add_string(payload, "direction", "backward");
    buffer_json_member_add_object(payload, "selections");
    {
        buffer_json_member_add_array(payload, "__logs_sources");
        buffer_json_add_array_item_string(payload, "all");
        buffer_json_array_close(payload);
        buffer_json_member_add_array(payload, "MESSAGE_ID");
        buffer_json_add_array_item_string(payload, AE_HOST_CHANGE_MSGID);
        buffer_json_array_close(payload);
    }
    buffer_json_object_close(payload);
    buffer_json_finalize(payload);

    struct json_object *jobj = NULL;
    bool available;
    int code = ae_call("systemd-journal", payload, 30, source, &jobj, &available, NULL);

    // no saving is involved here: a journal that answers with an error is an error
    const char *status = ae_status_from_code(code, available, jobj != NULL);
    buffer_json_member_add_object(wb, "host_changes");
    buffer_json_member_add_string(wb, "status", !strcmp(status, "not_saved") ? "error" : status);
    buffer_json_member_add_time_t(wb, "after", after);
    buffer_json_member_add_time_t(wb, "before", before);
    buffer_json_member_add_boolean(wb, "includes_startup", startup_before != 0);
    if(jobj) {
        const char *keys[] = { "timestamp", "ND_HOST_CHANGE", "ND_HOST_CHANGE_OLD", "ND_HOST_CHANGE_NEW", NULL };
        bool truncated;
        ae_table_rows(wb, "events", jobj, NULL, keys, AE_HOST_CHANGES_MAX, &truncated);
        json_object_put(jobj);
    }
    else
        buffer_json_member_add_int64(wb, "code", code);
    buffer_json_object_close(wb);
}

// --------------------------------------------------------------------------------------------------------------------
// the dashboard view: a picker of this agent's recent alerts; `all` lists them, one alert shows the processes around it

struct ae_list_row {
    nd_uuid_t id;
    time_t when;
    char *alert;
    char *chart;
    char *units;
    int new_status;
    int old_status;
    NETDATA_DOUBLE value;
};

struct ae_list {
    struct ae_list_row *rows;
    size_t used;
};

static void ae_list_cb(struct sql_alert_transition_data *t, void *data) {
    struct ae_list *list = data;
    if(list->used >= AE_LIST_ROWS_MAX ||
       (t->new_status != RRDCALC_STATUS_WARNING && t->new_status != RRDCALC_STATUS_CRITICAL))
        return;

    struct ae_list_row *r = &list->rows[list->used++];
    uuid_copy(r->id, *t->transition_id);
    r->when = t->when_key;
    r->alert = strdupz(t->alert_name ? t->alert_name : "");
    r->chart = strdupz(t->chart ? t->chart : "");
    r->units = strdupz(t->units ? t->units : "");
    r->new_status = t->new_status;
    r->old_status = t->old_status;
    r->value = t->new_value;
}

static void ae_list_load(struct ae_list *list) {
    list->rows = callocz(AE_LIST_ROWS_MAX, sizeof(*list->rows));
    list->used = 0;

    const time_t now = now_realtime_sec();
    DICTIONARY *nodes = dictionary_create(DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE);
    dictionary_set(nodes, localhost->machine_guid, NULL, 0);
    sql_alert_transitions(nodes, now - AE_LIST_WINDOW_S, now, NULL, NULL, NULL, ae_list_cb, list, false);
    dictionary_destroy(nodes);
}

static void ae_list_free(struct ae_list *list) {
    for(size_t i = 0; i < list->used; i++) {
        freez(list->rows[i].alert);
        freez(list->rows[i].chart);
        freez(list->rows[i].units);
    }
    freez(list->rows);
}

static void ae_table_preamble(BUFFER *wb, const char *help) {
    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);
    buffer_json_member_add_string(wb, "hostname", rrdhost_hostname(localhost));
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_string(wb, "type", "table");
    buffer_json_member_add_time_t(wb, "update_every", 10);
    buffer_json_member_add_boolean(wb, "has_history", false);
    buffer_json_member_add_string(wb, "help", help);
}

// the alert picker; the dashboard sends the choice back as `alert:<id>`
static void ae_picker_params(BUFFER *wb, struct ae_list *list) {
    buffer_json_member_add_array(wb, "accepted_params");
    buffer_json_add_array_item_string(wb, "alert");
    buffer_json_array_close(wb);

    buffer_json_member_add_array(wb, "required_params");
    buffer_json_add_array_item_object(wb);
    {
        buffer_json_member_add_string(wb, "id", "alert");
        buffer_json_member_add_string(wb, "name", "Alert");
        buffer_json_member_add_string(wb, "help", "Pick an alert to see the processes around it");
        buffer_json_member_add_boolean(wb, "unique_view", true);
        buffer_json_member_add_string(wb, "type", "select");
        buffer_json_member_add_array(wb, "options");
        {
            buffer_json_add_array_item_object(wb);
            buffer_json_member_add_string(wb, "id", "all");
            buffer_json_member_add_string(wb, "name", "Recent alerts");
            buffer_json_object_close(wb);

            for(size_t i = 0; i < list->used && i < AE_PICKER_OPTIONS_MAX; i++) {
                struct ae_list_row *r = &list->rows[i];
                char id[UUID_STR_LEN], when[32], name[512];
                uuid_unparse_lower(r->id, id);
                struct tm tm;
                strftime(when, sizeof(when), "%m-%d %H:%M:%S", localtime_r(&r->when, &tm));
                snprintfz(name, sizeof(name), "%s %s %s on %s (%.2f %s)", when, rrdcalc_status2string(r->new_status),
                          r->alert, r->chart, (double)r->value, r->units);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "id", id);
                buffer_json_member_add_string(wb, "name", name);
                buffer_json_object_close(wb);
            }
        }
        buffer_json_array_close(wb);
    }
    buffer_json_object_close(wb);
    buffer_json_array_close(wb);
}

static void ae_list_table(BUFFER *wb, struct ae_list *list) {
    buffer_json_member_add_array(wb, "data");
    for(size_t i = 0; i < list->used; i++) {
        struct ae_list_row *r = &list->rows[i];
        buffer_json_add_array_item_array(wb);
        buffer_json_add_array_item_uuid(wb, &r->id);
        buffer_json_add_array_item_uint64(wb, (uint64_t)r->when * USEC_PER_SEC);
        buffer_json_add_array_item_string(wb, r->alert);
        buffer_json_add_array_item_string(wb, r->chart);
        buffer_json_add_array_item_string(wb, rrdcalc_status2string(r->new_status));
        buffer_json_add_array_item_string(wb, rrdcalc_status2string(r->old_status));
        buffer_json_add_array_item_double(wb, r->value);
        buffer_json_add_array_item_string(wb, r->units);
        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_string(wb, "severity", r->new_status == RRDCALC_STATUS_CRITICAL ? "critical" : "warning");
        buffer_json_object_close(wb);
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "columns");
    {
        size_t field_id = 0;

        buffer_rrdf_table_add_field(wb, field_id++, "Transition", "Transition ID: ask for its evidence with alert-evidence transition:<id>",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_UNIQUE_KEY, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "When", "Time of the transition",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_USEC,
                                    0, NULL, NAN, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Alert", "Alert name",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_STICKY, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Instance", "Chart the alert runs on",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Status", "Status the alert moved to",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "From", "Status the alert moved from",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Value", "Alert value at the transition",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    2, NULL, NAN, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Units", "Units of the value",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "rowOptions", "rowOptions",
                                    RRDF_FIELD_TYPE_NONE, RRDR_FIELD_VISUAL_ROW_OPTIONS, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_FIXED, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_DUMMY, NULL);
    }
    buffer_json_object_close(wb); // columns
    buffer_json_member_add_string(wb, "default_sort_column", "When");
}

// a table answer of another Function, copied without one column (the indexes renumbered)
static void ae_table_passthrough(BUFFER *wb, struct json_object *answer, const char *drop) {
    struct json_object *columns = NULL, *data = NULL;
    if(!json_object_object_get_ex(answer, "columns", &columns) || !json_object_is_type(columns, json_type_object))
        columns = NULL;
    if(!json_object_object_get_ex(answer, "data", &data) || !json_object_is_type(data, json_type_array))
        data = NULL;

    size_t ncols = 0;
    if(columns) {
        json_object_object_foreach(columns, cname, cdef) {
            (void)cname; (void)cdef;
            ncols++;
        }
    }
    int64_t *from = callocz(ncols ? ncols : 1, sizeof(*from));

    size_t used = 0;
    buffer_json_member_add_object(wb, "columns");
    if(columns) {
        json_object_object_foreach(columns, name, def) {
            struct json_object *idx;
            if(!json_object_is_type(def, json_type_object) || !json_object_object_get_ex(def, "index", &idx) ||
               (drop && !strcmp(name, drop)))
                continue;

            from[used] = json_object_get_int64(idx);
            buffer_json_member_add_object(wb, name);
            json_object_object_foreach(def, k, v) {
                if(!strcmp(k, "index"))
                    buffer_json_member_add_uint64(wb, "index", used);
                else
                    ae_json_value(wb, k, v);
            }
            buffer_json_object_close(wb);
            used++;
        }
    }
    buffer_json_object_close(wb);

    buffer_json_member_add_array(wb, "data");
    for(size_t r = 0; data && r < json_object_array_length(data); r++) {
        struct json_object *row = json_object_array_get_idx(data, r);
        if(!json_object_is_type(row, json_type_array))
            continue;

        buffer_json_add_array_item_array(wb);
        for(size_t c = 0; c < used; c++)
            ae_json_value(wb, NULL, json_object_array_get_idx(row, from[c]));
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    freez(from);
}

// a dashboard answer that cannot show processes: one row saying why, with the picker kept so another alert can be
// picked (the dashboard shows only "rejected" for an error status)
static int ae_message_table(BUFFER *wb, struct ae_list *list, const char *message) {
    ae_table_preamble(wb, FUNCTION_ALERT_EVIDENCE_HELP);
    ae_picker_params(wb, list);

    buffer_json_member_add_array(wb, "data");
    buffer_json_add_array_item_array(wb);
    buffer_json_add_array_item_string(wb, message);
    buffer_json_array_close(wb);
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "columns");
    buffer_rrdf_table_add_field(wb, 0, "Message", "Why there are no processes to show",
                                RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_UNIQUE_KEY | RRDF_FIELD_OPTS_FULL_WIDTH | RRDF_FIELD_OPTS_WRAP,
                                NULL);
    buffer_json_object_close(wb);

    buffer_json_finalize(wb);
    return HTTP_RESP_OK;
}

static int ae_processes_view(BUFFER *wb, struct ae_list *list, const struct ae_transition *tr, const char *source) {
    const time_t T = tr->when;
    char cmd[128];
    snprintfz(cmd, sizeof(cmd), "processes after:%lld before:%lld",
              (long long)(T - ALERT_EVIDENCE_PROCESSES_BEFORE_S), (long long)(T + ALERT_EVIDENCE_PROCESSES_AFTER_S));

    struct json_object *jobj = NULL, *recorded = NULL, *error_body = NULL, *save = NULL;
    bool available;
    int code = ae_call(cmd, NULL, 20, source, &jobj, &available, &error_body);
    const char *reason = (error_body && json_object_object_get_ex(error_body, "save", &save)) ? ae_json_str(save, "reason") : NULL;

    // an answer without `recorded` is the live table, not the past
    if(jobj && !json_object_object_get_ex(jobj, "recorded", &recorded)) {
        json_object_put(jobj);
        jobj = NULL;
        available = false;
    }
    if(!jobj) {
        char message[256];
        if(!available || code == HTTP_RESP_SERVICE_UNAVAILABLE)
            snprintfz(message, sizeof(message), "This agent has no process flight recorder (apps.plugin on Linux).");
        else if(code == HTTP_RESP_NOT_FOUND && reason && !strcmp(reason, "disk_full"))
            snprintfz(message, sizeof(message),
                      "The process history around this alert was recorded but could not be saved: the disk was full.");
        else if(code == HTTP_RESP_NOT_FOUND && reason)
            snprintfz(message, sizeof(message),
                      "The process history around this alert was recorded but could not be saved (%s).", reason);
        else if(code == HTTP_RESP_NOT_FOUND)
            snprintfz(message, sizeof(message),
                      "No process history was saved around this alert: the agent did not record processes then, "
                      "or the saved history was deleted to stay within its disk limit.");
        else
            snprintfz(message, sizeof(message), "The processes Function answered with an error (%d).", code);
        if(error_body)
            json_object_put(error_body);
        return ae_message_table(wb, list, message);
    }

    char when[32], help[512];
    struct tm tm;
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime_r(&T, &tm));
    snprintfz(help, sizeof(help),
              "Processes from 10 minutes before to 2 minutes after %s went %s on %s at %s: average and peak CPU, "
              "peak memory, and whether each started or exited in that time.",
              tr->alert, rrdcalc_status2string(tr->new_status), tr->chart, when);

    ae_table_preamble(wb, help);
    ae_picker_params(wb, list);
    // decision 3: command lines need VIEW_AGENT_CONFIG; apps.plugin options can lift that for `processes`, not here
    ae_table_passthrough(wb, jobj, "CmdLine");
    buffer_json_member_add_string(wb, "default_sort_column", "AvgCPU");
    buffer_json_finalize(wb);

    json_object_put(jobj);
    return HTTP_RESP_OK;
}

static void ae_transition_lookup(const char *transition, struct ae_transition *tr) {
    DICTIONARY *nodes = dictionary_create(DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE);
    sql_alert_transitions(nodes, 0, 0, NULL, NULL, transition, ae_transition_cb, tr, false);
    dictionary_destroy(nodes);
}

// no transition: the alert picker; `info` (the dashboard asking for the parameters), `alert:all` or nothing lists the
// recent alerts, `alert:<id>` shows the processes around that one
static int ae_dashboard_view(BUFFER *wb, const char *alert, bool info, const char *source) {
    struct ae_list list;
    ae_list_load(&list);

    int code = HTTP_RESP_OK;
    nd_uuid_t alert_uuid;
    if(!info && alert && strcmp(alert, "all") != 0) {
        struct ae_transition tr = { 0 };
        if(uuid_parse(alert, alert_uuid) == 0)
            ae_transition_lookup(alert, &tr);

        if(!tr.found || uuid_compare(tr.host_id, localhost->host_id.uuid) != 0)
            code = ae_message_table(wb, &list, "This agent has no alert of its own with this id.");
        else
            code = ae_processes_view(wb, &list, &tr, source);

        ae_transition_free(&tr);
    }
    else {
        ae_table_preamble(wb, FUNCTION_ALERT_EVIDENCE_HELP);
        ae_picker_params(wb, &list);
        if(!info)
            ae_list_table(wb, &list);
        buffer_json_finalize(wb);
    }

    ae_list_free(&list);
    return code;
}

// --------------------------------------------------------------------------------------------------------------------

int function_alert_evidence(BUFFER *wb, const char *function, BUFFER *payload __maybe_unused, const char *source) {
    char *function_copy = strdupz(function ? function : "");
    char *words[64];
    size_t num_words = quoted_strings_splitter_whitespace(function_copy, words, _countof(words));
    const char *transition = NULL, *alert = NULL;
    bool info = false;
    for(size_t i = 1; i < num_words; i++) {
        const char *w = get_word(words, num_words, i);
        if(!w)
            continue;
        if(!strncmp(w, "transition:", sizeof("transition:") - 1))
            transition = w + sizeof("transition:") - 1;
        else if(!strncmp(w, "alert:", sizeof("alert:") - 1))
            alert = w + sizeof("alert:") - 1;
        else if(!strcmp(w, "info"))
            info = true;
    }

    char call_source[256];
    snprintfz(call_source, sizeof(call_source), "%s,alert-evidence", source ? source : "");

    if(!transition) {
        int code = ae_dashboard_view(wb, alert, info, call_source);
        freez(function_copy);
        return code;
    }

    nd_uuid_t transition_uuid;
    if(!*transition || uuid_parse(transition, transition_uuid) != 0) {
        freez(function_copy);
        return ae_error(wb, HTTP_RESP_BAD_REQUEST, "give the transition as transition:<transition id>");
    }

    struct ae_transition tr = { 0 };
    ae_transition_lookup(transition, &tr);

    if(!tr.found) {
        freez(function_copy);
        return ae_error(wb, HTTP_RESP_NOT_FOUND, "this agent has no alert transition with this id");
    }

    const time_t T = tr.when;
    char host_guid[UUID_STR_LEN];
    uuid_unparse_lower(tr.host_id, host_guid);
    bool local = uuid_compare(tr.host_id, localhost->host_id.uuid) == 0;

    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_string(wb, "type", "alert-evidence");
    buffer_json_member_add_string(wb, "help", FUNCTION_ALERT_EVIDENCE_HELP);

    buffer_json_member_add_object(wb, "transition");
    {
        RRDHOST *host = local ? localhost : rrdhost_find_by_guid(host_guid);
        buffer_json_member_add_string(wb, "id", transition);
        buffer_json_member_add_string(wb, "node", host ? rrdhost_hostname(host) : host_guid);
        buffer_json_member_add_string(wb, "machine_guid", host_guid);
        buffer_json_member_add_string(wb, "alert", tr.alert);
        buffer_json_member_add_string(wb, "chart", tr.chart);
        buffer_json_member_add_string(wb, "context", tr.context);
        buffer_json_member_add_string(wb, "classification", tr.classification);
        buffer_json_member_add_string(wb, "component", tr.component);
        buffer_json_member_add_string(wb, "type", tr.type);
        buffer_json_member_add_string(wb, "summary", tr.summary);
        buffer_json_member_add_time_t(wb, "when", T);
        buffer_json_member_add_string(wb, "old_status", rrdcalc_status2string(tr.old_status));
        buffer_json_member_add_string(wb, "new_status", rrdcalc_status2string(tr.new_status));
        buffer_json_member_add_double(wb, "old_value", tr.old_value);
        buffer_json_member_add_double(wb, "new_value", tr.new_value);
        buffer_json_member_add_string(wb, "units", tr.units);
    }
    buffer_json_object_close(wb);

    buffer_json_member_add_object(wb, "evidence");
    if(!local) {
        // decision 15: no relay; the node of the alert keeps its own evidence
        buffer_json_member_add_object(wb, "all");
        buffer_json_member_add_string(wb, "status", "other_node");
        buffer_json_member_add_string(wb, "message",
            "this transition was evaluated for another node; ask that node's agent for the evidence of its own alerts");
        buffer_json_object_close(wb);
    }
    else {
        const time_t p_after = T - ALERT_EVIDENCE_PROCESSES_BEFORE_S, p_before = T + ALERT_EVIDENCE_PROCESSES_AFTER_S;

        char cmd[128];
        snprintfz(cmd, sizeof(cmd), "processes at:%lld", (long long)T);
        ae_section_processes(wb, "processes_at", cmd, "CPU", (usec_t)(T - AE_AT_MAX_AGE_S) * USEC_PER_SEC, call_source, NULL);

        snprintfz(cmd, sizeof(cmd), "processes after:%lld before:%lld", (long long)p_after, (long long)p_before);
        struct json_object *window;
        const char *window_status = ae_section_processes(wb, "processes_window", cmd, "PeakCPU", 0, call_source, &window);
        ae_section_files(wb, window, window_status);
        ae_section_connection_events(wb, window, window_status);
        if(window)
            json_object_put(window);

        ae_section_connections(wb, T);

        // an alert leaving UNINITIALIZED is a first evaluation, usually right after the agent started
        const time_t startup_before =
            (tr.old_status == RRDCALC_STATUS_UNINITIALIZED) ? T + ALERT_EVIDENCE_HOST_CHANGES_STARTUP_S : 0;
        ae_section_host_changes(wb, T - ALERT_EVIDENCE_HOST_CHANGES_BEFORE_S, T, startup_before, call_source);

        buffer_json_member_add_object(wb, "journal");
        {
            buffer_json_member_add_string(wb, "status", "pointer");
            buffer_json_member_add_string(wb, "function", "systemd-journal");
            buffer_json_member_add_time_t(wb, "after", p_after);
            buffer_json_member_add_time_t(wb, "before", p_before);
        }
        buffer_json_object_close(wb);

        buffer_json_member_add_object(wb, "correlated_metrics");
        {
            const time_t w_before = T, w_after = T - ALERT_EVIDENCE_WEIGHTS_WINDOW_S;
            buffer_json_member_add_string(wb, "status", "pointer");
            buffer_json_member_add_string(wb, "api", "/api/v2/weights");
            buffer_json_member_add_string(wb, "mcp_tool", "find_correlated_metrics");
            buffer_json_member_add_string(wb, "method", "ks2");
            buffer_json_member_add_time_t(wb, "after", w_after);
            buffer_json_member_add_time_t(wb, "before", w_before);
            buffer_json_member_add_time_t(wb, "baseline_after", w_after - ALERT_EVIDENCE_WEIGHTS_BASELINE_S);
            buffer_json_member_add_time_t(wb, "baseline_before", w_after);
        }
        buffer_json_object_close(wb);
    }
    buffer_json_object_close(wb); // evidence

    buffer_json_finalize(wb);

    ae_transition_free(&tr);
    freez(function_copy);
    return HTTP_RESP_OK;
}
