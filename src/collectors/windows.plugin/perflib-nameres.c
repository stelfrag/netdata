// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_plugin.h"
#include "windows-internals.h"

static bool do_name_resolution(PERF_DATA_BLOCK *pDataBlock, int update_every)
{
    static RRDSET *st = NULL;
    static RRDDIM *rd_resolve = NULL;
    static RRDDIM *rd_cache = NULL;

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Peer Name Resolution Protocol");
    if(!pObjectType) return false;

    static COUNTER_DATA resolve = {.key = "Resolve"};
    static COUNTER_DATA cache = {.key = "Cache Entry"};

    if (!st) {
        st = rrdset_create_localhost(
            "network",
            "resolution",
            NULL,
            "network",
            NULL,
            "Name Resolution Statistics",
            "entries",
            PLUGIN_WINDOWS_NAME,
            "PerflibNetwork",
            NETDATA_CHART_PRIO_IPV4_PACKETS,
            update_every,
            RRDSET_TYPE_LINE);

        rd_resolve = rrddim_add(st, "resolve", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        rd_cache = rrddim_add(st, "cache", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    perflibGetObjectCounter(pDataBlock, pObjectType, &cache);
    perflibGetObjectCounter(pDataBlock, pObjectType, &resolve);

    rrddim_set_by_pointer(st, rd_cache, (collected_number) cache.current.Data);
    rrddim_set_by_pointer(st, rd_resolve, resolve.current.Data);

    rrdset_done(st);

    return true;
}

int do_Perflib_nameres(int update_every, usec_t dt __maybe_unused)
{

    DWORD id = RegistryFindIDByName("Peer Name Resolution Protocol");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_name_resolution(pDataBlock, update_every);

    return 0;
}
