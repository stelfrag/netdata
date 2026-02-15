// SPDX-License-Identifier: GPL-3.0-or-later

#include "ml_private.h"

#include "database/sqlite/sqlite_db_migration.h"
#include "ml_event_loop.h"

#include <random>

#define ML_METADATA_VERSION 2

bool ml_capable()
{
    return true;
}

bool ml_enabled(RRDHOST *rh)
{
    if (!rh)
        return false;

    if (!Cfg.enable_anomaly_detection)
        return false;

    if (simple_pattern_matches(Cfg.sp_host_to_skip, rrdhost_hostname(rh)))
        return false;

    return true;
}

bool ml_streaming_enabled()
{
    return Cfg.stream_anomaly_detection_charts;
}

void ml_host_new(RRDHOST *rh)
{
    if (!ml_enabled(rh))
        return;

    auto *host = new ml_host_t();

    host->rh = rh;
    host->mls = ml_machine_learning_stats_t();
    host->host_anomaly_rate = 0.0;
    host->anomaly_rate_rs = nullptr;

    netdata_mutex_init(&host->mutex);
    spinlock_init(&host->context_anomaly_rate_spinlock);

    host->ml_running = false;
    rh->ml_host = reinterpret_cast<rrd_ml_host_t*>(host);
}

void ml_host_delete(RRDHOST *rh)
{
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host)
        return;

    netdata_mutex_destroy(&host->mutex);

    delete host;
    rh->ml_host = nullptr;
}

void ml_host_start(RRDHOST *rh) {
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host)
        return;

    host->ml_running = true;
}

void ml_host_stop(RRDHOST *rh) {
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host || !host->ml_running)
        return;

    netdata_mutex_lock(&host->mutex);

    // reset host stats
    host->mls = ml_machine_learning_stats_t();

    // reset charts/dims
    void *rsp = nullptr;
    rrdset_foreach_read(rsp, host->rh) {
        auto *rs = static_cast<RRDSET *>(rsp);

        auto *chart = reinterpret_cast<ml_chart_t*>(rs->ml_chart);
        if (!chart)
            continue;

        // reset chart
        chart->mls = ml_machine_learning_stats_t();

        void *rdp = nullptr;
        rrddim_foreach_read(rdp, rs) {
            auto *rd = static_cast<RRDDIM *>(rdp);

            auto *dim = reinterpret_cast<ml_dimension_t*>(rd->ml_dimension);
            if (!dim)
                continue;

            spinlock_lock(&dim->slock);

            dim->mt = METRIC_TYPE_CONSTANT;
            dim->ts = TRAINING_STATUS_UNTRAINED;

            dim->suppression_anomaly_counter = 0;
            dim->suppression_window_counter = 0;
            dim->cns.clear();

            ml_kmeans_init(&dim->kmeans);

            spinlock_unlock(&dim->slock);
        }
        rrddim_foreach_done(rdp);
    }
    rrdset_foreach_done(rsp);

    netdata_mutex_unlock(&host->mutex);

    host->ml_running = false;
}

void ml_host_get_info(RRDHOST *rh, BUFFER *wb)
{
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host) {
        buffer_json_member_add_boolean(wb, "enabled", false);
        return;
    }

    buffer_json_member_add_uint64(wb, "version", 1);

    buffer_json_member_add_boolean(wb, "enabled", Cfg.enable_anomaly_detection);

    buffer_json_member_add_uint64(wb, "training-window", Cfg.training_window);
    buffer_json_member_add_uint64(wb, "min-training-window", Cfg.min_training_window);
    buffer_json_member_add_uint64(wb, "max-training-vectors", Cfg.max_training_vectors);
    buffer_json_member_add_uint64(wb, "max-samples-to-smooth", Cfg.max_samples_to_smooth);
    buffer_json_member_add_uint64(wb, "train-every", Cfg.train_every);

    buffer_json_member_add_uint64(wb, "diff-n", Cfg.diff_n);
    buffer_json_member_add_uint64(wb, "lag-n", Cfg.lag_n);

    buffer_json_member_add_uint64(wb, "max-kmeans-iters", Cfg.max_kmeans_iters);

    buffer_json_member_add_double(wb, "dimension-anomaly-score-threshold", Cfg.dimension_anomaly_score_threshold);

    buffer_json_member_add_string(wb, "anomaly-detection-grouping-method", time_grouping_id2txt(Cfg.anomaly_detection_grouping_method));

    buffer_json_member_add_int64(wb, "anomaly-detection-query-duration", Cfg.anomaly_detection_query_duration);

    buffer_json_member_add_string(wb, "hosts-to-skip", Cfg.hosts_to_skip.c_str());
    buffer_json_member_add_string(wb, "charts-to-skip", Cfg.charts_to_skip.c_str());
}

void ml_host_get_detection_info(RRDHOST *rh, BUFFER *wb)
{
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host)
        return;

    netdata_mutex_lock(&host->mutex);

    buffer_json_member_add_uint64(wb, "version", 2);
    buffer_json_member_add_uint64(wb, "ml-running", host->ml_running);
    buffer_json_member_add_uint64(wb, "anomalous-dimensions", host->mls.num_anomalous_dimensions);
    buffer_json_member_add_uint64(wb, "normal-dimensions", host->mls.num_normal_dimensions);
    buffer_json_member_add_uint64(wb, "total-dimensions", host->mls.num_anomalous_dimensions +
                                                          host->mls.num_normal_dimensions);
    buffer_json_member_add_uint64(wb, "trained-dimensions", host->mls.num_training_status_trained +
                                                            host->mls.num_training_status_pending_with_model);
    netdata_mutex_unlock(&host->mutex);
}

bool ml_host_get_host_status(RRDHOST *rh, struct ml_metrics_statistics *mlm) {
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host) {
        memset(mlm, 0, sizeof(*mlm));
        return false;
    }

    netdata_mutex_lock(&host->mutex);

    mlm->anomalous = host->mls.num_anomalous_dimensions;
    mlm->normal = host->mls.num_normal_dimensions;
    mlm->trained = host->mls.num_training_status_trained + host->mls.num_training_status_pending_with_model;
    mlm->pending = host->mls.num_training_status_untrained + host->mls.num_training_status_pending_without_model;
    mlm->silenced = host->mls.num_training_status_silenced;

    netdata_mutex_unlock(&host->mutex);

    return true;
}

bool ml_host_running(RRDHOST *rh) {
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if(!host)
        return false;

    return true;
}

void ml_host_get_models(RRDHOST *rh, BUFFER *wb)
{
    UNUSED(rh);
    UNUSED(wb);

    // TODO: To be implemented
    netdata_log_error("Fetching KMeans models is not supported yet");
}

void ml_chart_new(RRDSET *rs)
{
    auto *host = reinterpret_cast<ml_host_t*>(rs->rrdhost->ml_host);
    if (!host)
        return;

    auto *chart = new ml_chart_t();

    chart->rs = rs;
    chart->mls = ml_machine_learning_stats_t();

    rs->ml_chart = reinterpret_cast<rrd_ml_chart_t*>(chart);
}

void ml_chart_delete(RRDSET *rs)
{
    auto *host = reinterpret_cast<ml_host_t*>(rs->rrdhost->ml_host);
    if (!host)
        return;

    auto *chart = reinterpret_cast<ml_chart_t*>(rs->ml_chart);

    delete chart;
    rs->ml_chart = nullptr;
}

ALWAYS_INLINE_ONLY bool ml_chart_update_begin(RRDSET *rs)
{
    auto *chart = reinterpret_cast<ml_chart_t*>(rs->ml_chart);
    if (!chart)
        return false;

    chart->mls = {};
    return true;
}

void ml_chart_update_end(RRDSET *rs)
{
    auto *chart = reinterpret_cast<ml_chart_t*>(rs->ml_chart);
    if (!chart)
        return;
}

void ml_dimension_new(RRDDIM *rd)
{
    auto *chart = reinterpret_cast<ml_chart_t*>(rd->rrdset->ml_chart);
    if (!chart)
        return;

    auto *dim = new ml_dimension_t();

    dim->rd = rd;

    dim->mt = METRIC_TYPE_CONSTANT;
    dim->ts = TRAINING_STATUS_UNTRAINED;
    dim->suppression_anomaly_counter = 0;
    dim->suppression_window_counter = 0;
    dim->training_in_progress = false;

    ml_kmeans_init(&dim->kmeans);

    if (simple_pattern_matches(Cfg.sp_charts_to_skip, rrdset_name(rd->rrdset)))
        dim->mls = MACHINE_LEARNING_STATUS_DISABLED_DUE_TO_EXCLUDED_CHART;
    else
        dim->mls = MACHINE_LEARNING_STATUS_ENABLED;

    spinlock_init(&dim->slock);

    dim->km_contexts.reserve(Cfg.num_models_to_use);

    rd->ml_dimension = reinterpret_cast<rrd_ml_dimension_t*>(dim);

    metaqueue_ml_load_models(rd);

    // Queue dimension for training using the new event loop
    if (rd->uuid)
        ml_training_queue_dimension(rd->uuid);
}

void ml_dimension_delete(RRDDIM *rd)
{
    auto *dim = reinterpret_cast<ml_dimension_t*>(rd->ml_dimension);
    if (!dim)
        return;

    // Wait for any in-progress training to complete before deleting
    // This prevents use-after-free crashes when training thread accesses dim->rd
    size_t wait_iterations = 0;
    const size_t max_wait_iterations = 3000; // 30 seconds max (3000 * 10ms)

    spinlock_lock(&dim->slock);
    while (dim->training_in_progress && wait_iterations < max_wait_iterations) {
        spinlock_unlock(&dim->slock);
        sleep_usec(10000); // Wait 10ms
        wait_iterations++;
        spinlock_lock(&dim->slock);
    }

    if (dim->training_in_progress) {
        // Training is stuck, but we can't wait forever
        // Log the issue but proceed with deletion
        netdata_log_error("ML: Dimension '%s' of chart '%s' is being deleted while training is in progress after waiting %zu ms",
                          rrddim_id(rd), rrdset_id(rd->rrdset), wait_iterations * 10);
    }

    spinlock_unlock(&dim->slock);

    delete dim;
    rd->ml_dimension = nullptr;
}

ALWAYS_INLINE_ONLY void ml_dimension_received_anomaly(RRDDIM *rd, bool is_anomalous) {
    auto *dim = reinterpret_cast<ml_dimension_t*>(rd->ml_dimension);
    if (!dim)
        return;

    auto *host = reinterpret_cast<ml_host_t*>(rd->rrdset->rrdhost->ml_host);
    if (!host->ml_running)
        return;

    auto *chart = reinterpret_cast<ml_chart_t*>(rd->rrdset->ml_chart);

    ml_chart_update_dimension(chart, dim, is_anomalous);
}

bool ml_dimension_is_anomalous(RRDDIM *rd, time_t curr_time, double value, bool exists)
{
    UNUSED(curr_time);

    auto *dim = reinterpret_cast<ml_dimension_t*>(rd->ml_dimension);
    if (!dim)
        return false;

    auto *host = reinterpret_cast<ml_host_t*>(rd->rrdset->rrdhost->ml_host);
    if (!host->ml_running)
        return false;

    auto *chart = reinterpret_cast<ml_chart_t*>(rd->rrdset->ml_chart);

    bool is_anomalous = ml_dimension_predict(dim, value, exists);
    ml_chart_update_dimension(chart, dim, is_anomalous);

    return is_anomalous;
}

void ml_init()
{
    // Read config values
    ml_config_load(&Cfg);

    if (!Cfg.enable_anomaly_detection)
        return;

    // Generate random numbers to efficiently sample the features we need
    // for KMeans clustering.
    std::random_device RD;
    std::mt19937 Gen(RD());

    Cfg.random_nums.reserve(Cfg.max_training_vectors);
    for (size_t Idx = 0; Idx != Cfg.max_training_vectors; Idx++)
        Cfg.random_nums.push_back(Gen());

    // open sqlite db
    char path[FILENAME_MAX];
    snprintfz(path, FILENAME_MAX - 1, "%s/%s", netdata_configured_cache_dir, "ml.db");
    int rc = sqlite3_open(path, &ml_db);
    if (rc != SQLITE_OK) {
        error_report("Failed to initialize database at %s, due to \"%s\"", path, sqlite3_errstr(rc));
        sqlite3_close(ml_db);
        ml_db = nullptr;
    }

    // create table
    if (ml_db) {
        int target_version = perform_ml_database_migration(ml_db, ML_METADATA_VERSION);
        if (configure_sqlite_database(ml_db, target_version, "ml_config")) {
            error_report("Failed to setup ML database");
            sqlite3_close(ml_db);
            ml_db = nullptr;
        }
        else {
            char *err = nullptr;
            int rc1 = sqlite3_exec(ml_db, db_models_create_table, nullptr, nullptr, &err);
            if (rc1 != SQLITE_OK) {
                error_report("Failed to create models table (%s, %s)", sqlite3_errstr(rc1), err ? err : "");
                sqlite3_close(ml_db);
                sqlite3_free(err);
                ml_db = nullptr;
            }
        }
    }
}

uint64_t sqlite_get_ml_space(void)
{
    return sqlite_get_db_space(ml_db);
}

void ml_fini() {
    if (!Cfg.enable_anomaly_detection || !ml_db)
        return;

    sql_close_database(ml_db, "ML");
    ml_db = nullptr;
}

void ml_start_threads() {
    if (!Cfg.enable_anomaly_detection)
        return;

    // start detection thread
    Cfg.detection_stop = false;

    char tag[NETDATA_THREAD_TAG_MAX + 1];

    snprintfz(tag, NETDATA_THREAD_TAG_MAX, "%s", "PREDICT");
    Cfg.detection_thread = nd_thread_create(tag, NETDATA_THREAD_OPTION_DEFAULT, ml_detect_main, nullptr);

    // Initialize the new training event loop
    ml_training_initialize();
}

void ml_stop_threads()
{
    if (!Cfg.enable_anomaly_detection)
        return;

    Cfg.detection_stop = true;

    if (!Cfg.detection_thread)
        return;

    nd_thread_join(Cfg.detection_thread);
    Cfg.detection_thread = nullptr;

    // Shutdown the training event loop
    ml_training_shutdown();
}

bool ml_model_received_from_child(RRDHOST *host, const char *json)
{
    UNUSED(host);

    bool ok = ml_dimension_deserialize_kmeans(json);
    if (!ok) {
        global_statistics_ml_models_deserialization_failures();
    }

    return ok;
}

void ml_host_disconnected(RRDHOST *rh) {
    auto *host = reinterpret_cast<ml_host_t*>(rh->ml_host);
    if (!host)
        return;

    __atomic_store_n(&host->reset_pointers, true, __ATOMIC_RELAXED);
}

// C accessor functions for ML config (to avoid C++ header includes in C code)
extern "C" time_t ml_get_training_window(void) {
    return Cfg.training_window;
}

extern "C" unsigned ml_get_lag_n(void) {
    return Cfg.lag_n;
}

extern "C" unsigned ml_get_train_every(void) {
    return Cfg.train_every;
}

// Create an ml_worker_t with training buffers and model info storage
extern "C" void *ml_worker_create(size_t buffer_size) {
    auto *worker = new ml_worker_t();

    worker->training_cns = new calculated_number_t[buffer_size]();
    worker->scratch_training_cns = new calculated_number_t[buffer_size]();
    worker->training_samples.clear();
    worker->pending_model_info.reserve(Cfg.flush_models_batch_size);

    worker->stream_payload_buffer = buffer_create(0, nullptr);
    worker->stream_wb_buffer = buffer_create(0, nullptr);

    worker->num_db_transactions = 0;
    worker->num_models_to_prune = 0;

    netdata_mutex_init(&worker->nd_mutex);

    return worker;
}

// Destroy an ml_worker_t and free all its resources
extern "C" void ml_worker_destroy(void *opaque) {
    if (!opaque)
        return;

    auto *worker = static_cast<ml_worker_t *>(opaque);

    delete[] worker->training_cns;
    delete[] worker->scratch_training_cns;

    buffer_free(worker->stream_payload_buffer);
    buffer_free(worker->stream_wb_buffer);

    netdata_mutex_destroy(&worker->nd_mutex);

    delete worker;
}

// Flush all pending models to the database (unconditional)
extern "C" void ml_flush_worker_models(void *opaque) {
    if (!opaque)
        return;

    auto *worker = static_cast<ml_worker_t *>(opaque);
    if (worker->pending_model_info.empty())
        return;

    netdata_mutex_lock(&db_mutex);
    ml_flush_pending_models(worker);
    netdata_mutex_unlock(&db_mutex);
}

// Flush pending models only when the batch threshold is met
extern "C" void ml_flush_worker_models_if_needed(void *opaque) {
    if (!opaque)
        return;

    auto *worker = static_cast<ml_worker_t *>(opaque);
    if (worker->pending_model_info.size() >= Cfg.flush_models_batch_size) {
        netdata_mutex_lock(&db_mutex);
        ml_flush_pending_models(worker);
        netdata_mutex_unlock(&db_mutex);
    }
}

// Return codes for ml_train_dimension_by_uuid:
//   0 to -99:  training results from ml_dimension_train_model (transient, reschedule)
//   <= -100:   dimension is gone or permanently excluded (do NOT reschedule)
#define ML_TRAIN_ERR_INVALID_ARGS   (-100)
#define ML_TRAIN_ERR_DIM_NOT_FOUND  (-101)
#define ML_TRAIN_ERR_NO_ML_DIM      (-102)
#define ML_TRAIN_ERR_ML_DISABLED    (-103)

// C wrapper function for training a dimension by UUID
// Returns 0 on success, negative on failure (see ML_TRAIN_ERR_* above)
extern "C" int ml_train_dimension_by_uuid(UUIDMAP_ID metric_id, void *ml_worker_opaque) {
    if (!metric_id || !ml_worker_opaque)
        return ML_TRAIN_ERR_INVALID_ARGS;

    auto *worker = static_cast<ml_worker_t *>(ml_worker_opaque);

    // Look up and acquire the dimension by UUID (safe reference)
    RRDDIM_ACQUIRED *rda = rrddim_find_and_acquire_by_uuid(metric_id);
    if (!rda)
        return ML_TRAIN_ERR_DIM_NOT_FOUND;

    RRDDIM *rd = rrddim_acquired_to_rrddim(rda);

    // Get the ML dimension
    auto *dim = reinterpret_cast<ml_dimension_t *>(rd->ml_dimension);
    if (!dim) {
        rrddim_acquired_release(rda);
        return ML_TRAIN_ERR_NO_ML_DIM;
    }

    // Check if ML is enabled for this dimension
    if (dim->mls != MACHINE_LEARNING_STATUS_ENABLED) {
        rrddim_acquired_release(rda);
        return ML_TRAIN_ERR_ML_DISABLED;
    }

    // Train the model using the persistent worker
    enum ml_worker_result result = ml_dimension_train_model(worker, dim);

    // Release the acquired reference
    rrddim_acquired_release(rda);

    return (result == ML_WORKER_RESULT_OK) ? 0 : -static_cast<int>(result);
}
