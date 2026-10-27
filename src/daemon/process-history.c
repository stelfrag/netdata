// SPDX-License-Identifier: GPL-3.0-or-later

#include "common.h"
#include "process-history.h"
#include "alert-evidence.h"

#define PROCESS_HISTORY_FUNCTION            "__processes-save"
#define PROCESS_HISTORY_EXIT_BEFORE_S       1800    // all the RAM apps.plugin holds
#define PROCESS_HISTORY_EXIT_TIMEOUT_S      3
#define PROCESS_HISTORY_QUEUE_MAX           64
#define PROCESS_HISTORY_REASON_MAX          128
#define PROCESS_HISTORY_MIN_INTERVAL_S      10      // requests closer than this are merged into one call
#define PROCESS_HISTORY_BEFORE_S            600
#define PROCESS_HISTORY_AFTER_S             120
#define PROCESS_HISTORY_ANOMALY_RATE        0.10    // of the node's dimensions
#define PROCESS_HISTORY_ANOMALY_SECONDS     60

typedef struct {
    time_t at_s;
    char reason[PROCESS_HISTORY_REASON_MAX];
} PROCESS_HISTORY_REQUEST;

static struct {
    bool running;                   // the thread is up; requests are dropped otherwise

    SPINLOCK spinlock;
    PROCESS_HISTORY_REQUEST queue[PROCESS_HISTORY_QUEUE_MAX];
    size_t queued;
    size_t dropped;

    // anomaly episodes, updated only by the ML detection thread of localhost
    time_t anomalous_since_s;
    bool anomaly_requested;
} ph = {
    .spinlock = SPINLOCK_INITIALIZER,
};

// --------------------------------------------------------------------------------------------------------------------
// producers

void process_history_request(time_t at_s, const char *reason) {
    if(!__atomic_load_n(&ph.running, __ATOMIC_ACQUIRE))
        return;

    spinlock_lock(&ph.spinlock);
    if(ph.queued < PROCESS_HISTORY_QUEUE_MAX) {
        PROCESS_HISTORY_REQUEST *r = &ph.queue[ph.queued++];
        r->at_s = at_s;

        // the reason travels as one word of the Function call
        size_t i = 0;
        for(const char *s = reason ? reason : "unspecified"; *s && i < sizeof(r->reason) - 1; s++)
            r->reason[i++] = (isspace((uint8_t)*s) || *s == '"' || *s == '\'' || *s == '\\') ? '_' : *s;
        r->reason[i] = '\0';
    }
    else {
        // a full queue extends its newest request, so the window still reaches this moment
        PROCESS_HISTORY_REQUEST *r = &ph.queue[PROCESS_HISTORY_QUEUE_MAX - 1];
        r->at_s = MAX(r->at_s, at_s);
        ph.dropped++;
    }
    spinlock_unlock(&ph.spinlock);
}

void process_history_anomaly_rate(double rate) {
    if(!__atomic_load_n(&ph.running, __ATOMIC_ACQUIRE))
        return;

    time_t now_s = now_realtime_sec();
    if(rate < PROCESS_HISTORY_ANOMALY_RATE) {
        ph.anomalous_since_s = 0;
        ph.anomaly_requested = false;
        return;
    }

    if(!ph.anomalous_since_s)
        ph.anomalous_since_s = now_s;

    // once per episode: the save window covers the minute before and the minutes after
    if(!ph.anomaly_requested && now_s - ph.anomalous_since_s >= PROCESS_HISTORY_ANOMALY_SECONDS) {
        ph.anomaly_requested = true;
        char reason[64];
        snprintfz(reason, sizeof(reason), "anomaly-rate:%.0f%%", rate * 100.0);
        process_history_request(ph.anomalous_since_s, reason);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// the thread

bool process_history_enabled(void) {
#if defined(OS_LINUX)
    // decision 2: saving to disk needs a disk-backed database, and never on iot
    return default_rrd_memory_mode == RRD_DB_MODE_DBENGINE && !netdata_conf_is_iot();
#else
    return false;
#endif
}

struct process_history_call {
    BUFFER *wb;
    char cmd[PROCESS_HISTORY_REASON_MAX + 128];
};

static void process_history_result(BUFFER *wb __maybe_unused, int code, void *data) {
    struct process_history_call *c = data;

    if(code == HTTP_RESP_OK)
        nd_log(NDLS_DAEMON, NDLP_DEBUG, "PROCESS_HISTORY: '%s' accepted", c->cmd);
    else {
        nd_log_limit_static_global_var(erl, 3600, 0);
        nd_log_limit(&erl, NDLS_DAEMON, NDLP_INFO,
                     "PROCESS_HISTORY: apps.plugin answered %d to '%s' (is its flight recorder enabled?)",
                     code, c->cmd);
    }

    buffer_free(c->wb);
    freez(c);
}

static void process_history_send(const PROCESS_HISTORY_REQUEST *reqs, size_t n) {
    // one call covers them all: the window ends after the latest moment and reaches back to the earliest
    time_t first_s = reqs[0].at_s, last_s = reqs[0].at_s;
    for(size_t i = 1; i < n; i++) {
        first_s = MIN(first_s, reqs[i].at_s);
        last_s = MAX(last_s, reqs[i].at_s);
    }

    struct process_history_call *c = callocz(1, sizeof(*c));
    c->wb = buffer_create(0, NULL);
    snprintfz(c->cmd, sizeof(c->cmd), PROCESS_HISTORY_FUNCTION " at:%lld before:%lld after:%d reason:%s%s",
              (long long)last_s, (long long)(PROCESS_HISTORY_BEFORE_S + (last_s - first_s)), PROCESS_HISTORY_AFTER_S,
              reqs[0].reason, n > 1 ? "+more" : "");

    nd_log(NDLS_DAEMON, NDLP_INFO, "PROCESS_HISTORY: saving process history around %lld (%s%s)",
           (long long)last_s, reqs[0].reason, n > 1 ? " and more" : "");

    nrpc_call(&(struct nrpc_call_spec) {
        .owner = rrdhost_nrpc_owner(localhost),
        .result_wb = c->wb,
        .cmd = c->cmd,
        .source = "process-history",
        .user_access = HTTP_ACCESS_ALL,
        .timeout_s = 10,
        .wait = false,
        .allow_restricted = true,
        .result.cb = process_history_result,
        .result.data = c,
    });

    // open connections exist only now; the alert-evidence Function reads them back
    alert_evidence_capture_connections();
}

void process_history_save_on_exit(void) {
#if defined(OS_LINUX)
    if(!process_history_enabled() || !localhost)
        return;

    char cmd[128];
    snprintfz(cmd, sizeof(cmd), PROCESS_HISTORY_FUNCTION " at:%lld before:%d after:0 reason:agent-exit seal:yes",
              (long long)now_realtime_sec(), PROCESS_HISTORY_EXIT_BEFORE_S);

    CLEAN_BUFFER *wb = buffer_create(0, NULL);
    usec_t started_ut = now_monotonic_usec();
    int code = nrpc_call(&(struct nrpc_call_spec) {
        .owner = rrdhost_nrpc_owner(localhost),
        .result_wb = wb,
        .cmd = cmd,
        .source = "process-history",
        .user_access = HTTP_ACCESS_ALL,
        .timeout_s = PROCESS_HISTORY_EXIT_TIMEOUT_S,
        .wait = true,
        .allow_restricted = true,
    });

    nd_log(NDLS_DAEMON, code == HTTP_RESP_OK ? NDLP_INFO : NDLP_NOTICE,
           "PROCESS_HISTORY: keeping the recent process history before exiting: apps.plugin answered %d in %llu ms",
           code, (unsigned long long)((now_monotonic_usec() - started_ut) / USEC_PER_MS));
#endif
}

static void process_history_main_cleanup(void *pptr) {
    struct netdata_static_thread *static_thread = CLEANUP_FUNCTION_GET_PTR(pptr);
    if(!static_thread) return;

    static_thread->enabled = NETDATA_MAIN_THREAD_EXITING;
    __atomic_store_n(&ph.running, false, __ATOMIC_RELEASE);
    worker_unregister();
    static_thread->enabled = NETDATA_MAIN_THREAD_EXITED;
}

void process_history_main(void *ptr) {
    CLEANUP_FUNCTION_REGISTER(process_history_main_cleanup) cleanup_ptr = ptr;

    worker_register("PROCHIST");
    worker_register_job_name(0, "save request");

    alert_evidence_init();
    __atomic_store_n(&ph.running, true, __ATOMIC_RELEASE);

    PROCESS_HISTORY_REQUEST batch[PROCESS_HISTORY_QUEUE_MAX];
    time_t last_sent_s = 0;

    heartbeat_t hb;
    heartbeat_init(&hb, USEC_PER_SEC);
    while(service_running(SERVICE_COLLECTORS)) {
        worker_is_idle();
        heartbeat_next(&hb);

        alert_evidence_retry();

        time_t now_s = now_realtime_sec();
        if(now_s - last_sent_s < PROCESS_HISTORY_MIN_INTERVAL_S)
            continue;

        spinlock_lock(&ph.spinlock);
        size_t n = ph.queued, dropped = ph.dropped;
        memcpy(batch, ph.queue, n * sizeof(*batch));
        ph.queued = 0;
        ph.dropped = 0;
        spinlock_unlock(&ph.spinlock);

        if(!n)
            continue;

        if(dropped)
            nd_log(NDLS_DAEMON, NDLP_INFO, "PROCESS_HISTORY: merged %zu more save requests", dropped);

        worker_is_busy(0);
        process_history_send(batch, n);
        last_sent_s = now_s;
    }
}
