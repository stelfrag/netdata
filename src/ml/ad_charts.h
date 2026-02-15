// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ML_ADCHARTS_H
#define ML_ADCHARTS_H

#include "ml_private.h"
#include "ml_event_loop.h"

void ml_update_dimensions_chart(ml_host_t *host, const ml_machine_learning_stats_t &mls);

void ml_update_host_and_detection_rate_charts(ml_host_t *host, collected_number anomaly_rate);

void ml_update_event_loop_training_chart(const ml_training_stats_t &stats);

#endif /* ML_ADCHARTS_H */
