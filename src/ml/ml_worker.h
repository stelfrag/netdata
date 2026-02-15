// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ML_WORKER_H
#define ML_WORKER_H

#include "ml_dimension.h"

typedef struct {
    nd_uuid_t metric_uuid;
    ml_kmeans_inlined_t inlined_kmeans;
} ml_model_info_t;

typedef struct {
    netdata_mutex_t nd_mutex;

    calculated_number_t *training_cns;
    calculated_number_t *scratch_training_cns;
    std::vector<DSample> training_samples;

    std::vector<ml_model_info_t> pending_model_info;

    // Reusable buffers for streaming kmeans models
    BUFFER *stream_payload_buffer;
    BUFFER *stream_wb_buffer;

    size_t num_db_transactions;
    size_t num_models_to_prune;
} ml_worker_t;

#endif /* ML_WORKER_H */
