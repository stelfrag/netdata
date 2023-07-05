// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_SQLITE_SNAPSHOT_H
#define NETDATA_SQLITE_SNAPSHOT_H

#include "daemon/common.h"
#include "sqlite3.h"

int sql_init_snapshot_database(int memory);
void sql_close_snapshot_database(void);

void sql_add_metric_uuid_retention(sqlite3_stmt *lookup, sqlite3_stmt *store, sqlite3_stmt *res, uuid_t *metric_uuid, int fileno, time_t first_time_t, time_t last_time_t, int update_every);

void sql_snapshot_begin_transaction(sqlite3 *database, SPINLOCK *spinlock);
void sql_snapshot_commit_transaction(sqlite3 *database, SPINLOCK *spinlock);
sqlite3 *sql_create_tier_snapshot_database(int tier);
bool sql_check_metric_count(sqlite3_stmt *res, int fileno, int entries, int file_size);
sqlite3_stmt *snapshot_prepare_add_file_retention(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_lookup_metric(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_store_metric(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_check(sqlite3 *database);
void sql_snapshot_store_file_info(sqlite3 *database, int fileno,  int entries, time_t first_time_t, time_t last_time_t, size_t file_size);
void sql_snapshot_reset_fileno(sqlite3 *database, int fileno);
void sql_replay_snapshot_to_mrg(struct rrdengine_instance *ctx, sqlite3 *database);

#endif //NETDATA_SQLITE_SNAPSHOT_H
