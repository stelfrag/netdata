// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ML_EVENT_LOOP_H
#define ML_EVENT_LOOP_H

#include "libnetdata/libnetdata.h"
#include "database/rrd.h"

// ML Training Event Loop Opcodes
typedef enum __attribute__((packed)) {
    ML_TRAINING_NOOP = 0,
    ML_TRAINING_QUEUE_DIMENSION,
    ML_TRAINING_EXECUTE_BATCH,
    ML_TRAINING_SHUTDOWN,
    ML_TRAINING_OPCODE_MAX
} ml_training_opcode_t;

// Command data structure
typedef struct {
    ml_training_opcode_t opcode;
    UUIDMAP_ID metric_id;
    void *data;
} ml_training_cmd_t;

// Command pool for lock-free command queue
typedef struct {
    ml_training_cmd_t *cmds;
    size_t size;
    size_t head;
    size_t tail;
    SPINLOCK spinlock;
} ml_training_cmd_pool_t;

#define ML_TRAINING_WORKER_BATCH_SIZE (512)

// Worker data structure
typedef struct ml_training_worker {
    uv_work_t request;
    UUIDMAP_ID metric_id[ML_TRAINING_WORKER_BATCH_SIZE];
    int8_t result[ML_TRAINING_WORKER_BATCH_SIZE];  // per-dimension training result
    struct ml_event_loop_config *config;
    bool in_use;
    uint32_t batch_count;

    // Opaque pointer to ml_worker_t (C++ struct with training buffers,
    // pending models, etc.) - one per worker, created at pool init time.
    void *ml_worker;
} ml_training_worker_t;

// Worker pool
typedef struct {
    ml_training_worker_t *workers;
    size_t count;
    size_t max_workers;
    SPINLOCK spinlock;
} ml_training_worker_pool_t;

// Main event loop configuration
typedef struct ml_event_loop_config {
    ND_THREAD *thread;
    uv_loop_t loop;
    uv_async_t async;
    uv_timer_t timer;

    bool initialized;
    bool shutdown_requested;

    // Command pool
    ml_training_cmd_pool_t cmd_pool;

    // Worker pool
    ml_training_worker_pool_t worker_pool;

    // Pending dimensions to train (JudyL: sequence_number -> metric_id)
    Pvoid_t pending_dimensions_judy;

    // Deduplication map (JudyL: metric_id -> sequence_number)
    Pvoid_t queued_dimensions_judy;

    // Monotonic counter for FIFO ordering
    Word_t queue_sequence;

    // Retraining schedule (two-level JudyL)
    // Outer: time_offset → Pvoid_t (inner JudyL of metric_ids scheduled at that time)
    // time_offset = scheduled_time - schedule_base_time (32-bit safe)
    Pvoid_t schedule_judy;
    time_t schedule_base_time;

    // Statistics (written on event loop thread, read via ml_training_get_stats)
    size_t queue_depth;
    size_t scheduled_count;     // total dimensions waiting for retraining
    size_t active_workers;
    size_t max_worker_threads;

    // Accumulator counters for training results
    size_t stats_total_queued;
    size_t stats_total_trained;
    size_t stats_result_ok;
    size_t stats_result_invalid_query_time_range;
    size_t stats_result_not_enough_collected_values;
    size_t stats_result_dim_not_found;
    size_t stats_result_chart_under_replication;

    // Protects stats reads from detection thread
    SPINLOCK stats_spinlock;

    // Synchronization
    struct completion start_stop_complete;
} ml_event_loop_config_t;

// Training statistics snapshot (read by detection thread)
typedef struct {
    size_t queue_depth;                         // current pending dimensions
    size_t scheduled_count;                     // dimensions waiting for retrain timer
    size_t total_queued;                        // total dimensions ever queued
    size_t total_trained;                       // total successful trainings
    size_t result_ok;
    size_t result_invalid_query_time_range;
    size_t result_not_enough_collected_values;
    size_t result_dim_not_found;
    size_t result_chart_under_replication;
} ml_training_stats_t;

// API Functions
#ifdef __cplusplus
extern "C" {
#endif

// Initialize and start the ML training event loop
void ml_training_initialize(void);

// Shutdown the ML training event loop
void ml_training_shutdown(void);

// Queue a dimension for training by UUIDMAP_ID
void ml_training_queue_dimension(UUIDMAP_ID metric_id);

// Execute pending batch
void ml_training_execute_batch(void);

// Get a snapshot of training statistics (thread-safe)
void ml_training_get_stats(ml_training_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // ML_EVENT_LOOP_H
