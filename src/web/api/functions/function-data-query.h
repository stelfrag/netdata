// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_FUNCTION_DATA_QUERY_H
#define NETDATA_FUNCTION_DATA_QUERY_H

#include "libnetdata/libnetdata.h"
#include "database/rrdfunctions.h"

// The function a parent calls on a child to run a data query that the parent's
// own database cannot answer. It is hidden from the Functions UI: it is the
// transport for query delegation, not something a user invokes directly.
#define RRDFUNCTIONS_DATA_QUERY "netdata-delegated-query"

#define RRDFUNCTIONS_DATA_QUERY_HELP \
    "Executes a read-only data query on this node on behalf of a parent that does not have the requested data."

// Maximum size of a delegated response body.
//
// The reply travels back to the parent through the child's sender circular
// buffer, which defaults to 10MB (CBUFFER_INITIAL_MAX_SIZE) and is SHARED with
// metric traffic. Overflowing it tears down the streaming link with
// STREAM_HANDSHAKE_DISCONNECT_BUFFER_OVERFLOW. A delegated query must never be
// able to disconnect a child, so we refuse oversized responses instead.
#define RRDFUNCTIONS_DATA_QUERY_MAX_RESPONSE (5ULL * 1024 * 1024)

// How many delegation hops a single query may traverse before it is refused.
// This terminates cycles in active-active parent clusters (A streams to B and
// B streams to A) while still allowing grandparent -> parent -> child chains.
#define RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL 3

// Worker pool sizing. Delegated queries must not run on the stream thread (see
// function-data-query.c), so they are handed to these workers. The queue is
// bounded so that a parent cannot exhaust a child's resources; when it is full
// we answer 503 immediately rather than queueing without limit.
#define RRDFUNCTIONS_DATA_QUERY_WORKERS 2
#define RRDFUNCTIONS_DATA_QUERY_QUEUE_MAX 8

// Payload JSON keys exchanged between the delegating parent and the child.
#define RRDFUNCTIONS_DATA_QUERY_KEY_PATH  "path"
#define RRDFUNCTIONS_DATA_QUERY_KEY_QUERY "query"
#define RRDFUNCTIONS_DATA_QUERY_KEY_TTL   "ttl"

// Registered on localhost by global_functions_add().
int function_data_query(struct rrd_function_execute *rfe, void *data);

// Lifecycle. Started from the daemon after the web server is usable; stopped on
// shutdown. Safe to call stop without a prior start.
void function_data_query_init(void);
void function_data_query_shutdown(void);

// The delegation hop budget still available to the query running on THIS thread.
//
// Returns RRDFUNCTIONS_DATA_QUERY_DEFAULT_TTL for a normal (non-delegated)
// query, and the parent's decremented budget while a delegated query is being
// served. api_v23_data_internal() consults this before delegating further, so
// that a cycle in an active-active parent cluster terminates.
int function_data_query_ttl_remaining(void);

#endif //NETDATA_FUNCTION_DATA_QUERY_H
