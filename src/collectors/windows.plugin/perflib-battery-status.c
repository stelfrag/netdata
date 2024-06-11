#include "windows_plugin.h"
#include "windows-internals.h"

struct battery_performance {
    bool collected_metadata;

    RRDSET *st_charge_rate;
    RRDDIM *rd_charge_rate;

    RRDSET *st_discharge_rate;
    RRDDIM *rd_discharge_rate;

    RRDSET *st_remaining_capacity;
    RRDDIM *rd_remaining_capacity;

    RRDSET *st_voltage;
    RRDDIM *rd_voltage;

    COUNTER_DATA chargeRate;
    COUNTER_DATA dischargeRate;
    COUNTER_DATA remainingCapacity;
    COUNTER_DATA voltage;
};

static struct battery_performance battery_status = { 0 };

void initialize_battery_performance_keys(struct battery_performance *p) {
    p->chargeRate.key = "Charge Rate";
    p->dischargeRate.key = "Discharge Rate";
    p->remainingCapacity.key = "Remaining Capacity";
    p->voltage.key = "Voltage";
}

static void battery_initialize(void) {
    initialize_battery_performance_keys(&battery_status);
}

static bool do_battery_performance(PERF_DATA_BLOCK *pDataBlock, int update_every) {
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Battery Status");
    if(!pObjectType) return false;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for(LONG i = 0; i < pObjectType->NumInstances ; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if(!pi) break;

        if(!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        struct battery_performance *p = &battery_status;

        if(!p->collected_metadata) {
            // TODO collect battery metadata
            p->collected_metadata = true;
        }

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->chargeRate);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->dischargeRate);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->remainingCapacity);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->voltage);

        if(!p->st_charge_rate) {
            p->st_charge_rate = rrdset_create_localhost(
                "system"
                , "battery_charge_rate", NULL
                , "battery"
                , "system.battery_charge_rate"
                , "Battery Charge Rate"
                , "milliwatts"
                , PLUGIN_WINDOWS_NAME
                , "PerflibBatteryStatus"
                , NETDATA_CHART_PRIO_POWER_SUPPLY_CAPACITY
                , update_every
                , RRDSET_TYPE_LINE
            );

            p->rd_charge_rate = rrddim_add(p->st_charge_rate, "charge_rate", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        if(!p->st_discharge_rate) {
            p->st_discharge_rate = rrdset_create_localhost(
                "system"
                , "battery_discharge_rate", NULL
                , "battery"
                , "system.battery_discharge_rate"
                , "Battery Discharge Rate"
                , "milliwatts"
                , PLUGIN_WINDOWS_NAME
                , "PerflibBatteryStatus"
                , NETDATA_CHART_PRIO_POWER_SUPPLY_CAPACITY
                , update_every
                , RRDSET_TYPE_LINE
            );

            p->rd_discharge_rate = rrddim_add(p->st_discharge_rate, "discharge_rate", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        if(!p->st_remaining_capacity) {
            p->st_remaining_capacity = rrdset_create_localhost(
                "system"
                , "battery_remaining_capacity", NULL
                , "battery"
                , "system.battery_remaining_capacity"
                , "Battery Remaining Capacity"
                , "milliwatt-hours"
                , PLUGIN_WINDOWS_NAME
                , "PerflibBatteryStatus"
                , NETDATA_CHART_PRIO_POWER_SUPPLY_CAPACITY
                , update_every
                , RRDSET_TYPE_LINE
            );

            p->rd_remaining_capacity = rrddim_add(p->st_remaining_capacity, "remaining_capacity", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        if(!p->st_voltage) {
            p->st_voltage = rrdset_create_localhost(
                "system"
                , "battery_voltage", NULL
                , "battery"
                , "system.battery_voltage"
                , "Battery Voltage"
                , "millivolts"
                , PLUGIN_WINDOWS_NAME
                , "PerflibBatteryStatus"
                , NETDATA_CHART_PRIO_POWER_SUPPLY_CAPACITY
                , update_every
                , RRDSET_TYPE_LINE
            );

            p->rd_voltage = rrddim_add(p->st_voltage, "voltage", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        uint64_t charge_rate = p->chargeRate.current.Data;
        uint64_t discharge_rate = p->dischargeRate.current.Data;
        uint64_t remaining_capacity = p->remainingCapacity.current.Data;
        uint64_t voltage = p->voltage.current.Data;

        rrddim_set_by_pointer(p->st_charge_rate, p->rd_charge_rate, (collected_number) charge_rate);
        rrddim_set_by_pointer(p->st_discharge_rate, p->rd_discharge_rate, (collected_number) discharge_rate);
        rrddim_set_by_pointer(p->st_remaining_capacity, p->rd_remaining_capacity, (collected_number) remaining_capacity);
        rrddim_set_by_pointer(p->st_voltage, p->rd_voltage, (collected_number) voltage);

        rrdset_done(p->st_charge_rate);
        rrdset_done(p->st_discharge_rate);
        rrdset_done(p->st_remaining_capacity);
        rrdset_done(p->st_voltage);
    }

    return true;
}

int do_PerflibBatteryStatus(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        battery_initialize();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("Battery Status");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_battery_performance(pDataBlock, update_every);

    return 0;
}
