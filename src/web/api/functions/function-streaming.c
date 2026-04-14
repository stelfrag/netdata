// SPDX-License-Identifier: GPL-3.0-or-later

#include "function-streaming.h"

#include <ctype.h>

#define STREAMING_FUNCTION_UPDATE_EVERY 10

#define GROUP_BY_COLUMN(name, descr) \
    buffer_json_member_add_object(wb, name);\
    {\
        buffer_json_member_add_string(wb, "name", descr);\
        buffer_json_member_add_array(wb, "columns");\
        {\
            buffer_json_add_array_item_string(wb, name);\
        }\
        buffer_json_array_close(wb);\
    }\
    buffer_json_object_close(wb);

struct streaming_topology_filters;
struct streaming_topology_host_state;
struct streaming_topology_runtime;

static bool streaming_topology_host_guid(RRDHOST *host, char *dst, size_t dst_size);
static bool streaming_topology_uuid_guid(ND_UUID host_id, char *dst, size_t dst_size);
static void streaming_topology_agent_id_for_host(RRDHOST *host, char *dst, size_t dst_size);
static void streaming_topology_actor_id_from_guid(const char *guid, char *dst, size_t dst_size);
static void streaming_topology_actor_id_for_uuid(ND_UUID host_id, char *dst, size_t dst_size);
static void streaming_topology_actor_id_for_host(RRDHOST *host, char *dst, size_t dst_size);
static uint32_t *streaming_topology_parent_child_count_get(DICTIONARY *parent_child_count, RRDHOST *host);
static struct streaming_topology_descendant_list *streaming_topology_descendants_get(DICTIONARY *parent_descendants, RRDHOST *host);
static bool streaming_topology_visible_actor_set(DICTIONARY *visible_actors, const char *actor_id);
static bool streaming_topology_visible_actor_exists(DICTIONARY *visible_actors, const char *actor_id);
static const char *streaming_topology_visible_actor_id(DICTIONARY *visible_actors, const char *actor_id);
static const char *streaming_topology_node_type(DICTIONARY *parent_child_count, RRDHOST *host, const RRDHOST_STATUS *status);
static struct streaming_topology_host_state *streaming_topology_host_state_get(DICTIONARY *host_states, RRDHOST *host);
static bool streaming_topology_host_state_set(DICTIONARY *host_states, DICTIONARY *parent_child_count, RRDHOST *host, time_t now);
static void streaming_topology_host_states_cleanup(DICTIONARY *host_states);
static const ND_UUID *streaming_topology_path_previous_uuid(const struct streaming_topology_host_state *hs, ND_UUID target_uuid);
static const ND_UUID *streaming_topology_path_next_uuid(const struct streaming_topology_host_state *hs, ND_UUID target_uuid);
static uint16_t streaming_topology_get_path_ids(RRDHOST *host, uint16_t from, ND_UUID *host_ids, uint16_t max);
static bool value_in_csv(const char *csv, const char *value);
static bool streaming_topology_host_matches_filters(const struct streaming_topology_filters *filters, const struct streaming_topology_host_state *hs);
static void streaming_topology_runtime_cleanup(struct streaming_topology_runtime *runtime);
static const char *streaming_topology_host_severity(RRDHOST *host, const RRDHOST_STATUS *status);
static void streaming_topology_emit_filtered_streaming_path(BUFFER *wb, struct streaming_topology_runtime *runtime, RRDHOST *host, const struct streaming_topology_host_state *hs);
static void streaming_topology_emit_actors(BUFFER *wb, struct streaming_topology_runtime *runtime, const struct streaming_topology_filters *filters, size_t *actors_total);
static void streaming_topology_emit_links(BUFFER *wb, struct streaming_topology_runtime *runtime, const struct streaming_topology_filters *filters, time_t now, usec_t now_ut, size_t *links_total);

enum streaming_topology_received_type {
    STREAMING_TOPOLOGY_RECEIVED_STREAMING = 0,
    STREAMING_TOPOLOGY_RECEIVED_VIRTUAL,
    STREAMING_TOPOLOGY_RECEIVED_STALE,
};

static const char *streaming_topology_received_type_to_string(enum streaming_topology_received_type type);

struct streaming_topology_descendant {
    RRDHOST *host;
    ND_UUID source_uuid;
    bool source_local;
    uint8_t type;
};

struct streaming_topology_descendant_list {
    struct streaming_topology_descendant *items;
    size_t used;
    size_t size;
};

struct streaming_topology_filters {
    bool info_only;
    const char *node_type;
    const char *ingest_status;
    const char *stream_status;
    char *function_copy;
};

struct streaming_topology_host_state {
    RRDHOST_STATUS status;
    char actor_id[256];
    const char *node_type;
    uint32_t child_count;
    ND_UUID *path_ids;
    uint16_t path_len;
};

struct streaming_topology_runtime {
    DICTIONARY *parent_child_count;
    DICTIONARY *parent_descendants;
    DICTIONARY *visible_actors;
    DICTIONARY *host_states;
};

static void streaming_topology_add_host_match(BUFFER *wb, RRDHOST *host) {
    buffer_json_member_add_object(wb, "match");
    {
        buffer_json_member_add_array(wb, "hostnames");
        {
            buffer_json_add_array_item_string(wb, rrdhost_hostname(host));
        }
        buffer_json_array_close(wb);

        char host_guid[UUID_STR_LEN];
        if(streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
            buffer_json_member_add_string(wb, "netdata_machine_guid", host_guid);

        if(!UUIDiszero(host->node_id))
            buffer_json_member_add_uuid(wb, "netdata_node_id", host->node_id.uuid);
    }
    buffer_json_object_close(wb);
}

static bool streaming_topology_host_guid(RRDHOST *host, char *dst, size_t dst_size) {
    if(!dst || dst_size < UUID_STR_LEN)
        return false;

    dst[0] = '\0';
    if(!host)
        return false;

    if(streaming_topology_uuid_guid(host->host_id, dst, dst_size))
        return true;

    if(host->machine_guid[0]) {
        ND_UUID machine_guid = UUID_ZERO;
        if(!uuid_parse(host->machine_guid, machine_guid.uuid))
            return streaming_topology_uuid_guid(machine_guid, dst, dst_size);
    }

    return false;
}

static bool streaming_topology_uuid_guid(ND_UUID host_id, char *dst, size_t dst_size) {
    if(!dst || dst_size < UUID_STR_LEN)
        return false;

    dst[0] = '\0';
    if(UUIDiszero(host_id))
        return false;

    uuid_unparse_lower(host_id.uuid, dst);
    return true;
}

static void streaming_topology_actor_id_from_guid(const char *guid, char *dst, size_t dst_size) {
    if(!dst || !dst_size)
        return;

    if(guid && *guid)
        snprintf(dst, dst_size, "netdata-machine-guid:%s", guid);
    else
        snprintf(dst, dst_size, "host:unknown");
}

static void streaming_topology_agent_id_for_host(RRDHOST *host, char *dst, size_t dst_size) {
    if(!dst || !dst_size)
        return;

    char host_guid[UUID_STR_LEN];
    if(streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        snprintf(dst, dst_size, "%s", host_guid);
    else if(host)
        snprintf(dst, dst_size, "%s", rrdhost_hostname(host));
    else
        dst[0] = '\0';
}

static void streaming_topology_actor_id_for_uuid(ND_UUID host_id, char *dst, size_t dst_size) {
    char guid[UUID_STR_LEN];
    if(streaming_topology_uuid_guid(host_id, guid, sizeof(guid)))
        streaming_topology_actor_id_from_guid(guid, dst, dst_size);
    else
        streaming_topology_actor_id_from_guid(NULL, dst, dst_size);
}

static uint32_t *streaming_topology_parent_child_count_get(DICTIONARY *parent_child_count, RRDHOST *host) {
    char host_guid[UUID_STR_LEN];

    if(!streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        return NULL;

    return dictionary_get(parent_child_count, host_guid);
}

static struct streaming_topology_descendant_list *streaming_topology_descendants_get(DICTIONARY *parent_descendants, RRDHOST *host) {
    char host_guid[UUID_STR_LEN];

    if(!streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        return NULL;

    return dictionary_get(parent_descendants, host_guid);
}

static bool streaming_topology_visible_actor_set(DICTIONARY *visible_actors, const char *actor_id) {
    if(!visible_actors || !actor_id || !*actor_id)
        return false;

    bool visible = true;
    return dictionary_set(visible_actors, actor_id, &visible, sizeof(visible)) != NULL;
}

static bool streaming_topology_visible_actor_exists(DICTIONARY *visible_actors, const char *actor_id) {
    if(!visible_actors || !actor_id || !*actor_id)
        return false;

    return dictionary_get(visible_actors, actor_id) != NULL;
}

static const char *streaming_topology_visible_actor_id(DICTIONARY *visible_actors, const char *actor_id) {
    return streaming_topology_visible_actor_exists(visible_actors, actor_id) ? actor_id : NULL;
}

static const char *streaming_topology_node_type(DICTIONARY *parent_child_count, RRDHOST *host, const RRDHOST_STATUS *status) {
    if(!host || !status)
        return "child";

    if(rrdhost_is_virtual(host))
        return "vnode";

    if(host != localhost && status->ingest.status == RRDHOST_INGEST_STATUS_ARCHIVED)
        return "stale";

    uint32_t *cc = streaming_topology_parent_child_count_get(parent_child_count, host);
    return (cc && *cc > 0) ? "parent" : "child";
}

static struct streaming_topology_host_state *streaming_topology_host_state_get(DICTIONARY *host_states, RRDHOST *host) {
    char host_guid[UUID_STR_LEN];

    if(!host_states || !streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        return NULL;

    return dictionary_get(host_states, host_guid);
}

static bool streaming_topology_host_state_set(DICTIONARY *host_states, DICTIONARY *parent_child_count, RRDHOST *host, time_t now) {
    char host_guid[UUID_STR_LEN];

    if(!host_states || !streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        return false;

    struct streaming_topology_host_state state = { 0 };
    rrdhost_status(host, now, &state.status, RRDHOST_STATUS_ALL);
    streaming_topology_actor_id_for_host(host, state.actor_id, sizeof(state.actor_id));
    state.node_type = streaming_topology_node_type(parent_child_count, host, &state.status);

    uint32_t *cc = streaming_topology_parent_child_count_get(parent_child_count, host);
    state.child_count = cc ? *cc : 0;

    ND_UUID path_ids[128];
    state.path_len = streaming_topology_get_path_ids(host, 0, path_ids, 128);
    if(state.path_len) {
        state.path_ids = mallocz(state.path_len * sizeof(*state.path_ids));
        memcpy(state.path_ids, path_ids, state.path_len * sizeof(*state.path_ids));
    }

    bool ok = dictionary_set(host_states, host_guid, &state, sizeof(state)) != NULL;
    if(!ok)
        freez(state.path_ids);

    return ok;
}

static void streaming_topology_host_states_cleanup(DICTIONARY *host_states) {
    if(!host_states)
        return;

    struct streaming_topology_host_state *hs;
    dfe_start_write(host_states, hs) {
        freez(hs->path_ids);
        hs->path_ids = NULL;
        hs->path_len = 0;
    }
    dfe_done(hs);
}

static const ND_UUID *streaming_topology_path_previous_uuid(const struct streaming_topology_host_state *hs, ND_UUID target_uuid) {
    if(!hs || !hs->path_ids || !hs->path_len)
        return NULL;

    for(uint16_t i = 1; i < hs->path_len; i++) {
        if(UUIDeq(hs->path_ids[i], target_uuid))
            return &hs->path_ids[i - 1];
    }

    return NULL;
}

static const ND_UUID *streaming_topology_path_next_uuid(const struct streaming_topology_host_state *hs, ND_UUID target_uuid) {
    if(!hs || !hs->path_ids || !hs->path_len)
        return NULL;

    for(uint16_t i = 0; i + 1 < hs->path_len; i++) {
        if(UUIDeq(hs->path_ids[i], target_uuid))
            return &hs->path_ids[i + 1];
    }

    return NULL;
}

static bool streaming_topology_host_matches_filters(const struct streaming_topology_filters *filters, const struct streaming_topology_host_state *hs) {
    if(!hs)
        return false;

    if(!filters)
        return true;

    if(!value_in_csv(filters->node_type, hs->node_type))
        return false;

    if(!value_in_csv(filters->ingest_status, rrdhost_ingest_status_to_string(hs->status.ingest.status)))
        return false;

    if(!value_in_csv(filters->stream_status, rrdhost_streaming_status_to_string(hs->status.stream.status)))
        return false;

    return true;
}

static void streaming_topology_runtime_cleanup(struct streaming_topology_runtime *runtime) {
    if(!runtime)
        return;

    if(runtime->parent_descendants) {
        struct streaming_topology_descendant_list *descendants;
        dfe_start_write(runtime->parent_descendants, descendants) {
            freez(descendants->items);
            descendants->items = NULL;
            descendants->used = 0;
            descendants->size = 0;
        }
        dfe_done(descendants);
        dictionary_destroy(runtime->parent_descendants);
        runtime->parent_descendants = NULL;
    }

    if(runtime->parent_child_count) {
        dictionary_destroy(runtime->parent_child_count);
        runtime->parent_child_count = NULL;
    }

    if(runtime->visible_actors) {
        dictionary_destroy(runtime->visible_actors);
        runtime->visible_actors = NULL;
    }

    if(runtime->host_states) {
        streaming_topology_host_states_cleanup(runtime->host_states);
        dictionary_destroy(runtime->host_states);
        runtime->host_states = NULL;
    }
}

static const char *streaming_topology_host_severity(RRDHOST *host, const RRDHOST_STATUS *status) {
    if(!host || !status)
        return "normal";

    const char *severity = "normal";
    if(!rrdhost_option_check(host, RRDHOST_OPTION_EPHEMERAL_HOST)) {
        switch(status->ingest.status) {
            case RRDHOST_INGEST_STATUS_OFFLINE:
            case RRDHOST_INGEST_STATUS_ARCHIVED:
                severity = "critical";
                break;

            default:
                break;
        }

        if(strcmp(severity, "normal") == 0) {
            switch(status->stream.status) {
                case RRDHOST_STREAM_STATUS_OFFLINE:
                    if(status->stream.reason != STREAM_HANDSHAKE_SP_NO_DESTINATION)
                        severity = "warning";
                    break;

                default:
                    break;
            }
        }
    }

    return severity;
}

static void streaming_topology_emit_filtered_streaming_path(BUFFER *wb, struct streaming_topology_runtime *runtime, RRDHOST *host, const struct streaming_topology_host_state *hs) {
    if(!wb || !runtime || !host || !hs) {
        buffer_json_member_add_array(wb, "streaming_path");
        buffer_json_array_close(wb);
        return;
    }

    buffer_json_member_add_array(wb, "streaming_path");
    {
        for(uint16_t pi = 0; pi < hs->path_len; pi++) {
            char path_actor_id[256];
            char path_guid[UUID_STR_LEN];
            const char *hostname = NULL;
            time_t since = 0;
            char flags[64] = "";
            bool first_flag = true;

            streaming_topology_actor_id_for_uuid(hs->path_ids[pi], path_actor_id, sizeof(path_actor_id));
            if(!streaming_topology_visible_actor_exists(runtime->visible_actors, path_actor_id))
                continue;

            RRDHOST *path_host = NULL;
            if(UUIDeq(hs->path_ids[pi], localhost->host_id))
                path_host = localhost;
            else if(streaming_topology_uuid_guid(hs->path_ids[pi], path_guid, sizeof(path_guid)))
                path_host = rrdhost_find_by_guid(path_guid);

            if(path_host)
                hostname = rrdhost_hostname(path_host);
            if(!hostname)
                hostname = UUIDeq(hs->path_ids[pi], localhost->host_id) ? rrdhost_hostname(localhost) : path_guid;

            if(path_host) {
                struct streaming_topology_host_state *path_hs = streaming_topology_host_state_get(runtime->host_states, path_host);
                if(path_hs)
                    since = path_hs->status.ingest.since;

                if(rrdhost_option_check(path_host, RRDHOST_OPTION_EPHEMERAL_HOST)) {
                    snprintfz(flags + strlen(flags), sizeof(flags) - strlen(flags), "%sephemeral", first_flag ? "" : ",");
                    first_flag = false;
                }
                if(rrdhost_is_virtual(path_host)) {
                    snprintfz(flags + strlen(flags), sizeof(flags) - strlen(flags), "%svirtual", first_flag ? "" : ",");
                    first_flag = false;
                }
                if(path_host->health.enabled) {
                    snprintfz(flags + strlen(flags), sizeof(flags) - strlen(flags), "%shealth", first_flag ? "" : ",");
                    first_flag = false;
                }
                if(ml_enabled(path_host)) {
                    snprintfz(flags + strlen(flags), sizeof(flags) - strlen(flags), "%sml", first_flag ? "" : ",");
                    first_flag = false;
                }
            }

            buffer_json_add_array_item_object(wb);
            {
                buffer_json_member_add_string(wb, "hostname", hostname);
                buffer_json_member_add_uint64(wb, "hops", pi);
                buffer_json_member_add_uint64(wb, "since", since);
                buffer_json_member_add_string(wb, "flags", flags);
            }
            buffer_json_object_close(wb);
        }
    }
    buffer_json_array_close(wb);
}

static void streaming_topology_emit_actors(BUFFER *wb, struct streaming_topology_runtime *runtime, const struct streaming_topology_filters *filters, size_t *actors_total) {
    if(!wb || !runtime)
        return;

    DICTIONARY *parent_descendants = runtime->parent_descendants;
    DICTIONARY *visible_actors = runtime->visible_actors;
    DICTIONARY *host_states = runtime->host_states;

    buffer_json_member_add_array(wb, "actors");
    {
        RRDHOST *host;
        dfe_start_read(rrdhost_root_index, host) {
            struct streaming_topology_host_state *hs = streaming_topology_host_state_get(host_states, host);
            if(!hs)
                continue;

            RRDHOST_STATUS *s = &hs->status;
            const char *hostname = rrdhost_hostname(host);
            const char *node_type = hs->node_type;

            if(!streaming_topology_host_matches_filters(filters, hs))
                continue;

            uint32_t child_count = hs->child_count;
            const char *severity = streaming_topology_host_severity(host, s);

            if(actors_total)
                (*actors_total)++;
            streaming_topology_visible_actor_set(visible_actors, hs->actor_id);
            buffer_json_add_array_item_object(wb);
            {
                buffer_json_member_add_string(wb, "actor_id", hs->actor_id);
                buffer_json_member_add_string(wb, "actor_type", node_type);
                buffer_json_member_add_string(wb, "layer", "infra");
                buffer_json_member_add_string(wb, "source", "streaming");
                streaming_topology_add_host_match(wb, host);

                buffer_json_member_add_object(wb, "attributes");
                {
                    buffer_json_member_add_string(wb, "display_name", hostname);
                    buffer_json_member_add_string(wb, "node_type", node_type);
                    buffer_json_member_add_string(wb, "severity", severity);
                    buffer_json_member_add_uint64(wb, "child_count", child_count);
                    buffer_json_member_add_string(wb, "ephemerality", rrdhost_option_check(host, RRDHOST_OPTION_EPHEMERAL_HOST) ? "ephemeral" : "permanent");
                    buffer_json_member_add_string(wb, "agent_name", rrdhost_program_name(host));
                    buffer_json_member_add_string(wb, "agent_version", rrdhost_program_version(host));
                    rrdhost_system_info_to_json_object_fields(wb, s->host->system_info);
                    buffer_json_member_add_string(wb, "health_status", rrdhost_health_status_to_string(s->health.status));
                    if(s->health.status == RRDHOST_HEALTH_STATUS_RUNNING) {
                        buffer_json_member_add_uint64(wb, "health_critical", s->health.alerts.critical);
                        buffer_json_member_add_uint64(wb, "health_warning", s->health.alerts.warning);
                        buffer_json_member_add_uint64(wb, "health_clear", s->health.alerts.clear);
                    }
                    else {
                        buffer_json_member_add_uint64(wb, "health_critical", 0);
                        buffer_json_member_add_uint64(wb, "health_warning", 0);
                        buffer_json_member_add_uint64(wb, "health_clear", 0);
                    }
                }
                buffer_json_object_close(wb);

                buffer_json_member_add_object(wb, "labels");
                {
                    buffer_json_member_add_string(wb, "hostname", hostname);
                    buffer_json_member_add_string(wb, "node_type", node_type);
                    buffer_json_member_add_string(wb, "severity", severity);
                    buffer_json_member_add_string(wb, "ephemerality", rrdhost_option_check(host, RRDHOST_OPTION_EPHEMERAL_HOST) ? "ephemeral" : "permanent");
                    buffer_json_member_add_string(wb, "ingest_status", rrdhost_ingest_status_to_string(s->ingest.status));
                    buffer_json_member_add_string(wb, "stream_status", rrdhost_streaming_status_to_string(s->stream.status));
                    buffer_json_member_add_string(wb, "ml_status", rrdhost_ml_status_to_string(s->ml.status));
                    buffer_json_member_add_string(wb, "display_name", hostname);
                    buffer_json_member_add_object(wb, "host_labels");
                    {
                        rrdlabels_to_buffer_json_members(host->rrdlabels, wb);
                    }
                    buffer_json_object_close(wb);
                }
                buffer_json_object_close(wb);

                buffer_json_member_add_array(wb, "streaming_path");
                for(uint16_t pi = 0; pi < hs->path_len; pi++) {
                    char path_actor_id[256];
                    streaming_topology_actor_id_for_uuid(hs->path_ids[pi], path_actor_id, sizeof(path_actor_id));
                    if(!streaming_topology_visible_actor_exists(visible_actors, path_actor_id))
                        continue;
                    buffer_json_add_array_item_string(wb, path_actor_id);
                }
                buffer_json_array_close(wb);

                if(child_count > 0) {
                    buffer_json_member_add_array(wb, "received_nodes");
                    struct streaming_topology_descendant_list *received_nodes =
                        streaming_topology_descendants_get(parent_descendants, host);
                    if(received_nodes) {
                        for(size_t i = 0; i < received_nodes->used; i++) {
                            struct streaming_topology_descendant *descendant = &received_nodes->items[i];
                            RRDHOST *rn_host = descendant->host;
                            struct streaming_topology_host_state *rnhs = streaming_topology_host_state_get(host_states, rn_host);

                            if(rn_host == host || !rnhs)
                                continue;

                            if(!streaming_topology_visible_actor_exists(visible_actors, rnhs->actor_id))
                                continue;

                            buffer_json_add_array_item_object(wb);
                            buffer_json_member_add_string(wb, "name", rrdhost_hostname(rn_host));
                            buffer_json_member_add_string(wb, "type",
                                streaming_topology_received_type_to_string((enum streaming_topology_received_type)descendant->type));
                            buffer_json_object_close(wb);
                        }
                    }
                    buffer_json_array_close(wb);
                }

                buffer_json_member_add_object(wb, "tables");
                {
                    bool is_observer = (host == localhost);

                    if(child_count > 0) {
                        if(is_observer) {
                            buffer_json_member_add_array(wb, "inbound");
                            {
                                RRDHOST *ih;
                                dfe_start_read(rrdhost_root_index, ih) {
                                    struct streaming_topology_host_state *ihs = streaming_topology_host_state_get(host_states, ih);
                                    if(!ihs)
                                        continue;

                                    const char *src_hostname = NULL;
                                    char src_actor_id[256] = "";
                                    if(ih == host)
                                        src_hostname = "local";
                                    else if(rrdhost_is_virtual(ih))
                                        src_hostname = "local";
                                    else {
                                        const ND_UUID *src_uuid = streaming_topology_path_previous_uuid(ihs, host->host_id);
                                        if(src_uuid) {
                                            char src_guid[UUID_STR_LEN];
                                            uuid_unparse_lower(src_uuid->uuid, src_guid);
                                            streaming_topology_actor_id_from_guid(src_guid, src_actor_id, sizeof(src_actor_id));
                                            RRDHOST *src = rrdhost_find_by_guid(src_guid);
                                            src_hostname = src ? rrdhost_hostname(src) : src_guid;
                                        }
                                        if(!src_hostname) src_hostname = "unknown";
                                    }

                                    buffer_json_add_array_item_object(wb);
                                    buffer_json_member_add_string(wb, "name", rrdhost_hostname(ih));
                                    const char *ih_actor_id = streaming_topology_visible_actor_id(visible_actors, ihs->actor_id);
                                    if(ih_actor_id)
                                        buffer_json_member_add_string(wb, "name_id", ih_actor_id);
                                    buffer_json_member_add_string(wb, "received_from", src_hostname);
                                    if(src_actor_id[0] && streaming_topology_visible_actor_exists(visible_actors, src_actor_id))
                                        buffer_json_member_add_string(wb, "received_from_id", src_actor_id);
                                    buffer_json_member_add_string(wb, "node_type", ihs->node_type);
                                    buffer_json_member_add_string(wb, "ingest_status", rrdhost_ingest_status_to_string(ihs->status.ingest.status));
                                    buffer_json_member_add_int64(wb, "hops", ihs->status.ingest.hops);
                                    buffer_json_member_add_uint64(wb, "collected_metrics", ihs->status.ingest.collected.metrics);
                                    buffer_json_member_add_uint64(wb, "collected_instances", ihs->status.ingest.collected.instances);
                                    buffer_json_member_add_uint64(wb, "collected_contexts", ihs->status.ingest.collected.contexts);
                                    buffer_json_member_add_double(wb, "repl_completion", ihs->status.ingest.replication.completion);
                                    buffer_json_member_add_time_t(wb, "ingest_age", ihs->status.ingest.since ? ihs->status.now - ihs->status.ingest.since : 0);
                                    buffer_json_member_add_string(wb, "ssl", ihs->status.ingest.ssl ? "SSL" : "PLAIN");
                                    buffer_json_member_add_uint64(wb, "alerts_critical",
                                        ihs->status.health.status == RRDHOST_HEALTH_STATUS_RUNNING ? ihs->status.health.alerts.critical : 0);
                                    buffer_json_member_add_uint64(wb, "alerts_warning",
                                        ihs->status.health.status == RRDHOST_HEALTH_STATUS_RUNNING ? ihs->status.health.alerts.warning : 0);
                                    buffer_json_object_close(wb);
                                }
                                dfe_done(ih);
                            }
                            buffer_json_array_close(wb);
                        }
                        else {
                            buffer_json_member_add_array(wb, "inbound");
                            {
                                struct streaming_topology_descendant_list *inbound_nodes =
                                    streaming_topology_descendants_get(parent_descendants, host);
                                if(inbound_nodes) {
                                    for(size_t i = 0; i < inbound_nodes->used; i++) {
                                        struct streaming_topology_descendant *descendant = &inbound_nodes->items[i];
                                        RRDHOST *ih = descendant->host;
                                        const char *src_hostname = NULL;
                                        char src_actor_id[256] = "";
                                        if(descendant->source_local)
                                            src_hostname = "local";
                                        else if(!UUIDiszero(descendant->source_uuid)) {
                                            char src_guid[UUID_STR_LEN];
                                            uuid_unparse_lower(descendant->source_uuid.uuid, src_guid);
                                            streaming_topology_actor_id_from_guid(src_guid, src_actor_id, sizeof(src_actor_id));
                                            RRDHOST *src = rrdhost_find_by_guid(src_guid);
                                            src_hostname = src ? rrdhost_hostname(src) : src_guid;
                                        }
                                        else
                                            src_hostname = "unknown";

                                        struct streaming_topology_host_state *ihs = streaming_topology_host_state_get(host_states, ih);
                                        if(!ihs)
                                            continue;

                                        buffer_json_add_array_item_object(wb);
                                        buffer_json_member_add_string(wb, "name", rrdhost_hostname(ih));
                                        const char *ih_actor_id = streaming_topology_visible_actor_id(visible_actors, ihs->actor_id);
                                        if(ih_actor_id)
                                            buffer_json_member_add_string(wb, "name_id", ih_actor_id);
                                        buffer_json_member_add_string(wb, "received_from", src_hostname);
                                        if(src_actor_id[0] && streaming_topology_visible_actor_exists(visible_actors, src_actor_id))
                                            buffer_json_member_add_string(wb, "received_from_id", src_actor_id);
                                        buffer_json_member_add_string(wb, "node_type", ihs->node_type);
                                        buffer_json_member_add_string(wb, "ingest_status", rrdhost_ingest_status_to_string(ihs->status.ingest.status));
                                        buffer_json_member_add_int64(wb, "hops", ihs->status.ingest.hops);
                                        buffer_json_member_add_uint64(wb, "collected_metrics", ihs->status.ingest.collected.metrics);
                                        buffer_json_member_add_uint64(wb, "collected_instances", ihs->status.ingest.collected.instances);
                                        buffer_json_member_add_uint64(wb, "collected_contexts", ihs->status.ingest.collected.contexts);
                                        buffer_json_member_add_double(wb, "repl_completion", ihs->status.ingest.replication.completion);
                                        buffer_json_member_add_time_t(wb, "ingest_age", ihs->status.ingest.since ? ihs->status.now - ihs->status.ingest.since : 0);
                                        buffer_json_member_add_string(wb, "ssl", ihs->status.ingest.ssl ? "SSL" : "PLAIN");
                                        buffer_json_member_add_uint64(wb, "alerts_critical",
                                            ihs->status.health.status == RRDHOST_HEALTH_STATUS_RUNNING ? ihs->status.health.alerts.critical : 0);
                                        buffer_json_member_add_uint64(wb, "alerts_warning",
                                            ihs->status.health.status == RRDHOST_HEALTH_STATUS_RUNNING ? ihs->status.health.alerts.warning : 0);
                                        buffer_json_object_close(wb);
                                    }
                                }
                            }
                            buffer_json_array_close(wb);
                        }

                        if(is_observer) {
                            buffer_json_member_add_array(wb, "retention");
                            {
                                RRDHOST *rh;
                                dfe_start_read(rrdhost_root_index, rh) {
                                    struct streaming_topology_host_state *rhs = streaming_topology_host_state_get(host_states, rh);
                                    if(!rhs || (!rhs->status.db.first_time_s && !rhs->status.db.last_time_s))
                                        continue;

                                    buffer_json_add_array_item_object(wb);
                                    buffer_json_member_add_string(wb, "name", rrdhost_hostname(rh));
                                    const char *rh_actor_id = streaming_topology_visible_actor_id(visible_actors, rhs->actor_id);
                                    if(rh_actor_id)
                                        buffer_json_member_add_string(wb, "name_id", rh_actor_id);
                                    buffer_json_member_add_string(wb, "db_status", rrdhost_db_status_to_string(rhs->status.db.status));
                                    buffer_json_member_add_uint64(wb, "db_from", rhs->status.db.first_time_s * MSEC_PER_SEC);
                                    buffer_json_member_add_uint64(wb, "db_to", rhs->status.db.last_time_s * MSEC_PER_SEC);
                                    if(rhs->status.db.first_time_s && rhs->status.db.last_time_s && rhs->status.db.last_time_s > rhs->status.db.first_time_s)
                                        buffer_json_member_add_uint64(wb, "db_duration", rhs->status.db.last_time_s - rhs->status.db.first_time_s);
                                    else
                                        buffer_json_member_add_uint64(wb, "db_duration", 0);
                                    buffer_json_member_add_uint64(wb, "db_metrics", rhs->status.db.metrics);
                                    buffer_json_member_add_uint64(wb, "db_instances", rhs->status.db.instances);
                                    buffer_json_member_add_uint64(wb, "db_contexts", rhs->status.db.contexts);
                                    buffer_json_object_close(wb);
                                }
                                dfe_done(rh);
                            }
                            buffer_json_array_close(wb);
                        }
                    }

                    if(is_observer && s->stream.status != RRDHOST_STREAM_STATUS_DISABLED) {
                        bool stream_connected = (s->stream.status == RRDHOST_STREAM_STATUS_ONLINE ||
                                                 s->stream.status == RRDHOST_STREAM_STATUS_REPLICATING);

                        const char *dst_hostname = NULL;
                        char dst_actor_id[256] = "";
                        if(stream_connected) {
                            const ND_UUID *dst_uuid = streaming_topology_path_next_uuid(hs, host->host_id);
                            if(dst_uuid) {
                                char guid[UUID_STR_LEN];
                                uuid_unparse_lower(dst_uuid->uuid, guid);
                                streaming_topology_actor_id_from_guid(guid, dst_actor_id, sizeof(dst_actor_id));
                                RRDHOST *dst_host = rrdhost_find_by_guid(guid);
                                if(dst_host)
                                    dst_hostname = rrdhost_hostname(dst_host);
                            }
                            if(!dst_hostname) dst_hostname = s->stream.peers.peer.ip;
                        }

                        buffer_json_member_add_array(wb, "outbound");
                        {
                            RRDHOST *oh;
                            dfe_start_read(rrdhost_root_index, oh) {
                                struct streaming_topology_host_state *ohs = streaming_topology_host_state_get(host_states, oh);
                                if(!ohs)
                                    continue;

                                RRDHOST_STREAMING_STATUS oh_ss = (oh == host) ? s->stream.status : ohs->status.stream.status;
                                bool oh_streaming = (oh_ss == RRDHOST_STREAM_STATUS_ONLINE || oh_ss == RRDHOST_STREAM_STATUS_REPLICATING);

                                buffer_json_add_array_item_object(wb);
                                buffer_json_member_add_string(wb, "name", rrdhost_hostname(oh));
                                const char *oh_actor_id = streaming_topology_visible_actor_id(visible_actors, ohs->actor_id);
                                if(oh_actor_id)
                                    buffer_json_member_add_string(wb, "name_id", oh_actor_id);
                                if(dst_hostname && oh_streaming) {
                                    buffer_json_member_add_string(wb, "streamed_to", dst_hostname);
                                    if(dst_actor_id[0] && streaming_topology_visible_actor_exists(visible_actors, dst_actor_id))
                                        buffer_json_member_add_string(wb, "streamed_to_id", dst_actor_id);
                                }
                                buffer_json_member_add_string(wb, "node_type", ohs->node_type);
                                buffer_json_member_add_string(wb, "stream_status", rrdhost_streaming_status_to_string(oh_ss));

                                if(oh == host && oh_streaming) {
                                    buffer_json_member_add_uint64(wb, "hops", s->stream.hops);
                                    buffer_json_member_add_string(wb, "ssl", s->stream.ssl ? "SSL" : "PLAIN");
                                    buffer_json_member_add_string(wb, "compression", s->stream.compression ? "COMPRESSED" : "UNCOMPRESSED");
                                }

                                buffer_json_object_close(wb);
                            }
                            dfe_done(oh);
                        }
                        buffer_json_array_close(wb);
                    }
                    else if(!is_observer && child_count > 0) {
                        const char *dst_hostname = NULL;

                        buffer_json_member_add_array(wb, "outbound");
                        {
                            char dst_actor_id[256] = "";
                            const ND_UUID *dst_uuid = streaming_topology_path_next_uuid(hs, host->host_id);
                            if(dst_uuid) {
                                char dst_guid[UUID_STR_LEN];
                                uuid_unparse_lower(dst_uuid->uuid, dst_guid);
                                streaming_topology_actor_id_from_guid(dst_guid, dst_actor_id, sizeof(dst_actor_id));
                                RRDHOST *dst = rrdhost_find_by_guid(dst_guid);
                                dst_hostname = dst ? rrdhost_hostname(dst) : dst_guid;
                            }

                            struct streaming_topology_descendant_list *outbound_nodes =
                                streaming_topology_descendants_get(parent_descendants, host);
                            if(outbound_nodes) {
                                for(size_t i = 0; i < outbound_nodes->used; i++) {
                                    RRDHOST *oh = outbound_nodes->items[i].host;
                                    struct streaming_topology_host_state *ohs = streaming_topology_host_state_get(host_states, oh);
                                    if(!ohs)
                                        continue;

                                    RRDHOST_STREAMING_STATUS oh_ss = (oh == host) ? s->stream.status : ohs->status.stream.status;
                                    bool oh_streaming = (oh_ss == RRDHOST_STREAM_STATUS_ONLINE || oh_ss == RRDHOST_STREAM_STATUS_REPLICATING);

                                    buffer_json_add_array_item_object(wb);
                                    buffer_json_member_add_string(wb, "name", rrdhost_hostname(oh));
                                    const char *oh_actor_id = streaming_topology_visible_actor_id(visible_actors, ohs->actor_id);
                                    if(oh_actor_id)
                                        buffer_json_member_add_string(wb, "name_id", oh_actor_id);
                                    if(dst_hostname && oh_streaming) {
                                        buffer_json_member_add_string(wb, "streamed_to", dst_hostname);
                                        if(dst_actor_id[0] && streaming_topology_visible_actor_exists(visible_actors, dst_actor_id))
                                            buffer_json_member_add_string(wb, "streamed_to_id", dst_actor_id);
                                    }
                                    buffer_json_member_add_string(wb, "node_type", ohs->node_type);
                                    buffer_json_member_add_string(wb, "stream_status", rrdhost_streaming_status_to_string(oh_ss));
                                    if(oh == host && oh_streaming) {
                                        buffer_json_member_add_uint64(wb, "hops", s->stream.hops);
                                        buffer_json_member_add_string(wb, "ssl", s->stream.ssl ? "SSL" : "PLAIN");
                                        buffer_json_member_add_string(wb, "compression", s->stream.compression ? "COMPRESSED" : "UNCOMPRESSED");
                                    }
                                    buffer_json_object_close(wb);
                                }
                            }
                        }
                        buffer_json_array_close(wb);
                    }

                    streaming_topology_emit_filtered_streaming_path(wb, runtime, host, hs);

                    if(child_count == 0) {
                        buffer_json_member_add_array(wb, "retention");
                        {
                            char observer_actor_id[256];
                            streaming_topology_actor_id_for_host(localhost, observer_actor_id, sizeof(observer_actor_id));

                            buffer_json_add_array_item_object(wb);
                            buffer_json_member_add_string(wb, "name", rrdhost_hostname(localhost));
                            const char *visible_observer_actor_id = streaming_topology_visible_actor_id(visible_actors, observer_actor_id);
                            if(visible_observer_actor_id)
                                buffer_json_member_add_string(wb, "name_id", visible_observer_actor_id);
                            buffer_json_member_add_string(wb, "db_status", rrdhost_db_status_to_string(s->db.status));
                            buffer_json_member_add_uint64(wb, "db_from", s->db.first_time_s * MSEC_PER_SEC);
                            buffer_json_member_add_uint64(wb, "db_to", s->db.last_time_s * MSEC_PER_SEC);
                            if(s->db.first_time_s && s->db.last_time_s && s->db.last_time_s > s->db.first_time_s)
                                buffer_json_member_add_uint64(wb, "db_duration", s->db.last_time_s - s->db.first_time_s);
                            else
                                buffer_json_member_add_uint64(wb, "db_duration", 0);
                            buffer_json_member_add_uint64(wb, "db_metrics", s->db.metrics);
                            buffer_json_member_add_uint64(wb, "db_instances", s->db.instances);
                            buffer_json_member_add_uint64(wb, "db_contexts", s->db.contexts);
                            buffer_json_object_close(wb);
                        }
                        buffer_json_array_close(wb);
                    }
                }
                buffer_json_object_close(wb);
            }
            buffer_json_object_close(wb);
        }
        dfe_done(host);
    }
    buffer_json_array_close(wb);
}

static void streaming_topology_emit_links(BUFFER *wb, struct streaming_topology_runtime *runtime, const struct streaming_topology_filters *filters, time_t now, usec_t now_ut, size_t *links_total) {
    if(!wb || !runtime)
        return;

    char localhost_actor_id[256];
    streaming_topology_actor_id_for_host(localhost, localhost_actor_id, sizeof(localhost_actor_id));

    buffer_json_member_add_array(wb, "links");
    {
        RRDHOST *host;
        dfe_start_read(rrdhost_root_index, host) {
            if(host == localhost)
                continue;

            struct streaming_topology_host_state *hs = streaming_topology_host_state_get(runtime->host_states, host);
            if(!hs)
                continue;

            if(!streaming_topology_host_matches_filters(filters, hs))
                continue;

            bool is_vnode = rrdhost_is_virtual(host);

            const char *link_type = NULL;
            char target_actor_id[256] = "";

            if(is_vnode && hs->path_len >= 2) {
                streaming_topology_actor_id_for_uuid(hs->path_ids[0], target_actor_id, sizeof(target_actor_id));
                link_type = "virtual";
            }
            else if(!is_vnode && hs->path_len >= 2) {
                streaming_topology_actor_id_for_uuid(hs->path_ids[1], target_actor_id, sizeof(target_actor_id));
                link_type = "streaming";
            }
            else {
                snprintfz(target_actor_id, sizeof(target_actor_id), "%s", localhost_actor_id);
                link_type = "stale";
            }

            if(!streaming_topology_visible_actor_exists(runtime->visible_actors, target_actor_id))
                continue;

            const char *hostname = rrdhost_hostname(host);

            if(links_total)
                (*links_total)++;

            buffer_json_add_array_item_object(wb);
            {
                buffer_json_member_add_string(wb, "layer", "infra");
                buffer_json_member_add_string(wb, "protocol", "streaming");
                buffer_json_member_add_string(wb, "link_type", link_type);
                buffer_json_member_add_string(wb, "src_actor_id", hs->actor_id);
                buffer_json_member_add_string(wb, "dst_actor_id", target_actor_id);
                buffer_json_member_add_string(wb, "state", rrdhost_ingest_status_to_string(hs->status.ingest.status));
                buffer_json_member_add_datetime_rfc3339(wb, "discovered_at",
                    ((uint64_t)(hs->status.ingest.since ? hs->status.ingest.since : now)) * USEC_PER_SEC, true);
                buffer_json_member_add_datetime_rfc3339(wb, "last_seen", now_ut, true);

                buffer_json_member_add_object(wb, "dst");
                {
                    buffer_json_member_add_object(wb, "attributes");
                    {
                        buffer_json_member_add_string(wb, "port_name", hostname);
                    }
                    buffer_json_object_close(wb);
                }
                buffer_json_object_close(wb);

                buffer_json_member_add_object(wb, "metrics");
                {
                    buffer_json_member_add_uint64(wb, "hops", hs->status.ingest.hops);
                    if(strcmp(link_type, "virtual") != 0) {
                        buffer_json_member_add_uint64(wb, "connections", hs->status.host->stream.rcv.status.connections);
                        buffer_json_member_add_uint64(wb, "replication_instances", hs->status.ingest.replication.instances);
                        buffer_json_member_add_double(wb, "replication_completion", hs->status.ingest.replication.completion);
                        buffer_json_member_add_uint64(wb, "collected_metrics", hs->status.ingest.collected.metrics);
                        buffer_json_member_add_uint64(wb, "collected_instances", hs->status.ingest.collected.instances);
                        buffer_json_member_add_uint64(wb, "collected_contexts", hs->status.ingest.collected.contexts);
                    }
                }
                buffer_json_object_close(wb);
            }
            buffer_json_object_close(wb);
        }
        dfe_done(host);
    }
    buffer_json_array_close(wb);
}

static struct streaming_topology_descendant_list *streaming_topology_descendants_get_or_create(DICTIONARY *parent_descendants, ND_UUID host_id) {
    char host_guid[UUID_STR_LEN];
    if(!streaming_topology_uuid_guid(host_id, host_guid, sizeof(host_guid)))
        return NULL;

    struct streaming_topology_descendant_list *list = dictionary_get(parent_descendants, host_guid);
    if(list)
        return list;

    struct streaming_topology_descendant_list empty = {0};
    return dictionary_set(parent_descendants, host_guid, &empty, sizeof(empty));
}

static void streaming_topology_descendants_append(
    DICTIONARY *parent_descendants,
    ND_UUID parent_id,
    RRDHOST *host,
    enum streaming_topology_received_type type,
    bool source_local,
    ND_UUID source_uuid
) {
    struct streaming_topology_descendant_list *list = streaming_topology_descendants_get_or_create(parent_descendants, parent_id);
    if(!list)
        return;

    if(list->used == list->size) {
        size_t new_size = list->size ? list->size * 2 : 4;
        list->items = reallocz(list->items, new_size * sizeof(*list->items));
        list->size = new_size;
    }

    list->items[list->used++] = (struct streaming_topology_descendant) {
        .host = host,
        .source_uuid = source_uuid,
        .source_local = source_local,
        .type = (uint8_t)type,
    };
}

static const char *streaming_topology_received_type_to_string(enum streaming_topology_received_type type) {
    switch(type) {
        case STREAMING_TOPOLOGY_RECEIVED_VIRTUAL:
            return "virtual";

        case STREAMING_TOPOLOGY_RECEIVED_STALE:
            return "stale";

        case STREAMING_TOPOLOGY_RECEIVED_STREAMING:
        default:
            return "streaming";
    }
}

static void streaming_topology_actor_id_for_host(RRDHOST *host, char *dst, size_t dst_size) {
    if(!dst || !dst_size)
        return;

    char host_guid[UUID_STR_LEN];
    if(streaming_topology_host_guid(host, host_guid, sizeof(host_guid)))
        streaming_topology_actor_id_from_guid(host_guid, dst, dst_size);
    else if(host)
        snprintf(dst, dst_size, "hostname:%s", rrdhost_hostname(host));
    else
        snprintf(dst, dst_size, "host:unknown");
}

// get streaming_path host_ids, appending localhost only when the path already
// has upstream entries but does not yet include us; callers still use n == 0
// to detect hosts without an active path
static uint16_t streaming_topology_get_path_ids(RRDHOST *host, uint16_t from, ND_UUID *host_ids, uint16_t max) {
    uint16_t n = rrdhost_stream_path_get_host_ids(host, from, host_ids, max);
    uint16_t filtered_n = 0;

    // check if localhost is already in the path
    bool found_localhost = false;
    for(uint16_t i = 0; i < n; i++) {
        if(UUIDiszero(host_ids[i]))
            continue;

        host_ids[filtered_n++] = host_ids[i];
        if(UUIDeq(host_ids[i], localhost->host_id)) {
            found_localhost = true;
        }
    }
    n = filtered_n;

    // append localhost if not found (same as rrdhost_stream_path_to_json)
    if(!found_localhost && n < max && n > 0)
        host_ids[n++] = localhost->host_id;

    return n;
}

static void streaming_topology_parse_filters(const char *function, struct streaming_topology_filters *filters) {
    if(!filters)
        return;

    *filters = (struct streaming_topology_filters){ 0 };
    if(!function || !*function)
        return;

    filters->function_copy = strdupz(function);
    char *words[1024];
    size_t num_words = quoted_strings_splitter_whitespace(filters->function_copy, words, 1024);
    for(size_t i = 1; i < num_words; i++) {
        char *param = get_word(words, num_words, i);
        if(strcmp(param, "info") == 0)
            filters->info_only = true;
        else if(strncmp(param, "node_type:", 10) == 0)
            filters->node_type = param + 10;
        else if(strncmp(param, "ingest_status:", 14) == 0)
            filters->ingest_status = param + 14;
        else if(strncmp(param, "stream_status:", 14) == 0)
            filters->stream_status = param + 14;
    }
}

static int streaming_topology_return_error(BUFFER *wb, char *function_copy, int status, const char *error) {
    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);

    buffer_json_member_add_uint64(wb, "status", status);
    buffer_json_member_add_string(wb, "type", "topology");
    buffer_json_member_add_time_t(wb, "update_every", STREAMING_FUNCTION_UPDATE_EVERY);
    buffer_json_member_add_boolean(wb, "has_history", false);
    buffer_json_member_add_string(wb, "help", RRDFUNCTIONS_STREAMING_TOPOLOGY_HELP);
    buffer_json_member_add_string(wb, "error", error);
    buffer_json_finalize(wb);

    freez(function_copy);
    return status;
}

// check if a value appears in a comma-separated list (NULL list matches everything)
static bool value_in_csv(const char *csv, const char *value) {
    if(!csv || !*csv) return true;
    if(!value || !*value) return false;
    size_t vlen = strlen(value);
    const char *p = csv;
    while(*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while(len && isspace((unsigned char)*p)) {
            p++;
            len--;
        }
        while(len && isspace((unsigned char)p[len - 1]))
            len--;
        if(len == vlen && strncmp(p, value, len) == 0)
            return true;
        if(!comma) break;
        p = comma + 1;
    }
    return false;
}

// helpers: emit one summary field and one table column
static void streaming_topology_emit_sf(BUFFER *wb, const char *key, const char *label, const char *source) {
    buffer_json_add_array_item_object(wb);
    buffer_json_member_add_string(wb, "key", key);
    buffer_json_member_add_string(wb, "label", label);
    buffer_json_member_add_array(wb, "sources");
    buffer_json_add_array_item_string(wb, source);
    buffer_json_array_close(wb);
    buffer_json_object_close(wb);
}

static void streaming_topology_emit_col(BUFFER *wb, const char *key, const char *label, const char *type) {
    buffer_json_add_array_item_object(wb);
    buffer_json_member_add_string(wb, "key", key);
    buffer_json_member_add_string(wb, "label", label);
    if(type)
        buffer_json_member_add_string(wb, "type", type);
    buffer_json_object_close(wb);
}

static void streaming_topology_info_tab(BUFFER *wb) {
    buffer_json_member_add_array(wb, "modal_tabs");
    {
        buffer_json_add_array_item_object(wb);
        buffer_json_member_add_string(wb, "id", "info");
        buffer_json_member_add_string(wb, "label", "Info");
        buffer_json_object_close(wb);
    }
    buffer_json_array_close(wb);
}

// streaming path table: shared by all non-parent types
static void streaming_topology_emit_streaming_path_table(BUFFER *wb, uint64_t order) {
    buffer_json_member_add_object(wb, "streaming_path");
    {
        buffer_json_member_add_string(wb, "label", "Streaming Path");
        buffer_json_member_add_string(wb, "source", "data");
        buffer_json_member_add_uint64(wb, "order", order);
        buffer_json_member_add_array(wb, "columns");
        {
            streaming_topology_emit_col(wb, "hostname", "Agent", NULL);
            streaming_topology_emit_col(wb, "hops", "Hops", "number");
            streaming_topology_emit_col(wb, "since", "Since", "number");
            streaming_topology_emit_col(wb, "flags", "Flags", NULL);
        }
        buffer_json_array_close(wb);
    }
    buffer_json_object_close(wb);
}

// retention table: shared by all types
static void streaming_topology_emit_retention_table(BUFFER *wb, uint64_t order, const char *name_label) {
    buffer_json_member_add_object(wb, "retention");
    {
        buffer_json_member_add_string(wb, "label", "Retention");
        buffer_json_member_add_string(wb, "source", "data");
        buffer_json_member_add_uint64(wb, "order", order);
        buffer_json_member_add_array(wb, "columns");
        {
            streaming_topology_emit_col(wb, "name", name_label, "actor_link");
            streaming_topology_emit_col(wb, "db_status", "Status", "badge");
            streaming_topology_emit_col(wb, "db_from", "From", "timestamp");
            streaming_topology_emit_col(wb, "db_to", "To", "timestamp");
            streaming_topology_emit_col(wb, "db_duration", "Duration", "duration");
            streaming_topology_emit_col(wb, "db_metrics", "Metrics", "number");
            streaming_topology_emit_col(wb, "db_instances", "Instances", "number");
            streaming_topology_emit_col(wb, "db_contexts", "Contexts", "number");
        }
        buffer_json_array_close(wb);
    }
    buffer_json_object_close(wb);
}

// parent actor presentation: intrinsic summary + inbound/retention tables
static void streaming_topology_parent_presentation(BUFFER *wb) {
    buffer_json_member_add_array(wb, "summary_fields");
    {
        streaming_topology_emit_sf(wb, "display_name", "Name", "attributes.display_name");
        streaming_topology_emit_sf(wb, "node_type", "Type", "attributes.node_type");
        streaming_topology_emit_sf(wb, "agent_version", "Version", "attributes.agent_version");
        streaming_topology_emit_sf(wb, "os_name", "OS", "attributes.os_name");
        streaming_topology_emit_sf(wb, "architecture", "Arch", "attributes.architecture");
        streaming_topology_emit_sf(wb, "cpu_cores", "CPUs", "attributes.cpu_cores");
        streaming_topology_emit_sf(wb, "child_count", "Children", "attributes.child_count");
        streaming_topology_emit_sf(wb, "health_critical", "Alerts Critical", "attributes.health_critical");
        streaming_topology_emit_sf(wb, "health_warning", "Alerts Warning", "attributes.health_warning");
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "tables");
    {
        // inbound: all nodes this parent has data for (rows = all known hosts)
        buffer_json_member_add_object(wb, "inbound");
        {
            buffer_json_member_add_string(wb, "label", "Inbound");
            buffer_json_member_add_string(wb, "source", "data");
            buffer_json_member_add_boolean(wb, "bullet_source", true);
            buffer_json_member_add_uint64(wb, "order", 0);
            buffer_json_member_add_array(wb, "columns");
            {
                streaming_topology_emit_col(wb, "name", "Node", "actor_link");
                streaming_topology_emit_col(wb, "received_from", "Source", "actor_link");
                streaming_topology_emit_col(wb, "node_type", "Type", "badge");
                streaming_topology_emit_col(wb, "ingest_status", "Ingest", "badge");
                streaming_topology_emit_col(wb, "hops", "Hops", "number");
                streaming_topology_emit_col(wb, "collected_metrics", "Metrics", "number");
                streaming_topology_emit_col(wb, "collected_instances", "Instances", "number");
                streaming_topology_emit_col(wb, "collected_contexts", "Contexts", "number");
                streaming_topology_emit_col(wb, "repl_completion", "Replication", "number");
                streaming_topology_emit_col(wb, "ingest_age", "Age", "duration");
                streaming_topology_emit_col(wb, "ssl", "SSL", "badge");
                streaming_topology_emit_col(wb, "alerts_critical", "Critical", "number");
                streaming_topology_emit_col(wb, "alerts_warning", "Warning", "number");
            }
            buffer_json_array_close(wb);
        }
        buffer_json_object_close(wb);

        // outbound: all nodes this parent streams to its parent (rows = all known hosts)
        buffer_json_member_add_object(wb, "outbound");
        {
            buffer_json_member_add_string(wb, "label", "Outbound");
            buffer_json_member_add_string(wb, "source", "data");
            buffer_json_member_add_uint64(wb, "order", 1);
            buffer_json_member_add_array(wb, "columns");
            {
                streaming_topology_emit_col(wb, "name", "Node", "actor_link");
                streaming_topology_emit_col(wb, "streamed_to", "Destination", "actor_link");
                streaming_topology_emit_col(wb, "node_type", "Type", "badge");
                streaming_topology_emit_col(wb, "stream_status", "Status", "badge");
                streaming_topology_emit_col(wb, "hops", "Hops", "number");
                streaming_topology_emit_col(wb, "ssl", "SSL", "badge");
                streaming_topology_emit_col(wb, "compression", "Compression", "badge");
            }
            buffer_json_array_close(wb);
        }
        buffer_json_object_close(wb);

        streaming_topology_emit_retention_table(wb, 2, "Node");
        streaming_topology_emit_streaming_path_table(wb, 3);
    }
    buffer_json_object_close(wb);

    streaming_topology_info_tab(wb);
}

// child actor presentation: intrinsic summary + streaming path/retention tables
static void streaming_topology_child_presentation(BUFFER *wb) {
    buffer_json_member_add_array(wb, "summary_fields");
    {
        streaming_topology_emit_sf(wb, "display_name", "Name", "attributes.display_name");
        streaming_topology_emit_sf(wb, "node_type", "Type", "attributes.node_type");
        streaming_topology_emit_sf(wb, "agent_version", "Version", "attributes.agent_version");
        streaming_topology_emit_sf(wb, "os_name", "OS", "attributes.os_name");
        streaming_topology_emit_sf(wb, "architecture", "Arch", "attributes.architecture");
        streaming_topology_emit_sf(wb, "cpu_cores", "CPUs", "attributes.cpu_cores");
        streaming_topology_emit_sf(wb, "health_critical", "Alerts Critical", "attributes.health_critical");
        streaming_topology_emit_sf(wb, "health_warning", "Alerts Warning", "attributes.health_warning");
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "tables");
    {
        streaming_topology_emit_streaming_path_table(wb, 0);
        streaming_topology_emit_retention_table(wb, 1, "Parent");
    }
    buffer_json_object_close(wb);

    streaming_topology_info_tab(wb);
}

// vnode actor presentation: minimal summary + streaming path/retention
static void streaming_topology_vnode_presentation(BUFFER *wb) {
    buffer_json_member_add_array(wb, "summary_fields");
    {
        streaming_topology_emit_sf(wb, "display_name", "Name", "attributes.display_name");
        streaming_topology_emit_sf(wb, "node_type", "Type", "attributes.node_type");
        streaming_topology_emit_sf(wb, "ephemerality", "Ephemerality", "attributes.ephemerality");
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "tables");
    {
        streaming_topology_emit_streaming_path_table(wb, 0);
        streaming_topology_emit_retention_table(wb, 1, "Parent");
    }
    buffer_json_object_close(wb);

    streaming_topology_info_tab(wb);
}

// stale actor presentation: identity summary + streaming path/retention
static void streaming_topology_stale_presentation(BUFFER *wb) {
    buffer_json_member_add_array(wb, "summary_fields");
    {
        streaming_topology_emit_sf(wb, "display_name", "Name", "attributes.display_name");
        streaming_topology_emit_sf(wb, "node_type", "Type", "attributes.node_type");
        streaming_topology_emit_sf(wb, "agent_version", "Version", "attributes.agent_version");
        streaming_topology_emit_sf(wb, "os_name", "OS", "attributes.os_name");
        streaming_topology_emit_sf(wb, "architecture", "Arch", "attributes.architecture");
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "tables");
    {
        streaming_topology_emit_streaming_path_table(wb, 0);
        streaming_topology_emit_retention_table(wb, 1, "Parent");
    }
    buffer_json_object_close(wb);

    streaming_topology_info_tab(wb);
}

int function_streaming_topology(BUFFER *wb, const char *function, BUFFER *payload __maybe_unused, const char *source __maybe_unused) {
    time_t now = now_realtime_sec();
    usec_t now_ut = now_realtime_usec();

    struct streaming_topology_filters filters = { 0 };
    streaming_topology_parse_filters(function, &filters);
    bool info_only = filters.info_only;
    const char *filter_node_type = filters.node_type;
    const char *filter_ingest_status = filters.ingest_status;
    const char *filter_stream_status = filters.stream_status;
    char *function_copy = filters.function_copy;

    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);

    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_string(wb, "type", "topology");
    buffer_json_member_add_time_t(wb, "update_every", STREAMING_FUNCTION_UPDATE_EVERY);
    buffer_json_member_add_boolean(wb, "has_history", false);
    buffer_json_member_add_string(wb, "help", RRDFUNCTIONS_STREAMING_TOPOLOGY_HELP);
    buffer_json_member_add_array(wb, "accepted_params");
    {
        buffer_json_add_array_item_string(wb, "node_type");
        buffer_json_add_array_item_string(wb, "ingest_status");
        buffer_json_add_array_item_string(wb, "stream_status");
        buffer_json_add_array_item_string(wb, "info");
    }
    buffer_json_array_close(wb);
    buffer_json_member_add_array(wb, "required_params");
    buffer_json_array_close(wb);

    // --- presentation metadata ---
    buffer_json_member_add_object(wb, "presentation");
    {
        buffer_json_member_add_object(wb, "actor_types");
        {
            buffer_json_member_add_object(wb, "parent");
            {
                buffer_json_member_add_string(wb, "label", "Netdata Parent");
                buffer_json_member_add_string(wb, "color_slot", "primary");
                buffer_json_member_add_double(wb, "opacity", 1.0);
                buffer_json_member_add_boolean(wb, "border", true);
                buffer_json_member_add_boolean(wb, "size_by_links", true);
                buffer_json_member_add_boolean(wb, "show_port_bullets", true);
                streaming_topology_parent_presentation(wb);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "child");
            {
                buffer_json_member_add_string(wb, "label", "Netdata Child");
                buffer_json_member_add_string(wb, "color_slot", "primary");
                buffer_json_member_add_double(wb, "opacity", 1.0);
                buffer_json_member_add_boolean(wb, "border", false);
                buffer_json_member_add_boolean(wb, "size_by_links", false);
                buffer_json_member_add_boolean(wb, "show_port_bullets", false);
                streaming_topology_child_presentation(wb);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "vnode");
            {
                buffer_json_member_add_string(wb, "label", "Virtual Node");
                buffer_json_member_add_string(wb, "color_slot", "warning");
                buffer_json_member_add_double(wb, "opacity", 1.0);
                buffer_json_member_add_boolean(wb, "border", false);
                buffer_json_member_add_boolean(wb, "size_by_links", false);
                buffer_json_member_add_boolean(wb, "show_port_bullets", false);
                streaming_topology_vnode_presentation(wb);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "stale");
            {
                buffer_json_member_add_string(wb, "label", "Stale Node");
                buffer_json_member_add_string(wb, "color_slot", "dim");
                buffer_json_member_add_double(wb, "opacity", 0.5);
                buffer_json_member_add_boolean(wb, "border", false);
                buffer_json_member_add_boolean(wb, "size_by_links", false);
                buffer_json_member_add_boolean(wb, "show_port_bullets", false);
                streaming_topology_stale_presentation(wb);
            }
            buffer_json_object_close(wb);
        }
        buffer_json_object_close(wb); // actor_types

        buffer_json_member_add_object(wb, "link_types");
        {
            buffer_json_member_add_object(wb, "streaming");
            {
                buffer_json_member_add_string(wb, "label", "Streaming");
                buffer_json_member_add_string(wb, "color_slot", "primary");
                buffer_json_member_add_double(wb, "width", 2);
                buffer_json_member_add_boolean(wb, "dash", false);
                buffer_json_member_add_double(wb, "opacity", 1.0);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "virtual");
            {
                buffer_json_member_add_string(wb, "label", "Virtual origin");
                buffer_json_member_add_string(wb, "color_slot", "warning");
                buffer_json_member_add_double(wb, "width", 1);
                buffer_json_member_add_boolean(wb, "dash", true);
                buffer_json_member_add_double(wb, "opacity", 0.7);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "stale");
            {
                buffer_json_member_add_string(wb, "label", "Stale data");
                buffer_json_member_add_string(wb, "color_slot", "dim");
                buffer_json_member_add_double(wb, "width", 1);
                buffer_json_member_add_boolean(wb, "dash", true);
                buffer_json_member_add_double(wb, "opacity", 0.4);
            }
            buffer_json_object_close(wb);
        }
        buffer_json_object_close(wb); // link_types

        buffer_json_member_add_array(wb, "port_fields");
        {
            buffer_json_add_array_item_object(wb);
            buffer_json_member_add_string(wb, "key", "type");
            buffer_json_member_add_string(wb, "label", "Type");
            buffer_json_object_close(wb);
        }
        buffer_json_array_close(wb); // port_fields

        buffer_json_member_add_object(wb, "port_types");
        {
            buffer_json_member_add_object(wb, "streaming");
            {
                buffer_json_member_add_string(wb, "label", "Streaming child");
                buffer_json_member_add_string(wb, "color_slot", "primary");
                buffer_json_member_add_double(wb, "opacity", 1.0);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "virtual");
            {
                buffer_json_member_add_string(wb, "label", "Virtual node");
                buffer_json_member_add_string(wb, "color_slot", "warning");
                buffer_json_member_add_double(wb, "opacity", 1.0);
            }
            buffer_json_object_close(wb);

            buffer_json_member_add_object(wb, "stale");
            {
                buffer_json_member_add_string(wb, "label", "Stale node");
                buffer_json_member_add_string(wb, "color_slot", "dim");
                buffer_json_member_add_double(wb, "opacity", 0.5);
            }
            buffer_json_object_close(wb);
        }
        buffer_json_object_close(wb); // port_types

        buffer_json_member_add_object(wb, "legend");
        {
            buffer_json_member_add_array(wb, "actors");
            {
                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "parent");
                buffer_json_member_add_string(wb, "label", "Parent");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "child");
                buffer_json_member_add_string(wb, "label", "Child");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "vnode");
                buffer_json_member_add_string(wb, "label", "Virtual Node");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "stale");
                buffer_json_member_add_string(wb, "label", "Stale Node");
                buffer_json_object_close(wb);
            }
            buffer_json_array_close(wb); // actors

            buffer_json_member_add_array(wb, "links");
            {
                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "streaming");
                buffer_json_member_add_string(wb, "label", "Streaming");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "virtual");
                buffer_json_member_add_string(wb, "label", "Virtual origin");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "stale");
                buffer_json_member_add_string(wb, "label", "Stale data");
                buffer_json_object_close(wb);
            }
            buffer_json_array_close(wb); // links

            buffer_json_member_add_array(wb, "ports");
            {
                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "streaming");
                buffer_json_member_add_string(wb, "label", "Streaming child");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "virtual");
                buffer_json_member_add_string(wb, "label", "Virtual node");
                buffer_json_object_close(wb);

                buffer_json_add_array_item_object(wb);
                buffer_json_member_add_string(wb, "type", "stale");
                buffer_json_member_add_string(wb, "label", "Stale node");
                buffer_json_object_close(wb);
            }
            buffer_json_array_close(wb); // ports
        }
        buffer_json_object_close(wb); // legend

        buffer_json_member_add_string(wb, "actor_click_behavior", "highlight_path");
    }
    buffer_json_object_close(wb); // presentation

    if(!info_only) {
        // --- Phase 1: build parent_child_count dictionary from streaming_paths ---
        // A node is a parent if any other node's streaming_path contains it at position > 0
        struct streaming_topology_runtime runtime = { 0 };
        runtime.parent_child_count = dictionary_create_advanced(
            DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE,
            NULL, sizeof(uint32_t));
        runtime.parent_descendants = dictionary_create_advanced(
            DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE,
            NULL, sizeof(struct streaming_topology_descendant_list));
        runtime.visible_actors = dictionary_create_advanced(
            DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE,
            NULL, sizeof(bool));
        runtime.host_states = dictionary_create_advanced(
            DICT_OPTION_SINGLE_THREADED | DICT_OPTION_DONT_OVERWRITE_VALUE | DICT_OPTION_FIXED_SIZE,
            NULL, sizeof(struct streaming_topology_host_state));

        DICTIONARY *parent_child_count = runtime.parent_child_count;
        DICTIONARY *parent_descendants = runtime.parent_descendants;
        DICTIONARY *visible_actors = runtime.visible_actors;
        DICTIONARY *host_states = runtime.host_states;

        if(!parent_child_count || !parent_descendants || !visible_actors || !host_states) {
            streaming_topology_runtime_cleanup(&runtime);

            return streaming_topology_return_error(wb, function_copy,
                HTTP_RESP_INTERNAL_SERVER_ERROR,
                "failed to allocate streaming topology state");
        }
        else {

            {
                RRDHOST *host;
                dfe_start_read(rrdhost_root_index, host) {
                    // get all path entries at position > 0 (parents in the chain)
                    ND_UUID path_ids[128];
                    uint16_t n = streaming_topology_get_path_ids(host, 1, path_ids, 128);
                    for(uint16_t i = 0; i < n; i++) {
                        char guid[UUID_STR_LEN];
                        if(!streaming_topology_uuid_guid(path_ids[i], guid, sizeof(guid)))
                            continue;

                        uint32_t *count = dictionary_get(parent_child_count, guid);
                        if(count)
                            (*count)++;
                        else {
                            uint32_t one = 1;
                            dictionary_set(parent_child_count, guid, &one, sizeof(one));
                        }
                    }

                    ND_UUID full_path_ids[128];
                    uint16_t full_path_n = streaming_topology_get_path_ids(host, 0, full_path_ids, 128);
                    ND_UUID empty_uuid = {};

                    if(rrdhost_is_virtual(host)) {
                        if(full_path_n > 0) {
                            streaming_topology_descendants_append(parent_descendants,
                                full_path_ids[0], host, STREAMING_TOPOLOGY_RECEIVED_VIRTUAL, true, empty_uuid);

                            for(uint16_t i = 1; i < full_path_n; i++) {
                                streaming_topology_descendants_append(parent_descendants,
                                    full_path_ids[i], host, STREAMING_TOPOLOGY_RECEIVED_STREAMING, false, full_path_ids[i - 1]);
                            }
                        }
                    }
                    else if(full_path_n > 0) {
                        for(uint16_t i = 0; i < full_path_n; i++) {
                            bool source_local = (i == 0);
                            ND_UUID source_uuid = source_local ? empty_uuid : full_path_ids[i - 1];
                            streaming_topology_descendants_append(parent_descendants,
                                full_path_ids[i], host, STREAMING_TOPOLOGY_RECEIVED_STREAMING, source_local, source_uuid);
                        }
                    }
                    else if(host != localhost) {
                        streaming_topology_descendants_append(parent_descendants,
                            localhost->host_id, host, STREAMING_TOPOLOGY_RECEIVED_STALE, false, empty_uuid);
                    }
                }
                dfe_done(host);
            }

            {
                RRDHOST *host;
                bool host_state_cache_failed = false;
                dfe_start_read(rrdhost_root_index, host) {
                    if(!streaming_topology_host_state_set(host_states, parent_child_count, host, now)) {
                        host_state_cache_failed = true;
                        break;
                    }
                }
                dfe_done(host);

                if(host_state_cache_failed) {
                    streaming_topology_runtime_cleanup(&runtime);

                    return streaming_topology_return_error(wb, function_copy,
                        HTTP_RESP_INTERNAL_SERVER_ERROR,
                        "failed to cache streaming topology host state");
                }
            }

            buffer_json_member_add_object(wb, "data");
            {
                size_t actors_total = 0;
                size_t links_total = 0;

            buffer_json_member_add_string(wb, "schema_version", "2.0");
            buffer_json_member_add_string(wb, "source", "streaming");
            buffer_json_member_add_string(wb, "layer", "infra");
            char localhost_agent_id[256];
            streaming_topology_agent_id_for_host(localhost, localhost_agent_id, sizeof(localhost_agent_id));
            buffer_json_member_add_string(wb, "agent_id", localhost_agent_id);
            buffer_json_member_add_datetime_rfc3339(wb, "collected_at", now_ut, true);

            streaming_topology_emit_actors(wb, &runtime, &filters, &actors_total);

            streaming_topology_emit_links(wb, &runtime, &filters, now, now_ut, &links_total);

            buffer_json_member_add_object(wb, "stats");
            {
                buffer_json_member_add_uint64(wb, "actors_total", actors_total);
                buffer_json_member_add_uint64(wb, "links_total", links_total);
            }
            buffer_json_object_close(wb); // stats
        }
            buffer_json_object_close(wb); // data

            streaming_topology_runtime_cleanup(&runtime);
        }
    }

    buffer_json_member_add_time_t(wb, "expires", now_realtime_sec() + STREAMING_FUNCTION_UPDATE_EVERY);
    buffer_json_finalize(wb);
    freez(function_copy);
    return HTTP_RESP_OK;
}


int function_streaming(BUFFER *wb, const char *function __maybe_unused, BUFFER *payload __maybe_unused, const char *source __maybe_unused) {

    time_t now = now_realtime_sec();

    buffer_flush(wb);
    wb->content_type = CT_APPLICATION_JSON;
    buffer_json_initialize(wb, "\"", "\"", 0, true, BUFFER_JSON_OPTIONS_DEFAULT);

    buffer_json_member_add_string(wb, "hostname", rrdhost_hostname(localhost));
    buffer_json_member_add_uint64(wb, "status", HTTP_RESP_OK);
    buffer_json_member_add_string(wb, "type", "table");
    buffer_json_member_add_time_t(wb, "update_every", STREAMING_FUNCTION_UPDATE_EVERY);
    buffer_json_member_add_boolean(wb, "has_history", false);
    buffer_json_member_add_string(wb, "help", RRDFUNCTIONS_STREAMING_HELP);
    buffer_json_member_add_array(wb, "data");

    size_t max_sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_MAX] = { 0 };
    size_t max_db_metrics = 0, max_db_instances = 0, max_db_contexts = 0;
    size_t max_collection_replication_instances = 0, max_streaming_replication_instances = 0;
    size_t max_ml_anomalous = 0, max_ml_normal = 0, max_ml_trained = 0, max_ml_pending = 0, max_ml_silenced = 0;

    time_t
        max_db_duration = 0,
        max_db_from = 0,
        max_db_to = 0,
        max_in_age = 0,
        max_out_age = 0,
        max_out_attempt_age = 0;

    uint64_t
        max_in_since = 0,
        max_out_since = 0,
        max_out_attempt_since = 0;

    int16_t
        max_in_hops = -1,
        max_out_hops = -1;

    int
        max_in_local_port = 0,
        max_in_remote_port = 0,
        max_out_local_port = 0,
        max_out_remote_port = 0;

    uint32_t
        max_in_connections = 0,
        max_out_connections = 0;

    {
        RRDHOST *host;
        dfe_start_read(rrdhost_root_index, host) {
            RRDHOST_STATUS s;
            rrdhost_status(host, now, &s, RRDHOST_STATUS_ALL);
            buffer_json_add_array_item_array(wb);

            if(s.db.metrics > max_db_metrics)
                max_db_metrics = s.db.metrics;

            if(s.db.instances > max_db_instances)
                max_db_instances = s.db.instances;

            if(s.db.contexts > max_db_contexts)
                max_db_contexts = s.db.contexts;

            if(s.ingest.replication.instances > max_collection_replication_instances)
                max_collection_replication_instances = s.ingest.replication.instances;

            if(s.stream.replication.instances > max_streaming_replication_instances)
                max_streaming_replication_instances = s.stream.replication.instances;

            for(int i = 0; i < STREAM_TRAFFIC_TYPE_MAX ;i++) {
                if (s.stream.sent_bytes_on_this_connection_per_type[i] >
                    max_sent_bytes_on_this_connection_per_type[i])
                    max_sent_bytes_on_this_connection_per_type[i] =
                        s.stream.sent_bytes_on_this_connection_per_type[i];
            }

            // Node
            buffer_json_add_array_item_string(wb, rrdhost_hostname(s.host));

            // rowOptions
            buffer_json_add_array_item_object(wb);
            {
                const char *severity = NULL; // normal, debug, notice, warning, critical
                if(!rrdhost_option_check(host, RRDHOST_OPTION_EPHEMERAL_HOST)) {
                    switch(s.ingest.status) {
                        case RRDHOST_INGEST_STATUS_OFFLINE:
                        case RRDHOST_INGEST_STATUS_ARCHIVED:
                            severity = "critical";
                            break;

                        default:
                        case RRDHOST_INGEST_STATUS_INITIALIZING:
                        case RRDHOST_INGEST_STATUS_ONLINE:
                        case RRDHOST_INGEST_STATUS_REPLICATING:
                            break;
                    }

                    switch(s.stream.status) {
                        case RRDHOST_STREAM_STATUS_OFFLINE:
                            if(!severity && s.stream.reason != STREAM_HANDSHAKE_SP_NO_DESTINATION)
                                severity = "warning";
                            break;

                        default:
                        case RRDHOST_STREAM_STATUS_REPLICATING:
                        case RRDHOST_STREAM_STATUS_ONLINE:
                            break;
                    }
                }
                buffer_json_member_add_string(wb, "severity", severity ? severity : "normal");
            }
            buffer_json_object_close(wb); // rowOptions

            // Ephemerality
            buffer_json_add_array_item_string(wb, rrdhost_option_check(s.host, RRDHOST_OPTION_EPHEMERAL_HOST) ? "ephemeral" : "permanent");

            // AgentName and AgentVersion
            buffer_json_add_array_item_string(wb, rrdhost_program_name(host));
            buffer_json_add_array_item_string(wb, rrdhost_program_version(host));

            // System Info
            rrdhost_system_info_to_streaming_function_array(wb, s.host->system_info);

            // retention
            buffer_json_add_array_item_uint64(wb, s.db.first_time_s * MSEC_PER_SEC); // dbFrom
            if(s.db.first_time_s > max_db_from) max_db_from = s.db.first_time_s;

            buffer_json_add_array_item_uint64(wb, s.db.last_time_s * MSEC_PER_SEC); // dbTo
            if(s.db.last_time_s > max_db_to) max_db_to = s.db.last_time_s;

            if(s.db.first_time_s && s.db.last_time_s && s.db.last_time_s > s.db.first_time_s) {
                time_t db_duration = s.db.last_time_s - s.db.first_time_s;
                buffer_json_add_array_item_uint64(wb, db_duration); // dbDuration
                if(db_duration > max_db_duration) max_db_duration = db_duration;
            }
            else
                buffer_json_add_array_item_string(wb, NULL); // dbDuration

            buffer_json_add_array_item_uint64(wb, s.db.metrics); // dbMetrics
            buffer_json_add_array_item_uint64(wb, s.db.instances); // dbInstances
            buffer_json_add_array_item_uint64(wb, s.db.contexts); // dbContexts

            // statuses
            buffer_json_add_array_item_string(wb, rrdhost_ingest_status_to_string(s.ingest.status)); // InStatus
            buffer_json_add_array_item_string(wb, rrdhost_streaming_status_to_string(s.stream.status)); // OutStatus
            buffer_json_add_array_item_string(wb, rrdhost_ml_status_to_string(s.ml.status)); // MLStatus

            // collection

            // InConnections
            buffer_json_add_array_item_uint64(wb, s.host->stream.rcv.status.connections);
            if(s.host->stream.rcv.status.connections > max_in_connections)
                max_in_connections = s.host->stream.rcv.status.connections;

            if(s.ingest.since) {
                uint64_t in_since = s.ingest.since * MSEC_PER_SEC;
                buffer_json_add_array_item_uint64(wb, in_since); // InSince
                if(in_since > max_in_since) max_in_since = in_since;

                time_t in_age = s.now - s.ingest.since;
                buffer_json_add_array_item_time_t(wb, in_age); // InAge
                if(in_age > max_in_age) max_in_age = in_age;
            }
            else {
                buffer_json_add_array_item_string(wb, NULL); // InSince
                buffer_json_add_array_item_string(wb, NULL); // InAge
            }

            // InReason
            if(s.ingest.type == RRDHOST_INGEST_TYPE_LOCALHOST)
                buffer_json_add_array_item_string(wb, "LOCALHOST");
            else if(s.ingest.type == RRDHOST_INGEST_TYPE_VIRTUAL)
                buffer_json_add_array_item_string(wb, "VIRTUAL NODE");
            else
                buffer_json_add_array_item_string(wb, stream_handshake_error_to_string(s.ingest.reason));

            buffer_json_add_array_item_int64(wb, s.ingest.hops); // InHops
            if(s.ingest.hops > max_in_hops) max_in_hops = s.ingest.hops;

            buffer_json_add_array_item_double(wb, s.ingest.replication.completion); // InReplCompletion
            buffer_json_add_array_item_uint64(wb, s.ingest.replication.instances); // InReplInstances
            buffer_json_add_array_item_string(wb, s.ingest.type == RRDHOST_INGEST_TYPE_LOCALHOST || s.ingest.type == RRDHOST_INGEST_TYPE_VIRTUAL ? "localhost" : s.ingest.peers.local.ip); // InLocalIP

            buffer_json_add_array_item_uint64(wb, s.ingest.peers.local.port); // InLocalPort
            if(s.ingest.peers.local.port > max_in_local_port) max_in_local_port = s.ingest.peers.local.port;

            buffer_json_add_array_item_string(wb, s.ingest.peers.peer.ip); // InRemoteIP
            buffer_json_add_array_item_uint64(wb, s.ingest.peers.peer.port); // InRemotePort
            if(s.ingest.peers.peer.port > max_in_remote_port) max_in_remote_port = s.ingest.peers.peer.port;

            buffer_json_add_array_item_string(wb, s.ingest.ssl ? "SSL" : "PLAIN"); // InSSL
            stream_capabilities_to_json_array(wb, s.ingest.capabilities, NULL); // InCapabilities

            buffer_json_add_array_item_uint64(wb, s.ingest.collected.metrics); // CollectedMetrics
            buffer_json_add_array_item_uint64(wb, s.ingest.collected.instances); // CollectedInstances
            buffer_json_add_array_item_uint64(wb, s.ingest.collected.contexts); // CollectedContexts

            // streaming

            // OutConnections
            buffer_json_add_array_item_uint64(wb, s.host->stream.snd.status.connections);
            if(s.host->stream.snd.status.connections > max_out_connections)
                max_out_connections = s.host->stream.snd.status.connections;

            if(s.stream.since) {
                uint64_t out_since = s.stream.since * MSEC_PER_SEC;
                buffer_json_add_array_item_uint64(wb, out_since); // OutSince
                if(out_since > max_out_since) max_out_since = out_since;

                time_t out_age = s.now - s.stream.since;
                buffer_json_add_array_item_time_t(wb, out_age); // OutAge
                if(out_age > max_out_age) max_out_age = out_age;
            }
            else {
                buffer_json_add_array_item_string(wb, NULL); // OutSince
                buffer_json_add_array_item_string(wb, NULL); // OutAge
            }
            buffer_json_add_array_item_string(wb, stream_handshake_error_to_string(s.stream.reason)); // OutReason

            buffer_json_add_array_item_int64(wb, s.stream.hops); // OutHops
            if(s.stream.hops > max_out_hops) max_out_hops = s.stream.hops;

            buffer_json_add_array_item_double(wb, s.stream.replication.completion); // OutReplCompletion
            buffer_json_add_array_item_uint64(wb, s.stream.replication.instances); // OutReplInstances
            buffer_json_add_array_item_string(wb, s.stream.peers.local.ip); // OutLocalIP
            buffer_json_add_array_item_uint64(wb, s.stream.peers.local.port); // OutLocalPort
            if(s.stream.peers.local.port > max_out_local_port) max_out_local_port = s.stream.peers.local.port;

            buffer_json_add_array_item_string(wb, s.stream.peers.peer.ip); // OutRemoteIP
            buffer_json_add_array_item_uint64(wb, s.stream.peers.peer.port); // OutRemotePort
            if(s.stream.peers.peer.port > max_out_remote_port) max_out_remote_port = s.stream.peers.peer.port;

            buffer_json_add_array_item_string(wb, s.stream.ssl ? "SSL" : "PLAIN"); // OutSSL
            buffer_json_add_array_item_string(wb, s.stream.compression ? "COMPRESSED" : "UNCOMPRESSED"); // OutCompression
            stream_capabilities_to_json_array(wb, s.stream.capabilities, NULL); // OutCapabilities
            buffer_json_add_array_item_uint64(wb, s.stream.sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_DATA]);
            buffer_json_add_array_item_uint64(wb, s.stream.sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_METADATA]);
            buffer_json_add_array_item_uint64(wb, s.stream.sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_REPLICATION]);
            buffer_json_add_array_item_uint64(wb, s.stream.sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_FUNCTIONS]);

            buffer_json_add_array_item_array(wb); // OutAttemptHandshake
            usec_t last_attempt = stream_parent_handshake_error_to_json(wb, host);
            buffer_json_array_close(wb); // // OutAttemptHandshake

            if(!last_attempt) {
                buffer_json_add_array_item_string(wb, NULL); // OutAttemptSince
                buffer_json_add_array_item_string(wb, NULL); // OutAttemptAge
            }
            else {
                uint64_t out_attempt_since = last_attempt / USEC_PER_MS;
                buffer_json_add_array_item_uint64(wb, out_attempt_since); // OutAttemptSince
                if(out_attempt_since > max_out_attempt_since) max_out_attempt_since = out_attempt_since;

                time_t out_attempt_age = s.now - (time_t)(last_attempt / USEC_PER_SEC);
                buffer_json_add_array_item_time_t(wb, out_attempt_age); // OutAttemptAge
                if(out_attempt_age > max_out_attempt_age) max_out_attempt_age = out_attempt_age;
            }

            // ML
            if(s.ml.status == RRDHOST_ML_STATUS_RUNNING) {
                buffer_json_add_array_item_uint64(wb, s.ml.metrics.anomalous); // MlAnomalous
                buffer_json_add_array_item_uint64(wb, s.ml.metrics.normal); // MlNormal
                buffer_json_add_array_item_uint64(wb, s.ml.metrics.trained); // MlTrained
                buffer_json_add_array_item_uint64(wb, s.ml.metrics.pending); // MlPending
                buffer_json_add_array_item_uint64(wb, s.ml.metrics.silenced); // MlSilenced

                if(s.ml.metrics.anomalous > max_ml_anomalous)
                    max_ml_anomalous = s.ml.metrics.anomalous;

                if(s.ml.metrics.normal > max_ml_normal)
                    max_ml_normal = s.ml.metrics.normal;

                if(s.ml.metrics.trained > max_ml_trained)
                    max_ml_trained = s.ml.metrics.trained;

                if(s.ml.metrics.pending > max_ml_pending)
                    max_ml_pending = s.ml.metrics.pending;

                if(s.ml.metrics.silenced > max_ml_silenced)
                    max_ml_silenced = s.ml.metrics.silenced;

            }
            else {
                buffer_json_add_array_item_string(wb, NULL); // MlAnomalous
                buffer_json_add_array_item_string(wb, NULL); // MlNormal
                buffer_json_add_array_item_string(wb, NULL); // MlTrained
                buffer_json_add_array_item_string(wb, NULL); // MlPending
                buffer_json_add_array_item_string(wb, NULL); // MlSilenced
            }

            // close
            buffer_json_array_close(wb);
        }
        dfe_done(host);
    }
    buffer_json_array_close(wb); // data
    buffer_json_member_add_object(wb, "columns");
    {
        size_t field_id = 0;

        // Node
        buffer_rrdf_table_add_field(wb, field_id++, "Node", "Node's Hostname",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE | RRDF_FIELD_OPTS_UNIQUE_KEY | RRDF_FIELD_OPTS_STICKY,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "rowOptions", "rowOptions",
                                    RRDF_FIELD_TYPE_NONE, RRDR_FIELD_VISUAL_ROW_OPTIONS, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_FIXED, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE, RRDF_FIELD_OPTS_DUMMY,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Ephemerality", "The type of ephemerality for the node",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "AgentName", "The name of the Netdata agent",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "AgentVersion", "The version of the Netdata agent",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSName", "The name of the host's operating system",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSId", "The identifier of the host's operating system",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSIdLike", "The ID-like string for the host's OS",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSVersion", "The version of the host's operating system",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSVersionId", "The version identifier of the host's OS",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OSDetection", "Details about host OS detection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CPUCores", "The number of CPU cores in the host",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "DiskSpace", "The total disk space available on the host",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CPUFreq", "The CPU frequency of the host",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "RAMTotal", "The total RAM available on the host",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSName", "The name of the container's operating system",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSId", "The identifier of the container's operating system",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSIdLike", "The ID-like string for the container's OS",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSVersion", "The version of the container's OS",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSVersionId", "The version identifier of the container's OS",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerOSDetection", "Details about container OS detection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "IsK8sNode", "Whether this node is part of a Kubernetes cluster",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "KernelName", "The kernel name",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "KernelVersion", "The kernel version",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Architecture", "The system architecture",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Virtualization", "The virtualization technology in use",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "VirtDetection", "Details about virtualization detection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "Container", "Container type information",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "ContainerDetection", "Details about container detection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CloudProviderType", "The type of cloud provider",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CloudInstanceType", "The type of cloud instance",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CloudInstanceRegion", "The region of the cloud instance",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE,
                                    NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbFrom", "DB Data Retention From",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_MS,
                                    0, NULL, (double)max_db_from * MSEC_PER_SEC, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbTo", "DB Data Retention To",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_MS,
                                    0, NULL, (double)max_db_to * MSEC_PER_SEC, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbDuration", "DB Data Retention Duration",
                                    RRDF_FIELD_TYPE_DURATION, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DURATION_S,
                                    0, NULL, (double)max_db_duration, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbMetrics", "Time-series Metrics in the DB",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_metrics, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbInstances", "Instances in the DB",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_instances, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "dbContexts", "Contexts in the DB",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_contexts, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        // --- statuses ---

        buffer_rrdf_table_add_field(wb, field_id++, "InStatus", "Data Collection Online Status",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);


        buffer_rrdf_table_add_field(wb, field_id++, "OutStatus", "Streaming Online Status",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "MlStatus", "ML Status",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        // --- collection ---

        buffer_rrdf_table_add_field(wb, field_id++, "InConnections", "Number of times this child connected",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, (double)max_in_connections, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InSince", "Last Data Collection Status Change",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_MS,
                                    0, NULL, (double)max_in_since, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InAge", "Last Data Collection Online Status Change Age",
                                    RRDF_FIELD_TYPE_DURATION, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DURATION_S,
                                    0, NULL, (double)max_in_age, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InReason", "Data Collection Online Status Reason",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InHops", "Data Collection Distance Hops from Origin Node",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, (double)max_in_hops, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InReplCompletion", "Inbound Replication Completion",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_BAR, RRDF_FIELD_TRANSFORM_NUMBER,
                                    1, "%", 100.0, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InReplInstances", "Inbound Replicating Instances",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "instances", (double)max_collection_replication_instances, RRDF_FIELD_SORT_DESCENDING,
                                    NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InLocalIP", "Inbound Local IP",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InLocalPort", "Inbound Local Port",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_in_local_port, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InRemoteIP", "Inbound Remote IP",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InRemotePort", "Inbound Remote Port",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_in_remote_port, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InSSL", "Inbound SSL Connection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "InCapabilities", "Inbound Connection Capabilities",
                                    RRDF_FIELD_TYPE_ARRAY, RRDF_FIELD_VISUAL_PILL, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CollectedMetrics", "Time-series Metrics Currently Collected",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_metrics, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CollectedInstances", "Instances Currently Collected",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_instances, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "CollectedContexts", "Contexts Currently Collected",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_db_contexts, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        // --- streaming ---

        buffer_rrdf_table_add_field(wb, field_id++, "OutConnections", "Number of times connected to a parent",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, (double)max_out_connections, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutSince", "Last Streaming Status Change",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_MS,
                                    0, NULL, (double)max_out_since, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutAge", "Last Streaming Status Change Age",
                                    RRDF_FIELD_TYPE_DURATION, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DURATION_S,
                                    0, NULL, (double)max_out_age, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutReason", "Streaming Status Reason",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutHops", "Streaming Distance Hops from Origin Node",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, (double)max_out_hops, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutReplCompletion", "Outbound Replication Completion",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_BAR, RRDF_FIELD_TRANSFORM_NUMBER,
                                    1, "%", 100.0, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutReplInstances", "Outbound Replicating Instances",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "instances", (double)max_streaming_replication_instances, RRDF_FIELD_SORT_DESCENDING,
                                    NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutLocalIP", "Outbound Local IP",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutLocalPort", "Outbound Local Port",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutRemoteIP", "Outbound Remote IP",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutRemotePort", "Outbound Remote Port",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, NULL, (double)max_out_remote_port, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutSSL", "Outbound SSL Connection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutCompression", "Outbound Compressed Connection",
                                    RRDF_FIELD_TYPE_STRING, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutCapabilities", "Outbound Connection Capabilities",
                                    RRDF_FIELD_TYPE_ARRAY, RRDF_FIELD_VISUAL_PILL, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutTrafficData", "Outbound Metric Data Traffic",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "bytes", (double)max_sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_DATA],
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutTrafficMetadata", "Outbound Metric Metadata Traffic",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "bytes",
                                    (double)max_sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_METADATA],
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutTrafficReplication", "Outbound Metric Replication Traffic",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "bytes",
                                    (double)max_sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_REPLICATION],
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutTrafficFunctions", "Outbound Metric Functions Traffic",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "bytes",
                                    (double)max_sent_bytes_on_this_connection_per_type[STREAM_TRAFFIC_TYPE_FUNCTIONS],
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutAttemptHandshake",
                                    "Outbound Connection Attempt Handshake Status",
                                    RRDF_FIELD_TYPE_ARRAY, RRDF_FIELD_VISUAL_PILL, RRDF_FIELD_TRANSFORM_NONE,
                                    0, NULL, NAN, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_COUNT, RRDF_FIELD_FILTER_MULTISELECT,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutAttemptSince",
                                    "Last Outbound Connection Attempt Status Change Time",
                                    RRDF_FIELD_TYPE_TIMESTAMP, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DATETIME_MS,
                                    0, NULL, (double)max_out_attempt_since, RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MAX, RRDF_FIELD_FILTER_NONE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "OutAttemptAge",
                                    "Last Outbound Connection Attempt Status Change Age",
                                    RRDF_FIELD_TYPE_DURATION, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_DURATION_S,
                                    0, NULL, (double)max_out_attempt_age, RRDF_FIELD_SORT_ASCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_MIN, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_VISIBLE, NULL);

        // --- ML ---

        buffer_rrdf_table_add_field(wb, field_id++, "MlAnomalous", "Number of Anomalous Metrics",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "metrics",
                                    (double)max_ml_anomalous,
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "MlNormal", "Number of Not Anomalous Metrics",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "metrics",
                                    (double)max_ml_normal,
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "MlTrained", "Number of Trained Metrics",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "metrics",
                                    (double)max_ml_trained,
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "MlPending", "Number of Pending Metrics",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "metrics",
                                    (double)max_ml_pending,
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);

        buffer_rrdf_table_add_field(wb, field_id++, "MlSilenced", "Number of Silenced Metrics",
                                    RRDF_FIELD_TYPE_INTEGER, RRDF_FIELD_VISUAL_VALUE, RRDF_FIELD_TRANSFORM_NUMBER,
                                    0, "metrics",
                                    (double)max_ml_silenced,
                                    RRDF_FIELD_SORT_DESCENDING, NULL,
                                    RRDF_FIELD_SUMMARY_SUM, RRDF_FIELD_FILTER_RANGE,
                                    RRDF_FIELD_OPTS_NONE, NULL);
    }
    buffer_json_object_close(wb); // columns
    buffer_json_member_add_string(wb, "default_sort_column", "Node");
    buffer_json_member_add_object(wb, "charts");
    {
        // Data Collection Age chart
        buffer_json_member_add_object(wb, "InAge");
        {
            buffer_json_member_add_string(wb, "name", "Data Collection Age");
            buffer_json_member_add_string(wb, "type", "stacked-bar");
            buffer_json_member_add_array(wb, "columns");
            {
                buffer_json_add_array_item_string(wb, "InAge");
            }
            buffer_json_array_close(wb);
        }
        buffer_json_object_close(wb);

        // Streaming Age chart
        buffer_json_member_add_object(wb, "OutAge");
        {
            buffer_json_member_add_string(wb, "name", "Streaming Age");
            buffer_json_member_add_string(wb, "type", "stacked-bar");
            buffer_json_member_add_array(wb, "columns");
            {
                buffer_json_add_array_item_string(wb, "OutAge");
            }
            buffer_json_array_close(wb);
        }
        buffer_json_object_close(wb);

        // DB Duration
        buffer_json_member_add_object(wb, "dbDuration");
        {
            buffer_json_member_add_string(wb, "name", "Retention Duration");
            buffer_json_member_add_string(wb, "type", "stacked-bar");
            buffer_json_member_add_array(wb, "columns");
            {
                buffer_json_add_array_item_string(wb, "dbDuration");
            }
            buffer_json_array_close(wb);
        }
        buffer_json_object_close(wb);
    }
    buffer_json_object_close(wb); // charts

    buffer_json_member_add_array(wb, "default_charts");
    {
        buffer_json_add_array_item_array(wb);
        buffer_json_add_array_item_string(wb, "InAge");
        buffer_json_add_array_item_string(wb, "Node");
        buffer_json_array_close(wb);

        buffer_json_add_array_item_array(wb);
        buffer_json_add_array_item_string(wb, "OutAge");
        buffer_json_add_array_item_string(wb, "Node");
        buffer_json_array_close(wb);
    }
    buffer_json_array_close(wb);

    buffer_json_member_add_object(wb, "group_by");
    {
        GROUP_BY_COLUMN("OSName", "O/S Name");
        GROUP_BY_COLUMN("OSId", "O/S ID");
        GROUP_BY_COLUMN("OSIdLike", "O/S ID Like");
        GROUP_BY_COLUMN("OSVersion", "O/S Version");
        GROUP_BY_COLUMN("OSVersionId", "O/S Version ID");
        GROUP_BY_COLUMN("OSDetection", "O/S Detection");
        GROUP_BY_COLUMN("CPUCores", "CPU Cores");
        GROUP_BY_COLUMN("ContainerOSName", "Container O/S Name");
        GROUP_BY_COLUMN("ContainerOSId", "Container O/S ID");
        GROUP_BY_COLUMN("ContainerOSIdLike", "Container O/S ID Like");
        GROUP_BY_COLUMN("ContainerOSVersion", "Container O/S Version");
        GROUP_BY_COLUMN("ContainerOSVersionId", "Container O/S Version ID");
        GROUP_BY_COLUMN("ContainerOSDetection", "Container O/S Detection");
        GROUP_BY_COLUMN("IsK8sNode", "Kubernetes Nodes");
        GROUP_BY_COLUMN("KernelName", "Kernel Name");
        GROUP_BY_COLUMN("KernelVersion", "Kernel Version");
        GROUP_BY_COLUMN("Architecture", "Architecture");
        GROUP_BY_COLUMN("Virtualization", "Virtualization Technology");
        GROUP_BY_COLUMN("VirtDetection", "Virtualization Detection");
        GROUP_BY_COLUMN("Container", "Container");
        GROUP_BY_COLUMN("ContainerDetection", "Container Detection");
        GROUP_BY_COLUMN("CloudProviderType", "Cloud Provider Type");
        GROUP_BY_COLUMN("CloudInstanceType", "Cloud Instance Type");
        GROUP_BY_COLUMN("CloudInstanceRegion", "Cloud Instance Region");

        GROUP_BY_COLUMN("InStatus", "Collection Status");
        GROUP_BY_COLUMN("OutStatus", "Streaming Status");
        GROUP_BY_COLUMN("MlStatus", "ML Status");
        GROUP_BY_COLUMN("InRemoteIP", "Inbound IP");
        GROUP_BY_COLUMN("OutRemoteIP", "Outbound IP");
    }
    buffer_json_object_close(wb); // group_by

    buffer_json_member_add_time_t(wb, "expires", now_realtime_sec() + STREAMING_FUNCTION_UPDATE_EVERY);
    buffer_json_finalize(wb);

    return HTTP_RESP_OK;
}
