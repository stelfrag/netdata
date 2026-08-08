// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_QUERY_DELEGATION_H
#define NETDATA_QUERY_DELEGATION_H

#include "database/rrd.h"

struct web_client;

// Try to answer a data query that THIS agent's database cannot answer, by
// running it on the child that can.
//
// Preconditions the caller must have established:
//   - query_target_create() succeeded and matched no metrics at all
//     (qt->query.used == 0), i.e. we would otherwise return an empty result.
//
// Returns true when the query was delegated AND answered; in that case
// w->response.data holds the child's response verbatim, *code holds its HTTP
// status, and the caller must return immediately without running the local
// query.
//
// Returns false in every other case - no candidate, child too old, hop budget
// exhausted, RPC failure, timeout, oversized response. The caller then proceeds
// with the normal local path, so behaviour is never worse than without
// delegation.
bool query_delegation_try(QUERY_TARGET *qt, struct web_client *w,
                          const char *api_path, const char *original_query_string,
                          int timeout_s, int *code);

#endif //NETDATA_QUERY_DELEGATION_H
