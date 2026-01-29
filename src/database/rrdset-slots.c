// SPDX-License-Identifier: GPL-3.0-or-later

#include "rrdset-slots.h"

void rrdset_stream_send_chart_slot_assign(RRDSET *st) {
    RRDHOST *host = st->rrdhost;
    spinlock_lock(&host->stream.snd.pluginsd_chart_slots.available.spinlock);

    if(host->stream.snd.pluginsd_chart_slots.available.used > 0)
        st->stream.snd.chart_slot =
            host->stream.snd.pluginsd_chart_slots.available.array[--host->stream.snd.pluginsd_chart_slots.available.used];
    else
        st->stream.snd.chart_slot = ++host->stream.snd.pluginsd_chart_slots.last_used;

    spinlock_unlock(&host->stream.snd.pluginsd_chart_slots.available.spinlock);
}

void rrdset_stream_send_chart_slot_release(RRDSET *st) {
    if(!st->stream.snd.chart_slot || st->rrdhost->stream.snd.pluginsd_chart_slots.available.ignore)
        return;

    RRDHOST *host = st->rrdhost;
    spinlock_lock(&host->stream.snd.pluginsd_chart_slots.available.spinlock);

    if(host->stream.snd.pluginsd_chart_slots.available.used >= host->stream.snd.pluginsd_chart_slots.available.size) {
        uint32_t old_slots = host->stream.snd.pluginsd_chart_slots.available.size;
        uint32_t new_slots = (old_slots > 0) ? (old_slots * 2) : 1024;

        host->stream.snd.pluginsd_chart_slots.available.array =
            reallocz(host->stream.snd.pluginsd_chart_slots.available.array, new_slots * sizeof(uint32_t));

        host->stream.snd.pluginsd_chart_slots.available.size = new_slots;

        rrd_slot_memory_added((new_slots - old_slots) * sizeof(uint32_t));
    }

    host->stream.snd.pluginsd_chart_slots.available.array[host->stream.snd.pluginsd_chart_slots.available.used++] =
        st->stream.snd.chart_slot;

    st->stream.snd.chart_slot = 0;
    spinlock_unlock(&host->stream.snd.pluginsd_chart_slots.available.spinlock);
}

void rrdset_pluginsd_receive_unslot(RRDSET *st) {
    // Use atomic loads with ACQUIRE semantics to synchronize with cleanup code
    // that uses RELEASE semantics when freeing the array
    struct pluginsd_rrddim *prd_array = __atomic_load_n(&st->pluginsd.prd_array, __ATOMIC_ACQUIRE);
    size_t prd_size = __atomic_load_n(&st->pluginsd.size, __ATOMIC_ACQUIRE);

    for(size_t i = 0; i < prd_size && prd_array; i++) {
        rrddim_acquired_release(prd_array[i].rda); // can be NULL
        prd_array[i].rda = NULL;
        prd_array[i].rd = NULL;
        prd_array[i].id = NULL;
    }

    RRDHOST *host = st->rrdhost;

    if(st->pluginsd.last_slot >= 0 &&
        (uint32_t)st->pluginsd.last_slot < host->stream.rcv.pluginsd_chart_slots.size &&
        host->stream.rcv.pluginsd_chart_slots.array[st->pluginsd.last_slot] == st) {
        host->stream.rcv.pluginsd_chart_slots.array[st->pluginsd.last_slot] = NULL;
    }

    st->pluginsd.last_slot = -1;
    st->pluginsd.dims_with_slots = false;
}

void rrdset_pluginsd_receive_unslot_and_cleanup(RRDSET *st) {
    if(!st)
        return;

    spinlock_lock(&st->pluginsd.spinlock);

    // Check if collector is still active - if so, we cannot safely cleanup
    // The collector will clear collector_tid after it's done accessing the array
    pid_t collector_tid = __atomic_load_n(&st->pluginsd.collector_tid, __ATOMIC_ACQUIRE);
    if(collector_tid != 0) {
        // Collector is still active, cannot cleanup now
        // This shouldn't happen during normal operation - log a warning
        nd_log_limit_static_global_var(erl, 1, 0);
        nd_log_limit(&erl, NDLS_DAEMON, NDLP_WARNING,
                     "PLUGINSD: attempted cleanup while collector (tid %d) is still active on chart, skipping",
                     collector_tid);
        spinlock_unlock(&st->pluginsd.spinlock);
        return;
    }

    rrdset_pluginsd_receive_unslot(st);

    // Save old values for freeing after we NULL the pointers
    struct pluginsd_rrddim *old_prd_array = __atomic_load_n(&st->pluginsd.prd_array, __ATOMIC_RELAXED);
    size_t old_size = __atomic_load_n(&st->pluginsd.size, __ATOMIC_RELAXED);

    // Use atomic stores with RELEASE semantics to ensure readers with ACQUIRE
    // see consistent state - they will either see the old valid state or NULL
    __atomic_store_n(&st->pluginsd.prd_array, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&st->pluginsd.size, 0, __ATOMIC_RELEASE);

    __atomic_store_n(&st->pluginsd.pos, 0, __ATOMIC_RELAXED);
    st->pluginsd.set = false;
    st->pluginsd.last_slot = -1;
    st->pluginsd.dims_with_slots = false;

    spinlock_unlock(&st->pluginsd.spinlock);

    // Free AFTER releasing lock and NULLing the pointer
    rrd_slot_memory_removed(old_size * sizeof(struct pluginsd_rrddim));
    freez(old_prd_array);
}

void rrdset_pluginsd_receive_slots_initialize(RRDSET *st) {
    spinlock_init(&st->pluginsd.spinlock);
    st->pluginsd.last_slot = -1;
}
