// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_plugin.h"
#include "windows-internals.h"

struct processor_performance {
    bool collected_metadata;

    RRDSET *st;
    RRDDIM *rd_performance;
    RRDDIM *rd_utility;
    RRDDIM *rd_limit;

    COUNTER_DATA percentPerformance;
    COUNTER_DATA percentPerformanceUtility;
    COUNTER_DATA percentPerformanceLimit;
};

struct processor_performance total = { 0 };

void initialize_processor_performance_keys(struct processor_performance *p) {
    p->percentPerformance.key = "% Processor Performance";
    p->percentPerformanceUtility.key = "% Performance Utility";
    p->percentPerformanceLimit.key = "% Performance Limit";
}

void dict_processor_performance_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct processor_performance *p = value;
    initialize_processor_performance_keys(p);
}

static DICTIONARY *processors = NULL;

static void initialize(void) {
    initialize_processor_performance_keys(&total);

    processors = dictionary_create_advanced(
        DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct processor_performance));

    dictionary_register_insert_callback(processors, dict_processor_performance_insert_cb, NULL);
}

static bool do_processors_performance(PERF_DATA_BLOCK *pDataBlock, int update_every) {
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Processor Information");
    if(!pObjectType) return false;

    static const RRDVAR_ACQUIRED *cpus_var = NULL;
    int cores_found = 0;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for(LONG i = 0; i < pObjectType->NumInstances ; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if(!pi) break;

        if(!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        bool is_total = false;
        struct processor_performance *p;
        int cpu = -1;
        if(strcasecmp(windows_shared_buffer, "_Total") == 0) {
            p = &total;
            is_total = true;
            cpu = -1;
        }
        else {
            p = dictionary_set(processors, windows_shared_buffer, NULL, sizeof(*p));
            is_total = false;
            cpu = str2i(windows_shared_buffer);
            snprintfz(windows_shared_buffer, sizeof(windows_shared_buffer), "cpu%d", cpu);

            if(cpu + 1 > cores_found)
                cores_found = cpu + 1;
        }

        if(!is_total && !p->collected_metadata) {
            // TODO collect processor metadata
            p->collected_metadata = true;
        }

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->percentPerformance);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->percentPerformanceUtility);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->percentPerformanceLimit);

        if(!p->st) {
            p->st = rrdset_create_localhost(
                is_total ? "system" : "cpu"
                , is_total ? "cpu" : windows_shared_buffer, NULL
                , is_total ? "cpu" : "utilization"
                , is_total ? "system.cpu" : "cpu.cpu"
                , is_total ? "Total CPU Utilization" : "Core Utilization"
                , "percentage"
                , PLUGIN_WINDOWS_NAME
                , "PerflibProcessorPerformance"
                , is_total ? NETDATA_CHART_PRIO_SYSTEM_CPU : NETDATA_CHART_PRIO_CPU_PER_CORE
                , update_every
                , RRDSET_TYPE_STACKED
            );

            p->rd_performance = rrddim_add(p->st, "performance", NULL, 1, 1, RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL);
            p->rd_limit       = rrddim_add(p->st, "limit",       NULL, 1, 1, RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL);
            p->rd_utility     = rrddim_add(p->st, "utilization", NULL, 1, 1, RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL);
        }

        uint64_t limit = p->percentPerformanceLimit.current.Data;
        uint64_t performance = p->percentPerformance.current.Data;
        uint64_t utilization = p->percentPerformanceUtility.current.Data;

        rrddim_set_by_pointer(p->st, p->rd_limit, (collected_number) limit);
        rrddim_set_by_pointer(p->st, p->rd_performance,  (collected_number) performance);
        rrddim_set_by_pointer(p->st, p->rd_utility, (collected_number) utilization);
        
        rrdset_done(p->st);
    }

    if(cpus_var)
        rrdvar_host_variable_set(localhost, cpus_var, cores_found);

    return true;
}

int do_PerflibProcessorPerformance(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        initialize();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("Processor Information");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_processors_performance(pDataBlock, update_every);

    return 0;
}
