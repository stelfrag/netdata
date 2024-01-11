// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_SQLITE_SNAPSHOT_H
#define NETDATA_SQLITE_SNAPSHOT_H

#include "daemon/common.h"
#include "sqlite3.h"

struct journal_v2_header;

struct metric_data {
    int first_fileno;
    int last_fileno;
    uint32_t update_every_s;    // Last update every for this metric in this journal (last page collected)
    bool updated;
    time_t start_time_s;
    time_t end_time_s;
};

int sql_init_snapshot_database(int memory);
void sql_close_snapshot_database(void);

sqlite3 *sql_create_tier_snapshot_database(int tier);

sqlite3_stmt *snapshot_prepare_add_file_retention(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_lookup_metric(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_store_metric(sqlite3 *database);
sqlite3_stmt *snapshot_prepare_check(sqlite3 *database);
int sql_snapshot_store_file_info(sqlite3 *database, int fileno,  int entries, time_t first_time_t, time_t last_time_t, size_t file_size);
void sql_replay_snapshot_to_mrg(STORAGE_INSTANCE *db_instance);
void snapshot_init(STORAGE_INSTANCE *db_instance);
bool check_metric_count_judy(STORAGE_INSTANCE *db_instance, int fileno, int entries, int file_size, struct journal_v2_header **j2_header);
int sql_find_or_create_metric_uuid(sqlite3_stmt *lookup_res, sqlite3_stmt *add_res, uuid_t *metric_uuid);
int sql_add_metric_id_retention(sqlite3_stmt *res, int metric_id, int start_fileno, int end_fileno, time_t first_time_t, time_t last_time_t, uint32_t update_every);
sqlite3_stmt *snapshot_prepare_store_metric_id(sqlite3 *database);
#endif //NETDATA_SQLITE_SNAPSHOT_H
