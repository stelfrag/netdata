// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_ML_PRIVATE_H
#define NETDATA_ML_PRIVATE_H

#include <vector>
#include <unordered_map>

#include "ml_config.h"

void ml_detect_main(void *arg);

enum ml_worker_result ml_dimension_train_model(ml_worker_t *worker, ml_dimension_t *dim);

void ml_flush_pending_models(ml_worker_t *worker);

extern sqlite3 *ml_db;
extern netdata_mutex_t db_mutex;
extern const char *db_models_create_table;


#endif /* NETDATA_ML_PRIVATE_H */
