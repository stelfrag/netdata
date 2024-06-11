#include "windows_plugin.h"
#include "windows-internals.h"

struct thermal_zone_performance {
    bool collected_metadata;

    RRDSET *st_temperature;
    RRDDIM *rd_temperature;

    RRDSET *st_passive_limit;
    RRDDIM *rd_passive_limit;

    RRDSET *st_throttle_reasons;
    RRDDIM *rd_throttle_reasons;

    RRDSET *st_high_precision_temperature;
    RRDDIM *rd_high_precision_temperature;

    COUNTER_DATA temperature;
    COUNTER_DATA passiveLimit;
    COUNTER_DATA throttleReasons;
    COUNTER_DATA highPrecisionTemperature;
};

static struct thermal_zone_performance thermal_zone_info = { 0 };

void initialize_thermal_zone_performance_keys(struct thermal_zone_performance *p) {
    p->temperature.key = "Temperature";
    p->passiveLimit.key = "% Passive Limit";
    p->throttleReasons.key = "Throttle Reasons";
    p->highPrecisionTemperature.key = "High Precision Temperature";
}

static void thermal_zone_initialize(void) {
    initialize_thermal_zone_performance_keys(&thermal_zone_info);
}

static bool do_thermal_zone_performance(PERF_DATA_BLOCK *pDataBlock, int update_every) {
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Thermal Zone Information");
    if(!pObjectType) return false;

    struct thermal_zone_performance *p = &thermal_zone_info;

    if(!p->collected_metadata) {
        // TODO collect thermal zone metadata
        p->collected_metadata = true;
    }

    perflibGetObjectCounter(pDataBlock, pObjectType, &p->temperature);
    perflibGetObjectCounter(pDataBlock, pObjectType, &p->passiveLimit);
    perflibGetObjectCounter(pDataBlock, pObjectType, &p->throttleReasons);
    perflibGetObjectCounter(pDataBlock, pObjectType, &p->highPrecisionTemperature);

    if(!p->st_temperature) {
        p->st_temperature = rrdset_create_localhost(
            "system"
            , "thermal_zone_temperature", NULL
            , "thermal_zone"
            , "system.thermal_zone_temperature"
            , "Thermal Zone Temperature"
            , "Kelvin"
            , PLUGIN_WINDOWS_NAME
            , "PerflibThermalZoneInformation"
            , NETDATA_CHART_PRIO_CPU_TEMPERATURE
            , update_every
            , RRDSET_TYPE_LINE
        );

        p->rd_temperature = rrddim_add(p->st_temperature, "temperature", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    if(!p->st_passive_limit) {
        p->st_passive_limit = rrdset_create_localhost(
            "system"
            , "thermal_zone_passive_limit", NULL
            , "thermal_zone"
            , "system.thermal_zone_passive_limit"
            , "Thermal Zone Passive Limit"
            , "%"
            , PLUGIN_WINDOWS_NAME
            , "PerflibThermalZoneInformation"
            , NETDATA_CHART_PRIO_CPU_TEMPERATURE
            , update_every
            , RRDSET_TYPE_LINE
        );

        p->rd_passive_limit = rrddim_add(p->st_passive_limit, "passive_limit", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    if(!p->st_throttle_reasons) {
        p->st_throttle_reasons = rrdset_create_localhost(
            "system"
            , "thermal_zone_throttle_reasons", NULL
            , "thermal_zone"
            , "system.thermal_zone_throttle_reasons"
            , "Thermal Zone Throttle Reasons"
            , "count"
            , PLUGIN_WINDOWS_NAME
            , "PerflibThermalZoneInformation"
            , NETDATA_CHART_PRIO_CPU_TEMPERATURE
            , update_every
            , RRDSET_TYPE_LINE
        );

        p->rd_throttle_reasons = rrddim_add(p->st_throttle_reasons, "throttle_reasons", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    if(!p->st_high_precision_temperature) {
        p->st_high_precision_temperature = rrdset_create_localhost(
            "system"
            , "thermal_zone_high_precision_temperature", NULL
            , "thermal_zone"
            , "system.thermal_zone_high_precision_temperature"
            , "Thermal Zone High Precision Temperature"
            , "tenths of Kelvin"
            , PLUGIN_WINDOWS_NAME
            , "PerflibThermalZoneInformation"
            , NETDATA_CHART_PRIO_CPU_TEMPERATURE
            , update_every
            , RRDSET_TYPE_LINE
        );

        p->rd_high_precision_temperature = rrddim_add(p->st_high_precision_temperature, "high_precision_temperature", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    uint64_t temperature = p->temperature.current.Data;
    uint64_t passive_limit = p->passiveLimit.current.Data;
    uint64_t throttle_reasons = p->throttleReasons.current.Data;
    uint64_t high_precision_temperature = p->highPrecisionTemperature.current.Data;

    rrddim_set_by_pointer(p->st_temperature, p->rd_temperature, (collected_number) temperature);
    rrddim_set_by_pointer(p->st_passive_limit, p->rd_passive_limit, (collected_number) passive_limit);
    rrddim_set_by_pointer(p->st_throttle_reasons, p->rd_throttle_reasons, (collected_number) throttle_reasons);
    rrddim_set_by_pointer(p->st_high_precision_temperature, p->rd_high_precision_temperature, (collected_number) high_precision_temperature);

    rrdset_done(p->st_temperature);
    rrdset_done(p->st_passive_limit);
    rrdset_done(p->st_throttle_reasons);
    rrdset_done(p->st_high_precision_temperature);

    return true;
}

int do_PerflibThermalZoneInformation(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        thermal_zone_initialize();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("Thermal Zone Information");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_thermal_zone_performance(pDataBlock, update_every);

    return 0;
}
