// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_DAEMON_NETDATA_CONF_DBENGINE_H
#define NETDATA_DAEMON_NETDATA_CONF_DBENGINE_H

#include "libnetdata/libnetdata.h"

extern bool dbengine_enabled;
extern bool dbengine_datafiles_present; // dbengine datafiles exist on disk, even if the agent is not currently running dbengine
extern bool dbengine_use_direct_io;

// ---------------------------------------------------------------------------
// tier-0 offline spill store
//
// When enabled, a host that streams to a parent keeps tier 0 in its configured
// (non-dbengine) db mode while the parent is reachable, and additionally writes
// tier-0-resolution samples into multidb_ctx[0] while it is orphaned, so the
// parent can replicate an outage longer than the in-memory ring can hold.
//
// This is deliberately NOT dbengine_enabled: rrd_init() sets that true for unit
// tests without initializing multidb_ctx, and prepare_host_for_unittest()
// builds a per-host dbengine instance that must never be mixed with
// multidb_ctx[0]. Only the real init path sets spill_enabled.
extern bool spill_enabled;

// The slot index of the offline spill store inside RRDDIM.tiers[],
// RRDHOST.db[], RRDSET.smg[] and QUERY_METRIC.tiers[]. Valid only when
// spill_enabled is true.
#define rrd_spill_slot() (nd_profile.storage_tiers)

// The number of per-dimension storage slots that exist: the real tiers, plus the
// spill slot when the feature is on. This is the correct bound for every loop
// over RRDDIM.tiers[] / RRDHOST.db[] / RRDSET.smg[] that should also touch the
// spill store. Loops that must stay tier-only keep nd_profile.storage_tiers.
//
// Hosts that do not have a spill store leave db[rrd_spill_slot()].eng NULL, so
// loops that already skip a NULL engine are self-gating.
#define rrd_storage_slots() (nd_profile.storage_tiers + (spill_enabled ? 1 : 0))

// True when a slot index refers to the offline spill store rather than a real
// tier. Use this to skip tier-aggregation logic, which the spill (tier-0
// resolution, tier_grouping 1) must not run.
#define rrddim_tier_is_spill(tier) (spill_enabled && (tier) == rrd_spill_slot())

// Default disk cap for the spill store. Much smaller than
// RRDENG_DEFAULT_TIER_DISK_SPACE_MB because the spill only ever holds outage
// windows, not continuous collection.
#define OFFLINE_RETENTION_DEFAULT_SIZE_MB 256

// Hysteresis for the spill activation predicate, evaluated per host by the
// service thread. See rrdhost_spill_evaluate().
extern time_t offline_retention_start_delay_s; // orphaned for this long -> start spilling
extern time_t offline_retention_stop_delay_s;  // reconnected for this long -> may stop
extern time_t offline_retention_stop_max_s;    // stop anyway, even if replication never drains

// True when [db].offline retention asks for the spill store. Read by rrd_init()
// to decide whether dbengine must be initialized on a non-dbengine agent.
bool spill_conf_requested(void);

extern int default_rrd_history_entries;
extern int gap_when_lost_iterations_above;
extern time_t rrdset_free_obsolete_time_s;

size_t get_tier_grouping(size_t tier);

void netdata_conf_section_db(void);
// enable_spill: bring multidb_ctx[0] up as the tier-0 offline spill store and
// pin the agent to a single dbengine tier. The caller decides this, because it
// depends on streaming configuration that is not visible here. Passing true
// when db mode is dbengine is a no-op.
void netdata_conf_dbengine_init(const char *hostname, bool enable_spill);

#include "netdata-conf.h"

#endif //NETDATA_DAEMON_NETDATA_CONF_DBENGINE_H
