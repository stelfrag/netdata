// SPDX-License-Identifier: GPL-3.0-or-later

#define PULSE_INTERNALS
#include "pulse.h"

// --------------------------------------------------------------------------------------------------------------------
// parents

struct by_reason {
    size_t counters[STREAM_HANDSHAKE_NEGATIVE_MAX + 3];
    RRDSET *st;
    RRDDIM *rd[STREAM_HANDSHAKE_NEGATIVE_MAX + 3];
};

#define STREAM_HANDSHAKE_STREAM_INFO (STREAM_HANDSHAKE_NEGATIVE_MAX)
#define STREAM_HANDSHAKE_CONNECT (STREAM_HANDSHAKE_NEGATIVE_MAX + 1)
#define STREAM_HANDSHAKE_OTHER (STREAM_HANDSHAKE_NEGATIVE_MAX + 2)

struct {
    struct {
        // event counters (event-driven)
        struct by_reason events_by_reason;
        struct by_reason disconnects_by_reason;

        // gauge chart pointers (the per-state counts are computed read-side by the traversal)
        struct {
            RRDSET *st_nodes;
            RRDDIM *rd_loading;
            RRDDIM *rd_local;
            RRDDIM *rd_virtual;
            RRDDIM *rd_archived;
            RRDDIM *rd_offline;
            RRDDIM *rd_waiting;
            RRDDIM *rd_replication_waiting;
            RRDDIM *rd_replicating;
            RRDDIM *rd_running;
        } type[2];
    } parent;

    struct {
        // event counters (event-driven)
        struct by_reason stream_info_failed_by_reason;
        struct by_reason events_by_reason;
        struct by_reason disconnects_by_reason;
    } sender;

} p = { 0 };

static PULSE_HOST_STATUS pulse_host_detect_receiver_status(RRDHOST *host) {
    RRDHOST_STATUS status = { 0 };
    rrdhost_status(host, now_realtime_sec(), &status, RRDHOST_STATUS_BASIC);

    PULSE_HOST_STATUS rc = 0;

    if(status.db.status == RRDHOST_DB_STATUS_INITIALIZING || status.ingest.status == RRDHOST_INGEST_STATUS_INITIALIZING)
        rc = PULSE_HOST_STATUS_LOADING;

    else if(status.ingest.type == RRDHOST_INGEST_TYPE_LOCALHOST)
        rc = PULSE_HOST_STATUS_LOCAL;

    else if(status.ingest.type == RRDHOST_INGEST_TYPE_VIRTUAL)
        rc = PULSE_HOST_STATUS_VIRTUAL;

    else if(status.ingest.status == RRDHOST_INGEST_STATUS_ARCHIVED)
        rc = PULSE_HOST_STATUS_ARCHIVED;

    else if(status.ingest.status == RRDHOST_INGEST_STATUS_REPLICATING)
        rc = PULSE_HOST_STATUS_RCV_REPLICATING;

    else if(status.ingest.status == RRDHOST_INGEST_STATUS_OFFLINE)
        rc = PULSE_HOST_STATUS_RCV_OFFLINE;

    else if(status.ingest.status == RRDHOST_INGEST_STATUS_ONLINE)
        rc = PULSE_HOST_STATUS_RCV_RUNNING;

    return rc;
}

static void update_reason(struct by_reason *b, STREAM_HANDSHAKE reason) {
    int r = reason;

    if(r >= 0)
        r = 0;
    else if(r > -STREAM_HANDSHAKE_NEGATIVE_MAX)
        r = -reason;
    else
        r = STREAM_HANDSHAKE_NEGATIVE_MAX;

    __atomic_add_fetch(&b->counters[r], 1, __ATOMIC_RELAXED);
}

static void pulse_host_add_sub_status(PULSE_HOST_STATUS status, ssize_t val, STREAM_HANDSHAKE reason) {
    status &= ~(PULSE_HOST_STATUS_EPHEMERAL | PULSE_HOST_STATUS_PERMANENT);

    while(status) {
        PULSE_HOST_STATUS s = 1 << (__builtin_ffs(status) - 1);
        status &= ~s;

        bool do_parent_reason = false, do_sender_reason = false;

        switch(s) {
            default:
                break;

            // inbound and outbound node gauges are now computed read-side by the pulse traversal;
            // only the event-driven reason/event counters remain here.
            case PULSE_HOST_STATUS_RCV_OFFLINE:
                do_parent_reason = true;
                break;

            case PULSE_HOST_STATUS_RCV_WAITING:
                do_parent_reason = true;
                reason = 0;
                break;

            case PULSE_HOST_STATUS_SND_OFFLINE:
                do_sender_reason = true;
                break;

            case PULSE_HOST_STATUS_SND_CONNECTING:
                __atomic_add_fetch(&p.sender.events_by_reason.counters[STREAM_HANDSHAKE_CONNECT], 1, __ATOMIC_RELAXED);
                break;
        }

        if(do_parent_reason && val > 0)
            update_reason(&p.parent.disconnects_by_reason, reason);

        if(do_sender_reason && val > 0)
            update_reason(&p.sender.disconnects_by_reason, reason);
    }
}

void pulse_host_status(RRDHOST *host, PULSE_HOST_STATUS status, STREAM_HANDSHAKE reason) {
    PULSE_HOST_STATUS remove = 0;

    if(!status)
        status = pulse_host_detect_receiver_status(host);

    static const PULSE_HOST_STATUS ephemerality =
        PULSE_HOST_STATUS_EPHEMERAL | PULSE_HOST_STATUS_PERMANENT;

    static const PULSE_HOST_STATUS basic =
        PULSE_HOST_STATUS_LOCAL | PULSE_HOST_STATUS_VIRTUAL | PULSE_HOST_STATUS_LOADING |
        PULSE_HOST_STATUS_ARCHIVED | PULSE_HOST_STATUS_DELETED;

    static const PULSE_HOST_STATUS receiver =
        PULSE_HOST_STATUS_RCV_OFFLINE | PULSE_HOST_STATUS_RCV_WAITING | PULSE_HOST_STATUS_RCV_REPLICATING |
        PULSE_HOST_STATUS_RCV_REPLICATION_WAIT | PULSE_HOST_STATUS_RCV_RUNNING;

    static const PULSE_HOST_STATUS sender =
        PULSE_HOST_STATUS_SND_OFFLINE | PULSE_HOST_STATUS_SND_PENDING | PULSE_HOST_STATUS_SND_CONNECTING |
        PULSE_HOST_STATUS_SND_WAITING | PULSE_HOST_STATUS_SND_REPLICATING | PULSE_HOST_STATUS_SND_RUNNING |
        PULSE_HOST_STATUS_SND_NO_DST | PULSE_HOST_STATUS_SND_NO_DST_FAILED;

    if((status & (basic | receiver)) && !(status & ephemerality))
        status |= rrdhost_option_check(host, RRDHOST_OPTION_EPHEMERAL_HOST) ?
                      PULSE_HOST_STATUS_EPHEMERAL : PULSE_HOST_STATUS_PERMANENT;

    // running-latch: once the node first reaches RCV_RUNNING after a (re)connect, ignore the
    // per-chart replication ripples that would otherwise flip the host status back to replicating.
    // Makes no assumption that replication happens: RCV_RUNNING may be reached directly (replication
    // disabled), and the block path is only ever taken for an incoming RCV_REPLICATING.
    if(status & PULSE_HOST_STATUS_RCV_RUNNING)
        __atomic_store_n(&host->stream.rcv.status.running_latched, true, __ATOMIC_RELAXED);
    else if(status & PULSE_HOST_STATUS_RCV_REPLICATING) {
        if(__atomic_load_n(&host->stream.rcv.status.running_latched, __ATOMIC_RELAXED) &&
           (__atomic_load_n(&host->stream.pulse_state, __ATOMIC_RELAXED) & PULSE_HOST_STATUS_RCV_RUNNING))
            status = (PULSE_HOST_STATUS)((status & ~PULSE_HOST_STATUS_RCV_REPLICATING) | PULSE_HOST_STATUS_RCV_RUNNING);
        else
            __atomic_store_n(&host->stream.rcv.status.running_latched, false, __ATOMIC_RELAXED);
    }
    else if(status & PULSE_HOST_STATUS_RCV_OFFLINE)
        __atomic_store_n(&host->stream.rcv.status.running_latched, false, __ATOMIC_RELAXED);

    if(status & basic)
        remove = basic | receiver | ephemerality | sender;
    else if(status & receiver)
        remove = basic | receiver | ephemerality;
    else if(status & sender)
        remove = sender;

    // maintain the combined resolved state on the host (CAS, lock-free; replaces the global PHOST
    // Judy + spinlock). The pulse traversal reads it to compute the streaming_inbound and
    // streaming_outbound gauges read-side.
    uint32_t cur = __atomic_load_n(&host->stream.pulse_state, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (status == PULSE_HOST_STATUS_DELETED) ? 0u
                                                     : (uint32_t)((cur & ~(uint32_t)remove) | (uint32_t)status);
    } while(!__atomic_compare_exchange_n(&host->stream.pulse_state, &cur, next,
                                         false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

    PULSE_HOST_STATUS old = (PULSE_HOST_STATUS)cur; // the state we transitioned from

    // reset the inbound-state age timer whenever the inbound (basic|receiver) state changes
    if(((PULSE_HOST_STATUS)next & (basic | receiver)) != (old & (basic | receiver)))
        __atomic_store_n(&host->stream.rcv.status.state_changed_s, now_realtime_sec(), __ATOMIC_RELAXED);

    if(status == PULSE_HOST_STATUS_DELETED)
        status = 0; // do not add anything, just remove the old flags

    // event-driven reason/event counters only (gauges are computed by the traversal)
    remove &= old;
    pulse_host_add_sub_status(remove, -1, 0);
    pulse_host_add_sub_status(status, 1, reason);
}

void pulse_parent_stream_info_received_request(void) {
    __atomic_add_fetch(&p.parent.events_by_reason.counters[STREAM_HANDSHAKE_STREAM_INFO], 1, __ATOMIC_RELAXED);
}

void pulse_parent_receiver_request(void) {
    __atomic_add_fetch(&p.parent.events_by_reason.counters[STREAM_HANDSHAKE_CONNECT], 1, __ATOMIC_RELAXED);
}

void pulse_parent_receiver_rejected(STREAM_HANDSHAKE reason) {
    update_reason(&p.parent.events_by_reason, reason);
}

// --------------------------------------------------------------------------------------------------------------------
// children / senders

void pulse_stream_info_sent_request(void) {
    __atomic_add_fetch(&p.sender.events_by_reason.counters[STREAM_HANDSHAKE_STREAM_INFO], 1, __ATOMIC_RELAXED);
}

void pulse_sender_stream_info_failed(const char *destination __maybe_unused, STREAM_HANDSHAKE reason) {
    update_reason(&p.sender.stream_info_failed_by_reason, reason);
}

void pulse_sender_connection_failed(const char *destination __maybe_unused, STREAM_HANDSHAKE reason) {
    update_reason(&p.sender.events_by_reason, reason);
}

// --------------------------------------------------------------------------------------------------------------------

static void chart_by_reason(struct by_reason *b, const char *id, const char *context, const char *title, const char *label, int priority) {
    if(!b->st) {
        b->st = rrdset_create_localhost(
            "netdata"
            , id
            , NULL
            , "Streaming"
            , context
            , title
            , "events/s"
            , "netdata"
            , "pulse"
            , priority
            , localhost->rrd_update_every
            , RRDSET_TYPE_LINE
        );

        for(int i = 0; i < STREAM_HANDSHAKE_NEGATIVE_MAX ;i++) {
            char buf[1024];
            if(!i)
                strncpyz(buf, "connected", sizeof(buf) - 1);
            else
                strncpyz(buf, stream_handshake_error_to_string(-i), sizeof(buf) - 1);
            for(int c = 0; buf[c] ;c++)
                buf[c] = (char)tolower(buf[c]);

            b->rd[i] = rrddim_add(b->st, buf, NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrdlabels_add(b->st->rrdlabels, "type", label, RRDLABEL_SRC_AUTO);

        b->rd[STREAM_HANDSHAKE_STREAM_INFO] = rrddim_add(b->st, "info", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        b->rd[STREAM_HANDSHAKE_CONNECT] = rrddim_add(b->st, "connect", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        b->rd[STREAM_HANDSHAKE_OTHER] = rrddim_add(b->st, "other", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    for(size_t i = 0; i <= STREAM_HANDSHAKE_OTHER ;i++)
        rrddim_set_by_pointer(b->st, b->rd[i], (collected_number)__atomic_load_n(&b->counters[i], __ATOMIC_RELAXED));

    rrdset_done(b->st);
}

// --------------------------------------------------------------------------------------------------------------------
// inbound aggregate + per-child charts (parent only)
//
// One read-only pass over the rrdhost dictionary computes BOTH the streaming_inbound aggregate
// (nodes per ephemerality x state) AND each child's per-instance charts on the parent's localhost.
// No global shared counters and no rrd_rdlock, and every per-host value read here is a
// single-writer relaxed atomic.
//
// CAVEAT, pre-existing: dfe_start_reentrant() refcounts the dictionary ITEM, not the RRDHOST.
// rrdhost_root_index is DICT_OPTION_VALUE_LINK_DONT_CLONE with no delete callback, so the
// dictionary never owns the host and freez(host) is not serialized by it - only rrd_rdlock()
// gives lifetime here (see the comment above rrdhost_find_and_run() in src/database/rrdhost.c).
// So this traversal can, in principle, READ a host that is being freed concurrently by
// svc_rrdhost_cleanup_orphan_hosts() (src/daemon/service.c) or by "netdatacli remove-stale-node
// --unregister" (src/daemon/commands.c). The worst outcome of such a read is a garbage sample.
//
// What this pass deliberately does NOT do: it never writes to the host, and it keeps no pointer of
// its own inside the host. All pulse-owned per-child state - the resolved chart set and the
// applied-labels version - lives in pulse_child_charts_registry below, which belongs to the pulse
// thread alone. So a host being freed under us cannot hand us a dangling RRDSET_ACQUIRED to
// dereference, and nothing here can write into freed memory.
//
// Closing the remaining read window needs rrd_rdlock() around this traversal, which serialises the
// whole pass against every host add and remove; that is tracked separately.

typedef enum {
    PULSE_INBOUND_LOCAL = 0,
    PULSE_INBOUND_VIRTUAL,
    PULSE_INBOUND_LOADING,
    PULSE_INBOUND_ARCHIVED,
    PULSE_INBOUND_OFFLINE,
    PULSE_INBOUND_WAITING,
    PULSE_INBOUND_REPLICATION_WAITING,
    PULSE_INBOUND_REPLICATING,
    PULSE_INBOUND_RUNNING,

    PULSE_INBOUND_MAX,
} PULSE_INBOUND_STATE;

static PULSE_INBOUND_STATE pulse_inbound_state(PULSE_HOST_STATUS s) {
    if(s & PULSE_HOST_STATUS_LOCAL)                 return PULSE_INBOUND_LOCAL;
    if(s & PULSE_HOST_STATUS_VIRTUAL)               return PULSE_INBOUND_VIRTUAL;
    if(s & PULSE_HOST_STATUS_LOADING)               return PULSE_INBOUND_LOADING;
    if(s & PULSE_HOST_STATUS_ARCHIVED)              return PULSE_INBOUND_ARCHIVED;
    if(s & PULSE_HOST_STATUS_RCV_OFFLINE)           return PULSE_INBOUND_OFFLINE;
    if(s & PULSE_HOST_STATUS_RCV_WAITING)           return PULSE_INBOUND_WAITING;
    if(s & PULSE_HOST_STATUS_RCV_REPLICATION_WAIT)  return PULSE_INBOUND_REPLICATION_WAITING;
    if(s & PULSE_HOST_STATUS_RCV_REPLICATING)       return PULSE_INBOUND_REPLICATING;
    if(s & PULSE_HOST_STATUS_RCV_RUNNING)           return PULSE_INBOUND_RUNNING;
    return PULSE_INBOUND_MAX;
}

typedef enum {
    PULSE_OUTBOUND_OFFLINE = 0,
    PULSE_OUTBOUND_CONNECTING,
    PULSE_OUTBOUND_PENDING,
    PULSE_OUTBOUND_WAITING,
    PULSE_OUTBOUND_REPLICATING,
    PULSE_OUTBOUND_RUNNING,
    PULSE_OUTBOUND_NO_DST,
    PULSE_OUTBOUND_NO_DST_FAILED,

    PULSE_OUTBOUND_MAX,
} PULSE_OUTBOUND_STATE;

static PULSE_OUTBOUND_STATE pulse_outbound_state(PULSE_HOST_STATUS s) {
    if(s & PULSE_HOST_STATUS_SND_OFFLINE)       return PULSE_OUTBOUND_OFFLINE;
    if(s & PULSE_HOST_STATUS_SND_CONNECTING)    return PULSE_OUTBOUND_CONNECTING;
    if(s & PULSE_HOST_STATUS_SND_PENDING)       return PULSE_OUTBOUND_PENDING;
    if(s & PULSE_HOST_STATUS_SND_WAITING)       return PULSE_OUTBOUND_WAITING;
    if(s & PULSE_HOST_STATUS_SND_REPLICATING)   return PULSE_OUTBOUND_REPLICATING;
    if(s & PULSE_HOST_STATUS_SND_RUNNING)       return PULSE_OUTBOUND_RUNNING;
    if(s & PULSE_HOST_STATUS_SND_NO_DST)        return PULSE_OUTBOUND_NO_DST;
    if(s & PULSE_HOST_STATUS_SND_NO_DST_FAILED) return PULSE_OUTBOUND_NO_DST_FAILED;
    return PULSE_OUTBOUND_MAX;
}

// --------------------------------------------------------------------------------------------------------------------
// per-child pulse charts: one set per child, resolved by id once and then held in
// pulse_child_charts_registry - a registry owned exclusively by the pulse thread, keyed by the
// child's machine_guid.
//
// Why a registry and not a member of the RRDHOST: rrdhost_root_index does not own the RRDHOST (see
// the CAVEAT above), so anything cached inside a host can be read after that host has been freed.
// This registry is created, read, written and destroyed ONLY here, on the pulse thread -
// pulse_parents_do() has a single caller, pulse_thread_main() - so it needs no locking and no host
// teardown path can reach it. Hence DICT_OPTION_SINGLE_THREADED: a second writer would be a bug,
// not a supported mode.
//
// Why the cache: re-resolving the set on every pass was the bulk of this thread's work on a parent
// with many children. rrdset_create_localhost() on an existing chart costs two chart-index lookups
// - the second, in rrdset_index_add_and_acquire(), takes the localhost chart-index WRITE lock to run
// rrdset_conflict_callback(), so it also serialises against every other collector and query on
// localhost - plus that callback's metadata field compares and its unconditional
// rrdset_update_permanent_labels(), plus rrdset_reset_name(). That last one is pure waste for any
// chart created without an explicit name: rrdset_create_custom() calls it with `id`, while st->name
// was built by rrdset_fix_name() as "<type>.<id>", so its strcmp guard can never match and it runs
// the full path (sanitize + a chart-name-index lookup) only to return 0. Four times per child per
// second; measured at 29% of this thread on an 808-host parent, with the label probe below another
// 23%. The registry replaces all of it with one hashed lookup per child per pass.
//
// Why holding the chart references is safe, and why a raw RRDSET * would NOT be. A cached chart
// CAN be obsoleted by something else: pluginsd applies "CHART <id> ... obsolete" to whatever the id
// resolves to, with no ownership check (src/plugins.d/pluginsd_parser.c), and chart ids on localhost
// are not a protected namespace. Once obsolete, svc_rrdset_lock_for_deletion() frees the chart as
// soon as its last_accessed / last_updated / last_collected are all older than
// rrdset_free_obsolete_time_s (src/daemon/service.c) - which happens whenever a pulse step is
// further apart than that window, since [pulse] update every has no upper clamp.
//
// Re-resolving by id every pass used to absorb this for free: rrdset_create_custom() revives an
// obsoleted chart under its destroy_lock before returning it (src/database/rrdset-index-id.c). A
// plain pointer cache removes that revival, and an ASAN run reproduced the resulting
// heap-use-after-free (pulse reading a chart the obsolete-chart reaper had already freed). So the
// cache does two things instead:
//   1. it holds an ACQUIRED reference on each chart's dictionary item, so the item cannot be freed
//      while cached - the dictionary refuses to free a referenced item;
//   2. it re-checks RRDSET_FLAG_OBSOLETE on every pass and, if set, releases the reference and
//      re-resolves through rrdset_create_localhost(), which performs the same revival the old
//      per-pass path did;
// (1) keeps the object alive so that (2)'s flag read is safe, and (2) keeps us from silently
// collecting into a chart the reaper has already unlinked from the index.
//
// Reclamation - mark and sweep, entirely on this thread:
//   3. pulse_parents_traverse() bumps pulse_child_charts_pass once per pass; every child the
//      traversal produces stamps its entry with that pass number; pulse_child_charts_sweep() then
//      deletes every entry that was not stamped, and the registry's delete callback releases that
//      entry's four acquired references.
// A departed child's references are therefore released exactly one pulse step after its last
// appearance, WITHOUT reading the (possibly freed) RRDHOST: "gone" is defined as "not produced by
// the traversal", never as anything read out of a host. Both release sites - dictionary_del() in
// the sweep and dictionary_destroy() in pulse_parents_cleanup() - run on the pulse thread.
//
// This is what an RRDHOST-resident cache could not do: the release would have had to run from
// rrdhost_free_unlinked() (src/database/rrdhost.c), i.e. on the teardown thread, while this
// traversal may still be inside the same host - a second unsynchronised writer of those slots, and
// no caller helps, since rrdhost_free___while_having_rrd_wrlock() holds rrd_wrlock but this
// traversal never takes rrd_rdlock.
//
// (3) also matters to the reaper: it now skips (and re-arms RRDHOST_FLAG_PENDING_OBSOLETE_CHARTS
// for) any obsolete chart whose dictionary item somebody else holds. A departed child's references
// used to live for the rest of the agent's life, so its four charts, once obsoleted, would have
// made every subsequent service run re-scan and re-arm forever. With the sweep, the reference is
// gone one pulse step later and the reaper completes.
//
// One accepted narrowing versus the old per-pass path, needing an adversarial obsoleter to reach at
// all (the reaper can only win the staleness race when a pulse step is further apart than
// rrdset_free_obsolete_time_s, so it is not reachable at the default 1s cadence): if the obsoletion
// and the unlink both land after (2)'s flag read but before this pass's rrdset_done(), we collect
// into a just-unlinked chart for that single pass and re-resolve on the next one. The old code
// re-added the chart within the same pass. Non-crashing thanks to the reference, and it costs at
// most one sample.
//
// The reaper no longer frees a chart whose dictionary item somebody else holds: it applies the same
// reference test it already applied to dimensions (src/daemon/service.c), skipping the chart and
// re-arming RRDHOST_FLAG_PENDING_OBSOLETE_CHARTS instead of leaving an unindexed, OBSOLETE chart
// with its destroy_lock held. So a cached chart of ours can no longer become a zombie that fatal()s
// another collector in rrdset_timed_done().
//
// Also note the conflict callback no longer re-asserts title/units/family/context/priority/
// chart_type/plugin/module on every pass. Nothing we own changes them, so this is invisible in
// normal operation; a colliding collector's overrides would now persist until the next obsolete or
// re-resolve instead of being corrected within a second.
//
// The four charts do still leak on localhost for every child that goes away (pre-existing: nothing
// obsoletes them, so nothing frees them). A future sweep that fixes that leak is safe against this
// registry, and thanks to (3) it will actually be able to reclaim the charts of departed children:
// their references are already released.
//
// Identity + copied child labels are set when the set is resolved and whenever the child's label
// version changes.

struct pulse_child_charts {
    // last host-label version applied to the charts below; the traversal re-applies labels + hops
    // only when it changes, i.e. on reconnect / mid-stream label push
    uint32_t labels_applied_version;

    // mark-and-sweep stamp: the value of pulse_child_charts_pass when this entry was last seen
    uint64_t seen_pass;

    // ACQUIRED chart references, not raw RRDSET pointers: the reference is what keeps the chart
    // from being freed underneath the cache
    RRDSET_ACQUIRED *traffic;
    RRDSET_ACQUIRED *state;
    RRDSET_ACQUIRED *reconnects;
    RRDSET_ACQUIRED *age;
};

// pulse-thread-owned; never touched by any other thread
static DICTIONARY *pulse_child_charts_registry = NULL;
static uint64_t pulse_child_charts_pass = 0;

// Reached from dictionary_del() in pulse_child_charts_sweep() and from dictionary_destroy() in
// pulse_parents_cleanup() - both on the pulse thread, and nowhere else.
static void pulse_child_charts_delete_cb(
    const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct pulse_child_charts *c = value;

    rrdset_acquired_release(c->traffic);
    rrdset_acquired_release(c->state);
    rrdset_acquired_release(c->reconnects);
    rrdset_acquired_release(c->age);

    c->traffic = c->state = c->reconnects = c->age = NULL;
}

static struct pulse_child_charts *pulse_child_charts_entry(const char *machine_guid) {
    if(unlikely(!pulse_child_charts_registry)) {
        pulse_child_charts_registry =
            dictionary_create(DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE);
        dictionary_register_delete_callback(pulse_child_charts_registry, pulse_child_charts_delete_cb, NULL);
    }

    // a NULL value with a size makes the dictionary callocz() the entry on insert, and
    // DONT_OVERWRITE_VALUE makes this a get-or-create that never disturbs an existing entry
    return dictionary_set(pulse_child_charts_registry, machine_guid, NULL, sizeof(struct pulse_child_charts));
}

static void pulse_child_chart_labels(RRDSET *st, RRDHOST *host) {
    char node_id[UUID_STR_LEN] = "";
    if(!UUIDiszero(host->node_id))
        uuid_unparse_lower(host->node_id.uuid, node_id);

    char hops[16];
    snprintfz(hops, sizeof(hops), "%d", (int)rrdhost_ingestion_hops(host));

    // copy the child's labels FIRST, then set the authoritative identity labels so a colliding
    // child label cannot overwrite machine_guid/hostname/node_id/hops (rrdlabels_copy/add replace
    // the value for a shared key)
    rrdlabels_copy(st->rrdlabels, host->rrdlabels);
    rrdlabels_add(st->rrdlabels, "machine_guid", host->machine_guid, RRDLABEL_SRC_AUTO);
    rrdlabels_add(st->rrdlabels, "hostname", rrdhost_hostname(host), RRDLABEL_SRC_AUTO);
    if(node_id[0])
        rrdlabels_add(st->rrdlabels, "node_id", node_id, RRDLABEL_SRC_AUTO);
    rrdlabels_add(st->rrdlabels, "hops", hops, RRDLABEL_SRC_AUTO);

    // Chart labels are a prototype matching input, and these are written with the
    // rrdlabels_* primitives directly rather than through rrdset_update_rrdlabels(),
    // which is what normally raises these flags. The caller only invokes us when the
    // labels actually need re-applying, so tell health to re-evaluate this chart.
    rrdset_flag_set(st, RRDSET_FLAG_PENDING_LABEL_RECHECK);
    rrdhost_flag_set(st->rrdhost, RRDHOST_FLAG_PENDING_HEALTH_INITIALIZATION);
}

// The per-child charts are refreshed on every pulse step, for every streamed host. rrddim_add() is a
// dictionary write (conflict callback, destroy_lock trylock, collection reinitialise) and is only
// needed the first time each dimension appears, so look it up first and fall back to creating it.
static inline RRDDIM *pulse_child_dim(RRDSET *st, const char *id, collected_number multiplier, RRD_ALGORITHM algorithm) {
    // rrddim_find_active() only hides an obsolete dimension when its chart is ALSO undiscoverable
    // (rrdset_is_discoverable()), so it can hand back an obsolete dimension. Returning it unrevived
    // would defer revival to rrdset_done(), which logs "has the OBSOLETE flag set, but it is
    // collected" as an error. Fall through to rrddim_add(), whose pre-lookup revives it properly.
    // (This used to say these charts are never obsolete. They can be: anything may obsolete them,
    // see pulse_child_charts_update() - the chart is revived on the next pass, and the dimension
    // has to be handled here in the meantime.)
    RRDDIM *rd = rrddim_find_active(st, id);
    if(likely(rd && !rrddim_flag_check(rd, RRDDIM_FLAG_OBSOLETE)))
        return rd;

    return rrddim_add(st, id, NULL, multiplier, 1, algorithm);
}

// Return the cached chart, or NULL when it must be (re)resolved: either nothing is cached yet, or
// something obsoleted it and rrdset_create_localhost() must revive it under its destroy_lock. The
// acquired reference keeps the chart alive so this flag read is always safe.
static inline RRDSET *pulse_child_chart_cached(RRDSET_ACQUIRED **slot) {
    RRDSET *st = rrdset_acquired_to_rrdset(*slot);
    if(likely(st && !rrdset_flag_check(st, RRDSET_FLAG_OBSOLETE)))
        return st;

    if(*slot) {
        rrdset_acquired_release(*slot);
        *slot = NULL;
    }
    return NULL;
}

// Hold an acquired reference on a chart we just created or revived, so the obsolete-chart reaper
// cannot free it while it is cached.
static inline void pulse_child_chart_hold(RRDSET_ACQUIRED **slot, RRDSET *st) {
    *slot = rrdset_find_and_acquire(localhost, rrdset_id(st), true);
}

static void pulse_child_charts_update(RRDHOST *host, PULSE_INBOUND_STATE state) {
    char id[RRD_ID_LENGTH_MAX + 1];
    const char *guid = host->machine_guid;

    // get-or-create this child's pulse-owned entry and stamp it as seen in this pass, so the sweep
    // at the end of the traversal keeps it
    struct pulse_child_charts *c = pulse_child_charts_entry(guid);
    c->seen_pass = pulse_child_charts_pass;

    // re-apply labels + hops only when the host's labels changed (reconnect / mid-stream push), via a
    // cheap version compare - so we don't re-copy every child's labels on every pass. Resolving a
    // chart below also forces a refresh.
    //
    // ACCEPTED LIMITATION, to be closed by a follow-up that compares the effective label state
    // rather than a version number. Two ways stale labels survive here:
    //
    //   - Same host, changed identity. hostname comes from rrdhost_hostname() and hops from
    //     host->system_info (rrdhost_ingestion_hops()); neither is part of host->rrdlabels, and
    //     rrdlabels_migrate_to_these() ASSIGNS dst->version = src->version rather than bumping it.
    //     So a reconnect can change either while the version stays put. Pre-existing: verified on
    //     c290568811, before this cache existed, 4/4 reconnects kept a stale hostname.
    //
    //   - Replaced host, same machine_guid. This entry is keyed by guid and the sweep only drops it
    //     when a pass fails to collect that guid, so a host freed and re-created between two passes
    //     keeps this entry and its remembered version. Note this one is a REGRESSION against
    //     c290568811, where labels_applied_version lived on the RRDHOST: a replacement was
    //     callocz'd to 0, so any non-zero version forced a refresh. Moving the version into this
    //     registry lost that implicit reset.
    //
    // Do NOT plug the second case with an RRDHOST address as an identity token: allocator reuse
    // makes two different hosts compare equal, so it fails exactly when it is needed. Complete
    // invalidation has to account for the effective label state - copied labels including their
    // source flags, plus the authoritative hostname / node_id presence / hops this writer applies -
    // which then covers both cases and makes an incarnation id unnecessary.
    uint32_t lv = rrdlabels_version(host->rrdlabels);
    bool refresh_labels = (lv != c->labels_applied_version);
    c->labels_applied_version = lv;

    // Each chart (re)resolved below sets refresh_labels, so a freshly created or revived chart always
    // gets its labels. This replaces the former per-chart rrdlabels_exist(st->rrdlabels,
    // "machine_guid") probe, which ran on every pass for every chart. That probe is O(number of
    // labels): the labels JudyL is keyed by the interned RRDLABEL pointer
    // (src/database/rrdlabels.c), so rrdlabels_exist() walks the whole array, and it interns + frees
    // the key string on every call.
    //
    // This is at least as eager as the probe it replaces. Label removal does exist in general
    // (rrdlabels_flush(), rrdlabels_remove_all_unmarked(), rrdlabels_migrate_to_these()), but nothing
    // applies it to these four chart ids. Their only per-pass label writer was
    // rrdset_update_permanent_labels(), via the conflict callback of the rrdset_create_localhost()
    // call this cache removes; it re-added _collect_plugin / _collect_module every second and now
    // does so only at creation. Ours is pulse_child_chart_labels() below, which always (re)adds
    // machine_guid after copying the child's labels. So a cached chart cannot lose machine_guid, and
    // a set resolved from scratch always gets its labels here.
    //
    // Known consequence of that, judged acceptable: a child that configures a reserved key as a host
    // label (e.g. [host labels] _collect_plugin = foo) used to have it reverted within a second by
    // the permanent-labels rewrite and now keeps it on these four charts, because rrdlabels_copy()'s
    // same-key cleanup does not honour RRDLABEL_FLAG_DONT_DELETE. It needs a deliberately
    // reserved-prefixed host label to reach, and reporting what the child actually declared is
    // arguably the more honest outcome.
    // --- traffic ---
    RRDSET *st_traffic = pulse_child_chart_cached(&c->traffic);
    if(unlikely(!st_traffic)) {
        snprintfz(id, sizeof(id), "streaming.in.traffic.%s", guid);
        st_traffic = rrdset_create_localhost(
            "netdata", id, NULL, "Streaming", "netdata.streaming.in.traffic",
            "Inbound Streaming Traffic", "bytes/s", "netdata", "pulse",
            130160, localhost->rrd_update_every, RRDSET_TYPE_AREA);
        pulse_child_chart_hold(&c->traffic, st_traffic);
        refresh_labels = true;
    }
    if(unlikely(refresh_labels))
        pulse_child_chart_labels(st_traffic, host);
    rrddim_set_by_pointer(st_traffic, pulse_child_dim(st_traffic, "in", 1, RRD_ALGORITHM_INCREMENTAL),
        (collected_number)single_writer_atomic_read(&host->stream.rcv.status.bytes_in));
    rrddim_set_by_pointer(st_traffic, pulse_child_dim(st_traffic, "out", -1, RRD_ALGORITHM_INCREMENTAL),
        (collected_number)single_writer_atomic_read(&host->stream.rcv.status.bytes_out));
    rrdset_done(st_traffic);

    // --- state (one-hot) ---
    static const char *state_dim[PULSE_INBOUND_MAX] = {
        [PULSE_INBOUND_ARCHIVED]            = "archived",
        [PULSE_INBOUND_OFFLINE]             = "offline",
        [PULSE_INBOUND_WAITING]             = "waiting",
        [PULSE_INBOUND_REPLICATION_WAITING] = "waiting replication",
        [PULSE_INBOUND_REPLICATING]         = "replicating",
        [PULSE_INBOUND_RUNNING]             = "running",
    };
    RRDSET *st_state = pulse_child_chart_cached(&c->state);
    if(unlikely(!st_state)) {
        snprintfz(id, sizeof(id), "streaming.in.state.%s", guid);
        st_state = rrdset_create_localhost(
            "netdata", id, NULL, "Streaming", "netdata.streaming.in.state",
            "Inbound Streaming State", "state", "netdata", "pulse",
            130161, localhost->rrd_update_every, RRDSET_TYPE_LINE);
        pulse_child_chart_hold(&c->state, st_state);
        refresh_labels = true;
    }
    if(unlikely(refresh_labels))
        pulse_child_chart_labels(st_state, host);
    for(size_t i = 0; i < PULSE_INBOUND_MAX ; i++) {
        if(!state_dim[i]) continue;
        rrddim_set_by_pointer(st_state, pulse_child_dim(st_state, state_dim[i], 1, RRD_ALGORITHM_ABSOLUTE),
            (collected_number)(state == i ? 1 : 0));
    }
    rrdset_done(st_state);

    // --- reconnects ---
    RRDSET *st_reconnects = pulse_child_chart_cached(&c->reconnects);
    if(unlikely(!st_reconnects)) {
        snprintfz(id, sizeof(id), "streaming.in.reconnects.%s", guid);
        st_reconnects = rrdset_create_localhost(
            "netdata", id, NULL, "Streaming", "netdata.streaming.in.reconnects",
            "Inbound Streaming Reconnects", "connects/s", "netdata", "pulse",
            130162, localhost->rrd_update_every, RRDSET_TYPE_LINE);
        pulse_child_chart_hold(&c->reconnects, st_reconnects);
        refresh_labels = true;
    }
    if(unlikely(refresh_labels))
        pulse_child_chart_labels(st_reconnects, host);
    rrddim_set_by_pointer(st_reconnects, pulse_child_dim(st_reconnects, "connections", 1, RRD_ALGORITHM_INCREMENTAL),
        (collected_number)__atomic_load_n(&host->stream.rcv.status.connections, __ATOMIC_RELAXED));
    rrdset_done(st_reconnects);

    // --- age (seconds in the current inbound state; reset to 0 on every state change) ---
    RRDSET *st_age = pulse_child_chart_cached(&c->age);
    if(unlikely(!st_age)) {
        snprintfz(id, sizeof(id), "streaming.in.age.%s", guid);
        st_age = rrdset_create_localhost(
            "netdata", id, NULL, "Streaming", "netdata.streaming.in.age",
            "Inbound Streaming State Age", "seconds", "netdata", "pulse",
            130163, localhost->rrd_update_every, RRDSET_TYPE_LINE);
        pulse_child_chart_hold(&c->age, st_age);
        refresh_labels = true;
    }
    if(unlikely(refresh_labels))
        pulse_child_chart_labels(st_age, host);
    time_t changed = __atomic_load_n(&host->stream.rcv.status.state_changed_s, __ATOMIC_RELAXED);
    time_t now_s = now_realtime_sec();
    rrddim_set_by_pointer(st_age, pulse_child_dim(st_age, "age", 1, RRD_ALGORITHM_ABSOLUTE),
        (collected_number)((changed && now_s > changed) ? now_s - changed : 0));
    rrdset_done(st_age);
}

// Drop the entries of the children this pass did not produce, releasing their chart references.
// "Gone" is decided purely from what the traversal saw: we never read a departed child's RRDHOST,
// which may already have been freed. Runs on the pulse thread, like every other access here.
static void pulse_child_charts_sweep(void) {
    if(!pulse_child_charts_registry)
        return;

    struct pulse_child_charts *c;
    dfe_start_write(pulse_child_charts_registry, c) {
        if(c->seen_pass != pulse_child_charts_pass)
            dictionary_del(pulse_child_charts_registry, c_dfe.name);
    }
    dfe_done(c);

    dictionary_garbage_collect(pulse_child_charts_registry);
}

// traverse all hosts once: tally BOTH the inbound and outbound aggregates from each host's combined
// pulse_state, and refresh the per-child charts. A host may carry both an inbound (receiver) and an
// outbound (sender) state simultaneously, so both are tallied independently.
static void pulse_parents_traverse(ssize_t inbound[2][PULSE_INBOUND_MAX], ssize_t outbound[PULSE_OUTBOUND_MAX]) {
    // per-child charts only when streaming ingest is actually configured
    bool do_children = stream_conf_is_parent(false);

    pulse_child_charts_pass++;

    RRDHOST *host;
    dfe_start_reentrant(rrdhost_root_index, host) {
        PULSE_HOST_STATUS s = __atomic_load_n(&host->stream.pulse_state, __ATOMIC_RELAXED);
        if(!s)
            continue; // not classified yet

        PULSE_INBOUND_STATE in = pulse_inbound_state(s);
        if(in < PULSE_INBOUND_MAX) {
            size_t type = (s & PULSE_HOST_STATUS_EPHEMERAL) ? 1 : 0;
            inbound[type][in]++;

            if(do_children && !rrdhost_is_local(host))
                pulse_child_charts_update(host, in);
        }

        PULSE_OUTBOUND_STATE out = pulse_outbound_state(s);
        if(out < PULSE_OUTBOUND_MAX)
            outbound[out]++;
    }
    dfe_done(host);

    // children that disappeared during or before this pass are not stamped - reclaim them here.
    // This also reclaims everything when do_children is false, which is correct: nothing is cached.
    pulse_child_charts_sweep();
}

// Release everything this thread owns. Called by pulse_thread_main() once its loop has ended, so
// the acquired chart references are released by the thread that took them, after the last possible
// traversal.
void pulse_parents_cleanup(void) {
    if(!pulse_child_charts_registry)
        return;

    dictionary_destroy(pulse_child_charts_registry);
    pulse_child_charts_registry = NULL;
}

void pulse_parents_do(bool extended) {
    bool is_parent = netdata_conf_is_parent();
    bool is_child = stream_conf_is_child();

    // one read-only pass over the host dictionary: tally both streaming aggregates and refresh the
    // per-child charts (no global counters, no rrd_rdlock)
    ssize_t inbound[2][PULSE_INBOUND_MAX] = { 0 };
    ssize_t outbound[PULSE_OUTBOUND_MAX] = { 0 };
    if(is_parent || is_child)
        pulse_parents_traverse(inbound, outbound);

    if(is_parent) {
        for(size_t idx = 0; idx < _countof(p.parent.type) ; idx++) {
            if (unlikely(!p.parent.type[idx].st_nodes)) {
                const char *type;
                const char *id;
                if(idx == 0) {
                    type = "permanent";
                    id = "netdata.streaming_inbound_permanent";
                }
                else {
                    type = "ephemeral";
                    id = "netdata.streaming_inbound_ephemeral";
                }

                p.parent.type[idx].st_nodes = rrdset_create_localhost(
                    "netdata"
                    , id
                    , NULL
                    , "Streaming"
                    , "netdata.streaming_inbound"
                    , "Inbound Nodes"
                    , "nodes"
                    , "netdata"
                    , "pulse"
                    , 130150
                    , localhost->rrd_update_every
                    , RRDSET_TYPE_LINE
                );

                rrdlabels_add(p.parent.type[idx].st_nodes->rrdlabels, "type", type, RRDLABEL_SRC_AUTO);

                p.parent.type[idx].rd_local = rrddim_add(p.parent.type[idx].st_nodes, "local", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_virtual = rrddim_add(p.parent.type[idx].st_nodes, "virtual", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_loading = rrddim_add(p.parent.type[idx].st_nodes, "loading", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_archived = rrddim_add(p.parent.type[idx].st_nodes, "stale archived", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_offline = rrddim_add(p.parent.type[idx].st_nodes, "stale disconnected", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_waiting = rrddim_add(p.parent.type[idx].st_nodes, "waiting", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_replication_waiting = rrddim_add(p.parent.type[idx].st_nodes, "waiting replication", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_replicating = rrddim_add(p.parent.type[idx].st_nodes, "replicating", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                p.parent.type[idx].rd_running = rrddim_add(p.parent.type[idx].st_nodes, "running", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            }

            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_local, (collected_number)inbound[idx][PULSE_INBOUND_LOCAL]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_virtual, (collected_number)inbound[idx][PULSE_INBOUND_VIRTUAL]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_loading, (collected_number)inbound[idx][PULSE_INBOUND_LOADING]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_archived, (collected_number)inbound[idx][PULSE_INBOUND_ARCHIVED]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_offline, (collected_number)inbound[idx][PULSE_INBOUND_OFFLINE]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_waiting, (collected_number)inbound[idx][PULSE_INBOUND_WAITING]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_replication_waiting, (collected_number)inbound[idx][PULSE_INBOUND_REPLICATION_WAITING]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_replicating, (collected_number)inbound[idx][PULSE_INBOUND_REPLICATING]);
            rrddim_set_by_pointer(p.parent.type[idx].st_nodes, p.parent.type[idx].rd_running, (collected_number)inbound[idx][PULSE_INBOUND_RUNNING]);

            rrdset_done(p.parent.type[idx].st_nodes);
        }

        if(extended) {
            chart_by_reason(
                &p.parent.events_by_reason,
                "streaming_rejections_inbound",
                "netdata.streaming_events_inbound",
                "Inbound Streaming Events",
                "rejections",
                130151);
            chart_by_reason(
                &p.parent.disconnects_by_reason,
                "streaming_disconnects_inbound",
                "netdata.streaming_events_inbound",
                "Inbound Streaming Events",
                "disconnects",
                130151);
        }
    }

    if(is_child) {
        {
            static RRDSET *st_nodes = NULL;
            static RRDDIM *rd_pending = NULL;
            static RRDDIM *rd_connecting = NULL;
            static RRDDIM *rd_offline = NULL;
            static RRDDIM *rd_waiting = NULL;
            static RRDDIM *rd_replicating = NULL;
            static RRDDIM *rd_running = NULL;
            static RRDDIM *rd_no_dst = NULL;
            static RRDDIM *rd_no_dst_failed = NULL;

            if (unlikely(!st_nodes)) {
                st_nodes = rrdset_create_localhost(
                    "netdata"
                    , "streaming_outbound"
                    , NULL
                    , "Streaming"
                    , "netdata.streaming_outbound"
                    , "Outbound Nodes"
                    , "nodes"
                    , "netdata"
                    , "pulse"
                    , 130153
                    , localhost->rrd_update_every
                    , RRDSET_TYPE_LINE
                );

                rd_connecting = rrddim_add(st_nodes, "connecting", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_pending = rrddim_add(st_nodes, "pending", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_offline = rrddim_add(st_nodes, "offline", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_waiting = rrddim_add(st_nodes, "waiting", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_replicating = rrddim_add(st_nodes, "replicating", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_running = rrddim_add(st_nodes, "running", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_no_dst = rrddim_add(st_nodes, "no dst", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
                rd_no_dst_failed = rrddim_add(st_nodes, "failed", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            }

            rrddim_set_by_pointer(st_nodes, rd_connecting, (collected_number)outbound[PULSE_OUTBOUND_CONNECTING]);
            rrddim_set_by_pointer(st_nodes, rd_pending, (collected_number)outbound[PULSE_OUTBOUND_PENDING]);
            rrddim_set_by_pointer(st_nodes, rd_offline, (collected_number)outbound[PULSE_OUTBOUND_OFFLINE]);
            rrddim_set_by_pointer(st_nodes, rd_waiting, (collected_number)outbound[PULSE_OUTBOUND_WAITING]);
            rrddim_set_by_pointer(st_nodes, rd_replicating, (collected_number)outbound[PULSE_OUTBOUND_REPLICATING]);
            rrddim_set_by_pointer(st_nodes, rd_running, (collected_number)outbound[PULSE_OUTBOUND_RUNNING]);
            rrddim_set_by_pointer(st_nodes, rd_no_dst, (collected_number)outbound[PULSE_OUTBOUND_NO_DST]);
            rrddim_set_by_pointer(st_nodes, rd_no_dst_failed, (collected_number)outbound[PULSE_OUTBOUND_NO_DST_FAILED]);

            rrdset_done(st_nodes);
        }

        if(extended) {
            chart_by_reason(
                &p.sender.stream_info_failed_by_reason,
                "streaming_info_failed_outbound",
                "netdata.streaming_events_outbound",
                "Outbound Streaming Events",
                "stream-info",
                130154);
            chart_by_reason(
                &p.sender.events_by_reason,
                "streaming_rejections_outbound",
                "netdata.streaming_events_outbound",
                "Outbound Streaming Events",
                "rejections",
                130154);
            chart_by_reason(
                &p.sender.disconnects_by_reason,
                "streaming_disconnects_outbound",
                "netdata.streaming_events_outbound",
                "Outbound Streaming Events",
                "disconnects",
                130154);
        }
    }
}
