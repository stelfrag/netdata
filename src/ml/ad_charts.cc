// SPDX-License-Identifier: GPL-3.0-or-later

#include "ad_charts.h"
#include "ml_config.h"

void ml_update_dimensions_chart(ml_host_t *host, const ml_machine_learning_stats_t &mls) {

    if(__atomic_load_n(&host->reset_pointers, __ATOMIC_RELAXED)) {
        __atomic_store_n(&host->reset_pointers, false, __ATOMIC_RELAXED);

        host->ml_running_rs = nullptr;
        host->ml_running_rd = nullptr;
        host->machine_learning_status_rs = nullptr;
        host->machine_learning_status_enabled_rd = nullptr;
        host->machine_learning_status_disabled_sp_rd = nullptr;
        host->metric_type_rs = nullptr;
        host->metric_type_constant_rd = nullptr;
        host->metric_type_variable_rd = nullptr;
        host->training_status_rs = nullptr;
        host->training_status_untrained_rd = nullptr;
        host->training_status_pending_without_model_rd = nullptr;
        host->training_status_trained_rd = nullptr;
        host->training_status_pending_with_model_rd = nullptr;
        host->training_status_silenced_rd = nullptr;
        host->dimensions_rs = nullptr;
        host->dimensions_anomalous_rd = nullptr;
        host->dimensions_normal_rd = nullptr;
        host->anomaly_rate_rs = nullptr;
        host->anomaly_rate_rd = nullptr;
        host->detector_events_rs = nullptr;
        host->detector_events_above_threshold_rd = nullptr;
        host->detector_events_new_anomaly_event_rd = nullptr;
        host->context_anomaly_rate_rs = nullptr;
    }

    /*
     * Machine learning status
    */
    if (Cfg.enable_statistics_charts) {
        if (!host->machine_learning_status_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "machine_learning_status_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "machine_learning_status_on_%s", rrdhost_hostname(localhost));

            host->machine_learning_status_rs = rrdset_create(
                    host->rh,
                    "netdata", // type
                    id_buf,
                    name_buf, // name
                    NETDATA_ML_CHART_FAMILY, // family
                    "netdata.ml_status", // ctx
                    "Machine learning status", // title
                    "dimensions", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_TRAINING, // module
                    NETDATA_ML_CHART_PRIO_MACHINE_LEARNING_STATUS, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->machine_learning_status_rs , RRDSET_FLAG_ANOMALY_DETECTION);

            host->machine_learning_status_enabled_rd =
                rrddim_add(host->machine_learning_status_rs, "enabled", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->machine_learning_status_disabled_sp_rd =
                rrddim_add(host->machine_learning_status_rs, "disabled-sp", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->machine_learning_status_rs,
                              host->machine_learning_status_enabled_rd, mls.num_machine_learning_status_enabled);
        rrddim_set_by_pointer(host->machine_learning_status_rs,
                              host->machine_learning_status_disabled_sp_rd, mls.num_machine_learning_status_disabled_sp);

        rrdset_done(host->machine_learning_status_rs);
    }

    /*
     * Metric type
    */
    if (Cfg.enable_statistics_charts) {
        if (!host->metric_type_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "metric_types_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "metric_types_on_%s", rrdhost_hostname(localhost));

            host->metric_type_rs = rrdset_create(
                    host->rh,
                    "netdata", // type
                    id_buf, // id
                    name_buf, // name
                    NETDATA_ML_CHART_FAMILY, // family
                    "netdata.ml_metric_types", // ctx
                    "Dimensions by metric type", // title
                    "dimensions", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_TRAINING, // module
                    NETDATA_ML_CHART_PRIO_METRIC_TYPES, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->metric_type_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->metric_type_constant_rd =
                rrddim_add(host->metric_type_rs, "constant", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->metric_type_variable_rd =
                rrddim_add(host->metric_type_rs, "variable", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->metric_type_rs,
                              host->metric_type_constant_rd, mls.num_metric_type_constant);
        rrddim_set_by_pointer(host->metric_type_rs,
                              host->metric_type_variable_rd, mls.num_metric_type_variable);

        rrdset_done(host->metric_type_rs);
    }

    /*
     * Training status
    */
    if (Cfg.enable_statistics_charts) {
        if (!host->training_status_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "training_status_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "training_status_on_%s", rrdhost_hostname(localhost));

            host->training_status_rs = rrdset_create(
                    host->rh,
                    "netdata", // type
                    id_buf, // id
                    name_buf, // name
                    NETDATA_ML_CHART_FAMILY, // family
                    "netdata.ml_training_status", // ctx
                    "Training status of dimensions", // title
                    "dimensions", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_TRAINING, // module
                    NETDATA_ML_CHART_PRIO_TRAINING_STATUS, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );

            rrdset_flag_set(host->training_status_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->training_status_untrained_rd =
                rrddim_add(host->training_status_rs, "untrained", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->training_status_pending_without_model_rd =
                rrddim_add(host->training_status_rs, "pending-without-model", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->training_status_trained_rd =
                rrddim_add(host->training_status_rs, "trained", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->training_status_pending_with_model_rd =
                rrddim_add(host->training_status_rs, "pending-with-model", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->training_status_silenced_rd =
                rrddim_add(host->training_status_rs, "silenced", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->training_status_rs,
                              host->training_status_untrained_rd, mls.num_training_status_untrained);
        rrddim_set_by_pointer(host->training_status_rs,
                              host->training_status_pending_without_model_rd, mls.num_training_status_pending_without_model);
        rrddim_set_by_pointer(host->training_status_rs,
                              host->training_status_trained_rd, mls.num_training_status_trained);
        rrddim_set_by_pointer(host->training_status_rs,
                              host->training_status_pending_with_model_rd, mls.num_training_status_pending_with_model);
        rrddim_set_by_pointer(host->training_status_rs,
                              host->training_status_silenced_rd, mls.num_training_status_silenced);

        rrdset_done(host->training_status_rs);
    }

    /*
     * Prediction status
    */
    {
        if (!host->dimensions_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "dimensions_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "dimensions_on_%s", rrdhost_hostname(localhost));

            host->dimensions_rs = rrdset_create(
                    host->rh,
                    "anomaly_detection", // type
                    id_buf, // id
                    name_buf, // name
                    "dimensions", // family
                    "anomaly_detection.dimensions", // ctx
                    "Anomaly detection dimensions", // title
                    "dimensions", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_TRAINING, // module
                    ML_CHART_PRIO_DIMENSIONS, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->dimensions_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->dimensions_anomalous_rd =
                rrddim_add(host->dimensions_rs, "anomalous", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->dimensions_normal_rd =
                rrddim_add(host->dimensions_rs, "normal", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->dimensions_rs,
                              host->dimensions_anomalous_rd, mls.num_anomalous_dimensions);
        rrddim_set_by_pointer(host->dimensions_rs,
                              host->dimensions_normal_rd, mls.num_normal_dimensions);

        rrdset_done(host->dimensions_rs);
    }

    // ML running
    {
        if (!host->ml_running_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "ml_running_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "ml_running_on_%s", rrdhost_hostname(localhost));

            host->ml_running_rs = rrdset_create(
                    host->rh,
                    "anomaly_detection", // type
                    id_buf, // id
                    name_buf, // name
                    "anomaly_detection", // family
                    "anomaly_detection.ml_running", // ctx
                    "ML running", // title
                    "boolean", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_DETECTION, // module
                    NETDATA_ML_CHART_RUNNING, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->ml_running_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->ml_running_rd =
                rrddim_add(host->ml_running_rs, "ml_running", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->ml_running_rs,
                              host->ml_running_rd, host->ml_running);
        rrdset_done(host->ml_running_rs);
    }
}

void ml_update_host_and_detection_rate_charts(ml_host_t *host, collected_number AnomalyRate) {
    /*
     * Host anomaly rate
    */
    {
        if (!host->anomaly_rate_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "anomaly_rate_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "anomaly_rate_on_%s", rrdhost_hostname(localhost));

            host->anomaly_rate_rs = rrdset_create(
                    host->rh,
                    "anomaly_detection", // type
                    id_buf, // id
                    name_buf, // name
                    "anomaly_rate", // family
                    "anomaly_detection.anomaly_rate", // ctx
                    "Percentage of anomalous dimensions", // title
                    "percentage", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_DETECTION, // module
                    ML_CHART_PRIO_ANOMALY_RATE, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->anomaly_rate_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->anomaly_rate_rd =
                rrddim_add(host->anomaly_rate_rs, "anomaly_rate", NULL, 1, 100, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(host->anomaly_rate_rs, host->anomaly_rate_rd, AnomalyRate);

        rrdset_done(host->anomaly_rate_rs);
    }

    /*
     * Context anomaly rate
    */
    {
        if (!host->context_anomaly_rate_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "context_anomaly_rate_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "context_anomaly_rate_on_%s", rrdhost_hostname(localhost));

            host->context_anomaly_rate_rs = rrdset_create(
                    host->rh,
                    "anomaly_detection",
                    id_buf,
                    name_buf,
                    "anomaly_rate",
                    "anomaly_detection.context_anomaly_rate",
                    "Percentage of anomalous dimensions by context",
                    "percentage",
                    NETDATA_ML_PLUGIN,
                    NETDATA_ML_MODULE_DETECTION,
                    ML_CHART_PRIO_CONTEXT_ANOMALY_RATE,
                    localhost->rrd_update_every,
                    RRDSET_TYPE_STACKED
            );

            rrdset_flag_set(host->context_anomaly_rate_rs, RRDSET_FLAG_ANOMALY_DETECTION);
        }

        spinlock_lock(&host->context_anomaly_rate_spinlock);
        for (auto &entry : host->context_anomaly_rate) {
            ml_context_anomaly_rate_t &context_anomaly_rate = entry.second;

            if (!context_anomaly_rate.rd)
                context_anomaly_rate.rd = rrddim_add(host->context_anomaly_rate_rs, string2str(entry.first), NULL, 1, 100, RRD_ALGORITHM_ABSOLUTE);

            double ar = 0.0;
            size_t n = context_anomaly_rate.anomalous_dimensions + context_anomaly_rate.normal_dimensions;
            if (n)
                ar = static_cast<double>(context_anomaly_rate.anomalous_dimensions) / n;

            rrddim_set_by_pointer(host->context_anomaly_rate_rs, context_anomaly_rate.rd, ar * 10000.0);

            context_anomaly_rate.anomalous_dimensions = 0;
            context_anomaly_rate.normal_dimensions = 0;
        }
        spinlock_unlock(&host->context_anomaly_rate_spinlock);

        rrdset_done(host->context_anomaly_rate_rs);
    }

    /*
     * Detector Events
    */
    {
        if (!host->detector_events_rs) {
            char id_buf[1024];
            char name_buf[1024];

            snprintfz(id_buf, 1024, "anomaly_detection_on_%s", localhost->machine_guid);
            snprintfz(name_buf, 1024, "anomaly_detection_on_%s", rrdhost_hostname(localhost));

            host->detector_events_rs = rrdset_create(
                    host->rh,
                    "anomaly_detection", // type
                    id_buf, // id
                    name_buf, // name
                    "anomaly_detection", // family
                    "anomaly_detection.detector_events", // ctx
                    "Anomaly detection events", // title
                    "status", // units
                    NETDATA_ML_PLUGIN, // plugin
                    NETDATA_ML_MODULE_DETECTION, // module
                    ML_CHART_PRIO_DETECTOR_EVENTS, // priority
                    localhost->rrd_update_every, // update_every
                    RRDSET_TYPE_LINE // chart_type
            );
            rrdset_flag_set(host->detector_events_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            host->detector_events_above_threshold_rd =
                rrddim_add(host->detector_events_rs, "above_threshold", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            host->detector_events_new_anomaly_event_rd =
                rrddim_add(host->detector_events_rs, "new_anomaly_event", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        /*
         * Compute the values of the dimensions based on the host rate chart
        */
        if (host->ml_running) {
            ONEWAYALLOC *OWA = onewayalloc_create(0);
            time_t Now = now_realtime_sec();
            time_t Before = Now - host->rh->rrd_update_every;
            time_t After = Before - Cfg.anomaly_detection_query_duration;
            RRDR_OPTIONS Options = static_cast<RRDR_OPTIONS>(0x00000000);

            RRDR *R = rrd2rrdr_legacy(
                    OWA,
                    host->anomaly_rate_rs,
                    1 /* points wanted */,
                    After,
                    Before,
                    Cfg.anomaly_detection_grouping_method,
                    0 /* resampling time */,
                    Options, "anomaly_rate",
                    NULL /* group options */,
                    0, /* timeout */
                    0, /* tier */
                    QUERY_SOURCE_ML,
                    STORAGE_PRIORITY_SYNCHRONOUS
            );

            if (R) {
                if (R->d == 1 && R->n == 1 && R->rows == 1) {
                    static thread_local bool prev_above_threshold = false;
                    bool above_threshold = R->v[0] >= Cfg.host_anomaly_rate_threshold;
                    bool new_anomaly_event = above_threshold && !prev_above_threshold;
                    prev_above_threshold = above_threshold;

                    rrddim_set_by_pointer(host->detector_events_rs,
                                          host->detector_events_above_threshold_rd, above_threshold);
                    rrddim_set_by_pointer(host->detector_events_rs,
                                          host->detector_events_new_anomaly_event_rd, new_anomaly_event);

                    rrdset_done(host->detector_events_rs);
                }

                rrdr_free(OWA, R);
            }

            onewayalloc_destroy(OWA);
        } else {
            rrddim_set_by_pointer(host->detector_events_rs,
                                  host->detector_events_above_threshold_rd, 0);
            rrddim_set_by_pointer(host->detector_events_rs,
                                  host->detector_events_new_anomaly_event_rd, 0);
            rrdset_done(host->detector_events_rs);
        }
    }
}

void ml_update_event_loop_training_chart(const ml_training_stats_t &stats) {
    // Static chart/dimension pointers (single event loop, not N workers)
    static RRDSET *queue_ops_rs = nullptr;
    static RRDDIM *queue_ops_queued_rd = nullptr;
    static RRDDIM *queue_ops_trained_rd = nullptr;

    static RRDSET *queue_size_rs = nullptr;
    static RRDDIM *queue_size_pending_rd = nullptr;
    static RRDDIM *queue_size_scheduled_rd = nullptr;

    static RRDSET *training_results_rs = nullptr;
    static RRDDIM *results_ok_rd = nullptr;
    static RRDDIM *results_invalid_query_rd = nullptr;
    static RRDDIM *results_not_enough_values_rd = nullptr;
    static RRDDIM *results_dim_not_found_rd = nullptr;
    static RRDDIM *results_chart_replication_rd = nullptr;

    // Queue operations chart
    {
        if (!queue_ops_rs) {
            queue_ops_rs = rrdset_create(
                    localhost,
                    "netdata",
                    "ml_training_queue_ops",
                    "ml_training_queue_ops",
                    NETDATA_ML_CHART_FAMILY,
                    "netdata.ml_queue_ops",
                    "Training queue operations",
                    "count",
                    NETDATA_ML_PLUGIN,
                    NETDATA_ML_MODULE_TRAINING,
                    NETDATA_ML_CHART_PRIO_QUEUE_STATS,
                    localhost->rrd_update_every,
                    RRDSET_TYPE_LINE
            );
            rrdset_flag_set(queue_ops_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            queue_ops_queued_rd = rrddim_add(queue_ops_rs, "queued", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            queue_ops_trained_rd = rrddim_add(queue_ops_rs, "trained", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrddim_set_by_pointer(queue_ops_rs, queue_ops_queued_rd, (collected_number)stats.total_queued);
        rrddim_set_by_pointer(queue_ops_rs, queue_ops_trained_rd, (collected_number)stats.total_trained);
        rrdset_done(queue_ops_rs);
    }

    // Queue size chart
    {
        if (!queue_size_rs) {
            queue_size_rs = rrdset_create(
                    localhost,
                    "netdata",
                    "ml_training_queue_size",
                    "ml_training_queue_size",
                    NETDATA_ML_CHART_FAMILY,
                    "netdata.ml_queue_size",
                    "Training queue size",
                    "dimensions",
                    NETDATA_ML_PLUGIN,
                    NETDATA_ML_MODULE_TRAINING,
                    NETDATA_ML_CHART_PRIO_QUEUE_STATS + 1,
                    localhost->rrd_update_every,
                    RRDSET_TYPE_LINE
            );
            rrdset_flag_set(queue_size_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            queue_size_pending_rd = rrddim_add(queue_size_rs, "pending", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
            queue_size_scheduled_rd = rrddim_add(queue_size_rs, "scheduled", NULL, 1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(queue_size_rs, queue_size_pending_rd, (collected_number)stats.queue_depth);
        rrddim_set_by_pointer(queue_size_rs, queue_size_scheduled_rd, (collected_number)stats.scheduled_count);
        rrdset_done(queue_size_rs);
    }

    // Training results chart
    {
        if (!training_results_rs) {
            training_results_rs = rrdset_create(
                    localhost,
                    "netdata",
                    "ml_training_results",
                    "ml_training_results",
                    NETDATA_ML_CHART_FAMILY,
                    "netdata.ml_training_results",
                    "Training results",
                    "events",
                    NETDATA_ML_PLUGIN,
                    NETDATA_ML_MODULE_TRAINING,
                    NETDATA_ML_CHART_PRIO_TRAINING_RESULTS,
                    localhost->rrd_update_every,
                    RRDSET_TYPE_LINE
            );
            rrdset_flag_set(training_results_rs, RRDSET_FLAG_ANOMALY_DETECTION);

            results_ok_rd = rrddim_add(training_results_rs, "ok", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            results_invalid_query_rd = rrddim_add(training_results_rs, "invalid-queries", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            results_not_enough_values_rd = rrddim_add(training_results_rs, "not-enough-values", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            results_dim_not_found_rd = rrddim_add(training_results_rs, "null-acquired-dimensions", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            results_chart_replication_rd = rrddim_add(training_results_rs, "chart-under-replication", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrddim_set_by_pointer(training_results_rs, results_ok_rd, (collected_number)stats.result_ok);
        rrddim_set_by_pointer(training_results_rs, results_invalid_query_rd, (collected_number)stats.result_invalid_query_time_range);
        rrddim_set_by_pointer(training_results_rs, results_not_enough_values_rd, (collected_number)stats.result_not_enough_collected_values);
        rrddim_set_by_pointer(training_results_rs, results_dim_not_found_rd, (collected_number)stats.result_dim_not_found);
        rrddim_set_by_pointer(training_results_rs, results_chart_replication_rd, (collected_number)stats.result_chart_under_replication);
        rrdset_done(training_results_rs);
    }
}

void ml_update_global_statistics_charts(uint64_t models_consulted,
                                        uint64_t models_received,
                                        uint64_t models_sent,
                                        uint64_t models_ignored,
                                        uint64_t models_deserialization_failures,
                                        uint64_t memory_consumption,
                                        uint64_t memory_new,
                                        uint64_t memory_delete)
{
    if (!Cfg.enable_statistics_charts)
        return;

    {
        static RRDSET *st = NULL;
        static RRDDIM *rd = NULL;

        if (unlikely(!st)) {
            st = rrdset_create_localhost(
                    "netdata" // type
                    , "ml_models_consulted" // id
                    , NULL // name
                    , NETDATA_ML_CHART_FAMILY // family
                    , NULL // context
                    , "KMeans models used for prediction" // title
                    , "models" // units
                    , NETDATA_ML_PLUGIN // plugin
                    , NETDATA_ML_MODULE_DETECTION // module
                    , NETDATA_ML_CHART_PRIO_MACHINE_LEARNING_STATUS // priority
                    , localhost->rrd_update_every // update_every
                    , RRDSET_TYPE_AREA // chart_type
            );

            rd = rrddim_add(st, "num_models_consulted", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrddim_set_by_pointer(st, rd, (collected_number) models_consulted);

        rrdset_done(st);
    }

    {
        static RRDSET *st = NULL;
        static RRDDIM *rd_received = NULL;
        static RRDDIM *rd_sent = NULL;
        static RRDDIM *rd_ignored = NULL;
        static RRDDIM *rd_deserialization_failures = NULL;

        if (unlikely(!st)) {
            st = rrdset_create_localhost(
                    "netdata" // type
                    , "ml_models_streamed" // id
                    , NULL // name
                    , NETDATA_ML_CHART_FAMILY // family
                    , NULL // context
                    , "KMeans models streamed" // title
                    , "models" // units
                    , NETDATA_ML_PLUGIN // plugin
                    , NETDATA_ML_MODULE_DETECTION // module
                    , NETDATA_ML_CHART_PRIO_MACHINE_LEARNING_STATUS // priority
                    , localhost->rrd_update_every // update_every
                    , RRDSET_TYPE_LINE // chart_type
            );

            rd_received = rrddim_add(st, "received", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            rd_sent = rrddim_add(st, "sent", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            rd_ignored = rrddim_add(st, "ignored", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            rd_deserialization_failures = rrddim_add(st, "deserialization failures", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrddim_set_by_pointer(st, rd_received, (collected_number) models_received);
        rrddim_set_by_pointer(st, rd_sent, (collected_number) models_sent);
        rrddim_set_by_pointer(st, rd_ignored, (collected_number) models_ignored);
        rrddim_set_by_pointer(st, rd_deserialization_failures, (collected_number) models_deserialization_failures);

        rrdset_done(st);
    }

    {
        static RRDSET *st = NULL;
        static RRDDIM *rd_memory_consumption = NULL;

        if (unlikely(!st)) {
            st = rrdset_create_localhost(
                    "netdata" // type
                    , "ml_memory_used" // id
                    , NULL // name
                    , NETDATA_ML_CHART_FAMILY // family
                    , NULL // context
                    , "ML memory usage" // title
                    , "bytes" // units
                    , NETDATA_ML_PLUGIN // plugin
                    , NETDATA_ML_MODULE_DETECTION // module
                    , NETDATA_ML_CHART_PRIO_MACHINE_LEARNING_STATUS // priority
                    , localhost->rrd_update_every // update_every
                    , RRDSET_TYPE_LINE // chart_type
            );

            rd_memory_consumption = rrddim_add(st, "used", NULL, 1024, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_memory_consumption, (collected_number) memory_consumption / (1024));
        rrdset_done(st);
    }

    {
        static RRDSET *st = NULL;
        static RRDDIM *rd_memory_new = NULL;
        static RRDDIM *rd_memory_delete = NULL;

        if (unlikely(!st)) {
            st = rrdset_create_localhost(
                    "netdata" // type
                    , "ml_memory_ops" // id
                    , NULL // name
                    , NETDATA_ML_CHART_FAMILY // family
                    , NULL // context
                    , "ML memory operations" // title
                    , "count" // units
                    , NETDATA_ML_PLUGIN // plugin
                    , NETDATA_ML_MODULE_DETECTION // module
                    , NETDATA_ML_CHART_PRIO_MACHINE_LEARNING_STATUS // priority
                    , localhost->rrd_update_every // update_every
                    , RRDSET_TYPE_LINE // chart_type
            );

            rd_memory_new = rrddim_add(st, "new", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
            rd_memory_delete = rrddim_add(st, "delete", NULL, 1, 1, RRD_ALGORITHM_INCREMENTAL);
        }

        rrddim_set_by_pointer(st, rd_memory_new, (collected_number) memory_new);
        rrddim_set_by_pointer(st, rd_memory_delete, (collected_number) memory_delete);
        rrdset_done(st);
    }
}
