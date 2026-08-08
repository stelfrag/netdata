// SPDX-License-Identifier: GPL-3.0-or-later

#include "rrddim-collection.h"

ALWAYS_INLINE void store_metric_collection_completed() {
    pulse_queries_rrdset_collection_completed(rrdset_done_statistics_points_stored_per_tier);
}

static inline time_t tier_next_point_time_s(RRDDIM *rd, struct rrddim_tier *t, time_t now_s) {
    time_t loop = (time_t)rd->rrdset->update_every * (time_t)t->tier_grouping;
    return now_s + loop - ((now_s + loop) % loop);
}

#define LAST_COMPLETED_POINT_EXISTS(t) (t->last_completed_point.end_time_s != 0)

ALWAYS_INLINE_HOT
void store_metric_at_tier_flush_last_completed(RRDDIM *rd __maybe_unused, size_t tier, struct rrddim_tier *t) {
    // when there is no end_time_s we do not have a saved last_completed_point
    if(!LAST_COMPLETED_POINT_EXISTS(t)) return;

    STORAGE_POINT *sp = &t->last_completed_point;
    if(likely(!storage_point_is_unset(t->last_completed_point))) {
        storage_engine_store_metric(
            t->sch,
            sp->end_time_s * USEC_PER_SEC,
            sp->sum,
            sp->min,
            sp->max,
            sp->count,
            sp->anomaly_count,
            sp->flags);
    }
    else {
        storage_engine_store_metric(
            t->sch,
            sp->end_time_s * USEC_PER_SEC,
            NAN,
            NAN,
            NAN,
            0,
            0, SN_FLAG_NONE);
    }

    rrdset_done_statistics_points_stored_per_tier[tier]++;

    // make the point unset
    t->last_completed_point.count = 0;      // make it unset
    t->last_completed_point.end_time_s = 0; // make it not saved
}

ALWAYS_INLINE_HOT
static void store_metric_at_tier_save_last_completed(RRDDIM *rd, size_t tier, struct rrddim_tier *t, STORAGE_POINT sp) {
    // make sure the last_completed_point is empty
    store_metric_at_tier_flush_last_completed(rd, tier, t);

    // copy the point
    t->last_completed_point = sp;

    // set the end_time_s, so that we will know we have saved a last_completed_point
    t->last_completed_point.end_time_s = t->next_point_end_time_s;
}

ALWAYS_INLINE_HOT
void store_metric_at_tier(RRDDIM *rd, size_t tier, struct rrddim_tier *t, STORAGE_POINT sp, usec_t now_ut __maybe_unused) {
    if(LAST_COMPLETED_POINT_EXISTS(t) && sp.start_time_s % t->last_completed_point_flush_modulo == 0)
        store_metric_at_tier_flush_last_completed(rd, tier, t);

    if (unlikely(!t->next_point_end_time_s))
        t->next_point_end_time_s = tier_next_point_time_s(rd, t, sp.end_time_s);

    if(unlikely(sp.start_time_s >= t->next_point_end_time_s)) {
        // flush the virtual point, it is done

        if (likely(!storage_point_is_unset(t->virtual_point)))
            store_metric_at_tier_save_last_completed(rd, tier, t, t->virtual_point);
        else
            store_metric_at_tier_save_last_completed(rd, tier, t, STORAGE_POINT_UNSET);

        t->virtual_point.count = 0; // make the point unset
        t->next_point_end_time_s = tier_next_point_time_s(rd, t, sp.end_time_s);
    }

    // merge the dates into our virtual point
    if (unlikely(sp.start_time_s < t->virtual_point.start_time_s))
        t->virtual_point.start_time_s = sp.start_time_s;

    if (likely(sp.end_time_s > t->virtual_point.end_time_s))
        t->virtual_point.end_time_s = sp.end_time_s;

    // merge the values into our virtual point
    if (likely(!storage_point_is_gap(sp))) {
        // we aggregate only non NULLs into higher tiers

        if (likely(!storage_point_is_unset(t->virtual_point))) {
            // merge the collected point to our virtual one
            t->virtual_point.sum += sp.sum;
            t->virtual_point.min = MIN(t->virtual_point.min, sp.min);
            t->virtual_point.max = MAX(t->virtual_point.max, sp.max);
            t->virtual_point.count += sp.count;
            t->virtual_point.anomaly_count += sp.anomaly_count;
            t->virtual_point.flags |= sp.flags;
        }
        else {
            // reset our virtual point to this one
            t->virtual_point = sp;
        }
    }
}

// ----------------------------------------------------------------------------
// tier-0 offline spill store

// Copy whatever the in-memory tier-0 ring still holds into a freshly opened
// spill handle. This closes two seams at once:
//   - the activation grace period ('offline retention start delay'), during
//     which the parent is already gone but we were not spilling yet;
//   - the gap between a previous spill session and this one.
// Without it the parent's backfill would stop dead at the moment spilling
// started, which is exactly the data an outage makes most valuable.
//
// The lower bound is mandatory: rrdeng_store_metric_init() seeds the handle's
// page_end_time_ut from the metric's MRG last_time_s, so writing before that
// trips dbengine's "collection went back in time" handling.
static void rrddim_spill_backfill_from_ring(RRDDIM *rd, struct rrddim_tier *t, time_t now_s) {
    time_t ring_first_s = storage_engine_oldest_time_s(rd->tiers[0].seb, rd->tiers[0].smh);
    if(!ring_first_s)
        return;

    time_t spill_last_s = storage_engine_latest_time_s(t->seb, t->smh);
    time_t after_s = ring_first_s;

    if(spill_last_s) {
        time_t resume_s = spill_last_s + rd->rrdset->update_every;
        if(resume_s > after_s)
            after_s = resume_s;
    }

    // Stop one step short of the sample being collected right now: the caller
    // stores that one through the normal path immediately after this returns,
    // and writing the same timestamp twice would make dbengine see collection
    // going backwards.
    time_t before_s = now_s - rd->rrdset->update_every;

    if(after_s > before_s)
        return;

    stream_control_backfill_query_started();

    struct storage_engine_query_handle seqh;
    storage_engine_query_init(rd->tiers[0].seb, rd->tiers[0].smh, &seqh,
                              after_s, before_s, STORAGE_PRIORITY_SYNCHRONOUS_FIRST);

    size_t points_read = 0;
    while(!storage_engine_query_is_finished(&seqh)) {
        STORAGE_POINT sp = storage_engine_query_next_metric(&seqh);
        points_read++;

        if(storage_point_is_unset(sp) || storage_point_is_gap(sp))
            continue;

        // tier-0 resolution single points, stored exactly the way the live
        // collect path stores them: the value, count 1, and the anomaly bit
        // carried in flags rather than in anomaly_count.
        storage_engine_store_metric(
            t->sch, (usec_t)sp.end_time_s * USEC_PER_SEC,
            sp.sum, 0, 0, 1, 0, sp.flags);
    }

    storage_engine_query_finalize(&seqh);
    store_metric_collection_completed();
    pulse_queries_backfill_query_completed(points_read);

    stream_control_backfill_query_finished();
}

// Bring this dimension's spill collect handle in line with 'active'.
//
// Collector thread only. The spill handle's lifetime belongs solely to this
// function - rrddim_reinitialize_collection() deliberately does not recreate it.
// The tier spinlock is taken because the service thread can finalize the same
// handle concurrently from rrddim_finalize_collection_and_check_retention()
// while archiving an obsolete dimension.
NOT_INLINE_HOT void rrddim_spill_toggle(RRDDIM *rd, bool active, time_t now_s) {
    RRDSET *st = rd->rrdset;
    RRDHOST *host = st->rrdhost;
    size_t spill = rrd_spill_slot();
    struct rrddim_tier *t = &rd->tiers[spill];

    spinlock_lock(&t->spinlock);

    if(active && !t->sch) {
        // Never resurrect a handle on a dimension the service thread has already
        // finished with - it would leak the handle and its unflushed hot page,
        // and leak ctx->atomic.collectors_running. This check has no tier-0
        // analogue, because tier 0's handle only ever goes non-NULL -> NULL.
        if(rrdset_flag_check(st, RRDSET_FLAG_COLLECTION_FINISHED) ||
           rrddim_flag_check(rd, RRDDIM_FLAG_OBSOLETE))
            goto done;

        if(!t->smh) {
            STORAGE_ENGINE *eng = host->db[spill].eng;
            if(!eng) goto done;
            t->smh = eng->api.metric_get_or_create(rd, host->db[spill].si);
            if(!t->smh) goto done;
        }

        t->sch = storage_metric_store_init(t->seb, t->smh, st->update_every, st->smg[spill]);
        if(!t->sch) goto done;

        rrddim_spill_backfill_from_ring(rd, t, now_s);
    }
    else if(!active && t->sch) {
        // Flush the hot page and release dbengine's own metric reference.
        // t->smh is OURS and is deliberately kept: the spilled data stays
        // queryable and replicable until dbengine retention expires.
        storage_engine_store_finalize(t->sch);
        t->sch = NULL;
    }

done:
    spinlock_unlock(&t->spinlock);
}

NOT_INLINE_HOT
#ifdef NETDATA_LOG_COLLECTION_ERRORS
void rrddim_store_metric_with_trace(RRDDIM *rd, usec_t point_end_time_ut, NETDATA_DOUBLE n, SN_FLAGS flags, const char *function) {
#else // !NETDATA_LOG_COLLECTION_ERRORS
void rrddim_store_metric(RRDDIM *rd, usec_t point_end_time_ut, NETDATA_DOUBLE n, SN_FLAGS flags) {
#endif // !NETDATA_LOG_COLLECTION_ERRORS

    static __thread struct log_stack_entry lgs[] = {
        [0] = ND_LOG_FIELD_STR(NDF_NIDL_DIMENSION, NULL),
        [1] = ND_LOG_FIELD_END(),
    };
    lgs[0].str = rd->id;
    log_stack_push(lgs);

#ifdef NETDATA_LOG_COLLECTION_ERRORS
    rd->rrddim_store_metric_count++;

    if(likely(rd->rrddim_store_metric_count > 1)) {
        usec_t expected = rd->rrddim_store_metric_last_ut + rd->rrdset->update_every * USEC_PER_SEC;

        if(point_end_time_ut != rd->rrddim_store_metric_last_ut) {
            internal_error(true,
                           "%s COLLECTION: 'host:%s/chart:%s/dim:%s' granularity %d, collection %zu, expected to store at tier 0 a value at %llu, but it gave %llu [%s%llu usec] (called from %s(), previously by %s())",
                           (point_end_time_ut < rd->rrddim_store_metric_last_ut) ? "**PAST**" : "GAP",
                           rrdhost_hostname(rd->rrdset->rrdhost), rrdset_id(rd->rrdset), rrddim_id(rd),
                           rd->rrdset->update_every,
                           rd->rrddim_store_metric_count,
                           expected, point_end_time_ut,
                           (point_end_time_ut < rd->rrddim_store_metric_last_ut)?"by -" : "gap ",
                           expected - point_end_time_ut,
                           function,
                           rd->rrddim_store_metric_last_caller?rd->rrddim_store_metric_last_caller:"none");
        }
    }

    rd->rrddim_store_metric_last_ut = point_end_time_ut;
    rd->rrddim_store_metric_last_caller = function;
#endif // NETDATA_LOG_COLLECTION_ERRORS

    // store the metric on tier 0
    storage_engine_store_metric(rd->tiers[0].sch, point_end_time_ut,
                                n, 0, 0,
                                1, 0, flags);

    rrdset_done_statistics_points_stored_per_tier[0]++;

    // mirror the sample into the tier-0 offline spill store while orphaned.
    // rd->rrdset->spill_active is latched once per rrdset_timed_done(), so every
    // dimension of a chart agrees for the whole iteration - a mid-chart flip
    // would give the dimensions ragged retention.
    if(unlikely(spill_enabled)) {
        size_t spill = rrd_spill_slot();

        if(unlikely(rd->rrdset->spill_active != (rd->tiers[spill].sch != NULL)))
            rrddim_spill_toggle(rd, rd->rrdset->spill_active,
                                (time_t)(point_end_time_ut / USEC_PER_SEC));

        if(rd->tiers[spill].sch) {
            storage_engine_store_metric(rd->tiers[spill].sch, point_end_time_ut,
                                        n, 0, 0,
                                        1, 0, flags);

            rrdset_done_statistics_points_stored_per_tier[spill]++;
        }
    }

    time_t now_s = (time_t)(point_end_time_ut / USEC_PER_SEC);

    STORAGE_POINT sp = {
        .start_time_s = now_s - rd->rrdset->update_every,
        .end_time_s = now_s,
        .min = n,
        .max = n,
        .sum = n,
        .count = 1,
        .anomaly_count = (flags & SN_FLAG_NOT_ANOMALOUS) ? 0 : 1,
        .flags = flags
    };

    for(size_t tier = 1; tier < nd_profile.storage_tiers;tier++) {
        if(unlikely(!rd->tiers[tier].smh)) continue;

        struct rrddim_tier *t = &rd->tiers[tier];

        if(!rrddim_option_check(rd, RRDDIM_OPTION_BACKFILLED_HIGH_TIERS)) {
            // we have not collected this tier before
            // let's fill any gap that may exist
            backfill_tier_from_smaller_tiers(rd, tier, now_s);
        }

        store_metric_at_tier(rd, tier, t, sp, point_end_time_ut);
    }
    rrddim_option_set(rd, RRDDIM_OPTION_BACKFILLED_HIGH_TIERS);

    rrdcontext_collected_rrddim(rd);
    log_stack_pop(&lgs);
}
