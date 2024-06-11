// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_plugin.h"
#include "windows-internals.h"

struct ldap_client_performance {
    bool collected_metadata;

    RRDSET *st_connections;
    RRDDIM *rd_open_connections;
    RRDDIM *rd_request_count;
    RRDDIM *rd_new_connections_sec;
    RRDDIM *rd_new_requests_sec;

    COUNTER_DATA openConnections;
    COUNTER_DATA requestCount;
    COUNTER_DATA newConnectionsSec;
    COUNTER_DATA newRequestsSec;
};

struct ldap_client_performance total_ldap = { 0 };

void initialize_ldap_client_performance_keys(struct ldap_client_performance *p) {
    p->openConnections.key = "Connections: Open Connections";
    p->requestCount.key = "Requests: Request Count";
    p->newConnectionsSec.key = "Connections: New Connections/sec";
    p->newRequestsSec.key = "Requests: New Requests/sec";
}

void dict_ldap_client_performance_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused) {
    struct ldap_client_performance *p = value;
    initialize_ldap_client_performance_keys(p);
}

static DICTIONARY *ldap_clients = NULL;

static void initialize_ldap_client(void) {
    initialize_ldap_client_performance_keys(&total_ldap);

    ldap_clients = dictionary_create_advanced(
        DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct ldap_client_performance));

    dictionary_register_insert_callback(ldap_clients, dict_ldap_client_performance_insert_cb, NULL);
}

static bool do_ldap_client_performance(PERF_DATA_BLOCK *pDataBlock, int update_every) {
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, "Ldap Client");
    if(!pObjectType) return false;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for(LONG i = 0; i < pObjectType->NumInstances ; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if(!pi) break;

        if(!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        bool is_total = false;
        struct ldap_client_performance *p;
        if(strcasecmp(windows_shared_buffer, "_Total") == 0) {
            p = &total_ldap;
            is_total = true;
        }
        else {
            p = dictionary_set(ldap_clients, windows_shared_buffer, NULL, sizeof(*p));
            is_total = false;
        }

        if(!is_total && !p->collected_metadata) {
            // TODO collect LDAP client metadata
            p->collected_metadata = true;
        }

        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->openConnections);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->requestCount);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->newConnectionsSec);
        perflibGetInstanceCounter(pDataBlock, pObjectType, pi, &p->newRequestsSec);

        if(!p->st_connections) {
            p->st_connections = rrdset_create_localhost(
                is_total ? "ldap_client" : "ldap_client_instance"
                , is_total ? "ldap_client" : windows_shared_buffer, NULL
                , is_total ? "ldap" : "ldap_instance"
                , "ldap.client"
                , "LDAP Client Performance"
                , "connections"
                , PLUGIN_WINDOWS_NAME
                , "PerflibLdapClientPerformance"
                , 300000
                , update_every
                , RRDSET_TYPE_STACKED
            );

            p->rd_open_connections = rrddim_add(p->st_connections, "open_connections", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            p->rd_request_count    = rrddim_add(p->st_connections, "request_count", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            p->rd_new_connections_sec = rrddim_add(p->st_connections, "new_connections_sec", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            p->rd_new_requests_sec = rrddim_add(p->st_connections, "new_requests_sec", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        uint64_t open_connections = p->openConnections.current.Data;
        uint64_t request_count = p->requestCount.current.Data;
        uint64_t new_connections_sec = p->newConnectionsSec.current.Data;
        uint64_t new_requests_sec = p->newRequestsSec.current.Data;

        rrddim_set_by_pointer(p->st_connections, p->rd_open_connections, (collected_number) open_connections);
        rrddim_set_by_pointer(p->st_connections, p->rd_request_count, (collected_number) request_count);
        rrddim_set_by_pointer(p->st_connections, p->rd_new_connections_sec, (collected_number) new_connections_sec);
        rrddim_set_by_pointer(p->st_connections, p->rd_new_requests_sec, (collected_number) new_requests_sec);

        rrdset_done(p->st_connections);
    }

    return true;
}

int do_PerflibLdapClientPerformance(int update_every, usec_t dt __maybe_unused) {
    static bool initialized = false;

    if(unlikely(!initialized)) {
        initialize_ldap_client();
        initialized = true;
    }

    DWORD id = RegistryFindIDByName("Ldap Client");
    if(id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
        return -1;

    PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
    if(!pDataBlock) return -1;

    do_ldap_client_performance(pDataBlock, update_every);

    return 0;
}
