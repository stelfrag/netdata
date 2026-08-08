// SPDX-License-Identifier: GPL-3.0-or-later

#include "query-delegation.h"

#include "web/api/functions/function-data-query.h"
#include "web/api/web_api.h"
#include "web/server/web_client.h"

// ----------------------------------------------------------------------------
// Delegating a query to the child that can answer it
//
// The parent's query engine only ever sees its own storage. When a query window
// falls entirely outside what the parent kept, query_metric_add() drops every
// metric, the pruning cascades up through instance -> context -> node, and
// /api/v{2,3}/data answers HTTP 200 with an empty result - even though the child
// that is streaming to us right now still holds the data.
//
// This module closes that gap for the case where the parent has NOTHING for the
// window: it finds the single child that could answer, asks it over the existing
// FUNCTION RPC (see function-data-query.c on the child side), and returns the
// child's response.
//
// Deliberate limits:
//   - only when the parent matched no metrics at all (no RRDR merging);
//   - only one candidate node (no fan-out);
//   - only children with a live receiver (we have no channel to anything else).
// ----------------------------------------------------------------------------

struct delegation_candidate {
    RRDHOST *host;
    size_t matched;                         // how many hosts matched the scope
};

static ssize_t delegation_collect_host(void *data, RRDHOST *host, bool queryable __maybe_unused) {
    struct delegation_candidate *c = data;

    c->matched++;

    // Remember only the first; more than one means we bail out (see below).
    if(!c->host)
        c->host = host;

    return 1;
}

struct child_retention {
    bool known;
    time_t first_time_s;
};

static bool delegation_origin_retention_cb(
    void *userdata, uint16_t index,
    STRING *hostname __maybe_unused,
    ND_UUID host_id __maybe_unused, ND_UUID node_id __maybe_unused, ND_UUID claim_id __maybe_unused,
    int16_t hops __maybe_unused,
    time_t since __maybe_unused, time_t first_time_t,
    uint32_t start_time_ms __maybe_unused, uint32_t shutdown_time_ms __maybe_unused,
    STREAM_CAPABILITIES capabilities __maybe_unused,
    uint32_t flags __maybe_unused) {

    struct child_retention *r = userdata;

    // slot 0 is the origin - the agent where the data was collected
    if(index == 0 && first_time_t > 0) {
        r->known = true;
        r->first_time_s = first_time_t;
    }

    return false;   // we only need the first entry
}

// Does the child plausibly hold data at the start of the requested window?
//
// This is a cheap pre-filter that avoids a pointless round trip for queries far
// older than anything that ever existed. It is intentionally permissive: the
// stored stream path lags the live values and is updated only sparsely (see
// STREAM_PATH.md sections 3 and 6), so "unknown" must mean "try anyway", never
// "skip".
static bool delegation_child_may_have_window(RRDHOST *host, time_t before) {
    struct child_retention r = { .known = false, .first_time_s = 0 };
    rrdhost_stream_path_visit(host, 0, delegation_origin_retention_cb, &r);

    if(!r.known)
        return true;

    return r.first_time_s <= before;
}

// Rebuild the caller's query string, forcing absolute after/before.
//
// The caller may have asked for a relative window ("last 1 hour"). Forwarding
// that verbatim would make the child resolve it against ITS clock, so a skew
// between the two agents would silently shift the answer. qt->window holds the
// window we already resolved, so we send that.
static void delegation_build_query_string(BUFFER *dst, const char *original, time_t after, time_t before) {
    if(original && *original) {
        const char *p = original;

        while(*p) {
            const char *amp = strchr(p, '&');
            size_t len = amp ? (size_t)(amp - p) : strlen(p);

            // drop the caller's after/before; we append resolved ones below
            bool drop =
                (len >= 6 && strncmp(p, "after=", 6) == 0) ||
                (len >= 7 && strncmp(p, "before=", 7) == 0);

            if(!drop && len) {
                if(buffer_strlen(dst))
                    buffer_putc(dst, '&');
                buffer_fast_strcat(dst, p, len);
            }

            if(!amp) break;
            p = amp + 1;
        }
    }

    if(buffer_strlen(dst))
        buffer_putc(dst, '&');

    buffer_sprintf(dst, "after=%lld&before=%lld", (long long)after, (long long)before);
}

bool query_delegation_try(QUERY_TARGET *qt, struct web_client *w,
                          const char *api_path, const char *original_query_string,
                          int timeout_s, int *code) {

    if(!qt || !w || !api_path || !code)
        return false;

    // Hop budget. On a normal query this is the full default; while we are
    // ourselves serving a delegated query it is what our parent left us. This
    // is what terminates cycles in active-active parent clusters.
    int ttl = function_data_query_ttl_remaining();
    if(ttl <= 0)
        return false;

    // ------------------------------------------------------------------------
    // find the single child that could answer

    RRDHOST *host;

    if(qt->request.host) {
        // the URL selected a single host (/host/<guid>/... or /node/<id>/...),
        // so the node patterns were never compiled
        host = qt->request.host;
    }
    else {
        struct delegation_candidate candidate = { .host = NULL, .matched = 0 };
        query_scope_foreach_host(qt->nodes.scope_pattern, qt->nodes.pattern,
                                 delegation_collect_host, &candidate,
                                 NULL, NULL);

        if(candidate.matched != 1 || !candidate.host)
            // no node, or a fan-out we do not support yet
            return false;

        host = candidate.host;
    }

    if(host == localhost)
        // we are the origin; there is nobody below us to ask
        return false;

    // We can only talk to a child that is streaming to us right now. An
    // archived host, or one that moved to a sibling parent, has no channel.
    if(!host->receiver)
        return false;

    // Is the child new enough to serve delegated queries? Children advertise
    // their global functions on connect, so the presence of this function in
    // host->functions IS the capability negotiation - no STREAM_CAP bit needed.
    if(!rrd_function_available(host, RRDFUNCTIONS_DATA_QUERY))
        return false;

    if(!delegation_child_may_have_window(host, qt->window.before))
        return false;

    // ------------------------------------------------------------------------
    // build the request

    CLEAN_BUFFER *query = buffer_create(0, NULL);
    delegation_build_query_string(query, original_query_string, qt->window.after, qt->window.before);

    CLEAN_BUFFER *payload = buffer_create(0, NULL);
    buffer_json_initialize(payload, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_MINIFY);
    buffer_json_member_add_string(payload, RRDFUNCTIONS_DATA_QUERY_KEY_PATH, api_path);
    buffer_json_member_add_string(payload, RRDFUNCTIONS_DATA_QUERY_KEY_QUERY, buffer_tostring(query));
    buffer_json_member_add_int64(payload, RRDFUNCTIONS_DATA_QUERY_KEY_TTL, ttl);
    buffer_json_finalize(payload);

    CLEAN_BUFFER *source = buffer_create(100, NULL);
    user_auth_to_source_buffer(&w->user_auth, source);

    usec_t started_ut = now_monotonic_usec();

    // ------------------------------------------------------------------------
    // ask the child
    //
    // wait=true: we are on the HTTP thread and the caller expects a response.
    // The interrupt callback lets a Cloud-side cancellation turn into a
    // FUNCTION_CANCEL that reaches the child's running query.

    BUFFER *response = buffer_create(0, NULL);

    // allow_restricted = true.
    //
    // The delegation function is registered with RRDFUNCTIONS_TAG_HIDDEN, which
    // makes it RRD_FUNCTION_RESTRICTED - so /api/v1/function (which passes
    // false) can never invoke it directly, which is exactly what we want. This
    // call is not a user asking for a function by name; it is the agent itself
    // continuing a data query that was already authorized above. The user's own
    // access still travels as user_access and is verified against the
    // function's registered access on the child.
    // transaction = NULL, so rrd_function_run() mints a fresh one.
    //
    // We must NOT reuse w->transaction here. When this agent is itself serving
    // a delegated query, the inbound request is already registered in the
    // GLOBAL inflight dictionary under that same transaction id; registering
    // the onward call under it too would be rejected as "Duplicate
    // transaction." (rrdfunctions-inflight.c), breaking every delegation past
    // the first hop. Cancellation still chains correctly: our interrupt
    // callback polls the inbound request's cancelled flag.
    int rc = rrd_function_run(
        host, response, timeout_s > 0 ? timeout_s : 60,
        w->user_auth.access, RRDFUNCTIONS_DATA_QUERY,
        true, NULL,
        NULL, NULL,
        web_client_progress_functions_update, NULL,
        web_client_interrupt_callback, w,
        payload, buffer_tostring(source), true);

    usec_t duration_ut = now_monotonic_usec() - started_ut;

    if(rc != HTTP_RESP_OK) {
        // Fall back to the local (empty) answer rather than surfacing the
        // delegation failure - the caller is never worse off than before.
        nd_log(NDLS_DAEMON, NDLP_INFO,
               "DELEGATED QUERY: node '%s' could not serve the query (code %d, %llu ms) - "
               "answering locally instead",
               rrdhost_hostname(host), rc, (unsigned long long)(duration_ut / USEC_PER_MS));

        buffer_free(response);
        return false;
    }

    nd_log(NDLS_ACCESS, NDLP_INFO,
           "DELEGATED QUERY: served by node '%s' in %llu ms (%zu bytes)",
           rrdhost_hostname(host),
           (unsigned long long)(duration_ut / USEC_PER_MS),
           buffer_strlen(response));

    buffer_flush(w->response.data);
    buffer_fast_strcat(w->response.data, buffer_tostring(response), buffer_strlen(response));
    w->response.data->content_type = response->content_type;
    w->response.data->expires = response->expires;

    buffer_free(response);

    *code = HTTP_RESP_OK;
    return true;
}
