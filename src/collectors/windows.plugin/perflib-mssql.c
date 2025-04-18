// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_plugin.h"
#include "windows-internals.h"

#include <sqlext.h>
#include <sqltypes.h>
#include <sql.h>

#define MEGA_FACTOR (1048576) // 1024 * 1024
// https://learn.microsoft.com/en-us/sql/sql-server/install/instance-configuration?view=sql-server-ver16
#define NETDATA_MAX_INSTANCE_NAME 32
#define NETDATA_MAX_INSTANCE_OBJECT 128
// https://learn.microsoft.com/en-us/previous-versions/sql/sql-server-2008-r2/ms191240(v=sql.105)#sysname
#define SQLSERVER_MAX_NAME_LENGTH (128)

BOOL is_sqlexpress = FALSE;

struct netdata_mssql_conn {
    const char *driver;
    const char *server;
    const char *address;
    const char *username;
    const char *password;
    int instances;
    bool windows_auth;

    SQLCHAR *connectionString;

    SQLHENV netdataSQLEnv;
    SQLHDBC netdataSQLHDBc;
    SQLHSTMT dataFileSizeSTMT;

    BOOL is_connected;
};

enum netdata_mssql_metrics {
    NETDATA_MSSQL_GENERAL_STATS,
    NETDATA_MSSQL_SQL_ERRORS,
    NETDATA_MSSQL_DATABASE,
    NETDATA_MSSQL_LOCKS,
    NETDATA_MSSQL_MEMORY,
    NETDATA_MSSQL_BUFFER_MANAGEMENT,
    NETDATA_MSSQL_SQL_STATS,
    NETDATA_MSSQL_ACCESS_METHODS,

    NETDATA_MSSQL_METRICS_END
};

struct mssql_instance {
    char *instanceID;

    struct netdata_mssql_conn conn;

    char *objectName[NETDATA_MSSQL_METRICS_END];

    RRDSET *st_user_connections;
    RRDDIM *rd_user_connections;

    RRDSET *st_process_blocked;
    RRDDIM *rd_process_blocked;

    RRDSET *st_stats_auto_param;
    RRDDIM *rd_stats_auto_param;

    RRDSET *st_stats_batch_request;
    RRDDIM *rd_stats_batch_request;

    RRDSET *st_stats_safe_auto;
    RRDDIM *rd_stats_safe_auto;

    RRDSET *st_stats_compilation;
    RRDDIM *rd_stats_compilation;

    RRDSET *st_stats_recompiles;
    RRDDIM *rd_stats_recompiles;

    RRDSET *st_buff_cache_hits;
    RRDDIM *rd_buff_cache_hits;

    RRDSET *st_buff_cache_page_life_expectancy;
    RRDDIM *rd_buff_cache_page_life_expectancy;

    RRDSET *st_buff_checkpoint_pages;
    RRDDIM *rd_buff_checkpoint_pages;

    RRDSET *st_buff_page_iops;
    RRDDIM *rd_buff_page_reads;
    RRDDIM *rd_buff_page_writes;

    RRDSET *st_access_method_page_splits;
    RRDDIM *rd_access_method_page_splits;

    RRDSET *st_sql_errors;
    RRDDIM *rd_sql_errors;

    DICTIONARY *locks_instances;
    RRDSET *st_deadLocks;
    RRDSET *st_lockWait;

    DICTIONARY *databases;

    RRDSET *st_conn_memory;
    RRDDIM *rd_conn_memory;

    RRDSET *st_ext_benefit_mem;
    RRDDIM *rd_ext_benefit_mem;

    RRDSET *st_pending_mem_grant;
    RRDDIM *rd_pending_mem_grant;

    RRDSET *st_mem_tot_server;
    RRDDIM *rd_mem_tot_server;

    COUNTER_DATA MSSQLAccessMethodPageSplits;
    COUNTER_DATA MSSQLBufferCacheHits;
    COUNTER_DATA MSSQLBufferCheckpointPages;
    COUNTER_DATA MSSQLBufferPageLifeExpectancy;
    COUNTER_DATA MSSQLBufferPageReads;
    COUNTER_DATA MSSQLBufferPageWrites;
    COUNTER_DATA MSSQLBlockedProcesses;
    COUNTER_DATA MSSQLUserConnections;
    COUNTER_DATA MSSQLConnectionMemoryBytes;
    COUNTER_DATA MSSQLExternalBenefitOfMemory;
    COUNTER_DATA MSSQLPendingMemoryGrants;
    COUNTER_DATA MSSQLSQLErrorsTotal;
    COUNTER_DATA MSSQLTotalServerMemory;
    COUNTER_DATA MSSQLStatsAutoParameterization;
    COUNTER_DATA MSSQLStatsBatchRequests;
    COUNTER_DATA MSSQLStatSafeAutoParameterization;
    COUNTER_DATA MSSQLCompilations;
    COUNTER_DATA MSSQLRecompilations;
};

struct mssql_lock_instance {
    char *resourceID;

    COUNTER_DATA lockWait;
    COUNTER_DATA deadLocks;

    RRDDIM *rd_lockWait;
    RRDDIM *rd_deadLocks;
};

enum db_instance_idx {
    NETDATA_MSSQL_ENUM_MDI_IDX_ACTIVE_TRANSACTIONS,
    NETDATA_MSSQL_ENUM_MDI_IDX_BACKUP_RESTORE_OP,
    NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHED,
    NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHES,
    NETDATA_MSSQL_ENUM_MDI_IDX_TRANSACTIONS,
    NETDATA_MSSQL_ENUM_MDI_IDX_WRITE_TRANSACTIONS,

    NETDATA_MSSQL_ENUM_MDI_IDX_END
};

struct mssql_db_instance {
    struct mssql_instance *parent;

    RRDSET *st_db_data_file_size;
    RRDSET *st_db_active_transactions;
    RRDSET *st_db_backup_restore_operations;
    RRDSET *st_db_log_flushed;
    RRDSET *st_db_log_flushes;
    RRDSET *st_db_transactions;
    RRDSET *st_db_write_transactions;

    RRDDIM *rd_db_data_file_size;
    RRDDIM *rd_db_active_transactions;
    RRDDIM *rd_db_backup_restore_operations;
    RRDDIM *rd_db_log_flushed;
    RRDDIM *rd_db_log_flushes;
    RRDDIM *rd_db_transactions;
    RRDDIM *rd_db_write_transactions;

    COUNTER_DATA MSSQLDatabaseActiveTransactions;
    COUNTER_DATA MSSQLDatabaseBackupRestoreOperations;
    COUNTER_DATA MSSQLDatabaseDataFileSize;
    COUNTER_DATA MSSQLDatabaseLogFlushed;
    COUNTER_DATA MSSQLDatabaseLogFlushes;
    COUNTER_DATA MSSQLDatabaseTransactions;
    COUNTER_DATA MSSQLDatabaseWriteTransactions;

    uint32_t updated;
};

enum netdata_mssql_odbc_errors {
    NETDATA_MSSQL_ODBC_NO_ERROR,
    NETDATA_MSSQL_ODBC_CONNECT,
    NETDATA_MSSQL_ODBC_BIND,
    NETDATA_MSSQL_ODBC_PREPARE,
    NETDATA_MSSQL_ODBC_QUERY,
    NETDATA_MSSQL_ODBC_FETCH
};

static char *netdata_MSSQL_error_text(enum netdata_mssql_odbc_errors val)
{
    switch (val) {
        case NETDATA_MSSQL_ODBC_NO_ERROR:
            return "NO ERROR";
        case NETDATA_MSSQL_ODBC_CONNECT:
            return "CONNECTION";
        case NETDATA_MSSQL_ODBC_BIND:
            return "BIND PARAMETER";
        case NETDATA_MSSQL_ODBC_PREPARE:
            return "PREPARE PARAMETER";
        case NETDATA_MSSQL_ODBC_QUERY:
            return "QUERY PARAMETER";
        case NETDATA_MSSQL_ODBC_FETCH:
        default:
            return "QUERY FETCH";
    }
}

static char *netdata_MSSQL_type_text(uint32_t type)
{
    switch (type) {
        case SQL_HANDLE_STMT:
            return "STMT";
        case SQL_HANDLE_DBC:
        default:
            return "DBC";
    }
}

// Connection and SQL
static void netdata_MSSQL_error(uint32_t type, SQLHANDLE handle, enum netdata_mssql_odbc_errors step)
{
    SQLCHAR state[1024];
    SQLCHAR message[1024];
    if (SQL_SUCCESS == SQLGetDiagRec((short)type, handle, 1, state, NULL, message, 1024, NULL)) {
        char *str_step = netdata_MSSQL_error_text(step);
        char *str_type = netdata_MSSQL_type_text(type);
        nd_log(
            NDLS_COLLECTORS,
            NDLP_ERR,
            "MSSQL server error using the handle %s running %s :  %s, %s",
            str_type,
            str_step,
            message,
            state);
    }
}

static ULONGLONG netdata_MSSQL_fill_data_file_size_dict(SQLHSTMT *stmt, SQLCHAR *query)
{
    static long db_size = 0;
    static SQLLEN col_data_len = 0;

    SQLRETURN ret;

    ret = SQLExecDirect(stmt, query, SQL_NTS);
    if (ret != SQL_SUCCESS) {
        netdata_MSSQL_error(SQL_HANDLE_STMT, stmt, NETDATA_MSSQL_ODBC_QUERY);
        return 0;
    }

    ret = SQLBindCol(stmt, 1, SQL_C_LONG, &db_size, sizeof(long), &col_data_len);

    if (ret != SQL_SUCCESS) {
        netdata_MSSQL_error(SQL_HANDLE_STMT, stmt, NETDATA_MSSQL_ODBC_PREPARE);
        return 0;
    }

    ret = SQLFetch(stmt);
    if (ret != SQL_SUCCESS) {
        netdata_MSSQL_error(SQL_HANDLE_STMT, stmt, NETDATA_MSSQL_ODBC_FETCH);
        return 0;
    }

    return (ULONGLONG)(db_size * MEGA_FACTOR);
}

ULONGLONG netdata_MSSQL_fill_data_file_size(struct netdata_mssql_conn *nmc, char *dbname)
{
    ULONGLONG value = 0;

    // We cannot access data for these tables without additional changes.
    // They should be blacklisted.
    if (!strcmp(dbname, "model") || !!strcmp(dbname, "model_msdb") || !!strcmp(dbname, "model_replicatedmaster")  || strcmp(dbname, "mssqlsystemresource"))
        return ULONG_LONG_MAX;

    // https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-database-files-transact-sql?view=sql-server-ver16
    SQLCHAR query[512];
    snprintfz((char *)query, 511, "SELECT size * 8/1024 FROM %s.sys.database_files WHERE type = 0;", dbname);

    value = netdata_MSSQL_fill_data_file_size_dict(nmc->dataFileSizeSTMT, query);

    SQLFreeStmt(nmc->dataFileSizeSTMT, SQL_CLOSE);
    return value;
}

int dict_mssql_databases_run_query(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    struct mssql_db_instance *mdi = value;
    const char *dbname = dictionary_acquired_item_name((DICTIONARY_ITEM *)item);

    mdi->MSSQLDatabaseDataFileSize.current.Data = netdata_MSSQL_fill_data_file_size(&mdi->parent->conn, (char *)dbname);
}

static bool netdata_MSSQL_initialize_conection(struct netdata_mssql_conn *nmc)
{
    SQLRETURN ret;
    if (nmc->netdataSQLEnv == NULL) {
        ret = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &nmc->netdataSQLEnv);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
            return FALSE;

        ret = SQLSetEnvAttr(nmc->netdataSQLEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
            return FALSE;
        }
    }

    ret = SQLAllocHandle(SQL_HANDLE_DBC, nmc->netdataSQLEnv, &nmc->netdataSQLHDBc);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        return FALSE;
    }

    ret = SQLSetConnectAttr(nmc->netdataSQLHDBc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        return FALSE;
    }

    ret = SQLSetConnectAttr(nmc->netdataSQLHDBc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)TRUE, 0);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        return FALSE;
    }

    SQLCHAR ret_conn_str[1024];
    ret = SQLDriverConnect(
        nmc->netdataSQLHDBc, NULL, nmc->connectionString, SQL_NTS, ret_conn_str, 1024, NULL, SQL_DRIVER_NOPROMPT);

    BOOL retConn;
    switch (ret) {
        case SQL_NO_DATA_FOUND:
        case SQL_INVALID_HANDLE:
        case SQL_ERROR:
        default:
            netdata_MSSQL_error(SQL_HANDLE_DBC, nmc->netdataSQLHDBc, NETDATA_MSSQL_ODBC_CONNECT);
            retConn = FALSE;
            break;
        case SQL_SUCCESS:
        case SQL_SUCCESS_WITH_INFO:
            retConn = TRUE;
            break;
    }

    if (retConn) {
        ret = SQLAllocHandle(SQL_HANDLE_STMT, nmc->netdataSQLHDBc, &nmc->dataFileSizeSTMT);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
            retConn = FALSE;
    }

    return retConn;
}

// Dictionary
static DICTIONARY *mssql_instances = NULL;

static void initialize_mssql_objects(struct mssql_instance *mi, const char *instance)
{
    char prefix[NETDATA_MAX_INSTANCE_NAME];
    if (!strcmp(instance, "MSSQLSERVER")) {
        strncpyz(prefix, "SQLServer:", sizeof(prefix) - 1);
    } else if (!strcmp(instance, "SQLEXPRESS")) {
        strncpyz(prefix, "MSSQL$SQLEXPRESS:", sizeof(prefix) - 1);
    } else {
        char *express = (!is_sqlexpress) ? "" : "SQLEXPRESS:";
        snprintfz(prefix, sizeof(prefix) - 1, "MSSQL$%s%s:", express, instance);
    }

    size_t length = strlen(prefix);
    char name[NETDATA_MAX_INSTANCE_OBJECT];
    snprintfz(name, sizeof(name) - 1, "%s%s", prefix, "General Statistics");
    mi->objectName[NETDATA_MSSQL_GENERAL_STATS] = strdupz(name);

    strncpyz(&name[length], "SQL Errors", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_SQL_ERRORS] = strdupz(name);

    strncpyz(&name[length], "Databases", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_DATABASE] = strdupz(name);

    strncpyz(&name[length], "SQL Statistics", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_SQL_STATS] = strdupz(name);

    strncpyz(&name[length], "Buffer Manager", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_BUFFER_MANAGEMENT] = strdupz(name);

    strncpyz(&name[length], "Memory Manager", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_MEMORY] = strdupz(name);

    strncpyz(&name[length], "Locks", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_LOCKS] = strdupz(name);

    strncpyz(&name[length], "Access Methods", sizeof(name) - length);
    mi->objectName[NETDATA_MSSQL_ACCESS_METHODS] = strdupz(name);

    mi->instanceID = strdupz(instance);
}

static inline void initialize_mssql_keys(struct mssql_instance *mi)
{
    // General Statistics (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-general-statistics-object)
    mi->MSSQLUserConnections.key = "User Connections";
    mi->MSSQLBlockedProcesses.key = "Processes blocked";

    // SQL Statistics (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-sql-statistics-object)
    mi->MSSQLStatsAutoParameterization.key = "Auto-Param Attempts/sec";
    mi->MSSQLStatsBatchRequests.key = "Batch Requests/sec";
    mi->MSSQLStatSafeAutoParameterization.key = "Safe Auto-Params/sec";
    mi->MSSQLCompilations.key = "SQL Compilations/sec";
    mi->MSSQLRecompilations.key = "SQL Re-Compilations/sec";

    // Buffer Management (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-buffer-manager-object)
    mi->MSSQLBufferCacheHits.key = "Buffer cache hit ratio";
    mi->MSSQLBufferPageLifeExpectancy.key = "Page life expectancy";
    mi->MSSQLBufferCheckpointPages.key = "Checkpoint pages/sec";
    mi->MSSQLBufferPageReads.key = "Page reads/sec";
    mi->MSSQLBufferPageWrites.key = "Page writes/sec";

    // Access Methods (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-access-methods-object)
    mi->MSSQLAccessMethodPageSplits.key = "Page Splits/sec";

    // Errors (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-sql-errors-object)
    mi->MSSQLSQLErrorsTotal.key = "Errors/sec";

    // Memory Management (https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-memory-manager-object)
    mi->MSSQLConnectionMemoryBytes.key = "Connection Memory (KB)";
    mi->MSSQLExternalBenefitOfMemory.key = "External benefit of memory";
    mi->MSSQLPendingMemoryGrants.key = "Memory Grants Pending";
    mi->MSSQLTotalServerMemory.key = "Total Server Memory (KB)";
}

void dict_mssql_insert_locks_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    const char *resource = dictionary_acquired_item_name((DICTIONARY_ITEM *)item);

    // https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-locks-object
    struct mssql_lock_instance *ptr = value;
    ptr->resourceID = strdupz(resource);
    ptr->deadLocks.key = "Number of Deadlocks/sec";
    ptr->lockWait.key = "Lock Waits/sec";
}

void dict_mssql_insert_databases_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    struct mssql_db_instance *ptr = value;

    // https://learn.microsoft.com/en-us/sql/relational-databases/performance-monitor/sql-server-databases-object
    ptr->MSSQLDatabaseActiveTransactions.key = "Active Transactions";
    ptr->MSSQLDatabaseBackupRestoreOperations.key = "Backup/Restore Throughput/sec";
    ptr->MSSQLDatabaseLogFlushed.key = "Log Bytes Flushed/sec";
    ptr->MSSQLDatabaseLogFlushes.key = "Log Flushes/sec";
    ptr->MSSQLDatabaseTransactions.key = "Transactions/sec";
    ptr->MSSQLDatabaseWriteTransactions.key = "Write Transactions/sec";
}

// Options
void netdata_mount_mssql_connection_string(struct netdata_mssql_conn *dbInput)
{
    SQLCHAR conn[1024];
    const char *serverAddress;
    const char *serverAddressArg;
    if (!dbInput) {
        return;
    }

    char auth[512];
    if (dbInput->server && dbInput->address) {
        nd_log(
            NDLS_COLLECTORS,
            NDLP_ERR,
            "Collector is not expecting server and address defined together, please, select one of them.");
        dbInput->connectionString = NULL;
        return;
    }

    if (dbInput->server) {
        serverAddress = "Server";
        serverAddressArg = dbInput->server;
    } else {
        serverAddressArg = "Address";
        serverAddress = dbInput->address;
    }

    if (dbInput->windows_auth)
        snprintfz(auth, sizeof(auth) - 1, "Trusted_Connection = yes");
    else if (!dbInput->username || !dbInput->password) {
        nd_log(
            NDLS_COLLECTORS,
            NDLP_ERR,
            "You are not using Windows Authentication. Thus, it is necessary to specify user and password.");
        dbInput->connectionString = NULL;
        return;
    } else {
        snprintfz(auth, sizeof(auth) - 1, "UID=%s;PWD=%s;", dbInput->username, dbInput->password);
    }

    snprintfz(
        (char *)conn, sizeof(conn) - 1, "Driver={%s};%s=%s;%s", dbInput->driver, serverAddress, serverAddressArg, auth);
    dbInput->connectionString = (SQLCHAR *)strdupz((char *)conn);
}

static void netdata_read_config_options(struct netdata_mssql_conn *dbconn)
{
    dbconn->netdataSQLEnv = NULL;
    dbconn->netdataSQLHDBc = NULL;
    dbconn->dataFileSizeSTMT = NULL;

    dbconn->is_connected = FALSE;

    static uint16_t expected_instances = 1;
    static uint16_t total_instances = 0;
    if (total_instances > expected_instances)
        return;

#define NETDATA_MAX_MSSSQL_SECTION_LENGTH (40)
#define NETDATA_DEFAULT_MSSQL_SECTION "plugin:windows:PerflibMSSQL"
    char section_name[NETDATA_MAX_MSSSQL_SECTION_LENGTH + 1];
    strncpyz(section_name, NETDATA_DEFAULT_MSSQL_SECTION, sizeof(NETDATA_DEFAULT_MSSQL_SECTION));
    if (total_instances) {
        snprintfz(&section_name[sizeof(NETDATA_DEFAULT_MSSQL_SECTION) - 1], 5, "%d", total_instances);
    }

    dbconn->driver = inicfg_get(&netdata_config, section_name, "driver", "SQL Server");
    dbconn->server = inicfg_get(&netdata_config, section_name, "server", NULL);
    dbconn->address = inicfg_get(&netdata_config, section_name, "address", NULL);
    dbconn->username = inicfg_get(&netdata_config, section_name, "uid", NULL);
    dbconn->password = inicfg_get(&netdata_config, section_name, "pwd", NULL);
    dbconn->instances = (int)inicfg_get_number(&netdata_config, section_name, "additional instances", 0);
    dbconn->windows_auth = inicfg_get_boolean(&netdata_config, section_name, "windows authentication", false);

    netdata_mount_mssql_connection_string(dbconn);
    if (!total_instances)
        expected_instances = dbconn->instances;

    total_instances++;
}

void dict_mssql_insert_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    struct mssql_instance *mi = value;
    const char *instance = dictionary_acquired_item_name((DICTIONARY_ITEM *)item);

    if (!mi->locks_instances) {
        mi->locks_instances = dictionary_create_advanced(
            DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct mssql_lock_instance));
        dictionary_register_insert_callback(mi->locks_instances, dict_mssql_insert_locks_cb, NULL);
    }

    if (!mi->databases) {
        mi->databases = dictionary_create_advanced(
            DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct mssql_db_instance));
        dictionary_register_insert_callback(mi->databases, dict_mssql_insert_databases_cb, NULL);
    }

    initialize_mssql_objects(mi, instance);
    initialize_mssql_keys(mi);
    netdata_read_config_options(&mi->conn);

    if (mi->conn.connectionString)
        mi->conn.is_connected = netdata_MSSQL_initialize_conection(&mi->conn);
}

static int mssql_fill_dictionary()
{
    HKEY hKey;
    LSTATUS ret = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Microsoft SQL Server\\Instance Names\\SQL", 0, KEY_READ, &hKey);
    if (ret != ERROR_SUCCESS)
        return -1;

    DWORD values = 0;

    ret = RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    if (ret != ERROR_SUCCESS) {
        goto endMSSQLFillDict;
    }

    if (!values) {
        ret = ERROR_PATH_NOT_FOUND;
        goto endMSSQLFillDict;
    }

// https://learn.microsoft.com/en-us/windows/win32/sysinfo/enumerating-registry-subkeys
#define REGISTRY_MAX_VALUE 16383

    DWORD i;
    char avalue[REGISTRY_MAX_VALUE] = {'\0'};
    for (i = 0; i < values; i++) {
        avalue[0] = '\0';
        DWORD length = REGISTRY_MAX_VALUE;

        ret = RegEnumValue(hKey, i, avalue, &length, NULL, NULL, NULL, NULL);
        if (ret != ERROR_SUCCESS)
            continue;

        if (!strcmp(avalue, "SQLEXPRESS")) {
            is_sqlexpress = TRUE;
        }

        struct mssql_instance *p = dictionary_set(mssql_instances, avalue, NULL, sizeof(*p));
    }

endMSSQLFillDict:
    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS) ? 0 : -1;
}

static int initialize(void)
{
    mssql_instances = dictionary_create_advanced(
        DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE, NULL, sizeof(struct mssql_instance));

    dictionary_register_insert_callback(mssql_instances, dict_mssql_insert_cb, NULL);

    if (mssql_fill_dictionary()) {
        return -1;
    }

    return 0;
}

static void do_mssql_general_stats(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType =
        perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_GENERAL_STATS]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLUserConnections)) {
        if (!mi->st_user_connections) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_user_connections", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_user_connections = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "connections",
                "mssql.instance_user_connections",
                "User connections",
                "connections",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_USER_CONNECTIONS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_user_connections = rrddim_add(mi->st_user_connections, "user", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_user_connections->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_user_connections, mi->rd_user_connections, (collected_number)mi->MSSQLUserConnections.current.Data);
        rrdset_done(mi->st_user_connections);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBlockedProcesses)) {
        if (!mi->st_process_blocked) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_blocked_process", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_process_blocked = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "processes",
                "mssql.instance_blocked_processes",
                "Blocked processes",
                "process",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BLOCKED_PROCESSES,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_process_blocked = rrddim_add(mi->st_process_blocked, "blocked", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_process_blocked->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_process_blocked, mi->rd_process_blocked, (collected_number)mi->MSSQLBlockedProcesses.current.Data);
        rrdset_done(mi->st_process_blocked);
    }
}

static void do_mssql_sql_statistics(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_SQL_STATS]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLStatsAutoParameterization)) {
        if (!mi->st_stats_auto_param) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_sqlstats_auto_parameterization_attempts", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_stats_auto_param = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "sql activity",
                "mssql.instance_sqlstats_auto_parameterization_attempts",
                "Failed auto-parameterization attempts",
                "attempts/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_STATS_AUTO_PARAMETRIZATION,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_stats_auto_param =
                rrddim_add(mi->st_stats_auto_param, "failed", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_stats_auto_param->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_stats_auto_param,
            mi->rd_stats_auto_param,
            (collected_number)mi->MSSQLStatsAutoParameterization.current.Data);
        rrdset_done(mi->st_stats_auto_param);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLStatsBatchRequests)) {
        if (!mi->st_stats_batch_request) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_sqlstats_batch_requests", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_stats_batch_request = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "sql activity",
                "mssql.instance_sqlstats_batch_requests",
                "Total of batches requests",
                "requests/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_STATS_BATCH_REQUEST,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_stats_batch_request =
                rrddim_add(mi->st_stats_batch_request, "batch", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_stats_batch_request->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_stats_batch_request,
            mi->rd_stats_batch_request,
            (collected_number)mi->MSSQLStatsBatchRequests.current.Data);
        rrdset_done(mi->st_stats_batch_request);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLStatSafeAutoParameterization)) {
        if (!mi->st_stats_safe_auto) {
            snprintfz(
                id, RRD_ID_LENGTH_MAX, "instance_%s_sqlstats_safe_auto_parameterization_attempts", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_stats_safe_auto = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "sql activity",
                "mssql.instance_sqlstats_safe_auto_parameterization_attempts",
                "Safe auto-parameterization attempts",
                "attempts/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_STATS_SAFE_AUTO_PARAMETRIZATION,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_stats_safe_auto = rrddim_add(mi->st_stats_safe_auto, "safe", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_stats_safe_auto->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_stats_safe_auto,
            mi->rd_stats_safe_auto,
            (collected_number)mi->MSSQLStatSafeAutoParameterization.current.Data);
        rrdset_done(mi->st_stats_safe_auto);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLCompilations)) {
        if (!mi->st_stats_compilation) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_sqlstats_sql_compilations", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_stats_compilation = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "sql activity",
                "mssql.instance_sqlstats_sql_compilations",
                "SQL compilations",
                "compilations/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_STATS_COMPILATIONS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_stats_compilation =
                rrddim_add(mi->st_stats_compilation, "compilations", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_stats_compilation->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_stats_compilation, mi->rd_stats_compilation, (collected_number)mi->MSSQLCompilations.current.Data);
        rrdset_done(mi->st_stats_compilation);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLRecompilations)) {
        if (!mi->st_stats_recompiles) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_sqlstats_sql_recompilations", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_stats_recompiles = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "sql activity",
                "mssql.instance_sqlstats_sql_recompilations",
                "SQL re-compilations",
                "recompiles/",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_STATS_RECOMPILATIONS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_stats_recompiles =
                rrddim_add(mi->st_stats_recompiles, "recompiles", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_stats_recompiles->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_stats_recompiles, mi->rd_stats_recompiles, (collected_number)mi->MSSQLRecompilations.current.Data);
        rrdset_done(mi->st_stats_recompiles);
    }
}

static void do_mssql_buffer_management(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType =
        perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_BUFFER_MANAGEMENT]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBufferCacheHits)) {
        if (!mi->st_buff_cache_hits) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_cache_hit_ratio", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_buff_cache_hits = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "buffer cache",
                "mssql.instance_cache_hit_ratio",
                "Buffer Cache hit ratio",
                "percentage",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BUFF_CACHE_HIT_RATIO,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_buff_cache_hits =
                rrddim_add(mi->st_buff_cache_hits, "hit_ratio", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_buff_cache_hits->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_buff_cache_hits, mi->rd_buff_cache_hits, (collected_number)mi->MSSQLBufferCacheHits.current.Data);
        rrdset_done(mi->st_buff_cache_hits);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBufferCheckpointPages)) {
        if (!mi->st_buff_checkpoint_pages) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_bufman_checkpoint_pages", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_buff_checkpoint_pages = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "buffer cache",
                "mssql.instance_bufman_checkpoint_pages",
                "Flushed pages",
                "pages/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BUFF_CHECKPOINT_PAGES,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_buff_checkpoint_pages =
                rrddim_add(mi->st_buff_checkpoint_pages, "log", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_buff_checkpoint_pages->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_buff_checkpoint_pages,
            mi->rd_buff_checkpoint_pages,
            (collected_number)mi->MSSQLBufferCheckpointPages.current.Data);
        rrdset_done(mi->st_buff_checkpoint_pages);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBufferPageLifeExpectancy)) {
        if (!mi->st_buff_cache_page_life_expectancy) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_bufman_page_life_expectancy", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_buff_cache_page_life_expectancy = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "buffer cache",
                "mssql.instance_bufman_page_life_expectancy",
                "Page life expectancy",
                "seconds",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BUFF_PAGE_LIFE_EXPECTANCY,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_buff_cache_page_life_expectancy = rrddim_add(
                mi->st_buff_cache_page_life_expectancy, "life_expectancy", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(
                mi->st_buff_cache_page_life_expectancy->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_buff_cache_page_life_expectancy,
            mi->rd_buff_cache_page_life_expectancy,
            (collected_number)mi->MSSQLBufferPageLifeExpectancy.current.Data);
        rrdset_done(mi->st_buff_cache_page_life_expectancy);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBufferPageReads) &&
        perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLBufferPageWrites)) {
        if (!mi->st_buff_page_iops) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_bufman_iops", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_buff_page_iops = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "buffer cache",
                "mssql.instance_bufman_iops",
                "Number of pages input and output",
                "pages/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BUFF_MAN_IOPS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_buff_page_reads = rrddim_add(mi->st_buff_page_iops, "read", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            mi->rd_buff_page_writes =
                rrddim_add(mi->st_buff_page_iops, "written", NULL, -1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_buff_page_iops->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_buff_page_iops, mi->rd_buff_page_reads, (collected_number)mi->MSSQLBufferPageReads.current.Data);
        rrddim_set_by_pointer(
            mi->st_buff_page_iops, mi->rd_buff_page_writes, (collected_number)mi->MSSQLBufferPageWrites.current.Data);

        rrdset_done(mi->st_buff_page_iops);
    }
}

static void do_mssql_access_methods(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType =
        perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_ACCESS_METHODS]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLAccessMethodPageSplits)) {
        if (!mi->st_access_method_page_splits) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_accessmethods_page_splits", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_access_method_page_splits = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "buffer cache",
                "mssql.instance_accessmethods_page_splits",
                "Page splits",
                "splits/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_BUFF_METHODS_PAGE_SPLIT,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_access_method_page_splits =
                rrddim_add(mi->st_access_method_page_splits, "page", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(
                mi->st_access_method_page_splits->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_access_method_page_splits,
            mi->rd_access_method_page_splits,
            (collected_number)mi->MSSQLAccessMethodPageSplits.current.Data);
        rrdset_done(mi->st_access_method_page_splits);
    }
}

static void do_mssql_errors(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_SQL_ERRORS]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLSQLErrorsTotal)) {
        if (!mi->st_sql_errors) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_sql_errors_total", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_sql_errors = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "errors",
                "mssql.instance_sql_errors",
                "Errors",
                "errors/s",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_SQL_ERRORS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_sql_errors = rrddim_add(mi->st_sql_errors, "errors", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);

            rrdlabels_add(mi->st_sql_errors->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_sql_errors, mi->rd_sql_errors, (collected_number)mi->MSSQLAccessMethodPageSplits.current.Data);
        rrdset_done(mi->st_sql_errors);
    }
}

void dict_mssql_locks_wait_charts(struct mssql_instance *mi, int update_every)
{
    if (!mi->st_lockWait) {
        char id[RRD_ID_LENGTH_MAX + 1];

        snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_locks_lock_wait", mi->instanceID);
        netdata_fix_chart_name(id);
        mi->st_lockWait = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "locks",
            "mssql.instance_locks_lock_wait",
            "Lock requests that required the caller to wait.",
            "locks/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_LOCKS_WAIT,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(mi->st_lockWait->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
    }
}

void dict_mssql_locks_wait_dimension(struct mssql_instance *mi, struct mssql_lock_instance *mli)
{
    if (!mli->rd_lockWait) {
        char id[RRD_ID_LENGTH_MAX + 1];
        snprintfz(id, RRD_ID_LENGTH_MAX, "%s", mli->resourceID);
        netdata_fix_chart_name(id);

        mli->rd_lockWait = rrddim_add(mi->st_lockWait, id, NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }
    rrddim_set_by_pointer(mi->st_lockWait, mli->rd_lockWait, (collected_number)(mli->lockWait.current.Data));
}

void dict_mssql_dead_locks_charts(struct mssql_instance *mi, int update_every)
{
    if (!mi->st_deadLocks) {
        char id[RRD_ID_LENGTH_MAX + 1];

        snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_locks_deadlocks", mi->instanceID);
        netdata_fix_chart_name(id);
        mi->st_deadLocks = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "locks",
            "mssql.instance_locks_deadlocks",
            "Lock requests that resulted in deadlock.",
            "deadlocks/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_LOCKS_DEADLOCK,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(mi->st_deadLocks->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
    }
}

void dict_mssql_deadlocks_dimension(struct mssql_instance *mi, struct mssql_lock_instance *mli)
{
    if (!mli->rd_deadLocks) {
        char id[RRD_ID_LENGTH_MAX + 1];
        snprintfz(id, RRD_ID_LENGTH_MAX, "%s", mli->resourceID);
        netdata_fix_chart_name(id);

        mli->rd_deadLocks = rrddim_add(mi->st_deadLocks, id, NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }
    rrddim_set_by_pointer(mi->st_deadLocks, mli->rd_deadLocks, (collected_number)mli->deadLocks.current.Data);
}

static void do_mssql_locks(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_LOCKS]);
    if (!pObjectType)
        return;

    if (!pObjectType->NumInstances)
        return;

    dict_mssql_locks_wait_charts(mi, update_every);
    dict_mssql_dead_locks_charts(mi, update_every);
    PERF_INSTANCE_DEFINITION *pi = NULL;
    for (LONG i = 0; i < pObjectType->NumInstances; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if (!pi)
            break;

        if (!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        if (!strcasecmp(windows_shared_buffer, "_Total"))
            continue;

        struct mssql_lock_instance *mli =
            dictionary_set(mi->locks_instances, windows_shared_buffer, NULL, sizeof(*mli));
        if (!mli)
            continue;

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mli->lockWait))
            dict_mssql_locks_wait_dimension(mi, mli);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mli->deadLocks))
            dict_mssql_deadlocks_dimension(mi, mli);
    }

    if (mi->st_lockWait)
        rrdset_done(mi->st_lockWait);

    if (mi->st_deadLocks)
        rrdset_done(mi->st_deadLocks);
}

static void mssql_database_backup_restore_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_backup_restore_operations) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_backup_restore_operations", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_backup_restore_operations = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_backup_restore_operations",
            "Backup IO per database",
            "operations/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_BACKUP_RESTORE_OPERATIONS,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(
            mli->st_db_backup_restore_operations->rrdlabels,
            "mssql_instance",
            mli->parent->instanceID,
            RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_backup_restore_operations->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_backup_restore_operations) {
        mli->rd_db_backup_restore_operations =
            rrddim_add(mli->st_db_backup_restore_operations, "backup", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_BACKUP_RESTORE_OP)) {
        rrddim_set_by_pointer(
            mli->st_db_backup_restore_operations,
            mli->rd_db_backup_restore_operations,
            (collected_number)mli->MSSQLDatabaseBackupRestoreOperations.current.Data);
    }

    rrdset_done(mli->st_db_backup_restore_operations);
}

static void mssql_database_log_flushes_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_log_flushes) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_log_flushes", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_log_flushes = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_log_flushes",
            "Log flushes",
            "flushes/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_LOG_FLUSHES,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(mli->st_db_log_flushes->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_log_flushes->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_log_flushes) {
        mli->rd_db_log_flushes = rrddim_add(mli->st_db_log_flushes, "flushes", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHES)) {
        rrddim_set_by_pointer(
            mli->st_db_log_flushes,
            mli->rd_db_log_flushes,
            (collected_number)mli->MSSQLDatabaseLogFlushes.current.Data);
    }

    rrdset_done(mli->st_db_log_flushes);
}

static void mssql_database_log_flushed_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_log_flushed) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_log_flushed", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_log_flushed = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_log_flushed",
            "Log flushed",
            "bytes/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_LOG_FLUSHED,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(mli->st_db_log_flushed->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_log_flushed->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_log_flushed) {
        mli->rd_db_log_flushed = rrddim_add(mli->st_db_log_flushed, "flushed", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHED)) {
        rrddim_set_by_pointer(
            mli->st_db_log_flushed,
            mli->rd_db_log_flushed,
            (collected_number)mli->MSSQLDatabaseLogFlushed.current.Data);
    }

    rrdset_done(mli->st_db_log_flushed);
}

static void mssql_transactions_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_transactions) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_transactions", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_transactions = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_transactions",
            "Transactions",
            "transactions/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_TRANSACTIONS,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(mli->st_db_transactions->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_transactions->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_transactions) {
        mli->rd_db_transactions =
            rrddim_add(mli->st_db_transactions, "transactions", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_TRANSACTIONS)) {
        rrddim_set_by_pointer(
            mli->st_db_transactions,
            mli->rd_db_transactions,
            (collected_number)mli->MSSQLDatabaseTransactions.current.Data);
    }

    rrdset_done(mli->st_db_transactions);
}

static void mssql_write_transactions_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_write_transactions) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_write_transactions", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_write_transactions = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_write_transactions",
            "Write transactions",
            "transactions/s",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_WRITE_TRANSACTIONS,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(
            mli->st_db_write_transactions->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_write_transactions->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_write_transactions) {
        mli->rd_db_write_transactions =
            rrddim_add(mli->st_db_write_transactions, "write", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_WRITE_TRANSACTIONS)) {
        rrddim_set_by_pointer(
            mli->st_db_write_transactions,
            mli->rd_db_write_transactions,
            (collected_number)mli->MSSQLDatabaseWriteTransactions.current.Data);
    }

    rrdset_done(mli->st_db_write_transactions);
}

static void mssql_active_transactions_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    if (!mli->st_db_active_transactions) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_active_transactions", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_active_transactions = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "transactions",
            "mssql.database_active_transactions",
            "Active transactions per database",
            "transactions",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_ACTIVE_TRANSACTIONS,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(
            mli->st_db_active_transactions->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_active_transactions->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (!mli->rd_db_active_transactions) {
        mli->rd_db_active_transactions =
            rrddim_add(mli->st_db_active_transactions, "active", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    if (mli->updated & (1 << NETDATA_MSSQL_ENUM_MDI_IDX_ACTIVE_TRANSACTIONS)) {
        rrddim_set_by_pointer(
            mli->st_db_active_transactions,
            mli->rd_db_active_transactions,
            (collected_number)mli->MSSQLDatabaseActiveTransactions.current.Data);
    }

    rrdset_done(mli->st_db_active_transactions);
}

static inline void mssql_data_file_size_chart(struct mssql_db_instance *mli, const char *db, int update_every)
{
    if (unlikely(!mli->parent->conn.is_connected) || mli->MSSQLDatabaseDataFileSize.current.Data == ULONG_LONG_MAX)
        return;

    char id[RRD_ID_LENGTH_MAX + 1];

    if (unlikely(!mli->st_db_data_file_size)) {
        snprintfz(id, RRD_ID_LENGTH_MAX, "db_%s_instance_%s_data_files_size", db, mli->parent->instanceID);
        netdata_fix_chart_name(id);
        mli->st_db_data_file_size = rrdset_create_localhost(
            "mssql",
            id,
            NULL,
            "size",
            "mssql.database_data_files_size",
            "Current database size.",
            "bytes",
            PLUGIN_WINDOWS_NAME,
            "PerflibMSSQL",
            PRIO_MSSQL_DATABASE_DATA_FILE_SIZE,
            update_every,
            RRDSET_TYPE_LINE);

        rrdlabels_add(
            mli->st_db_data_file_size->rrdlabels, "mssql_instance", mli->parent->instanceID, RRDLABEL_SRC_AUTO);
        rrdlabels_add(mli->st_db_data_file_size->rrdlabels, "database", db, RRDLABEL_SRC_AUTO);
    }

    if (unlikely(!mli->rd_db_data_file_size)) {
        mli->rd_db_data_file_size = rrddim_add(mli->st_db_data_file_size, "size", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
    }

    collected_number data = mli->MSSQLDatabaseDataFileSize.current.Data;
    rrddim_set_by_pointer(mli->st_db_data_file_size, mli->rd_db_data_file_size, data);

    rrdset_done(mli->st_db_data_file_size);
}

int dict_mssql_databases_charts_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    struct mssql_db_instance *mli = value;
    const char *db = dictionary_acquired_item_name((DICTIONARY_ITEM *)item);

    int *update_every = data;

    void (*transaction_chart[])(struct mssql_db_instance *, const char *, int) = {
        mssql_data_file_size_chart,
        mssql_transactions_chart,
        mssql_database_backup_restore_chart,
        mssql_database_log_flushed_chart,
        mssql_database_log_flushes_chart,
        mssql_active_transactions_chart,
        mssql_write_transactions_chart,

        // Last function pointer must be NULL
        NULL};

    int i;
    for (i = 0; transaction_chart[i]; i++) {
        transaction_chart[i](mli, db, *update_every);
    }

    return 1;
}

static void do_mssql_databases(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_DATABASE]);
    if (!pObjectType)
        return;

    PERF_INSTANCE_DEFINITION *pi = NULL;
    for (LONG i = 0; i < pObjectType->NumInstances; i++) {
        pi = perflibForEachInstance(pDataBlock, pObjectType, pi);
        if (!pi)
            break;

        if (!getInstanceName(pDataBlock, pObjectType, pi, windows_shared_buffer, sizeof(windows_shared_buffer)))
            strncpyz(windows_shared_buffer, "[unknown]", sizeof(windows_shared_buffer) - 1);

        if (!strcasecmp(windows_shared_buffer, "_Total"))
            continue;

        struct mssql_db_instance *mdi = dictionary_set(mi->databases, windows_shared_buffer, NULL, sizeof(*mdi));
        if (!mdi)
            continue;

        mdi->updated = 0;
        if (!mdi->parent) {
            mdi->parent = mi;
        }

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseActiveTransactions))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_ACTIVE_TRANSACTIONS);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseBackupRestoreOperations))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_BACKUP_RESTORE_OP);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseLogFlushed))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHED);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseLogFlushes))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_LOG_FLUSHES);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseTransactions))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_TRANSACTIONS);

        if (perflibGetObjectCounter(pDataBlock, pObjectType, &mdi->MSSQLDatabaseWriteTransactions))
            mdi->updated |= (1 << NETDATA_MSSQL_ENUM_MDI_IDX_WRITE_TRANSACTIONS);
    }

    dictionary_sorted_walkthrough_read(mi->databases, dict_mssql_databases_charts_cb, &update_every);
}

static void do_mssql_memory_mgr(PERF_DATA_BLOCK *pDataBlock, struct mssql_instance *mi, int update_every)
{
    char id[RRD_ID_LENGTH_MAX + 1];

    PERF_OBJECT_TYPE *pObjectType = perflibFindObjectTypeByName(pDataBlock, mi->objectName[NETDATA_MSSQL_MEMORY]);
    if (!pObjectType)
        return;

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLConnectionMemoryBytes)) {
        if (!mi->st_conn_memory) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_memmgr_connection_memory_bytes", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_conn_memory = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "memory",
                "mssql.instance_memmgr_connection_memory_bytes",
                "Amount of dynamic memory to maintain connections",
                "bytes",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_MEMMGR_CONNECTION_MEMORY_BYTES,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_conn_memory = rrddim_add(mi->st_conn_memory, "memory", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_conn_memory->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_conn_memory,
            mi->rd_conn_memory,
            (collected_number)(mi->MSSQLConnectionMemoryBytes.current.Data * 1024));
        rrdset_done(mi->st_conn_memory);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLExternalBenefitOfMemory)) {
        if (!mi->st_ext_benefit_mem) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_memmgr_external_benefit_of_memory", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_ext_benefit_mem = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "memory",
                "mssql.instance_memmgr_external_benefit_of_memory",
                "Performance benefit from adding memory to a specific cache",
                "bytes",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_MEMMGR_EXTERNAL_BENEFIT_OF_MEMORY,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_ext_benefit_mem = rrddim_add(mi->st_ext_benefit_mem, "benefit", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_ext_benefit_mem->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_ext_benefit_mem,
            mi->rd_ext_benefit_mem,
            (collected_number)mi->MSSQLExternalBenefitOfMemory.current.Data);
        rrdset_done(mi->st_ext_benefit_mem);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLPendingMemoryGrants)) {
        if (!mi->st_pending_mem_grant) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_memmgr_pending_memory_grants", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_pending_mem_grant = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "memory",
                "mssql.instance_memmgr_pending_memory_grants",
                "Process waiting for memory grant",
                "processes",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_MEMMGR_PENDING_MEMORY_GRANTS,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_pending_mem_grant =
                rrddim_add(mi->st_pending_mem_grant, "pending", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_pending_mem_grant->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_pending_mem_grant,
            mi->rd_pending_mem_grant,
            (collected_number)mi->MSSQLPendingMemoryGrants.current.Data);

        rrdset_done(mi->st_pending_mem_grant);
    }

    if (perflibGetObjectCounter(pDataBlock, pObjectType, &mi->MSSQLTotalServerMemory)) {
        if (!mi->st_mem_tot_server) {
            snprintfz(id, RRD_ID_LENGTH_MAX, "instance_%s_memmgr_server_memory", mi->instanceID);
            netdata_fix_chart_name(id);
            mi->st_mem_tot_server = rrdset_create_localhost(
                "mssql",
                id,
                NULL,
                "memory",
                "mssql.instance_memmgr_server_memory",
                "Memory committed",
                "bytes",
                PLUGIN_WINDOWS_NAME,
                "PerflibMSSQL",
                PRIO_MSSQL_MEMMGR_TOTAL_SERVER,
                update_every,
                RRDSET_TYPE_LINE);

            mi->rd_mem_tot_server = rrddim_add(mi->st_mem_tot_server, "memory", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);

            rrdlabels_add(mi->st_mem_tot_server->rrdlabels, "mssql_instance", mi->instanceID, RRDLABEL_SRC_AUTO);
        }

        rrddim_set_by_pointer(
            mi->st_mem_tot_server,
            mi->rd_mem_tot_server,
            (collected_number)(mi->MSSQLTotalServerMemory.current.Data * 1024));

        rrdset_done(mi->st_mem_tot_server);
    }
}

int dict_mssql_charts_cb(const DICTIONARY_ITEM *item __maybe_unused, void *value, void *data __maybe_unused)
{
    struct mssql_instance *mi = value;
    int *update_every = data;

    if (mi->conn.is_connected) {
        dictionary_sorted_walkthrough_read(mi->databases, dict_mssql_databases_run_query, NULL);
    }

    static void (*doMSSQL[])(PERF_DATA_BLOCK *, struct mssql_instance *, int) = {
        do_mssql_general_stats,
        do_mssql_errors,
        do_mssql_databases,
        do_mssql_locks,
        do_mssql_memory_mgr,
        do_mssql_buffer_management,
        do_mssql_sql_statistics,
        do_mssql_access_methods};

    DWORD i;
    for (i = 0; i < NETDATA_MSSQL_METRICS_END; i++) {
        if (!doMSSQL[i])
            continue;

        DWORD id = RegistryFindIDByName(mi->objectName[i]);
        if (id == PERFLIB_REGISTRY_NAME_NOT_FOUND)
            return -1;

        PERF_DATA_BLOCK *pDataBlock = perflibGetPerformanceData(id);
        if (!pDataBlock)
            return -1;

        doMSSQL[i](pDataBlock, mi, *update_every);
    }

    return 1;
}

// Entry point

int do_PerflibMSSQL(int update_every, usec_t dt __maybe_unused)
{
    static bool initialized = false;

    if (unlikely(!initialized)) {
        if (initialize())
            return -1;

        initialized = true;
    }

    dictionary_sorted_walkthrough_read(mssql_instances, dict_mssql_charts_cb, &update_every);

    return 0;
}
