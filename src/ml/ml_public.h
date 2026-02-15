// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_ML_PUBLIC_H
#define NETDATA_ML_PUBLIC_H

#include "database/rrd.h"
#include "web/api/queries/rrdr.h"
#include "database/sqlite/vendored/sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ml_capable();
bool ml_enabled(RRDHOST *rh);
bool ml_streaming_enabled();

void ml_init(void);
void ml_fini(void);

void ml_start_threads(void);
void ml_stop_threads(void);

void ml_host_new(RRDHOST *rh);
void ml_host_delete(RRDHOST *rh);

void ml_host_start(RRDHOST *RH);
void ml_host_stop(RRDHOST *RH);

void ml_host_get_info(RRDHOST *RH, BUFFER *wb);
void ml_host_get_detection_info(RRDHOST *RH, BUFFER *wb);
void ml_host_get_models(RRDHOST *RH, BUFFER *wb);

void ml_chart_new(RRDSET *rs);
void ml_chart_delete(RRDSET *rs);
bool ml_chart_update_begin(RRDSET *rs);
void ml_chart_update_end(RRDSET *rs);

void ml_dimension_new(RRDDIM *rd);
void ml_dimension_delete(RRDDIM *rd);
bool ml_dimension_is_anomalous(RRDDIM *rd, time_t curr_time, double value, bool exists);
void ml_dimension_received_anomaly(RRDDIM *rd, bool is_anomalous);

int ml_dimension_load_models(RRDDIM *rd, sqlite3_stmt **stmt);

void ml_update_global_statistics_charts(uint64_t models_consulted,
                                        uint64_t models_received,
                                        uint64_t models_sent,
                                        uint64_t models_ignored,
                                        uint64_t models_deserialization_failures,
                                        uint64_t memory_consumption,
                                        uint64_t memory_new,
                                        uint64_t memory_delete);

bool ml_host_get_host_status(RRDHOST *rh, struct ml_metrics_statistics *mlm);
bool ml_host_running(RRDHOST *rh);
uint64_t sqlite_get_ml_space(void);

bool ml_model_received_from_child(RRDHOST *host, const char *json);

void ml_host_disconnected(RRDHOST *host);

// C wrapper for training a dimension by UUID (for event loop)
// Returns 0 on success, negative error code on failure
int ml_train_dimension_by_uuid(UUIDMAP_ID metric_id, void *ml_worker);

// Create/destroy an ml_worker_t (opaque C++ struct with training buffers,
// pending model info, streaming buffers, etc.)
void *ml_worker_create(size_t buffer_size);
void ml_worker_destroy(void *ml_worker);

// Flush pending trained models to the database.
// ml_flush_worker_models always flushes; _if_needed only when threshold is met.
void ml_flush_worker_models(void *ml_worker);
void ml_flush_worker_models_if_needed(void *ml_worker);

#ifdef __cplusplus
};
#endif

#endif /* NETDATA_ML_PUBLIC_H */
