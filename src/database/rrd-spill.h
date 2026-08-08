// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_RRD_SPILL_H
#define NETDATA_RRD_SPILL_H

// Tier-0 offline spill store.
//
// A host that streams to a parent keeps tier 0 in its configured in-memory db
// mode while the parent is reachable, so it does no tier-0 disk writes at all.
// While it is orphaned, it ALSO writes each tier-0 sample into dbengine
// (multidb_ctx[0], reached through host->db[rrd_spill_slot()]), so the parent can
// replicate an outage far longer than the in-memory ring can hold. On reconnect
// the host stops writing but keeps the data queryable until dbengine retention
// expires.
//
// rd->tiers[0] is never touched by any of this. That is deliberate: ML,
// exporting and the replication sender all read tiers[0].smh raw, with no
// metric_dup(), so swapping it under them would be a use-after-free. Adding a
// second store leaves every one of those call sites correct by construction.
//
// This header must be included AFTER daemon/common.h, because it needs the
// [db].offline retention* configuration from netdata-conf-db.h.

#include "rrdhost.h"

// Whether this host should currently be mirroring tier-0 samples into the spill
// store. A pure function of sender state - no state machine to keep in sync, so
// it is safe to call from any collector thread and cheap enough to evaluate once
// per rrdset_timed_done().
//
// ON:  the sender has not been ready to send metrics for at least
//      'offline retention start delay'. This keys off READY_4_METRICS rather
//      than ..._CONNECTED because READY_4_METRICS is the flag the data path
//      itself gates on (stream_send_metrics_init()); CONNECTED is set earlier,
//      on the connector thread, and would leave a window where the socket is up
//      but no metrics flow.
//
// OFF: only once the sender has been ready for 'offline retention stop delay'
//      AND replication has drained. Continuing to spill while the parent is
//      still catching up means a second disconnect mid-backfill cannot open a
//      hole between what was spilled and what was replicated.
//      'offline retention stop max' is the escape hatch for a replication that
//      never completes.
static inline bool rrdhost_spill_should_be_active(RRDHOST *host, time_t now_s) {
    if(!host->spill.enabled)
        return false;

    if(!rrdhost_flag_check(host, RRDHOST_FLAG_STREAM_SENDER_READY_4_METRICS))
        return (now_s - host->spill.not_ready_since_s) >= offline_retention_start_delay_s;

    time_t ready_for_s = now_s - host->spill.ready_since_s;

    if(ready_for_s < offline_retention_stop_delay_s)
        return true;

    // stream_sender_charts_and_replication_reset() zeroes this counter on every
    // connect, so it only becomes meaningful once the sender has been ready for
    // a while - which the check above guarantees.
    if(rrdhost_sender_replicating_charts(host) > 0 && ready_for_s < offline_retention_stop_max_s)
        return true;

    return false;
}

// Called by the streaming sender on the two transitions of
// RRDHOST_FLAG_STREAM_SENDER_READY_4_METRICS.
//
// Both are edge-triggered via ready_since_s, so calling the not-ready hook from
// several teardown paths (the dispatcher's disconnect, the connector's
// on-disconnect, and sender removal) is idempotent. Without that, a path that
// fires more than once would keep pushing not_ready_since_s forward and delay
// the start of spilling indefinitely.
//
// ready_since_s == 0 means "not ready"; the activation predicate only reads it
// after confirming the flag is set, so the zero is never observed as a time.
static inline void rrdhost_spill_sender_became_ready(RRDHOST *host) {
    if(host->spill.enabled)
        host->spill.ready_since_s = now_realtime_sec();
}

static inline void rrdhost_spill_sender_not_ready(RRDHOST *host) {
    if(host->spill.enabled && host->spill.ready_since_s) {
        host->spill.not_ready_since_s = now_realtime_sec();
        host->spill.ready_since_s = 0;
    }
}

// Create or finalize this dimension's spill collect handle so that it matches
// 'active'. On activation it also backfills whatever the in-memory tier-0 ring
// still holds, up to but excluding now_s (the sample the caller is about to
// store through the normal path). Collector thread only.
// Defined in rrddim-collection.c.
void rrddim_spill_toggle(RRDDIM *rd, bool active, time_t now_s);

#endif //NETDATA_RRD_SPILL_H
