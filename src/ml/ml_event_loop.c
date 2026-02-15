// SPDX-License-Identifier: GPL-3.0-or-later

#include "ml_event_loop.h"
#include "ml_public.h"

// Forward declarations to avoid including C++ headers
// We only need these specific config values
extern time_t ml_get_training_window(void);
extern unsigned ml_get_lag_n(void);
extern unsigned ml_get_train_every(void);

#define ML_TRAINING_CMD_POOL_SIZE 2048
#define ML_TRAINING_TIMER_PERIOD_MS 5000            // Schedule every 5 seconds
#define ML_TRAINING_MAX_WORKERS 16

// Global event loop configuration
static ml_event_loop_config_t ml_training_config = {0};

// ----------------------------------------------------------------------------
// Command Pool Functions

static void ml_training_init_cmd_pool(ml_training_cmd_pool_t *pool, size_t size) {
    pool->cmds = callocz(size, sizeof(ml_training_cmd_t));
    pool->size = size;
    pool->head = 0;
    pool->tail = 0;
    spinlock_init(&pool->spinlock);
}

static void ml_training_destroy_cmd_pool(ml_training_cmd_pool_t *pool) {
    freez(pool->cmds);
    pool->cmds = NULL;
    pool->size = 0;
}

static bool ml_training_push_cmd(ml_training_cmd_pool_t *pool, ml_training_cmd_t *cmd)
{
    spinlock_lock(&pool->spinlock);

    size_t next_head = (pool->head + 1) % pool->size;
    if (next_head == pool->tail) {
        spinlock_unlock(&pool->spinlock);
        return false;
    }

    pool->cmds[pool->head] = *cmd;
    pool->head = next_head;

    spinlock_unlock(&pool->spinlock);
    return true;
}

static ml_training_cmd_t ml_training_pop_cmd(ml_training_cmd_pool_t *pool)
{
    ml_training_cmd_t cmd = {.opcode = ML_TRAINING_NOOP, .metric_id = 0, .data = NULL};

    spinlock_lock(&pool->spinlock);

    if (pool->tail != pool->head) {
        cmd = pool->cmds[pool->tail];
        pool->tail = (pool->tail + 1) % pool->size;
    }

    spinlock_unlock(&pool->spinlock);
    return cmd;
}

// ----------------------------------------------------------------------------
// Worker Pool Functions

static void ml_training_init_worker_pool(ml_training_worker_pool_t *pool, size_t max_workers)
{
    pool->workers = callocz(max_workers, sizeof(ml_training_worker_t));
    pool->count = 0;
    pool->max_workers = max_workers;
    spinlock_init(&pool->spinlock);

    // Calculate buffer size based on ML config (same as old workers)
    // For 1-second metrics: training_window samples with lag
    size_t max_elements_needed = (size_t)ml_get_training_window() * (size_t)(ml_get_lag_n() + 1);

    for (size_t i = 0; i < max_workers; i++) {
        pool->workers[i].in_use = false;
        pool->workers[i].ml_worker = ml_worker_create(max_elements_needed);
    }
}

static void ml_training_destroy_worker_pool(ml_training_worker_pool_t *pool) {
    for (size_t i = 0; i < pool->max_workers; i++) {
        // Flush any remaining pending models before destroying
        ml_flush_worker_models(pool->workers[i].ml_worker);
        ml_worker_destroy(pool->workers[i].ml_worker);
        pool->workers[i].ml_worker = NULL;
    }

    freez(pool->workers);
    pool->workers = NULL;
    pool->count = 0;
}

static ml_training_worker_t *ml_training_get_worker(ml_training_worker_pool_t *pool) {
    ml_training_worker_t *worker = NULL;

    spinlock_lock(&pool->spinlock);

    for (size_t i = 0; i < pool->max_workers; i++) {
        if (!pool->workers[i].in_use) {
            pool->workers[i].in_use = true;
            worker = &pool->workers[i];
            pool->count++;
            break;
        }
    }

    spinlock_unlock(&pool->spinlock);
    return worker;
}

static void ml_training_return_worker(ml_training_worker_pool_t *pool, ml_training_worker_t *worker) {
    spinlock_lock(&pool->spinlock);

    worker->in_use = false;
    if (pool->count > 0)
        pool->count--;

    spinlock_unlock(&pool->spinlock);
}

// ----------------------------------------------------------------------------
// Pending Dimensions Management (JudyL)

static void add_pending_dimension(ml_event_loop_config_t *config, UUIDMAP_ID metric_id)
{
    if (!metric_id)
        return;

    // Dedup check: is this metric_id already queued?
    Word_t index = metric_id;
    PPvoid_t pvalue = JudyLGet(config->queued_dimensions_judy, index, PJE0);
    if (pvalue && pvalue != PPJERR)
        return;

    // Assign a sequence number for FIFO ordering
    Word_t seq = ++config->queue_sequence;

    // Insert into pending: sequence_number -> metric_id
    pvalue = JudyLIns(&config->pending_dimensions_judy, seq, PJE0);
    if (pvalue && pvalue != PPJERR)
        *pvalue = (void *)(uintptr_t)metric_id;

    // Insert into dedup map: metric_id -> sequence_number
    pvalue = JudyLIns(&config->queued_dimensions_judy, index, PJE0);
    if (pvalue && pvalue != PPJERR)
        *pvalue = (void *)seq;

    config->queue_depth++;
    config->stats_total_queued++;
}

static UUIDMAP_ID get_next_pending_dimension(ml_event_loop_config_t *config) {
    // Get the oldest entry (lowest sequence number)
    Word_t seq = 0;
    PPvoid_t pvalue = JudyLFirst(config->pending_dimensions_judy, &seq, PJE0);
    if (!pvalue || pvalue == PPJERR)
        return 0;

    UUIDMAP_ID metric_id = (UUIDMAP_ID)(uintptr_t)*pvalue;

    // Remove from both maps
    JudyLDel(&config->pending_dimensions_judy, seq, PJE0);
    JudyLDel(&config->queued_dimensions_judy, (Word_t)metric_id, PJE0);

    if (config->queue_depth > 0)
        config->queue_depth--;

    return metric_id;
}

// ----------------------------------------------------------------------------
// Retraining Schedule Management (two-level JudyL)

static void schedule_dimension_retrain(ml_event_loop_config_t *config, UUIDMAP_ID metric_id)
{
    if (!metric_id)
        return;

    time_t now = now_realtime_sec();

    // Guard against clock jumping backward
    if (now < config->schedule_base_time)
        config->schedule_base_time = now;

    Word_t offset = (Word_t)(now - config->schedule_base_time) + (Word_t)ml_get_train_every();

    // Get or create inner JudyL for this time offset
    PPvoid_t pvalue = JudyLIns(&config->schedule_judy, offset, PJE0);
    if (!pvalue || pvalue == PPJERR)
        return;

    Pvoid_t *inner_judy = (Pvoid_t *)pvalue;

    // Insert metric_id into the inner JudyL (value doesn't matter, just the key)
    PPvoid_t pinner = JudyLIns(inner_judy, (Word_t)metric_id, PJE0);
    if (pinner && pinner != PPJERR) {
        *pinner = NULL;
        config->scheduled_count++;
    }
}

// Move all due dimensions from the schedule into the pending queue.
// Returns the number of dimensions moved.
static size_t schedule_move_due_to_pending(ml_event_loop_config_t *config)
{
    if (!config->schedule_judy)
        return 0;

    time_t now = now_realtime_sec();

    // Guard against clock jumping backward
    if (now < config->schedule_base_time)
        config->schedule_base_time = now;

    Word_t current_offset = (Word_t)(now - config->schedule_base_time);
    size_t moved = 0;

    // Iterate outer JudyL from offset 0 up to current_offset.
    // After deleting an entry, restart from offset 0 (JudyLFirst) since
    // the array was modified. This is safe because we only process entries
    // with offset <= current_offset, and we delete each one after processing.
    Word_t offset = 0;
    PPvoid_t pvalue;

    while ((pvalue = JudyLFirst(config->schedule_judy, &offset, PJE0)) != NULL
           && pvalue != PPJERR
           && offset <= current_offset) {

        Pvoid_t inner_judy = *pvalue;

        // Iterate all metric_ids in this time slot
        if (inner_judy) {
            Word_t metric_id = 0;
            PPvoid_t pinner = JudyLFirst(inner_judy, &metric_id, PJE0);

            while (pinner && pinner != PPJERR) {
                add_pending_dimension(config, (UUIDMAP_ID)metric_id);
                moved++;
                if (config->scheduled_count > 0)
                    config->scheduled_count--;

                pinner = JudyLNext(inner_judy, &metric_id, PJE0);
            }

            // Free the inner JudyL
            JudyLFreeArray((Pvoid_t *)&inner_judy, PJE0);
        }

        // Delete this time slot from the outer JudyL and restart
        JudyLDel(&config->schedule_judy, offset, PJE0);
        offset = 0;
    }

    return moved;
}

static void schedule_destroy(ml_event_loop_config_t *config)
{
    if (!config->schedule_judy)
        return;

    // Free all inner JudyL arrays
    Word_t offset = 0;
    PPvoid_t pvalue = JudyLFirst(config->schedule_judy, &offset, PJE0);

    while (pvalue && pvalue != PPJERR) {
        Pvoid_t inner_judy = *pvalue;
        if (inner_judy)
            JudyLFreeArray((Pvoid_t *)&inner_judy, PJE0);

        pvalue = JudyLNext(config->schedule_judy, &offset, PJE0);
    }

    JudyLFreeArray(&config->schedule_judy, PJE0);
    config->scheduled_count = 0;
}

// ----------------------------------------------------------------------------
// ML Training Worker Job

static void after_ml_worker_job(uv_work_t *req, int status __maybe_unused)
{
    ml_training_worker_t *worker = req->data;
    ml_event_loop_config_t *config = worker->config;

    // Schedule retraining for dimensions that still exist and accumulate stats.
    // Results <= -100 mean the dimension is gone or permanently excluded
    // (see ML_TRAIN_ERR_* in ml_public.cc). Everything else is transient.
    for (uint32_t i = 0; i < worker->batch_count; i++) {
        int8_t r = worker->result[i];

        if (r > -100)
            schedule_dimension_retrain(config, worker->metric_id[i]);

        // Accumulate training result counters (single-threaded on event loop)
        switch (r) {
            case 0:
                config->stats_result_ok++;
                config->stats_total_trained++;
                break;
            case -1:  // ML_WORKER_RESULT_INVALID_QUERY_TIME_RANGE
                config->stats_result_invalid_query_time_range++;
                break;
            case -2:  // ML_WORKER_RESULT_NOT_ENOUGH_COLLECTED_VALUES
                config->stats_result_not_enough_collected_values++;
                break;
            case -4:  // ML_WORKER_RESULT_CHART_UNDER_REPLICATION
                config->stats_result_chart_under_replication++;
                break;
            case -101: // ML_TRAIN_ERR_DIM_NOT_FOUND
                config->stats_result_dim_not_found++;
                break;
            default:
                break;
        }
    }

    config->active_workers--;

    ml_training_return_worker(&config->worker_pool, worker);
}

static void ml_worker_job(uv_work_t *req)
{
    ml_training_worker_t *worker = req->data;

    // Train each dimension in the batch using the persistent ml_worker
    for (uint32_t i = 0; i < worker->batch_count; i++) {
        int result = ml_train_dimension_by_uuid(worker->metric_id[i], worker->ml_worker);
        worker->result[i] = (int8_t)result;

        // result == 0: success
        // <= -100: dimension gone/excluded (normal, silent)
        // -1..-99: transient training failures (normal for new dimensions, silent)
    }

    // Flush accumulated models to DB if the batch threshold is met
    ml_flush_worker_models_if_needed(worker->ml_worker);
}

// ----------------------------------------------------------------------------
// Event Loop Callbacks

static void timer_cb(uv_timer_t *handle)
{
    ml_event_loop_config_t *config = handle->data;

    // Move scheduled dimensions that are due into the pending queue
    schedule_move_due_to_pending(config);

    size_t pending_count = config->queue_depth;

    // If there are pending dimensions, trigger a batch execution
    if (pending_count > 0) {
        ml_training_cmd_t cmd = {.opcode = ML_TRAINING_EXECUTE_BATCH, .metric_id = 0, .data = NULL};

        if (ml_training_push_cmd(&config->cmd_pool, &cmd)) {
            uv_async_send(&config->async);
        } else {
            netdata_log_info("ML: Failed to queue batch execution command (command pool full)");
        }
    }
}

// ----------------------------------------------------------------------------
// Main Event Loop

static void ml_training_event_loop(void *arg) {
    ml_event_loop_config_t *config = arg;
    uv_loop_t *loop = &config->loop;

    // Initialize libuv loop
    int rc = uv_loop_init(loop);
    if (rc) {
        netdata_log_error("ML: Failed to initialize event loop: %s", uv_strerror(rc));
        completion_mark_complete(&config->start_stop_complete);
        return;
    }

    rc = uv_async_init(loop, &config->async, NULL);
    if (rc) {
        netdata_log_error("ML: Failed to initialize async handle: %s", uv_strerror(rc));
        uv_loop_close(loop);
        completion_mark_complete(&config->start_stop_complete);
        return;
    }

    // Initialize timer (periodic callback)
    rc = uv_timer_init(loop, &config->timer);
    if (rc) {
        netdata_log_error("ML: Failed to initialize timer: %s", uv_strerror(rc));
        uv_close((uv_handle_t *)&config->async, NULL);
        uv_loop_close(loop);
        completion_mark_complete(&config->start_stop_complete);
        return;
    }

    config->timer.data = config;
    rc = uv_timer_start(&config->timer, timer_cb, ML_TRAINING_TIMER_PERIOD_MS, ML_TRAINING_TIMER_PERIOD_MS);
    if (rc) {
        netdata_log_error("ML: Failed to start timer: %s", uv_strerror(rc));
    }

    // Register worker jobs (must match pulse-workers.c entry "MLTRAIN")
    worker_register("MLTRAIN");
    worker_register_job_name(0, "queue");
    worker_register_job_name(1, "dispatch");

    // Mark as initialized and signal completion
    __atomic_store_n(&config->initialized, true, __ATOMIC_RELEASE);
    completion_mark_complete(&config->start_stop_complete);

    netdata_log_info("ML: Training event loop started");

    // Main event loop
    while (likely(!config->shutdown_requested)) {
        worker_is_idle();
        uv_run(loop, UV_RUN_ONCE);

        // Process commands
        ml_training_cmd_t cmd;
        do {
            cmd = ml_training_pop_cmd(&config->cmd_pool);

            switch (cmd.opcode) {
                case ML_TRAINING_QUEUE_DIMENSION:
                    worker_is_busy(0);
                    add_pending_dimension(config, cmd.metric_id);
                    break;

                case ML_TRAINING_EXECUTE_BATCH:
                    worker_is_busy(1);
                    // Execute pending training jobs
                    while (config->active_workers < config->max_worker_threads) {

                        ml_training_worker_t *worker = ml_training_get_worker(&config->worker_pool);
                        if (!worker)
                            break;
                        worker->config = config;
                        worker->request.data = worker;

                        worker->batch_count = 0;
                        while (worker->batch_count < ML_TRAINING_WORKER_BATCH_SIZE) {
                            UUIDMAP_ID metric_id = get_next_pending_dimension(config);
                            if (!metric_id)
                                break;

                            worker->metric_id[worker->batch_count++] = metric_id;
                        }
                        if (worker->batch_count == 0) {
                            ml_training_return_worker(&config->worker_pool, worker);
                            break;
                        }

                        config->active_workers++;
                        int uv_rc = uv_queue_work(loop, &worker->request, ml_worker_job, after_ml_worker_job);
                        if (uv_rc) {
                            config->active_workers--;
                            ml_training_return_worker(&config->worker_pool, worker);
                            netdata_log_error("ML: Failed to queue work: %s", uv_strerror(uv_rc));
                            break;
                        }
                    }
                    break;

                case ML_TRAINING_SHUTDOWN:
                    config->shutdown_requested = true;
                    break;

                case ML_TRAINING_NOOP:
                default:
                    break;
            }

            if (cmd.opcode != ML_TRAINING_NOOP)
                uv_run(loop, UV_RUN_NOWAIT);

        } while (cmd.opcode != ML_TRAINING_NOOP);
    }

    netdata_log_info("ML: Training event loop shutting down");

    // Stop timer
    uv_timer_stop(&config->timer);

    // Wait for active workers to complete (with timeout)
    time_t shutdown_start = now_monotonic_sec();
    while (config->active_workers > 0) {
        uv_run(loop, UV_RUN_NOWAIT);
        if (now_monotonic_sec() - shutdown_start > 5) {
            netdata_log_info("ML: Timeout waiting for workers to complete (%zu still active)",
                             config->active_workers);
            break;
        }
        sleep_usec(100 * USEC_PER_MS);
    }

    // Close handles
    uv_close((uv_handle_t *)&config->timer, NULL);
    uv_close((uv_handle_t *)&config->async, NULL);

    // Run loop until all handles are closed
    while (uv_loop_close(loop) == UV_EBUSY) {
        uv_run(loop, UV_RUN_NOWAIT);
    }

    // Clean up JudyL arrays
    JudyLFreeArray(&config->pending_dimensions_judy, PJE0);
    JudyLFreeArray(&config->queued_dimensions_judy, PJE0);
    schedule_destroy(config);

    __atomic_store_n(&config->initialized, false, __ATOMIC_RELEASE);
    completion_mark_complete(&config->start_stop_complete);

    netdata_log_info("ML: Training event loop stopped");
}

// ----------------------------------------------------------------------------
// Public API

void ml_training_initialize(void) {
    memset(&ml_training_config, 0, sizeof(ml_training_config));

    // Initialize synchronization
    completion_init(&ml_training_config.start_stop_complete);

    // Initialize command pool
    ml_training_init_cmd_pool(&ml_training_config.cmd_pool, ML_TRAINING_CMD_POOL_SIZE);

    // Initialize worker pool
    ml_training_init_worker_pool(&ml_training_config.worker_pool, ML_TRAINING_MAX_WORKERS);

    ml_training_config.max_worker_threads = ML_TRAINING_MAX_WORKERS;

    // Initialize retraining schedule
    ml_training_config.schedule_base_time = now_realtime_sec();

    // Initialize stats spinlock
    spinlock_init(&ml_training_config.stats_spinlock);

    // Create event loop thread
    ml_training_config.thread = nd_thread_create(
        "MLTRAINING",
        NETDATA_THREAD_OPTION_DEFAULT,
        ml_training_event_loop,
        &ml_training_config
    );

    // Wait for initialization to complete
    completion_wait_for(&ml_training_config.start_stop_complete);
    completion_reset(&ml_training_config.start_stop_complete);

    netdata_log_info("ML: Training system initialized");
}

void ml_training_shutdown(void) {
    if (!ml_training_config.thread)
        return;

    netdata_log_info("ML: Shutting down training system");

    // Send shutdown command
    ml_training_cmd_t cmd = {
        .opcode = ML_TRAINING_SHUTDOWN,
        .metric_id = 0,
        .data = NULL
    };

    if (ml_training_push_cmd(&ml_training_config.cmd_pool, &cmd)) {
        uv_async_send(&ml_training_config.async);
        completion_wait_for(&ml_training_config.start_stop_complete);
    }

    // Join thread
    nd_thread_join(ml_training_config.thread);
    ml_training_config.thread = NULL;

    // Clean up pools
    ml_training_destroy_cmd_pool(&ml_training_config.cmd_pool);
    ml_training_destroy_worker_pool(&ml_training_config.worker_pool);

    // Destroy synchronization
    completion_destroy(&ml_training_config.start_stop_complete);

    netdata_log_info("ML: Training system shutdown complete");
}

void ml_training_queue_dimension(UUIDMAP_ID metric_id) {
    if (!metric_id)
        return;

    if (!__atomic_load_n(&ml_training_config.initialized, __ATOMIC_ACQUIRE))
        return;

    ml_training_cmd_t cmd = {
        .opcode = ML_TRAINING_QUEUE_DIMENSION,
        .metric_id = metric_id,
        .data = NULL
    };

    if (ml_training_push_cmd(&ml_training_config.cmd_pool, &cmd)) {
        uv_async_send(&ml_training_config.async);
    }
}

void ml_training_execute_batch(void) {
    if (!__atomic_load_n(&ml_training_config.initialized, __ATOMIC_ACQUIRE))
        return;

    ml_training_cmd_t cmd = {
        .opcode = ML_TRAINING_EXECUTE_BATCH,
        .metric_id = 0,
        .data = NULL
    };

    if (ml_training_push_cmd(&ml_training_config.cmd_pool, &cmd)) {
        uv_async_send(&ml_training_config.async);
    }
}

void ml_training_get_stats(ml_training_stats_t *stats) {
    ml_event_loop_config_t *config = &ml_training_config;

    spinlock_lock(&config->stats_spinlock);

    stats->queue_depth = config->queue_depth;
    stats->scheduled_count = config->scheduled_count;
    stats->total_queued = config->stats_total_queued;
    stats->total_trained = config->stats_total_trained;
    stats->result_ok = config->stats_result_ok;
    stats->result_invalid_query_time_range = config->stats_result_invalid_query_time_range;
    stats->result_not_enough_collected_values = config->stats_result_not_enough_collected_values;
    stats->result_dim_not_found = config->stats_result_dim_not_found;
    stats->result_chart_under_replication = config->stats_result_chart_under_replication;

    spinlock_unlock(&config->stats_spinlock);
}
