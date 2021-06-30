// SPDX-License-Identifier: GPL-3.0-or-later

#include "sqlite_functions.h"

const char *database_config[] = {
    "PRAGMA auto_vacuum=incremental; PRAGMA synchronous=1 ; PRAGMA journal_mode=WAL; PRAGMA temp_store=MEMORY;",
    "PRAGMA journal_size_limit=16777216;",
    "CREATE TABLE IF NOT EXISTS host(host_id blob PRIMARY KEY, hostname text, "
    "registry_hostname text, update_every int, os text, timezone text, tags text);",
    "CREATE TABLE IF NOT EXISTS chart(chart_id blob PRIMARY KEY, host_id blob, type text, id text, name text, "
    "family text, context text, title text, unit text, plugin text, module text, priority int, update_every int, "
    "chart_type int, memory_mode int, history_entries);",
    "CREATE TABLE IF NOT EXISTS dimension(dim_id blob PRIMARY KEY, chart_id blob, id text, name text, "
    "multiplier int, divisor int , algorithm int, options text);",
    "CREATE TABLE IF NOT EXISTS chart_active(chart_id blob PRIMARY KEY, date_created int);",
    "CREATE TABLE IF NOT EXISTS dimension_active(dim_id blob primary key, date_created int);",
    "CREATE TABLE IF NOT EXISTS metadata_migration(filename text, file_size, date_created int);",
    "CREATE INDEX IF NOT EXISTS ind_d1 on dimension (chart_id, id, name);",
    "CREATE INDEX IF NOT EXISTS ind_c1 on chart (host_id, id, type, name);",
    "CREATE TABLE IF NOT EXISTS chart_label(chart_id blob, source_type int, label_key text, "
    "label_value text, date_created int, PRIMARY KEY (chart_id, label_key));",
    "CREATE TABLE IF NOT EXISTS node_instance (host_id blob PRIMARY KEY, claim_id, node_id, date_created);",
    
    "CREATE TABLE IF NOT EXISTS alert_hash(hash_id blob PRIMARY KEY, alarm text, template text, "
    "on_key text, class text, component text, type text, os text, hosts text, lookup text, every text, units text, calc text, families text, plugin text, module text, "
    "green text, red text, warn text, crit text, exec text, to_key text, "
    "info text, delay text, options text, repeat text, host_labels text);",
    
    "delete from chart_active;",
    "delete from dimension_active;",
    "delete from chart where chart_id not in (select chart_id from dimension);",
    "delete from host where host_id not in (select host_id from chart);",
    "delete from chart_label where chart_id not in (select chart_id from chart);",
    NULL
};

sqlite3 *db_meta = NULL;

static uv_mutex_t sqlite_transaction_lock;

static int execute_insert(sqlite3_stmt *res)
{
    int rc;

    while ((rc = sqlite3_step(res)) != SQLITE_DONE && unlikely(netdata_exit)) {
        if (likely(rc == SQLITE_BUSY || rc == SQLITE_LOCKED))
            usleep(SQLITE_INSERT_DELAY * USEC_PER_MS);
        else {
            error_report("SQLite error %d", rc);
            break;
        }
    }

    return rc;
}

#define MAX_OPEN_STATEMENTS (512)

static void add_stmt_to_list(sqlite3_stmt *res)
{
    static int idx = 0;
    static sqlite3_stmt *statements[MAX_OPEN_STATEMENTS];

    if (unlikely(!res)) {
        while (idx > 0)
            sqlite3_finalize(statements[--idx]);
        return;
    }

    if (unlikely(idx == MAX_OPEN_STATEMENTS))
        return;
    statements[idx++] = res;
}

static int prepare_statement(sqlite3 *database, char *query, sqlite3_stmt **statement) {
    int rc = sqlite3_prepare_v2(database, query, -1, statement, 0);
    if (likely(rc == SQLITE_OK))
        add_stmt_to_list(*statement);
    return rc;
}

/*
 * Store a chart or dimension UUID in  chart_active or dimension_active
 * The statement that will be prepared determines that
 */

static int store_active_uuid_object(sqlite3_stmt **res, char *statement, uuid_t *uuid)
{
    int rc;

    // Check if we should need to prepare the statement
    if (!*res) {
        rc = sqlite3_prepare_v2(db_meta, statement, -1, res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store active object, rc = %d", rc);
            return rc;
        }
    }

    rc = sqlite3_bind_blob(*res, 1, uuid, sizeof(*uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to bind input parameter to store active object, rc = %d", rc);
    else
        rc = execute_insert(*res);
    return rc;
}

/*
 * Marks a chart with UUID as active
 * Input: UUID
 */
void store_active_chart(uuid_t *chart_uuid)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    if (unlikely(!chart_uuid))
        return;

    rc = store_active_uuid_object(&res, SQL_STORE_ACTIVE_CHART, chart_uuid);
    if (rc != SQLITE_DONE)
        error_report("Failed to store active chart, rc = %d", rc);

    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize statement in store active chart, rc = %d", rc);
    return;
}

/*
 * Marks a dimension with UUID as active
 * Input: UUID
 */
void store_active_dimension(uuid_t *dimension_uuid)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    if (unlikely(!dimension_uuid))
        return;

    rc = store_active_uuid_object(&res, SQL_STORE_ACTIVE_DIMENSION, dimension_uuid);
    if (rc != SQLITE_DONE)
        error_report("Failed to store active dimension, rc = %d", rc);

    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize statement in store active dimension, rc = %d", rc);
    return;
}

/*
 * Initialize the SQLite database
 * Return 0 on success
 */
int sql_init_database(void)
{
    char *err_msg = NULL;
    char sqlite_database[FILENAME_MAX + 1];
    int rc;

    fatal_assert(0 == uv_mutex_init(&sqlite_transaction_lock));

    snprintfz(sqlite_database, FILENAME_MAX, "%s/netdata-meta.db", netdata_configured_cache_dir);
    rc = sqlite3_open(sqlite_database, &db_meta);
    if (rc != SQLITE_OK) {
        error_report("Failed to initialize database at %s, due to \"%s\"", sqlite_database, sqlite3_errstr(rc));
        sqlite3_close(db_meta);
        db_meta = NULL;
        return 1;
    }

    info("SQLite database %s initialization", sqlite_database);

    for (int i = 0; database_config[i]; i++) {
        debug(D_METADATALOG, "Executing %s", database_config[i]);
        rc = sqlite3_exec(db_meta, database_config[i], 0, 0, &err_msg);
        if (rc != SQLITE_OK) {
            error_report("SQLite error during database setup, rc = %d (%s)", rc, err_msg);
            error_report("SQLite failed statement %s", database_config[i]);
            sqlite3_free(err_msg);
            return 1;
        }
    }
    info("SQLite database initialization completed");
    return 0;
}

/*
 * Close the sqlite database
 */

void sql_close_database(void)
{
    int rc;
    if (unlikely(!db_meta))
        return;

    info("Closing SQLite database");

    add_stmt_to_list(NULL);

    rc = sqlite3_close_v2(db_meta);
    if (unlikely(rc != SQLITE_OK))
        error_report("Error %d while closing the SQLite database, %s", rc, sqlite3_errstr(rc));
    return;
}

#define FIND_UUID_TYPE  "select 1 from host where host_id = @uuid union select 2 from chart where chart_id = @uuid union select 3 from dimension where dim_id = @uuid;"

int find_uuid_type(uuid_t *uuid)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;
    int uuid_type = 3;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, FIND_UUID_TYPE, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to bind prepare statement to find UUID type in the database");
            return 0;
        }
    }

    rc = sqlite3_bind_blob(res, 1, uuid, sizeof(*uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW))
        uuid_type = sqlite3_column_int(res, 0);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement during find uuid type, rc = %d", rc);

    return uuid_type;

bind_fail:
    return 0;
}

int find_dimension_uuid(RRDSET *st, RRDDIM *rd, uuid_t *store_uuid)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;
    int status = 1;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return 1;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_FIND_DIMENSION_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to bind prepare statement to lookup dimension UUID in the database");
            return 1;
        }
    }

    rc = sqlite3_bind_blob(res, 1, st->chart_uuid, sizeof(*st->chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 2, rd->id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, rd->name, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        uuid_copy(*store_uuid, *((uuid_t *) sqlite3_column_blob(res, 0)));
        status = 0;
    }
    else {
        uuid_generate(*store_uuid);
        status = sql_store_dimension(store_uuid, st->chart_uuid, rd->id, rd->name, rd->multiplier, rd->divisor, rd->algorithm);
        if (unlikely(status))
            error_report("Failed to store dimension metadata in the database");
    }

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement find dimension uuid, rc = %d", rc);
    return status;

bind_fail:
    error_report("Failed to bind input parameter to perform dimension UUID database lookup, rc = %d", rc);
    return 1;
}

#define DELETE_DIMENSION_UUID   "delete from dimension where dim_id = @uuid;"

void delete_dimension_uuid(uuid_t *dimension_uuid)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

#ifdef NETDATA_INTERNAL_CHECKS
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*dimension_uuid, uuid_str);
    debug(D_METADATALOG,"Deleting dimension uuid %s", uuid_str);
#endif

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, DELETE_DIMENSION_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to prepare statement to delete a dimension uuid");
            return;
        }
    }

    rc = sqlite3_bind_blob(res, 1, dimension_uuid,  sizeof(*dimension_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to delete dimension uuid, rc = %d", rc);

bind_fail:
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when deleting dimension UUID, rc = %d", rc);
    return;
}

/*
 * Do a database lookup to find the UUID of a chart
 *
 */
uuid_t *find_chart_uuid(RRDHOST *host, const char *type, const char *id, const char *name)
{
    static __thread sqlite3_stmt *res = NULL;
    uuid_t *uuid = NULL;
    int rc;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return NULL;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_FIND_CHART_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to prepare statement to lookup chart UUID in the database");
            return NULL;
        }
    }

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
            goto bind_fail;

    rc = sqlite3_bind_text(res, 2, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 4, name ? name : id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        uuid = mallocz(sizeof(uuid_t));
        uuid_copy(*uuid, sqlite3_column_blob(res, 0));
    }

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when searching for a chart UUID, rc = %d", rc);

#ifdef NETDATA_INTERNAL_CHECKS
    char  uuid_str[GUID_LEN + 1];
    if (likely(uuid)) {
        uuid_unparse_lower(*uuid, uuid_str);
        debug(D_METADATALOG, "Found UUID %s for chart %s.%s", uuid_str, type, name ? name : id);
    }
    else
        debug(D_METADATALOG, "UUID not found for chart %s.%s", type, name ? name : id);
#endif
    return uuid;

bind_fail:
    error_report("Failed to bind input parameter to perform chart UUID database lookup, rc = %d", rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when searching for a chart UUID, rc = %d", rc);
    return NULL;
}

int update_chart_metadata(uuid_t *chart_uuid, RRDSET *st, const char *id, const char *name)
{
    int rc;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return 0;

    rc = sql_store_chart(
        chart_uuid, &st->rrdhost->host_uuid, st->type, id, name, st->family, st->context, st->title, st->units, st->plugin_name,
        st->module_name, st->priority, st->update_every, st->chart_type, st->rrd_memory_mode, st->entries);

    return rc;
}

uuid_t *create_chart_uuid(RRDSET *st, const char *id, const char *name)
{
    uuid_t *uuid = NULL;
    int rc;

    uuid = mallocz(sizeof(uuid_t));
    uuid_generate(*uuid);

#ifdef NETDATA_INTERNAL_CHECKS
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*uuid, uuid_str);
    debug(D_METADATALOG,"Generating uuid [%s] for chart %s under host %s", uuid_str, st->id, st->rrdhost->hostname);
#endif

    rc = update_chart_metadata(uuid, st, id, name);

    if (unlikely(rc))
        error_report("Failed to store chart metadata in the database");

    return uuid;
}

// Functions to create host, chart, dimension in the database

int sql_store_host(
    uuid_t *host_uuid, const char *hostname, const char *registry_hostname, int update_every, const char *os,
    const char *tzone, const char *tags)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely((!res))) {
        rc = prepare_statement(db_meta, SQL_STORE_HOST, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store host, rc = %d", rc);
            return 1;
        }
    }

    rc = sqlite3_bind_blob(res, 1, host_uuid, sizeof(*host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 2, hostname, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, registry_hostname, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 4, update_every);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 5, os, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 6, tzone, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 7, tags, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    int store_rc = sqlite3_step(res);
    if (unlikely(store_rc != SQLITE_DONE))
        error_report("Failed to store host %s, rc = %d", hostname, rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement to store host %s, rc = %d", hostname, rc);

    return !(store_rc == SQLITE_DONE);
bind_fail:
    error_report("Failed to bind parameter to store host %s, rc = %d", hostname, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement to store host %s, rc = %d", hostname, rc);
    return 1;
}

/*
 * Store a chart in the database
 */

int sql_store_chart(
    uuid_t *chart_uuid, uuid_t *host_uuid, const char *type, const char *id, const char *name, const char *family,
    const char *context, const char *title, const char *units, const char *plugin, const char *module, long priority,
    int update_every, int chart_type, int memory_mode, long history_entries)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc, param = 0;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_CHART, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store chart, rc = %d", rc);
            return 1;
        }
    }

    param++;
    rc = sqlite3_bind_blob(res, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_blob(res, 2, host_uuid, sizeof(*host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 3, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 4, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    if (name && *name)
        rc = sqlite3_bind_text(res, 5, name, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 5);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 6, family, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 7, context, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 8, title, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 9, units, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 10, plugin, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 11, module, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 12, priority);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 13, update_every);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 14, chart_type);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 15, memory_mode);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 16, history_entries);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store chart, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart store function, rc = %d", rc);

    return 0;

bind_fail:
    error_report("Failed to bind parameter %d to store chart, rc = %d", param, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart store function, rc = %d", rc);
    return 1;
}

/*
 * Store a dimension
 */
int sql_store_dimension(
    uuid_t *dim_uuid, uuid_t *chart_uuid, const char *id, const char *name, collected_number multiplier,
    collected_number divisor, int algorithm)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_DIMENSION, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store dimension, rc = %d", rc);
            return 1;
        }
    }

    rc = sqlite3_bind_blob(res, 1, dim_uuid, sizeof(*dim_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_blob(res, 2, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 4, name, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 5, multiplier);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 6, divisor);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 7, algorithm);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store dimension, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in store dimension, rc = %d", rc);
    return 0;

bind_fail:
    error_report("Failed to bind parameter to store dimension, rc = %d", rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in store dimension, rc = %d", rc);
    return 1;
}


//
// Support for archived charts
//
#define SELECT_DIMENSION "select d.id, d.name from dimension d where d.chart_id = @chart_uuid;"

void sql_rrdim2json(sqlite3_stmt *res_dim, uuid_t *chart_uuid, BUFFER *wb, size_t *dimensions_count)
{
    int rc;

    rc = sqlite3_bind_blob(res_dim, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (rc != SQLITE_OK)
        return;

    int dimensions = 0;
    buffer_sprintf(wb, "\t\t\t\"dimensions\": {\n");

    while (sqlite3_step(res_dim) == SQLITE_ROW) {
        if (dimensions)
            buffer_strcat(wb, ",\n\t\t\t\t\"");
        else
            buffer_strcat(wb, "\t\t\t\t\"");
        buffer_strcat_jsonescape(wb, (const char *) sqlite3_column_text(res_dim, 0));
        buffer_strcat(wb, "\": { \"name\": \"");
        buffer_strcat_jsonescape(wb, (const char *) sqlite3_column_text(res_dim, 1));
        buffer_strcat(wb, "\" }");
        dimensions++;
    }
    *dimensions_count += dimensions;
    buffer_sprintf(wb, "\n\t\t\t}");
}

#define SELECT_CHART "select chart_id, id, name, type, family, context, title, priority, plugin, " \
    "module, unit, chart_type, update_every from chart " \
    "where host_id = @host_uuid and chart_id not in (select chart_id from chart_active) order by chart_id asc;"

void sql_rrdset2json(RRDHOST *host, BUFFER *wb)
{
    //    time_t first_entry_t = 0; //= rrdset_first_entry_t(st);
    //   time_t last_entry_t = 0; //rrdset_last_entry_t(st);
    static char *custom_dashboard_info_js_filename = NULL;
    int rc;

    sqlite3_stmt *res_chart = NULL;
    sqlite3_stmt *res_dim = NULL;
    time_t now = now_realtime_sec();

    rc = sqlite3_prepare_v2(db_meta, SELECT_CHART, -1, &res_chart, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host archived charts");
        return;
    }

    rc = sqlite3_bind_blob(res_chart, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    rc = sqlite3_prepare_v2(db_meta, SELECT_DIMENSION, -1, &res_dim, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch chart archived dimensions");
        goto failed;
    };

    if(unlikely(!custom_dashboard_info_js_filename))
        custom_dashboard_info_js_filename = config_get(CONFIG_SECTION_WEB, "custom dashboard_info.js", "");

    buffer_sprintf(wb, "{\n"
                       "\t\"hostname\": \"%s\""
                       ",\n\t\"version\": \"%s\""
                       ",\n\t\"release_channel\": \"%s\""
                       ",\n\t\"os\": \"%s\""
                       ",\n\t\"timezone\": \"%s\""
                       ",\n\t\"update_every\": %d"
                       ",\n\t\"history\": %ld"
                       ",\n\t\"memory_mode\": \"%s\""
                       ",\n\t\"custom_info\": \"%s\""
                       ",\n\t\"charts\": {"
        , host->hostname
        , host->program_version
        , get_release_channel()
        , host->os
        , host->timezone
        , host->rrd_update_every
        , host->rrd_history_entries
        , rrd_memory_mode_name(host->rrd_memory_mode)
        , custom_dashboard_info_js_filename
    );

    size_t c = 0;
    size_t dimensions = 0;

    while (sqlite3_step(res_chart) == SQLITE_ROW) {
        char id[512];
        sprintf(id, "%s.%s", sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 1));
        RRDSET *st = rrdset_find(host, id);
        if (st && !rrdset_flag_check(st, RRDSET_FLAG_ARCHIVED))
            continue;

        if (c)
            buffer_strcat(wb, ",\n\t\t\"");
        else
            buffer_strcat(wb, "\n\t\t\"");
        c++;

        buffer_strcat(wb, id);
        buffer_strcat(wb, "\": ");

        buffer_sprintf(
            wb,
            "\t\t{\n"
            "\t\t\t\"id\": \"%s\",\n"
            "\t\t\t\"name\": \"%s\",\n"
            "\t\t\t\"type\": \"%s\",\n"
            "\t\t\t\"family\": \"%s\",\n"
            "\t\t\t\"context\": \"%s\",\n"
            "\t\t\t\"title\": \"%s (%s)\",\n"
            "\t\t\t\"priority\": %ld,\n"
            "\t\t\t\"plugin\": \"%s\",\n"
            "\t\t\t\"module\": \"%s\",\n"
            "\t\t\t\"enabled\": %s,\n"
            "\t\t\t\"units\": \"%s\",\n"
            "\t\t\t\"data_url\": \"/api/v1/data?chart=%s\",\n"
            "\t\t\t\"chart_type\": \"%s\",\n",
            id //sqlite3_column_text(res_chart, 1)
            ,
            id // sqlite3_column_text(res_chart, 2)
            ,
            sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 4), sqlite3_column_text(res_chart, 5),
            sqlite3_column_text(res_chart, 6), id //sqlite3_column_text(res_chart, 2)
            ,
            (long ) sqlite3_column_int(res_chart, 7),
            (const char *) sqlite3_column_text(res_chart, 8) ? (const char *) sqlite3_column_text(res_chart, 8) : (char *) "",
            (const char *) sqlite3_column_text(res_chart, 9) ? (const char *) sqlite3_column_text(res_chart, 9) : (char *) "", (char *) "false",
            (const char *) sqlite3_column_text(res_chart, 10), id //sqlite3_column_text(res_chart, 2)
            ,
            rrdset_type_name(sqlite3_column_int(res_chart, 11)));

        sql_rrdim2json(res_dim, (uuid_t *) sqlite3_column_blob(res_chart, 0), wb, &dimensions);

        rc = sqlite3_reset(res_dim);
        if (unlikely(rc != SQLITE_OK))
            error_report("Failed to reset the prepared statement when reading archived chart dimensions");
        buffer_strcat(wb, "\n\t\t}");
    }

    buffer_sprintf(wb
        , "\n\t}"
          ",\n\t\"charts_count\": %zu"
          ",\n\t\"dimensions_count\": %zu"
          ",\n\t\"alarms_count\": %zu"
          ",\n\t\"rrd_memory_bytes\": %zu"
          ",\n\t\"hosts_count\": %zu"
          ",\n\t\"hosts\": ["
        , c
        , dimensions
        , (size_t) 0
        , (size_t) 0
        , rrd_hosts_available
    );

    if(unlikely(rrd_hosts_available > 1)) {
        rrd_rdlock();

        size_t found = 0;
        RRDHOST *h;
        rrdhost_foreach_read(h) {
            if(!rrdhost_should_be_removed(h, host, now) && !rrdhost_flag_check(h, RRDHOST_FLAG_ARCHIVED)) {
                buffer_sprintf(wb
                    , "%s\n\t\t{"
                      "\n\t\t\t\"hostname\": \"%s\""
                      "\n\t\t}"
                    , (found > 0) ? "," : ""
                    , h->hostname
                );

                found++;
            }
        }

        rrd_unlock();
    }
    else {
        buffer_sprintf(wb
            , "\n\t\t{"
              "\n\t\t\t\"hostname\": \"%s\""
              "\n\t\t}"
            , host->hostname
        );
    }

    buffer_sprintf(wb, "\n\t]\n}\n");

    rc = sqlite3_finalize(res_dim);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived chart dimensions");

failed:
    rc = sqlite3_finalize(res_chart);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived charts");

    return;
}

#define SELECT_HOST "select host_id, registry_hostname, update_every, os, timezone, tags from host where hostname = @hostname order by rowid desc;"
#define SELECT_HOST_BY_UUID "select host_id, registry_hostname, update_every, os, timezone, tags from host where host_id = @host_id ;"

RRDHOST *sql_create_host_by_uuid(char *hostname)
{
    int rc;
    RRDHOST *host = NULL;
    uuid_t host_uuid;

    sqlite3_stmt *res = NULL;

    rc = uuid_parse(hostname, host_uuid);
    if (!rc) {
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST_BY_UUID, -1, &res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to fetch host by uuid");
            return NULL;
        }
        rc = sqlite3_bind_blob(res, 1, &host_uuid, sizeof(host_uuid), SQLITE_STATIC);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to bind host_id parameter to fetch host information");
            goto failed;
        }
    }
    else {
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST, -1, &res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to fetch host by hostname");
            return NULL;
        }
        rc = sqlite3_bind_text(res, 1, hostname, -1, SQLITE_STATIC);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to bind hostname parameter to fetch host information");
            goto failed;
        }
    }

    rc = sqlite3_step(res);
    if (unlikely(rc != SQLITE_ROW)) {
        error_report("Failed to find hostname %s", hostname);
        goto failed;
    }

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*((uuid_t *) sqlite3_column_blob(res, 0)), uuid_str);

    host = callocz(1, sizeof(RRDHOST));

    set_host_properties(host, sqlite3_column_int(res, 2), RRD_MEMORY_MODE_DBENGINE, hostname,
                        (char *) sqlite3_column_text(res, 1), (const char *) uuid_str,
                        (char *) sqlite3_column_text(res, 3), (char *) sqlite3_column_text(res, 5),
                        (char *) sqlite3_column_text(res, 4), NULL, 0, NULL, NULL);

    uuid_copy(host->host_uuid, *((uuid_t *) sqlite3_column_blob(res, 0)));

    host->system_info = callocz(1, sizeof(*host->system_info));;
    rrdhost_flag_set(host, RRDHOST_FLAG_ARCHIVED);
#ifdef ENABLE_DBENGINE
    host->rrdeng_ctx = &multidb_ctx;
#endif

failed:
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading host information");

    return host;
}

void db_execute(char *cmd)
{
    int rc;
    char *err_msg;
    rc = sqlite3_exec(db_meta, cmd, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        error_report("Failed to execute '%s', rc = %d (%s)", cmd, rc, err_msg);
        sqlite3_free(err_msg);
    }

    return;
}

void db_lock(void)
{
    uv_mutex_lock(&sqlite_transaction_lock);
    return;
}

void db_unlock(void)
{
    uv_mutex_unlock(&sqlite_transaction_lock);
    return;
}


#define SELECT_MIGRATED_FILE    "select 1 from metadata_migration where filename = @path;"

int file_is_migrated(char *path)
{
    sqlite3_stmt *res = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db_meta, SELECT_MIGRATED_FILE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host");
        return 0;
    }

    rc = sqlite3_bind_text(res, 1, path, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind filename parameter to check migration");
        return 0;
    }

    rc = sqlite3_step(res);

    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when checking if metadata file is migrated");

    return (rc == SQLITE_ROW);
}

#define STORE_MIGRATED_FILE    "insert or replace into metadata_migration (filename, file_size, date_created) " \
                                "values (@file, @size, strftime('%s'));"

void add_migrated_file(char *path, uint64_t file_size)
{
    sqlite3_stmt *res = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db_meta, STORE_MIGRATED_FILE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host");
        return;
    }

    rc = sqlite3_bind_text(res, 1, path, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind filename parameter to store migration information");
        return;
    }

    rc = sqlite3_bind_int64(res, 2, file_size);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind size parameter to store migration information");
        return;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store migrated file, rc = %d", rc);

    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when checking if metadata file is migrated");

    return;
}

#define SQL_INS_CHART_LABEL "insert or replace into chart_label " \
    "(chart_id, source_type, label_key, label_value, date_created) " \
    "values (@chart, @source, @label, @value, strftime('%s'));"

void sql_store_chart_label(uuid_t *chart_uuid, int source_type, char *label, char *value)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_INS_CHART_LABEL, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind chart_id parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, source_type);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind type parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 3, label, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind label parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 4, value, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind value parameter to store label information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store chart label entry, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing chart label information");

    return;
}

int find_dimension_first_last_t(char *machine_guid, char *chart_id, char *dim_id,
                                uuid_t *uuid, time_t *first_entry_t, time_t *last_entry_t, uuid_t *rrdeng_uuid)
{
#ifdef ENABLE_DBENGINE
    int rc;
    uuid_t  legacy_uuid;
    uuid_t  multihost_legacy_uuid;
    time_t dim_first_entry_t, dim_last_entry_t;

    rc = rrdeng_metric_latest_time_by_uuid(uuid, &dim_first_entry_t, &dim_last_entry_t);
    if (unlikely(rc)) {
        rrdeng_generate_legacy_uuid(dim_id, chart_id, &legacy_uuid);
        rc = rrdeng_metric_latest_time_by_uuid(&legacy_uuid, &dim_first_entry_t, &dim_last_entry_t);
        if (likely(rc)) {
            rrdeng_convert_legacy_uuid_to_multihost(machine_guid, &legacy_uuid, &multihost_legacy_uuid);
            rc = rrdeng_metric_latest_time_by_uuid(&multihost_legacy_uuid, &dim_first_entry_t, &dim_last_entry_t);
            if (likely(!rc))
                uuid_copy(*rrdeng_uuid, multihost_legacy_uuid);
        }
        else
            uuid_copy(*rrdeng_uuid, legacy_uuid);
    }
    else
        uuid_copy(*rrdeng_uuid, *uuid);

    if (likely(!rc)) {
        *first_entry_t = MIN(*first_entry_t, dim_first_entry_t);
        *last_entry_t = MAX(*last_entry_t, dim_last_entry_t);
    }
    return rc;
#else
    UNUSED(machine_guid);
    UNUSED(chart_id);
    UNUSED(dim_id);
    UNUSED(uuid);
    UNUSED(first_entry_t);
    UNUSED(last_entry_t);
    UNUSED(rrdeng_uuid);
    return 1;
#endif
}

#ifdef ENABLE_DBENGINE
static RRDDIM *create_rrdim_entry(RRDSET *st, char *id, char *name, uuid_t *metric_uuid)
{
    RRDDIM *rd = callocz(1, sizeof(*rd));
    rd->rrdset = st;
    rd->last_stored_value = NAN;
    rrddim_flag_set(rd, RRDDIM_FLAG_NONE);
    rd->state = mallocz(sizeof(*rd->state));
    rd->rrd_memory_mode = RRD_MEMORY_MODE_DBENGINE;
    rd->state->query_ops.init = rrdeng_load_metric_init;
    rd->state->query_ops.next_metric = rrdeng_load_metric_next;
    rd->state->query_ops.is_finished = rrdeng_load_metric_is_finished;
    rd->state->query_ops.finalize = rrdeng_load_metric_finalize;
    rd->state->query_ops.latest_time = rrdeng_metric_latest_time;
    rd->state->query_ops.oldest_time = rrdeng_metric_oldest_time;
    rd->state->rrdeng_uuid = mallocz(sizeof(uuid_t));
    uuid_copy(*rd->state->rrdeng_uuid, *metric_uuid);
    uuid_copy(rd->state->metric_uuid, *metric_uuid);
    rd->id = strdupz(id);
    rd->name = strdupz(name);
    return rd;
}
#endif

#define SELECT_CHART_CONTEXT  "select d.dim_id, d.id, d.name, c.id, c.type, c.name, c.update_every, c.chart_id from chart c, " \
    "dimension d, host h " \
    "where d.chart_id = c.chart_id and c.host_id = h.host_id and c.host_id = @host_id and c.context = @context " \
    "order by c.chart_id asc, c.type||c.id desc;"

#define SELECT_CHART_SINGLE  "select d.dim_id, d.id, d.name, c.id, c.type, c.name, c.update_every, c.chart_id, c.context from chart c, " \
    "dimension d, host h " \
    "where d.chart_id = c.chart_id and c.host_id = h.host_id and c.host_id = @host_id and c.type||'.'||c.id = @chart " \
    "order by c.chart_id asc, c.type||'.'||c.id desc;"

void sql_build_context_param_list(struct context_param **param_list, RRDHOST *host, char *context, char *chart)
{
#ifdef ENABLE_DBENGINE
    int rc;

    if (unlikely(!param_list) || host->rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return;

    if (unlikely(!(*param_list))) {
        *param_list = mallocz(sizeof(struct context_param));
        (*param_list)->first_entry_t = LONG_MAX;
        (*param_list)->last_entry_t = 0;
        (*param_list)->rd = NULL;
        (*param_list)->flags = CONTEXT_FLAGS_ARCHIVE;
        if (chart)
            (*param_list)->flags |= CONTEXT_FLAGS_CHART;
        else
            (*param_list)->flags |= CONTEXT_FLAGS_CONTEXT;
    }

    sqlite3_stmt *res = NULL;

    if (context)
        rc = sqlite3_prepare_v2(db_meta, SELECT_CHART_CONTEXT, -1, &res, 0);
    else
        rc = sqlite3_prepare_v2(db_meta, SELECT_CHART_SINGLE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host archived charts");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    if (context)
        rc = sqlite3_bind_text(res, 2, context, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_text(res, 2, chart, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    RRDSET *st = NULL;
    char machine_guid[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, machine_guid);
    uuid_t rrdeng_uuid;
    uuid_t chart_id;

    while (sqlite3_step(res) == SQLITE_ROW) {
        char id[512];
        sprintf(id, "%s.%s", sqlite3_column_text(res, 3), sqlite3_column_text(res, 1));

        if (!st || uuid_compare(*(uuid_t *)sqlite3_column_blob(res, 7), chart_id)) {
            if (unlikely(st && !st->counter)) {
                freez(st->context);
                freez((char *) st->name);
                freez(st);
            }
            st = callocz(1, sizeof(*st));
            char n[RRD_ID_LENGTH_MAX + 1];

            snprintfz(
                n, RRD_ID_LENGTH_MAX, "%s.%s", (char *)sqlite3_column_text(res, 4),
                (char *)sqlite3_column_text(res, 3));
            st->name = strdupz(n);
            st->update_every = sqlite3_column_int(res, 6);
            st->counter = 0;
            if (chart) {
                st->context = strdupz((char *)sqlite3_column_text(res, 8));
                strncpyz(st->id, chart, RRD_ID_LENGTH_MAX);
            }
            uuid_copy(chart_id, *(uuid_t *)sqlite3_column_blob(res, 7));
            st->last_entry_t = 0;
            st->rrdhost = host;
        }

        if (unlikely(find_dimension_first_last_t(machine_guid, (char *)st->name, (char *)sqlite3_column_text(res, 1),
                (uuid_t *)sqlite3_column_blob(res, 0), &(*param_list)->first_entry_t, &(*param_list)->last_entry_t,
                &rrdeng_uuid)))
            continue;

        st->counter++;
        st->last_entry_t = MAX(st->last_entry_t, (*param_list)->last_entry_t);

        RRDDIM *rd = create_rrdim_entry(st, (char *)sqlite3_column_text(res, 1), (char *)sqlite3_column_text(res, 2), &rrdeng_uuid);
        rd->next = (*param_list)->rd;
        (*param_list)->rd = rd;
    }
    if (st) {
        if (!st->counter) {
            freez(st->context);
            freez((char *)st->name);
            freez(st);
        }
        else
            if (!st->context && context)
                st->context = strdupz(context);
    }

failed:
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived charts");
#else
    UNUSED(param_list);
    UNUSED(host);
    UNUSED(context);
    UNUSED(chart);
#endif
    return;
}

#define SQL_STORE_CLAIM_ID  "insert into node_instance " \
    "(host_id, claim_id, date_created) values (@host_id, @claim_id, strftime('%s')) " \
    "on conflict(host_id) do update set claim_id = excluded.claim_id;"

void store_claim_id(uuid_t *host_id, uuid_t *claim_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_STORE_CLAIM_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    if (claim_id)
        rc = sqlite3_bind_blob(res, 2, claim_id, sizeof(*claim_id), SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 2);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind claim_id parameter to store node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store node instance information, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return;
}

static inline void set_host_node_id(RRDHOST *host, uuid_t *node_id)
{
    if (unlikely(!host))
        return;

    if (unlikely(!node_id)) {
        freez(host->node_id);
        host->node_id = NULL;
        return;
    }

    if (unlikely(!host->node_id))
        host->node_id = mallocz(sizeof(*host->node_id));
    uuid_copy(*(host->node_id), *node_id);
    return;
}

#define SQL_UPDATE_NODE_ID  "update node_instance set node_id = @node_id where host_id = @host_id;"

int update_node_id(uuid_t *host_id, uuid_t *node_id)
{
    sqlite3_stmt *res = NULL;
    RRDHOST *host = NULL;
    int rc = 2;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_UPDATE_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to store node instance information");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, node_id, sizeof(*node_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = sqlite3_bind_blob(res, 2, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store node instance information, rc = %d", rc);
    rc = sqlite3_changes(db_meta);

    char host_guid[GUID_LEN + 1];
    uuid_unparse_lower(*host_id, host_guid);
    rrd_wrlock();
    host = rrdhost_find_by_guid(host_guid, 0);
    if (likely(host))
            set_host_node_id(host, node_id);
    rrd_unlock();

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return rc - 1;
}

#define SQL_SELECT_HOST_BY_NODE_ID  "select host_id from node_instance where node_id = @node_id;"

int get_host_id(uuid_t *node_id, uuid_t *host_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_HOST_BY_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to select node instance information for a node");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, node_id, sizeof(*node_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to select node instance information");
        goto failed;
    }

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW && host_id))
        uuid_copy(*host_id, *((uuid_t *) sqlite3_column_blob(res, 0)));

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when selecting node instance information");

    return (rc == SQLITE_ROW) ? 0 : -1;
}

#define SQL_SELECT_NODE_ID  "select node_id from node_instance where host_id = @host_id and node_id not null;"

int get_node_id(uuid_t *host_id, uuid_t *node_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to select node instance information for a host");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to select node instance information");
        goto failed;
    }

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW && node_id))
        uuid_copy(*node_id, *((uuid_t *) sqlite3_column_blob(res, 0)));

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when selecting node instance information");

    return (rc == SQLITE_ROW) ? 0 : -1;
}

#define SQL_INVALIDATE_NODE_INSTANCES "update node_instance set node_id = NULL where exists " \
    "(select host_id from node_instance where host_id = @host_id and (@claim_id is null or claim_id <> @claim_id));"

void invalidate_node_instances(uuid_t *host_id, uuid_t *claim_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_INVALIDATE_NODE_INSTANCES, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to invalidate node instance ids");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to invalidate node instance information");
        goto failed;
    }

    if (claim_id)
        rc = sqlite3_bind_blob(res, 2, claim_id, sizeof(*claim_id), SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 2);

    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind claim_id parameter to invalidate node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to invalidate node instance information, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when invalidating node instance information");
}

#define SQL_GET_NODE_INSTANCE_LIST "select ni.node_id, ni.host_id, h.hostname " \
    "from node_instance ni, host h where ni.host_id = h.host_id;"

struct  node_instance_list *get_node_list(void)
{
    struct  node_instance_list *node_list = NULL;
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return NULL;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_GET_NODE_INSTANCE_LIST, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return NULL;
    };

    int row = 0;
    char host_guid[37];
    while (sqlite3_step(res) == SQLITE_ROW)
        row++;

    if (sqlite3_reset(res) != SQLITE_OK) {
        error_report("Failed to reset the prepared statement fetching storing node instance information");
        goto failed;
    }
    node_list = callocz(row + 1, sizeof(*node_list));
    int max_rows = row;
    row = 0;
    while (sqlite3_step(res) == SQLITE_ROW) {
        if (sqlite3_column_bytes(res, 0) == sizeof(uuid_t))
            uuid_copy(node_list[row].node_id, *((uuid_t *)sqlite3_column_blob(res, 0)));
        if (sqlite3_column_bytes(res, 1) == sizeof(uuid_t)) {
            uuid_t *host_id = (uuid_t *)sqlite3_column_blob(res, 1);
            uuid_copy(node_list[row].host_id, *host_id);
            node_list[row].querable = 1;
            uuid_unparse_lower(*host_id, host_guid);
            node_list[row].live = rrdhost_find_by_guid(host_guid, 0) ? 1 : 0;
            node_list[row].hops = uuid_compare(*host_id, localhost->host_uuid) ? 1 : 0;
            node_list[row].hostname =
                sqlite3_column_bytes(res, 2) ? strdupz((char *)sqlite3_column_text(res, 2)) : NULL;
        }
        row++;
        if (row == max_rows)
            break;
    }

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return node_list;
};

#define SQL_GET_HOST_NODE_ID "select node_id from node_instance where host_id = @host_id;"

void sql_load_node_id(RRDHOST *host)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_GET_HOST_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return;
    };

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        if (likely(sqlite3_column_bytes(res, 0) == sizeof(uuid_t)))
            set_host_node_id(host, (uuid_t *)sqlite3_column_blob(res, 0));
        else
            set_host_node_id(host, NULL);
    }

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return;
};

#define MAX_HEALTH_SQL_SIZE 2048

/* Health related SQL queries
   Creates a health log table in sqlite, one per host guid
*/
#define SQL_CREATE_HEALTH_LOG_TABLE(guid) "CREATE TABLE IF NOT EXISTS health_log_%s(hostname text, unique_id int, alarm_id int, alarm_event_id int, config_hash_id blob, updated_by_id int, updates_id int, when_key int, duration int, non_clear_duration int, flags int, exec_run_timestamp int, delay_up_to_timestamp int, name text, chart text, family text, exec text, recipient text, source text, units text, info text, exec_code int, new_status real, old_status real, delay int, new_value double, old_value double, last_repeat int, class text, component text, type text);", guid
int sql_create_health_log_table(RRDHOST *host) {
    int rc;
    char *err_msg = NULL, command[MAX_HEALTH_SQL_SIZE + 1];

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    snprintfz(command, MAX_HEALTH_SQL_SIZE, SQL_CREATE_HEALTH_LOG_TABLE(uuid_str));

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("HEALTH [%s]: Database has not been initialized", host->hostname);
        return 1;
    }

    rc = sqlite3_exec(db_meta, command, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        error_report("HEALTH [%s]: SQLite error during creation of health log table, rc = %d (%s)", host->hostname, rc, err_msg);
        sqlite3_free(err_msg);
        return 1;
    }

    return 0;
}

/* Health related SQL queries
   Updates an entry in the table
*/
#define SQL_UPDATE_HEALTH_LOG(guid) "UPDATE health_log_%s set updated_by_id = ?, flags = ?, exec_run_timestamp = ?, exec_code = ? where unique_id = ?;", guid
void sql_health_alarm_log_update(RRDHOST *host, ALARM_ENTRY *ae) {
    sqlite3_stmt *res = NULL;
    int rc;
    char command[MAX_HEALTH_SQL_SIZE + 1];

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("HEALTH [%s]: Database has not been initialized", host->hostname);
        return;
    }

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    sprintf(command, SQL_UPDATE_HEALTH_LOG(uuid_str));

    rc = sqlite3_prepare_v2(db_meta, command, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("HEALTH [%s]: Failed to prepare statement for SQL_UPDATE_HEALTH_LOG", host->hostname);
        goto failed;
    }

    rc = sqlite3_bind_int(res, 1, ae->updated_by_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind updated_by_id parameter for SQL_UPDATE_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, ae->flags);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind flags parameter for SQL_UPDATE_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 3, ae->exec_run_timestamp);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind exec_run_timestamp parameter for SQL_UPDATE_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 4, ae->exec_code);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind exec_code parameter for SQL_UPDATE_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 5, ae->unique_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind unique_id parameter for SQL_UPDATE_HEALTH_LOG");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE)) {
        error_report("HEALTH [%s]: Failed to update health log, rc = %d", host->hostname, rc);
    }

 failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("HEALTH [%s]: Failed to finalize the prepared statement for updating health log.", host->hostname);

    return;
}

/* Health related SQL queries
   Inserts an entry in the table
*/
#define SQL_INSERT_HEALTH_LOG(guid) "INSERT INTO health_log_%s(hostname, unique_id, alarm_id, alarm_event_id, config_hash_id, updated_by_id, updates_id, when_key, duration, non_clear_duration, flags, exec_run_timestamp, delay_up_to_timestamp, name, chart, family, exec, recipient, source, units, info, exec_code, new_status, old_status, delay, new_value, old_value, last_repeat, class, component, type) values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", guid
void sql_health_alarm_log_insert(RRDHOST *host, ALARM_ENTRY *ae) {
    sqlite3_stmt *res = NULL;
    int rc;
    char command[MAX_HEALTH_SQL_SIZE + 1];
    
    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("HEALTH [%s]: Database has not been initialized", host->hostname);
        return;
    }

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    snprintfz(command, MAX_HEALTH_SQL_SIZE, SQL_INSERT_HEALTH_LOG(uuid_str));

    rc = sqlite3_prepare_v2(db_meta, command, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("HEALTH [%s]: Failed to prepare statement for SQL_INSERT_HEALTH_LOG", host->hostname);
        return;
    }

    rc = sqlite3_bind_text(res, 1, host->hostname, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind hostname parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, ae->unique_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind unique_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 3, ae->alarm_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind alarm_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 4, ae->alarm_event_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind alarm_event_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_null(res, 5);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind config_hash_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 6, ae->updated_by_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind updated_by_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 7, ae->updates_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind updates_id parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 8, ae->when);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind when parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 9, ae->duration);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind duration parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 10, ae->non_clear_duration);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind non_clear_duration parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 11, ae->flags);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind flags parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 12, ae->exec_run_timestamp);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind exec_run_timestamp parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 13, ae->delay_up_to_timestamp);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind delay_up_to_timestamp parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 14, ae->name, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind name parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 15, ae->chart, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind chart parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 16, ae->family, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind family parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 17, ae->exec, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind exec parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 18, ae->recipient, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind recipient parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 19, ae->source, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind source parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 20, ae->units, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 21, ae->info, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind info parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 22, ae->exec_code);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind exec_code parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 23, ae->new_status);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind new_status parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 24, ae->old_status);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind old_status parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 25, ae->delay);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind delay parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_double(res, 26, ae->new_value);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind new_value parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_double(res, 27, ae->old_value);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind old_value parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 28, ae->last_repeat);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind last_repeat parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 29, ae->classification, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind classification parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 30, ae->component, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind component parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 31, ae->type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind type parameter for SQL_INSERT_HEALTH_LOG");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE)) {
        error_report("HEALTH [%s]: Failed to execute SQL_INSERT_HEALTH_LOG, rc = %d", host->hostname, rc);
        goto failed;
    }

    ae->flags |= HEALTH_ENTRY_FLAG_SAVED;
    host->health_log_entries_written++;

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("HEALTH [%s]: Failed to finalize the prepared statement for inserting to health log.", host->hostname);

  return;
}

void sql_health_alarm_log_save(RRDHOST *host, ALARM_ENTRY *ae)
{
    if (ae->flags & HEALTH_ENTRY_FLAG_SAVED)
        sql_health_alarm_log_update(host, ae);
    else
        sql_health_alarm_log_insert(host, ae);
}

static uint32_t is_valid_alarm_id(RRDHOST *host, const char *chart, const char *name, uint32_t alarm_id)
{
    uint32_t hash_chart = simple_hash(chart);
    uint32_t hash_name = simple_hash(name);

    ALARM_ENTRY *ae;
    for(ae = host->health_log.alarms; ae ;ae = ae->next) {
        if (unlikely(
                ae->alarm_id == alarm_id && (!(ae->hash_name == hash_name && ae->hash_chart == hash_chart &&
                                               !strcmp(name, ae->name) && !strcmp(chart, ae->chart))))) {
            return 0;
        }
    }
    return 1;
}

/* Health related SQL queries
   Cleans up the health_log table.
*/
#define SQL_CLEANUP_HEALTH_LOG(guid,guid2,limit) "DELETE from health_log_%s where unique_id in (SELECT unique_id from health_log_%s order by unique_id asc LIMIT %ld);", guid, guid2, limit
void sql_health_alarm_log_cleanup(RRDHOST *host) {
    sqlite3_stmt *res = NULL;
    static size_t rotate_every = 0;
    int rc;
    char command[MAX_HEALTH_SQL_SIZE + 1];

    if(unlikely(rotate_every == 0)) {
        rotate_every = (size_t)config_get_number(CONFIG_SECTION_HEALTH, "rotate log every lines", 2000);
        if(rotate_every < 100) rotate_every = 100;
    }

    if(likely(host->health_log_entries_written < rotate_every)) {
        return;
    }

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }
    
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    snprintfz(command, MAX_HEALTH_SQL_SIZE, SQL_CLEANUP_HEALTH_LOG(uuid_str, uuid_str, host->health_log_entries_written - rotate_every));

    rc = sqlite3_prepare_v2(db_meta, command, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to cleanup health log table");
        return;
    }

    rc = sqlite3_step(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to cleanup health log table, rc = %d", rc);

    host->health_log_entries_written = rotate_every;
}

/* Health related SQL queries
   Get a count of rows from health log table
*/
#define SQL_COUNT_HEALTH_LOG(guid) "SELECT count(1) FROM health_log_%s;", guid
void sql_health_alarm_log_count(RRDHOST *host) {
    sqlite3_stmt *res = NULL;
    int rc;
    char command[MAX_HEALTH_SQL_SIZE + 1];

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    snprintfz(command, MAX_HEALTH_SQL_SIZE, SQL_COUNT_HEALTH_LOG(uuid_str));

    rc = sqlite3_prepare_v2(db_meta, command, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to count health log entries from db");
        return;
    }
    rc = sqlite3_step(res);

    host->health_log_entries_written = sqlite3_column_int(res, 0);
    
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement to count health log entries from db");
}

/* Health related SQL queries
   Load from the health log table
*/
#define SQL_LOAD_HEALTH_LOG(guid,limit) "SELECT hostname, unique_id, alarm_id, alarm_event_id, config_hash_id, updated_by_id, updates_id, when_key, duration, non_clear_duration, flags, exec_run_timestamp, delay_up_to_timestamp, name, chart, family, exec, recipient, source, units, info, exec_code, new_status, old_status, delay, new_value, old_value, last_repeat, class, component, type FROM (SELECT hostname, unique_id, alarm_id, alarm_event_id, config_hash_id, updated_by_id, updates_id, when_key, duration, non_clear_duration, flags, exec_run_timestamp, delay_up_to_timestamp, name, chart, family, exec, recipient, source, units, info, exec_code, new_status, old_status, delay, new_value, old_value, last_repeat, class, component, type FROM health_log_%s order by unique_id desc limit %d) order by unique_id asc;", guid, limit
void sql_health_alarm_log_load(RRDHOST *host) {
    sqlite3_stmt *res = NULL;
    int rc;
    char command[MAX_HEALTH_SQL_SIZE + 1];

    host->health_log_entries_written = 0;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("HEALTH [%s]: Database has not been initialized", host->hostname);
        return;
    }

    netdata_rwlock_rdlock(&host->health_log.alarm_log_rwlock);

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, uuid_str);
    uuid_str[8] = '_';
    uuid_str[13] = '_';
    uuid_str[18] = '_';
    uuid_str[23] = '_';

    snprintfz(command, MAX_HEALTH_SQL_SIZE, SQL_LOAD_HEALTH_LOG(uuid_str, host->health_log.max));

    rc = sqlite3_prepare_v2(db_meta, command, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("HEALTH [%s]: Failed to prepare sql statement to load health log.", host->hostname);
        return;
    }

    while (sqlite3_step(res) == SQLITE_ROW) {
        errno = 0;
        ssize_t errored = 0;

        ALARM_ENTRY *ae = NULL;

        // check that we have valid ids
        uint32_t unique_id = sqlite3_column_int(res, 1);
        if(!unique_id) {
            error_report("HEALTH [%s]: Got invalid unique id. Ignoring it.", host->hostname);
            errored++;
            continue;
        }

        uint32_t alarm_id = sqlite3_column_int(res, 2);
        if(!alarm_id) {
            error_report("HEALTH [%s]: Got invalid alarm id. Ignoring it.", host->hostname);
            errored++;
            continue;
        }

        // Check if we got last_repeat field
        time_t last_repeat = 0;
        char* alarm_name = strdupz((char *) sqlite3_column_text(res, 14));
        last_repeat = (time_t)sqlite3_column_int(res, 27);

        RRDCALC *rc = alarm_max_last_repeat(host, alarm_name,simple_hash(alarm_name));
        if (!rc) {
            for(rc = host->alarms; rc ; rc = rc->next) {
                RRDCALC *rdcmp  = (RRDCALC *) avl_insert_lock(&(host)->alarms_idx_name, (avl_t *)rc);
                if(rdcmp != rc) {
                    error("Cannot insert the alarm index ID using log %s", rc->name);
                }
            }

            rc = alarm_max_last_repeat(host, alarm_name,simple_hash(alarm_name));
        }

        if(unlikely(rc)) {
            if (rrdcalc_isrepeating(rc)) {
                rc->last_repeat = last_repeat;
                // We iterate through repeating alarm entries only to
                // find the latest last_repeat timestamp. Otherwise,
                // there is no need to keep them in memory.
                continue;
            }
        }

        freez(alarm_name);
    
        // check for a possible host mismatch
        if(strcmp((char *) sqlite3_column_text(res, 13), host->hostname)) {
            error("HEALTH [%s]: Unique id %u provides an alarm for host '%s' but this is named '%s'. Ignoring it.", host->hostname, unique_id, (char *) sqlite3_column_text(res, 13), host->hostname);
            errored++;
            continue;
        }

        ae = callocz(1, sizeof(ALARM_ENTRY));

        ae->unique_id = unique_id;
        
        if (!is_valid_alarm_id(host, (const char *) sqlite3_column_text(res, 14), (const char *) sqlite3_column_text(res, 13), alarm_id))
            alarm_id = rrdcalc_get_unique_id(host, (const char *) sqlite3_column_text(res, 14), (const char *) sqlite3_column_text(res, 13), NULL);
        ae->alarm_id = alarm_id;

        ae->alarm_event_id = sqlite3_column_int(res, 3);
        ae->updated_by_id = sqlite3_column_int(res, 5);
        ae->updates_id = sqlite3_column_int(res, 6);

        ae->when = sqlite3_column_int(res, 7);
        ae->duration = sqlite3_column_int(res, 8);
        ae->non_clear_duration = sqlite3_column_int(res, 9);

        ae->flags = sqlite3_column_int(res, 10);
        ae->flags |= HEALTH_ENTRY_FLAG_SAVED;

        ae->exec_run_timestamp = sqlite3_column_int(res, 11);
        ae->delay_up_to_timestamp = sqlite3_column_int(res, 12);

        ae->name = strdupz((char *) sqlite3_column_text(res, 13));
        ae->hash_name = simple_hash(ae->name);

        ae->chart = strdupz((char *) sqlite3_column_text(res, 14));
        ae->hash_chart = simple_hash(ae->chart);

        ae->family = strdupz((char *) sqlite3_column_text(res, 15));

        if (sqlite3_column_type(res, 16) != SQLITE_NULL)
            ae->exec = strdupz((char *) sqlite3_column_text(res, 16));
        else
            ae->exec = NULL;

        if (sqlite3_column_type(res, 17) != SQLITE_NULL)
            ae->recipient = strdupz((char *) sqlite3_column_text(res, 17));
        else
            ae->recipient = NULL;

        if (sqlite3_column_type(res, 18) != SQLITE_NULL)
            ae->source = strdupz((char *) sqlite3_column_text(res, 18));
        else
            ae->source = NULL;

        if (sqlite3_column_type(res, 19) != SQLITE_NULL)
            ae->units = strdupz((char *) sqlite3_column_text(res, 19));
        else
            ae->units = NULL;

        if (sqlite3_column_type(res, 20) != SQLITE_NULL)
            ae->info = strdupz((char *) sqlite3_column_text(res, 20));
        else
            ae->info = NULL;

        ae->exec_code   = sqlite3_column_int(res, 21);
        ae->new_status  = sqlite3_column_int(res, 22);
        ae->old_status  = sqlite3_column_int(res, 23);
        ae->delay       = sqlite3_column_int(res, 24);

        ae->new_value   = sqlite3_column_double(res, 25);
        ae->old_value   = sqlite3_column_double(res, 26);

        ae->last_repeat = last_repeat;

        if (sqlite3_column_type(res, 28) != SQLITE_NULL)
            ae->classification = strdupz((char *) sqlite3_column_text(res, 28));
        else
            ae->classification = NULL;

        if (sqlite3_column_type(res, 29) != SQLITE_NULL)
            ae->component = strdupz((char *) sqlite3_column_text(res, 29));
        else
            ae->component = NULL;

        if (sqlite3_column_type(res, 30) != SQLITE_NULL)
            ae->type = strdupz((char *) sqlite3_column_text(res, 30));
        else
            ae->type = NULL;

        char value_string[100 + 1];
        freez(ae->old_value_string);
        freez(ae->new_value_string);
        ae->old_value_string = strdupz(format_value_and_unit(value_string, 100, ae->old_value, ae->units, -1));
        ae->new_value_string = strdupz(format_value_and_unit(value_string, 100, ae->new_value, ae->units, -1));

        ae->next = host->health_log.alarms;
        host->health_log.alarms = ae;

        if(unlikely(ae->unique_id > host->health_max_unique_id))
            host->health_max_unique_id = ae->unique_id;

        if(unlikely(ae->alarm_id >= host->health_max_alarm_id))
            host->health_max_alarm_id = ae->alarm_id;
    }

    netdata_rwlock_unlock(&host->health_log.alarm_log_rwlock);
    
    if(!host->health_max_unique_id) host->health_max_unique_id = (uint32_t)now_realtime_sec();
    if(!host->health_max_alarm_id)  host->health_max_alarm_id  = (uint32_t)now_realtime_sec();

    host->health_log.next_log_id = host->health_max_unique_id + 1;
    if (unlikely(!host->health_log.next_alarm_id || host->health_log.next_alarm_id <= host->health_max_alarm_id))
        host->health_log.next_alarm_id = host->health_max_alarm_id + 1;

    //debug(D_HEALTH, "HEALTH [%s]: loaded file '%s' with %zd new alarm entries, updated %zd alarms, errors %zd entries, duplicate %zd", host->hostname, filename, loaded, updated, errored, duplicate);
        
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the health log read statement");

    sql_health_alarm_log_count(host);
}

/*
 * Store an alert hash in the database
 */
#define SQL_STORE_ALERT_HASH "insert into alert_hash (hash_id, alarm, template, on_key, class, component, type, os, hosts, lookup, every, units, calc, families, plugin, module, green, red, warn, crit, exec, to_key, info, delay, options, repeat, host_labels) " \
    "values (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27) on conflict(hash_id) do nothing;"

int sql_store_alert_hash(
        uuid_t *hash_id,
        const char *alarm,
        const char *template,
        const char *on,
        const char *classification,
        const char *component,
        const char *type,
        const char *os,
        const char *hosts,
        const char *lookup,
        const char *every,
        const char *units,
        const char *calc,
        const char *families,
        const char *plugin,
        const char *module,
        const char *green,
        const char *red,
        const char *warn,
        const char *crit,
        const char *exec,
        const char *to,
        const char *info,
        const char *delay,
        const char *options,
        const char *repeat,
        const char *host_labels)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc, param = 0;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_ALERT_HASH, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store alert, rc = %d", rc);
            return 1;
        }
    }

    param++;
    rc = sqlite3_bind_blob(res, 1, hash_id, sizeof(*hash_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    if (alarm && *alarm)
        rc = sqlite3_bind_text(res, 2, alarm, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 2);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    if (template && *template)
        rc = sqlite3_bind_text(res, 3, template, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 3);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 4, on, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 5, classification, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 6, component, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 7, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 8, os, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 9, hosts, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 10, lookup, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 11, every, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 12, units, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 13, calc, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 14, families, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 15, plugin, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 16, module, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 17, green, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 18, red, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 19, warn, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 20, crit, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 21, exec, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 22, to, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 23, info, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 24, delay, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 25, options, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 26, repeat, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 27, host_labels, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;
    

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store alert hash_id, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in alert hash_id store function, rc = %d", rc);

    return 0;

    bind_fail:
    error_report("Failed to bind parameter %d to store alert hash_id, rc = %d", param, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in alert hash_id store function, rc = %d", rc);
  
    return 1;
}

#define SQL_SELECT_ALERT_WITH_HASH "select hash_id, alarm, template, on_key, class, component, type, os, hosts, lookup, every, units, calc, families, plugin, module, green, red, warn, crit, exec, to_key, info, delay, options, repeat, host_labels " \
    "from alert_hash where hash_id = @hash_id;"

#define SQL_SELECT_ALERT "select hash_id, alarm, template, on_key, class, component, type, os, hosts, lookup, every, units, calc, families, plugin, module, green, red, warn, crit, exec, to_key, info, delay, options, repeat, host_labels " \
    "from alert_hash"

void sql_select_alert(char *hash_str, BUFFER *wb)
{
    int rc;
    sqlite3_stmt *res_alert = NULL;

    uuid_t hash_id;
    if (hash_str)
        uuid_parse(hash_str, hash_id);

    if (hash_str)
        rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_ALERT_WITH_HASH, -1, &res_alert, 0);
    else
        rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_ALERT, -1, &res_alert, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch chart config with hash");
        return;
    }

    if (hash_str) {
        rc = sqlite3_bind_blob(res_alert, 1, &hash_id, sizeof(hash_id), SQLITE_STATIC);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to bind host parameter to fetch chart config with hash");
            goto failed;
        }
    }

    buffer_sprintf(wb, "[\n");

    size_t c = 0;

    while (sqlite3_step(res_alert) == SQLITE_ROW) {
        /* char id[512]; */
        /* sprintf(id, "%s.%s", sqlite3_column_text(res_alert, 3), sqlite3_column_text(res_alert, 1)); */

        if (c)
            buffer_strcat(wb, ",\t\t\n");
        else
            buffer_strcat(wb, "\t\n");
        c++;

        char uuid_str[36 + 1];
        uuid_unparse_lower(*((uuid_t *) sqlite3_column_blob(res_alert, 0)), uuid_str);
       
        buffer_sprintf(
            wb,
            "\t{\n"
            "\t\t\"hash_id\": \"%s\",\n"
            "\t\t\"alarm\": \"%s\",\n"
            "\t\t\"template\": \"%s\",\n"
            "\t\t\"on\": \"%s\",\n"
            "\t\t\"class\": \"%s\",\n"
            "\t\t\"component\": \"%s\",\n"
            "\t\t\"type\": \"%s\",\n"
            "\t\t\"os\": \"%s\",\n"
            "\t\t\"hosts\": \"%s\",\n"
            "\t\t\"lookup\": \"%s\",\n"
            "\t\t\"every\": \"%s\",\n"
            "\t\t\"units\": \"%s\",\n"
            "\t\t\"calc\": \"%s\",\n"
            "\t\t\"families\": \"%s\",\n"
            "\t\t\"plugin\": \"%s\",\n"
            "\t\t\"module\": \"%s\",\n"
            "\t\t\"green\": \"%s\",\n"
            "\t\t\"red\": \"%s\",\n"
            "\t\t\"warn\": \"%s\",\n"
            "\t\t\"crit\": \"%s\",\n"
            "\t\t\"exec\": \"%s\",\n"
            "\t\t\"to\": \"%s\",\n"
            "\t\t\"info\": \"%s\",\n"
            "\t\t\"delay\": \"%s\",\n"
            "\t\t\"options\": \"%s\",\n"
            "\t\t\"repeat\": \"%s\",\n"
            "\t\t\"host_labels\": \"%s\"\n",
            uuid_str,
            sqlite3_column_text(res_alert, 1),
            sqlite3_column_text(res_alert, 2),
            sqlite3_column_text(res_alert, 3), sqlite3_column_text(res_alert, 4),
            sqlite3_column_text(res_alert, 5), sqlite3_column_text(res_alert, 6) ,
            sqlite3_column_text(res_alert,7),
            sqlite3_column_text(res_alert, 8),
            sqlite3_column_text(res_alert, 9),
            sqlite3_column_text(res_alert, 10),
            sqlite3_column_text(res_alert, 11),
            sqlite3_column_text(res_alert, 12),
            sqlite3_column_text(res_alert, 13),
            sqlite3_column_text(res_alert, 14),
            sqlite3_column_text(res_alert, 15),
            sqlite3_column_text(res_alert, 16),
            sqlite3_column_text(res_alert, 17),
            sqlite3_column_text(res_alert, 18),
            sqlite3_column_text(res_alert, 19),
            sqlite3_column_text(res_alert, 20),
            sqlite3_column_text(res_alert, 21),
            sqlite3_column_text(res_alert, 22),
            sqlite3_column_text(res_alert, 23),
            sqlite3_column_text(res_alert, 24),
            sqlite3_column_text(res_alert, 25),
            sqlite3_column_text(res_alert, 26));
            

        if (unlikely(rc != SQLITE_OK))
            error_report("Failed to reset the prepared statement when reading chart config with hash");
        buffer_strcat(wb, "\t}");
    }

    buffer_sprintf(wb, "\n]");

failed:
    rc = sqlite3_finalize(res_alert);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading chart config with hash");
}
  
int alert_hash_and_store_config(uuid_t hash_id, const char *alarm, const char *template, const char *os, const char *host, const char *on, const char *families, const char *plugin, const char *module, const char *lookup,const char *calc, const char *every, const char *green,const char *red,const char *warn,const char *crit,const char *exec,const char *to,const char *units,const char *info,const char *classification, const char *component, const char *type, const char *delay,const char *options,const char *repeat,const char *host_labels)
{
    EVP_MD_CTX *evpctx;
    unsigned char hash_value[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    evpctx = EVP_MD_CTX_create();
    EVP_DigestInit_ex(evpctx, EVP_sha256(), NULL);

    if (alarm) {
        EVP_DigestUpdate(evpctx, alarm, strlen(alarm));
    }else if (template) 
        EVP_DigestUpdate(evpctx, template, strlen(template));

    if (os)
        EVP_DigestUpdate(evpctx, os, strlen(os));
    if (host)
        EVP_DigestUpdate(evpctx, host, strlen(host));
    if (on)
        EVP_DigestUpdate(evpctx, on, strlen(on));
    if (families)
        EVP_DigestUpdate(evpctx, families, strlen(families));
    if (plugin)
        EVP_DigestUpdate(evpctx, plugin, strlen(plugin));
    if (module)
        EVP_DigestUpdate(evpctx, module, strlen(module));
    if (lookup)
        EVP_DigestUpdate(evpctx, lookup, strlen(lookup));
    if (calc)
        EVP_DigestUpdate(evpctx, calc, strlen(calc));
    if (every)
        EVP_DigestUpdate(evpctx, every, strlen(every));
    if (green)
        EVP_DigestUpdate(evpctx, green, strlen(green));
    if (red)
        EVP_DigestUpdate(evpctx, red, strlen(red));
    if (warn)
        EVP_DigestUpdate(evpctx, warn, strlen(warn));
    if (crit)
        EVP_DigestUpdate(evpctx, crit, strlen(crit));
    if (exec)
        EVP_DigestUpdate(evpctx, exec, strlen(exec));
    if (to)
        EVP_DigestUpdate(evpctx, to, strlen(to));
    if (units)
        EVP_DigestUpdate(evpctx, units, strlen(units));
    if (info)
        EVP_DigestUpdate(evpctx, info, strlen(info));
    if (classification)
        EVP_DigestUpdate(evpctx, classification, strlen(classification));
    if (component)
        EVP_DigestUpdate(evpctx, component, strlen(component));
    if (type)
        EVP_DigestUpdate(evpctx, type, strlen(type));
    if (delay)
        EVP_DigestUpdate(evpctx, delay, strlen(delay));
    if (options)
        EVP_DigestUpdate(evpctx, options, strlen(options));
    if (repeat)
        EVP_DigestUpdate(evpctx, repeat, strlen(repeat));
    if (host_labels)
        EVP_DigestUpdate(evpctx, host_labels, strlen(host_labels));
    
    
    EVP_DigestFinal_ex(evpctx, hash_value, &hash_len);
    EVP_MD_CTX_destroy(evpctx);
    fatal_assert(hash_len > sizeof(uuid_t));

    char uuid_str[36 + 1];
    uuid_unparse_lower(*((uuid_t *) &hash_value), uuid_str);
    info("Calculating HASH %s for alert %s", uuid_str, alarm);
    uuid_copy(hash_id, *((uuid_t *) &hash_value)); //not needed?

    /* store everything, so it can be recreated when not in memory or just a subset ? */
    (void)sql_store_alert_hash(
        (uuid_t *)&hash_value
        , alarm
        , template
        , on
        , classification
        , component
        , type
        , os
        , host
        , lookup
        , every
        , units
        , calc
        , families
        , plugin
        , module
        , green
        , red
        , warn
        , crit
        , exec
        , to
        , info
        , delay
        , options
        , repeat
        , host_labels);

    return 1;   
}
