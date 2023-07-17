// SPDX-License-Identifier: GPL-3.0-or-later

#include "sqlite_functions.h"
#include "sqlite_db_migration.h"

#define DB_SNAPSHOT_VERSION 1

const char *database_snapshot_config[] = {
    "CREATE TABLE IF NOT EXISTS metric(metric_id INTEGER PRIMARY KEY, metric_uuid BLOB)",

    "CREATE UNIQUE INDEX IF NOT EXISTS ind_metric_1 ON metric(metric_uuid)",

    "PRAGMA synchronous=0;",
    "PRAGMA journal_mode=OFF;",
    "PRAGMA temp_store=MEMORY;",

    NULL
};

const char *database_snapshot_tier_config[] = {
    "CREATE TABLE IF NOT EXISTS metric_file_retention(metric_id INTEGER, fileno INTEGER, " \
        "first_time INTEGER, last_time INTEGER, update_every INTEGER, PRIMARY KEY (fileno, metric_id)) WITHOUT ROWID",

    "CREATE VIEW IF NOT EXISTS v_metric_file_retention AS " \
    "SELECT metric_id,fileno,first_time,last_time,update_every FROM metric_file_retention",

    "CREATE TABLE IF NOT EXISTS metric_file_info(fileno INTEGER PRIMARY KEY, " \
            "metric_count INTEGER, first_time INTEGER, last_time, file_size INTEGER) WITHOUT ROWID",

    "CREATE TABLE IF NOT EXISTS metric_retention (metric_id INTEGER PRIMARY KEY, first_fileno INTEGER, last_fileno INTEGER, first_time INTEGER, " \
        "last_time INTEGER, update_every INTEGER)",

    "CREATE TRIGGER IF NOT EXISTS mfr_1 AFTER INSERT ON metric_file_retention BEGIN INSERT INTO metric_retention " \
        "(metric_id, first_fileno, last_fileno, first_time, last_time, update_every) values " \
        "(new.metric_id, new.fileno, new.fileno, new.first_time, new.last_time, new.update_every) ON CONFLICT DO "\
        " UPDATE SET first_time = MIN(first_time, new.first_time), last_time = MAX(last_time, new.last_time), " \
        " first_fileno = MIN(first_fileno, new.fileno), last_fileno = MAX(last_fileno, new.fileno), " \
        " update_every = new.update_every; END ",

    "CREATE TRIGGER IF NOT EXISTS tr_v_mfr_1 INSTEAD OF INSERT ON v_metric_file_retention " \
        "BEGIN " \
        "INSERT INTO metric_retention (metric_id, first_fileno, last_fileno, first_time, last_time, update_every) "
        "VALUES (new.metric_id, new.fileno, new.fileno, new.first_time, new.last_time, new.update_every) " \
        "ON CONFLICT (metric_id) DO UPDATE SET first_time = MIN(first_time, excluded.first_time), " \
        "last_time = MAX(last_time, excluded.last_time), first_fileno = MIN(first_fileno, excluded.first_fileno), " \
        "last_fileno = MAX(last_fileno, excluded.last_fileno),  update_every = excluded.update_every; " \
        "END;",

    "PRAGMA synchronous=0;",
    "PRAGMA journal_mode=OFF;",
    "PRAGMA temp_store=MEMORY;",
	"PRAGMA read_uncommitted=0;",

    NULL
};

//const char *database_snapshot_cleanup[] = {
//
//    NULL
//};

sqlite3 *db_snapshot = NULL;

/*
 * Initialize the SQLite database
 * Return 0 on success
 */
int sql_init_snapshot_database(int memory)
{
    char *err_msg = NULL;
    char sqlite_database[FILENAME_MAX + 1];
    int rc;

    if (likely(!memory))
        snprintfz(sqlite_database, FILENAME_MAX, "%s/netdata-metric.db", netdata_configured_cache_dir);
    else
        strcpy(sqlite_database, ":memory:");

    rc = sqlite3_open(sqlite_database, &db_snapshot);
    if (rc != SQLITE_OK) {
        error_report("Failed to initialize database at %s, due to \"%s\"", sqlite_database, sqlite3_errstr(rc));
        sqlite3_close(db_snapshot);
        db_snapshot = NULL;
        return 1;
    }

    netdata_log_info("SQLite database %s initialization", sqlite_database);

//    char buf[1024 + 1] = "";
//    const char *list[2] = { buf, NULL };

//    int target_version = DB_SNAPSHOT_VERSION;

    // TODO:
//    if (likely(!memory))
//        target_version = perform_database_migration(db_snapshot, DB_SNAPSHOT_VERSION);

    for (int i = 0; database_snapshot_config[i]; i++) {
        rc = sqlite3_exec_monitored(db_snapshot, database_snapshot_config[i], 0, 0, &err_msg);
        if (rc != SQLITE_OK) {
            error_report("SQLite error during database setup, rc = %d (%s)", rc, err_msg);
            error_report("SQLite failed statement %s", database_snapshot_config[i]);
            sqlite3_free(err_msg);
            if (SQLITE_CORRUPT == rc)
               error_report("Databse integrity errors reported");
            return 1;
        }
    }

    netdata_log_info("SQLite database initialization completed");

    return 0;
}

/*
 * Close the sqlite database
 */

void sql_close_snapshot_database(void)
{
    int rc;
    if (unlikely(!db_snapshot))
        return;

    netdata_log_info("Closing SQLite database");

    rc = sqlite3_close_v2(db_snapshot);
    if (unlikely(rc != SQLITE_OK))
        error_report("Error %d while closing the metric snapshot SQLite database, %s", rc, sqlite3_errstr(rc));
}

static inline sqlite3_stmt *prepare_statement_v2(sqlite3 *database, const char *query)
{
    sqlite3_stmt *res;
    int rc = sqlite3_prepare_v2(database ? database : db_snapshot, query, -1, &res, 0);
    if (rc != SQLITE_OK)
        return NULL;

    return res;
}


sqlite3_stmt *snapshot_prepare_lookup_metric(sqlite3 *database)
{
    return prepare_statement_v2(database, "SELECT metric_id FROM metric WHERE metric_uuid = @metric_uuid");
}

int sql_get_metric_id_from_uuid(sqlite3_stmt *res, uuid_t *metric_uuid)
{
    int rc;

    int metric_id = -1;

    rc = sqlite3_bind_blob(res, 1, metric_uuid, sizeof(*metric_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind metric_uuid parameter to get metric_id");
        goto failed;
    }

    rc = sqlite3_step_monitored(res);

    if (likely(rc == SQLITE_ROW))
        metric_id = (int) sqlite3_column_int(res, 0);

failed:
    if (unlikely(sqlite3_reset(res) != SQLITE_OK))
        error_report("Failed to reset the prepared statement when selecting node instance information");

    return metric_id;
}

sqlite3_stmt *snapshot_prepare_store_metric(sqlite3 *database)
{
    return prepare_statement_v2(database, "INSERT OR IGNORE INTO metric(metric_uuid) VALUES (@metric_uuid) RETURNING metric_id");
}
// Add a uuid and return rowid in the database
int sql_add_metric_uuid(sqlite3_stmt *res, uuid_t *metric_uuid)
{
    int rc;

    rc = sqlite3_bind_blob(res, 1, metric_uuid, sizeof(*metric_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind metric_uuid parameter to add metric id");
        goto failed;
    }

    int metric_id = -1;
    rc = sqlite3_step_monitored(res);
    if (SQLITE_ROW == rc)
        metric_id = sqlite3_column_int(res, 0);

failed:
    if (unlikely(sqlite3_reset(res) != SQLITE_OK))
        error_report("Failed to reset the prepared statement when adding a new metric");

    return metric_id;
}

int sql_create_metric_uuid(sqlite3_stmt *lookup_res, sqlite3_stmt *add_res, uuid_t *metric_uuid)
{
    int metric_id;

    metric_id = sql_get_metric_id_from_uuid(lookup_res, metric_uuid);
    if (metric_id == -1) {
        metric_id = sql_add_metric_uuid(add_res, metric_uuid);
        if (metric_id == -1)
            return sql_get_metric_id_from_uuid(lookup_res, metric_uuid);
    }
    return metric_id;
}

/*
#define SQL_ADD_METRIC_TIER_FILE_RETENTION "INSERT OR REPLACE INTO metric_file_retention " \
        " (metric_id, fileno, first_time, last_time, update_every) VALUES " \
        " (@metric_id, @fileno, @first_time, @last_time, @update_every) "
*/

#define SQL_ADD_METRIC_TIER_FILE_RETENTION "INSERT INTO v_metric_file_retention " \
        " (metric_id, fileno, first_time, last_time, update_every) VALUES " \
        " (@metric_id, @fileno, @first_time, @last_time, @update_every) "

sqlite3_stmt *snapshot_prepare_add_file_retention(sqlite3 *database)
{
    return prepare_statement_v2(database, SQL_ADD_METRIC_TIER_FILE_RETENTION);
}

int sql_add_metric_file_retention(sqlite3_stmt *res, int metric_id, int fileno, time_t first_time_t, time_t last_time_t, int update_every)
{
    int rc;

    rc = sqlite3_bind_int(res, 1, metric_id);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind metric_id parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, fileno);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind fileno parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int64(res, 3, first_time_t);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind first_time_t parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int64(res, 4, last_time_t);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind last_time_t parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 5, update_every);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind update_every parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_step_monitored(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed store file metric retention");

failed:
    if (unlikely(sqlite3_reset(res) != SQLITE_OK))
        error_report("Failed to reset the prepared statement when sql_add_metric_file_retention");

    return rc != SQLITE_DONE;
}

int sql_add_metric_uuid_retention(sqlite3_stmt *lookup_res, sqlite3_stmt *add_res, sqlite3_stmt *res, uuid_t *metric_uuid, int fileno, time_t first_time_t, time_t last_time_t, int update_every)
{
    int metric_id = sql_create_metric_uuid(lookup_res,add_res, metric_uuid);

    if (unlikely(metric_id == -1))
        return 1;

    if (unlikely(sql_add_metric_file_retention(res, metric_id, fileno, first_time_t, last_time_t, update_every))) {
        error_report("Failed to store metric retention");
        return 1;
    }
    return 0;
}


#define SQL_SNAPSHOT_GET_FILE_INFO "SELECT metric_count, file_size FROM metric_file_info WHERE fileno = @fileno"

sqlite3_stmt *snapshot_prepare_check(sqlite3 *database)
{
    sqlite3_stmt *res;
    int rc = sqlite3_prepare_v2(database, SQL_SNAPSHOT_GET_FILE_INFO, -1, &res, 0);
    if (rc != SQLITE_OK)
        return NULL;

    return res;
}

bool sql_check_metric_count(sqlite3_stmt *res, int fileno, int entries, int file_size)
{
    int rc;
    bool match = false;

    rc = sqlite3_bind_int(res, 1, fileno);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind fileno parameter to get sql_check_metric_count");
        goto failed;
    }

    rc = sqlite3_step_monitored(res);
    int metric_count = 0;
    int stored_file_size = 0;
    if (likely(rc == SQLITE_ROW)) {
        metric_count = sqlite3_column_int(res, 0);
        stored_file_size = sqlite3_column_int(res, 1);
    }
    match = (metric_count == entries && stored_file_size == file_size);

failed:
    if (unlikely(sqlite3_reset(res) != SQLITE_OK))
        error_report("Failed to reset the prepared statement when selecting node instance information");

    return match;
}


#define SQL_SNAPSHOT_STORE_FILE_INFO "INSERT OR REPLACE INTO metric_file_info (fileno, metric_count, first_time, last_time, file_size) VALUES " \
        "(@fileno, @metric_count, @first_time, @last_time, @file_size)"

int sql_snapshot_store_file_info(sqlite3 *database, int fileno,  int entries, time_t first_time_t, time_t last_time_t, size_t file_size)
{
    sqlite3_stmt *res;

    int store_rc = 0;

    int rc = sqlite3_prepare_v2(database, SQL_SNAPSHOT_STORE_FILE_INFO, -1, &res, 0);
    if (rc != SQLITE_OK)
        return 1;

    rc = sqlite3_bind_int(res, 1, fileno);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind metric_id parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, entries);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind fileno parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int64(res, 3, first_time_t);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind first_time_t parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int64(res, 4, last_time_t);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind last_time_t parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    rc = sqlite3_bind_int64(res, 5,  (sqlite3_int64) file_size);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind last_time_t parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    store_rc = execute_insert(res);
    if (unlikely(store_rc != SQLITE_DONE))
        error_report("Failed to insert snapshot file info rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize statement when storing snapshot file info");

    return store_rc != SQLITE_DONE;
}

#define SQL_SNAPSHOT_RESET_FILE "DELETE FROM metric_file_retention WHERE fileno = @fileno "

int sql_snapshot_reset_fileno(sqlite3 *database, int fileno)
{
    sqlite3_stmt *res;
    int store_rc = 0;

    int rc = sqlite3_prepare_v2(database, SQL_SNAPSHOT_RESET_FILE, -1, &res, 0);
    if (rc != SQLITE_OK)
        return 1;

    rc = sqlite3_bind_int(res, 1, fileno);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind metric_id parameter to get sql_add_metric_file_retention");
        goto failed;
    }

    store_rc = execute_insert(res);
    if (unlikely(store_rc != SQLITE_DONE))
        error_report("Failed to sql_snapshot_reset_fileno info rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize statement when sql_snapshot_reset_fileno");

    return store_rc != SQLITE_DONE;
}

void sql_snapshot_begin_transaction(sqlite3 *database, SPINLOCK *spinlock __maybe_unused)
{
    spinlock_lock(spinlock);
    db_execute(database,"BEGIN TRANSACTION");
}

void sql_snapshot_commit_or_rollaback_transaction(sqlite3 *database, SPINLOCK *spinlock __maybe_unused, bool commit)
{
    if (commit)
        db_execute(database,"COMMIT TRANSACTION");
    else
        db_execute(database,"ROLLBACK TRANSACTION");
    spinlock_unlock(spinlock);
}

sqlite3 *sql_create_tier_snapshot_database(int tier)
{
    char *err_msg = NULL;
    char sqlite_database[FILENAME_MAX + 1];
    int rc;

    snprintfz(sqlite_database, FILENAME_MAX, "%s/netdata-metric-tier-%d.db", netdata_configured_cache_dir, tier);

    sqlite3 *database;
    rc = sqlite3_open(sqlite_database, &database);
    if (rc != SQLITE_OK) {
        error_report("Failed to initialize database at %s, due to \"%s\"", sqlite_database, sqlite3_errstr(rc));
        sqlite3_close(database);
        database = NULL;
        return NULL;
    }

    netdata_log_info("SQLite metric retention for tier %d at %s initialization", tier, sqlite_database);

    char buf[1024 + 1] = "";
    const char *list[2] = { buf, NULL };

    int target_version = DB_SNAPSHOT_VERSION;

    // TODO:
    //    if (likely(!memory))
    //        target_version = perform_database_migration(db_snapshot, DB_SNAPSHOT_VERSION);

    for (int i = 0; database_snapshot_tier_config[i]; i++) {
        rc = sqlite3_exec_monitored(database, database_snapshot_tier_config[i], 0, 0, &err_msg);
        if (rc != SQLITE_OK) {
            error_report("SQLite error during database setup, rc = %d (%s)", rc, err_msg);
            error_report("SQLite failed statement %s", database_snapshot_tier_config[i]);
            sqlite3_free(err_msg);
            if (SQLITE_CORRUPT == rc)
               error_report("Databse integrity errors reported");
            // TODO: Close database
            return NULL;
        }
    }

    snprintfz(buf, 1024, "ATTACH DATABASE \"%s/netdata-metric.db\" as mrg;", netdata_configured_cache_dir);

    if(init_database_batch(database, DB_CHECK_NONE, 0, list)) return NULL;

    netdata_log_info("SQLite database initialization completed");
    return database;
}

#define SQL_REPLAY_SNAPSHOT "SELECT m.metric_uuid, mr.first_time, mr.last_time, mr.update_every " \
        "FROM metric_retention mr, mrg.metric m WHERE mr.metric_id = m.metric_id;"

void sql_replay_snapshot_to_mrg(struct rrdengine_instance *ctx, sqlite3 *database)
{
    sqlite3_stmt *res;

    int rc = sqlite3_prepare_v2(database, SQL_REPLAY_SNAPSHOT, -1, &res, 0);
    if (rc != SQLITE_OK)
        return;

    usec_t started_ut = now_monotonic_usec();


    time_t now_s = max_acceptable_collected_time();

    size_t count = 0;
    while (sqlite3_step_monitored(res) == SQLITE_ROW) {
        uuid_t *uuid = (uuid_t *)sqlite3_column_blob(res, 0);
        time_t start_time_s = (time_t)sqlite3_column_int64(res, 1);
        time_t end_time_s = (time_t)sqlite3_column_int64(res, 2);
        time_t update_every_s = (time_t)sqlite3_column_int64(res, 3);

        mrg_update_metric_retention_and_granularity_by_uuid(
            main_mrg, (Word_t)ctx, uuid, start_time_s, end_time_s, update_every_s, now_s);
        count++;
    }
    usec_t ended_ut = now_monotonic_usec();

    netdata_log_info("sql_replay_snapshot_to_mrg: TIER %d load %zu entries in %0.2f ms",
        ctx->config.tier, count, (double)(ended_ut - started_ut) / USEC_PER_MS);

}
