// SPDX-License-Identifier: GPL-3.0-or-later

#include "function-data-query.h"

#include "web/server/web_client.h"
#include "web/server/web_client_cache.h"

// ----------------------------------------------------------------------------
// Why this function needs its own worker threads
//
// rrd_function_run() invokes the registered execute_cb INLINE on the calling
// thread. Both rrd_call_function_async_and_wait() and
// rrd_call_function_async_and_dont_wait() do this; the `sync` flag only selects
// which result callbacks are wired, it does not move execution anywhere.
//
// When a parent calls this function on a child, the calling thread is the
// child's STREAM THREAD:
//
//   stream_thread_process_poll_events()      streaming/stream-thread.c
//   -> stream_sender_process_poll_events()   streaming/stream-sender.c
//   -> stream_sender_receive_data()
//   -> stream_sender_execute_commands()      streaming/stream-sender-execute.c
//   -> execute_commands_function()
//   -> rrd_function_run()  --> execute_cb == function_data_query()
//
// A stream thread services many senders and receivers from a single poll set,
// so running a data query there would stall metric streaming for every
// connection on that thread. External-plugin functions do not have this problem
// because their execute_cb only writes a request to the plugin's pipe and
// returns; the work happens in another process.
//
// So function_data_query() only enqueues, and returns immediately. The worker
// runs the query and calls rfe->result.cb() when it is done, which is exactly
// the contract rrd_inflight_async_function_nowait_finished() expects.
//
// The queue is bounded: a parent must not be able to exhaust a child. When it
// is full we answer 503 straight away.
// ----------------------------------------------------------------------------

// Paths a parent is allowed to ask a child to execute.
//
// Delegation exists to answer data queries, nothing else. Keeping this an exact
// allow-list (rather than a prefix check) means the path we execute is always
// one of OUR constants, never caller-supplied bytes, so no traversal or
// endpoint-confusion is reachable through it.
static const char *const data_query_allowed_paths[] = {
    "/api/v2/data",
    "/api/v3/data",
    NULL
};

struct data_query_job {
    // request
    const char *path;                       // points into data_query_allowed_paths[], not owned
    char *query;                            // owned
    HTTP_ACCESS access;
    int ttl;
    nd_uuid_t transaction;
    char *source;                           // owned, may be NULL

    // response plumbing back into the functions layer
    BUFFER *result_wb;
    rrd_function_result_callback_t result_cb;
    void *result_cb_data;

    // Cancellation is POLLED, not pushed.
    //
    // Registering a canceller would hand the functions layer a pointer to this
    // job, which the worker frees on completion; a FUNCTION_CANCEL racing that
    // completion would then dereference freed memory (the cancel path can
    // already hold the inflight item when we finish). Polling has no such
    // window: rrd_function_cancel_inflight() sets the inflight `cancelled` flag
    // atomically BEFORE invoking any canceller, and the inflight request is
    // guaranteed alive for as long as this job is - we are the ones who end its
    // life, by calling result_cb below.
    rrd_function_is_cancelled_cb_t is_cancelled_cb;
    void *is_cancelled_data;

    struct data_query_job *prev, *next;
};

static struct {
    bool running;

    netdata_mutex_t mutex;
    netdata_cond_t cond;

    struct data_query_job *queue_head;
    size_t queue_size;

    ND_THREAD *threads[RRDFUNCTIONS_DATA_QUERY_WORKERS];
} dq_globals = {
    .running = false,
    .queue_head = NULL,
    .queue_size = 0,
};

// The hop budget of the delegated query this thread is currently serving.
// Normal (non-delegated) queries never touch it, so they see the full default.
static __thread int data_query_thread_ttl = RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL;

int function_data_query_ttl_remaining(void) {
    return data_query_thread_ttl;
}

// ----------------------------------------------------------------------------
// payload parsing

struct data_query_payload {
    const char *path;                       // matched allow-list entry
    const char *query;                      // borrowed from the json object
    int64_t ttl;
};

static const char *data_query_match_allowed_path(const char *requested) {
    if(!requested || !*requested)
        return NULL;

    for(size_t i = 0; data_query_allowed_paths[i] ;i++) {
        if(strcmp(requested, data_query_allowed_paths[i]) == 0)
            return data_query_allowed_paths[i];
    }

    return NULL;
}

// A decoded query string arriving from a peer should never contain control
// characters. We do not build request text from it (see data_query_job_execute),
// so this is not the last line of defence against request splitting - but it
// keeps malformed input out of the query parser and out of the access log,
// where an embedded newline would forge a log line.
static bool data_query_string_is_safe(const char *s) {
    if(!s) return false;

    for(const char *p = s; *p ;p++) {
        if((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F)
            return false;
    }

    return true;
}

static bool data_query_parse_payload(json_object *jobj, void *data, BUFFER *error) {
    struct data_query_payload *req = data;
    struct json_object *j;

    if(!json_object_object_get_ex(jobj, RRDFUNCTIONS_DATA_QUERY_KEY_PATH, &j) ||
        !json_object_is_type(j, json_type_string)) {
        buffer_sprintf(error, "missing or non-string '%s'", RRDFUNCTIONS_DATA_QUERY_KEY_PATH);
        return false;
    }

    req->path = data_query_match_allowed_path(json_object_get_string(j));
    if(!req->path) {
        // do not echo the requested path back - it is caller-controlled
        buffer_sprintf(error, "'%s' is not a delegatable endpoint", RRDFUNCTIONS_DATA_QUERY_KEY_PATH);
        return false;
    }

    if(!json_object_object_get_ex(jobj, RRDFUNCTIONS_DATA_QUERY_KEY_QUERY, &j) ||
        !json_object_is_type(j, json_type_string)) {
        buffer_sprintf(error, "missing or non-string '%s'", RRDFUNCTIONS_DATA_QUERY_KEY_QUERY);
        return false;
    }

    req->query = json_object_get_string(j);
    if(!data_query_string_is_safe(req->query)) {
        buffer_sprintf(error, "'%s' contains control characters", RRDFUNCTIONS_DATA_QUERY_KEY_QUERY);
        return false;
    }

    req->ttl = 0;
    if(json_object_object_get_ex(jobj, RRDFUNCTIONS_DATA_QUERY_KEY_TTL, &j) &&
        json_object_is_type(j, json_type_int))
        req->ttl = json_object_get_int64(j);

    // clamp, never trust the peer's budget
    if(req->ttl < 0)
        req->ttl = 0;
    else if(req->ttl > RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL)
        req->ttl = RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL;

    return true;
}

// ----------------------------------------------------------------------------
// execution

static bool data_query_job_is_cancelled(struct data_query_job *job) {
    return job->is_cancelled_cb && job->is_cancelled_cb(job->is_cancelled_data);
}

static bool data_query_web_client_interrupt_cb(struct web_client *w __maybe_unused, void *data) {
    return data_query_job_is_cancelled(data);
}

static void data_query_job_free(struct data_query_job *job) {
    freez(job->query);
    freez(job->source);
    freez(job);
}

// Runs on a worker thread. Always invokes the result callback exactly once.
static void data_query_job_execute(struct data_query_job *job) {
    int code;

    if(data_query_job_is_cancelled(job)) {
        code = rrd_call_function_error(job->result_wb, "Request cancelled.", HTTP_RESP_CLIENT_CLOSED_REQUEST);
        goto respond;
    }

    struct web_client *w = web_client_get_from_cache();
    if(!w) {
        code = rrd_call_function_error(job->result_wb, "No web client available.", HTTP_RESP_SERVICE_UNAVAILABLE);
        goto respond;
    }

    // Present this exactly like the request it stands in for: it originated at
    // Cloud, arrived at our parent, and is being executed here on its behalf.
    web_client_set_conn_cloud(w);

    // Narrowest ACL that can serve this: an ACLK-style transport (no client-IP
    // validation, since there is no client IP here) plus the metrics feature
    // only. The path allow-list already restricts us to the data endpoints, so
    // there is no reason to hand out any other feature bit.
    w->port_acl = (HTTP_ACL)(HTTP_ACL_ACLK | HTTP_ACL_METRICS);
    w->acl = w->port_acl;

    // Propagate the ORIGINAL caller's access, not a hardcoded level. The parent
    // serialized it onto the FUNCTION line and rrd_function_verify_access()
    // already checked it against this function's registered access.
    web_client_set_permissions(w, job->access, HTTP_USER_ROLE_MEMBER, USER_AUTH_METHOD_CLOUD);

    w->mode = HTTP_REQUEST_MODE_GET;
    uuid_copy(w->transaction, job->transaction);

    w->interrupt.callback = data_query_web_client_interrupt_cb;
    w->interrupt.callback_data = job;

    // Populate the DECODED url buffers directly instead of synthesizing an HTTP
    // request and re-parsing it.
    //
    // The query string the parent captured was already url-decoded by its own
    // web server (web_api.c hands the decoded string to the data handlers), so
    // running it through http_request_validate() here would decode it a second
    // time and corrupt any value containing '%' or '+'. Writing the decoded
    // buffers directly is both correct and free of any request-splitting
    // surface, since we never build request text.
    buffer_flush(w->url_as_received);
    buffer_strcat(w->url_as_received, job->path);

    buffer_flush(w->url_path_decoded);
    buffer_strcat(w->url_path_decoded, job->path);

    buffer_flush(w->url_query_string_decoded);
    if(job->query && *job->query) {
        buffer_putc(w->url_query_string_decoded, '?');
        buffer_strcat(w->url_query_string_decoded, job->query);

        buffer_putc(w->url_as_received, '?');
        buffer_strcat(w->url_as_received, job->query);
    }

    buffer_flush(w->response.data);

    // Publish the remaining hop budget for this thread so that, if this node is
    // itself a parent with a gap, api_v23_data_internal() can decide whether it
    // may delegate one hop further. Restored before the thread is reused.
    data_query_thread_ttl = job->ttl;

    char *path = (char *)buffer_tostring(w->url_path_decoded);
    code = web_client_api_request_with_node_selection(localhost, w, path);

    data_query_thread_ttl = RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL;

    if(data_query_job_is_cancelled(job)) {
        code = rrd_call_function_error(job->result_wb, "Request cancelled.", HTTP_RESP_CLIENT_CLOSED_REQUEST);
        goto cleanup_client;
    }

    // Size guard. This body is about to be committed to the sender circular
    // buffer, which is shared with metric traffic; overflowing it disconnects
    // this child. Refusing the response is always better than dropping the
    // streaming link.
    if(buffer_strlen(w->response.data) > RRDFUNCTIONS_DATA_QUERY_MAX_RESPONSE) {
        nd_log(NDLS_DAEMON, NDLP_NOTICE,
               "DELEGATED QUERY: refusing a %zu bytes response (limit %llu bytes) - "
               "the parent should narrow the window or request fewer points",
               buffer_strlen(w->response.data),
               (unsigned long long)RRDFUNCTIONS_DATA_QUERY_MAX_RESPONSE);

        code = rrd_call_function_error(
            job->result_wb,
            "The delegated query response is too large to return over the streaming connection.",
            HTTP_RESP_CONTENT_TOO_LONG);

        goto cleanup_client;
    }

    buffer_flush(job->result_wb);
    buffer_fast_strcat(job->result_wb, buffer_tostring(w->response.data), buffer_strlen(w->response.data));
    job->result_wb->content_type = w->response.data->content_type;
    job->result_wb->expires = w->response.data->expires;
    job->result_wb->response_code = code;

cleanup_client:
    data_query_thread_ttl = RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL;
    w->interrupt.callback = NULL;
    w->interrupt.callback_data = NULL;
    web_client_log_completed_request(w, false);
    web_client_release_to_cache(w);

respond:
    if(job->result_cb)
        job->result_cb(job->result_wb, code, job->result_cb_data);
}

static void data_query_worker_thread(void *ptr __maybe_unused) {
    while(true) {
        netdata_mutex_lock(&dq_globals.mutex);

        while(dq_globals.running && !dq_globals.queue_head)
            netdata_cond_wait(&dq_globals.cond, &dq_globals.mutex);

        if(!dq_globals.running && !dq_globals.queue_head) {
            netdata_mutex_unlock(&dq_globals.mutex);
            break;
        }

        struct data_query_job *job = dq_globals.queue_head;
        DOUBLE_LINKED_LIST_REMOVE_ITEM_UNSAFE(dq_globals.queue_head, job, prev, next);
        dq_globals.queue_size--;

        netdata_mutex_unlock(&dq_globals.mutex);

        data_query_job_execute(job);
        data_query_job_free(job);
    }
}

// ----------------------------------------------------------------------------
// the function entry point

int function_data_query(struct rrd_function_execute *rfe, void *data __maybe_unused) {
    // IMPORTANT: this function MUST call rfe->result.cb on every failure path

    int code;

    if(!__atomic_load_n(&dq_globals.running, __ATOMIC_RELAXED)) {
        code = rrd_call_function_error(rfe->result.wb, "Query delegation is not available.",
                                       HTTP_RESP_SERVICE_UNAVAILABLE);
        goto fail;
    }

    struct data_query_payload req = { 0 };
    CLEAN_JSON_OBJECT *jobj = json_parse_function_payload_or_error(
        rfe->result.wb, rfe->payload, &code, data_query_parse_payload, &req);

    if(!jobj || code != HTTP_RESP_OK)
        goto fail;

    if(req.ttl <= 0) {
        // The hop budget is exhausted. This is the cycle terminator for
        // active-active parent clusters.
        code = rrd_call_function_error(rfe->result.wb, "Query delegation hop budget exhausted.",
                                       HTTP_RESP_SERVICE_UNAVAILABLE);
        goto fail;
    }

    struct data_query_job *job = callocz(1, sizeof(*job));
    job->path = req.path;
    job->query = strdupz(req.query ? req.query : "");
    job->access = rfe->user_access;
    job->ttl = (int)req.ttl - 1;
    job->source = rfe->source ? strdupz(rfe->source) : NULL;
    job->result_wb = rfe->result.wb;
    job->result_cb = rfe->result.cb;
    job->result_cb_data = rfe->result.data;
    job->is_cancelled_cb = rfe->is_cancelled.cb;
    job->is_cancelled_data = rfe->is_cancelled.data;
    uuid_copy(job->transaction, *rfe->transaction);

    netdata_mutex_lock(&dq_globals.mutex);

    if(!dq_globals.running || dq_globals.queue_size >= RRDFUNCTIONS_DATA_QUERY_QUEUE_MAX) {
        netdata_mutex_unlock(&dq_globals.mutex);

        data_query_job_free(job);

        nd_log_limit_static_global_var(erl, 1, 0);
        nd_log_limit(&erl, NDLS_DAEMON, NDLP_NOTICE,
                     "DELEGATED QUERY: refusing a request - %d already queued",
                     RRDFUNCTIONS_DATA_QUERY_QUEUE_MAX);

        code = rrd_call_function_error(rfe->result.wb, "Too many delegated queries in flight.",
                                       HTTP_RESP_SERVICE_UNAVAILABLE);
        goto fail;
    }

    DOUBLE_LINKED_LIST_APPEND_ITEM_UNSAFE(dq_globals.queue_head, job, prev, next);
    dq_globals.queue_size++;

    netdata_cond_signal(&dq_globals.cond);
    netdata_mutex_unlock(&dq_globals.mutex);

    // Accepted; the worker owns the job and will call the result callback.
    return HTTP_RESP_OK;

fail:
    if(rfe->result.cb)
        rfe->result.cb(rfe->result.wb, code, rfe->result.data);

    return code;
}

// ----------------------------------------------------------------------------
// lifecycle

void function_data_query_init(void) {
    if(dq_globals.running)
        return;

    netdata_mutex_init(&dq_globals.mutex);
    netdata_cond_init(&dq_globals.cond);

    dq_globals.running = true;

    for(size_t i = 0; i < RRDFUNCTIONS_DATA_QUERY_WORKERS ;i++) {
        char tag[NETDATA_THREAD_TAG_MAX + 1];
        snprintfz(tag, sizeof(tag), "DELEGQ[%zu]", i);
        dq_globals.threads[i] = nd_thread_create(tag, NETDATA_THREAD_OPTION_DEFAULT,
                                                 data_query_worker_thread, NULL);
    }
}

void function_data_query_shutdown(void) {
    netdata_mutex_lock(&dq_globals.mutex);
    if(!dq_globals.running) {
        netdata_mutex_unlock(&dq_globals.mutex);
        return;
    }
    dq_globals.running = false;
    netdata_cond_broadcast(&dq_globals.cond);
    netdata_mutex_unlock(&dq_globals.mutex);

    for(size_t i = 0; i < RRDFUNCTIONS_DATA_QUERY_WORKERS ;i++) {
        if(dq_globals.threads[i]) {
            nd_thread_join(dq_globals.threads[i]);
            dq_globals.threads[i] = NULL;
        }
    }

    // answer anything still queued, so no parent is left waiting for a timeout
    netdata_mutex_lock(&dq_globals.mutex);
    while(dq_globals.queue_head) {
        struct data_query_job *job = dq_globals.queue_head;
        DOUBLE_LINKED_LIST_REMOVE_ITEM_UNSAFE(dq_globals.queue_head, job, prev, next);
        dq_globals.queue_size--;
        netdata_mutex_unlock(&dq_globals.mutex);

        int code = rrd_call_function_error(job->result_wb, "Service is shutting down.",
                                           HTTP_RESP_SERVICE_UNAVAILABLE);
        if(job->result_cb)
            job->result_cb(job->result_wb, code, job->result_cb_data);

        data_query_job_free(job);
        netdata_mutex_lock(&dq_globals.mutex);
    }
    netdata_mutex_unlock(&dq_globals.mutex);
}
