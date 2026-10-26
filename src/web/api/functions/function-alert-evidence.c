// SPDX-License-Identifier: GPL-3.0-or-later

#include "function-alert-evidence.h"
#include "daemon/alert-evidence.h"

// The answer is a JSON document, not a table: one section per kind of evidence, each with a status. The processes
// come from the apps.plugin flight recorder, the connections from the snapshots taken when the save was triggered,
// the host changes from the systemd-journal Function; the journal range and the correlated metrics are pointers.

#define AE_HOST_CHANGE_MSGID        "adfd0357c3d6490fb5c7cd9337492f11"
#define AE_PROCESS_ROWS_MAX         50
#define AE_CONNECTION_ROWS_MAX      1000
#define AE_HOST_CHANGES_MAX         100
#define AE_AT_MAX_AGE_S             60      // processes_at answers with the newest sample before T: older is not evidence

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
static int ae_call(const char *cmd, BUFFER *payload, int timeout_s, const char *source, struct json_object **jobj, bool *available) {
    *jobj = NULL;
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

    return code;
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

// min_sample_ut: when not 0, a sample older than this is not evidence for the moment asked
static void ae_section_processes(BUFFER *wb, const char *key, const char *cmd, const char *sort_key, usec_t min_sample_ut, const char *source) {
    struct json_object *jobj = NULL;
    bool available;
    int code = ae_call(cmd, NULL, 20, source, &jobj, &available);

    // an apps.plugin without the flight recorder answers with the live table: that is not evidence for the past
    struct json_object *recorded = NULL;
    if(jobj && !json_object_object_get_ex(jobj, "recorded", &recorded)) {
        json_object_put(jobj);
        jobj = NULL;
        available = false;
    }

    // the recorder falls back to its newest sample before T, however old; nothing saved near T is not_saved
    if(jobj && min_sample_ut && (usec_t)ae_json_int(recorded, "sample_ut", 0) < min_sample_ut) {
        json_object_put(jobj);
        jobj = NULL;
        code = HTTP_RESP_NOT_FOUND;
    }

    buffer_json_member_add_object(wb, key);
    buffer_json_member_add_string(wb, "status", ae_status_from_code(code, available, jobj != NULL));
    buffer_json_member_add_string(wb, "function", cmd);
    if(!jobj)
        buffer_json_member_add_int64(wb, "code", code);
    else {
        ae_json_value(wb, "recorded", recorded);

        bool truncated;
        size_t rows = ae_table_rows(wb, "rows", jobj, sort_key, NULL, AE_PROCESS_ROWS_MAX, &truncated);

        // the range answer is capped by apps.plugin; it says how many processes there were
        int64_t total = ae_json_int(jobj, "processes_total", (int64_t)rows);
        buffer_json_member_add_uint64(wb, "total", (uint64_t)total);
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
    }
    buffer_json_object_close(wb);

    if(jobj)
        json_object_put(jobj);
}

static void ae_section_connections(BUFFER *wb, time_t when) {
    CLEAN_BUFFER *snapshot = buffer_create(0, NULL);
    time_t captured = 0;
    ALERT_EVIDENCE_CONNECTIONS_STATUS status = alert_evidence_connections_load(when, snapshot, &captured);
    struct json_object *jobj = (status == ALERT_EVIDENCE_CONNECTIONS_OK) ? json_tokener_parse(buffer_tostring(snapshot)) : NULL;

    buffer_json_member_add_object(wb, "connections");
    buffer_json_member_add_string(wb, "status", (status == ALERT_EVIDENCE_CONNECTIONS_OK && !jobj) ? "error" :
                                                    alert_evidence_connections_status_str(status));
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

static void ae_section_host_changes(BUFFER *wb, time_t after, time_t before, const char *source) {
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
    int code = ae_call("systemd-journal", payload, 30, source, &jobj, &available);

    // no saving is involved here: a journal that answers with an error is an error
    const char *status = ae_status_from_code(code, available, jobj != NULL);
    buffer_json_member_add_object(wb, "host_changes");
    buffer_json_member_add_string(wb, "status", !strcmp(status, "not_saved") ? "error" : status);
    buffer_json_member_add_time_t(wb, "after", after);
    buffer_json_member_add_time_t(wb, "before", before);
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

int function_alert_evidence(BUFFER *wb, const char *function, BUFFER *payload __maybe_unused, const char *source) {
    char *function_copy = strdupz(function ? function : "");
    char *words[64];
    size_t num_words = quoted_strings_splitter_whitespace(function_copy, words, _countof(words));
    const char *transition = NULL;
    for(size_t i = 1; i < num_words; i++) {
        const char *w = get_word(words, num_words, i);
        if(w && !strncmp(w, "transition:", sizeof("transition:") - 1))
            transition = w + sizeof("transition:") - 1;
    }

    nd_uuid_t transition_uuid;
    if(!transition || !*transition || uuid_parse(transition, transition_uuid) != 0) {
        freez(function_copy);
        return ae_error(wb, HTTP_RESP_BAD_REQUEST, "give the transition as transition:<transition id>");
    }

    struct ae_transition tr = { 0 };
    DICTIONARY *nodes = dictionary_create(DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE);
    sql_alert_transitions(nodes, 0, 0, NULL, NULL, transition, ae_transition_cb, &tr, false);
    dictionary_destroy(nodes);

    if(!tr.found) {
        freez(function_copy);
        return ae_error(wb, HTTP_RESP_NOT_FOUND, "this agent has no alert transition with this id");
    }

    char call_source[256];
    snprintfz(call_source, sizeof(call_source), "%s,alert-evidence", source ? source : "");

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
        ae_section_processes(wb, "processes_at", cmd, "CPU", (usec_t)(T - AE_AT_MAX_AGE_S) * USEC_PER_SEC, call_source);

        snprintfz(cmd, sizeof(cmd), "processes after:%lld before:%lld", (long long)p_after, (long long)p_before);
        ae_section_processes(wb, "processes_window", cmd, "PeakCPU", 0, call_source);

        ae_section_connections(wb, T);

        ae_section_host_changes(wb, T - ALERT_EVIDENCE_HOST_CHANGES_BEFORE_S, T, call_source);

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
