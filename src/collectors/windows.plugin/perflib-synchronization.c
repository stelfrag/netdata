#include "windows_plugin.h"
#include "windows-internals.h"

struct synchronization_performance {
    bool collected_metadata;

    RRDSET *st;
    RRDDIM *rd_spinlock_acquires;
    RRDDIM *rd_spinlock_contentions;
    RRDDIM *rd_spinlock_spins;

    COUNTER_DATA spinlockAcquires;
    COUNTER_DATA spinlockContentions;
    COUNTER_DATA spinlockSpins;
};

static struct synchronization_performance sync_total = { 0 };
static DICTIONARY *sync_processors = NULL;

void initialize_synchronization_performance_keys(struct synchronization_performance *p) {
    p->spinlockAcquires.key = "Spinlock Acquires/sec";
    p->spinlockContentions.key = "Spinlock Contentions/sec";
    p->spinlockSpins.key = "Spinlock Spins/sec";
}

void dict_synchronization_performance_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct synchronization_performance *p = value;
    initialize_synchronization_performance_keys(p);
}

static void sync_initialize(void) {
    initialize_synchronization_performance_keys(&sync_total);

    sync_processors = dictionary_create_advanced(
        DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct synchronization_performance));

    dictionary_register_insert_callback(sync_processors, dict_synchronization_performance_insert_cb, NULL);
}

static bool do_synchronization_performance(PERF_DATA_BLOCK *pDataBlock, int update_every) {
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Synchronization");
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
        struct synchronization_performance *p;
        int cpu = -1;
        if(strcasecmp(windows_shared_buffer, "_Total") == 0) {
            p = &sync_total;
            is_total = true;
            cpu = -1;
        }
        else {
            p = dictionary_set(sync_processors, windows_shared_buffer, NULL, sizeof(*p));
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

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->spinlockAcquires);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->spinlockContentions);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->spinlockSpins);

        if(!p->st) {
            p->st = rrdset_create_localhost(
                is_total ? "system" : "cpu_sync"
                , is_total ? "cpu_sync" : windows_shared_buffer, NULL
                , is_total ? "cpu_sync" : "synchronization"
                , is_total ? "system.cpu_sync" : "cpu_sync.cpu_sync"
                , is_total ? "Total Synchronization Performance" : "Core Synchronization Performance"
                , "per second"
                , PLUGIN_WINDOWS_NAME
                , "PerflibSynchronizationPerformance"
                , is_total ? NETDATA_CHART_PRIO_SYSTEM_CPU_KERNEL_SYNC : NETDATA_CHART_PRIO_CPU_PER_CORE
                , update_every
                , RRDSET_TYPE_LINE
            );

            p->rd_spinlock_acquires = rrddim_add(p->st, "spinlock_acquires", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            p->rd_spinlock_contentions = rrddim_add(p->st, "spinlock_contentions", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            p->rd_spinlock_spins = rrddim_add(p->st, "spinlock_spins", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        uint64_t spinlock_acquires = p->spinlockAcquires.current.Data;
        uint64_t spinlock_contentions = p->spinlockContentions.current.Data;
        uint64_t spinlock_spins = p->spinlockSpins.current.Data;

        rrddim_set_by_pointer(p->st, p->rd_spinlock_acquires, (collected_number) spinlock_acquires);
        rrddim_set_by_pointer(p->st, p->rd_spinlock_contentions, (collected_number) spinlock_contentions);
        rrddim_set_by_pointer(p->st, p->rd_spinlock_spins, (collected_number) spinlock_spins);

        rrdset_done(p->st);
    }

    if(cpus_var)
        rrdvar_host_variable_set(localhost, cpus_var, cores_found);

    return true;
}

int do_PerflibSynchronizationPerformance(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        sync_initialize();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("Synchronization");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_synchronization_performance(pDataBlock, update_every);

    return 0;
}
